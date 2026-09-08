# Three-line highlight rows: reference, passage, tags

**Date:** 2026-09-08
**Status:** Design v2 — §2 revised after on-device testing (see "Revision: why the newline design was abandoned")
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

- **A fourth line.** The row is three lines: reference and tags on the first,
  passage across the next two.
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

In `HighlightsActivity::rebuildRowItems`:

```
item.label    = reference          (label line)
item.value    = tags               (right-aligned, same line)
item.subtitle = passage            props.subtitleText.maxLines = 2
```

Three lines: reference and tags share the first, the passage wraps across the
next two. **No embedded newline anywhere** — see the revision note below for why
that matters.

`props.subtitleText` is assigned from the theme *before* `maxLines` is set.
`Screen::list` only substitutes the theme font into a style still considered
unset (`FreeInkApp.h:249-251`), and `textStyleUnset` counts `maxLines == 1` among
its conditions (`FreeInkUICore.h:550-554`), so setting `maxLines` alone skips the
substitution and the subtitle renders with `font == 0`. The same trap is
documented at `SettingsActivity.cpp:483-487`. `labelText` is left untouched for
the same reason: its default `maxLines` is already 1.

### The tag value is capped

`list()` subtracts the value slot's measured width from the label's
(`list.h:578-584`). An unbounded tag list therefore drives the reference's
available width negative, `rect.empty()` becomes true, and `layoutText` returns
without drawing the reference at all. The joined tag string is capped at
`MAX_TAG_NAME_BYTES`, which keeps one maximum-length tag whole.

### Composition rules

| `reference` | `label` (passage) | tags | label | value | subtitle |
|---|---|---|---|---|---|
| set | set | set | reference | tags | passage |
| set | set | — | reference | — | passage |
| set | — | set | reference | tags | *(null)* |
| — | set | set | passage | tags | *(null)* |
| — | — | set | `tr(STR_UNNAMED)` | tags | *(null)* |

`item.subtitle` is left **null**, not pointed at an empty string: `list()` tests
the pointer rather than the string, so an empty one still reserves a blank line.

The `reference`-absent rows reproduce the previous two-line behaviour, which is
what a non-Bible book with no verse anchors produces.

---

## Revision: why the newline design was abandoned

Design v1 put `passage + "\n" + tags` in the subtitle with `maxLines = 3`,
on the basis that the SDK's `layoutText` hard-breaks on `'\n'`
(`FreeInkUICore.h:716-717`). That was verified against `layoutText` directly and
**it was the wrong thing to verify.** On hardware the tags rendered welded to the
end of the passage — `"...Sin embargo, laesperanza"`.

Two independent reasons, both in the draw path:

1. **`GfxRenderer::wrappedText` splits on `' '` only** (`GfxRenderer.cpp:1792`).
   `GfxRendererTarget::text` draws through it, not through `layoutText`
   (`FreeInkUIGfxRenderer.h:183`), so `'\n'` was never a break. Measurement went
   through `measureWrappedText` → `layoutText`, which *does* break on it, so
   every row reserved a line the draw never produced.
2. **`GfxRendererTarget::text` has a fast path** (`FreeInkUIGfxRenderer.h:172`)
   that draws any string measuring narrower than the row on a single line
   without consulting `wrappedText` at all. A `'\n'` has no glyph and so adds no
   width. Fixing (1) alone would still have failed in landscape, where a 72-byte
   passage plus a tag fits one line. That file is in the `freeink-sdk` submodule.

The lesson worth keeping: the earlier verification built a *fake* `DrawTarget` to
exercise `layoutText`. That proved the SDK's reference implementation behaves as
documented; it proved nothing about the renderer this firmware actually uses.
Any future claim about text layout must go through `GfxRendererTarget`.

Moving the tags to the value slot removes the dependency entirely — measurement
and drawing now take the same path — and gives the passage two lines instead of
one, which was the point of the feature.

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

There is no host suite for the row composition. `HighlightsActivity` cannot be
built off-device, and after the revision above the composition is three field
assignments with no rule worth extracting — the tag cap reuses the already-tested
`utf8SafeSummary`. The `test/highlight_row_text/` suite that existed for
`composeSubtitle` was removed with it.

The slot behaviour that a test *cannot* reach is covered by the on-device checks
below, which is the honest place for it: every attempt to verify this layer off
the device so far has verified the wrong thing.

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
4. A highlight with no tags renders without an empty value slot, and one with
   no passage renders without a blank second line.
5. A highlight carrying several long tags still shows its reference — the value
   slot must not consume the whole label width.
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
