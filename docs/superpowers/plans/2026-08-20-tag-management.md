# Tag Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the tag palette two-way — a tag can be deleted, and a saved highlight's tags can be changed.

**Architecture:** One new pure `HighlightDoc` method (host-tested), plus two UI affordances built from existing components. `TagPickerActivity` gains the ability to persist palette changes itself, which also closes an existing durability hole.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest.

**Depends on:** the foundations, anchoring, data-layer and UI plans — all complete. Baseline **216 host tests** at `7672061a`.

**Delivery:** fork-only.

---

## Why this exists

The tag palette is currently a one-way door, and the consequences are worse than a missing screen:

- **`HighlightDoc::removeTag` is called only by tests.** It is implemented and well covered — six tests, including the index renumbering that keeps every highlight resolving to the same tag *name* — but no UI reaches it.
- **A typo in "New tag…" is permanent.** The palette is book-wide and `TagPickerActivity`'s class comment states the add is deliberately not rolled back on cancel.
- **`MAX_TAGS` is 32.** Once full, `addTag` returns `nullopt` forever and the user sees "palette full" with no on-device recovery.
- **A highlight's tags are set once, at creation.** `TagPickerActivity::initialSelection` exists and pre-checks rows (`TagPickerActivity.cpp:26`) but **no caller passes it** — it was written for a re-tag flow that does not exist. Retagging today means deleting the highlight and re-selecting the passage.

That undercuts the browser's tag filter as much as having no tags at all.

## An existing durability hole this plan closes

`TagPickerActivity` today takes only `HighlightDoc&`. It calls `addTag`, which mutates the in-memory palette — but the activity cannot save. The new tag reaches disk only if the *caller* happens to save afterwards.

In the creation flow that works by accident: `PassageSelectActivity::finalizeSelection` saves the whole document. But if the user creates a tag and then cancels the highlight, the tag exists in RAM and vanishes on reload. Worse, after this plan adds a *deletion* path, a delete that is never saved would silently come back.

**Decision: `TagPickerActivity` persists palette mutations itself, immediately.** It gains `bookPath` and `saveDisabled`, matching `HighlightsActivity`'s constructor shape. A palette change is book-wide state and should not depend on what the caller does next.

## What this plan does not do

- **No cross-book tag management.** Tags are per-book by design.
- **No tag renaming.** `removeTag` + `addTag` is the available primitive; renaming would need a new method and its own renumbering story. If you want it, it is a separate plan.
- **No merge on duplicate names.** `addTag` already dedupes by name and returns the existing index.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/HighlightDoc.h` / `.cpp` (modify) | `setTags` — change an existing entry's tags, validated |
| `test/highlight_doc/HighlightDocTest.cpp` (modify) | Host tests for `setTags` |
| `src/activities/reader/TagPickerActivity.{h,cpp}` (modify) | Persist palette changes; long-press a tag to delete it |
| `src/activities/reader/HighlightsActivity.{h,cpp}` (modify) | Long-press offers Tags / Delete / Cancel |
| `src/activities/reader/PassageSelectActivity.cpp` (modify) | Pass the new `TagPickerActivity` arguments |
| `lib/I18n/translations/english.yaml` (modify) | New strings |

---

### Task 1: `HighlightDoc::setTags`

`highlights()` returns a `const&`, so there is no way to change an entry's tags today. This is the only new pure logic in the plan, and the only part that is host-testable.

**Files:**
- Modify: `lib/Epub/Epub/HighlightDoc.h`, `lib/Epub/Epub/HighlightDoc.cpp`
- Modify: `test/highlight_doc/HighlightDocTest.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(HighlightDocSetTags, ReplacesAnEntrysTags) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  ASSERT_TRUE(doc.addTag("beta").has_value());
  doc.addHighlight(makeEntry(0, 0, 10, {0}));

  ASSERT_TRUE(doc.setTags(0, {1}));
  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "beta") << "the old tag must be replaced, not merged";
}

TEST(HighlightDocSetTags, ClearsTagsWithAnEmptyList) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  ASSERT_TRUE(doc.setTags(0, {}));
  EXPECT_TRUE(doc.highlights()[0].tagIndices.empty());
}

TEST(HighlightDocSetTags, RejectsAnOutOfRangeEntryIndex) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(0, 0, 10));
  EXPECT_FALSE(doc.setTags(7, {}));
  EXPECT_FALSE(doc.setTags(1, {})) << "one past the end is out of range";
}

TEST(HighlightDocSetTags, DropsTagIndicesOutsideThePalette) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  doc.addHighlight(makeEntry(0, 0, 10));

  ASSERT_TRUE(doc.setTags(0, {0, 9}));
  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u) << "index 9 has no tag and must not survive";
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
}

TEST(HighlightDocSetTags, CapsAtMaxTagsPerHighlight) {
  HighlightDoc doc;
  for (size_t i = 0; i < HighlightDoc::MAX_TAGS_PER_HIGHLIGHT + 2; ++i) {
    ASSERT_TRUE(doc.addTag("t" + std::to_string(i)).has_value());
  }
  doc.addHighlight(makeEntry(0, 0, 10));

  std::vector<uint16_t> many;
  for (size_t i = 0; i < HighlightDoc::MAX_TAGS_PER_HIGHLIGHT + 2; ++i) many.push_back(static_cast<uint16_t>(i));
  ASSERT_TRUE(doc.setTags(0, many));
  EXPECT_EQ(doc.highlights()[0].tagIndices.size(), HighlightDoc::MAX_TAGS_PER_HIGHLIGHT);
}

TEST(HighlightDocSetTags, DeduplicatesRepeatedIndices) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  doc.addHighlight(makeEntry(0, 0, 10));
  ASSERT_TRUE(doc.setTags(0, {0, 0, 0}));
  EXPECT_EQ(doc.highlights()[0].tagIndices.size(), 1u) << "a repeated index must not consume the per-highlight cap";
}

TEST(HighlightDocSetTags, TouchesOnlyTheNamedEntry) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  ASSERT_TRUE(doc.addTag("beta").has_value());
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.addHighlight(makeEntry(0, 20, 30, {0}));

  ASSERT_TRUE(doc.setTags(1, {1}));
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha") << "the first entry must be untouched";
  EXPECT_EQ(doc.tags()[doc.highlights()[1].tagIndices[0]], "beta");
}

TEST(HighlightDocSetTags, SurvivesARoundTrip) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  ASSERT_TRUE(doc.addTag("beta").has_value());
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  ASSERT_TRUE(doc.setTags(0, {1}));

  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(doc, parsed));
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "beta");
}
```

`makeEntry` and `roundTrip` are the existing helpers in that file.

- [ ] **Step 2: Run and confirm failure** (`setTags` is undeclared)

- [ ] **Step 3: Declare it**

```cpp
  // Replaces entry `index`'s tags. Returns false when `index` is out of range;
  // the entry is untouched in that case.
  //
  // Applies the same validation as the parse path: references outside the
  // palette are dropped, repeats are collapsed, and the result is capped at
  // MAX_TAGS_PER_HIGHLIGHT. A caller cannot produce an entry that setTags would
  // reject, but a caller CAN produce one whose tags a later removeTag would
  // renumber — so this is validation, not trust.
  bool setTags(size_t index, std::vector<uint16_t> tagIndices);
```

- [ ] **Step 4: Implement, run passing**

Share the validation with `fromJson`'s existing tag-reference handling rather than writing a second copy — a divergence between the two is exactly the class of bug that produced the earlier `classifyDocRead` finding.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
```

Expected **224** host tests (216 + 8).

---

### Task 2: `TagPickerActivity` persists palette changes

**Files:** `src/activities/reader/TagPickerActivity.{h,cpp}`, `src/activities/reader/PassageSelectActivity.cpp`

- [ ] **Step 1: Extend the constructor**

Match `HighlightsActivity`'s shape:

```cpp
  explicit TagPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                             std::string bookPath, bool saveDisabled,
                             std::vector<uint16_t> initialSelection = {});
```

- [ ] **Step 2: Save immediately after `addTag` succeeds**

The palette is book-wide state; its durability must not depend on whether the caller later saves the highlight. On `SaveResult::TooLarge` or `WriteFailed`, report via `ReaderUtils::showMessage` and **remove the just-added tag** so memory and disk agree — mirroring `PassageSelectActivity`'s rollback on a failed save.

Respect `saveDisabled`: if `HighlightFile::load` previously returned `Failed`, the palette must not be written either.

- [ ] **Step 3: Update the class comment**

The header currently states the add is *"NOT rolled back on cancel"*. After this task it is persisted immediately and rolled back on save failure. Rewrite it to say what is now true.

- [ ] **Step 4: Update the one construction site**

`PassageSelectActivity.cpp:200` — pass `bookPath` and `saveDisabled`, both of which that activity already holds.

- [ ] **Step 5: Build both boards and commit**

---

### Task 3: Delete a tag from the palette

**Files:** `src/activities/reader/TagPickerActivity.{h,cpp}`

- [ ] **Step 1: Long-press a tag row to delete it**

`UiListActivity` already provides `onRowLongPress(int index)`, and `HighlightsActivity` uses exactly this idiom for deleting a highlight — follow it rather than inventing a gesture.

Guard: the "New tag…" row is not a tag and must not be deletable. Check the index maps to a real palette entry.

- [ ] **Step 2: Confirm before deleting**

Use `OptionPopup`, as `HighlightsActivity::showDeleteConfirmation` does. The message must say what the user is actually about to do: deleting a tag **removes it from every highlight that carries it**, across the whole book — not just this one.

Handle the dismiss-without-firing path. `OptionPopup` dismisses on a tap outside the dialog without invoking the callback; `PassageSelectActivity` shipped a bug where that left the activity inert. Mirror `HighlightsActivity::handleCustomInput`'s recovery.

- [ ] **Step 3: Delete, save, rebuild**

Call `HighlightDoc::removeTag` — already covered by six host tests including renumbering — then `HighlightFile::save`.

**Rebuild the row cache immediately after the mutation and before the SD write.** `rowItems_` labels point into `tags()` strings; erasing shifts every later entry and the render task runs concurrently. This is the use-after-free that had to be fixed in `HighlightsActivity::deleteHighlight`; do not reintroduce it.

Also clear or re-validate `selected_`, the `bool[MAX_TAGS]` check state — it is index-aligned with the palette, so a deletion shifts what every checked box means.

- [ ] **Step 4: Build both boards and commit**

---

### Task 4: Edit a saved highlight's tags

**Files:** `src/activities/reader/HighlightsActivity.{h,cpp}`

- [ ] **Step 1: Long-press offers a choice**

`onRowLongPress` currently goes straight to `showDeleteConfirmation` (`HighlightsActivity.cpp:150-154`). Replace that with an `OptionPopup` offering **Tags… / Delete / Cancel**. Delete then shows its existing confirmation — do not remove that second step; it is the only destructive action in the feature.

Same dismiss-without-firing recovery as everywhere else.

- [ ] **Step 2: Launch the picker with the entry's current tags**

```cpp
startActivityForResult(std::make_unique<TagPickerActivity>(renderer, mappedInput, highlightDoc_, bookPath_,
                                                           saveDisabled_, entry.tagIndices),
                       /* result handler */);
```

`initialSelection` already pre-checks those rows (`TagPickerActivity.cpp:26`) — this is the flow that parameter was written for.

- [ ] **Step 3: Write the result back**

In the handler, guard on `!result.isCancelled` before `std::get<TagSelectionResult>` — the build is `-fno-exceptions`, so a mismatch calls `std::terminate`. Then `setTags(docIndex, ...)` and `HighlightFile::save`.

**Do not hold a `HighlightEntry*` or a doc index across the `startActivityForResult` push.** The picker can mutate the palette — and with Task 3, can *delete* a tag, which renumbers references and could change what the entry's own tags mean. Re-resolve the entry after the picker returns, and treat a now-invalid index as a no-op rather than indexing blindly.

- [ ] **Step 4: Rebuild and repaint**

After `setTags`, rebuild the row cache before the save (same reason as Task 3) and `requestUpdate()` so the change is visible without leaving the screen.

- [ ] **Step 5: Build both boards and commit**

---

### Task 5: Verification

- [ ] **Step 1: Full suite and both boards**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

Expected **224** host tests; both firmware targets SUCCESS. **Run the two `pio` invocations sequentially** — they race on a shared `idf_component.yml` during the Arduino-core rebuild.

- [ ] **Step 2: Confirm nothing is dead any more**

```bash
grep -rn 'removeTag' src | grep -v 'HighlightDoc'
grep -rn 'initialSelection' src/activities/reader/HighlightsActivity.cpp
```

Both must now have hits. `removeTag` and `TagPickerActivity::initialSelection` were the two orphans this plan exists to connect.

- [ ] **Step 3: Trace the flows in prose and record them**

Two paths, written out: create-a-tag-then-cancel-the-highlight (does the tag persist?), and delete-a-tag-that-two-highlights-share (do both lose it, and do their other tags still resolve to the same names?).

---

## Deferred to on-device verification

These join the existing Task 8 checklist in the UI plan; none can be judged from a build:

- Long-press discoverability now that it opens a chooser rather than acting directly.
- Whether the tag-deletion warning reads clearly enough that nobody deletes a tag from 40 highlights by accident.
- Whether editing tags from the browser feels like it belongs there, or wants to be reachable from the highlight itself.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| A stale doc index across the picker push | High | Task 4 Step 3 re-resolves after return; a deletion can renumber |
| Row labels aliasing erased strings | High | Rebuild before the SD write, in both Task 3 and Task 4 |
| `selected_` misaligned after a tag deletion | Medium | Task 3 Step 3 clears or re-validates it |
| Palette save failure leaves memory ≠ disk | Medium | Roll back the add, mirroring `PassageSelectActivity` |
| Popup dismissed without firing leaves an inert screen | Medium | Mirror `HighlightsActivity::handleCustomInput` in every new popup |
| Deleting a tag surprises the user | Medium | Confirmation states the book-wide effect explicitly |
