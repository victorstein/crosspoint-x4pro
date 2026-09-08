# Three-line highlight rows: reference, passage, tags

**Date:** 2026-09-08
**Status:** Design v1
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** fork-only, continuing `feat/tagged-highlights`
**Predecessor:** [2026-09-08-selection-and-labels-design.md](2026-09-08-selection-and-labels-design.md)

## Goal

Give each highlight in the Highlights list three lines instead of two:

```
Apocalipsis 1:8
8 "Yo soy el Alfa y el Omega", dice Jehová Dios, "el que…
esperanza, fe
```

Today the reference and the passage share line one, separated by ` · `, with the
tags on line two. Because `HighlightEntry::label` is capped at 72 bytes by
`utf8SafeSummary` (`Utf8.h:26-31`), the reference prefix consumes part of the
passage's budget: across the 56 migrated highlights the reference averages 12.2
bytes (min 9, max 21) and the ` · ` separator costs 4 more, so a mean of 16.2 of
the 72 bytes goes to something that is not the passage. All 56 sit at exactly the
cap, and all 56 are truncated.

Splitting the reference into its own field and its own line both fixes the layout
and returns those bytes to the passage.

## Non-goals

- **A fourth line, or a passage that wraps by design.** Considered and rejected:
  the extra text is not worth halving how many highlights fit on screen. The
  passage gets exactly one line in the normal case (see "Why `maxLines = 3`" for
  the one exception, which is a safety valve, not a feature).
- **Stripping the leading verse number** from stored passages (`8 "Yo soy…`).
  It is arguably useful and removing it is a separate decision.
- **Re-resolving the migrated highlights' anchors.** Their spine indices and
  offset ranges are correct and verified on hardware; the offline pass in
  Migration B re-extracts passage *text only*.
- **A `FORMAT_VERSION` bump.** See "Why the format version stays 1".

---

## 1. Data model

`HighlightEntry` (`lib/Epub/Epub/HighlightEntry.h`) gains one field:

```cpp
struct HighlightEntry {
  uint16_t spineIndex = 0;
  VisibleRange range;
  std::vector<uint16_t> tagIndices;
  std::string label;      // the passage snippet
  std::string reference;  // "Apocalipsis 1:8"; empty when unknown
};
```

`reference` is display-only, exactly like `label`, and must never be used to
locate a passage.

### Serialisation

`toJson` (`HighlightDoc.cpp:77-96`) emits `ref` only when non-empty, so a
highlight without a reference costs nothing:

```cpp
o["si"] = h.spineIndex;
o["start"] = h.range.start;
o["end"] = h.range.end;
if (!h.tagIndices.empty()) { /* unchanged */ }
if (!h.reference.empty()) o["ref"] = h.reference;
o["text"] = h.label;
```

### Why the format version stays 1

`fromJson` rejects any file whose version exceeds the compiled `FORMAT_VERSION`
(`HighlightDoc.cpp:99-100`). Bumping to 2 would therefore make every highlights
file written by this firmware **unreadable** by an older build — a firmware
downgrade would lose access to the user's data, not merely render it differently.

`ref` is additive instead. Older firmware ignores the unknown key and shows the
passage without its reference: degraded, never broken. This also means no
migration write is required at all — see Migration A.

### Reference length cap

`MAX_REFERENCE_BYTES = 48`, enforced on both the add and parse paths. The value
is not arbitrary: `verseReference` composes `toc.title + " " + verse`
(`PassageSelectActivity.cpp:225-230`), and a table-of-contents title is arbitrary
text from the book. 48 bytes holds `Apocalipsis 1:8` (15 bytes) with wide headroom
while bounding a hostile or unusual book's contribution to the save budget.

Enforcement reuses the existing normaliser via a new defaulted parameter, so the
passage path is untouched:

```cpp
// lib/Utf8/Utf8.h
std::string utf8SafeSummary(std::string passage, size_t maxBytes = 72);
```

The existing body already truncates on a UTF-8 boundary
(`Utf8.cpp:200-202`); only the literal `72` becomes `maxBytes`.

### Save-budget impact

The worst-case document already exceeds `SAVE_BYTE_BUDGET` (45,000 bytes), and
`HighlightDocTest` asserts that it does, deliberately
(`test/highlight_doc/HighlightDocTest.cpp:168-196`): `MAX_HIGHLIGHTS` is a growth
limit and the save-side byte guard is the actual safety mechanism. Adding `ref`
does not change that contract.

The cost differs between the two migration stages, because splitting *moves*
bytes out of `text` rather than only adding them. Measured against the live
56-entry file (7,591 bytes):

| Stage | Size | Delta | Per entry |
|---|---|---|---|
| today | 7,591 B | — | — |
| after Migration A (split only) | 7,871 B | +280 B | +5.0 B |
| after Migration A + B (steady state) | 8,801 B | +1,210 B | +21.6 B |

Steady state is +8.4KB at the 400-entry ceiling.

**Regression path worth recording:** a document larger than about 36,400 bytes
today would exceed `SAVE_BYTE_BUDGET` once migrated, at which point *edits* to
that book's highlights start failing with `STR_HIGHLIGHTS_TOO_LARGE`. Nothing is
lost — the save guard refuses before touching the file — but the user would be
unable to add or retag highlights in that book. At 7,591 bytes the live document
has roughly 5x headroom, so this is a documented ceiling rather than a live
concern.

---

## 2. Rendering

### Slot assignment

In `HighlightsActivity::rebuildRowItems` (`HighlightsActivity.cpp:77-101`):

```cpp
item.label    = entry.reference;   // labelText left alone: see below
item.subtitle = composeSubtitle(passage, tags);

props.subtitleText = screen.theme().smallText;  // MUST come first
props.subtitleText.maxLines = 3;
```

**`subtitleText` must be assigned from the theme before `maxLines` is set.**
`Screen::list` only substitutes the theme font into a style that is still
*unset* (`FreeInkApp.h:249-251`), and `textStyleUnset` counts `maxLines == 1`
among the conditions for "unset" (`FreeInkUICore.h:550-554`). Writing
`props.subtitleText.maxLines = 3` on its own therefore marks the style as
caller-supplied, the substitution is skipped, and the subtitle renders with
`font == 0` instead of the theme's `smallText`. The codebase already documents
this exact trap at `SettingsActivity.cpp:483-487`.

`labelText` is deliberately **not** touched. Its default `maxLines` is already 1,
and assigning even a no-op value to it would trip the same rule and lose the
theme's `bodyText`.

The SDK's text layout hard-breaks on `'\n'` (`FreeInkUICore.h:716-717`, handled at
`:773-776` and `:816-817`), so the embedded newline is a real line break, and
`measureWrappedText` is built on the same `layoutText`, so measurement and drawing
agree exactly (`FreeInkUICore.h:860-873`).

### Why the passage goes in the subtitle, not the label

Putting `"reference\npassage"` in the **label** with `labelText.maxLines = 2`
looks equivalent and is not. The row-growth pre-pass only measures a wrapped label
when a *single-line* measurement of the whole string already overflows the
available width (`list.h:410-416`):

```cpp
if (labelAvail > 0 &&
    frame.target().measureText(props.labelText.font, item.label, props.labelText)
        .width > labelAvail) {
```

For a short pair — `Juan 3:16` + `Dios amó` — that combined measurement fits, so
the gate fails, `labelLines` stays 1, and the band is sized for one line
(`list.h:501`). The draw path then calls `text()` on the same string, which
goes through `layoutText`, honours the `'\n'` unconditionally, and emits **two**
lines into a one-line band (`list.h:603-605`) — over the top of the subtitle.

The subtitle branch has no such gate. When `subtitleText.maxLines > 1` it measures
the wrapped height unconditionally and grows the row by the result
(`list.h:425-440`):

```cpp
subH = props.subtitleText.maxLines > 1
           ? measureWrappedText(frame.target(), item.subtitle,
                                props.subtitleText, contentAvail).height
           : subLh;
```

So the reserved height always matches what is drawn. This arrangement is correct
by construction rather than correct for the inputs we happen to expect.

### Why `maxLines = 3` and not 2

With `maxLines = 2` a passage wide enough to wrap would consume both subtitle
lines, and `layoutText` would ellipsise at the end of line two — **silently
dropping the tags**, which are the whole point of the feature.

This is measured, not assumed. Running the real `layoutText` against the longest
migrated passage at the 72-byte cap:

| Orientation | Content width | `maxLines = 2` | `maxLines = 3` |
|---|---|---|---|
| landscape | 740px | 2 lines, tags visible | 2 lines, tags visible |
| portrait | 420px | 2 lines, **tags dropped** | 3 lines, tags visible |

In portrait, at every glyph advance from 8px up, `maxLines = 2` silently loses
the tags. A 72-character ASCII passage needs three subtitle lines at 420px. So
`maxLines = 3` is load-bearing, not defensive.

**Consequence to accept:** in portrait a full-length passage produces a
four-line row (reference + two passage lines + tags). The "exactly three lines"
target holds in landscape, which is how the device is used for reading; portrait
trades a taller row for never hiding a tag.

### Row height arithmetic

For a three-line row, with `labelLh` the label line height, `subLh` the subtitle
line height, and `rowH` the theme row height (`FreeInkUI.cpp:114`,
`lineHeight * 2 + 8`):

```
labelLines = 1                       (label branch skipped: labelText.maxLines == 1)
subH       = 2 * subLh               (passage line + tags line)
basePad    = rowH - labelLh - subLh
needed     = labelLh + subH + basePad
           = rowH + subLh
```

The row grows by exactly one subtitle line (`list.h:437-440`). No SDK change is
required, and `freeink-sdk` — an upstream submodule — is not touched.

### Viewport

Variable row heights are already handled. `UiListActivity::render` re-runs the
build while `consumeRebuildNeeded()` is set, bounded at 8 passes
(`UiListActivity.cpp:160`), and paging uses the rows actually drawn rather
than the fixed-height estimate (`UiListActivity.cpp:113-119`). This design adds no
new machinery there.

### Composition rules

Extracted as a pure function so it is host-testable, following the existing
`TagRowMapping.h` precedent:

```cpp
// src/activities/reader/HighlightRowText.h
namespace HighlightRowText {
// Joins a passage and a rendered tag list into one subtitle string.
// A '\n' is inserted ONLY when both halves are non-empty. The leading case is
// the one that matters: layoutText preserves a blank line for a leading '\n'
// (FreeInkUICore.h:773-776), so an untagged-but-empty-passage row would render
// an empty first line. A trailing '\n' is harmless, but the symmetric rule is
// simpler to state and to test.
std::string composeSubtitle(const std::string& passage, const std::string& tags);
}
```

| `reference` | `label` (passage) | tags | label slot | subtitle slot |
|---|---|---|---|---|
| set | set | set | reference | `passage\ntags` |
| set | set | — | reference | `passage` |
| set | — | set | reference | `tags` |
| set | — | — | reference | *(empty)* |
| — | set | set | passage | `tags` |
| — | set | — | passage | *(empty)* |
| — | — | set | `tr(STR_UNNAMED)` | `tags` |
| — | — | — | `tr(STR_UNNAMED)` | *(empty)* |

The `reference`-absent rows reproduce today's two-line behaviour exactly, which is
what a non-Bible book with no verse anchors will produce.

`rowSubtitles_` (`HighlightsActivity.h:134`) already owns one `std::string` per
row to keep the `const char*` in `ListItem` valid; it now holds the composed
string instead of the tag list. No new allocation per row.

---

## 3. Write path

`PassageSelectActivity::finalizeSelection` (`PassageSelectActivity.cpp:364-395`)
currently rations one 72-byte budget between reference and passage:

```cpp
static constexpr size_t LABEL_BUDGET = 72;
static constexpr size_t MIN_SNIPPET_BYTES = 16;
if (reference.empty()) {
  entry.label = passage;
} else if (reference.size() + 4 + MIN_SNIPPET_BYTES > LABEL_BUDGET) {
  entry.label = reference;
} else {
  entry.label = reference + " \xc2\xb7 " + passage;
}
```

All of it is deleted. The two fields no longer compete:

```cpp
entry.reference = reference;
entry.label = passage;
```

`HighlightDoc::addHighlight` caps each independently. `selectionLabel`'s
`LABEL_SCAN_BYTES = 128` scan limit (`PassageSelectActivity.cpp:236-238`) still
comfortably exceeds the 72-byte cap, so it is unchanged.

---

## 4. Migration

Two halves, addressing two different populations. Both are required.

### Migration A — in firmware, at load

Highlights created on the device since the reference feature landed are stored as
`"Mateo 11:19 · 19 Vino el Hijo del hombre…"` with no `ref` key.

In `fromJson`, after reading `text` and before normalising, when `ref` is absent
and `text` contains the separator `" \xc2\xb7 "`, split on the **first**
occurrence: everything before it is the reference, everything after is the
passage.

Properties that make this safe:

- **Idempotent.** An entry that already has `ref` is never split, so a file that
  has been through Migration B, or through any save by this firmware, is
  untouched.
- **No rewrite.** Because the split happens on every load and `FORMAT_VERSION` is
  unchanged, nothing needs to be written to migrate. The file adopts the new shape
  the next time the user edits that book's highlights, through the ordinary save
  path.
- **Degrades correctly.** A label with no separator is treated entirely as the
  passage, with no reference — which is precisely the pre-reference behaviour.
- **First-occurrence split** means a passage that itself contains ` · ` keeps the
  separator inside the passage rather than corrupting the reference. The
  collision risk was measured, not assumed: the separator appears **zero** times
  across all 3,942 files of the Bible EPUB, both as a literal `·` and as
  `&middot;`/`&#183;`/`&#xB7;`. No pattern guard on the prefix is warranted for
  this corpus; if a future book proves otherwise, requiring the prefix to end in
  `<digits>:<digits>` is the cheap fix.
- **An empty `ref` counts as absent.** A file carrying `"ref": ""` is split
  exactly as one with no `ref` key at all, so a hand-edited or partially-written
  file cannot end up with the reference stranded inside `text` forever.

This covers every book, forever, with no manual step.

### Migration B — offline, one-time, for the 56

Migration A alone leaves the migrated highlights short: their passages were
truncated to fit *alongside* the reference inside 72 bytes. Recovering the
reference does not lengthen the passage, because the discarded bytes were never
stored.

The pipeline is intact and verified present:

| Artifact | Contents |
|---|---|
| `epubwork/src/OEBPS/` | unpacked `nwt_S_slim.epub`, 3,942 files — the build that is on the device |
| `jwl/userData.db` | the JW Library backup's notes and tags |
| `resolved.json` | all 56 entries with `spine`, `start`, `end`, `file`, `tags` |
| `offsets.py` | the offline replica of the firmware's visible-offset counter |
| `resolve.py` | verse anchor → spine index and offset range |

**Step 0 — durability.** These currently live in a `/private/tmp` scratchpad
belonging to an earlier session. Copy them into this session's scratchpad before
anything else. If they are gone, `epubwork` is reproducible by unzipping
`~/Downloads/nwt_S_slim.epub` and `resolve.py` regenerates `resolved.json` from
`~/Downloads/UserdataBackup_2026-09-07_Samsung_SM-F966B.jwlibrary`.

**Step 1 — extend the counter to capture text.** `offsets.py`'s `anchor_offsets`
already walks the document counting visible codepoints with exactly the firmware's
rules. Add a parallel accumulation of the visible text itself, so a
`[start, end)` range can be sliced out.

**Step 2 — re-extract.** For each of the 56, slice the visible text over its
stored range, then apply the same normalisation the firmware applies: collapse
whitespace runs, strip newlines, trim, truncate to 72 bytes on a UTF-8 boundary
(`Utf8.cpp:185-203`).

**Step 3 — take the reference from the existing label.** Split the current
`text` on the first ` · `. The reference half is already correct and already on
hardware; re-deriving it from the JW database would add risk for no gain.

**Step 4 — merge, do not replace.** Download the **live** file from the device
first and back it up. Match entries on `(spineIndex, range.start)` and rewrite
**only the `ref` and `text` fields** of a match. `si`, `start`, `end` and `t` are
never touched: the offsets are verified-correct on hardware, and `t` may carry
tag edits the user has made since the migration. An entry with no match — any
highlight created on the device since — is copied through untouched, and
Migration A handles its label.

**Step 5 — upload, then verify, then finish.** Upload the merged file,
re-download it, and diff against what was sent. Nothing is removed or considered
done until that diff is clean. (Recorded explicitly because an earlier step in
this project deleted a file before verifying its replacement, leaving the device
with no book.)

Note for the upload: the device's `WebServer` never answers `Expect: 100-continue`,
so `curl` needs `-H "Expect:"`.

---

## 5. Testing

### Host tests

`test/highlight_doc/HighlightDocTest.cpp`:

- A reference round-trips through `toJson`/`fromJson`.
- `ref` is omitted from the JSON when the reference is empty.
- A legacy entry (`text` with ` · `, no `ref`) splits into reference and passage.
- A legacy entry **with** `ref` present is not split again — idempotency.
- An entry with `"ref": ""` and a separator in `text` IS split (empty == absent).
- A legacy entry with no separator becomes passage-only, reference empty.
- A passage containing ` · ` splits at the first occurrence only.
- A reference longer than `MAX_REFERENCE_BYTES` is truncated on a UTF-8 boundary.

`test/utf8_summary/Utf8SummaryTest.cpp`:

- The `maxBytes` overload truncates at the requested cap and still respects UTF-8
  boundaries; the default remains 72 for every existing caller.

New `test/highlight_row_text/`:

- `composeSubtitle` joins with `\n` when both halves are present.
- No leading or trailing `\n` when either half is empty.
- Both empty yields an empty string.
- A tag name containing `\n` cannot inject an extra row line (the probe showed
  it produces a third line), so `composeSubtitle` strips control characters from
  the tag half.

### Build

`pio run -e x4pro` after the last code edit. The host suite does not compile every
firmware translation unit, so a green suite does not license a claim that the
firmware builds.

### On-device verification (human)

1. The 56 migrated highlights render as three lines with the reference alone on
   line one.
2. Their passages are visibly longer than before Migration B.
3. A newly created highlight renders identically to a migrated one — the
   consistency this feature exists to deliver.
4. A highlight with no tags renders as two lines, with no blank line.
5. Filtering by tag, editing tags, and deleting a highlight still work, and rows
   re-render at the correct height afterwards.
6. Scrolling past the first page and returning does not skip rows — the
   variable-height paging path.

---

## Risks

| Risk | Mitigation |
|---|---|
| A future edit sets `labelText.maxLines > 1` on this list and reintroduces the sizing-gate overlap | The reason is recorded in a comment at the call site, not only in this document |
| Migration B corrupts live highlights | Back up, merge by key rather than replace, verify by re-download before finishing |
| **Untested:** Migration B builds passages by slicing visible XHTML text, while the device builds them by joining word boxes (`PassageSelectActivity.cpp:233-258`). The two may differ in whitespace or punctuation, which would undercut the consistency this feature exists to deliver | Could not be tested locally: no device-created highlight with a known offset range is available off-device. Step 5 must diff one re-extracted passage against a device-created highlight over the same range **before** the merge is accepted |
| Reference and passage drift apart, as label and offsets could | They cannot: both are display-only, written together in one place, and neither is used to locate a passage |
| An older firmware build reads a file containing `ref` | Additive key, no version bump: the reference is ignored and the passage still renders |
