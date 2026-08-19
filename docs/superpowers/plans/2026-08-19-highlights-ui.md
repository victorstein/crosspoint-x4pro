# Highlights UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the reader select a passage, highlight it, tag it, and browse this book's highlights by tag — rendering each highlight as an inverted block that survives re-pagination.

**Architecture:** A pure geometry core decides *which rectangles* a highlight covers (host-tested); the reader applies them with `GfxRenderer::invertRect` in every render pass. Three new activities handle selection, tagging, and browsing, on the `ActivityManager` stack.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest.

**Depends on:** foundations, anchoring, and data-layer plans — all complete. Baseline **206 host tests** at `d4413ba9`.

**Delivery:** fork-only.

> **This plan needs the device.** Every previous plan was verifiable from a build log. This one is not: ghosting, the grayscale interaction, and touch selection can only be judged by looking at a panel. Tasks 1–7 can be written and compiled without hardware; **Task 8 cannot be skipped**, and no task here should be called done on a green build alone.

---

## What already exists

| Piece | Where |
| --- | --- |
| `GfxRenderer::invertRect` | `lib/GfxRenderer/GfxRenderer.cpp`, 9 host tests on the bit core |
| Per-word offsets | `TextBlock::wordVisibleOffset(i)`, populated bidi-aware |
| Range predicate | `lib/Epub/Epub/VisibleRange.h` — `contains`, `overlaps`, half-open |
| Storage | `HighlightDoc` + `HighlightFile`, status-returning, atomic, `.tmp`-recovering |
| Menu headroom | `MAX_MENU_ITEMS` raised 16 → 24, all loops clamped |

Nothing consumes any of it yet. This plan is where the dead surface comes alive.

## Constraints that shape every task

**Input.** The X4 Pro has **no physical Back, Confirm, Left or Right** — only Up/Down (GPIO0/GPIO7) and Power (`BoardConfig.h:1392-1398`). Back and Confirm come from the GT911 touchscreen and the capacitive Home key. **Selection is a touch flow.** Do not design a button-only path and assume it works.

**The reader renders each page up to three times.** Text anti-aliasing is on by default (`CrossPointSettings.h:219`), and on a strip-grayscale panel `EpubReaderActivity.cpp:1387-1450` runs a B/W pass plus two more into the LSB/MSB planes, each in 80-row strips via a `renderGrayscalePass` lambda that calls `page->render(...)`. **A highlight applied only to the B/W framebuffer disappears under anti-aliasing.** The overlay must run wherever `page->render` runs.

`invertRect` already honours strip mode — it writes through `getWriteTarget()` and clips to the active band — so it is safe inside the strip loop. Whether inverting a *grayscale plane* produces the right visual result is a genuine open question, answered only on hardware (Task 8).

**Panel variants differ.** `supportsStripGrayscale()` is true for SSD1677 and UC8279 but **false for UC8179** (`Uc8179Driver.h:86`), and all three ship as X4 Pro panels (`BoardConfig.h:113-116`, auto-detected). Do not assume which one is in the device on your desk.

**`ResultVariant` is a closed `std::variant`** (`ActivityResult.h:71-73`). Returning tag selections requires adding an alternative to that shared type — a small edit to a central file.

**`SNAPSHOT_CAPACITY` is 4096 bytes** (`DictionaryWordSelectActivity.h:79`). One full-width 800px line at ~40px is ~4000 bytes, so two lines do not fit and `readFramebufferRegion` refuses (`GfxRenderer.cpp:1702-1704`), forcing a full two-pass page repaint per keypress.

**Long-press is one exclusive slot.** `SETTINGS.longPressMenuFunction` switches over five mutually-exclusive values (`EpubReaderActivity.cpp:409-436`). A highlight option costs the user whichever action they use today.

**Activity launch pattern**, from `EpubReaderActivity.cpp:286`:

```cpp
startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page), ...));
```

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/HighlightGeometry.h` / `.cpp` (create) | Pure: word boxes + ranges → rectangles to invert |
| `src/activities/reader/HighlightOverlay.h` / `.cpp` (create) | Walks a `Page`, calls the geometry core, issues `invertRect` |
| `src/activities/reader/EpubReaderActivity.cpp` (modify) | Apply the overlay in every render pass; load/save highlights |
| `src/activities/reader/PassageSelectActivity.{h,cpp}` (create) | Anchor-then-extend selection |
| `src/activities/reader/TagPickerActivity.{h,cpp}` (create) | Pick from the palette, or type a new tag |
| `src/activities/reader/HighlightsActivity.{h,cpp}` (create) | Browse and filter this book's highlights |
| `src/activities/ActivityResult.h` (modify) | Add a tag-selection alternative |
| `src/activities/reader/EpubReaderMenuActivity.{h,cpp}` (modify) | Two new menu entries |
| `test/highlight_geometry/` (create) | Host tests for the geometry core |

---

### Task 1: Enlarge the selection snapshot buffer

Multi-line selection is unusable at 4096 bytes — every extension triggers a full page repaint with a glyph reload. With 8MB of PSRAM this is cheap.

**Files:** `src/activities/reader/DictionaryWordSelectActivity.h`

- [ ] **Step 1: Size it for a realistic selection**

`SNAPSHOT_CAPACITY` must hold the framebuffer region under a multi-line selection: `widthBytes × lineHeight × lines`. At 100 bytes/row and ~40px lines, six lines is ~24,000 bytes. Raise it to **32768** and note why:

```cpp
  // Framebuffer bytes saved under the selection highlight so a cursor move can
  // restore them instead of re-rendering the page. 4096 held barely one
  // full-width line, so any multi-line selection fell back to a full two-pass
  // repaint per keypress. Sized for ~6 lines at 800px; the X4 Pro has 8MB PSRAM
  // and allocation failure already degrades gracefully to the full-repaint path.
  static constexpr size_t SNAPSHOT_CAPACITY = 32768;
```

- [ ] **Step 2: Confirm the allocation still degrades gracefully**

Read the allocation site. If it cannot allocate, `snapshot` is null and `drawHighlightWithSnapshot` already falls back. Verify that path is intact — on the **C3** this allocation is far more likely to fail, and `default` is still a C3 build.

- [ ] **Step 3: Build both boards and commit**

```bash
pio run -e x4pro && pio run -e default
git add src/activities/reader/DictionaryWordSelectActivity.h
git commit -m "perf(reader): size the selection snapshot for multi-line ranges"
```

---

### Task 2: Pure highlight geometry

Which rectangles a highlight covers is arithmetic, and arithmetic is testable without a panel.

**Files:**
- Create: `lib/Epub/Epub/HighlightGeometry.h`, `lib/Epub/Epub/HighlightGeometry.cpp`
- Create: `test/highlight_geometry/HighlightGeometryTest.cpp`, `test/highlight_geometry/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// test/highlight_geometry/HighlightGeometryTest.cpp
#include <gtest/gtest.h>

#include <vector>

#include "Epub/HighlightGeometry.h"

namespace {

// Three words on one row, then two on the next.
std::vector<HighlightWord> sampleWords() {
  return {
      {100, 0, 0, 50, 40},   // offset 100, x 0,   y 0,  w 50
      {110, 60, 0, 40, 40},  // offset 110, x 60,  y 0,  w 40
      {120, 110, 0, 30, 40}, // offset 120, x 110, y 0,  w 30
      {130, 0, 40, 45, 40},  // offset 130, x 0,   y 40, w 45
      {140, 55, 40, 35, 40}, // offset 140, x 55,  y 40, w 35
  };
}

}  // namespace

TEST(HighlightGeometry, EmptyRangeCoversNothing) {
  EXPECT_TRUE(highlightRects(sampleWords(), {VisibleRange{120, 120}}).empty());
}

TEST(HighlightGeometry, ASingleWordYieldsOneRect) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{110, 111}});
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 60);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[0].w, 40);
  EXPECT_EQ(rects[0].h, 40);
}

TEST(HighlightGeometry, AdjacentWordsOnOneRowMergeIntoOneRect) {
  // Words at offsets 100 and 110 sit at x=0..50 and x=60..100 on the same row.
  // Merging avoids a seam of un-inverted background between them.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}});
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].w, 100) << "spans from the first word's left to the second word's right";
}

TEST(HighlightGeometry, ARangeSpanningTwoRowsYieldsARectPerRow) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{110, 140}});
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[1].y, 40) << "rows never merge — they are not contiguous in y";
}

TEST(HighlightGeometry, HalfOpenAtBothEnds) {
  // start is inclusive, end exclusive: 100..120 takes offsets 100 and 110, not 120.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}});
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].w, 100) << "word at offset 120 must be excluded";
}

TEST(HighlightGeometry, WordsOutsideEveryRangeAreIgnored) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{500, 600}});
  EXPECT_TRUE(rects.empty());
}

TEST(HighlightGeometry, MultipleRangesEachContribute) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 105}, VisibleRange{140, 145}});
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[1].y, 40);
}

TEST(HighlightGeometry, AWordIsNotDuplicatedByOverlappingRanges) {
  // Two ranges both covering offset 110 must not invert it twice — a double
  // invert is a no-op and would leave the word looking un-highlighted.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}, VisibleRange{105, 125}});
  for (size_t i = 1; i < rects.size(); ++i) {
    EXPECT_NE(rects[i].x, rects[i - 1].x) << "overlapping ranges produced duplicate rects";
  }
}

TEST(HighlightGeometry, UnsortedWordsStillProduceCorrectRects) {
  // TextBlock word order is VISUAL, not logical — on an RTL line the offsets
  // run backwards. The geometry must not assume ascending offsets.
  auto words = sampleWords();
  std::reverse(words.begin(), words.end());
  const auto rects = highlightRects(words, {VisibleRange{110, 111}});
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 60);
}
```

- [ ] **Step 2: Register the suite** (model on `test/visible_range/CMakeLists.txt`, include `${REPO_ROOT}/lib/Epub`; **no `${REPO_ROOT}/src`**)

- [ ] **Step 3: Run and confirm failure**

- [ ] **Step 4: Write the header**

```cpp
// lib/Epub/Epub/HighlightGeometry.h
#pragma once

#include <cstdint>
#include <vector>

#include "VisibleRange.h"

// A rendered word reduced to what highlighting needs: its anchor offset and its
// box. Screen coordinates, already including margins and ruby shift.
struct HighlightWord {
  uint32_t offset;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

struct HighlightRect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

// Rectangles to invert so every word inside any range is covered.
//
// Words on the same row whose boxes are contiguous are merged, so the inter-word
// gap inverts too and a multi-word highlight reads as one block rather than
// striped text. Rows never merge. A word covered by several ranges yields one
// rect, not several — inverting twice restores the original pixels.
//
// Word order is NOT assumed to be ascending by offset: TextBlock stores words in
// visual order, which runs backwards on an RTL line.
std::vector<HighlightRect> highlightRects(const std::vector<HighlightWord>& words,
                                          const std::vector<VisibleRange>& ranges);
```

- [ ] **Step 5: Implement, run passing, commit**

Sort candidate words by `(y, x)` before merging — the input may be in visual order. Merge on the same `y` when the next box starts at or before the previous box's right edge plus the inter-word gap.

```bash
git commit -m "feat(epub): add host-tested highlight rectangle geometry"
```

---

### Task 3: Apply the overlay in every render pass

**Files:**
- Create: `src/activities/reader/HighlightOverlay.h`, `.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Write the page walker**

`HighlightOverlay::apply(renderer, page, ranges, fontId, marginLeft, marginTop)` builds `HighlightWord`s and issues `renderer.invertRect` for each returned rect.

The walk mirrors `DictionaryWordSelectActivity.cpp:74-95` exactly — that code is the working reference:

```cpp
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      // x = line->xPos + block->wordXpos(i) + marginLeft
      // y = line->yPos + marginTop + rubyShift
      // offset = block->wordVisibleOffset(i)
    }
  }
```

Width comes from the renderer's advance for the word text and style, the same measurement word-select performs. Height is the line height.

**Do not filter by `isSelectableToken`.** Word-select skips punctuation because you cannot look it up in a dictionary; a highlight must cover it or the block will have holes.

- [ ] **Step 2: Apply it in all three passes**

In `EpubReaderActivity.cpp`, after the B/W `page->render(...)`, and **inside the `renderGrayscalePass` lambda** (`:1396-1400`) after its `page->render(...)`. The lambda runs per 80-row strip for both planes; `invertRect` clips to the active strip, so the same call is correct in all three places.

```cpp
  // Applied after every page->render, in the B/W pass and inside
  // renderGrayscalePass, because anti-aliasing re-renders the page into the
  // LSB/MSB planes. An overlay only on the B/W framebuffer vanishes under AA.
```

- [ ] **Step 3: Load the book's highlights on entry**

`HighlightFile::load` on book open; keep the `HighlightDoc` on the activity. On `LoadResult::Failed`, **do not save** for the rest of the session and surface a message — the file may still hold the user's data.

Only ranges for the current spine index matter: use `HighlightDoc::findBySpine`.

- [ ] **Step 4: Build both boards and commit**

There is no host test for this task — it is renderer and activity code. The geometry beneath it is covered by Task 2; correctness of the *visual* result is Task 8.

---

### Task 4: `PassageSelectActivity`

**Files:** create `src/activities/reader/PassageSelectActivity.{h,cpp}`

Derive from the structure of `DictionaryWordSelectActivity` — word-box extraction, hit-testing, differential repaint — and add a second anchor.

- [ ] **Step 1: Two-anchor selection**

Tap or arrow to the first word and confirm; tap or arrow to the last and confirm. Selection-in-progress renders as an **outlined box**, deliberately distinct from a committed highlight's inverted block, or the user cannot tell what is already saved.

Remember there is no physical Confirm on this device — confirm is a tap or the capacitive Home key.

- [ ] **Step 2: Action bar and result**

Highlight / Tag / Cancel. On confirm, build a `HighlightEntry` with `spineIndex`, the `VisibleRange` from the two anchors' offsets (half-open: `end` is the last word's offset + 1), and the label from the selected text.

**Take offsets from `wordVisibleOffset`, never recompute them from the text.** Offsets are counted on the raw parse stream and include collapsed whitespace and skipped content; any independent recount diverges, and NFC composition shifts intra-word offsets for NFD source text.

- [ ] **Step 3: Save through `HighlightFile`**

Honour `SaveResult`: on `TooLarge`, tell the user the book has too many highlights rather than failing silently.

- [ ] **Step 4: Build both boards and commit**

---

### Task 5: `TagPickerActivity` and the result type

**Files:** create `src/activities/reader/TagPickerActivity.{h,cpp}`; modify `src/activities/ActivityResult.h`

- [ ] **Step 1: Extend `ResultVariant`**

It is a closed variant (`ActivityResult.h:71-73`), so add an alternative:

```cpp
struct TagSelectionResult {
  std::vector<uint16_t> tagIndices;
};
```

and include it in the `using ResultVariant = std::variant<...>` list. Adding to a shared central type is unavoidable here — note it in the commit message.

- [ ] **Step 2: The picker**

List the book's palette with a check state per tag; a "New tag…" row pushes the existing `KeyboardEntryActivity` and calls `HighlightDoc::addTag`. Handle `addTag` returning `nullopt` — the palette is full, or the name is empty or too long — with a visible message rather than a silent no-op.

- [ ] **Step 3: Return the selection**, build both boards, commit

---

### Task 6: `HighlightsActivity`

**Files:** create `src/activities/reader/HighlightsActivity.{h,cpp}`

- [ ] **Step 1: Browse**

List this book's highlights by label, most recent first. Selecting one jumps to it via the existing offset-jump path (`Section::getPageForVisibleTextOffset`, as `EpubReaderActivity.cpp:626-634` already does for KOReader sync).

- [ ] **Step 2: Filter by tag**

A tag filter row narrows the list. Tags are per-book by design — there is no cross-book view and this screen must not imply one.

- [ ] **Step 3: Delete**

`HighlightDoc::removeHighlight` then save. This is the only destructive action in the feature; confirm before deleting.

- [ ] **Step 4: Build both boards and commit**

---

### Task 7: Menu and long-press wiring

**Files:** modify `EpubReaderMenuActivity.{h,cpp}`, `EpubReaderActivity.cpp`

- [ ] **Step 1: Two menu entries**

`HIGHLIGHT_PASSAGE` and `HIGHLIGHTS`. The cap is 24 with every loop clamped, so there is headroom — but re-check the count: 13 unconditional + footnotes + bookmarks + frontlight + these two is 18.

**`HighlightsActivity` sits beside the bookmarks entry, not replacing it.** Bookmarks and highlights are different things.

- [ ] **Step 2: Long-press option**

Add `LP_MENU_HIGHLIGHT` to `SETTINGS.longPressMenuFunction`. Say plainly in the setting's description that choosing it replaces the current long-press action — the slot is exclusive.

- [ ] **Step 3: Build both boards, full host suite, commit**

---

### Task 8: On-device verification — mandatory

Nothing above is confirmed until this passes. Record results in `docs/superpowers/notes/`.

- [ ] **Step 1: Anchoring survives re-pagination**

Highlight a passage. Change the font size **two steps** in each direction, change margins, rotate the screen. The highlight must cover **the same words** every time. This is the property the whole feature rests on and the one the host suite can only approximate.

- [ ] **Step 2: Anti-aliasing**

With text AA **on** (the default), confirm the highlight renders correctly and does not vanish or flicker between passes. Then turn AA **off** and confirm it still renders. If it appears only with AA off, the overlay is missing from the grayscale planes.

- [ ] **Step 3: Ghosting**

Turn several pages with a highlight on screen, then to a page without one. Look for residual shadow where the inverted block was. If it ghosts badly, a full refresh on highlight change is the fallback.

- [ ] **Step 4: Panel variant**

Note which controller the unit reports (SSD1677 / UC8179 / UC8279). UC8179 has no strip grayscale, so its render path differs — record which one was tested, because the other two are then unverified.

- [ ] **Step 5: RTL**

With an RTL EPUB, highlight a passage and confirm it covers the intended words rather than mirrored ones. The host suite covers the anchor permutation; this covers the geometry.

- [ ] **Step 6: Selection feel**

Multi-line selection after Task 1's buffer change: does extending across lines repaint smoothly, or still stutter? Record the subjective result — this is the one thing no test can express.

- [ ] **Step 7: Durability**

Create highlights, power off uncleanly mid-save if you can, and confirm nothing is lost. Check `/.crosspoint/highlights/` for orphaned `.tmp` files.

- [ ] **Step 8: Record and commit**

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Highlight missing from the grayscale planes | High | Applied inside `renderGrayscalePass`; Task 8 Step 2 tests AA on *and* off |
| Inverting a grayscale plane looks wrong | High | Genuinely unknown until Task 8; fallback is to force B/W refresh while a highlight is on screen |
| Ghosting from inverted blocks | Medium | Task 8 Step 3; fallback is a full refresh on change |
| Only one panel variant gets tested | Medium | Record which; treat the others as unverified |
| Multi-line selection still stutters | Medium | Task 1 raises the buffer; Task 8 Step 6 judges it |
| Long-press change annoys the user | Low | Opt-in setting, described honestly |

## Deliberately out of scope

- **Cross-book tags.** Per-book by design.
- **Notes.** Tags only.
- **On-page tag markers.** Decided against: a marker inside an inverted block clutters the page for a question rarely asked mid-read, and it stays purely additive to add later.
- **Overlap policy.** `addHighlight` neither merges nor rejects overlapping ranges. The geometry de-duplicates rects so overlaps render correctly; whether the *data model* should permit them is a product decision this plan leaves open.
