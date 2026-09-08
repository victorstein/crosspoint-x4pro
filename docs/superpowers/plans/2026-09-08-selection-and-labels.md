# Cross-page Selection, Reference Labels, and Tag Commit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a selection cross a page boundary, label highlights with their Bible reference, and stop the tag picker discarding tags on the exit it advertises.

**Architecture:** A new pure `VerseAnchors` component in `lib/Epub` scans a spine item with the repo's own expat and maps a visible-codepoint offset to a `chapter:verse` reference; host tests compile that same expat so the scan is tokenised identically to the firmware. `PassageSelectActivity` switches its first anchor from a page-local index to an absolute offset, which is what makes forward page turns cost nothing per page. `TagPickerActivity` gains an explicit commit row.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest, vendored expat.

**Depends on:** `docs/superpowers/specs/2026-09-08-selection-and-labels-design.md`. Baseline **224 host tests** at `f6516add`.

> **v2 — revised after adversarial review.**
>
> v1's feature 2 did not work. The scan registered two expat handlers where the
> real parser registers three: `XML_SetDefaultHandlerExpand`
> (`ChapterHtmlSlimParser.cpp:1575`) feeds expanded entities back through
> `characterData` (`:1355-1368`), so every `&nbsp;` shifted the count by one and
> every unknown entity by five. Its `isBody`/`isNonVisible` were case-sensitive
> and namespace-stripping; the parser's are the exact opposite
> (`:397-401`, `VisibleTextUtils.h:8-20`). Measured divergence on ordinary input.
>
> Worse, v1 argued a 3-codepoint agreement proved the counter correct. That is
> backwards: `find()` takes the greatest anchor <= the offset and verse markers
> sit **at** verse boundaries, so a few codepoints IS the entire margin between
> verse N and N-1. The cited evidence was a reproduction of the failure mode.
>
> **v2 stops re-implementing the counter.** Task 1 extracts the gate and counter
> into one unit both walks call, so "the two agree" is true by construction
> rather than by a test that cannot fail. Task 7 adds a device self-check
> against offsets the section cache already stores.
>
> v1 also shipped the off-by-one feature 3 was written to prevent: `activateIndex`
> and `onRowLongPress` receive `event.value` (an `actionValue`), not a row index,
> and `handleButtons` was missing from the plan entirely — while v1's own
> verification grep was written so that miss would pass.

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
| `lib/Epub/Epub/VisibleOffsetCounter.h` (create) | The gate and codepoint counter, shared verbatim by the layout parser and the verse scan |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}` (modify) | Delegate its counter to the shared unit — behaviour-preserving |
| `lib/Epub/Epub/Section.{h,cpp}` (modify) | Add `htmlCachePath()`; fold the two existing inline copies into it |
| `lib/Epub/Epub/VerseAnchors.h` / `.cpp` (create) | Scan a spine item for verse-anchor offsets; resolve an offset to `chapter:verse` |
| `test/verse_anchors/VerseAnchorsTest.cpp` + `CMakeLists.txt` (create) | Host tests, compiling the repo's expat |
| `test/CMakeLists.txt` (modify) | Register the suite |
| `src/activities/reader/PassageSelectActivity.{h,cpp}` (modify) | Absolute anchor, forward page turn, reference label |
| `src/activities/reader/EpubReaderActivity.cpp` (modify) | Pass `Section&` and `Epub&` to the selector |
| `src/activities/reader/TagPickerActivity.{h,cpp}` (modify) | "Done" row |

---

### Task 1: one counter, shared by both walks

**Files:** create `lib/Epub/Epub/VisibleOffsetCounter.h`, `test/visible_offset/VisibleOffsetTest.cpp`, `test/visible_offset/CMakeLists.txt`; modify `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}`, `test/CMakeLists.txt`

The review's central finding: a second implementation of this counter cannot be
kept in agreement by testing, because the tests are written by whoever wrote the
drift. So there is only one implementation.

- [ ] **Step 1: Write the shared unit**

`lib/Epub/Epub/VisibleOffsetCounter.h`:

```cpp
#pragma once

#include <cstdint>
#include <cstring>

#include "VisibleTextUtils.h"

// The canonical visible-codepoint counter. ChapterHtmlSlimParser owns the
// reading-position semantics; this holds the state machine so a second walk
// (VerseAnchors) can count identically instead of approximating it.
//
// Predicates are the parser's, verbatim: strcasecmp on the FULL element name
// for <body> (ChapterHtmlSlimParser.cpp:397-401), and
// VisibleTextUtils::isNonVisibleElement -- case-insensitive, no namespace
// stripping (VisibleTextUtils.h:8-20).
struct VisibleOffsetCounter {
  uint32_t offset = 0;
  int nonVisibleDepth = 0;
  bool insideBody = false;

  void onStartElement(const char* name) {
    if (strcasecmp(name, "body") == 0) insideBody = true;
    if (insideBody && (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name))) nonVisibleDepth++;
  }

  void onEndElement(const char* name) {
    if (nonVisibleDepth > 0) nonVisibleDepth--;
    if (strcasecmp(name, "body") == 0) insideBody = false;
  }

  bool counting() const { return insideBody && nonVisibleDepth == 0; }

  // Callers gate on their own synthetic flag before calling: parser-injected
  // table-cell prefixes and image alt text must not advance the offset.
  void onCharacterData(const char* s, const int len) {
    if (!counting()) return;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    for (int i = 0; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) offset++;  // skip UTF-8 continuation bytes
    }
  }
};
```

- [ ] **Step 2: Make the parser delegate to it**

In `ChapterHtmlSlimParser.h`, replace the three members `visibleTextOffset`,
`nonVisibleTextDepth` and `insideBody` with `VisibleOffsetCounter visibleCounter_`,
and add `uint32_t visibleTextOffset() const { return visibleCounter_.offset; }`
so existing readers keep working.

In `startElement` replace the `insideBody`/`nonVisibleTextDepth` block
(`:397-405`) with `self->visibleCounter_.onStartElement(name);`. In `endElement`
replace `:1373-1374` with `self->visibleCounter_.onEndElement(name);`. In
`characterData` replace the counting block (`:1149-1158`) with:

```cpp
  const uint32_t callbackVisibleOffset = self->visibleCounter_.offset;
  if (!self->syntheticCharacterData) self->visibleCounter_.onCharacterData(s, len);
```

**This must be behaviour-preserving.** Any change to the offsets it produces
invalidates every cached section on every device.

- [ ] **Step 3: Prove it is behaviour-preserving**

```bash
cmake --build build/test && ctest --test-dir build/test -j
```

Expected: **224/224**, unchanged. The pagination-invariance suite exercises the
offset pipeline end to end; a regression here fails it.

- [ ] **Step 4: Test the counter directly**

`test/visible_offset/VisibleOffsetTest.cpp` — the cases v1's tests could not detect:

```cpp
#include <gtest/gtest.h>

#include <cstring>

#include "VisibleOffsetCounter.h"

TEST(VisibleOffsetCounter, IsCaseInsensitiveOnBodyAndNonVisibleTags) {
  VisibleOffsetCounter c;
  c.onStartElement("BODY");
  EXPECT_TRUE(c.insideBody) << "the parser uses strcasecmp, not strcmp";
  c.onStartElement("TITLE");
  c.onCharacterData("skipme", 6);
  EXPECT_EQ(c.offset, 0u) << "<TITLE> inside <body> must not be counted";
}

TEST(VisibleOffsetCounter, DoesNotStripNamespacePrefixes) {
  VisibleOffsetCounter c;
  c.onStartElement("h:body");
  EXPECT_FALSE(c.insideBody) << "the parser matches the full name; a prefixed body is not <body>";
}

TEST(VisibleOffsetCounter, CountsCodepointsNotBytes) {
  VisibleOffsetCounter c;
  c.onStartElement("body");
  c.onCharacterData("\xc3\xa9\xc3\xa9", 4);
  EXPECT_EQ(c.offset, 2u);
}

TEST(VisibleOffsetCounter, NestedNonVisibleSubtreesUnwindSymmetrically) {
  VisibleOffsetCounter c;
  c.onStartElement("body");
  c.onStartElement("head");
  c.onStartElement("span");   // every start inside a non-visible subtree increments
  c.onEndElement("span");
  c.onEndElement("head");
  c.onCharacterData("abc", 3);
  EXPECT_EQ(c.offset, 3u) << "the gate must reopen once the subtree closes";
}
```

`test/visible_offset/CMakeLists.txt`:

```cmake
add_executable(VisibleOffsetTest VisibleOffsetTest.cpp)
target_include_directories(VisibleOffsetTest PRIVATE ${REPO_ROOT}/lib/Epub/Epub)
target_link_libraries(VisibleOffsetTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(VisibleOffsetTest)
```

Append `add_subdirectory(visible_offset)` to `test/CMakeLists.txt`. **Append —
that list is not alphabetical, it is append-ordered.**

- [ ] **Step 5: Build both boards and commit**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
git add lib/Epub/Epub/VisibleOffsetCounter.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp test/visible_offset test/CMakeLists.txt
git commit -m "refactor(epub): extract the visible-codepoint counter"
```

Expected **228** host tests (224 + 4). Run the `pio` invocations sequentially.

---

### Task 2: `VerseAnchors`

**Files:** create `lib/Epub/Epub/VerseAnchors.{h,cpp}`, `test/verse_anchors/{VerseAnchorsTest.cpp,CMakeLists.txt}`; modify `test/CMakeLists.txt`

- [ ] **Step 1: Header**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VerseAnchors {

struct VerseAnchor {
  uint32_t offset;
  uint16_t chapter;
  uint16_t verse;
};

// Anchors shaped `chapter<N>_verse<M>`, ascending by offset. Empty when the
// document has none, or when the parse fails part-way -- a partial list would
// resolve later highlights to a stale anchor with no way for the caller to tell.
std::vector<VerseAnchor> scan(const char* xhtml, size_t length);

const VerseAnchor* find(const std::vector<VerseAnchor>& anchors, uint32_t offset);
std::string format(const VerseAnchor* anchor);

}  // namespace VerseAnchors
```

- [ ] **Step 2: Implementation — all three handlers**

```cpp
#include "VerseAnchors.h"

#include <expat.h>

#include <cstdio>
#include <cstring>

#include "VisibleOffsetCounter.h"
#include "htmlEntities.h"

namespace VerseAnchors {
namespace {

struct State {
  VisibleOffsetCounter counter;
  std::vector<VerseAnchor> anchors;
};

void XMLCALL onStart(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* s = static_cast<State*>(userData);
  s->counter.onStartElement(name);
  if (!s->counter.insideBody) return;
  for (int i = 0; atts && atts[i]; i += 2) {
    if (strcmp(atts[i], "id") != 0) continue;
    unsigned chapter = 0, verse = 0;
    char tail = '\0';
    if (sscanf(atts[i + 1], "chapter%u_verse%u%c", &chapter, &verse, &tail) == 2 && chapter <= UINT16_MAX &&
        verse <= UINT16_MAX) {
      s->anchors.push_back({s->counter.offset, static_cast<uint16_t>(chapter), static_cast<uint16_t>(verse)});
    }
    break;
  }
}

void XMLCALL onEnd(void* userData, const XML_Char* name) {
  static_cast<State*>(userData)->counter.onEndElement(name);
}

void XMLCALL onText(void* userData, const XML_Char* text, const int len) {
  static_cast<State*>(userData)->counter.onCharacterData(text, len);
}

// Mirrors ChapterHtmlSlimParser::defaultHandlerExpand (:1355-1368). Without
// this, every named entity before a highlight shifts the offset -- by one for a
// known entity, by the whole `&foo;` run for an unknown one -- and a deficit of
// even one codepoint resolves a highlight to the PREVIOUS verse.
void XMLCALL onDefault(void* userData, const XML_Char* s, const int len) {
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (value != nullptr) {
      onText(userData, value, static_cast<int>(strlen(value)));
      return;
    }
    onText(userData, s, len);
  }
}

}  // namespace

std::vector<VerseAnchor> scan(const char* xhtml, const size_t length) {
  State state;
  state.anchors.reserve(64);  // Psalm 119 has 176; 64 covers the common chapter
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return {};
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);
  XML_SetDefaultHandlerExpand(parser, onDefault);
  const auto status = XML_Parse(parser, xhtml, static_cast<int>(length), 1);
  XML_ParserFree(parser);
  if (status == XML_STATUS_ERROR) return {};
  return std::move(state.anchors);
}

const VerseAnchor* find(const std::vector<VerseAnchor>& anchors, const uint32_t offset) {
  const VerseAnchor* best = nullptr;
  for (const auto& a : anchors) {
    if (a.offset > offset) break;
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

- [ ] **Step 3: Tests, including the divergences v1 could not detect**

`test/verse_anchors/VerseAnchorsTest.cpp` — note `#include <cstring>` explicitly:

```cpp
#include <gtest/gtest.h>

#include <cstring>

#include "VerseAnchors.h"

TEST(VerseAnchorsScan, CountsAKnownEntityAsOneCodepoint) {
  // The v1 failure: without a default handler this returned 4, not 5.
  const char* doc =
      "<html><body><p><span id=\"chapter1_verse1\"></span>ab&nbsp;cd"
      "<span id=\"chapter1_verse2\"></span>x</p></body></html>";
  const auto a = VerseAnchors::scan(doc, strlen(doc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[1].offset, 5u) << "&nbsp; must advance the offset by exactly one";
}

TEST(VerseAnchorsScan, SkipsUppercaseNonVisibleElementsInsideBody) {
  const char* doc =
      "<html><body><TITLE>skipme</TITLE><span id=\"chapter1_verse1\"></span>abc</body></html>";
  const auto a = VerseAnchors::scan(doc, strlen(doc));
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].offset, 0u) << "isNonVisibleElement is case-insensitive";
}

TEST(VerseAnchorsScan, ReturnsNothingWhenTheDocumentIsMalformed) {
  const char* doc = "<html><body><span id=\"chapter1_verse1\"></span>abc<unclosed>";
  EXPECT_TRUE(VerseAnchors::scan(doc, strlen(doc)).empty())
      << "a partial list would resolve later highlights to a stale anchor";
}

TEST(VerseAnchorsScan, IgnoresIdsThatAreNotVerseMarkers) {
  const char* doc =
      "<html><body><p id=\"p188\"><span id=\"pos107452\"></span>"
      "<span id=\"footnotesource14\"></span>abc</p></body></html>";
  EXPECT_TRUE(VerseAnchors::scan(doc, strlen(doc)).empty());
}

TEST(VerseAnchorsFind, ReturnsTheAnchorCoveringAnOffsetAndNullBeforeTheFirst) {
  std::vector<VerseAnchors::VerseAnchor> anchors{{5, 1, 1}, {20, 1, 2}};
  EXPECT_EQ(VerseAnchors::find(anchors, 5)->verse, 1);
  EXPECT_EQ(VerseAnchors::find(anchors, 19)->verse, 1);
  EXPECT_EQ(VerseAnchors::find(anchors, 20)->verse, 2);
  EXPECT_EQ(VerseAnchors::find(anchors, 4), nullptr);
  EXPECT_EQ(VerseAnchors::find({}, 0), nullptr);
}

TEST(VerseAnchorsFormat, RendersChapterColonVerseAndEmptyForNull) {
  const VerseAnchors::VerseAnchor a{0, 11, 19};
  EXPECT_EQ(VerseAnchors::format(&a), "11:19");
  EXPECT_EQ(VerseAnchors::format(nullptr), "");
}
```

`test/verse_anchors/CMakeLists.txt` — expat compiled as **C** with the firmware's flags:

```cmake
add_executable(VerseAnchorsTest
  VerseAnchorsTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerseAnchors.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(VerseAnchorsTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(VerseAnchorsTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)

target_link_libraries(VerseAnchorsTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(VerseAnchorsTest)
```

`xmltok_impl.c` and `xmltok_ns.c` are `#include`d by `xmltok.c` — do not list them.
The top-level `project(... C CXX)` declaration is load-bearing: these compile as C.

Append `add_subdirectory(verse_anchors)` to `test/CMakeLists.txt`.

- [ ] **Step 4: Run, then commit**

```bash
cmake --build build/test && ctest --test-dir build/test -j
git add lib/Epub/Epub/VerseAnchors.h lib/Epub/Epub/VerseAnchors.cpp test/verse_anchors test/CMakeLists.txt
git commit -m "feat(epub): resolve a visible offset to a verse reference"
```

Expected **234** tests (228 + 6).

---

### Task 3: label a new highlight with its reference

**Files:** modify `src/activities/reader/PassageSelectActivity.{h,cpp}`, `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Pass the book in**

Add `Epub& epub` to the constructor, stored as a reference member. Update the one
construction site, `EpubReaderActivity::openHighlightPassage` (`:349-353`), to pass
`*epub`. Include it as `#include <Epub/VerseAnchors.h>` in the `.cpp` (firmware
convention; the host CMake adds `lib/Epub/Epub` so its tests use `"VerseAnchors.h"`).

- [ ] **Step 2: Read from the HTML cache, not the zip**

The bytes are already inflated on SD: `Section::startBuild` writes
`<cachePath>/html/<spineIndex>.html` and `Section::hasHtmlCache()` (`:461-462`)
reports it. Re-inflating the zip entry instead would block the UI thread for
seconds on a large spine item, at the moment the user taps Highlight.

```cpp
std::string PassageSelectActivity::verseReference(const uint32_t startOffset) const {
  if (!section.hasHtmlCache()) return {};

  HalFile file;
  if (!Storage.openFileForRead("PSA", section.htmlCachePath(), file)) return {};

  // NOTE: htmlCachePath() does not exist yet. Section builds this string inline
  // in two places already (Section.cpp:276-277 and :461). Add the accessor and
  // route BOTH existing sites through it rather than adding a third copy:
  //   std::string Section::htmlCachePath() const {
  //     return epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  //   }

  // Chunked parse: never hold the whole spine item resident.
  XML_Parser parser = nullptr;
  std::vector<VerseAnchors::VerseAnchor> anchors = VerseAnchors::scanStream(file, parser);
  if (anchors.empty()) return {};

  const std::string verse = VerseAnchors::format(VerseAnchors::find(anchors, startOffset));
  if (verse.empty()) return {};

  const auto spine = epub.getSpineItem(spineIndex);
  if (spine.tocIndex < 0) return verse;
  const auto toc = epub.getTocItem(spine.tocIndex);
  return toc.title.empty() ? verse : toc.title + " " + verse;
}
```

Add `scanStream(HalFile&)` to `VerseAnchors` alongside `scan()`, driving expat with
`XML_GetBuffer`/`XML_ParseBuffer` in fixed chunks exactly as
`ChapterHtmlSlimParser::parseStep` does (`:1595-1618`). `scan()` stays for tests.

- [ ] **Step 3: Compose within the byte budget**

`utf8SafeSummary` truncates at **72 bytes, not characters** (`Utf8.h:26-31`), and
the separator costs 4. A long TOC title can consume the whole budget:

```cpp
  const std::string reference = verseReference(minOffset);
  const std::string passage = selectionLabel(lo, hi);
  static constexpr size_t LABEL_BUDGET = 72;
  static constexpr size_t MIN_SNIPPET = 16;
  if (reference.empty()) {
    entry.label = passage;
  } else if (reference.size() + 4 + MIN_SNIPPET > LABEL_BUDGET) {
    entry.label = reference;  // no room for a useful snippet; keep the reference whole
  } else {
    entry.label = reference + " \xc2\xb7 " + passage;
  }
```

- [ ] **Step 4: Build both boards and commit**

```bash
pio run -e x4pro
pio run -e default
git commit -am "feat(highlights): label a highlight with its verse reference"
```

---

### Task 4: anchor a selection by offset

**Files:** modify `src/activities/reader/PassageSelectActivity.{h,cpp}`

- [ ] **Step 1: Keep both, and never search by offset**

Word offsets are **not** unique: a synthesized table-cell prefix
(`ChapterHtmlSlimParser.cpp:534-551`) and the image `alt` fallback (`:854-863`)
emit several words while the offset is frozen. Searching for the anchor by offset
equality would resolve to the first of them. So the index is kept while it is
still valid, and only abandoned at a page turn:

```cpp
  static constexpr uint32_t NO_ANCHOR = UINT32_MAX;
  uint32_t anchorOffset = NO_ANCHOR;
  // Index of the anchor in `words`, or -1 once a page turn has left its page.
  // Never recovered by searching: offsets are not unique.
  int anchorIndex = -1;
```

`commitAt`'s `PickingStart` branch sets both: `anchorOffset = words[index].offset;
anchorIndex = index;`.

- [ ] **Step 2: Resolve the range, on-page exactly and cross-page by endpoints**

```cpp
  uint32_t minOffset, maxOffset;
  if (anchorIndex >= 0) {
    const int lo = std::min(anchorIndex, endIndex);
    const int hi = std::max(anchorIndex, endIndex);
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

The scan stays for the on-page case because `words` is in visual order and an RTL
line can hold an in-between word outside the endpoint range. Cross-page, those
words are unreachable and the endpoints are the best available answer.

- [ ] **Step 3: The preview must use the SAME split**

`drawSelectionOutline` (`:389-397`) runs the identical scan today. It must adopt
Step 2's branch verbatim, or on an RTL page the outline shown and the range stored
are different sets of words. **Keep its `Phase::PickingStart` carve-out**: with
`anchorOffset == UINT32_MAX`, a naive min/max outlines every word to the end of
the section before the first anchor is even placed.

- [ ] **Step 4: Build both boards and commit**

```bash
pio run -e x4pro
pio run -e default
git commit -am "refactor(highlights): anchor a selection by offset"
```

---

### Task 5: turn the page mid-selection

**Files:** modify `src/activities/reader/PassageSelectActivity.{h,cpp}`, `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Pass the section**

Add `Section& section` to the constructor and pass `*section` from
`openHighlightPassage`. Verified safe: `ActivityManager::loop()` runs only the top
activity (`ActivityManager.cpp:93`), so the reader's own `section.reset()` paths
cannot fire underneath.

- [ ] **Step 2: Advance**

```cpp
bool PassageSelectActivity::advancePage() {
  if (phase != Phase::PickingEnd) return false;

  auto next = section.loadPage(currentPageNumber + 1);
  if (!next) {
    // loadPage returns null past the build watermark, and the reader's loop --
    // which normally advances the build -- is frozen while this activity is on
    // top. Without this the swipe is a silent permanent dead end mid-chapter.
    if (!section.isBuildComplete()) {
      section.buildSomeMore(1);
      next = section.loadPage(currentPageNumber + 1);
    }
    if (!next) return false;
  }

  {
    RenderLock lock;
    page = std::move(next);
    currentPageNumber++;
    extractWords();
    rebuildCommittedRects();
    cursor = 0;
    anchorIndex = -1;   // the anchor's page is gone; anchorOffset carries it now
    // render() takes a differential fast path on snapshotValid (:476-482) and
    // would paint the new outline over the OLD page's pixels.
    snapshotValid = false;
  }
  requestUpdate();
  return true;
}
```

Verify `isBuildComplete()` and `buildSomeMore()` signatures in `Section.h` before
use; if the build API differs, surface the refusal with the existing indexing
popup rather than swallowing it.

- [ ] **Step 3: Bind the gesture**

Only in `PickingEnd`, before the word-tap branch:

```cpp
  if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left && advancePage()) return;
```

`SwipeDir::Left` is right-to-left and is already "next page" in the reader
(`ReaderUtils.h:87-89`). Back is a left-**edge** left-to-right swipe
(`MappedInputManager.cpp:266-271`) — no collision.

- [ ] **Step 4: Build both boards and commit**

```bash
pio run -e x4pro
pio run -e default
git commit -am "feat(highlights): extend a selection across page turns"
```

---

### Task 6: an explicit commit row in the tag picker

**Files:** modify `src/activities/reader/TagPickerActivity.{h,cpp}`

- [ ] **Step 1: `actionValue` carries the ROW, and one helper converts**

`activateIndex` and `onRowLongPress` receive `event.value`, which is the item's
`actionValue` (`UiListActivity.cpp:31-43`) — **not** a row position. Today they
coincide because `buildScreen` sets `actionValue = i`. With a Done row inserted
they must carry the row, or `nav.selected` becomes a tag index and the viewport
disagrees with the list.

```cpp
  static constexpr int DONE_ROW = 0;
  // Row -> index into highlightDoc.tags(), or -1 for Done / "New tag...".
  // EVERY row-to-tag conversion goes through this.
  int tagIndexForRow(int row) const;
```

```cpp
int TagPickerActivity::tagIndexForRow(const int row) const {
  const int tagCount = static_cast<int>(highlightDoc.tags().size());
  if (row <= DONE_ROW || row > tagCount) return -1;
  return row - 1;
}
```

- [ ] **Step 2: `buildScreen`, `listCount`, `MAX_ROWS`**

`listCount()` returns `tags.size() + 2`. `MAX_ROWS` becomes `MAX_TAGS + 2`.
`buildScreen` emits Done first (`label = tr(STR_DONE)`, `toggle = false`,
`actionValue = 0`), then each tag at `rowItems_[i + 1]` with
`actionValue = i + 1` and `toggleChecked = selected_[i]`, then "New tag..." last
with `actionValue = tagCount + 1`.

- [ ] **Step 3: Route every consumer through the helper**

`activateIndex(row)`: `row == DONE_ROW` -> `commitAndFinish()`; `row == tagCount + 1`
-> `startNewTagFlow()`; otherwise `toggleTag(tagIndexForRow(row))`.

`onRowLongPress(row)`: `if (tagIndexForRow(row) < 0) return;` then delete
`tagIndexForRow(row)` — long-pressing Done must not offer a delete.

**`handleButtons()` (`:316-329`)** — missing from v1 entirely. Line 323 reads
`if (selected < tagCount && ...) onRowLongPress(selected);`, comparing a row
index against a tag count: with Done inserted, a held Confirm on Done enters the
delete path and the last tag can never be deleted by button. Replace the guard
with `if (tagIndexForRow(selected) >= 0 && ...)`.

**Do not touch** `onEnter` (`:36-38`), `toggleTag`, `commitAndFinish` (`:216-217`),
`showDeleteConfirmation`, `deleteTag`, or the `selected_` shift at `:274` — all are
tag-index space already and a row offset applied there would be a second bug.

- [ ] **Step 4: Correct the footer**

`drawFooter` (`:45-50`) advertises only the discarding exit. Change the second
label from `tr(STR_TOGGLE)` to `tr(STR_SELECT)`. `STR_DONE` (`english.yaml:153`)
and `STR_SELECT` (`:237`) both already exist — **no i18n regeneration.**

- [ ] **Step 5: Build both boards and commit**

```bash
pio run -e x4pro
pio run -e default
git commit -am "fix(highlights): commit tags from an explicit Done row"
```

---

### Task 7: verification

- [ ] **Step 1: Full suite and both boards**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

Expected **234** host tests; both SUCCESS. Sequential `pio` runs.

- [ ] **Step 2: Greps that can actually fail**

```bash
grep -n "selected < tagCount" src/activities/reader/TagPickerActivity.cpp
grep -c "tagIndexForRow" src/activities/reader/TagPickerActivity.cpp
grep -n "snapshotValid = false" src/activities/reader/PassageSelectActivity.cpp
grep -n "Phase::PickingStart" src/activities/reader/PassageSelectActivity.cpp
```

The first must return **no** hits. The second must be **>= 4** (declaration,
`activateIndex`, `onRowLongPress`, `handleButtons`). The third must include the
line inside `advancePage`. The fourth must still appear in `drawSelectionOutline`.

- [ ] **Step 3: Device self-check against stored offsets**

The host tests exercise the counter against fixtures; this exercises it against
the book. Temporarily log, for the current spine item, `VerseAnchors::scan`'s
running offset at each page boundary versus the `visibleTextOffset` the section
cache already stores per page. They must match exactly. This is the one check
that would have caught v1's entity and case bugs, and it uses production data.

Remove the logging before committing.

- [ ] **Step 4: Regenerate the 56 imported labels**

Imported entries carry reference-only labels; new ones carry `reference · passage`.
Re-run the import with snippets and re-upload. Reproducible from the `.jwlibrary`
backup.

- [ ] **Step 5: Device checks**

1. Highlight a passage split by a page break: swipe right-to-left mid-selection, confirm on the next page, verify the saved highlight covers **both** parts and that the page actually repaints.
2. Confirm a new label reads `<book> <chapter>:<verse> · <passage>` and names the **right** verse — check one immediately after a verse boundary.
3. Pick tags, leave via **Done** — tags persist. Pick tags, leave via **Back** — they do not.
4. Hold Confirm on the Done row: nothing must be deleted.
5. Open a non-Bible EPUB, highlight, confirm the label is passage text with no stray separator.

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
