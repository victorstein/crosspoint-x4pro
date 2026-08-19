# Per-Word Visible Offsets (Anchoring) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every rendered word carry its visible-codepoint offset, so a highlight can be stored as an offset range and re-resolved to the correct words after any re-pagination.

**Architecture:** `ParsedText` already computes per-word visible offsets at layout time and throws them away. This plan widens `TextBlock`'s arena to keep them, threads them through `extractLine` (respecting bidi visual reordering), and bumps the section cache format. No highlights code — this plan only makes the data available.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest.

**Depends on:** `docs/superpowers/plans/2026-08-18-highlights-foundations.md` (complete).

**Delivery:** fork-only.

> **v2 — revised after adversarial review.** The first draft justified its encoding with a
> scenario that cannot occur, hand-waved the one function that actually has to change,
> and contained a cache-corruption window between two tasks. All corrected below. Claims
> are cited to `file:line`; where a claim could not be verified it is marked as such.

---

## Why this is needed

`ParsedText.h:39-49` states the data exists and is deliberately discarded: *"rendered TextBlocks do not carry any of this metadata."* Only the line-start offset survives, into the page LUT (`ParsedText.cpp:1241`).

Offsets **cannot be re-derived** at render time. `ChapterHtmlSlimParser.cpp:1147-1165` counts codepoints of body character data *before* the skip guards, so the offset space includes collapsed whitespace and some content layout never renders. Any independent recount diverges.

## What review confirmed (do not re-litigate)

- **Serialization is free.** `TextBlock::serialize` (`TextBlock.cpp:313-319`) writes the arena as one blob sized by `arenaSize()`; `deserialize` (`:345-384`) recomputes that size from the file-read scalars and does one bulk read, then `bindArenaPointers()`. Growing the arena propagates automatically. `arenaSize` has exactly three callers, all in `TextBlock.cpp` — no dumper, estimator, or test depends on the layout. `Page`/`Section` carry no block length prefixes.
- **The partial-version constant is derived.** `SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28)` (`Section.cpp:60`). Bumping is a one-constant change, and **no other cache version needs touching** — `CSS_CACHE_VERSION`, `BOOK_CACHE_VERSION`, the TXT reader's `CACHE_VERSION`, `CPFONT_VERSION` and `SIDECAR_VERSION` are all unrelated. Existing v40 partials carry `0xF2`, which matches neither new constant, so they are rejected and rebuilt.
- **Arena base alignment is real, not luck.** `makeUniqueNoThrow<uint8_t[]>` is `new (std::nothrow) uint8_t[n]()` (`Memory.h:33`), which must return storage aligned for any fundamental type; `uint8_t` is trivially destructible so no array cookie shifts the pointer. No overridden `operator new` exists in the tree.
- **The bidi indexing is correct.** `lastBreakAt` (`ParsedText.cpp:1239`) is a logical index into `words`; `visualOrderScratch[i]` is **line-relative** (`BidiUtils.cpp:199,286`), so `lastBreakAt + src` is right and does not double-count. `wordWidths[lastBreakAt + src]` (`:1338`) is a true parallel. Offset lockstep holds through every push, hyphenation insert, and prefix erase.

## Encoding decision: absolute `uint32`

We store an absolute `uint32_t` offset per word.

**The reason is simplicity, not overflow.** An earlier draft claimed a line-relative `uint16` delta could exceed 65535 because a nested table between two words inflates offsets. **That is unreachable:** `<table>` flattens into per-cell paragraphs (`ParsedText.cpp:535`, `startNewTextBlock(tableCellBlockStyle)`), so words either side of a nested table land in different `ParsedText` objects and can never share a line. The only genuinely inline counted-but-unrendered content is `display:none` (`ChapterHtmlSlimParser.cpp:491-496`) and `doc-pagebreak` labels (`:910-918`); overflowing a delta would need a hidden span of >65535 codepoints inside one rendered line.

The real argument: **`TextBlock` stores no base offset.** The line-start offset goes to the *page* LUT via `processLine`, never into the block. A delta scheme would therefore have to add a per-block `uint32` base to the arena anyway. For a 10-word line that is 24 B (base + 10×2) versus 40 B absolute — about 16 B per line, ~400 B per page — in exchange for rebase bookkeeping and a second failure mode. Not worth it.

**Alignment:** a `uint32_t` array needs 4-byte alignment, so it goes **first**, ahead of the 16-bit arrays. Note the target is **ESP32-S3, which is Xtensa LX7, not RISC-V** — the existing `TextBlock.h:19-21` comment was written for the C3. Xtensa also traps unaligned 32-bit access, so the mitigation stands; do not copy the RISC-V wording forward.

## Stated limit: NFC drift

`ParsedText::addWord` NFC-composes a word (`ParsedText.cpp:399`) *before* deriving sub-token offsets (`:467`, `:512-532`), while `visibleTextOffset` was counted on the raw parse stream. For NFD source text — the comment at `:393-398` names Vietnamese and NFD headings as real cases — a word's stored offset drifts from the true spine-body offset by the marks composed away earlier in that token.

This does **not** break re-pagination invariance: the drift is deterministic and identical at every width. But offsets are not exact spine-body codepoint offsets, and the consequence is a hard rule for the feature plan: **a highlight range must be recorded from these same stored offsets and never recomputed from source text.**

## Costs

- **RAM:** +4 B/word. ~10 words/line × ~28 lines ≈ 280 words/page → **~1.1 KB/page**. At most two pages are resident (current + prefetch, `EpubReaderActivity.cpp:277,309`; `loadPageAt` returns a `unique_ptr` with no page cache), so **~2.2 KB peak**.
- **SD growth — the larger cost, and previously unstated.** Per-word arena is currently 5 B (`2+2+1`) plus NUL-terminated text. For Latin prose (~6 B/word incl. NUL) that is ~11 B/word, so +4 B is **~+36%** on the arena portion of every section `.bin`. CJK is worse: `cjkCharacterBreakByteOffsets` (`ParsedText.cpp:455`) tokenizes per ideograph, giving 3 text bytes + NUL + 5 = 9 B/token → **~+44%**, and a 500k-character CJK novel gains roughly **2 MB** of cache. There is no cache eviction or size cap (`Section.cpp:73`). Acceptable on an SD card, but it must be measured, not assumed.

## Open decision: the C3 build targets

`platformio.ini:2` sets `default_envs = default`, and `[base]` at `:11` sets `board = esp32-c3-devkitm-1`. **Four ESP32-C3 envs remain in the tree** — `default`, `gh_release`, `gh_release_rc`, `slim` — and the default build is one of them.

> Earlier review claimed seven C3 envs, counting the three `sticky` targets. That is wrong:
> `sticky`, `sticky-gh_release` and `sticky-gh_release_rc` each override
> `board = esp32-s3-devkitc1-n16r8`, so they are S3. Verified per-env rather than inferred
> from `[base]`.

A +36–44% arena growth lands hardest on exactly the platform the delta scheme was designed for. This plan therefore verifies **both** `-e x4pro` and `-e default` at every build step. If this fork intends to abandon the C3, that decision should be recorded and the envs removed — but until it is, do not assume the C3 budget is irrelevant.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/blocks/TextBlock.h` / `.cpp` (modify) | Arena gains `uint32_t visibleOffset[]`; constructor takes it; accessor exposes it |
| `lib/Epub/Epub/ParsedText.cpp` (modify) | `extractLine` supplies offsets, bidi-aware |
| `lib/Epub/Epub/Section.cpp` (modify) | `SECTION_FILE_VERSION` 40 → 41 → 42, one bump per behaviour change |
| `lib/Epub/Epub/VisibleRange.h` (create) | Dependency-free range predicate |
| `test/visible_range/` (create) | Host tests for the predicate |
| `docs/superpowers/notes/host-shim-feasibility.md` (create) | Task 1 output |

---

### Task 1: Spike — cost the host shim honestly

The spec asserts the repagination-invariance test needs a host shim for `GfxRenderer`/`Storage`. Nobody has costed it. **No production code in this task.** Output is a written decision.

Two facts review already established — start from them, do not re-derive:

- `ParsedText` calls **six** renderer methods, not two: `getKerning` (8×), `getTextAdvanceX` (7×), `getSpaceAdvance` (7×), `getSpaceWidth` (2×), `isSdCardFont` (1×), `ensureSdCardFontReady` (1×).
- **The method count is not the problem.** `GfxRenderer` has **no virtual functions** — there is no seam to inject a fake. A shim needs either a link-time substitute `GfxRenderer.cpp` or templating `ParsedText` on the renderer. And `GfxRenderer.h` includes `HalDisplay.h`, which includes `<Arduino.h>`, so merely *compiling* `ParsedText.cpp` on the host drags in the Arduino core.

- [ ] **Step 1: Measure the two candidate approaches**

For each of (a) link-time substitute `GfxRenderer.cpp` compiled only into the test target, and (b) templating `ParsedText` on a renderer concept, determine: how many files change, whether `Arduino.h` can be kept out of the host translation units, and what `Storage`/`HalStorage` still forces in via `Section.cpp`.

- [ ] **Step 2: Record the decision**

Write `docs/superpowers/notes/host-shim-feasibility.md` covering: the closure for each approach, which (if either) keeps Arduino out, an honest effort estimate, and a recommendation of **shim now / shim later / not worth it**.

Do **not** pre-suppose the answer. If both approaches are large, "not worth it — verify anchoring on device instead" is a legitimate and probably correct outcome.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/notes/host-shim-feasibility.md
git commit -m "docs: cost the host test shim for pagination logic"
```

---

### Task 2: `TextBlock` carries per-word visible offsets

Land the storage and the constructor change together, populated with zeros. Bump to **41**.

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`
- Modify: `lib/Epub/Epub/ParsedText.cpp` (call sites only)
- Modify: `lib/Epub/Epub/Section.cpp`

- [ ] **Step 1: Update the arena layout comment**

In `TextBlock.h`, insert above the `textOff` line and amend the alignment note to say the 32-bit array comes first, then 16-bit, then 8-bit, then text. Replace the RISC-V wording — the S3 is Xtensa LX7.

```
//   uint32_t visibleOffset[wordCount]  visible-codepoint offset of word i in the
//                                      spine body; anchors highlights across
//                                      re-pagination. First in the arena because
//                                      it needs 4-byte alignment (Xtensa traps
//                                      unaligned 32-bit access).
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
```

- [ ] **Step 2: Extend the constructor**

`TextBlock.h:67-70`. Add the new vector as a **required** parameter, before the defaulted ones:

```cpp
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const std::vector<uint32_t>& visibleOffsets,
                     const BlockStyle& blockStyle = BlockStyle(), std::vector<std::string> rubyTexts = {});
```

Add the view member beside the others:

```cpp
  const uint32_t* visibleOffsetArr = nullptr;
```

and the public accessor beside `wordText`/`wordXpos`:

```cpp
  // Visible-codepoint offset of word i within the spine body. Absolute, comparable
  // against a stored highlight range. Returns 0 for out-of-range i.
  uint32_t wordVisibleOffset(uint16_t i) const;
```

- [ ] **Step 3: Extend the size guard**

`TextBlock.cpp:57-58`. This check is what stands between a caller bug and an out-of-bounds arena write, so the new vector **must** be in it:

```cpp
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != visibleOffsets.size() || words.size() > 10000 ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusSuffixX.size()))) {
```

Add `visibleOffsets.size()` to the `LOG_ERR` argument list too.

- [ ] **Step 4: Update `arenaSize`**

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

- [ ] **Step 5: Update `bindArenaPointers`**

`TextBlock.cpp:22-39` uses hard-coded byte multipliers; **every one shifts**:

```cpp
void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  visibleOffsetArr = reinterpret_cast<const uint32_t*>(base);
  textOffArr = reinterpret_cast<const uint16_t*>(base + wc * 4);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 6);
  size_t off = wc * 8;
  if (focusPresent) {
    focusSuffixXArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}
```

- [ ] **Step 6: Copy the offsets into the arena and implement the accessor**

In the constructor's arena-fill (`TextBlock.cpp:41-120`), write `visibleOffsets[i]` into the `uint32_t` region alongside the existing per-word copies.

> The arena is already zero-filled — `makeUniqueNoThrow` value-initializes (`Memory.h:33`,
> note the trailing `()`), so an explicit zero pass is unnecessary. Task 2 passes a
> zero-filled vector from the call sites instead, which the size guard then validates.

```cpp
uint32_t TextBlock::wordVisibleOffset(const uint16_t i) const {
  if (!visibleOffsetArr || i >= numWords) return 0;
  return visibleOffsetArr[i];
}
```

- [ ] **Step 7: Update both call sites with a zero vector**

`ParsedText.cpp:1549-1550` (no-focus fast path) and `:1572-1573` (focus path) construct `TextBlock` positionally. Pass a correctly-sized zero-filled `std::vector<uint32_t>` at the new position in both. Task 3 replaces it with real values.

- [ ] **Step 8: Bump the section format to 41**

In `Section.cpp`, above the constant, add a history line in the existing style, then set it to `41`. Leave `SECTION_FILE_PARTIAL_VERSION` alone — it is derived.

```cpp
// v41: TextBlock's arena carries a per-word visible-codepoint offset array. The
//      array is present but zero-valued at this version; v42 populates it.
```

- [ ] **Step 9: Verify both boards**

```bash
pio run -e x4pro
pio run -e default
cmake --build build/test && ctest --test-dir build/test -j
```

Expected: both firmware builds `SUCCESS`; host suite unchanged at 152/152. Record flash and RAM for **both** envs — the C3 (`default`) is the one at risk from arena growth.

- [ ] **Step 10: Commit**

```bash
git add lib/Epub/Epub/blocks/TextBlock.h lib/Epub/Epub/blocks/TextBlock.cpp lib/Epub/Epub/ParsedText.cpp lib/Epub/Epub/Section.cpp
git commit -m "feat(epub): reserve per-word visible offsets in the TextBlock arena

Widens the arena with a uint32 offset per word, extends the constructor
and its size guard, and bumps SECTION_FILE_VERSION to 41. Values are
zero at this version; v42 populates them. Absolute rather than delta
encoded because TextBlock stores no base offset, so a delta scheme would
have to add one anyway."
```

---

### Task 3: `ParsedText` populates the offsets

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp`
- Modify: `lib/Epub/Epub/Section.cpp`

`visibleOffsetAt(wordIndex)` returns the absolute offset for a **logical** index. Line word `i` is logical `lastBreakAt + i`.

**The bidi trap:** when `willReorder` is true the block is built in **visual** order, so offsets must follow the same permutation or every RTL line anchors to mirrored words.

**Do not add a scratch vector.** When `willReorder` is false there is no parallel array construction at all — `lineWords` is used directly (`ParsedText.cpp:1259-1266`), and the `else` branch at `:1445-1531` only computes `lineXPos`. The codebase already has the idiom for exactly this case, immediately below the branch at `:1533-1535`:

```cpp
const auto focusBoundaryAt = [&](const size_t idx) {
  return willReorder ? reorderedFocusBoundaryScratch[idx] : wordFocusBoundary[lastBreakAt + idx];
};
```

- [ ] **Step 1: Add the parallel lambda**

Beside `focusBoundaryAt`:

```cpp
const auto visibleOffsetForLineWord = [&](const size_t idx) {
  return visibleOffsetAt(lastBreakAt + (willReorder ? visualOrderScratch[idx] : idx));
};
```

`visualOrderScratch` is still live here — the swap at `:1443-1444` moves `reorderedWordsScratch` into `lineWords` but leaves `visualOrderScratch` untouched.

- [ ] **Step 2: Build the vector at each construction site**

Immediately before each `make_shared<TextBlock>` (`:1549-1550` and `:1572-1573`), replace Task 2's zero vector with real values:

```cpp
std::vector<uint32_t> lineVisibleOffsets;
lineVisibleOffsets.reserve(lineWordCount);
for (size_t i = 0; i < lineWordCount; ++i) {
  lineVisibleOffsets.push_back(visibleOffsetForLineWord(i));
}
```

`lineWordCount` is the right bound: `BidiUtils::computeVisualWordOrder` returns false unless `visualOrder.size() == nWords` (`BidiUtils.cpp:290-293`), so when `willReorder` is true the two are equal.

- [ ] **Step 3: Bump the section format to 42**

This is a **separate behaviour change** and needs its own version, or a v41 cache full of zeros is indistinguishable from a populated one and silently anchors every highlight to chapter start.

```cpp
// v42: the per-word visible-offset array introduced in v41 is now populated.
//      A v41 cache carries the array but all zeros, which would resolve every
//      highlight to the chapter start.
```

Set `SECTION_FILE_VERSION = 42`.

- [ ] **Step 4: Verify both boards**

```bash
pio run -e x4pro
pio run -e default
```

Expected: both `SUCCESS`.

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/ParsedText.cpp lib/Epub/Epub/Section.cpp
git commit -m "feat(epub): populate per-word visible offsets, bidi-aware

Offsets follow the same visual permutation as words and styles via a
lambda matching the existing focusBoundaryAt idiom, so an RTL line
anchors to the correct words. Bumps SECTION_FILE_VERSION to 42 so a
v41 cache of zero offsets is never mistaken for a populated one."
```

---

### Task 4: Host-testable range predicate

The comparison a highlight renderer performs is pure logic and belongs in its own dependency-free header, testable regardless of Task 1's outcome.

**Files:**
- Create: `test/visible_range/VisibleRangeTest.cpp`
- Create: `test/visible_range/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Create: `lib/Epub/Epub/VisibleRange.h`

- [ ] **Step 1: Write the failing test first**

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
  // Absolute spine-body offsets routinely exceed 65535 in a long chapter, which
  // is why the stored form is 32-bit.
  constexpr VisibleRange r{70000, 70010};
  EXPECT_TRUE(r.contains(70005));
  EXPECT_FALSE(r.contains(65535));
}
```

- [ ] **Step 2: Register the suite**

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

- [ ] **Step 3: Run to verify it fails**

```bash
cmake -S test -B build/test
cmake --build build/test --target VisibleRangeTest
```

Expected: FAIL with `Epub/VisibleRange.h` not found. The header does not exist yet — that is the red state.

- [ ] **Step 4: Write the header**

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

  constexpr bool overlaps(const VisibleRange& other) const {
    return !isEmpty() && !other.isEmpty() && start < other.end && other.start < end;
  }
};
```

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

- [ ] **Step 1: Full suite and both firmware targets**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

Expected: 158/158 host tests (152 + 6); both firmware builds `SUCCESS`.

- [ ] **Step 2: Record the cost**

Report flash and RAM for both envs. Baselines measured on the foundations branch in this session were `x4pro` flash 84.6% / RAM 19.7%; no `default` baseline was recorded, so capture one before Task 2 if you want a delta.

Also measure **SD growth**: build the section cache for one Latin book and one CJK book before and after, and record the actual percentage against the ~36% / ~44% estimates above.

- [ ] **Step 3: On-device check (required — first observable behaviour)**

Flash `x4pro` and confirm:

1. An existing book **re-paginates once** on first open (the version bump invalidated its cache), then opens instantly thereafter.
2. Page navigation, chapter jumps, and go-to-percent still land correctly — the page LUT is built from the same offset machinery.
3. No regression in justified text, and none in RTL if you have an RTL EPUB. RTL is where a permutation bug would show.

- [ ] **Step 4: Commit notes**

```bash
git add docs/
git commit -m "docs: record anchoring verification results"
```

---

## What this plan deliberately does not do

- **No highlights.** No file format, activity, or render pass.
- **No reading of the offsets.** `wordVisibleOffset` and `VisibleRange` are added and tested but unused in production, the same deliberate dead surface as the foundations branch.
- **No host shim.** Task 1 decides whether one is affordable; it does not build one.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Bidi permutation applied to words but not offsets | High | Task 3 mirrors the existing `focusBoundaryAt` idiom; RTL is the review focus |
| A v41 cache of zero offsets read as populated | High | Separate bumps: 41 in Task 2, 42 in Task 3 |
| Constructor size guard not extended | High | Task 2 Step 3; without it a length bug writes past the arena |
| `bindArenaPointers` multipliers missed | High | Every one shifts; Task 2 Step 5 gives the full function |
| Arena growth regresses the C3 | Medium | Both envs built at every step; C3 decision recorded as open |
| SD cache growth on long CJK books (~2 MB) | Medium | Measured in Task 5 Step 2 rather than assumed |
| NFC drift makes offsets inexact for NFD text | Medium | Documented above; ranges must be recorded from stored offsets, never recomputed |
| Cache invalidation surprises the user | Low | One-time, expected, verified in Task 5 Step 3 |
