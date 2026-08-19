# Highlights UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the reader select a passage, highlight it, tag it, and browse this book's highlights by tag — rendering each highlight as an inverted block that survives re-pagination.

**Architecture:** A pure geometry core turns word positions and offset ranges into rectangles (host-tested). The reader computes those rectangles **once per page** and replays them through `GfxRenderer::invertRect` in the B/W pass only. Three new activities handle selection, tagging, and browsing.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest.

**Depends on:** foundations, anchoring, and data-layer plans — all complete. Baseline **206 host tests** at `d4413ba9`.

**Delivery:** fork-only.

> **v2 — revised after adversarial review.** v1 deferred a question to hardware that the source answers definitively, and the answer was that its approach was wrong. It also contained a task that did nothing while eating 65% of the C3's free heap, a CMakeLists that would not link, a "reuse" that aborts under `-fno-exceptions`, long-press wiring that missed the only switch that fires on this device, and three defective tests including one that passes against an implementation returning nothing. All corrected below.

---

## The finding that reshaped this plan

v1 said applying `invertRect` inside `renderGrayscalePass` was safe and that whether it *looked* right was "unknown until hardware." Both halves were wrong.

**The grayscale planes are not images.** Each is a sparse 1-bit *"drive this pixel with the gray waveform"* mask. Every pass starts from `renderer.clearScreen(0x00)` (`EpubReaderActivity.cpp:1441`), and bits are set only on anti-aliased glyph edges:

```cpp
// GfxRenderer.cpp:519-527
} else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
  // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
  renderer.drawPixel(screenX, screenY, false);
```

XOR-ing a rectangle across that mask flips **every background pixel to "drive to gray"** and **clears every AA edge**. The result is a uniform gray slab with the glyphs punched out — not an inverted highlight. This is framebuffer-level and therefore identical on SSD1677, UC8179 and UC8279; the drivers differ only in whether they stream the planes verbatim or inverted.

`invertRect` never consults `renderMode` (`GfxRenderer.cpp:1153-1186`) — and its own header comment already says so (`GfxRenderer.h:249`, *"It flips framebuffer bits regardless of renderMode"*). v1 documented the hazard and then designed into it.

**Consequence:** the overlay runs in the **B/W pass only**, and `invertRect` gains a `renderMode` guard so the primitive is safe for every future caller.

**Accepted limitation:** anti-aliased glyph edges inside a highlight keep their original gray, which on an inverted block will read as slight fringing. Swapping AA levels inside a highlight means rewriting both planes coherently — a different operation, deliberately out of scope. Task 8 judges whether the fringing is even noticeable at 219 PPI.

## Constraints that shape every task

**Input.** The X4 Pro has **no physical Back, Confirm, Left or Right** — only Up/Down and Power (`BoardConfig.h:1392-1398`). Back and Confirm come from the touchscreen and the capacitive Home key. Selection is a touch flow, and long-press arrives through the **Home-key** switch, not the front-Confirm one.

**Compute rects once.** `renderPlaneToBuffer` loops in 80-row strips per plane, so `renderGrayscalePass` runs ~12 times per page turn on a 480-row panel. Walking every word and allocating vectors inside it would reintroduce exactly the allocation churn the `TextBlock` arena exists to prevent (`TextBlock.h:14-17`: *"~250 throwing allocations per page load… the primary driver of heap fragmentation on the ESP32-C3"*).

**Widths come from stored positions, not the renderer.** `TextBlock` already holds every word's x (`wordXpos(i)`), so word *i*'s right edge is `wordXpos(i+1)` for all but the last on a line. Measuring via `getTextAdvanceX` would require `ensureSdCardFontReady` to prewarm the advance table first (`DictionaryWordSelectActivity.cpp:105-108`) — without it, measurement falls into `onGlyphMiss()` and reads glyph metadata from SD per glyph, and the table caps at 768 entries per style so CJK misses by default.

**`std::get` aborts.** The build is `-fno-exceptions` (`platformio.ini:60`), so `std::get` on the wrong `ResultVariant` alternative calls `std::terminate`. Result routing must match exactly.

**C3 memory.** `platformio.ini:49` puts a reading session at *"~50KB free heap."* A resident 400-entry `HighlightDoc` is ~19 KB of vector storage plus per-label heap, and `HighlightFile::load` holds two `JsonDocument`s live at once. See Task 3 Step 4.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/GfxRenderer/GfxRenderer.cpp` (modify) | `invertRect` early-outs unless `renderMode == BW` |
| `lib/Epub/Epub/HighlightGeometry.h` / `.cpp` (create) | Pure: word boxes + ranges → rectangles |
| `src/activities/reader/HighlightOverlay.h` / `.cpp` (create) | Walks a `Page` once, produces rects |
| `src/activities/reader/EpubReaderActivity.cpp` (modify) | Cache rects per page, replay in the B/W pass |
| `src/activities/reader/PassageSelectActivity.{h,cpp}` (create) | Anchor-then-extend selection, own snapshot sizing |
| `src/activities/reader/TagPickerActivity.{h,cpp}` (create) | Palette picker |
| `src/activities/reader/HighlightsActivity.{h,cpp}` (create) | Browse and filter |
| `src/activities/ActivityResult.h` (modify) | Tag-selection alternative |
| `src/CrossPointSettings.h`, `src/SettingsList.h`, `EpubReaderActivity.cpp`, `EpubReaderMenuActivity.{h,cpp}` (modify) | Menu and long-press wiring — **four** sites |
| `test/highlight_geometry/` (create) | Host tests |

---

### Task 1: Make `invertRect` safe outside the B/W pass

**Files:** `lib/GfxRenderer/GfxRenderer.cpp`, `lib/GfxRenderer/GfxRenderer.h`

- [ ] **Step 1: Add the guard**

In `GfxRenderer::invertRect`, beside the existing early-outs:

```cpp
  // The grayscale planes are sparse "drive this pixel with the gray waveform"
  // masks cleared to 0x00 each pass, not images. XOR-ing a rect across one sets
  // every background pixel to "drive to gray" and clears the anti-aliased glyph
  // edges — a solid slab with the glyphs punched out, on every panel. Inverting
  // is only meaningful on the B/W framebuffer.
  if (renderMode != BW) return;
```

- [ ] **Step 2: Correct the header comment**

`GfxRenderer.h:249` currently warns that the method flips bits regardless of `renderMode`. Replace that with the guarantee it now provides: the call is a no-op outside `BW`, so a caller inside a grayscale pass is safe rather than silently wrong.

- [ ] **Step 3: Build both boards and commit**

```bash
pio run -e x4pro && pio run -e default
git add lib/GfxRenderer/GfxRenderer.h lib/GfxRenderer/GfxRenderer.cpp
git commit -m "fix(gfx): make invertRect a no-op outside the B/W render mode"
```

---

### Task 2: Pure highlight geometry

**Files:**
- Create: `lib/Epub/Epub/HighlightGeometry.h`, `.cpp`
- Create: `test/highlight_geometry/HighlightGeometryTest.cpp`, `test/highlight_geometry/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// test/highlight_geometry/HighlightGeometryTest.cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "Epub/HighlightGeometry.h"

namespace {

// Three words on one row, then two on the next. Widths derived from the next
// word's x, as the real walk does.
std::vector<HighlightWord> sampleWords() {
  return {
      {100, 0, 0, 50, 40},    // offset 100, x 0,   w 50 -> right edge 50
      {110, 60, 0, 40, 40},   // offset 110, x 60,  w 40 -> right edge 100
      {120, 110, 0, 30, 40},  // offset 120, x 110, w 30
      {130, 0, 40, 45, 40},   // next row
      {140, 55, 40, 35, 40},
  };
}

constexpr int16_t kGap = 12;  // merge tolerance wider than the 10px inter-word gap

}  // namespace

TEST(HighlightGeometry, EmptyRangeCoversNothing) {
  EXPECT_TRUE(highlightRects(sampleWords(), {VisibleRange{120, 120}}, kGap).empty());
}

TEST(HighlightGeometry, ASingleWordYieldsOneRect) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{110, 111}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 60);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[0].w, 40);
  EXPECT_EQ(rects[0].h, 40);
}

TEST(HighlightGeometry, AdjacentWordsOnOneRowMergeIntoOneRect) {
  // Words at 100 and 110 occupy x 0..50 and 60..100 — a 10px gap. Merging makes
  // the gap invert too, so the highlight reads as one block, not striped text.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].w, 100);
}

TEST(HighlightGeometry, AGapWiderThanTheToleranceDoesNotMerge) {
  // The same two words with a tolerance below the 10px gap must stay separate.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}}, 4);
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].w, 50);
  EXPECT_EQ(rects[1].x, 60);
}

TEST(HighlightGeometry, ARangeSpanningTwoRowsYieldsARectPerRow) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{110, 140}}, kGap);
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[1].y, 40) << "rows never merge — they are not contiguous in y";
}

TEST(HighlightGeometry, EndIsExclusive) {
  // 100..120 takes the words at 100 and 110; the word at exactly 120 is excluded,
  // so the rect stops at x=100 rather than extending to 140.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x + rects[0].w, 100) << "the word at offset 120 must not be covered";
}

TEST(HighlightGeometry, WordsOutsideEveryRangeAreIgnored) {
  EXPECT_TRUE(highlightRects(sampleWords(), {VisibleRange{500, 600}}, kGap).empty());
}

TEST(HighlightGeometry, MultipleRangesEachContribute) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 105}, VisibleRange{140, 145}}, kGap);
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[1].y, 40);
}

TEST(HighlightGeometry, OverlappingRangesDoNotDoubleCoverAWord) {
  // Inverting the same pixels twice restores them, so a word covered by two
  // ranges would render UN-highlighted. Both ranges cover offsets 100 and 110,
  // which merge to exactly one rect.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}, VisibleRange{105, 125}}, kGap);
  ASSERT_EQ(rects.size(), 1u) << "the two ranges together cover 100,110,120 on one row";
  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].x + rects[0].w, 140) << "including the word at 120, which the second range reaches";
}

TEST(HighlightGeometry, VisualOrderInputStillProducesCorrectRects) {
  // TextBlock word order is VISUAL, not logical — on an RTL line the offsets run
  // backwards. The geometry must not assume ascending offsets.
  auto words = sampleWords();
  std::reverse(words.begin(), words.end());
  const auto rects = highlightRects(words, {VisibleRange{110, 111}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 60);
}
```

- [ ] **Step 2: Register the suite**

Model on **`test/highlight_doc/CMakeLists.txt`**, not `visible_range` — `VisibleRange` is header-only, but `HighlightGeometry` has a `.cpp` that must be listed or the link fails:

```cmake
add_executable(HighlightGeometryTest
  HighlightGeometryTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/HighlightGeometry.cpp
)

target_include_directories(HighlightGeometryTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(HighlightGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HighlightGeometryTest)
```

Add `add_subdirectory(highlight_geometry)` to `test/CMakeLists.txt`. **No `${REPO_ROOT}/src`.**

- [ ] **Step 3: Run and confirm failure**

- [ ] **Step 4: Write the header**

```cpp
// lib/Epub/Epub/HighlightGeometry.h
#pragma once

#include <cstdint>
#include <vector>

#include "VisibleRange.h"

// A rendered word reduced to what highlighting needs. Screen coordinates,
// already including margins and ruby shift. Width is derived from the next
// word's x position, never measured through the renderer.
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
// Words on the same row are merged when the next box starts within `gapTolerance`
// of the previous box's right edge, so inter-word gaps invert too and a
// multi-word highlight reads as one block rather than striped text. The
// tolerance is a parameter because inter-word gaps scale with font size: a fixed
// value under-merges at large sizes and can merge across a paragraph indent at
// small ones. Rows never merge.
//
// A word covered by several ranges yields ONE rect. Inverting the same pixels
// twice restores them, so a double-covered word would render un-highlighted.
//
// Word order is NOT assumed ascending by offset: TextBlock stores words in
// visual order, which runs backwards on an RTL line.
std::vector<HighlightRect> highlightRects(const std::vector<HighlightWord>& words,
                                          const std::vector<VisibleRange>& ranges, int16_t gapTolerance);
```

- [ ] **Step 5: Implement, run passing, commit**

Collect covered words (a word is covered if **any** range contains it — test membership once per word, which is what prevents duplicates), sort by `(y, x)`, then merge along each row.

---

### Task 3: Compute the overlay once and replay it

**Files:**
- Create: `src/activities/reader/HighlightOverlay.{h,cpp}`
- Modify: `src/activities/reader/EpubReaderActivity.{h,cpp}`

- [ ] **Step 1: The page walk**

`HighlightOverlay::buildRects(page, ranges, marginLeft, marginTop, lineHeight, gapTolerance) -> std::vector<HighlightRect>`.

Mirrors the collection loop at `DictionaryWordSelectActivity.cpp:74-95` — iterate `page->elements`, skip non-`TAG_PageLine`, take `line->getBlock()`, then per word:

- `x = line->xPos + block->wordXpos(i) + marginLeft`
- `y = line->yPos + marginTop + block->getRubyShift(ascender)`
- `offset = block->wordVisibleOffset(i)`
- `w = block->wordXpos(i + 1) - block->wordXpos(i)` for all but the last word on the line; the last word's right edge comes from the line's own extent

**Do not filter by `isSelectableToken`** — word-select skips punctuation because it cannot be looked up in a dictionary, but a highlight must cover it or the block will have holes. `ParsedText::pushToken` populates `wordXpos` and `wordVisibleOffset` for every token uniformly (`ParsedText.cpp:408-419`), so the positions exist.

**Do not measure through the renderer.** No `getTextAdvanceX`, no `ensureSdCardFontReady` — see Constraints.

- [ ] **Step 2: Cache per page, replay per pass**

In `renderContents`, **before** the B/W `page->render` at `:1411`, compute the rects once into a local `std::vector<HighlightRect>`. Then apply them after the B/W render only.

Do **not** apply inside `renderGrayscalePass` — Task 1's guard makes that a no-op anyway, but calling it there would still burn a walk per strip per plane.

Two call sites the overlay must **not** touch:
- `:1374` — inside the `PrewarmScope` scan; `invertRect` early-outs on `isScanning()`.
- `:313` — the idle prewarm of the **next** page, rendered at margins `0,0`. Applying this page's rects there would be actively wrong.

Also note `:1405-1407`: on pages with images, `renderWithImagePlaceholders` displays a frame before the real render, so a highlight appears one frame late there. Acceptable; record it.

- [ ] **Step 3: A temporary highlight, so this task is verifiable when it lands**

Nothing creates highlights until Task 4, and this is the riskiest task in the plan. Add a **temporary** hardcoded `VisibleRange` covering a few words on the current spine, verify it on device (Task 8 Steps 2–4 can run early against it), and delete it in Task 4. Mark it clearly:

```cpp
  // TEMPORARY (removed in Task 4): a fixed range so the overlay can be seen on
  // hardware before PassageSelectActivity exists.
```

- [ ] **Step 4: Decide the C3 memory question before loading a document**

`HighlightFile::load` holds two `JsonDocument`s live at once against a 45,000-byte budget, and a resident 400-entry doc is ~19 KB of vector storage — on a chip the repo puts at ~50 KB free during reading.

Choose and record one:
- **(a)** Gate highlights on PSRAM: no load, no menu entries, on non-PSRAM boards.
- **(b)** State a C3 budget and design to it — load only the current spine's entries rather than the whole document.

**(a) is recommended**: the X4 Pro is the target, and (b) needs a per-spine file format this plan does not have.

- [ ] **Step 5: Handle a failed load honestly**

On `LoadResult::Failed`, do **not** save for the rest of the session and tell the user — the file may still hold their data. There is no shared toast in this codebase: `GUI.drawPopup(renderer, text)` exists (used as `showBuildPopup`) and is fire-and-forget into the framebuffer. Add a small `showMessage` helper here that Tasks 4 and 5 reuse, rather than three activities each rolling their own like `DictionaryWordSelectActivity` does.

- [ ] **Step 6: Build both boards and commit**

---

### Task 4: `PassageSelectActivity`

**Files:** create `src/activities/reader/PassageSelectActivity.{h,cpp}`; modify `EpubReaderActivity.cpp` to remove Task 3's temporary range

- [ ] **Step 1: Size its own snapshot buffer**

This is where multi-line selection actually lives. `DictionaryWordSelectActivity`'s `SNAPSHOT_CAPACITY` is 4096 and sized for a **single word** (`selected` is one `int`, snapshotting ~1.7 KB) — it is not inherited and must not be raised there.

Size this class's buffer from its own worst case: `widthBytes × lineHeight × maxSelectedLines`. On the C3 a large allocation is a real risk against ~50 KB free heap, so allocate with `makeUniqueNoThrow` and keep the full-repaint fallback intact when it returns null.

- [ ] **Step 2: Two-anchor selection**

Tap or arrow to the first word and confirm; tap or arrow to the last and confirm. Selection-in-progress renders as an **outlined box**, distinct from a committed highlight's inverted block — otherwise the user cannot tell what is already saved. Confirm is a tap or the Home key; there is no physical Confirm.

- [ ] **Step 3: Build the entry**

`end` = last word's offset **+ 1**. Verified correct: `contains` tests a word's start offset, offsets are strictly increasing across tokens, and no path emits two words at the same offset.

Take offsets from `wordVisibleOffset` — never recompute them from text.

> Note for whoever closes the overlap question: because `end` is word-granular, `VisibleRange::overlaps` between two stored highlights is not meaningful. It is correct for `contains`, which is all the render path needs.

- [ ] **Step 4: Save, honouring `SaveResult`**

On `TooLarge`, tell the user via Task 3's helper rather than failing silently.

- [ ] **Step 5: Remove the temporary range from Task 3, build both boards, commit**

---

### Task 5: `TagPickerActivity` and the result type

**Files:** create `TagPickerActivity.{h,cpp}`; modify `src/activities/ActivityResult.h`

- [ ] **Step 1: Extend `ResultVariant`**

```cpp
struct TagSelectionResult {
  std::vector<uint16_t> tagIndices;
};
```

Verified safe: no exhaustive `std::visit` exists over `ResultVariant` (every consumer uses `std::get<T>`), and `ProgressChangeResult` is already larger, so the variant does not grow. Note in the commit that `ActivityResult`'s converting constructor is `requires std::is_constructible_v<...>`, so a new alternative can change how a previously-ambiguous conversion resolves.

- [ ] **Step 2: The picker**

Palette list with a check state per tag; a "New tag…" row pushes `KeyboardEntryActivity` and calls `HighlightDoc::addTag`. Handle `nullopt` — palette full, or the name empty or too long — with a visible message.

- [ ] **Step 3: Build both boards and commit**

---

### Task 6: `HighlightsActivity`

**Files:** create `HighlightsActivity.{h,cpp}`

- [ ] **Step 1: Browse and filter**

List this book's highlights by label; a tag filter narrows them. Tags are per-book by design — this screen must not imply a cross-book view.

- [ ] **Step 2: Jump by returning a `ProgressChangeResult`**

**Do not route through `progressChangeResultHandler`.** That block (`EpubReaderActivity.cpp:623-640`) is a lambda local to `onReaderMenuConfirm` that opens with `std::get<ProgressChangeResult>(result.data)`; under `-fno-exceptions` a mismatched alternative calls `std::terminate`. It also calls `loadCachedBookmarks()` and re-opens the reader menu on cancel — both wrong here.

Return a `ProgressChangeResult` with `hasVisibleTextOffset = true` and the highlight's `start`, so the existing jump path applies unchanged. Alternatively extract `:626-640` into a named member both handlers call; returning the existing type is cheaper.

- [ ] **Step 3: Delete**

`HighlightDoc::removeHighlight` then save. The only destructive action in the feature — confirm first. Any cached rects must be invalidated after a mutation.

> **Lifetime:** cache derived `HighlightRect` values, never the `const HighlightEntry*` that `findBySpine` returns. `addHighlight` is a `push_back`, so reallocation invalidates every outstanding pointer.

- [ ] **Step 4: Build both boards and commit**

---

### Task 7: Menu and long-press wiring — four sites

- [ ] **Step 1: Two menu entries**

`HIGHLIGHT_PASSAGE` and `HIGHLIGHTS` in `EpubReaderMenuActivity`. Count verified: 13 unconditional + footnotes + bookmarks + frontlight + 2 = **18**, against `MAX_MENU_ITEMS = 24`. `HighlightsActivity` sits **beside** bookmarks, not replacing it.

> `listCount()` (`EpubReaderMenuActivity.h:59`) still returns the unclamped `menuItems.size()`, but at 18 < 24 it is unreachable. Noted so it is not re-raised; no action.

- [ ] **Step 2: The long-press option — all four edit sites**

1. `src/CrossPointSettings.h:148-152` — add `LP_MENU_HIGHLIGHT` to the enum.
2. `src/SettingsList.h:181-186` — add the picker value. **Fix the positional trap first:**

```cpp
  const size_t count = BoardConfig::hasHomeKey() ? std::size(VALUES) : std::size(VALUES) - 1;
```

The index is the enum value and the non-Home-key case hides the **last** entry positionally. Appending would hide *highlight* and newly **expose** `READER_MENU` on boards deliberately denied it. Replace `- 1` with an explicit per-board filter before appending.

3. `EpubReaderActivity.cpp:409-436` — the front-Confirm switch.
4. `EpubReaderActivity.cpp:442-465` — **the Home-key switch.** The X4 Pro is a Home-key board (`BoardConfig.h:1396`: back and confirm are `PIN_UNASSIGNED`), so this is the only one that fires on the target device. Missing it means the option silently does nothing, via `default: break;` at `:462-464`.

Say plainly in the setting's description that choosing it replaces the current long-press action.

- [ ] **Step 3: Build both boards, full host suite, commit**

---

### Task 8: On-device verification — mandatory

Record results in `docs/superpowers/notes/`.

- [ ] **Step 1: Anchoring survives re-pagination**

Highlight a passage; change font size two steps each way, change margins, rotate. It must cover **the same words** every time.

- [ ] **Step 2: Anti-aliasing fringing**

With AA **on** (default), check the anti-aliased glyph edges inside a highlight. Task 1 leaves them at their original gray, so slight fringing is expected — judge whether it is noticeable at 219 PPI. Then confirm the highlight renders identically with AA **off**.

- [ ] **Step 3: Ghosting**

Turn several pages with a highlight on screen, then to one without. Look for residual shadow. Fallback is a full refresh on highlight change.

- [ ] **Step 4: Panel variant**

Record which controller the unit reports (SSD1677 / UC8179 / UC8279). The other two remain unverified.

- [ ] **Step 5: RTL**

With an RTL EPUB, confirm the highlight covers the intended words, not mirrored ones.

- [ ] **Step 6: Selection feel**

Multi-line selection with Task 4's snapshot sizing: smooth, or still stuttering? Subjective and worth recording.

- [ ] **Step 7: Durability**

Create highlights, interrupt a save if you can, confirm nothing is lost. Check `/.crosspoint/highlights/` for orphaned `.tmp` files.

- [ ] **Step 8: Record and commit**

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| AA fringing inside a highlight is ugly | Medium | Task 8 Step 2; fallback is forcing a B/W-only refresh while a highlight is on screen |
| Ghosting from inverted blocks | Medium | Task 8 Step 3 |
| C3 heap exhaustion | Medium | Task 3 Step 4 gates on PSRAM |
| Only one panel variant tested | Medium | Recorded, not claimed as general |
| Long-press option silently dead on this device | Low | Task 7 Step 2 names the Home-key switch explicitly |
| Cached rects outlive a mutation | Low | Cache values, not pointers; invalidate on change |

## Deliberately out of scope

- **Cross-book tags**, **notes**, and **on-page tag markers** — all decided against earlier.
- **Coherent AA inside highlights.** Would require rewriting both grayscale planes; a different operation.
- **Overlap policy.** The geometry de-duplicates rects so overlaps render correctly. Whether the data model should permit them is a product decision — and note that `VisibleRange::overlaps` cannot answer it for word-granular stored ranges.
