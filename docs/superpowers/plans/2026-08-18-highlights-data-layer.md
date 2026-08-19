# Highlights Data Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store a book's tagged highlights durably and correctly — the format, the tag palette, and every rule that prevents silent data loss — with the logic host-tested.

**Architecture:** A pure document core (`HighlightDoc`, in `lib/`) holding every format rule, and a thin I/O shell (`HighlightFile`, in `src/`) that touches storage. **Bytes, not entry counts, are the safety invariant.**

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest, ArduinoJson 7.4.2 (`platformio.ini:141`).

**Depends on:** foundations and anchoring plans (both complete). Baseline is **166 host tests** at `0427caa6`.

**Delivery:** fork-only.

> **v2 — revised after adversarial review.** v1 did not compile (a `lib/` → `src/` include), gave `HighlightDoc` a string API that `writeDocToFileAtomic` cannot consume, set an entry cap measured at **195% over** the limit it claimed to respect, and "fixed" UTF-8 truncation with a call that neither truncates nor stays in bounds. Every one of those is corrected below, with the reasoning that actually holds rather than the one v1 asserted.

---

## Scope: why this plan has no UI

The device is a week out. No activities, no selection, no render pass — all three need on-device verification. What is here is where the data-loss risk lives, and nearly all of it is pure logic.

## Hard constraints, verified

- **`lib/` must never include from `src/`.** PlatformIO compiles each `lib/<Name>` as an independent static library and `src/` is never on its include path; **zero** files under `lib/` do this today. Cross-`lib` includes use the angle form (`lib/Epub/Epub/Page.cpp:5`, `#include <Serialization.h>`). A `lib/` → `src/` include is a hard compile failure that the host CMake **hides**, because the host suites add `-I src`.
- **`PersistableStoreBase` takes a `JsonDocument`, not a string** (`PersistableStore.h:58,65`). And `PersistableStore.h:12-20` states why `serializeJson`/`deserializeJson` must stay out of individual stores: GCC emits ~0.5 KB of parser clones per translation unit, and keeping them centralised "is what makes the abstraction flash-neutral." `BookmarkFile.cpp` is the in-tree pattern — it builds a `JsonDocument`, never a string.
- **`utf8SafeTruncateBuffer(const char* buf, int len) -> int`** (`Utf8.cpp:145-163`) does **not** mutate, returns the safe length, and `len` is an *actual buffer length*, not a budget. Passing a budget larger than the string over-reads the heap.
- **`SDCardManager::readFile` truncates at 50,000 bytes silently** and builds the result one `char` at a time into an Arduino `String`.
- **`writeDocToFileAtomic` creates only `/.crosspoint`** (`PersistableStore.cpp:22-23`). The subdirectory is the caller's job, exactly as `BookmarkFile.cpp:59-60` does it.
- **Only `DocReadStatus::Missing` is safe to overwrite.**
- **A failed rename leaves `<path>.tmp` as the only surviving copy**, and nothing else in the codebase reads a `.tmp`.

## The byte budget: why entry caps are a hint, not the mechanism

v1 set `MAX_HIGHLIGHTS = 300` and claimed it was derived. Measured with real ArduinoJson, a genuinely worst-case document — `si=65535`, `uint32` offsets at full width, a full tag palette, maximum tag references, and a label of escapable characters — serialises to **97,459 bytes**, nearly double the 50,000 cap. v1's "derived" test measured 36,632 because it used `si=0`, tiny offsets, no tags and an empty palette; it passed with headroom and could never have fired the "lower the cap" instruction.

Entry counts cannot bound bytes when per-entry size varies by more than 5×. So:

- **`save` measures the serialised document and refuses if it exceeds the budget**, surfacing a user-visible error. That is the safety mechanism.
- **`MAX_HIGHLIGHTS` is a generous soft cap** set at 400, matching the ~430 the design spec calls reachable for the study-Bible use case. It exists to stop unbounded growth, not to guarantee the byte bound.
- Per-entry cost is additionally bounded by capping tag-name length and tags-per-highlight, so the budget is reachable in practice rather than only in theory.

`measureJson(doc)` gives the serialised size without materialising the string.

> **Known limitation:** the real ceiling is `SDCardManager::readFile`'s 50 KB `String`. `readFileToBuffer` (`SDCardManager.h:49`) already exists and reads in chunks; routing highlight loads through it would remove this constraint. Out of scope here, recorded as follow-up.

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

`v` is **validated on load** — a file whose version exceeds `FORMAT_VERSION` is rejected rather than reinterpreted. A version field that is written but never read is worse than none, because it creates false confidence at both ends: a v2 file that redefines `start`/`end` would be silently misread and then overwritten with `"v":1`.

Scalars use `obj["k"] | default`; arrays need `.as<JsonArray>()`. Per `PersistableStore.h:88-91`, do **not** write `obj["k"] | std::string("")`.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Utf8/Utf8.h` / `.cpp` (modify) | Add `utf8SafeSummary` — whitespace collapse, trim, UTF-8-safe byte cap |
| `src/util/PathFlatten.h` / `.cpp` (create) | Book path → cache filename stem (storage rule, `src`-only) |
| `src/util/BookmarkUtil.cpp` (modify) | Delegate to both helpers |
| `lib/Epub/Epub/HighlightEntry.h` (create) | One highlight |
| `lib/Epub/Epub/HighlightDoc.h` / `.cpp` (create) | Format rules, palette, validation, `JsonDocument` I/O |
| `src/util/HighlightFile.h` / `.cpp` (create) | Storage shell: status-returning load, atomic save, `.tmp` promotion |
| `test/utf8_summary/`, `test/path_flatten/`, `test/highlight_doc/`, `test/highlight_file/` (create) | Host suites |

**Layering:** `HighlightDoc` is in `lib/` and includes `<Utf8.h>` (another `lib/`), never `src/`. `utf8SafeSummary` belongs in `Utf8` on its merits — it is UTF-8-aware text normalisation, not a storage concern. `toCacheName` is a storage-path rule used only from `src/`.

---

### Task 1: `utf8SafeSummary` in `lib/Utf8`

**Files:**
- Modify: `lib/Utf8/Utf8.h`, `lib/Utf8/Utf8.cpp`
- Create: `test/utf8_summary/Utf8SummaryTest.cpp`, `test/utf8_summary/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// test/utf8_summary/Utf8SummaryTest.cpp
#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

TEST(Utf8SafeSummary, CollapsesWhitespaceAndTrims) {
  EXPECT_EQ(utf8SafeSummary("  the   spirit  of  Jehovah "), "the spirit of Jehovah");
}

TEST(Utf8SafeSummary, StripsNewlines) {
  EXPECT_EQ(utf8SafeSummary("first\nsecond"), "firstsecond");
}

TEST(Utf8SafeSummary, LeavesShortAsciiUnchanged) {
  EXPECT_EQ(utf8SafeSummary("short"), "short");
}

TEST(Utf8SafeSummary, NeverSplitsAMixedWidthSequence) {
  // 1 ASCII + 24x U+65E5 (3 bytes each) = 73 bytes. A naive resize(72) lands two
  // bytes into the 24th character; the only correct cut is at 70.
  //
  // Mixed width is essential: a homogeneous 2-, 3- or 4-byte string is invisible
  // to this bug, because 72 is divisible by 2, 3 and 4 alike.
  std::string mixed = "x";
  for (int i = 0; i < 24; ++i) mixed += "\xe6\x97\xa5";
  ASSERT_EQ(mixed.size(), 73u);

  std::string expected = "x";
  for (int i = 0; i < 23; ++i) expected += "\xe6\x97\xa5";
  ASSERT_EQ(expected.size(), 70u);

  EXPECT_EQ(utf8SafeSummary(mixed), expected);
}

TEST(Utf8SafeSummary, DoesNotOverReadAShortBuffer) {
  // Regression guard: an implementation that passes the 72-byte budget straight
  // to utf8SafeTruncateBuffer reads far past the end of a short string. Run the
  // suite under -fsanitize=address to make this bite.
  EXPECT_EQ(utf8SafeSummary("ab"), "ab");
  EXPECT_EQ(utf8SafeSummary(""), "");
}

TEST(Utf8SafeSummary, CapsAtSeventyTwoBytes) {
  EXPECT_LE(utf8SafeSummary(std::string(200, 'x')).size(), 72u);
  EXPECT_EQ(utf8SafeSummary(std::string(200, 'x')).size(), 72u);
}
```

- [ ] **Step 2: Register the suite**

```cmake
# test/utf8_summary/CMakeLists.txt
add_executable(Utf8SummaryTest
  Utf8SummaryTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(Utf8SummaryTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(Utf8SummaryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Utf8SummaryTest)
```

Add `add_subdirectory(utf8_summary)` to `test/CMakeLists.txt`.

- [ ] **Step 3: Run and confirm failure**

```bash
cmake -S test -B build/test
cmake --build build/test --target Utf8SummaryTest
```

Expected: FAIL — `utf8SafeSummary` is undeclared.

- [ ] **Step 4: Declare and implement**

In `lib/Utf8/Utf8.h`, beside the other helpers:

```cpp
// Normalises a passage into a display label: collapses runs of whitespace,
// strips newlines, trims, and caps at 72 bytes WITHOUT splitting a UTF-8
// sequence. The byte cap matches BookmarkUtil's historical behaviour; the
// codepoint safety does not — a split sequence renders as a replacement
// character, and highlighted passages are far likelier to be non-ASCII than a
// page's first words.
std::string utf8SafeSummary(std::string passage);
```

Implementation notes that matter:

```cpp
  // utf8SafeTruncateBuffer does NOT mutate and its `len` is an ACTUAL buffer
  // length, not a budget — passing 72 for a shorter string over-reads the heap.
  if (summary.size() > 72) {
    summary.resize(static_cast<size_t>(utf8SafeTruncateBuffer(summary.data(), 72)));
  }
```

Include `<cctype>` explicitly — `BookmarkUtil.cpp` gets `std::isspace` transitively through the Arduino chain, which does not hold in a host translation unit. Cast to `unsigned char` before every `std::isspace`: `BookmarkUtil.cpp:23` passes a possibly-negative `char`, which is UB for any UTF-8 continuation byte (lines 27 and 29 already cast correctly).

- [ ] **Step 5: Verify and commit**

```bash
cmake --build build/test --target Utf8SummaryTest
ctest --test-dir build/test --output-on-failure -R Utf8SafeSummary
pio run -e x4pro
```

Expected: 6 tests pass; firmware builds.

```bash
git add lib/Utf8/Utf8.h lib/Utf8/Utf8.cpp test/utf8_summary test/CMakeLists.txt
git commit -m "feat(utf8): add UTF-8-safe passage summary normalisation"
```

---

### Task 2: `PathFlatten` and the `BookmarkUtil` delegation

**Files:**
- Create: `src/util/PathFlatten.h`, `src/util/PathFlatten.cpp`
- Modify: `src/util/BookmarkUtil.cpp`
- Create: `test/path_flatten/PathFlattenTest.cpp`, `test/path_flatten/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// test/path_flatten/PathFlattenTest.cpp
#include <gtest/gtest.h>

#include "util/PathFlatten.h"

TEST(PathFlatten, FlattensSeparatorsAndDropsTheExtension) {
  EXPECT_EQ(pathflatten::toCacheName("/books/novel.epub"), "books_novel");
  EXPECT_EQ(pathflatten::toCacheName("/a/b/c.txt"), "a_b_c");
}

TEST(PathFlatten, ReplacesBackslashesToo) {
  EXPECT_EQ(pathflatten::toCacheName("/a\\b/c.epub"), "a_b_c");
}

TEST(PathFlatten, TruncatesAtAnyDotInThePath) {
  // Inherited: find_last_of('.') runs on the FLATTENED name, so a dot in a
  // directory truncates it. Locked in by test — changing it orphans every
  // existing bookmark file.
  EXPECT_EQ(pathflatten::toCacheName("/v1.0/mybook"), "v1");
}

TEST(PathFlatten, DifferentExtensionsCollide) {
  EXPECT_EQ(pathflatten::toCacheName("/books/x.epub"), pathflatten::toCacheName("/books/x.txt"));
}

TEST(PathFlatten, StripsTheFirstCharacterUnconditionally) {
  // erase(0,1) is unconditional, so a path without a leading slash loses a real
  // character. Documented rather than fixed, for the same compatibility reason.
  EXPECT_EQ(pathflatten::toCacheName("books/x.epub"), "ooks_x");
}
```

- [ ] **Step 2: Register the suite**

```cmake
# test/path_flatten/CMakeLists.txt
add_executable(PathFlattenTest
  PathFlattenTest.cpp
  ${REPO_ROOT}/src/util/PathFlatten.cpp
)

target_include_directories(PathFlattenTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PathFlattenTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PathFlattenTest)
```

Add `add_subdirectory(path_flatten)` to `test/CMakeLists.txt`.

- [ ] **Step 3: Run, confirm failure, then write the header and implementation**

`toCacheName` ports `BookmarkUtil.cpp:8-19` exactly: `erase(0,1)`, `'/'`→`'_'`, `'\\'`→`'_'`, `erase(find_last_of('.'))`. Byte-identical, or every existing bookmark is orphaned.

- [ ] **Step 4: Point `BookmarkUtil` at both helpers**

`getBookmarkPath` calls `pathflatten::toCacheName`; `sanitizeBookmarkSummary` calls `utf8SafeSummary`.

> **Why the summary change is safe for existing bookmarks** — and v1's reason was wrong. Path identity says nothing about summaries. The correct argument: a summary is sanitised exactly once at creation (`EpubReaderActivity.cpp:1708`, the only caller) and then persisted verbatim (`BookmarkFile.cpp:48` writes it, `:28` reads it back with `obj["summary"] | ""` and never re-sanitises). So the behaviour change affects only newly created bookmarks.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
```

```bash
git add src/util/PathFlatten.h src/util/PathFlatten.cpp src/util/BookmarkUtil.cpp test/path_flatten test/CMakeLists.txt
git commit -m "refactor(util): share the book-path rule, delegate summaries to Utf8"
```

---

### Task 3: ArduinoJson on the host

- [ ] **Step 1: Add it via FetchContent, pinned to `platformio.ini:141`**

In `test/CMakeLists.txt`, after googletest:

```cmake
FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)
```

Verified: the tag exists, and `src/CMakeLists.txt` at that tag does `add_library(ArduinoJson INTERFACE)`, so `target_link_libraries(... ArduinoJson ...)` resolves. Its own test suite is guarded behind a project-name check and will not be pulled in. Do **not** point at `.pio/libdeps/` — that would couple host tests to a firmware build.

Two known nits: ArduinoJson's `install()` rules are unguarded and attach to this project's install set (harmless for tests), and its includes are not `SYSTEM`, so its headers compile under `-Wall -Wextra -pedantic`.

- [ ] **Step 2: Commit** (the proof lands with Task 4's suite, which uses it)

```bash
git add test/CMakeLists.txt
git commit -m "test: make ArduinoJson available to the host suite"
```

---

### Task 4: `HighlightEntry` and `HighlightDoc`

**Files:**
- Create: `lib/Epub/Epub/HighlightEntry.h`, `lib/Epub/Epub/HighlightDoc.h`, `lib/Epub/Epub/HighlightDoc.cpp`
- Create: `test/highlight_doc/HighlightDocTest.cpp`, `test/highlight_doc/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: The entry**

```cpp
// lib/Epub/Epub/HighlightEntry.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "VisibleRange.h"

// One highlighted passage. The range is absolute visible-codepoint offsets within
// the spine item, so it survives re-pagination. `label` is display-only and must
// never be used to locate the passage.
struct HighlightEntry {
  uint16_t spineIndex = 0;
  VisibleRange range;
  std::vector<uint16_t> tagIndices;
  std::string label;
};
```

- [ ] **Step 2: The document interface**

```cpp
// lib/Epub/Epub/HighlightDoc.h
#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "HighlightEntry.h"

// A book's highlights and tag palette, with all format rules and no storage
// access. The storage shell is src/util/HighlightFile.
//
// Bytes, not entry counts, are the safety invariant: per-entry serialised size
// varies by more than 5x with offset width, tag references and label escaping,
// so HighlightFile::save measures the document and refuses when it exceeds the
// budget. The caps below bound growth; they do not guarantee the byte bound.
class HighlightDoc {
 public:
  static constexpr int FORMAT_VERSION = 1;
  static constexpr size_t MAX_HIGHLIGHTS = 400;      // ~the spec's reachable ceiling
  static constexpr size_t MAX_TAGS = 32;
  static constexpr size_t MAX_TAGS_PER_HIGHLIGHT = 8;
  static constexpr size_t MAX_TAG_NAME_BYTES = 24;

  const std::vector<std::string>& tags() const { return tags_; }
  const std::vector<HighlightEntry>& highlights() const { return highlights_; }

  // Adds the tag if new; returns its index, or nullopt when the palette is full
  // or the name is empty. Returns an optional rather than a sentinel because a
  // signed -1 stored into the uint16_t tagIndices becomes 65535 and then indexes
  // tags_ out of bounds.
  std::optional<uint16_t> addTag(const std::string& name);

  // Removes tag `index`, renumbering every reference so highlights still resolve
  // to the same tag NAMES. Out-of-range is a no-op.
  void removeTag(uint16_t index);

  bool addHighlight(HighlightEntry entry);
  bool removeHighlight(size_t index);

  // Highlights in the given spine item, in stored order. The render pass needs
  // this every page turn.
  std::vector<const HighlightEntry*> findBySpine(uint16_t spineIndex) const;

  void toJson(JsonDocument& doc) const;

  // Parses and VALIDATES. Rejects a future format version. Drops tag references
  // that fall outside the palette, truncates at MAX_HIGHLIGHTS, re-normalises
  // labels, and range-checks spine indices — a hand-edited or foreign file must
  // not be able to produce an out-of-bounds read later.
  bool fromJson(JsonVariantConst doc);

 private:
  std::vector<std::string> tags_;
  std::vector<HighlightEntry> highlights_;
};
```

The `JsonDocument` interface is required, not stylistic: `writeDocToFileAtomic`/`readDocFromFileChecked` take a `JsonDocument`, so a string API would force a serialise-then-reparse round trip on every load and save. It also keeps `serializeJson`/`deserializeJson` out of this translation unit, which `PersistableStore.h:12-20` documents as what makes the abstraction flash-neutral.

- [ ] **Step 3: Write the failing tests**

Host tests call `serializeJson`/`deserializeJson` themselves — flash cost is irrelevant in a test TU.

```cpp
// test/highlight_doc/HighlightDocTest.cpp
#include <gtest/gtest.h>

#include <ArduinoJson.h>

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

// Round-trips through real JSON text, so the test exercises serialisation too.
bool roundTrip(const HighlightDoc& in, HighlightDoc& out) {
  JsonDocument doc;
  in.toJson(doc);
  std::string text;
  serializeJson(doc, text);
  JsonDocument reparsed;
  if (deserializeJson(reparsed, text)) return false;
  return out.fromJson(reparsed);
}

}  // namespace

TEST(HighlightDoc, RoundTripsAnEntry) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("greek").has_value());
  doc.addHighlight(makeEntry(3, 9412, 9598, {0}));

  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(doc, parsed));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  EXPECT_EQ(parsed.highlights()[0].spineIndex, 3);
  EXPECT_EQ(parsed.highlights()[0].range.start, 9412u);
  EXPECT_EQ(parsed.highlights()[0].range.end, 9598u);
  ASSERT_EQ(parsed.tags().size(), 1u);
  EXPECT_EQ(parsed.tags()[0], "greek");
}

TEST(HighlightDoc, RoundTripsEmptyAndUntagged) {
  HighlightDoc empty;
  HighlightDoc parsedEmpty;
  ASSERT_TRUE(roundTrip(empty, parsedEmpty));
  EXPECT_TRUE(parsedEmpty.highlights().empty());

  HighlightDoc untagged;
  untagged.addHighlight(makeEntry(0, 10, 20));
  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(untagged, parsed));
  EXPECT_TRUE(parsed.highlights()[0].tagIndices.empty());
}

TEST(HighlightDoc, IgnoresUnknownKeysButRejectsAFutureVersion) {
  JsonDocument known;
  ASSERT_FALSE(deserializeJson(
      known, R"({"v":1,"tags":["a"],"highlights":[{"si":1,"start":5,"end":9,"t":[0],"text":"x","colour":"red"}]})"));
  HighlightDoc ok;
  EXPECT_TRUE(ok.fromJson(known)) << "unknown keys are forward-compatible";

  JsonDocument future;
  ASSERT_FALSE(deserializeJson(future, R"({"v":2,"tags":[],"highlights":[]})"));
  HighlightDoc rejected;
  EXPECT_FALSE(rejected.fromJson(future))
      << "a future version may redefine start/end; reinterpreting it then saving v1 destroys data";
}

TEST(HighlightDoc, DropsTagReferencesOutsideThePalette) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"v":1,"tags":["a"],"highlights":[{"si":0,"start":0,"end":5,"t":[0,7]}]})"));
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  ASSERT_EQ(parsed.highlights()[0].tagIndices.size(), 1u) << "index 7 has no tag and must not survive";
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "a");
}

TEST(HighlightDoc, ClampsASpineIndexThatWouldNarrow) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"v":1,"tags":[],"highlights":[{"si":70000,"start":0,"end":5}]})"));
  HighlightDoc parsed;
  parsed.fromJson(doc);
  for (const auto& h : parsed.highlights()) {
    EXPECT_NE(h.spineIndex, 4464) << "70000 must not silently wrap to 4464";
  }
}

TEST(HighlightDoc, NormalisesLabelsOnParse) {
  std::string longLabel(200, 'x');
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(
      doc, R"({"v":1,"tags":[],"highlights":[{"si":0,"start":0,"end":5,"text":")" + longLabel + R"("}]})"));
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc));
  EXPECT_LE(parsed.highlights()[0].label.size(), 72u) << "addHighlight normalises; fromJson must too";
}

TEST(HighlightDoc, RejectsMalformedJson) {
  JsonDocument doc;
  EXPECT_TRUE(deserializeJson(doc, "{not json"));
}

TEST(HighlightDoc, FindsHighlightsBySpine) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(1, 0, 5));
  doc.addHighlight(makeEntry(3, 0, 5));
  doc.addHighlight(makeEntry(1, 10, 15));
  EXPECT_EQ(doc.findBySpine(1).size(), 2u);
  EXPECT_EQ(doc.findBySpine(3).size(), 1u);
  EXPECT_TRUE(doc.findBySpine(9).empty());
}

TEST(HighlightDoc, RemovesAHighlight) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(0, 0, 5));
  doc.addHighlight(makeEntry(0, 10, 15));
  ASSERT_TRUE(doc.removeHighlight(0));
  ASSERT_EQ(doc.highlights().size(), 1u);
  EXPECT_EQ(doc.highlights()[0].range.start, 10u);
  EXPECT_FALSE(doc.removeHighlight(9));
}

TEST(HighlightDoc, EnforcesTheEntryCap) {
  HighlightDoc doc;
  for (size_t i = 0; i < HighlightDoc::MAX_HIGHLIGHTS; ++i) {
    ASSERT_TRUE(doc.addHighlight(makeEntry(0, i * 10, i * 10 + 5))) << "at i=" << i;
  }
  EXPECT_FALSE(doc.addHighlight(makeEntry(0, 999999, 1000000)));
}

TEST(HighlightDoc, TruncatesAnOverlongEntryListOnParse) {
  JsonDocument doc;
  JsonArray arr = doc["highlights"].to<JsonArray>();
  doc["v"] = 1;
  doc["tags"].to<JsonArray>();
  for (size_t i = 0; i < HighlightDoc::MAX_HIGHLIGHTS + 50; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["si"] = 0;
    o["start"] = i * 10;
    o["end"] = i * 10 + 5;
  }
  HighlightDoc parsed;
  parsed.fromJson(doc);
  EXPECT_LE(parsed.highlights().size(), HighlightDoc::MAX_HIGHLIGHTS)
      << "an oversized file must not load past the cap and then be unsaveable";
}

TEST(HighlightDoc, RejectsAnOverlongTagName) {
  HighlightDoc doc;
  EXPECT_FALSE(doc.addTag(std::string(HighlightDoc::MAX_TAG_NAME_BYTES + 1, 'x')).has_value());
}

TEST(HighlightDoc, AddingAnExistingTagReturnsTheSameIndex) {
  HighlightDoc doc;
  const auto first = doc.addTag("greek");
  const auto again = doc.addTag("greek");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, *again);
  EXPECT_EQ(doc.tags().size(), 1u);
}

TEST(HighlightDoc, WorstCaseDocumentStaysUnderTheSaveBudget) {
  // The genuine worst case, not a convenient one: maximum spine index, full-width
  // uint32 offsets, a full palette of maximum-length names, the maximum tag
  // references per entry, and a label made entirely of characters JSON escapes.
  HighlightDoc doc;
  for (size_t t = 0; t < HighlightDoc::MAX_TAGS; ++t) {
    ASSERT_TRUE(doc.addTag(std::string(HighlightDoc::MAX_TAG_NAME_BYTES, 'a' + static_cast<char>(t % 26)))
                    .has_value());
  }
  std::vector<uint16_t> refs;
  for (size_t i = 0; i < HighlightDoc::MAX_TAGS_PER_HIGHLIGHT; ++i) refs.push_back(static_cast<uint16_t>(i));

  for (size_t i = 0; i < HighlightDoc::MAX_HIGHLIGHTS; ++i) {
    HighlightEntry e;
    e.spineIndex = 65535;
    e.range = VisibleRange{4294967290u, 4294967295u};
    e.tagIndices = refs;
    e.label = std::string(72, '"');  // every byte escapes to two
    if (!doc.addHighlight(e)) break;
  }

  JsonDocument json;
  doc.toJson(json);
  const size_t bytes = measureJson(json);
  // Not an assertion that it fits — it does NOT, and that is the point. This
  // records the real worst case so the save-side byte guard is understood as the
  // safety mechanism and MAX_HIGHLIGHTS as a growth limit only.
  EXPECT_GT(bytes, 50000u) << "if this ever fits, the caps changed — revisit the guard's necessity";
}
```

- [ ] **Step 4: Register the suite**

```cmake
# test/highlight_doc/CMakeLists.txt
add_executable(HighlightDocTest
  HighlightDocTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/HighlightDoc.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(HighlightDocTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(HighlightDocTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(HighlightDocTest)
```

Add `add_subdirectory(highlight_doc)` to `test/CMakeLists.txt`.

Note the include path contains **no `${REPO_ROOT}/src`** — that absence is what stops a `lib/` → `src/` include from compiling on the host while breaking the firmware.

- [ ] **Step 5: Run failing, implement, run passing**

Use `static_cast<size_t>` on every cap comparison — `size()` is `size_t` and the caps are `size_t` here precisely to avoid the `-Wsign-compare` sites v1 would have created.

```bash
cmake --build build/test --target HighlightDocTest
ctest --test-dir build/test --output-on-failure -R HighlightDoc
pio run -e x4pro
```

**Run `pio run -e x4pro` in this task, not two tasks later** — it is the only thing that catches a layering violation.

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/HighlightEntry.h lib/Epub/Epub/HighlightDoc.h lib/Epub/Epub/HighlightDoc.cpp test/highlight_doc test/CMakeLists.txt
git commit -m "feat(epub): add the highlight document core with validating parse"
```

---

### Task 5: Tag renumbering

**Files:** modify `test/highlight_doc/HighlightDocTest.cpp` and `lib/Epub/Epub/HighlightDoc.cpp`

- [ ] **Step 1: Write the failing tests**

Six tests, asserting on **names** rather than indices — the invariant that matters:

```cpp
TEST(HighlightDocTags, DeletingAMiddleTagKeepsEveryReferenceOnItsName) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addTag("gamma");
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.addHighlight(makeEntry(0, 20, 30, {2}));
  doc.addHighlight(makeEntry(0, 40, 50, {0, 2}));

  doc.removeTag(1);

  ASSERT_EQ(doc.tags().size(), 2u);
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
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
}

TEST(HighlightDocTags, RenumberingSurvivesARoundTrip) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addTag("gamma");
  doc.addHighlight(makeEntry(0, 0, 10, {2}));
  doc.removeTag(0);

  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(doc, parsed));
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "gamma");
}

TEST(HighlightDocTags, DeletingEveryTagInSequenceNeverLeavesADanglingIndex) {
  HighlightDoc doc;
  doc.addTag("a");
  doc.addTag("b");
  doc.addTag("c");
  doc.addHighlight(makeEntry(0, 0, 10, {0, 1, 2}));
  while (!doc.tags().empty()) {
    doc.removeTag(0);
    for (const auto idx : doc.highlights()[0].tagIndices) {
      EXPECT_LT(static_cast<size_t>(idx), doc.tags().size()) << "dangling index after a delete";
    }
  }
  EXPECT_TRUE(doc.highlights()[0].tagIndices.empty());
}
```

- [ ] **Step 2: Run failing, implement, run passing, commit**

Erase the tag, then per highlight: drop references equal to the removed index and decrement those greater. Write the loop over `uint16_t` values so integral promotion makes both operands `int` — do not compare an `int` counter against `tagIndices.size()`.

```bash
git commit -m "feat(epub): renumber tag references when a tag is deleted"
```

---

### Task 6: `HighlightFile` with a storage fake

v1 shipped this layer with **zero tests** while its risk table rated its failures High. The repo already has the link-time-fake pattern (`test/stubs/HalStorage.h`, `test/pagination/GfxRendererFake.cpp`), so the `DocReadStatus` switch and the `.tmp` branch are testable.

**Files:**
- Create: `src/util/HighlightFile.h`, `src/util/HighlightFile.cpp`
- Create: `test/highlight_file/` (fake storage + suite)

- [ ] **Step 1: The interface**

```cpp
// src/util/HighlightFile.h
#pragma once

#include <string>

#include "Epub/HighlightDoc.h"

namespace HighlightFile {

enum class LoadResult : uint8_t {
  Loaded,              // file read and parsed
  Empty,               // genuinely absent — safe to save over
  RecoveredFromTemp,   // a failed rename left .tmp as the only copy; promoted
  Failed,              // unreadable or unparseable — DATA MAY STILL EXIST
};

// Loads the highlights for bookPath.
//
// Failed means the bytes could not be read or parsed and the file may still hold
// the user's data — the caller MUST NOT save. A status is returned rather than a
// bool because collapsing these four states into one is precisely the bug the
// foundations plan fixed in readDocFromFile.
LoadResult load(const std::string& bookPath, HighlightDoc& doc);

enum class SaveResult : uint8_t { Ok, TooLarge, WriteFailed };

// Saves atomically. Refuses when the serialised document exceeds the budget,
// so the caller can surface an error instead of writing a file that will read
// back truncated and unparseable forever.
SaveResult save(const std::string& bookPath, const HighlightDoc& doc);

// Serialised bytes above which save refuses. Headroom under SDCardManager's
// silent 50,000-byte read truncation.
constexpr size_t SAVE_BYTE_BUDGET = 45000;

}  // namespace HighlightFile
```

- [ ] **Step 2: Implementation requirements**

- `load` switches on `readDocFromFileChecked`: `Ok` → parse; `Missing` → try `<path>.tmp`; `Unreadable`/`ParseError` → `Failed`, touching nothing.
- **On a successful `.tmp` parse, promote it immediately** — `Storage.rename(tmp, final)` — and return `RecoveredFromTemp`. v1 said "let the next save promote it", which is unsound: the next save writes to that same `.tmp` first (`PersistableStore.cpp:30`), overwriting the only surviving copy in place. A `.tmp` that fails to parse is deleted.
- `save` must `Storage.mkdir("/.crosspoint/highlights/")` before writing. `writeDocToFileAtomic` creates only `/.crosspoint` (`PersistableStore.cpp:22-23`); `BookmarkFile.cpp:59-60` is the precedent.
- `save` checks `measureJson(doc) > SAVE_BYTE_BUDGET` and returns `TooLarge` without writing.

> **Concurrency:** `storeMutex` belongs to `PersistableStore<T>`; the static helpers take no lock, and `PersistableStore.h:26-30` documents that concurrent saves are reachable (web-server task vs. main task). This layer assumes a **single writer**. If the web server ever writes highlights, add a mutex — two concurrent saves would produce a torn `.tmp` and a remove/rename race.

- [ ] **Step 3: Tests against a fake storage**

Cover: absent file → `Empty`; good file → `Loaded`; unparseable → `Failed` **and the file is not modified**; primary absent with a valid `.tmp` → `RecoveredFromTemp` **and the `.tmp` is promoted**; primary absent with a corrupt `.tmp` → `Empty` and the `.tmp` is gone; oversized document → `TooLarge` with nothing written; and that `save` creates the subdirectory.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

---

### Task 7: Verification

- [ ] **Step 1: Full suite and both boards**

Baseline is **166**. This plan adds 6 (utf8) + 5 (path) + 13 (doc) + 6 (tags) + 7 (file) = **~203**.

- [ ] **Step 2: Confirm the layering holds**

```bash
grep -rn '#include "util/\|#include "src/' lib/ && echo "LAYERING VIOLATION" || echo "lib/ is clean"
```

- [ ] **Step 3: Confirm the deliberate dead surface**

```bash
grep -rn "HighlightFile\|HighlightDoc" src lib --include=*.cpp | grep -v "HighlightDoc.cpp\|HighlightFile.cpp"
```

Expected: no hits.

---

## Follow-ups recorded, not done here

- **Route highlight loads through `readFileToBuffer`** to escape the 50 KB `String` ceiling and its byte-at-a-time build. That would let `SAVE_BYTE_BUDGET` rise well above the study-Bible ceiling.
- **Heap on the C3.** A large document becomes a `String` plus an ArduinoJson pool on a chip the repo puts at ~50 KB free during reading. Measure before enabling highlights on C3 targets, or restrict the feature to the S3.
- **Overlap policy.** `VisibleRange::overlaps` exists and is unused: `addHighlight` neither rejects nor merges overlapping ranges, and nothing keeps them sorted. The UI plan must decide.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| `lib/` → `src/` include breaks the firmware | High | Layering fixed by construction; `pio run` in Task 4, and Task 7 greps for it |
| Document exceeds the read cap | High | `save` measures and refuses; the worst-case test records that caps alone do not bound bytes |
| Overwriting a file that failed to parse | High | `LoadResult::Failed`; tested against a fake that asserts the file is untouched |
| `.tmp` is the only copy and gets overwritten | High | Promoted immediately on recovery, not deferred to the next save |
| Corrupt file yields an out-of-bounds tag index | High | `fromJson` validates; tested with an out-of-range reference |
| A future-version file is reinterpreted | Medium | `v` is checked and a newer version is rejected |
| Tag renumbering corrupts assignments | Medium | Six tests asserting on names, including delete-all-in-sequence |
