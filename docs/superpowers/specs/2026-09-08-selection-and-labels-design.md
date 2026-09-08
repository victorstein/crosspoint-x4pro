# Cross-page selection, reference labels, and tag-picker commit

**Date:** 2026-09-08
**Status:** Design v1
**Target:** CrossPoint Reader firmware, `x4pro` build target (UC8179 panel, ESP32-S3, 8MB PSRAM)
**Delivery:** fork-only, continuing `feat/tagged-highlights`

## Goal

Three defects and gaps found by using the shipped tagged-highlights feature on real
hardware with a real corpus (a 3,937-spine Bible carrying 56 migrated highlights):

1. A passage split across a page boundary cannot be highlighted at all.
2. A highlight created on the device is labelled with passage text, while the 56
   migrated ones are labelled `Mateo 11:19`. The list is inconsistent.
3. Tags picked for a new highlight are silently discarded unless the user exits
   through an unadvertised gesture.

## Non-goals

- **Backward page turns during selection.** Forward-only (decided).
- **Cross-book tags, notes, on-page tag markers.** Still out, per the original spec.
- **Re-labelling the 56 migrated highlights** beyond adding snippets for consistency.
- **Bible-awareness as a general feature.** Reference labels are best-effort and
  degrade to today's behaviour when a book has no verse anchors.

---

## Feature 1: selection across a page boundary

### Why it cannot work today

`PassageSelectActivity` stores both endpoints as indices into the current page's
word array — `cursor` and `anchorIndex` (`PassageSelectActivity.h:112-113`) — and
`extractWords()` builds that array from a single `Page` walked at
`PassageSelectActivity.cpp:77-115`. The activity receives a `std::unique_ptr<Page>`
and never sees the `Section` (constructor, `PassageSelectActivity.h:41-50`), so it
has no way to reach the next page even if the indices allowed it.

### Design

**Store the first anchor as an absolute visible offset, not a page index.**

`anchorIndex` becomes `anchorOffset` (`uint32_t`, the same coordinate space
`WordBox::offset` and `VisibleRange` already use). The committed range stays
`[min, max+1]` resolved by scanning, which the existing RTL comment
(`PassageSelectActivity.cpp:250-253`) already requires and which keeps working
unchanged when the two endpoints come from different pages.

**Memory cost is O(1) in pages crossed.** Only the current page's `words` and
`committedRects` are resident, exactly as today; crossing a boundary rebuilds them
rather than accumulating. Nothing per-crossed-page is retained, because earlier
pages are not on screen and their geometry is never needed again. This is why
forward-unlimited is affordable on this device.

**Page turning.** The activity takes a `Section&` alongside its `Page`.
`openHighlightPassage` (`EpubReaderActivity.cpp:328-353`) already holds a live
`section` when it pushes the activity and does not reset it — unlike the
`TEXT_SETTINGS` and `SELECT_CHAPTER` paths, which deliberately release it. On a
forward turn the activity calls `section.loadPage(n+1)`, replaces `page`, and
re-runs `extractWords()` and the committed-rect rebuild.

**Gesture.** Right-to-left swipe advances a page. It is currently unbound in this
activity; the left-edge swipe is already Back
(`MappedInputManager.cpp:266-271`), so the two do not collide. The gesture is only
live in `Phase::PickingEnd` — turning pages before the first anchor exists has no
meaning, and `Phase::ChoosingAction` is absorbed by the popup.

**End of section.** A forward turn past the last page of the spine item does
nothing. Selection does not cross spine items: `HighlightEntry.spineIndex` is a
single value, so a cross-spine range is unrepresentable in the stored format.
This is a real limit and is stated rather than worked around.

### Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Anchor offset no longer on any rendered page; preview looks wrong | Medium | The outline draws only words whose offset falls in range; off-page anchors simply have no rects |
| A long selection spans many pages and the user forgets where it started | Low | Status/footer shows selection is in progress; the action popup names the passage |
| Page turn mid-selection races the render task | Medium | Rebuild under `RenderLock`, matching the tag-deletion fence added in `0c1c884a` |

---

## Feature 2: reference + snippet labels

### The lookup problem

A label needs `(spineIndex, startOffset)` resolved to `Mateo 11:19`. The anchor map
CrossPoint already builds is **anchor → page** (`ChapterHtmlSlimParser.cpp:241`),
and it is serialized into the section cache with a patched `anchorMapOffset`
(`Section.cpp:549-560`). A page is far too coarse: this Bible puts several verses
on one page.

### Two approaches

**(A) Add an offset to the cached anchor map.** Precise and O(log n) to query, but
the on-disk layout changes, so `SECTION_FILE_VERSION` (42, `Section.cpp:48`) and
`SECTION_FILE_PARTIAL_VERSION`, which must move in lockstep (`Section.cpp:61-65`),
both bump — invalidating every cached section on every device. For the corpus that
motivated this work that is a 3,937-section re-index.

**(B) Resolve on demand at save time — recommended.** Walk the current spine
item's XHTML once, counting visible codepoints and recording each
`chapterN_verseM` anchor's offset, then pick the greatest anchor offset `<=` the
highlight's start. No layout, no fonts, no format change, no re-index.

The counter is small and fully specified by `characterData`
(`ChapterHtmlSlimParser.cpp:1147-1158`): count UTF-8 codepoints while
`insideBody && nonVisibleTextDepth == 0 && !syntheticCharacterData`. Non-visible
elements are `head`, `style`, `script`, `title`, `rp`
(`VisibleTextUtils.h:17-20`). Synthetic data is parser-injected heading and image
`alt` text (`ChapterHtmlSlimParser.cpp:549-551`, `:861-863`), which does not exist
in the source stream and therefore needs no handling in a second pass.

**(B) is chosen.** These chapter files are a few KB of visible text, the pass runs
once per save alongside SD I/O that already dominates, and it avoids making every
user of every book pay a re-index for a label.

**This counter has already been validated against production output.** An
independent implementation of exactly this walk reproduced the firmware's stored
offsets for a hand-made highlight: spine index matched, and the start differed by
3 codepoints, fully accounted for by the verse-number token and one space. A wrong
counter fails by orders of magnitude, not by three.

### Label format

`<book> <chapter>:<verse> · <passage snippet>`, e.g. `Mateo 11:19 · <first words>`.

`HighlightDoc::addHighlight` already normalises and truncates the label to 72 bytes
via `utf8SafeSummary` (`HighlightDoc.cpp:42`), so this side supplies raw text and
the collapse-and-truncate rule stays in one place.

**Book name** comes from the TOC entry covering the spine item, not from a built-in
book table — a hardcoded list would be wrong in every language and every non-Bible
book.

**Degradation.** No `chapterN_verseM` anchors resolved → no reference → the label
is passage text alone, which is exactly today's behaviour. Non-Bible EPUBs are
unaffected.

### Consistency with the migrated 56

The 56 imported entries carry reference-only labels. Once new highlights carry
`reference · snippet`, those 56 are regenerated with snippets so the list is
uniform. This is a data change, not a code change, and is reversible: the import
is reproducible from the backup.

### Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Second-pass counter drifts from the real parser | **High** | Host tests assert both walks agree on real chapter files; this is the one property the feature rests on |
| Save latency grows noticeably | Medium | Measure; the pass is bounded by spine-item size, and these are a few KB |
| Anchor naming differs in another EPUB | Low | Pattern match `chapterN_verseM`; no match means graceful degradation |

---

## Feature 3: tag-picker commit

### The defect

`TagPickerActivity` commits only through `handleHomeGesture()`
(`TagPickerActivity.cpp:204-211`). Both `onBackButton()` (`:197-202`) and the
`Button::Back` release in `handleButtons()` (`:310-313`) set `isCancelled` and
discard the selection. The footer advertises `BACK / TOGGLE / UP / DOWN`
(`:45-50`) and never names the committing gesture, so the exit the screen names is
the one that destroys the user's work.

Cancel is already only half-honoured: the picker persists palette changes itself,
so a tag created during a cancelled visit survives.

### Design

**Add a "Done" row as the first row**, which calls the existing `commitAndFinish()`.
First rather than last because a palette can now hold 100 tags and a trailing row
would sit below a long scroll. **Back stays a genuine cancel** — `applyTagEdit`
(`HighlightsActivity.cpp:240-258`) depends on `isCancelled` meaning "leave this
highlight's tags alone", and removing that would make an aborted edit apply itself.
**The footer is corrected** to name the commit action. Home keeps working.

Row indices shift by one, so `activateIndex`, `onRowLongPress` (which must not
offer to delete the "Done" row), and the `selected_` mapping all need the offset
applied consistently — the exact class of seam bug the August reviews caught twice.

### Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Off-by-one from the inserted row reaches `selected_` or delete | **High** | One helper converts row index to tag index; every site uses it |
| "Done" reads as a tag on a monochrome list | Low | Rendered as a non-toggle row; verified on device |

---

## Testing

Host-testable: the anchor-offset counter (against real chapter files and against
the main parser's own output), row-index mapping for the inserted "Done" row, and
range resolution when the two anchors come from different pages.

Device-only: page-turn feel during selection, label legibility at 72 bytes on a
1-bit panel, and that a selection spanning a boundary lands on the right words
after a font-size change.

## Open questions

None blocking. Feature 2's approach (B) is the one most worth challenging: it
trades a one-time format bump for a small repeated parse.
