"""One-time refill of migrated highlight passages.

Highlights migrated from JW Library packed "<reference> · <passage>" into a
single 72-byte label, so the passage was truncated to make room for the
reference. The firmware now stores the two apart; this recovers the reference
from the existing label and re-extracts the passage from the EPUB so it can use
the full budget.

Rewrites ONLY `ref` and `text`, and only for entries matching an existing
highlight on (si, start). Offsets and tags are never touched: the offsets are
verified correct on hardware and the tags may carry edits made since.
"""

import json
import os
import re
import sys
import xml.parsers.expat

SEPARATOR = " · "
PASSAGE_MAX_BYTES = 72

# Mirrors VisibleTextUtils::isNonVisibleElement; see offsets.py for why this
# list and the <body> handling must match the firmware's parser exactly.
NON_VISIBLE = {"head", "style", "script", "title", "rp"}


def _local(name):
    return name.split(":")[-1].lower()


def visible_text(xhtml_bytes):
    """Visible character data in the same coordinate space as the firmware's
    stored offsets, so a [start, end) slice lines up with what was highlighted."""
    state = {"inside_body": False, "nonvis": 0}
    chunks = []

    def start(name, attrs):
        local = _local(name)
        if local == "body":
            state["inside_body"] = True
        if state["inside_body"] and (state["nonvis"] > 0 or local in NON_VISIBLE):
            state["nonvis"] += 1

    def end(name):
        if state["nonvis"] > 0:
            state["nonvis"] -= 1
        if _local(name) == "body":
            state["inside_body"] = False

    def chars(data):
        if state["inside_body"] and state["nonvis"] == 0:
            chunks.append(data)

    parser = xml.parsers.expat.ParserCreate()
    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = chars
    parser.Parse(xhtml_bytes, True)
    return "".join(chunks)


# The source markup separates a verse number from its text with U+202F, and uses
# U+00A0 inside some phrases. The device never stores either: selectionLabel
# rebuilds a passage by joining laid-out word boxes with a plain space. Folding
# them here keeps a migrated passage byte-identical in kind to a device-made one,
# and avoids depending on the reader fonts having glyphs for them.
UNICODE_SPACES = "\u00a0\u202f\u2007\u2009\u200a\u2002\u2003"


def utf8_safe_summary(passage, max_bytes=PASSAGE_MAX_BYTES):
    """Byte-for-byte equivalent of lib/Utf8/Utf8.cpp's utf8SafeSummary, so a
    passage written here survives a device reload unchanged."""
    for space in UNICODE_SPACES:
        passage = passage.replace(space, " ")

    # ASCII-only, because the C++ runs std::isspace over individual BYTES of a
    # UTF-8 sequence; str.isspace() is Unicode-aware and would collapse runs the
    # firmware leaves alone.
    def ascii_space(ch):
        return ch in " \t\n\v\f\r"

    collapsed = []
    for ch in passage:
        if ascii_space(ch) and collapsed and ascii_space(collapsed[-1]):
            continue
        collapsed.append(ch)
    text = "".join(collapsed).replace("\n", "")
    text = text.strip(" \t\v\f\r")
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return raw.decode("utf-8")
    raw = raw[:max_bytes]

    # Mirrors utf8SafeTruncateBuffer: walk back to the lead byte, then drop the
    # whole sequence if the cut left it incomplete. Stripping only continuation
    # bytes is not enough -- the cut can land immediately after a lead byte.
    lead = len(raw) - 1
    while lead > 0 and (raw[lead] & 0xC0) == 0x80:
        lead -= 1
    first = raw[lead]
    if first < 0x80:
        expected = 1
    elif first >= 0xF0:
        expected = 4
    elif first >= 0xE0:
        expected = 3
    elif first >= 0xC0:
        expected = 2
    else:
        expected = 1
    if len(raw) - lead < expected:
        raw = raw[:lead]
    return raw.decode("utf-8")


def spine_files(oebps):
    """Spine index -> xhtml filename, read from the OPF in spine order. Lets an
    entry be re-extracted from its stored spine index alone, so highlights made
    on the device (which never went through resolve.py) are covered too."""
    opf = open(os.path.join(oebps, "content.opf"), encoding="utf-8").read()
    items = dict(re.findall(r'<item\b[^>]*?id="([^"]+)"[^>]*?href="([^"]+)"', opf))
    order = re.findall(r'<itemref[^>]*idref="([^"]+)"', opf)
    return {i: items[ref] for i, ref in enumerate(order) if ref in items}


def main():
    if len(sys.argv) != 5:
        print("usage: migrate_highlight_refs.py <live.json> <resolved.json> <OEBPS dir> <out.json>")
        return 2
    live_path, resolved_path, oebps, out_path = sys.argv[1:]

    live = json.load(open(live_path, encoding="utf-8"))
    resolved = json.load(open(resolved_path, encoding="utf-8"))
    by_key = {(r["spine"], r["start"]): r for r in resolved}
    by_spine = spine_files(oebps)

    cache = {}
    rewritten = 0
    untouched = []
    for entry in live["highlights"]:
        match = by_key.get((entry["si"], entry["start"]))
        if match is None:
            # Made on the device rather than migrated: the stored range is still
            # authoritative, so only the filename has to be recovered.
            name = by_spine.get(entry["si"])
            if name is None:
                untouched.append("spine index not in the OPF")
                continue
            match = {"file": name, "start": entry["start"], "end": entry["end"]}

        # The reference is already correct on the device; recover it rather than
        # re-deriving it from the JW database and risking a different answer.
        if not entry.get("ref"):
            if SEPARATOR not in entry.get("text", ""):
                untouched.append("no separator in label")
                continue
            entry["ref"] = entry["text"].split(SEPARATOR, 1)[0]

        name = match["file"]
        if name not in cache:
            with open(os.path.join(oebps, name), "rb") as handle:
                cache[name] = visible_text(handle.read())
        entry["text"] = utf8_safe_summary(cache[name][match["start"]:match["end"]])
        rewritten += 1

    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(live, handle, ensure_ascii=False, separators=(",", ":"))
    print(f"rewrote {rewritten}, left {len(untouched)} untouched -> {out_path}")
    for reason in set(untouched):
        print(f"  untouched because: {reason} ({untouched.count(reason)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
