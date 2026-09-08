# Cross-page Selection, Reference Labels, and Tag Commit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a selection cross a page boundary, label highlights with their Bible reference, and stop the tag picker discarding tags on the exit it advertises.

**Architecture:** A new pure `VerseAnchors` component in `lib/Epub` scans a spine item with the repo's own expat and maps a visible-codepoint offset to a `chapter:verse` reference; host tests compile that same expat so the scan is tokenised identically to the firmware. `PassageSelectActivity` switches its first anchor from a page-local index to an absolute offset, which is what makes forward page turns cost nothing per page. `TagPickerActivity` gains an explicit commit row.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest, vendored expat.

**Depends on:** `docs/superpowers/specs/2026-09-08-selection-and-labels-design.md`. Baseline **224 host tests** at `f6516add`.

**Delivery:** fork-only.

> **Verified before writing this plan** (do not re-litigate):
> - `lib/expat` vendors sources (`xmlparse.c`, `xmlrole.c`, `xmltok.c`) and all three compile on the host with `-DXML_GE=0 -DXML_CONTEXT_BYTES=1024`. `xmltok_impl.c` and `xmltok_ns.c` are `#include`d by `xmltok.c`, not compiled separately.
> - `Epub::getSpineItem(i).tocIndex` gives the covering TOC entry; `Epub::getTocItem(t).title` is the book name. No book table is needed.
> - `Epub::readItemContentsToBytes(href, &size, trailingNullByte)` returns the spine item's bytes.
> - `EpubReaderActivity::openHighlightPassage` (`:328-353`) holds a live `section` and does **not** reset it, so passing `Section&` is safe.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/VerseAnchors.h` / `.cpp` (create) | Pure: scan XHTML for anchor offsets; resolve an offset to `chapter:verse` |
| `test/verse_anchors/VerseAnchorsTest.cpp` + `CMakeLists.txt` (create) | Host tests, compiling the repo's expat |
| `test/CMakeLists.txt` (modify) | Register the suite |
| `src/activities/reader/PassageSelectActivity.{h,cpp}` (modify) | Absolute anchor, forward page turn, reference label |
| `src/activities/reader/EpubReaderActivity.cpp` (modify) | Pass `Section&` and `Epub&` to the selector |
| `src/activities/reader/TagPickerActivity.{h,cpp}` (modify) | "Done" row |

---

### Task 1: `VerseAnchors` — the pure core

**Files:** create `lib/Epub/Epub/VerseAnchors.h`, `lib/Epub/Epub/VerseAnchors.cpp`, `test/verse_anchors/VerseAnchorsTest.cpp`, `test/verse_anchors/CMakeLists.txt`; modify `test/CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Maps visible-codepoint offsets inside one spine item to Bible verse
// references, for labelling a highlight.
//
// The scan reproduces ChapterHtmlSlimParser::characterData
// (ChapterHtmlSlimParser.cpp:1147-1158): codepoints are counted while
// insideBody && nonVisibleTextDepth == 0. Parser-injected heading and image
// alt text is marked synthetic there and never appears in the source stream,
// so it needs no handling here. It uses the same expat build as the firmware,
// so tokenisation cannot drift.
namespace VerseAnchors {

struct VerseAnchor {
  uint32_t offset;   // visible codepoints from the start of the spine item
  uint16_t chapter;
  uint16_t verse;
};

// Anchors with ids shaped `chapter<N>_verse<M>`, in ascending offset order.
// Other ids are ignored. Returns empty for a document with no such anchors.
std::vector<VerseAnchor> scan(const char* xhtml, size_t length);

// The anchor covering `offset` (greatest offset <= it), or nullptr.
const VerseAnchor* find(const std::vector<VerseAnchor>& anchors, uint32_t offset);

// "11:19", or empty when `anchor` is null.
std::string format(const VerseAnchor* anchor);

}  // namespace VerseAnchors
```

- [ ] **Step 2: Write the failing tests**

`test/verse_anchors/VerseAnchorsTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "VerseAnchors.h"

namespace {
// Mirrors the shape the real book uses: empty marker spans followed by the
// verse text as siblings, two verses sharing one <p>.
const char* kDoc =
    "<html><head><title>skipme</title></head><body>"
    "<p id=\"p1\">"
    "<span id=\"chapter11_verse18\"></span><strong><sup>18</sup></strong> abcde"
    "<span id=\"chapter11_verse19\"></span><strong><sup>19</sup></strong> fghij"
    "</p></body></html>";
}  // namespace

TEST(VerseAnchorsScan, FindsEachVerseMarkerInOffsetOrder) {
  const auto a = VerseAnchors::scan(kDoc, strlen(kDoc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[0].chapter, 11); EXPECT_EQ(a[0].verse, 18);
  EXPECT_EQ(a[1].chapter, 11); EXPECT_EQ(a[1].verse, 19);
  EXPECT_LT(a[0].offset, a[1].offset);
}

TEST(VerseAnchorsScan, DoesNotCountNonVisibleText) {
  // "skipme" sits in <title>; counting it would push verse 18 to offset 6.
  const auto a = VerseAnchors::scan(kDoc, strlen(kDoc));
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a[0].offset, 0u);
}

TEST(VerseAnchorsScan, CountsCodepointsNotBytes) {
  const char* doc =
      "<html><body><p><span id=\"chapter1_verse1\"></span>\xc3\xa9\xc3\xa9"
      "<span id=\"chapter1_verse2\"></span>x</p></body></html>";
  const auto a = VerseAnchors::scan(doc, strlen(doc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[1].offset, 2u) << "two 2-byte codepoints must advance by 2, not 4";
}

TEST(VerseAnchorsScan, IgnoresIdsThatAreNotVerseMarkers) {
  const char* doc =
      "<html><body><p id=\"p188\"><span id=\"pos107452\"></span>"
      "<span id=\"footnotesource14\"></span>abc</p></body></html>";
  EXPECT_TRUE(VerseAnchors::scan(doc, strlen(doc)).empty());
}

TEST(VerseAnchorsFind, ReturnsTheAnchorCoveringAnOffset) {
  const auto a = VerseAnchors::scan(kDoc, strlen(kDoc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(VerseAnchors::find(a, a[1].offset + 2)->verse, 19);
  EXPECT_EQ(VerseAnchors::find(a, a[0].offset)->verse, 18) << "exactly on a marker is inside it";
}

TEST(VerseAnchorsFind, ReturnsNullBeforeTheFirstAnchorAndForEmptyInput) {
  std::vector<VerseAnchors::VerseAnchor> anchors{{5, 1, 1}};
  EXPECT_EQ(VerseAnchors::find(anchors, 4), nullptr);
  EXPECT_EQ(VerseAnchors::find({}, 0), nullptr);
}

TEST(VerseAnchorsFormat, RendersChapterColonVerseAndEmptyForNull) {
  const VerseAnchors::VerseAnchor a{0, 11, 19};
  EXPECT_EQ(VerseAnchors::format(&a), "11:19");
  EXPECT_EQ(VerseAnchors::format(nullptr), "");
}
```

- [ ] **Step 3: Write the CMake wiring**

`test/verse_anchors/CMakeLists.txt` — note expat is compiled here with the **same flags `platformio.ini` passes**, so host and firmware tokenise identically:

```cmake
add_executable(VerseAnchorsTest
  VerseAnchorsTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerseAnchors.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(VerseAnchorsTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(VerseAnchorsTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)

target_link_libraries(VerseAnchorsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VerseAnchorsTest)
```

Add to `test/CMakeLists.txt`, alphabetically among the existing `add_subdirectory` lines:

```cmake
add_subdirectory(verse_anchors)
```

- [ ] **Step 4: Run and confirm it fails**

```bash
cmake -S test -B build/test && cmake --build build/test --target VerseAnchorsTest
```

Expected: **configure or build failure** — `VerseAnchors.cpp` does not exist yet. That is the TDD checkpoint.

- [ ] **Step 5: Implement**

`lib/Epub/Epub/VerseAnchors.cpp`:

```cpp
#include "VerseAnchors.h"

#include <expat.h>

#include <cstdio>
#include <cstring>

namespace VerseAnchors {
namespace {

// Same list as VisibleTextUtils::isNonVisibleElement (VisibleTextUtils.h:17-20).
bool isNonVisible(const char* name) {
  const char* colon = strchr(name, ':');
  const char* n = colon ? colon + 1 : name;
  return strcmp(n, "head") == 0 || strcmp(n, "style") == 0 || strcmp(n, "script") == 0 ||
         strcmp(n, "title") == 0 || strcmp(n, "rp") == 0;
}

bool isBody(const char* name) {
  const char* colon = strchr(name, ':');
  return strcmp(colon ? colon + 1 : name, "body") == 0;
}

struct State {
  std::vector<VerseAnchor> anchors;
  uint32_t offset = 0;
  int nonVisibleDepth = 0;
  bool insideBody = false;
};

void XMLCALL onStart(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* s = static_cast<State*>(userData);
  if (isBody(name)) s->insideBody = true;
  if (s->insideBody && (s->nonVisibleDepth > 0 || isNonVisible(name))) s->nonVisibleDepth++;
  if (!s->insideBody) return;

  for (int i = 0; atts && atts[i]; i += 2) {
    if (strcmp(atts[i], "id") != 0) continue;
    unsigned chapter = 0, verse = 0;
    char tail = '\0';
    // %c catches trailing junk so "chapter1_verse2x" is rejected.
    if (sscanf(atts[i + 1], "chapter%u_verse%u%c", &chapter, &verse, &tail) == 2 &&
        chapter <= UINT16_MAX && verse <= UINT16_MAX) {
      s->anchors.push_back({s->offset, static_cast<uint16_t>(chapter), static_cast<uint16_t>(verse)});
    }
    break;
  }
}

void XMLCALL onEnd(void* userData, const XML_Char* name) {
  auto* s = static_cast<State*>(userData);
  if (s->nonVisibleDepth > 0) s->nonVisibleDepth--;
  if (isBody(name)) s->insideBody = false;
}

void XMLCALL onText(void* userData, const XML_Char* text, const int len) {
  auto* s = static_cast<State*>(userData);
  if (!s->insideBody || s->nonVisibleDepth != 0) return;
  // Count codepoints, not bytes: continuation bytes are 10xxxxxx.
  for (int i = 0; i < len; i++) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) s->offset++;
  }
}

}  // namespace

std::vector<VerseAnchor> scan(const char* xhtml, const size_t length) {
  State state;
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return {};
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);
  XML_Parse(parser, xhtml, static_cast<int>(length), 1);
  XML_ParserFree(parser);
  return std::move(state.anchors);
}

const VerseAnchor* find(const std::vector<VerseAnchor>& anchors, const uint32_t offset) {
  const VerseAnchor* best = nullptr;
  for (const auto& a : anchors) {
    if (a.offset > offset) break;  // ascending by construction
    best = &a;
  }
  return best;
}

std::string format(const VerseAnchor* anchor) {
  if (!anchor) return {};
  char buf[16];
  snprintf(buf, sizeof(buf), "%u:%u", anchor->chapter, anchor->verse);
  return buf;
}

}  // namespace VerseAnchors
```

- [ ] **Step 6: Run and confirm it passes**

```bash
cmake --build build/test --target VerseAnchorsTest && ctest --test-dir build/test -R VerseAnchors --output-on-failure
```

Expected: **7/7 pass**.

- [ ] **Step 7: Full suite, then commit**

```bash
cmake --build build/test && ctest --test-dir build/test -j
git add lib/Epub/Epub/VerseAnchors.h lib/Epub/Epub/VerseAnchors.cpp test/verse_anchors test/CMakeLists.txt
git commit -m "feat(epub): resolve a visible offset to a verse reference"
```

Expected: **231 tests** (224 + 7).

---

### Task 2: label a new highlight with its reference

**Files:** modify `src/activities/reader/PassageSelectActivity.{h,cpp}`, `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Give the activity what it needs to resolve a reference**

`PassageSelectActivity` currently receives a `Page` and a `spineIndex` but no `Epub`. Add `Epub& epub` as a constructor parameter, stored as a reference member `Epub& epub;` beside `highlightDoc`. Update the single construction site in `EpubReaderActivity::openHighlightPassage` (`:349-353`) to pass `*epub`.

- [ ] **Step 2: Build the reference at save time**

Add to `PassageSelectActivity.h`, next to `selectionLabel`:

```cpp
  // "Mateo 11:19", or empty when this book has no verse anchors.
  std::string verseReference(uint32_t startOffset) const;
```

In `PassageSelectActivity.cpp`:

```cpp
std::string PassageSelectActivity::verseReference(const uint32_t startOffset) const {
  const auto spine = epub.getSpineItem(spineIndex);
  if (spine.href.empty()) return {};

  size_t size = 0;
  // Owned buffer: readItemContentsToBytes allocates and the caller frees.
  auto* bytes = epub.readItemContentsToBytes(spine.href, &size, /*trailingNullByte=*/false);
  if (!bytes) return {};
  const auto anchors = VerseAnchors::scan(reinterpret_cast<const char*>(bytes), size);
  free(bytes);

  const std::string verse = VerseAnchors::format(VerseAnchors::find(anchors, startOffset));
  if (verse.empty()) return {};

  // Book name comes from the covering TOC entry, never a built-in table: a
  // hardcoded list would be wrong in every other language and every non-Bible book.
  if (spine.tocIndex < 0) return verse;
  const auto toc = epub.getTocItem(spine.tocIndex);
  if (toc.title.empty()) return verse;
  return toc.title + " " + verse;
}
```

- [ ] **Step 3: Prepend it to the label**

In `finalizeSelection`, replace the single label line with:

```cpp
  const std::string reference = verseReference(minOffset);
  const std::string passage = selectionLabel(lo, hi);
  // addHighlight truncates to 72 bytes, so the reference is placed first: it
  // is the part that must survive truncation.
  entry.label = reference.empty() ? passage : (reference + " \xc2\xb7 " + passage);
```

- [ ] **Step 4: Build both boards and commit**

```bash
pio run -e x4pro && pio run -e default
git add src/activities/reader/PassageSelectActivity.h src/activities/reader/PassageSelectActivity.cpp src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat(highlights): label a highlight with its verse reference"
```

Run the two `pio` invocations **sequentially** — they race on a shared `idf_component.yml`.

---

### Task 3: store the first anchor as an absolute offset

**Files:** modify `src/activities/reader/PassageSelectActivity.{h,cpp}`

No behaviour change. This is the refactor that makes Task 4 possible.

- [ ] **Step 1: Replace the member**

In `PassageSelectActivity.h:113`, replace `int anchorIndex = -1;` with:

```cpp
  // Absolute visible-codepoint offset, not an index into `words`: the anchor
  // must outlive the page it was placed on (see Task 4).
  static constexpr uint32_t NO_ANCHOR = UINT32_MAX;
  uint32_t anchorOffset = NO_ANCHOR;
```

- [ ] **Step 2: Set it in `commitAt`**

```cpp
  if (phase == Phase::PickingStart) {
    anchorOffset = words[index].offset;
    cursor = index;
    phase = Phase::PickingEnd;
    requestUpdate();
    return;
  }
```

- [ ] **Step 3: Resolve the range from offsets, not indices**

`finalizeSelection` currently scans `words[lo..hi]`. Replace the min/max block with:

```cpp
  const uint32_t cursorOffset = words[endIndex].offset;
  const uint32_t minOffset = std::min(anchorOffset, cursorOffset);
  const uint32_t maxOffset = std::max(anchorOffset, cursorOffset);
```

**This is not a pure refactor and must not be described as one.** The index
scan exists because `words` is in visual order: on an RTL line a word *between*
the two endpoints can hold an offset outside `[anchorOffset, cursorOffset]`, and
the scan catches it. Comparing only the endpoints drops those words from the
stored range.

Same-page selections keep the scan for exactly that reason. The endpoint
comparison is used **only** when the anchor is not on the current page, where
the intervening words are unreachable and no better answer exists:

```cpp
  uint32_t minOffset, maxOffset;
  const int anchorIdx = anchorIndexOnThisPage();
  if (anchorIdx >= 0) {
    const int lo = std::min(anchorIdx, endIndex);
    const int hi = std::max(anchorIdx, endIndex);
    minOffset = maxOffset = words[lo].offset;
    for (int i = lo; i <= hi; i++) {
      minOffset = std::min(minOffset, words[i].offset);
      maxOffset = std::max(maxOffset, words[i].offset);
    }
  } else {
    minOffset = std::min(anchorOffset, words[endIndex].offset);
    maxOffset = std::max(anchorOffset, words[endIndex].offset);
  }
```

The residual imprecision is confined to an RTL line on a page boundary of a
multi-page selection. It is accepted and recorded, not hidden.

`selectionLabel(lo, hi)` still needs page-local indices, so keep `lo`/`hi`
derived from `anchorIndexOnThisPage()` (below) and `endIndex`; when the anchor
is not on this page, start the label at index 0.

- [ ] **Step 4: Add the page-local lookup used by the outline and label**

```cpp
// Index of the anchor word on the current page, or -1 when the anchor was
// placed on an earlier page.
int PassageSelectActivity::anchorIndexOnThisPage() const {
  if (anchorOffset == NO_ANCHOR) return -1;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].offset == anchorOffset) return i;
  }
  return -1;
}
```

- [ ] **Step 5: Draw the outline from offsets**

`drawSelectionOutline` must outline every word whose offset falls in
`[min(anchorOffset, cursorOffset), max(...)]` rather than indices `[lo, hi]`.
An anchor on an earlier page simply contributes no rects on this page.

- [ ] **Step 6: Build both boards and commit**

```bash
pio run -e x4pro && pio run -e default
git commit -am "refactor(highlights): anchor a selection by offset, not page index"
```

---

### Task 4: turn the page mid-selection

**Files:** modify `src/activities/reader/PassageSelectActivity.{h,cpp}`, `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Give the activity the section**

Add `Section& section` to the constructor, stored as a reference member, and pass
`*section` from `openHighlightPassage`. This is safe: that handler holds a live
`section` and does not reset it, unlike the `TEXT_SETTINGS` and `SELECT_CHAPTER`
paths (`EpubReaderActivity.cpp:790-800`).

Track the page number too:

```cpp
  uint16_t currentPageNumber;  // constructor takes section.currentPage
```

- [ ] **Step 2: Advance a page**

```cpp
bool PassageSelectActivity::advancePage() {
  if (phase != Phase::PickingEnd) return false;

  // Deliberately NOT a `pageCount` comparison: on a partial section pageCount
  // is a watermark, not the chapter total (Section.h:114), so comparing
  // against it refuses valid turns into a still-building chapter. Asking for
  // the page and treating nullptr as "no more" is correct for both cases.
  auto next = section.loadPage(currentPageNumber + 1);
  if (!next) return false;

  {
    // The render task reads `page`, `words` and `committedRects`; swapping them
    // unfenced is the same hazard the tag-deletion fix closed in 0c1c884a.
    RenderLock lock;
    page = std::move(next);
    currentPageNumber++;
    extractWords();
    rebuildCommittedRects();
    cursor = 0;
  }
  requestUpdate();
  return true;
}
```

`rebuildCommittedRects()` is the existing `HighlightOverlay::buildRects` call from
`onEnter` (`PassageSelectActivity.cpp:61-67`), extracted into a method so both
callers share it.

- [ ] **Step 3: Bind the gesture**

In the input handler, before the word-tap branch and only while `PickingEnd`:

```cpp
  if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left && advancePage()) return;
```

Right-to-left is `SwipeDir::Left`. It cannot collide with Back, which is an
**edge**-anchored left-to-right swipe (`MappedInputManager.cpp:266-271`).

- [ ] **Step 4: Build both boards and commit**

```bash
pio run -e x4pro && pio run -e default
git commit -am "feat(highlights): extend a selection across page turns"
```

---

### Task 5: an explicit commit row in the tag picker

**Files:** modify `src/activities/reader/TagPickerActivity.{h,cpp}`

- [ ] **Step 1: One conversion, used everywhere**

Row 0 becomes "Done"; tags shift down by one. Add to the header:

```cpp
  static constexpr int DONE_ROW = 0;
  // Row index -> index into highlightDoc.tags(), or -1 for a non-tag row.
  // Every site that maps a row to a tag MUST go through this: the August
  // reviews twice found bugs from an offset applied in one place and not the next.
  int tagIndexForRow(int row) const;
```

```cpp
int TagPickerActivity::tagIndexForRow(const int row) const {
  const int tagCount = static_cast<int>(highlightDoc.tags().size());
  if (row <= DONE_ROW || row > tagCount) return -1;  // Done row, or "New tag..."
  return row - 1;
}
```

- [ ] **Step 2: Update `listCount` and `buildScreen`**

`listCount()` becomes `tags.size() + 2` (Done + tags + "New tag..."). In
`buildScreen`, emit the Done row first with `label = tr(STR_DONE)`, `toggle =
false`, then the tag rows at `rowItems_[i + 1]` with `toggleChecked =
selected_[i]`, then "New tag..." last. `MAX_ROWS` becomes `MAX_TAGS + 2`.

- [ ] **Step 3: Route activation and long-press through the helper**

`activateIndex`: `row == DONE_ROW` calls `commitAndFinish()`; the last row starts
the new-tag flow; otherwise `toggleTag(tagIndexForRow(row))`.

`onRowLongPress`: `if (tagIndexForRow(index) < 0) return;` — long-pressing Done or
"New tag..." must not offer a delete. Then delete `tagIndexForRow(index)`.

- [ ] **Step 4: Correct the footer**

`drawFooter` currently advertises only the discarding exit. Change the second
label from `tr(STR_TOGGLE)` to `tr(STR_SELECT)` and keep Back as cancel, so the
list no longer names a single exit that destroys the user's work.

- [ ] **Step 5: No i18n work is needed**

`STR_DONE` (`english.yaml:153`) and `STR_SELECT` (`:237`) both already exist.
Do **not** edit the translations or run `gen_i18n.py`: there is nothing to add,
and the three generated files are gitignored.

- [ ] **Step 6: Build both boards and commit**

```bash
pio run -e x4pro && pio run -e default
git commit -am "fix(highlights): commit tags from an explicit Done row"
```

---

### Task 6: verification

- [ ] **Step 1: Full suite and both boards**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

Expected **231** host tests; both boards SUCCESS. Sequential `pio` runs.

- [ ] **Step 2: Confirm no orphans**

```bash
grep -rn "anchorIndex" src/activities/reader/PassageSelectActivity.cpp
grep -rn "tagIndexForRow" src/activities/reader/TagPickerActivity.cpp
```

The first must return **no** hits (fully replaced by `anchorOffset`); the second
must appear in `activateIndex`, `onRowLongPress` and `buildScreen`.

- [ ] **Step 3: Regenerate the 56 imported labels**

The imported entries carry reference-only labels; new ones now carry
`reference · passage`. Re-run the import with snippets so the list is uniform,
and re-upload. The import is reproducible from the `.jwlibrary` backup.

- [ ] **Step 4: Device checks** (needs the panel)

1. Highlight a passage split by a page break: swipe right-to-left mid-selection, confirm on the next page, verify the saved highlight covers **both** parts.
2. Confirm a new highlight's label reads `<book> <chapter>:<verse> · <passage>`.
3. Pick tags, leave via **Done**, reopen the highlight — tags must persist. Then pick tags and leave via **Back** — they must not.
4. Open a non-Bible EPUB, highlight something, confirm the label is passage text with no stray separator.

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Scan disagrees with the layout parser's offsets | Medium | Host tests compile the repo's own expat with the firmware's flags, so tokenisation cannot drift; Task 6 Step 4.2 confirms on device |
| Off-by-one from the inserted Done row | **High** | `tagIndexForRow` is the single conversion; Task 6 Step 2 greps for its use |
| Page turn races the render task | Medium | Swap under `RenderLock`, as in `0c1c884a` |
| Reading a spine item on every save is slow | Low | Bounded by spine-item size (a few KB here); measure in Task 6 Step 4.2 |
| Multi-page RTL selection loses words whose offset falls outside the endpoints | Medium | Same-page selections keep the exact scan; only the cross-page case degrades, and it is stated in Task 3 rather than silently accepted |
| `readItemContentsToBytes` leaks or mismatches its deallocator | Medium | Verified `malloc` (`ZipFile.cpp:378`), so `free` is correct — not `delete[]`. Single `free` before any use of the result, with no early return between alloc and free |
