# Highlights Data Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store a book's tagged highlights durably and correctly — the format, the tag palette, and every rule that prevents silent data loss — with the logic host-tested.

**Architecture:** Split into a pure, dependency-free document core (`HighlightDoc`) that is fully host-testable, and a thin I/O shell (`HighlightFile`) that touches `Storage`. Same pattern as `DocReadStatus` / `PersistableStore`, and it is what makes this plan verifiable without hardware.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest, ArduinoJson 7.4.2.

**Depends on:** foundations plan (complete) and anchoring plan (complete).

**Delivery:** fork-only.

---

## Scope: why this plan has no UI

The physical X4 Pro is a week away. This plan deliberately contains **no activities, no selection, and no render pass**, because all three need on-device verification — an inverted block's ghosting, the grayscale passes, and touch selection cannot be judged from a build log.

What it does contain is where the data-loss risk lives, and almost all of it is pure logic:

| Risk | Host-testable? |
| --- | --- |
| Tag index renumbering on delete | Yes |
| Format round-trip and forward compatibility | Yes |
| UTF-8-safe label truncation | Yes |
| Highlight count cap before the 50 KB read limit | Yes |
| Atomic save and `.tmp` recovery | Partly — logic yes, real SD no |

The UI plan follows once hardware arrives.

## Inherited constraints this plan must honour

- **`SDCardManager::readFile` truncates at 50,000 bytes silently** (`SDCardManager.cpp:202-204`). A truncated prefix fails to parse. Cap the highlight count *before* that with a user-visible error.
- **Only `DocReadStatus::Missing` is safe to overwrite.** `readDocFromFileChecked` (added in the foundations plan) distinguishes missing from unreadable/unparseable. Overwriting on a parse failure destroys the file.
- **`writeDocToFileAtomic` leaves an orphaned `.tmp` if the rename fails**, and nothing in the codebase reads a `.tmp`. For annotations that is the only surviving copy. `HighlightFile::load` must try `<path>.tmp` when the primary reads `Missing`.
- **`BookmarkUtil::sanitizeBookmarkSummary` caps at 72 *bytes*, not characters** (`BookmarkUtil.cpp:33`), and `resize(72)` can split a UTF-8 sequence. Highlighted passages are far likelier to be non-ASCII than a page's first words. `Utf8.h` provides `utf8SafeTruncateBuffer(const char*, int)`.
- **`BookmarkUtil::getBookmarkPath` truncates at any dot in the *path***, not just the extension (`BookmarkUtil.cpp:8-19`), so `/v1.0/mybook` → `"v1"`. Share the helper rather than copying the bug twice.
- **Offsets are absolute `uint32`** per word (anchoring plan), and because of NFC drift they must be recorded from stored offsets, never recomputed from source text.

## File format

`/.crosspoint/highlights/<flattened-book-path>.json`

```json
{
  "v": 1,
  "tags": ["greek", "wt-study"],
  "highlights": [
    { "si": 3, "start": 9412, "end": 9598, "t": [0, 1], "text": "the spirit of Jehovah is upon me" }
  ]
}
```

- `v` — format version, so future changes are detectable rather than guessed.
- `tags` — the book's palette, ordered. Highlights reference tags by **index**.
- `si` / `start` / `end` — spine index and a half-open absolute offset range (`VisibleRange`).
- `t` — indices into `tags`.
- `text` — display label only, never used to locate a highlight.

Read every scalar with the `obj["k"] | default` idiom. Arrays need `.as<JsonArray>()` — `|` does not work for them, and a missing key yields an array that iterates zero times.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/HighlightEntry.h` (create) | Plain struct, one highlight |
| `lib/Epub/Epub/HighlightDoc.h` / `.cpp` (create) | Pure document core: palette ops, renumbering, JSON build/parse, caps |
| `src/util/PathFlatten.h` / `.cpp` (create) | Shared book-path → filename helper |
| `src/util/BookmarkUtil.cpp` (modify) | Use the shared helper; fix byte-truncation |
| `src/util/HighlightFile.h` / `.cpp` (create) | I/O shell: atomic save, checked read, `.tmp` recovery |
| `test/highlight_doc/` (create) | Host tests for the document core |
| `test/path_flatten/` (create) | Host tests for flattening and truncation |
| `test/CMakeLists.txt` (modify) | Register both suites; add ArduinoJson via FetchContent |

---

### Task 1: Make ArduinoJson available to host tests

`HighlightDoc` builds and parses a `JsonDocument`, so the host harness needs ArduinoJson. It is header-only and portable, but currently only reaches the firmware build via PlatformIO.

- [ ] **Step 1: Add it via FetchContent, pinned to the firmware's version**

`platformio.ini:146` pins `bblanchon/ArduinoJson @ 7.4.2`. Match it exactly so host and device cannot diverge. In `test/CMakeLists.txt`, after the googletest `FetchContent_MakeAvailable`:

```cmake
FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)
```

Do **not** point at `.pio/libdeps/` — that would make the host suite depend on a firmware build having run first.

- [ ] **Step 2: Prove it compiles on the host**

Write a throwaway `test/highlight_doc/` suite containing only:

```cpp
#include <gtest/gtest.h>
#include <ArduinoJson.h>

TEST(ArduinoJsonHost, RoundTripsAScalar) {
  JsonDocument doc;
  doc["n"] = 42;
  std::string out;
  serializeJson(doc, out);
  EXPECT_EQ(out, "{\"n\":42}");

  JsonDocument parsed;
  ASSERT_FALSE(deserializeJson(parsed, out));
  EXPECT_EQ(parsed["n"] | 0, 42);
}
```

with a `CMakeLists.txt` linking `ArduinoJson` alongside `crosspoint_test_common` and `GTest::gtest_main`.

```bash
cmake -S test -B build/test
cmake --build build/test --target HighlightDocTest
ctest --test-dir build/test --output-on-failure -R ArduinoJsonHost
```

Expected: PASS. **If ArduinoJson does not build on the host, STOP and report** — the rest of this plan assumes it does, and the fallback (testing only non-JSON logic) changes the task breakdown.

- [ ] **Step 3: Commit**

```bash
git add test/CMakeLists.txt test/highlight_doc
git commit -m "test: make ArduinoJson available to the host suite"
```

---

### Task 2: Shared path flattening and UTF-8-safe truncation

`BookmarkUtil` owns two behaviours highlights need. Extract rather than copy — copying would duplicate a known bug.

**Files:**
- Create: `src/util/PathFlatten.h`, `src/util/PathFlatten.cpp`
- Modify: `src/util/BookmarkUtil.cpp`
- Create: `test/path_flatten/PathFlattenTest.cpp`, `test/path_flatten/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests first**

```cpp
// test/path_flatten/PathFlattenTest.cpp
#include <gtest/gtest.h>

#include "util/PathFlatten.h"

TEST(PathFlatten, FlattensSlashesAndDropsTheExtension) {
  EXPECT_EQ(pathflatten::toCacheName("/books/novel.epub"), "books_novel");
  EXPECT_EQ(pathflatten::toCacheName("/a/b/c.txt"), "a_b_c");
}

TEST(PathFlatten, TruncatesAtAnyDotInThePath) {
  // Inherited behaviour from BookmarkUtil: find_last_of('.') runs on the
  // FLATTENED name, so a dot in a directory truncates it. Locked in by test so
  // that changing it is a deliberate act, not an accident.
  EXPECT_EQ(pathflatten::toCacheName("/v1.0/mybook"), "v1");
}

TEST(PathFlatten, DifferentExtensionsCollide) {
  // Also inherited: the extension is dropped, so these share one cache file.
  EXPECT_EQ(pathflatten::toCacheName("/books/x.epub"), pathflatten::toCacheName("/books/x.txt"));
}

TEST(SafeSummary, CollapsesWhitespaceAndTrims) {
  EXPECT_EQ(pathflatten::safeSummary("  the   spirit  of  Jehovah "), "the spirit of Jehovah");
}

TEST(SafeSummary, StripsNewlines) {
  EXPECT_EQ(pathflatten::safeSummary("first\nsecond"), "firstsecond");
}

TEST(SafeSummary, NeverSplitsAUtf8Sequence) {
  // 40 Greek letters = 80 bytes, so a naive 72-byte resize lands mid-sequence.
  std::string greek;
  for (int i = 0; i < 40; ++i) greek += "α";  // 2 bytes each
  const std::string out = pathflatten::safeSummary(greek);
  EXPECT_LE(out.size(), 72u);
  EXPECT_EQ(out.size() % 2, 0u) << "a 2-byte sequence must not be cut in half";
  // Every byte must be either an ASCII byte or part of a complete sequence:
  // a trailing lone continuation byte (0b10xxxxxx) at the end proves a split.
  if (!out.empty()) {
    const unsigned char last = static_cast<unsigned char>(out.back());
    EXPECT_FALSE((last & 0xC0) == 0x80 && (out.size() % 2) != 0);
  }
}

TEST(SafeSummary, LeavesShortAsciiUnchanged) {
  EXPECT_EQ(pathflatten::safeSummary("short"), "short");
}
```

- [ ] **Step 2: Register the suite**

```cmake
# test/path_flatten/CMakeLists.txt
add_executable(PathFlattenTest
  PathFlattenTest.cpp
  ${REPO_ROOT}/src/util/PathFlatten.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(PathFlattenTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(PathFlattenTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PathFlattenTest)
```

Add `add_subdirectory(path_flatten)` to `test/CMakeLists.txt`.

- [ ] **Step 3: Run and confirm failure**

```bash
cmake -S test -B build/test
cmake --build build/test --target PathFlattenTest
```

Expected: FAIL — `util/PathFlatten.h` does not exist.

- [ ] **Step 4: Write the header**

```cpp
// src/util/PathFlatten.h
#pragma once

#include <string>

// Book-path helpers shared by the bookmark and highlight stores. Free of Arduino
// and storage dependencies so the rules can be unit tested on the host.
namespace pathflatten {

// Maps a book path to the flat filename stem used under /.crosspoint/<store>/.
// Strips the leading slash, replaces path separators with '_', and drops
// everything after the last '.'.
//
// Two inherited quirks, preserved deliberately so bookmarks and highlights agree:
// the extension is dropped, so /books/x.epub and /books/x.txt collide; and the
// last '.' is sought in the FLATTENED name, so a dot in a directory truncates it
// (/v1.0/mybook -> "v1"). Changing either would orphan every existing bookmark.
std::string toCacheName(const std::string& bookPath);

// Normalises a passage into a display label: collapses runs of whitespace,
// strips newlines, trims, and truncates to at most 72 bytes WITHOUT splitting a
// UTF-8 sequence. The byte cap matches BookmarkUtil; the codepoint safety does
// not — highlighted passages are far likelier to be non-ASCII than a page's
// first words, and a split sequence renders as a replacement character.
std::string safeSummary(std::string passage);

}  // namespace pathflatten
```

- [ ] **Step 5: Implement**

Port the logic from `BookmarkUtil.cpp:8-19` (flattening) and `:21-34` (summary), with two corrections in `safeSummary`:

- use `utf8SafeTruncateBuffer(str.data(), 72)` from `lib/Utf8/Utf8.h` instead of `resize(72)`;
- cast to `unsigned char` before `std::isspace` — `BookmarkUtil.cpp:23` passes a possibly-negative `char`, which is undefined behaviour for any UTF-8 continuation byte (lines 27 and 29 already cast correctly).

- [ ] **Step 6: Run and confirm the tests pass**

```bash
cmake --build build/test --target PathFlattenTest
ctest --test-dir build/test --output-on-failure -R 'PathFlatten|SafeSummary'
```

Expected: PASS, 7 tests.

- [ ] **Step 7: Point `BookmarkUtil` at the shared helper**

Rewrite `BookmarkUtil::getBookmarkPath` to call `pathflatten::toCacheName`, and `sanitizeBookmarkSummary` to call `pathflatten::safeSummary`. Behaviour for bookmarks is unchanged except that the UTF-8 split and the `isspace` UB are now fixed — both are strict improvements, and existing bookmark files stay readable because the *path* rule is byte-identical.

- [ ] **Step 8: Verify and commit**

```bash
pio run -e x4pro
cmake --build build/test && ctest --test-dir build/test -j
```

```bash
git add src/util/PathFlatten.h src/util/PathFlatten.cpp src/util/BookmarkUtil.cpp test/path_flatten test/CMakeLists.txt
git commit -m "refactor(util): share book-path flattening, fix UTF-8 label truncation

Extracts the path rule and the summary rule from BookmarkUtil so the
highlight store reuses them instead of copying a known quirk. Fixes a
72-byte resize that could split a UTF-8 sequence, and an isspace call
on a possibly-negative char."
```

---

### Task 3: `HighlightEntry` and the pure document core

**Files:**
- Create: `lib/Epub/Epub/HighlightEntry.h`
- Create: `lib/Epub/Epub/HighlightDoc.h`, `lib/Epub/Epub/HighlightDoc.cpp`
- Modify: `test/highlight_doc/HighlightDocTest.cpp` (replace Task 1's throwaway test)
- Modify: `test/highlight_doc/CMakeLists.txt`

- [ ] **Step 1: Write the entry struct**

```cpp
// lib/Epub/Epub/HighlightEntry.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "VisibleRange.h"

// One highlighted passage. The range is absolute visible-codepoint offsets within
// the spine item, so it survives re-pagination; `label` is display-only and must
// never be used to locate the passage.
struct HighlightEntry {
  uint16_t spineIndex = 0;
  VisibleRange range;
  std::vector<uint16_t> tagIndices;  // indices into HighlightDoc's palette
  std::string label;
};
```

- [ ] **Step 2: Write the failing tests**

```cpp
// test/highlight_doc/HighlightDocTest.cpp
#include <gtest/gtest.h>

#include "Epub/HighlightDoc.h"

namespace {

HighlightEntry makeEntry(const uint16_t spine, const uint32_t start, const uint32_t end,
                         std::vector<uint16_t> tags = {}) {
  HighlightEntry e;
  e.spineIndex = spine;
  e.range = VisibleRange{start, end};
  e.tagIndices = std::move(tags);
  e.label = "label";
  return e;
}

}  // namespace

TEST(HighlightDoc, RoundTripsAnEntry) {
  HighlightDoc doc;
  doc.addTag("greek");
  doc.addHighlight(makeEntry(3, 9412, 9598, {0}));

  const std::string json = doc.toJson();
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(json));

  ASSERT_EQ(parsed.highlights().size(), 1u);
  EXPECT_EQ(parsed.highlights()[0].spineIndex, 3);
  EXPECT_EQ(parsed.highlights()[0].range.start, 9412u);
  EXPECT_EQ(parsed.highlights()[0].range.end, 9598u);
  ASSERT_EQ(parsed.tags().size(), 1u);
  EXPECT_EQ(parsed.tags()[0], "greek");
  ASSERT_EQ(parsed.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(parsed.highlights()[0].tagIndices[0], 0);
}

TEST(HighlightDoc, RoundTripsAnEmptyDocument) {
  HighlightDoc doc;
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc.toJson()));
  EXPECT_TRUE(parsed.tags().empty());
  EXPECT_TRUE(parsed.highlights().empty());
}

TEST(HighlightDoc, RoundTripsAnUntaggedHighlight) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(0, 10, 20));
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc.toJson()));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  EXPECT_TRUE(parsed.highlights()[0].tagIndices.empty());
}

TEST(HighlightDoc, IgnoresUnknownFutureKeys) {
  const std::string future =
      R"({"v":1,"tags":["a"],"highlights":[{"si":1,"start":5,"end":9,"t":[0],"text":"x","colour":"red"}],"extra":7})";
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(future));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  EXPECT_EQ(parsed.highlights()[0].range.start, 5u);
}

TEST(HighlightDoc, RejectsMalformedJson) {
  HighlightDoc parsed;
  EXPECT_FALSE(parsed.fromJson("{not json"));
}

TEST(HighlightDoc, TruncatesTheLabelSafely) {
  HighlightDoc doc;
  HighlightEntry e = makeEntry(0, 0, 10);
  for (int i = 0; i < 60; ++i) e.label += "α";  // 120 bytes of Greek
  doc.addHighlight(e);
  EXPECT_LE(doc.highlights()[0].label.size(), 72u);
  EXPECT_EQ(doc.highlights()[0].label.size() % 2, 0u) << "must not split a 2-byte sequence";
}

TEST(HighlightDoc, RefusesToExceedTheEntryCap) {
  HighlightDoc doc;
  for (int i = 0; i < HighlightDoc::MAX_HIGHLIGHTS; ++i) {
    ASSERT_TRUE(doc.addHighlight(makeEntry(0, i * 10, i * 10 + 5))) << "at i=" << i;
  }
  EXPECT_FALSE(doc.addHighlight(makeEntry(0, 999999, 1000000)))
      << "the cap exists because SDCardManager::readFile silently truncates at 50KB";
  EXPECT_EQ(doc.highlights().size(), static_cast<size_t>(HighlightDoc::MAX_HIGHLIGHTS));
}

TEST(HighlightDoc, SerializedFormStaysUnderTheReadCap) {
  HighlightDoc doc;
  for (int i = 0; i < HighlightDoc::MAX_HIGHLIGHTS; ++i) {
    HighlightEntry e = makeEntry(0, i * 10, i * 10 + 5);
    e.label = std::string(72, 'x');  // worst-case label
    doc.addHighlight(e);
  }
  EXPECT_LT(doc.toJson().size(), 50000u)
      << "a full document must still be readable through SDCardManager's 50KB cap";
}
```

- [ ] **Step 3: Update the suite's CMakeLists**

```cmake
add_executable(HighlightDocTest
  HighlightDocTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/HighlightDoc.cpp
  ${REPO_ROOT}/src/util/PathFlatten.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(HighlightDocTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(HighlightDocTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(HighlightDocTest)
```

- [ ] **Step 4: Run and confirm failure**

```bash
cmake --build build/test --target HighlightDocTest
```

Expected: FAIL — `Epub/HighlightDoc.h` does not exist.

- [ ] **Step 5: Write the header**

```cpp
// lib/Epub/Epub/HighlightDoc.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "HighlightEntry.h"

// A book's highlights and its tag palette, with all format rules and no I/O.
// Dependency-free apart from ArduinoJson so every rule here is host-testable —
// the storage shell lives in src/util/HighlightFile.
class HighlightDoc {
 public:
  // Chosen so a full document with worst-case labels still serialises under
  // SDCardManager::readFile's silent 50,000-byte truncation. Exceeding that cap
  // makes the file read as empty and the next save destroys it.
  static constexpr int MAX_HIGHLIGHTS = 300;
  static constexpr int MAX_TAGS = 64;
  static constexpr int FORMAT_VERSION = 1;

  const std::vector<std::string>& tags() const { return tags_; }
  const std::vector<HighlightEntry>& highlights() const { return highlights_; }

  // Returns the tag's index, adding it if new. Returns -1 if the palette is full.
  int addTag(const std::string& name);

  // Removes tag `index` and renumbers every highlight's references so they still
  // point at the same tag names. Highlights referencing the removed tag lose that
  // reference only.
  void removeTag(uint16_t index);

  // Returns false when the cap is reached; the entry is not stored.
  bool addHighlight(HighlightEntry entry);

  std::string toJson() const;
  bool fromJson(const std::string& json);

 private:
  std::vector<std::string> tags_;
  std::vector<HighlightEntry> highlights_;
};
```

- [ ] **Step 6: Implement, then run the tests**

`addHighlight` applies `pathflatten::safeSummary` to the label before storing. `toJson`/`fromJson` use the format above with the `| default` idiom for scalars and `.as<JsonArray>()` for arrays.

```bash
cmake --build build/test --target HighlightDocTest
ctest --test-dir build/test --output-on-failure -R HighlightDoc
```

Expected: PASS, 8 tests. **If `SerializedFormStaysUnderTheReadCap` fails, lower `MAX_HIGHLIGHTS` until it passes** — that test defines the cap, not the other way round. Report the final number.

- [ ] **Step 7: Commit**

```bash
git add lib/Epub/Epub/HighlightEntry.h lib/Epub/Epub/HighlightDoc.h lib/Epub/Epub/HighlightDoc.cpp test/highlight_doc
git commit -m "feat(epub): add the pure highlight document core"
```

---

### Task 4: Tag palette renumbering

The sharpest data-integrity edge in the feature: highlights reference tags by index, so deleting a tag must renumber every reference or tags silently shift onto the wrong highlights.

**Files:**
- Modify: `test/highlight_doc/HighlightDocTest.cpp`
- Modify: `lib/Epub/Epub/HighlightDoc.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(HighlightDocTags, DeletingAMiddleTagKeepsEveryReferenceOnItsName) {
  HighlightDoc doc;
  doc.addTag("alpha");   // 0
  doc.addTag("beta");    // 1
  doc.addTag("gamma");   // 2

  HighlightEntry a = makeEntry(0, 0, 10, {0});
  HighlightEntry c = makeEntry(0, 20, 30, {2});
  HighlightEntry both = makeEntry(0, 40, 50, {0, 2});
  doc.addHighlight(a);
  doc.addHighlight(c);
  doc.addHighlight(both);

  doc.removeTag(1);  // drop "beta"

  ASSERT_EQ(doc.tags().size(), 2u);
  EXPECT_EQ(doc.tags()[0], "alpha");
  EXPECT_EQ(doc.tags()[1], "gamma");

  // Every highlight must still resolve to the SAME NAME it had before.
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
  EXPECT_EQ(doc.tags()[doc.highlights()[1].tagIndices[0]], "gamma");
  ASSERT_EQ(doc.highlights()[2].tagIndices.size(), 2u);
  EXPECT_EQ(doc.tags()[doc.highlights()[2].tagIndices[0]], "alpha");
  EXPECT_EQ(doc.tags()[doc.highlights()[2].tagIndices[1]], "gamma");
}

TEST(HighlightDocTags, DeletingATagDropsOnlyItsOwnReferences) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addHighlight(makeEntry(0, 0, 10, {0, 1}));

  doc.removeTag(0);

  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "beta");
}

TEST(HighlightDocTags, DeletingTheLastTagLeavesHighlightsUntagged) {
  HighlightDoc doc;
  doc.addTag("only");
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.removeTag(0);
  EXPECT_TRUE(doc.tags().empty());
  EXPECT_TRUE(doc.highlights()[0].tagIndices.empty());
}

TEST(HighlightDocTags, RemovingAnOutOfRangeIndexIsANoOp) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.removeTag(7);
  ASSERT_EQ(doc.tags().size(), 1u);
  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
}

TEST(HighlightDocTags, AddingAnExistingTagReturnsTheSameIndex) {
  HighlightDoc doc;
  const int first = doc.addTag("greek");
  const int again = doc.addTag("greek");
  EXPECT_EQ(first, again);
  EXPECT_EQ(doc.tags().size(), 1u);
}

TEST(HighlightDocTags, RenumberingSurvivesARoundTrip) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addTag("gamma");
  doc.addHighlight(makeEntry(0, 0, 10, {2}));
  doc.removeTag(0);

  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc.toJson()));
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "gamma");
}
```

- [ ] **Step 2: Run and confirm they fail**

```bash
cmake --build build/test --target HighlightDocTest
ctest --test-dir build/test --output-on-failure -R HighlightDocTags
```

Expected: FAIL (`removeTag` is unimplemented or does not renumber).

- [ ] **Step 3: Implement `removeTag`**

Erase the tag, then for every highlight: drop references equal to the removed index, and decrement references greater than it. Do not renumber by rebuilding from names — the indices are the source of truth in the file.

- [ ] **Step 4: Run and confirm they pass, then run the full suite**

```bash
ctest --test-dir build/test --output-on-failure -j
```

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/HighlightDoc.cpp test/highlight_doc/HighlightDocTest.cpp
git commit -m "feat(epub): renumber tag references when a tag is deleted"
```

---

### Task 5: `HighlightFile` — the I/O shell

Thin by design: every rule that can be tested lives in `HighlightDoc`; this layer only moves bytes.

**Files:**
- Create: `src/util/HighlightFile.h`, `src/util/HighlightFile.cpp`

- [ ] **Step 1: Write the header**

```cpp
// src/util/HighlightFile.h
#pragma once

#include <string>

#include "Epub/HighlightDoc.h"

// Per-book highlight persistence. All format rules live in HighlightDoc; this
// layer only reads and writes bytes, and enforces the two durability rules that
// need storage knowledge.
namespace HighlightFile {

// Loads the highlights for bookPath.
//
// Returns false ONLY when the data could not be read and may still exist —
// never overwrite after a false return. A genuinely absent file yields an empty
// doc and returns true.
//
// If the primary path is absent, the orphaned temp file is tried: a failed
// rename inside writeDocToFileAtomic leaves <path>.tmp as the only surviving
// copy, and nothing else in the codebase reads a .tmp.
bool load(const std::string& bookPath, HighlightDoc& doc);

// Saves atomically (temp file, then rename). Returns false on any failure.
bool save(const std::string& bookPath, const HighlightDoc& doc);

}  // namespace HighlightFile
```

- [ ] **Step 2: Implement**

`load` uses `PersistableStoreBase::readDocFromFileChecked` and switches on `DocReadStatus`:

- `Ok` → parse and return true
- `Missing` → try `<path>.tmp`; if that parses, return true with its content (and let the next `save` promote it); otherwise empty doc, return true
- `Unreadable` / `ParseError` → **return false**, leaving the file untouched

`save` uses `PersistableStoreBase::writeDocToFileAtomic`, with the path from `pathflatten::toCacheName` under `/.crosspoint/highlights/`.

- [ ] **Step 3: Verify and commit**

```bash
pio run -e x4pro
pio run -e default
cmake --build build/test && ctest --test-dir build/test -j
```

```bash
git add src/util/HighlightFile.h src/util/HighlightFile.cpp
git commit -m "feat(util): add per-book highlight persistence

Never overwrites a file that failed to parse, and recovers an orphaned
.tmp left by a failed atomic rename."
```

---

### Task 6: Verification

- [ ] **Step 1: Full suite and both boards**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

Expected: all suites pass — the anchoring branch ended at 158, plus 7 path/summary, 8 document, 6 tag tests.

- [ ] **Step 2: Confirm the deliberate dead surface**

```bash
grep -rn "HighlightFile\|HighlightDoc" src lib --include=*.cpp | grep -v "HighlightDoc.cpp\|HighlightFile.cpp"
```

Expected: no hits. Nothing consumes this yet — the UI plan does.

---

## What this plan deliberately does not do

- **No activities, no selection, no render pass.** All need on-device verification; hardware is a week out.
- **No menu entries.** The cap was raised to 24 in the foundations plan, but adding entries belongs with the UI.
- **No cross-book tag index.** Out of scope by design — tags are per-book.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Tag renumbering corrupts assignments | High | Task 4 is six tests aimed squarely at it, including a round-trip |
| Document exceeds the 50 KB read cap | High | `MAX_HIGHLIGHTS` derived from a test that serialises worst-case labels |
| Overwriting a file that failed to parse | High | `load` returns false and callers must not save |
| Orphaned `.tmp` is the only surviving copy | Medium | `load` tries it when the primary is missing |
| ArduinoJson unavailable on host | Medium | Task 1 proves it before anything depends on it |
| `BookmarkUtil` refactor regresses bookmarks | Medium | Path rule byte-identical; only UTF-8 and UB fixes change behaviour |
