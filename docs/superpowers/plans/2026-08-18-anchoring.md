# Per-Word Visible Offsets (Anchoring) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every rendered word carry its visible-codepoint offset, so a highlight can be stored as an offset range and re-resolved to the correct words after any re-pagination.

**Architecture:** `ParsedText` already computes per-word visible offsets at layout time and throws them away. This plan widens `TextBlock`'s arena to keep them, threads them through `extractLine` (respecting bidi visual reordering), and bumps the section cache format. No highlights code — this plan only makes the data available.

**Tech Stack:** C++20, PlatformIO (`x4pro`), host CMake + GoogleTest.

**Depends on:** `docs/superpowers/plans/2026-08-18-highlights-foundations.md` (complete).

**Delivery:** fork-only. No upstream contribution, so the format bump and the RAM cost are ours to decide.

---

## Why this is needed

`ParsedText.h:39-49` is explicit that the data exists and is deliberately discarded:

```
// Zero-based visible Unicode-codepoint offsets in the spine body, stored as
// uint16_t deltas from a shared base to keep this layout-only metadata small.
// Pathological spans wider than uint16_t use sparse rebases; rendered
// TextBlocks do not carry any of this metadata.
```

Only the *line-start* offset survives, to populate the page LUT (`ParsedText.cpp:1241`). A highlight needs *word* granularity, and the offsets **cannot be re-derived** at render time: `ChapterHtmlSlimParser.cpp:1147-1165` counts codepoints of body character data *before* the skip guards, so the offset space includes collapsed whitespace and entire subtrees that layout never renders. Any independent recount diverges.

## Two findings that make this smaller than feared

**1. Serialization is free.** `TextBlock::serialize` writes the arena verbatim in one call:

```cpp
const size_t size = arenaSize(numWords, focusPresent, textBytes);
if (file.write(arena.get(), size) != size) { ... }
```

The comment above it states the intent: *"its in-memory layout is exactly the on-disk layout ... so one write covers all per-word arrays and the text blob."* Adding an array to the arena therefore serialises and deserialises with no new I/O code — only `arenaSize()` and the view binding change.

**2. The partial-version constant is derived, not manual.**

```cpp
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
```

`Section.cpp:60`, with the comment *"Derived so the pairing can't be forgotten."* Bumping `SECTION_FILE_VERSION` is a **one-constant change**. (The v2 spec implied two edits; it was wrong.)

## Encoding decision: absolute `uint32`, not `uint16` deltas

`ParsedText` stores its layout-time copy as `uint16_t` deltas from a shared base with sparse rebases, because that metadata is transient and RAM-critical on the C3.

For the persisted copy we take **absolute `uint32_t` per word** instead:

- Offsets count skipped subtrees. A nested table or `skipUntilDepth` region between two words *on the same line* can push a line-relative delta past 65535. That is rare, but it is a silent wrong-anchor bug, not a crash — the worst kind.
- Absolute offsets remove the rebase bookkeeping entirely; the render-time comparison is a plain `offset >= start && offset < end`.
- Cost is 2 extra bytes per word over the delta scheme. A page holds ~25-30 blocks of ~10 words, so ~300 words → **~1.2 KB per page** absolute vs ~600 B delta. Negligible against 8 MB PSRAM.
- We are fork-only and X4 Pro-targeted, so the C3 budget that motivated the delta scheme no longer binds us. The spec's "Delivery: fork" section explicitly permits choosing correctness first.

**Alignment constraint:** `TextBlock.h`'s arena comment warns *"2-byte alignment holds by construction: all 16-bit arrays come first and the arena base is allocator-aligned; RISC-V faults on unaligned multi-byte access."* A `uint32_t` array needs 4-byte alignment, so it must be placed **first**, ahead of the 16-bit arrays.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/blocks/TextBlock.h` / `.cpp` (modify) | Arena gains a `uint32_t visibleOffset[]` array, plus an accessor |
| `lib/Epub/Epub/ParsedText.cpp` (modify) | `extractLine` populates offsets, handling bidi reorder |
| `lib/Epub/Epub/Section.cpp` (modify) | `SECTION_FILE_VERSION` 40 → 41 + history comment |
| `test/visible_offset/` (create) | Host tests for the pure offset-selection predicate |
| `lib/Epub/Epub/VisibleRange.h` (create) | Dependency-free range predicate, host-testable |

---

### Task 1: Spike — decide whether a host shim is affordable

The v2 spec claims the critical repagination-invariance test needs a host shim for `GfxRenderer`/`Storage`. Nobody has costed that. Do so before committing to it.

**No production code changes in this task.** Output is a written decision.

- [ ] **Step 1: Map the dependency closure**

```bash
cd ~/Documents/development/personal/crosspoint-x4pro
head -20 lib/Epub/Epub/ParsedText.cpp lib/Epub/Epub/Section.cpp lib/Epub/Epub/Page.cpp
grep -rn "renderer\." lib/Epub/Epub/ParsedText.cpp | head -30
```

Identify precisely which `GfxRenderer` members `ParsedText` calls. The suspicion from earlier reading is that it is a small surface — mostly `getTextAdvanceX` and `getKerning` (font metrics), not drawing.

- [ ] **Step 2: Decide and record**

Write findings to `docs/superpowers/notes/host-shim-feasibility.md`, answering:

1. Exact list of `GfxRenderer` methods `ParsedText` needs.
2. Whether a fake renderer returning fixed-width metrics (e.g. every glyph 10px) is enough to exercise pagination. **A fixed-width fake is sufficient for repagination invariance** — the test asserts that a recorded offset range resolves to the same *words* at two different widths, which needs deterministic metrics, not real ones.
3. Whether `Storage`/`HalStorage` can be stubbed, or whether `Section`'s file I/O forces it out of scope.
4. A recommendation: **shim now**, **shim later**, or **not worth it**.

- [ ] **Step 3: Commit the note**

```bash
git add docs/superpowers/notes/host-shim-feasibility.md
git commit -m "docs: cost the host test shim for pagination logic"
```

**Escalate rather than guess** if the closure turns out to be large. This step exists to replace an assumption with a number.

---

### Task 2: `TextBlock` carries per-word visible offsets

Land the storage first, populated with zeros. The build stays green and caches rebuild once; nothing reads the values yet.

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`
- Modify: `lib/Epub/Epub/Section.cpp`

- [ ] **Step 1: Update the arena layout comment**

In `TextBlock.h`, the arena layout block currently reads:

```
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
```

Insert **above** it, and extend the alignment note:

```
//   uint32_t visibleOffset[wordCount]  visible-codepoint offset of word i in
//                                      the spine body; anchors highlights across
//                                      re-pagination. Absolute, not a delta:
//                                      offsets include content layout skips, so a
//                                      line-relative delta can exceed 16 bits.
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
```

and amend the alignment sentence to say the 32-bit array comes first, then the 16-bit arrays, then the 8-bit arrays, then text.

- [ ] **Step 2: Add the member, view, and accessor**

Add alongside the other typed views:

```cpp
  const uint32_t* visibleOffsetArr = nullptr;
```

Add a public accessor next to the other per-word accessors (`wordText`, `wordXpos`, …):

```cpp
  // Visible-codepoint offset of word i within the spine body. Absolute; comparable
  // against a stored highlight range. Returns 0 for out-of-range i.
  uint32_t wordVisibleOffset(uint16_t i) const;
```

- [ ] **Step 3: Update `arenaSize`**

```cpp
size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const uint16_t textBytes) {
  // Layout documented in TextBlock.h: the 32-bit array first, then 16-bit arrays,
  // then 8-bit arrays, then text.
  size_t size = static_cast<size_t>(wordCount) *
                (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  return size + textBytes;
}
```

- [ ] **Step 4: Bind the view and implement the accessor**

Find where the other views are bound after the arena is filled (search for `textOffArr =`). The `visibleOffsetArr` binds at arena offset 0, and every subsequent base shifts by `wordCount * sizeof(uint32_t)`. Update all of them.

```cpp
uint32_t TextBlock::wordVisibleOffset(const uint16_t i) const {
  if (!visibleOffsetArr || i >= numWords) return 0;
  return visibleOffsetArr[i];
}
```

- [ ] **Step 5: Zero-fill at construction**

Wherever the arena is populated, write `0` into every `visibleOffset[i]` for now. Task 3 replaces this with real values. Do not leave the array uninitialised — a stale-heap value would look like a plausible offset.

- [ ] **Step 6: Bump the section format**

In `Section.cpp`, above `constexpr uint8_t SECTION_FILE_VERSION`, add a history line matching the existing style:

```cpp
// v41: TextBlock's arena carries a per-word visible-codepoint offset array, used
//      to anchor highlights across re-pagination. Older caches have no such array
//      and would be misread as having one.
```

and change the constant to `41`. **Do not touch `SECTION_FILE_PARTIAL_VERSION`** — it is derived and updates itself.

- [ ] **Step 7: Verify**

```bash
pio run -e x4pro
cmake --build build/test && ctest --test-dir build/test -j
```

Expected: firmware `SUCCESS`; host suite still 152/152 (nothing here is host-testable yet). Record the flash/RAM figures — flash was 84.6% before this plan.

- [ ] **Step 8: Commit**

```bash
git add lib/Epub/Epub/blocks/TextBlock.h lib/Epub/Epub/blocks/TextBlock.cpp lib/Epub/Epub/Section.cpp
git commit -m "feat(epub): reserve per-word visible offsets in the TextBlock arena

Widens the arena with a uint32 visible-codepoint offset per word and
bumps SECTION_FILE_VERSION to 41. Values are zero for now; ParsedText
populates them next. Absolute rather than delta-encoded because offsets
include layout-skipped content, so a line-relative delta can exceed 16
bits."
```

---

### Task 3: `ParsedText` populates the offsets

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`extractLine`, around lines 1233-1400)

`ParsedText::visibleOffsetAt(wordIndex)` already returns the absolute offset for a **logical** word index. The line being extracted starts at logical index `lastBreakAt`, so word `i` of the line is `visibleOffsetAt(lastBreakAt + i)`.

**The bidi trap:** when `willReorder` is true, the block is built from `reorderedWordsScratch` in **visual** order. Offsets must follow the same permutation, or an RTL line anchors every highlight to the wrong words. The existing code already shows the correct pattern for exactly this — note how widths are handled:

```cpp
const uint16_t src = visualOrderScratch[i];
reorderedWordsScratch.push_back(std::move(lineWords[src]));
reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
```

- [ ] **Step 1: Add the scratch vector**

Beside the other `reordered*Scratch` members in `ParsedText.h`:

```cpp
  std::vector<uint32_t> reorderedVisibleOffsetsScratch;
```

- [ ] **Step 2: Populate it in the reorder path**

Inside the `if (willReorder)` block, alongside the existing `reserve` calls:

```cpp
    reorderedVisibleOffsetsScratch.clear();
    reorderedVisibleOffsetsScratch.reserve(visualOrderScratch.size());
```

and inside the loop, next to the width push:

```cpp
      reorderedVisibleOffsetsScratch.push_back(visibleOffsetAt(lastBreakAt + src));
```

- [ ] **Step 3: Populate the non-reorder path**

In the branch where `willReorder` is false, build the same vector in logical order:

```cpp
    reorderedVisibleOffsetsScratch.clear();
    reorderedVisibleOffsetsScratch.reserve(lineWordCount);
    for (size_t i = 0; i < lineWordCount; ++i) {
      reorderedVisibleOffsetsScratch.push_back(visibleOffsetAt(lastBreakAt + i));
    }
```

Read the surrounding code first: if the non-reorder path passes `lineWords` directly rather than a reordered copy, mirror whatever mechanism it uses to hand per-word arrays to the `TextBlock` builder. **If the two paths differ structurally, report it before implementing** rather than forcing a shape that does not fit.

- [ ] **Step 4: Pass the offsets into the block**

Thread `reorderedVisibleOffsetsScratch` into whichever function fills the arena (the same one that receives styles, widths, and focus boundaries), and write each value into `visibleOffset[i]` in place of Task 2's zero fill.

- [ ] **Step 5: Verify**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`.

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp
git commit -m "feat(epub): populate per-word visible offsets, bidi-aware

Offsets follow the same visual permutation as words, styles and widths,
so an RTL line anchors to the correct words rather than mirrored ones."
```

---

### Task 4: Host-testable range predicate

The comparison a highlight renderer performs is pure logic and belongs in its own dependency-free header, so it can be tested on the host regardless of Task 1's shim decision.

**Files:**
- Create: `lib/Epub/Epub/VisibleRange.h`
- Create: `test/visible_range/VisibleRangeTest.cpp`
- Create: `test/visible_range/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
// lib/Epub/Epub/VisibleRange.h
#pragma once

#include <cstdint>

// A half-open range of visible-codepoint offsets within one spine item, as stored
// by a highlight. Dependency-free so the selection rule can be unit tested on the
// host, away from the renderer and storage layers.
struct VisibleRange {
  uint32_t start = 0;
  uint32_t end = 0;  // exclusive

  constexpr bool isEmpty() const { return end <= start; }

  // True when a word at `offset` falls inside the range. Half-open so two
  // adjacent highlights cannot both claim the same word.
  constexpr bool contains(const uint32_t offset) const { return offset >= start && offset < end; }

  // True when this range and `other` share at least one offset.
  constexpr bool overlaps(const VisibleRange& other) const {
    return !isEmpty() && !other.isEmpty() && start < other.end && other.start < end;
  }
};
```

- [ ] **Step 2: Write the failing test**

```cpp
// test/visible_range/VisibleRangeTest.cpp
#include <gtest/gtest.h>

#include "Epub/VisibleRange.h"

TEST(VisibleRange, ContainsIsHalfOpen) {
  constexpr VisibleRange r{100, 200};
  EXPECT_TRUE(r.contains(100)) << "start is inclusive";
  EXPECT_TRUE(r.contains(199));
  EXPECT_FALSE(r.contains(200)) << "end is exclusive";
  EXPECT_FALSE(r.contains(99));
}

TEST(VisibleRange, AdjacentRangesDoNotShareAWord) {
  constexpr VisibleRange left{100, 200};
  constexpr VisibleRange right{200, 300};
  EXPECT_FALSE(left.contains(200));
  EXPECT_TRUE(right.contains(200));
  EXPECT_FALSE(left.overlaps(right)) << "touching at a boundary is not overlapping";
}

TEST(VisibleRange, EmptyRangeContainsNothing) {
  constexpr VisibleRange empty{150, 150};
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_FALSE(empty.contains(150));
  EXPECT_FALSE(empty.overlaps(VisibleRange{100, 200}));
}

TEST(VisibleRange, InvertedRangeIsTreatedAsEmpty) {
  constexpr VisibleRange inverted{300, 100};
  EXPECT_TRUE(inverted.isEmpty());
  EXPECT_FALSE(inverted.contains(200)) << "a reversed range must not match the span between its ends";
}

TEST(VisibleRange, DetectsGenuineOverlap) {
  constexpr VisibleRange a{100, 200};
  EXPECT_TRUE(a.overlaps(VisibleRange{150, 250}));
  EXPECT_TRUE(a.overlaps(VisibleRange{50, 150}));
  EXPECT_TRUE(a.overlaps(VisibleRange{120, 130})) << "fully contained";
  EXPECT_TRUE(a.overlaps(VisibleRange{50, 250})) << "fully containing";
  EXPECT_FALSE(a.overlaps(VisibleRange{200, 300}));
}

TEST(VisibleRange, HandlesOffsetsBeyond16Bits) {
  // Offsets count layout-skipped content, so they routinely exceed 65535 in a
  // long chapter. This is why the stored form is uint32, not a uint16 delta.
  constexpr VisibleRange r{70000, 70010};
  EXPECT_TRUE(r.contains(70005));
  EXPECT_FALSE(r.contains(65535));
}
```

- [ ] **Step 3: Register the suite**

```cmake
# test/visible_range/CMakeLists.txt
add_executable(VisibleRangeTest
  VisibleRangeTest.cpp
)

target_include_directories(VisibleRangeTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(VisibleRangeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VisibleRangeTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(visible_range)
```

- [ ] **Step 4: Run to verify it fails**

```bash
cmake -S test -B build/test
cmake --build build/test --target VisibleRangeTest
```

Expected: FAIL — `Epub/VisibleRange.h` not found until Step 1's file exists.

- [ ] **Step 5: Run to verify it passes**

```bash
cmake --build build/test --target VisibleRangeTest
ctest --test-dir build/test --output-on-failure -R VisibleRange
```

Expected: PASS, 6 tests.

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/VisibleRange.h test/visible_range test/CMakeLists.txt
git commit -m "feat(epub): add host-tested visible-offset range predicate"
```

---

### Task 5: Verification and cache-invalidation check

- [ ] **Step 1: Full suite and firmware**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
```

Expected: 158/158 host tests (152 + 6 new); firmware `SUCCESS`.

- [ ] **Step 2: Record the cost**

Note flash and RAM against the 84.6% / 19.7% baseline from the foundations branch. The arena grew by 4 bytes per word, so a RAM increase during reading is expected and correct — confirm it is proportionate, not alarming.

- [ ] **Step 3: On-device sanity check (required — this is the first testable behaviour)**

Flash the build and confirm:

1. An existing book **re-paginates once** on first open (the v41 bump invalidated its cache) and then opens instantly on subsequent loads.
2. Page navigation, chapter jumps, and go-to-percent still land correctly — the page LUT is built from the same offset machinery.
3. No visible regression in justified or RTL text.

Record the results. This is the first point at which the format change is observable, and the cache invalidation is the user-visible cost of the whole plan.

- [ ] **Step 4: Commit any notes**

```bash
git add docs/
git commit -m "docs: record anchoring verification results"
```

---

## What this plan deliberately does not do

- **No highlights.** No file format, activity, or render pass. That is the feature plan.
- **No reading of the offsets.** `wordVisibleOffset` and `VisibleRange` are added and tested but unused in production — the same deliberate dead surface pattern as the foundations branch. The feature plan consumes both.
- **No host shim.** Task 1 decides whether to build one; it does not build it.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Bidi permutation applied to words but not offsets | High | Task 3 mirrors the existing width-reorder line exactly; RTL is the review focus |
| Arena alignment broken by the uint32 array | High | Placed first, ahead of the 16-bit arrays; RISC-V faults on misalignment so a mistake shows immediately |
| Cache invalidation surprises the user | Medium | Expected and one-time; verified in Task 5 Step 3 |
| Offsets wrong in ways only real EPUBs show | Medium | Task 1 decides whether a host shim can catch this before hardware |
| Flash headroom (84.6% before this plan) | Medium | Measure each task; the arena change is RAM, not flash |
