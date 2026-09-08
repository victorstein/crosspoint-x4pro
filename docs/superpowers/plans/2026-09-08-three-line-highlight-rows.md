# Three-line highlight rows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Status:** Tasks 1-6 implemented and verified on hardware. Tasks 4 and 6 were
> superseded during device testing: the `'\n'` subtitle design does not work on
> this renderer, `HighlightRowText` was deleted, and tags moved to the row's
> value slot. See "Revision: why the newline design was abandoned" in the spec.
> Task 7 (the offline passage refill) has not been run.

**Goal:** Split a highlight's stored 72-byte label into a separate reference and passage, and render them on three lines (reference / passage / tags) instead of crowding the reference and passage onto one.

**Architecture:** `HighlightEntry` gains a `reference` field serialised as an additive `ref` JSON key — no `FORMAT_VERSION` bump, so older firmware still reads the file. The Highlights list puts the reference in the row's label slot and `passage + "\n" + tags` in its subtitle slot with `maxLines = 3`; the SDK's layout engine hard-breaks on `'\n'` and grows the row to fit. Legacy `"reference · passage"` labels are split at load, and a one-time offline pass refills the 56 migrated passages to the full byte budget.

**Tech Stack:** C++20 (`-fno-exceptions`, no RTTI), ArduinoJson 7.4.2, GoogleTest 1.17 on the host, PlatformIO for firmware, Python 3 for the offline migration.

**Spec:** [docs/superpowers/specs/2026-09-08-highlights-three-line-rows-design.md](../specs/2026-09-08-highlights-three-line-rows-design.md)

---

## Before you start

Read the spec. Two decisions in it look arbitrary and are not — if you "simplify" either one you will reintroduce a bug that was already found and measured:

1. **The passage goes in the subtitle slot, never in the label with a `\n`.** The
   row-growth pre-pass gates on a single-line width measure (`list.h:410-416`),
   so a short pair like `Juan 3:16` + `Dios amó` reserves one line and then draws
   two, over the top of the subtitle. Verified experimentally.
2. **`props.subtitleText` must be assigned from the theme BEFORE `maxLines` is
   set.** `textStyleUnset` counts `maxLines == 1` among its conditions
   (`FreeInkUICore.h:550-554`), so setting `maxLines` alone makes `Screen::list`
   skip the theme substitution (`FreeInkApp.h:249-251`) and the subtitle renders
   with `font == 0`.

**Environment check.** Run `uname -s` once. On Darwin/Linux everything below works
as written.

**Git.** Work on the existing `feat/tagged-highlights` branch. Do not push and do
not open a PR — the user approves those separately. End every commit message with:

```
Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
```

**Host test loop** (used by every task below):

```bash
cmake -S test -B build/test
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
```

**Formatting.** Before each commit run `./bin/clang-format-fix -g`. Never invoke
`clang-format` directly, and never probe for it with `command -v` — the wrapper is
the only sanctioned entry point.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `lib/Utf8/Utf8.h` / `.cpp` | Label normalisation | Add a `maxBytes` parameter to `utf8SafeSummary` |
| `lib/Epub/Epub/HighlightEntry.h` | The stored highlight | Add `reference` |
| `lib/Epub/Epub/HighlightDoc.h` / `.cpp` | Format rules, validation, the legacy split | Add `MAX_REFERENCE_BYTES`, serialise `ref`, split legacy labels |
| `src/activities/reader/HighlightRowText.h` | Pure subtitle composition | **New** — host-testable, no dependencies |
| `src/activities/reader/HighlightsActivity.cpp` | The list rows | Reference to label, composed subtitle, themed `subtitleText` |
| `src/activities/reader/PassageSelectActivity.cpp` | Highlight creation | Store the two fields separately; delete the budget rationing |
| `test/highlight_row_text/` | Composition tests | **New** suite |
| `test/highlight_doc/HighlightDocTest.cpp` | Format tests | Reference round-trip, legacy split, idempotency |
| `test/utf8_summary/Utf8SummaryTest.cpp` | Normalisation tests | The `maxBytes` overload |
| `scripts/migrate_highlight_refs.py` | **New** — the one-time offline pass | Split + re-extract passages for the 56 |

Tasks 1-6 are firmware and land in order. Task 7 (the offline migration) touches
live device data and is deliberately last, after the firmware is on the device and
the layout has been seen working.

---

## Task 1: `utf8SafeSummary` takes a byte cap

The reference needs a 48-byte cap while the passage keeps 72. Rather than a second
near-duplicate normaliser, the existing one takes a parameter.

**Files:**
- Modify: `lib/Utf8/Utf8.h:32`
- Modify: `lib/Utf8/Utf8.cpp:185-203`
- Test: `test/utf8_summary/Utf8SummaryTest.cpp`

- [x] **Step 1: Write the failing tests**

Append to `test/utf8_summary/Utf8SummaryTest.cpp`:

```cpp
TEST(Utf8SafeSummary, HonoursAnExplicitByteCap) {
  EXPECT_EQ(utf8SafeSummary(std::string(200, 'x'), 48).size(), 48u);
  EXPECT_EQ(utf8SafeSummary(std::string(200, 'x'), 10).size(), 10u);
}

TEST(Utf8SafeSummary, ExplicitCapStillRespectsCodepointBoundaries) {
  // Four 2-byte codepoints. A cap of 5 must cut back to 4 bytes, not split the
  // third sequence and emit a replacement character.
  const std::string accented = "\xc3\xa9\xc3\xa9" "\xc3\xa9\xc3\xa9";
  const std::string capped = utf8SafeSummary(accented, 5);
  EXPECT_EQ(capped.size(), 4u);
  EXPECT_EQ(capped, "\xc3\xa9\xc3\xa9");
}

TEST(Utf8SafeSummary, DefaultCapIsStill72) {
  EXPECT_EQ(utf8SafeSummary(std::string(200, 'x')).size(), 72u);
}
```

Note the split string literal in the second test. Writing `"\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"`
as one literal is fine, but any hex escape followed by a hex digit character
(`"\xc3\xa9ab"`) is parsed as one oversized escape and fails to compile.

- [x] **Step 2: Run the tests and watch them fail**

```bash
cmake -S test -B build/test && cmake --build build/test --target Utf8SummaryTest
```

Expected: compile error, `too many arguments to function call, expected 1, have 2`.

- [x] **Step 3: Add the parameter**

In `lib/Utf8/Utf8.h`, replace the declaration at line 32 and extend the comment
above it:

```cpp
// Normalises a passage into a display label: collapses runs of whitespace,
// strips newlines, trims, and caps at maxBytes WITHOUT splitting a UTF-8
// sequence. The 72-byte default matches BookmarkUtil's historical behaviour; the
// codepoint safety does not — a split sequence renders as a replacement
// character, and highlighted passages are far likelier to be non-ASCII than a
// page's first words. A highlight's reference passes a smaller cap.
std::string utf8SafeSummary(std::string passage, size_t maxBytes = 72);
```

In `lib/Utf8/Utf8.cpp`, change the signature and the two literals:

```cpp
std::string utf8SafeSummary(std::string passage, const size_t maxBytes) {
```

and

```cpp
  if (passage.size() > maxBytes) {
    passage.resize(static_cast<size_t>(utf8SafeTruncateBuffer(passage.data(), static_cast<int>(maxBytes))));
  }
```

The default argument goes in the header only — repeating it in the `.cpp` is a
compile error.

- [x] **Step 4: Run the tests and watch them pass**

```bash
cmake --build build/test --target Utf8SummaryTest && ctest --test-dir build/test -R Utf8Safe --output-on-failure
```

Expected: all `Utf8SafeSummary.*` tests PASS, including the pre-existing ones.

- [x] **Step 5: Commit**

```bash
./bin/clang-format-fix -g
git add lib/Utf8/Utf8.h lib/Utf8/Utf8.cpp test/utf8_summary/Utf8SummaryTest.cpp
git commit -F - <<'MSG'
refactor(utf8): let utf8SafeSummary take an explicit byte cap

A highlight's reference needs a smaller cap than its passage. A parameter
with the existing 72-byte default keeps one normaliser instead of two that
can drift.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

---

## Task 2: `HighlightEntry` carries a reference

**Files:**
- Modify: `lib/Epub/Epub/HighlightEntry.h`
- Modify: `lib/Epub/Epub/HighlightDoc.h:22-27`, `lib/Epub/Epub/HighlightDoc.cpp:36-47` and `:77-96`
- Test: `test/highlight_doc/HighlightDocTest.cpp`

- [x] **Step 1: Write the failing tests**

Append to `test/highlight_doc/HighlightDocTest.cpp`:

```cpp
TEST(HighlightDoc, RoundTripsAReference) {
  HighlightDoc doc;
  HighlightEntry e = makeEntry(3, 100, 200);
  e.reference = "Apocalipsis 1:8";
  e.label = "8 Yo soy el Alfa";
  doc.addHighlight(std::move(e));

  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(doc, parsed));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  EXPECT_EQ(parsed.highlights()[0].reference, "Apocalipsis 1:8");
  EXPECT_EQ(parsed.highlights()[0].label, "8 Yo soy el Alfa");
}

TEST(HighlightDoc, OmitsTheRefKeyWhenTheReferenceIsEmpty) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(0, 0, 10));

  JsonDocument json;
  doc.toJson(json);
  std::string text;
  serializeJson(json, text);
  EXPECT_EQ(text.find("\"ref\""), std::string::npos) << text;
}

TEST(HighlightDoc, TruncatesAnOverlongReferenceOnACodepointBoundary) {
  HighlightDoc doc;
  HighlightEntry e = makeEntry(0, 0, 10);
  // 30 two-byte codepoints = 60 bytes, past the 48-byte cap. Cutting at 48
  // would land mid-sequence if the cap were applied blindly.
  for (int i = 0; i < 30; ++i) e.reference += "\xc3\xa9";
  doc.addHighlight(std::move(e));

  const std::string stored = doc.highlights()[0].reference;
  EXPECT_EQ(stored.size(), 48u);
  EXPECT_EQ(static_cast<unsigned char>(stored.back()), 0xa9u) << "cut mid-sequence";
}
```

- [x] **Step 2: Run the tests and watch them fail**

```bash
cmake --build build/test --target HighlightDocTest
```

Expected: compile error, `no member named 'reference' in 'HighlightEntry'`.

- [x] **Step 3: Add the field, the cap, and the serialisation**

`lib/Epub/Epub/HighlightEntry.h` — add the field and extend the struct comment:

```cpp
// One highlighted passage. The range is absolute visible-codepoint offsets within
// the spine item, so it survives re-pagination. `label` (the passage snippet) and
// `reference` (e.g. "Apocalipsis 1:8") are both display-only and must never be
// used to locate the passage.
struct HighlightEntry {
  uint16_t spineIndex = 0;
  VisibleRange range;
  std::vector<uint16_t> tagIndices;
  std::string label;
  std::string reference;
};
```

`lib/Epub/Epub/HighlightDoc.h` — add the cap beside the others (after
`MAX_TAG_NAME_BYTES`):

```cpp
  static constexpr size_t MAX_TAG_NAME_BYTES = 24;
  // A reference is composed from a book's own table-of-contents title
  // (PassageSelectActivity::verseReference), which is arbitrary text, so it is
  // bounded here rather than trusted.
  static constexpr size_t MAX_REFERENCE_BYTES = 48;
```

`lib/Epub/Epub/HighlightDoc.cpp` — in `addHighlight`, beside the existing label
normalisation:

```cpp
  entry.label = utf8SafeSummary(std::move(entry.label));
  entry.reference = utf8SafeSummary(std::move(entry.reference), MAX_REFERENCE_BYTES);
```

In `toJson`, between the tag array and `o["text"]`:

```cpp
    if (!h.reference.empty()) o["ref"] = h.reference;
    o["text"] = h.label;
```

In `fromJson`, beside the existing label parse:

```cpp
    entry.label = utf8SafeSummary(std::string(o["text"] | ""));
    entry.reference = utf8SafeSummary(std::string(o["ref"] | ""), MAX_REFERENCE_BYTES);
```

- [x] **Step 4: Run the tests and watch them pass**

```bash
cmake --build build/test --target HighlightDocTest && ctest --test-dir build/test -R HighlightDoc --output-on-failure
```

Expected: PASS, including the pre-existing `WorstCaseDocumentStaysUnderTheSaveBudget`
(it asserts the worst case is *over* budget, and stays over).

- [x] **Step 5: Commit**

```bash
./bin/clang-format-fix -g
git add lib/Epub/Epub/HighlightEntry.h lib/Epub/Epub/HighlightDoc.h lib/Epub/Epub/HighlightDoc.cpp test/highlight_doc/HighlightDocTest.cpp
git commit -F - <<'MSG'
feat(highlights): store a passage's reference in its own field

The reference and the passage shared one 72-byte label, so the reference
consumed a mean 16.2 bytes of the passage's budget across the migrated
set. `ref` is an additive JSON key with no FORMAT_VERSION bump: older
firmware ignores it and still renders the passage.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

---

## Task 3: Split legacy labels at load (Migration A)

Highlights already on the device store `"Mateo 11:19 · 19 Vino el Hijo…"` in one
field. Splitting them at parse time needs no file rewrite and no version bump.

**Files:**
- Modify: `lib/Epub/Epub/HighlightDoc.cpp` (`fromJson`)
- Test: `test/highlight_doc/HighlightDocTest.cpp`

- [x] **Step 1: Write the failing tests**

Append to `test/highlight_doc/HighlightDocTest.cpp`:

```cpp
namespace {
// Parses a hand-written document, bypassing toJson, so these tests exercise
// exactly the bytes a device wrote before `ref` existed.
bool parseText(const char* json, HighlightDoc& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  return out.fromJson(doc);
}
}  // namespace

TEST(HighlightDocLegacy, SplitsAReferencePrefixOutOfTheLabel) {
  HighlightDoc doc;
  ASSERT_TRUE(parseText(
      R"({"v":1,"tags":[],"highlights":[
         {"si":3,"start":100,"end":200,"text":"Mateo 11:19 · 19 Vino el Hijo"}]})",
      doc));
  ASSERT_EQ(doc.highlights().size(), 1u);
  EXPECT_EQ(doc.highlights()[0].reference, "Mateo 11:19");
  EXPECT_EQ(doc.highlights()[0].label, "19 Vino el Hijo");
}

TEST(HighlightDocLegacy, LeavesAnEntryThatAlreadyHasARefAlone) {
  HighlightDoc doc;
  ASSERT_TRUE(parseText(
      R"({"v":1,"tags":[],"highlights":[
         {"si":3,"start":100,"end":200,"ref":"Mateo 11:19",
          "text":"algo · con separador"}]})",
      doc));
  EXPECT_EQ(doc.highlights()[0].reference, "Mateo 11:19");
  EXPECT_EQ(doc.highlights()[0].label, "algo \xc2\xb7 con separador");
}

TEST(HighlightDocLegacy, TreatsAnEmptyRefAsAbsentAndStillSplits) {
  HighlightDoc doc;
  ASSERT_TRUE(parseText(
      R"({"v":1,"tags":[],"highlights":[
         {"si":3,"start":100,"end":200,"ref":"",
          "text":"Mateo 11:19 · 19 Vino el Hijo"}]})",
      doc));
  EXPECT_EQ(doc.highlights()[0].reference, "Mateo 11:19");
  EXPECT_EQ(doc.highlights()[0].label, "19 Vino el Hijo");
}

TEST(HighlightDocLegacy, ALabelWithNoSeparatorBecomesPassageOnly) {
  HighlightDoc doc;
  ASSERT_TRUE(parseText(
      R"({"v":1,"tags":[],"highlights":[
         {"si":3,"start":100,"end":200,"text":"just a passage"}]})",
      doc));
  EXPECT_EQ(doc.highlights()[0].reference, "");
  EXPECT_EQ(doc.highlights()[0].label, "just a passage");
}

TEST(HighlightDocLegacy, SplitsOnTheFirstSeparatorOnly) {
  HighlightDoc doc;
  ASSERT_TRUE(parseText(
      R"({"v":1,"tags":[],"highlights":[
         {"si":3,"start":100,"end":200,
          "text":"Mateo 11:19 · uno · dos"}]})",
      doc));
  EXPECT_EQ(doc.highlights()[0].reference, "Mateo 11:19");
  EXPECT_EQ(doc.highlights()[0].label, "uno \xc2\xb7 dos");
}

TEST(HighlightDocLegacy, SplittingIsIdempotentAcrossASaveAndReload) {
  HighlightDoc first;
  ASSERT_TRUE(parseText(
      R"({"v":1,"tags":[],"highlights":[
         {"si":3,"start":100,"end":200,"text":"Mateo 11:19 · 19 Vino el Hijo"}]})",
      first));
  HighlightDoc second;
  ASSERT_TRUE(roundTrip(first, second));
  EXPECT_EQ(second.highlights()[0].reference, "Mateo 11:19");
  EXPECT_EQ(second.highlights()[0].label, "19 Vino el Hijo");
}
```

- [x] **Step 2: Run the tests and watch them fail**

```bash
cmake --build build/test --target HighlightDocTest && ctest --test-dir build/test -R HighlightDocLegacy --output-on-failure
```

Expected: `SplitsAReferencePrefixOutOfTheLabel` FAILS with
`reference` empty and `label` still holding the whole `"Mateo 11:19 · 19 Vino el Hijo"`.
`LeavesAnEntryThatAlreadyHasARefAlone` and `ALabelWithNoSeparatorBecomesPassageOnly`
already pass — that is correct, they are the regression guards.

- [x] **Step 3: Implement the split**

Add near the top of `lib/Epub/Epub/HighlightDoc.cpp`, inside an anonymous namespace:

```cpp
namespace {

// Labels written before the reference had its own field packed both into one
// string as "<reference> \xc2\xb7 <passage>". Splitting here rather than in a
// migration pass means no file rewrite is needed: the entry adopts the new
// shape on the next ordinary save, and re-reading an already-split entry is a
// no-op. An empty `ref` counts as absent so a partially-written file still
// recovers.
constexpr const char* LEGACY_LABEL_SEPARATOR = " \xc2\xb7 ";

void splitLegacyLabel(HighlightEntry& entry) {
  if (!entry.reference.empty()) return;
  const size_t at = entry.label.find(LEGACY_LABEL_SEPARATOR);
  if (at == std::string::npos) return;
  entry.reference = entry.label.substr(0, at);
  entry.label.erase(0, at + strlen(LEGACY_LABEL_SEPARATOR));
}

}  // namespace
```

Add `#include <cstring>` to the includes at the top of the file for `strlen`.

In `fromJson`, replace the two normalisation lines from Task 2 with a split that
runs *before* the caps are applied, so the separator cannot survive inside a
truncated passage:

```cpp
    entry.label = std::string(o["text"] | "");
    entry.reference = std::string(o["ref"] | "");
    splitLegacyLabel(entry);
    entry.label = utf8SafeSummary(std::move(entry.label));
    entry.reference = utf8SafeSummary(std::move(entry.reference), MAX_REFERENCE_BYTES);
```

- [x] **Step 4: Run the tests and watch them pass**

```bash
cmake --build build/test --target HighlightDocTest && ctest --test-dir build/test -R HighlightDoc --output-on-failure
```

Expected: all `HighlightDoc*` tests PASS.

- [x] **Step 5: Commit**

```bash
./bin/clang-format-fix -g
git add lib/Epub/Epub/HighlightDoc.cpp test/highlight_doc/HighlightDocTest.cpp
git commit -F - <<'MSG'
feat(highlights): split legacy "reference · passage" labels at load

Recovers the reference from every highlight written before it had its own
field, with no file rewrite and no format version bump: the entry adopts
the new shape on the next ordinary save, and re-reading a split entry is
a no-op.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

---

## Task 4: `HighlightRowText::composeSubtitle`

A pure function in its own header so the row's text rules are host-testable,
following the `TagRowMapping.h` precedent.

**Files:**
- Create: `src/activities/reader/HighlightRowText.h`
- Create: `test/highlight_row_text/CMakeLists.txt`, `test/highlight_row_text/HighlightRowTextTest.cpp`
- Modify: `test/CMakeLists.txt`

- [x] **Step 1: Write the failing test**

Create `test/highlight_row_text/HighlightRowTextTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "activities/reader/HighlightRowText.h"

TEST(ComposeSubtitle, JoinsBothHalvesWithANewline) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("Porque fuimos salvados", "esperanza"),
            "Porque fuimos salvados\nesperanza");
}

TEST(ComposeSubtitle, NoLeadingNewlineWhenThePassageIsEmpty) {
  // layoutText preserves a blank line for a leading '\n', which would render an
  // empty first row line.
  EXPECT_EQ(HighlightRowText::composeSubtitle("", "esperanza"), "esperanza");
}

TEST(ComposeSubtitle, NoTrailingNewlineWhenThereAreNoTags) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("Porque fuimos salvados", ""),
            "Porque fuimos salvados");
}

TEST(ComposeSubtitle, BothEmptyYieldsAnEmptyString) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("", ""), "");
}

TEST(ComposeSubtitle, ATagNameCannotInjectAnExtraRowLine) {
  // A newline reaching the tag half would add a fourth line to the row.
  EXPECT_EQ(HighlightRowText::composeSubtitle("passage", "a\nb"), "passage\na b");
}

TEST(ComposeSubtitle, ControlCharactersInThePassageAreNeutralisedToo) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("one\ttwo", "tag"), "one two\ntag");
}
```

- [x] **Step 2: Register the suite and watch it fail**

Create `test/highlight_row_text/CMakeLists.txt`:

```cmake
add_executable(HighlightRowTextTest
  HighlightRowTextTest.cpp
)

target_include_directories(HighlightRowTextTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HighlightRowTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HighlightRowTextTest)
```

Append to `test/CMakeLists.txt`, after the `add_subdirectory(tag_rows)` line:

```cmake
add_subdirectory(highlight_row_text)
```

```bash
cmake -S test -B build/test && cmake --build build/test --target HighlightRowTextTest
```

Expected: `fatal error: 'activities/reader/HighlightRowText.h' file not found`.

- [x] **Step 3: Write the header**

Create `src/activities/reader/HighlightRowText.h`:

```cpp
#pragma once

#include <string>

// Text rules for one row of the Highlights list. The row shows the reference in
// its label slot and this composed string in its subtitle slot, so the '\n' here
// is what produces the third line: the SDK's layout engine hard-breaks on it
// (FreeInkUICore.h:716-717).
//
// Free function in a header so the rules are host-testable — the activity that
// uses them cannot be built off-device.
namespace HighlightRowText {

// Replaces every control character with a space, so neither half can add a line
// the row was not measured for. A tag name is user-entered and a passage comes
// from book markup, so neither is trusted to be single-line.
inline std::string flatten(std::string text) {
  for (char& c : text) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  }
  return text;
}

// Joins a passage and a rendered tag list into one subtitle. The '\n' is
// inserted ONLY when both halves are non-empty: layoutText preserves a blank
// line for a leading '\n' (FreeInkUICore.h:773-776), which would render an empty
// first line on a row whose passage is missing.
inline std::string composeSubtitle(const std::string& passage, const std::string& tags) {
  const std::string flatPassage = flatten(passage);
  const std::string flatTags = flatten(tags);
  if (flatPassage.empty()) return flatTags;
  if (flatTags.empty()) return flatPassage;
  return flatPassage + "\n" + flatTags;
}

}  // namespace HighlightRowText
```

- [x] **Step 4: Run the tests and watch them pass**

```bash
cmake --build build/test --target HighlightRowTextTest && ctest --test-dir build/test -R ComposeSubtitle --output-on-failure
```

Expected: all six PASS.

- [x] **Step 5: Commit**

```bash
./bin/clang-format-fix -g
git add src/activities/reader/HighlightRowText.h test/highlight_row_text test/CMakeLists.txt
git commit -F - <<'MSG'
feat(highlights): add a host-tested subtitle composer for list rows

The '\n' between passage and tags is what produces the row's third line,
and a control character reaching either half would add a fourth the row
was never measured for. Both rules live in a pure header so they can be
tested off-device.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

---

## Task 5: Store the reference separately when a highlight is created

**Files:**
- Modify: `src/activities/reader/PassageSelectActivity.cpp:374-388`

There is no host test for this file — it cannot be built off-device. The firmware
build is the check, and Task 2's `RoundTripsAReference` already covers the storage
contract this feeds.

- [x] **Step 1: Replace the budget rationing**

In `finalizeSelection`, replace this block:

```cpp
  const std::string reference = verseReference(range.start);
  const std::string passage = selectionLabel(lo, hi);
  // addHighlight truncates to 72 BYTES (Utf8.h:26-31), and the separator costs
  // 4 of them. A long TOC title can leave no room for a useful snippet, so the
  // reference is kept whole rather than shipping a truncated one.
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

with:

```cpp
  // Separate fields, so the two no longer compete for one 72-byte budget:
  // addHighlight caps each independently.
  entry.reference = verseReference(range.start);
  entry.label = selectionLabel(lo, hi);
```

- [x] **Step 2: Build the firmware**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`. If `LABEL_BUDGET` or `MIN_SNIPPET_BYTES` is reported unused,
you left a declaration behind — both must be gone.

- [x] **Step 3: Commit**

```bash
./bin/clang-format-fix -g
git add src/activities/reader/PassageSelectActivity.cpp
git commit -F - <<'MSG'
feat(highlights): store a new highlight's reference and passage apart

Deletes the budget rationing that dropped the passage entirely when a
long table-of-contents title left under 16 bytes for it. The two fields
are now capped independently, so the passage gets the full 72 bytes.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

---

## Task 6: Render the three-line row

**Files:**
- Modify: `src/activities/reader/HighlightsActivity.cpp:77-101` (`rebuildRowItems`) and `:462-469` (`buildScreen`'s `ListProps`)

- [x] **Step 1: Compose the subtitle and move the reference to the label**

Add the include beside the others at the top of `src/activities/reader/HighlightsActivity.cpp`:

```cpp
#include "HighlightRowText.h"
```

In `rebuildRowItems`, replace the loop body:

```cpp
  const auto& highlights = highlightDoc_.highlights();
  for (size_t i = 0; i < visibleIndices_.size(); ++i) {
    const auto& entry = highlights[visibleIndices_[i]];
    const std::string tags = tagsSubtitleFor(entry);

    fui::ListItem item{};
    // Reference on its own line, passage and tags in the subtitle beneath it.
    // Without a reference (a book with no verse anchors) the passage takes the
    // label slot, which is exactly the two-line layout this replaced.
    if (!entry.reference.empty()) {
      item.label = entry.reference.c_str();
      rowSubtitles_.push_back(HighlightRowText::composeSubtitle(entry.label, tags));
    } else if (!entry.label.empty()) {
      item.label = entry.label.c_str();
      rowSubtitles_.push_back(HighlightRowText::composeSubtitle("", tags));
    } else {
      item.label = tr(STR_UNNAMED);
      rowSubtitles_.push_back(HighlightRowText::composeSubtitle("", tags));
    }
    item.subtitle = rowSubtitles_.back().c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    rowItems_.push_back(item);
  }
```

`rowSubtitles_` is reserved to `visibleIndices_.size()` before the loop, so
`back().c_str()` stays valid as rows are appended — reassigning an element does
not reallocate the vector.

- [x] **Step 2: Theme the subtitle style, then raise `maxLines`**

In `buildScreen`, immediately before the `syncListViewport` call:

```cpp
  // Theme FIRST: Screen::list only substitutes the theme font into a style that
  // still passes textStyleUnset (FreeInkUICore.h:550-554), and maxLines != 1
  // fails that test. Setting maxLines alone would skip the substitution and
  // render the subtitle in font 0.
  props.subtitleText = screen.theme().smallText;
  // Three, not two: at two, a passage wide enough to wrap consumes both lines
  // and the tags are silently ellipsised away. Measured to happen in portrait.
  props.subtitleText.maxLines = 3;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
```

Do not touch `props.labelText`. Its default `maxLines` is already 1, and
assigning it anything trips the same rule and loses the theme's `bodyText`.

- [x] **Step 3: Build the firmware**

```bash
pio run -e x4pro
```

Expected: `SUCCESS`.

- [x] **Step 4: Run the whole host suite**

```bash
ctest --test-dir build/test --output-on-failure -j
```

Expected: 100% tests passed. This suite does not compile `HighlightsActivity.cpp`,
so a green run does not license any claim that the firmware builds — Step 3 is
what does that.

- [x] **Step 5: Commit**

```bash
./bin/clang-format-fix -g
git add src/activities/reader/HighlightsActivity.cpp
git commit -F - <<'MSG'
feat(highlights): give each row three lines

Reference on the label line, passage and tags in the subtitle beneath it.
subtitleText is assigned from the theme before maxLines is raised, because
a style with maxLines != 1 fails textStyleUnset and Screen::list would
skip the font substitution entirely.

maxLines is 3 rather than 2 so a passage that wraps cannot push the tags
off the row; in portrait at realistic glyph advances, 2 drops them.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

- [x] **Step 6: Flash and look at it**

```bash
pio run -e x4pro -t upload
```

Then on the device open a book with highlights and check:

1. Rows show reference / passage / tags on three separate lines.
2. The subtitle font matches the old two-line rows — if it looks wrong or
   oversized, Step 2's ordering was not applied and the theme substitution
   is being skipped.
3. A highlight with no tags renders as two lines with **no** blank line.
4. Filtering by tag, editing tags, and deleting a highlight all still work and
   the rows re-render at the right height afterwards.
5. Scroll past the first page and come back — no rows are skipped.

Stop here and report before starting Task 7. Task 7 rewrites live device data
and should only run once the layout is confirmed working.

---

## Task 7: Refill the migrated passages (Migration B)

The 56 migrated highlights had their passages truncated *with* the reference
prefix inside the 72-byte cap, so Task 3 recovers their reference but cannot
lengthen their passage. This one-time offline pass re-extracts the passage text
from the EPUB.

**Files:**
- Create: `scripts/migrate_highlight_refs.py`

**Inputs.** The pipeline from the original migration lives in a scratchpad
directory belonging to an earlier session, under `/private/tmp`, which is not
durable:

```
/private/tmp/claude-501/-Volumes-stein-Documents-development-personal-crosspoint-x4pro/00f8c092-02b5-427c-b5a7-eb87ab219286/scratchpad/
  epubwork/src/OEBPS/   unpacked nwt_S_slim.epub (3,942 files) — the build on the device
  offsets.py            offline replica of the firmware's visible-offset counter
  resolved.json         all 56 entries with spine, start, end, file, tags
```

- [x] **Step 1: Copy the inputs somewhere durable, or rebuild them**

```bash
SRC=/private/tmp/claude-501/-Volumes-stein-Documents-development-personal-crosspoint-x4pro/00f8c092-02b5-427c-b5a7-eb87ab219286/scratchpad
WORK="$(mktemp -d)/highlight-migration"
mkdir -p "$WORK"
cp -R "$SRC/epubwork" "$SRC/offsets.py" "$SRC/resolve.py" "$SRC/resolved.json" "$WORK/" \
  && echo "copied to $WORK" \
  || echo "SCRATCHPAD GONE — rebuild per Step 1b"
```

**Step 1b, only if the copy failed.** Rebuild from the two source files the user
still has:

```bash
mkdir -p "$WORK/epubwork/src" && unzip -q ~/Downloads/nwt_S_slim.epub -d "$WORK/epubwork/src"
mkdir -p "$WORK/jwl" && unzip -q ~/Downloads/UserdataBackup_2026-09-07_Samsung_SM-F966B.jwlibrary -d "$WORK/jwl"
cd "$WORK" && python3 resolve.py   # regenerates resolved.json
```

`resolve.py` prints `resolved 56/56  failed 0` when it has worked. Any other count
means the EPUB is not the build that is on the device — stop and report rather
than migrating against the wrong offsets.

- [x] **Step 2: Download and back up the live file**

There is no highlights-specific endpoint. The device exposes a generic SD file
browser (`CrossPointWebServer.cpp:143-152`), and highlights live at
`/.crosspoint/highlights/<flattened book path>.json`, where the flattened name is
the book path with its leading `/` removed, `/` replaced by `_`, and the extension
stripped (`PathFlatten.cpp:8-17`).

Do not guess that name — list the directory. Confirm the device address with the
user first; it was `192.168.68.92`, but a DHCP lease can move.

```bash
cd "$WORK"
DEV=http://192.168.68.92
curl -sf "$DEV/api/files?path=/.crosspoint/highlights"
```

Expected: a JSON listing with one `.json` per book that has highlights. Pick the
one for the Bible and set it:

```bash
HL=/.crosspoint/highlights/<name from the listing>.json
curl -sf "$DEV/download?path=$HL" -o live.json && cp live.json live.backup.json
python3 -c "import json;d=json.load(open('live.json'));print(len(d['highlights']),'highlights,',len(d['tags']),'tags')"
```

Expected: a count of 56 or more. If `curl` fails, the device is off, on another
address, or the web server is not running — stop, do not proceed with a stale
file.

- [x] **Step 3: Write the migration script**

Create `scripts/migrate_highlight_refs.py`:

```python
"""One-time refill of migrated highlight passages.

Highlights migrated from JW Library packed "<reference> \xc2\xb7 <passage>" into a
single 72-byte label, so the passage was truncated to make room. The firmware
now stores the two apart; this recovers the reference from the existing label
and re-extracts the passage from the EPUB to use the full budget.

Rewrites ONLY `ref` and `text`, and only for entries that match an existing
highlight on (si, start). Offsets and tags are never touched: the offsets are
verified correct on hardware and the tags may carry edits made since.
"""
import json
import os
import sys
import xml.parsers.expat

SEPARATOR = " · "
PASSAGE_MAX_BYTES = 72

NON_VISIBLE = {"head", "style", "script", "title", "rp"}


def _local(name):
    return name.split(":")[-1].lower()


def visible_text(xhtml_bytes):
    """Visible character data, in the same coordinate space as the firmware's
    offsets. Mirrors offsets.py, which mirrors ChapterHtmlSlimParser."""
    st = {"inside_body": False, "nonvis": 0}
    out = []

    def start(name, attrs):
        n = _local(name)
        if n == "body":
            st["inside_body"] = True
        if st["inside_body"] and (st["nonvis"] > 0 or n in NON_VISIBLE):
            st["nonvis"] += 1

    def end(name):
        if st["nonvis"] > 0:
            st["nonvis"] -= 1
        if _local(name) == "body":
            st["inside_body"] = False

    def chars(data):
        if st["inside_body"] and st["nonvis"] == 0:
            out.append(data)

    p = xml.parsers.expat.ParserCreate()
    p.StartElementHandler = start
    p.EndElementHandler = end
    p.CharacterDataHandler = chars
    p.Parse(xhtml_bytes, True)
    return "".join(out)


def utf8_safe_summary(passage, max_bytes=PASSAGE_MAX_BYTES):
    """Byte-for-byte equivalent of lib/Utf8/Utf8.cpp's utf8SafeSummary."""
    collapsed = []
    for ch in passage:
        if ch.isspace() and collapsed and collapsed[-1].isspace():
            continue
        collapsed.append(ch)
    text = "".join(collapsed).replace("\n", "").strip()
    raw = text.encode("utf-8")[:max_bytes]
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    return raw.decode("utf-8")


def main():
    if len(sys.argv) != 5:
        print("usage: migrate_highlight_refs.py <live.json> <resolved.json> <OEBPS dir> <out.json>")
        return 1
    live_path, resolved_path, oebps, out_path = sys.argv[1:]

    live = json.load(open(live_path, encoding="utf-8"))
    resolved = json.load(open(resolved_path, encoding="utf-8"))

    # (spine, start) -> the source file holding that passage
    by_key = {(r["spine"], r["start"]): r for r in resolved}

    cache = {}
    rewritten, skipped = 0, 0
    for h in live["highlights"]:
        key = (h["si"], h["start"])
        match = by_key.get(key)
        if match is None:
            skipped += 1
            continue

        # The reference is already correct on the device; recover it rather than
        # re-deriving it from the JW database.
        if "ref" not in h or not h["ref"]:
            if SEPARATOR in h.get("text", ""):
                ref, _ = h["text"].split(SEPARATOR, 1)
                h["ref"] = ref
            else:
                skipped += 1
                continue

        fname = match["file"]
        if fname not in cache:
            cache[fname] = visible_text(open(os.path.join(oebps, fname), "rb").read())
        h["text"] = utf8_safe_summary(cache[fname][match["start"]:match["end"]])
        rewritten += 1

    json.dump(live, open(out_path, "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
    print(f"rewrote {rewritten}, left {skipped} untouched -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [x] **Step 4: Run it and check the one untested assumption**

```bash
cd "$WORK"
python3 "$OLDPWD/scripts/migrate_highlight_refs.py" live.json resolved.json epubwork/src/OEBPS merged.json
```

Expected: `rewrote 56, left N untouched`, where N is the number of highlights
created on the device since the migration.

Now the check the spec flags as mandatory. The offline pass slices visible XHTML;
the device builds a passage by joining word boxes (`PassageSelectActivity.cpp:233-258`).
These are different mechanisms and may disagree on whitespace or punctuation:

```bash
python3 - <<'PY'
import json
live = {(h['si'], h['start']): h for h in json.load(open('live.json', encoding='utf-8'))['highlights']}
merged = {(h['si'], h['start']): h for h in json.load(open('merged.json', encoding='utf-8'))['highlights']}
for k, m in merged.items():
    old = live[k].get('text', '')
    tail = old.split(' · ', 1)[1] if ' · ' in old else old
    if not m['text'].startswith(tail[:30]):
        print('DIVERGES at', k)
        print('  device :', repr(tail[:60]))
        print('  offline:', repr(m['text'][:60]))
PY
```

Expected: no output. Every re-extracted passage must begin with what the device
already stored. **If anything diverges, stop and report it** — it means the two
extraction paths disagree and the merged file would make the migrated highlights
inconsistent with device-created ones, which is the opposite of this feature's
goal.

- [x] **Step 5: Upload, verify, and only then finish**

`/upload` takes a multipart form file, with the destination *directory* as a
`path` query parameter and the destination filename taken from the uploaded
part's own filename (`CrossPointWebServer.cpp:669`, `:679-694`). So the local file
must be named exactly as it is on the device.

```bash
cd "$WORK"
cp merged.json "$(basename "$HL")"
curl -sf -X POST "$DEV/upload?path=/.crosspoint/highlights" \
     -H "Expect:" \
     -F "file=@$(basename "$HL")"
```

The `-H "Expect:"` is required: curl auto-sends `Expect: 100-continue` for a
multipart body and the ESP32 `WebServer` never answers it, so the upload stalls
and reports HTTP 000. Raising `--expect100-timeout` makes it worse, not better.

Expected: `File uploaded successfully: <name>`.

Verify before considering this done:

```bash
curl -sf "$DEV/download?path=$HL" -o verify.json
python3 -c "
import json
a=json.load(open('merged.json',encoding='utf-8')); b=json.load(open('verify.json',encoding='utf-8'))
print('MATCH' if a==b else 'MISMATCH — device did not store what was sent')
"
```

Expected: `MATCH`. If it mismatches, restore with the same upload flow using
`live.backup.json` and report. Keep `live.backup.json` until the user has seen
the result on the device and confirmed it.

**The device caches highlights in memory per open book.** Close the book (or
restart the device) before checking the result, or the reader may still be
holding the pre-upload document and overwrite the file on its next save.

- [x] **Step 6: Commit the script**

```bash
./bin/clang-format-fix -g
git add scripts/migrate_highlight_refs.py
git commit -F - <<'MSG'
chore(scripts): add the one-time highlight reference migration

Refills the passages of highlights migrated from JW Library, whose text
was truncated to share a 72-byte label with the reference. Rewrites only
ref and text, and only for entries matching an existing highlight on
(si, start), so offsets and any tag edits made since are preserved.

Claude-Session: https://claude.ai/code/session_014SN3tM7Uzvyh1PsgvFzNEV
MSG
```

---

## Done when

- [ ] `ctest --test-dir build/test --output-on-failure -j` is 100% green
- [ ] `pio run -e x4pro` succeeds
- [ ] The device shows reference / passage / tags on three lines, in the theme's
      subtitle font
- [ ] An untagged highlight shows two lines with no blank line
- [ ] The 56 migrated highlights show visibly longer passages than before
- [ ] A newly created highlight is indistinguishable in layout from a migrated one
- [ ] Tag filtering, tag editing and deletion still work, and rows re-render at
      the correct height afterwards

**Not done, and out of scope:** stripping the leading verse number from passages
(`8 "Yo soy…`), and any change to how anchors are resolved.
