# Tag Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the tag palette two-way — a tag can be deleted, and a saved highlight's tags can be changed.

**Architecture:** One new pure `HighlightDoc` method (host-tested), plus two UI affordances. `TagPickerActivity` gains the ability to persist palette changes itself, which also closes an existing durability hole.

**Tech Stack:** C++20, PlatformIO, host CMake + GoogleTest.

**Depends on:** foundations, anchoring, data-layer and UI plans — all complete. Baseline **216 host tests** at `7672061a`.

**Delivery:** fork-only.

> **v3 — revised after two adversarial reviews.** v1 contained four instructions that would have shipped broken behaviour (a rollback that deletes a pre-existing tag, a long-press that is dead on arrival, nested popups that free the running callback, and a `selected_` reset that wipes the user's tags), two guaranteed build/verification failures, and three misdiagnoses that would have put defensive code in the wrong place while leaving the real stale state untouched. All corrected below.

---

## Why this exists

The tag palette is a one-way door:

- **`HighlightDoc::removeTag` is called only by tests** (`test/highlight_doc/HighlightDocTest.cpp:198-271`, six tests including index renumbering). No UI reaches it.
- **A typo in "New tag…" is permanent**, and the palette is book-wide.
- **`MAX_TAGS` is 32.** Once full, `addTag` returns `nullopt` forever.
- **A highlight's tags are set once, at creation.** `TagPickerActivity::initialSelection` pre-checks rows (`TagPickerActivity.cpp:26`) but no caller passes it.

## An existing durability hole this closes

`TagPickerActivity` takes only `HighlightDoc&` and cannot save. A new tag reaches disk only if the caller later saves. Create a tag, cancel the highlight, and it vanishes on reload.

**Decision: the picker persists palette mutations itself.** It gains `bookPath` and `saveDisabled`, matching `HighlightsActivity`'s constructor shape.

> Note: `saveDisabled` is currently dead on the only existing call site — `PassageSelectActivity::onEnter` bails before the picker is reachable when it is true (`PassageSelectActivity.cpp:26-30`). It becomes live via Task 4's launch from `HighlightsActivity`. Wire it anyway.

## Out of scope

No cross-book tags, no tag renaming (`removeTag` + `addTag` is the available primitive), no merge on duplicate names (`addTag` already dedupes by name).

---

## File Structure

| File | Responsibility |
| --- | --- |
| `lib/Epub/Epub/HighlightDoc.h` / `.cpp` (modify) | `setTags` |
| `test/highlight_doc/HighlightDocTest.cpp` (modify) | 8 host tests |
| `src/activities/reader/TagPickerActivity.{h,cpp}` (modify) | Persist palette changes; long-press to delete a tag |
| `src/activities/reader/HighlightsActivity.{h,cpp}` (modify) | Long-press offers Tags / Delete / Cancel |
| `src/activities/reader/PassageSelectActivity.cpp` (modify) | Pass the new constructor arguments |
| `lib/I18n/translations/english.yaml` (modify) | 3 new strings — see Tasks 3 and 4 |

---

### Task 1: `HighlightDoc::setTags`

`highlights()` returns a `const&`, so an entry's tags cannot be changed. This is the only new pure logic, and the only host-testable part.

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

TEST(HighlightDocSetTags, RejectsAnOutOfRangeEntryIndexWithoutTouchingAnything) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("alpha").has_value());
  doc.addHighlight(makeEntry(0, 0, 10, {0}));

  EXPECT_FALSE(doc.setTags(7, {}));
  EXPECT_FALSE(doc.setTags(1, {})) << "one past the end is out of range";
  // The contract says the entry is untouched on rejection — assert it, or an
  // implementation that clobbers before range-checking passes.
  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
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
  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u);
  ASSERT_EQ(doc.highlights()[1].tagIndices.size(), 1u);
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
  ASSERT_EQ(parsed.highlights().size(), 1u) << "the entry itself must survive the round trip";
  ASSERT_EQ(parsed.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "beta");
}
```

Every container is size-asserted before it is indexed, including `parsed.highlights()` itself — against a regression these must fail red rather than over-read a vector.

> **On `RejectsAnOutOfRangeEntryIndexWithoutTouchingAnything`:** be honest about what it proves. It is a real off-by-one test — `EXPECT_FALSE(doc.setTags(1, {}))` catches a `<=` where `<` belongs. It does **not** prove atomicity: an implementation that grabbed `highlights_[index]` and cleared it *before* range-checking would write out of bounds at `[7]`/`[1]`, never touching entry 0, and would still pass. Only a sanitiser build catches that. Do not write a comment claiming otherwise.

`makeEntry(uint16_t, uint32_t, uint32_t, std::vector<uint16_t> = {})` is at `HighlightDocTest.cpp:9-17`; `roundTrip(const HighlightDoc&, HighlightDoc&)` at `:20-28`. Both signatures verified.

- [ ] **Step 2: Run and confirm failure** (`setTags` undeclared)

- [ ] **Step 3: Declare it**

```cpp
  // Replaces entry `index`'s tags. Returns false when `index` is out of range;
  // the entry is untouched in that case.
  //
  // Drops references outside the palette, collapses repeats, and caps at
  // MAX_TAGS_PER_HIGHLIGHT. Note this is deliberately STRICTER than the parse
  // path: fromJson drops out-of-range refs and caps, but does NOT dedupe
  // (HighlightDoc.cpp:111-117), so {"t":[0,0,0]} parses to three copies. A UI
  // caller can produce repeats by toggling; JSON on disk comes from toJson,
  // which never emits them.
  bool setTags(size_t index, std::vector<uint16_t> tagIndices);
```

- [ ] **Step 4: Implement directly — do NOT try to share with `fromJson`**

v1 said to share the validation. That is wrong twice over: `fromJson` does **not** dedupe, so sharing would fail this task's own `DeduplicatesRepeatedIndices` test; and its loop is interleaved with JSON traversal (`JsonVariantConst` iteration, the `ref | -1` default idiom, and validation against a *local* `tags` vector that is only committed at `HighlightDoc.cpp:124-125`, not against `tags_`). Extracting a shared helper means parameterising over both the source range and the palette, to share four lines.

Write the ~6-line loop against `tags_` directly.

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

```cpp
  explicit TagPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                             std::string bookPath, bool saveDisabled,
                             std::vector<uint16_t> initialSelection = {});
```

- [ ] **Step 2: Save after `addTag` — but ONLY when the palette actually grew**

**This is the step v1 got dangerously wrong.** `addTag` dedupes by name and returns the *existing* index without adding anything (`HighlightDoc.cpp:11-18`) — the activity already relies on this (`TagPickerActivity.cpp:124-127`). So a blanket "remove the just-added tag on save failure" would call `removeTag` on a **pre-existing** tag, erasing it from every highlight in the book because the user retyped a name and the SD write failed.

```cpp
  const size_t before = highlightDoc.tags().size();
  const auto tagIndex = highlightDoc.addTag(name);
  if (!tagIndex) { /* existing failure messages: full / empty / too long */ }

  const bool grew = highlightDoc.tags().size() > before;
  if (grew && !saveDisabled_) {
    switch (HighlightFile::save(bookPath_, highlightDoc)) {
      case HighlightFile::SaveResult::Ok:
        break;
      default:
        // Roll back ONLY a genuinely new tag. A dedupe hit added nothing, so
        // there is nothing to undo — and removeTag would strip a tag the user
        // already had off every highlight in the book.
        highlightDoc.removeTag(*tagIndex);
        ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_SAVE_FAILED));
        return;
    }
  }
```

When `grew` is false there is no palette change to persist — **skip the save entirely.** `selected_[*tagIndex]` must be set on both paths.

- [ ] **Step 3: Rewrite the class comment**

`TagPickerActivity.h` currently says the add is *"NOT rolled back on cancel — the palette is book-wide state."* That is now half-true: it is persisted immediately and rolled back only on save failure. State what is true.

- [ ] **Step 4: Update the one construction site**

`PassageSelectActivity.cpp:200`. Both `bookPath` and `saveDisabled` are `const` members of that class (`PassageSelectActivity.h:90,92`) — verified available.

- [ ] **Step 5: Build both boards and commit**

---

### Task 3: Delete a tag from the palette

**Files:** `src/activities/reader/TagPickerActivity.{h,cpp}`, `lib/I18n/translations/english.yaml`

v1 said "`UiListActivity` already provides `onRowLongPress` — follow the idiom." That is true and insufficient: the hook is gated behind **two independent opt-ins** that `TagPickerActivity` does not set, and the popup it opens would never be drawn. Following v1 literally produces a gesture that does nothing, on a build that passes both boards.

- [ ] **Step 1: Enable long-press — all four changes**

1. **Constructor flag.** `TagPickerActivity.cpp:18` passes `UiListActivity("TagPicker", renderer, mappedInput)`; `wantsTouchLongPress` defaults to `false` (`UiListActivity.h:29`). Pass `/*wantsTouchLongPress=*/true`, as `HighlightsActivity.cpp:25` does.
2. **Input mask.** `TagPickerActivity.cpp:75` sets `props.inputMask = fui::InputTouch;`. It must be `fui::InputTouch | fui::InputLongPress` (`HighlightsActivity.cpp:293`).
3. **Popup member + routing.** Add `OptionPopup confirmPopup_;` and `bool confirmingDelete_ = false;`, plus a `handleCustomInput()` override — the base returns `false` (`UiListActivity.h:56`).
4. **Render seam.** `UiListActivity::render()` (`UiListActivity.cpp:151-167`) has no seam to interleave a popup. `HighlightsActivity` had to duplicate the whole body and says so (`HighlightsActivity.cpp:299-303`). Add the same `render(RenderLock&&)` override, popup drawn between the app render and the footer.
5. **A physical-button path.** The four above are all *touch* plumbing. On a board where `MappedInputManager::hasTouch()` is false (`MappedInputManager.cpp:128`) — reachable on `default` — long-press does not exist and tag deletion would be **unreachable while both boards build green**, the exact failure this task was written to eliminate. `HighlightsActivity` needed a fifth change for this and it is in the same file (`HighlightsActivity.cpp:238-249`):

```cpp
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected < 0 || selected >= listCount()) return true;
    if (selected > 0 && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
      onRowLongPress(selected);
    } else {
      activateIndex(selected);
    }
    return true;
  }
```

`TagPickerActivity` has no `handleButtons` override today, so Confirm falls through to the base (`UiListActivity.cpp:51-55`) → `activateIndex` → `toggleTag`. Add the override with the held-Confirm branch (`ENTER_DELETE_MODE_MS = 700`, `HighlightsActivity.cpp:20`), guarding the "New tag…" row as in Step 3.

If you decide tag deletion should be touch-only instead, that is a legitimate choice — but say so explicitly in the plan and add it to "Deferred to on-device verification". Do not leave it undecided.

- [ ] **Step 2: Guard `handleHomeGesture`**

`TagPickerActivity.cpp:161-164` unconditionally commits and finishes. With a dialog open that would commit the selection out from under it. Add `if (confirmPopup_.isActive()) return true;` first.

- [ ] **Step 3: Long-press a tag row, with the "New tag…" row excluded**

`rowActionTrampoline` bounds-checks against `listCount()` (`UiListActivity.cpp:33`), which is `tags().size() + 1` — so `onRowLongPress(tagCount)` **is** delivered for the "New tag…" row. Guard with `if (index < 0 || index >= tagCount) return;`.

- [ ] **Step 4: Confirm, stating the book-wide effect — and bail when saving is disabled**

**Gate the whole delete on `saveDisabled_` before showing the confirmation**, exactly as `showDeleteConfirmation` does (`HighlightsActivity.cpp:156-165`):

```cpp
  if (saveDisabled_) {
    // The file may still hold the user's data (LoadResult::Failed); never let a
    // destructive palette change through in that state.
    ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_LOAD_FAILED));
    requestUpdate();
    return;
  }
```

Then use `OptionPopup`, as `showDeleteConfirmation` does. The message must say deleting a tag **removes it from every highlight that carries it**, not just this one.

Add to `lib/I18n/translations/english.yaml`: **`STR_CONFIRM_DELETE_TAG`** — e.g. *"Delete this tag from every highlight in this book?"*. `gen_i18n.py` is a pre-build script that **exits 1** on a referenced-but-missing key (`gen_i18n.py:873-881`), so a missing string is a hard build failure, not a warning. The other 31 languages fall back to English automatically (`gen_i18n.py:217-222`) — English-only is sufficient.

Handle dismiss-without-firing (a tap outside the dialog invokes no callback). Mirror `HighlightsActivity::handleCustomInput`'s recovery, which clears its flag on the frame *after* the dismissal — `OptionPopup::handleInput` returns `true` on the dismissing call itself.

- [ ] **Step 5: Delete under a `RenderLock`, then save**

v1 said to "rebuild the row cache before the SD write." **That recipe does not transfer.** `TagPickerActivity` has no rebuild method: `rowItems_` is populated inside `buildScreen`, which runs on the **render task** via the base trampoline (`UiListActivity.cpp:27-29`) and is deliberately never cached across visits (`TagPickerActivity.h:59-64`).

The hazard is still real — the loop task erasing from `tags_` while the render task sits between `item.label = tags[i].c_str()` (`TagPickerActivity.cpp:59`) and `screen.list(props)` (`:77`). The right tool is the one `UiListActivity::moveSelectionTo` uses for the same loop-vs-render race (`UiListActivity.cpp:71-79`):

```cpp
  {
    RenderLock lock(*this);
    highlightDoc.removeTag(static_cast<uint16_t>(index));  // removeTag takes uint16_t; row index is int
    // selected_ is index-aligned with the palette; shift it to match.
    for (size_t j = static_cast<size_t>(index); j + 1 < HighlightDoc::MAX_TAGS; ++j) selected_[j] = selected_[j + 1];
    selected_[HighlightDoc::MAX_TAGS - 1] = false;
  }
  requestUpdate();

  // SD write after the lock releases. HighlightFile::save is read-only on the
  // doc, so this is safe outside the lock.
  switch (HighlightFile::save(bookPath_, highlightDoc)) {
    case HighlightFile::SaveResult::Ok:
      break;
    case HighlightFile::SaveResult::TooLarge:
    case HighlightFile::SaveResult::WriteFailed:
      // NOTE: unlike every other mutation in this feature, this one CANNOT be
      // rolled back. removeTag erases the tag and rewrites every highlight's
      // references (HighlightDoc.cpp:21-34); addTag only appends, and the set
      // of highlights that carried the tag was not retained. So memory and disk
      // diverge here and the next successful save commits the deletion. Tell
      // the user rather than failing silently.
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_SAVE_FAILED));
      requestUpdate();
      break;
  }
```

Enumerate both failure cases rather than writing `default:` — the house style does (`HighlightsActivity.cpp:196-214`), and it keeps `-Wswitch` useful if a third result is ever added.

**Shift `selected_`, never clear it.** Clearing looks tidy and is a data-loss path: `initialSelection_` is consumed once in `onEnter` (`TagPickerActivity.cpp:25-28`) and not retained, so in Task 4's retag flow a clear is unrecoverable — `commitAndFinish` would return an empty list and `setTags(docIndex, {})` would strip every tag off the highlight the user was editing.

Zeroing the vacated top slot is **not** optional: `toggleTag` counts checks across the whole fixed array to enforce the per-highlight cap (`TagPickerActivity.cpp:96`), so a stale trailing `true` would make the picker refuse an 8th tag after seven.

- [ ] **Step 6: Fix the now-false member comment**

`TagPickerActivity.h:54-57` says the palette *"can only grow … never shrink"* — that comment is the entire justification for the fixed-capacity array and the index alignment. Rewrite it for the delete path.

- [ ] **Step 7: Build both boards and commit**

---

### Task 4: Edit a saved highlight's tags

**Files:** `src/activities/reader/HighlightsActivity.{h,cpp}`, `lib/I18n/translations/english.yaml`

- [ ] **Step 1: A second popup, not a reused one**

`onRowLongPress` (`HighlightsActivity.cpp:148-154`) goes straight to `showDeleteConfirmation`. It must instead open a chooser offering **Tags… / Delete / Cancel**.

**Do not reuse `confirmPopup_`.** `OptionPopup::show()` reassigns `onSelectCallback` (`OptionPopup.h:42-53`), and the callback is invoked as that same member (`:80-84`) — calling `show()` from inside it destroys the executing closure. Add a second member:

```cpp
  OptionPopup actionChooser_;
  bool choosingAction_ = false;
```

**Gate "Tags…" on `saveDisabled_`.** Retagging writes the document, so on a book whose file failed to load it must not be offered — the resident doc was built from scratch this session and saving would destroy the user's on-disk highlights. Either omit the option when `saveDisabled_` is set, or have its callback show `STR_HIGHLIGHTS_LOAD_FAILED` and return, mirroring `showDeleteConfirmation` (`:156-165`). The existing Delete option is already gated that way; do not leave the new one open.

**Extend the existing popup guards to both popups.** Three sites currently guard on `confirmPopup_.isActive()` — `activateIndex` (`:133`), `onRowLongPress` (`:149`), `showDeleteConfirmation` (`:157`). Each becomes `if (confirmPopup_.isActive() || actionChooser_.isActive()) return;`. Unreachable today (an active popup makes `handleCustomInput` return true before `loop()` routes touch), but they are defence in depth and half-updated guards are how the next change breaks.

- [ ] **Step 2: Route both popups explicitly**

`handleCustomInput()` must, in this order: chooser `handleInput` → chooser dismiss-recovery → confirm `handleInput` → confirm dismiss-recovery.

`render()` must contain **two guarded early-returns in sequence, chooser first** — not two bare calls:

```cpp
  if (actionChooser_.processRender(renderer, mappedInput)) return;
  if (confirmPopup_.processRender(renderer, mappedInput)) return;
  drawFooter();
  renderer.displayBuffer();
```

`processRender` is not a passive draw: it paints the dialog *and* calls `renderer.displayBuffer()`, returning true (`OptionPopup.h:134-141`). Calling both and falling through would paint the footer over the dialog and trigger a second e-ink refresh.

Clear `choosingAction_` **before** calling `showDeleteConfirmation`, mirroring how the existing callback clears `confirmingDelete_` before `deleteHighlight` (`HighlightsActivity.cpp:171`) so the fall-through recovery does not misfire.

`OptionPopup` draws over the current screen without clearing (`OptionPopup.h:15`), and the 3-option chooser is taller than the 2-option confirm — so its remnants would frame the confirm dialog. Force a clean repaint between them with `requestUpdateAndWait()`, as `PassageSelectActivity::showActionChooser` does for the same reason (`PassageSelectActivity.cpp:174-180`).

Add to `english.yaml`: **`STR_HIGHLIGHT_ACTIONS`** (chooser title) and **`STR_EDIT_TAGS`** (the "Tags…" option label).

- [ ] **Step 3: Launch the picker with the entry's current tags**

```cpp
  // clearTapFlash is what the row-tap pushes do (HighlightsActivity.cpp:144,151)
  // because a row flash is what lingers. This push comes from a popup button, so
  // it is optional here — PassageSelectActivity::startTagFlow pushes the same
  // activity from a popup callback without it (PassageSelectActivity.cpp:199).
  app.clearTapFlash();
  const std::vector<uint16_t> initialSelection = highlightDoc_.highlights()[docIndex].tagIndices;
  startActivityForResult(std::make_unique<TagPickerActivity>(renderer, mappedInput, highlightDoc_, bookPath_,
                                                             saveDisabled_, initialSelection),
                         /* handler below */);
```

Naming the local `initialSelection` also makes Task 5's grep meaningful.

- [ ] **Step 4: The result handler — and the stale state v1 named wrongly**

v1 warned that a doc index could be invalidated across the push because "the picker can delete a tag, which renumbers references." **That is false.** `removeTag` erases from `tags_` and rewrites each entry's `tagIndices`; it never resizes or reorders `highlights_` (`HighlightDoc.cpp:21-34`). `TagPickerActivity` calls neither `addHighlight` nor `removeHighlight`. A `size_t` doc index is **stable** across the push — which is exactly what `pendingDeleteIndex_` already relies on (`HighlightsActivity.h:122-125`).

What genuinely goes stale is:

- **`filterTagIndex_`** (`HighlightsActivity.h:107`) — a raw index into `tags()`. Delete a tag below it and the filter silently means a different tag; delete enough and `computeFilterSubtitle` reports "All" (`:58`) while `rebuildVisibleIndices` still filters on the dead index (`:47-50`) — "Filter by tag: All" over an empty list.
- **`visibleIndices_`** — computed against the pre-deletion numbering.
- **`rowSubtitles_`** — rendered tag names (`tagsSubtitleFor`, `:62-72`).

So the handler must:

1. Bounds-check `docIndex` against `highlights().size()` and treat an invalid index as a no-op.
2. On the **committed** path (`!result.isCancelled`), `std::get<TagSelectionResult>`, then `setTags(docIndex, ...)`, then — **only when `!saveDisabled_`** — `HighlightFile::save`. Without that gate this path writes over a book whose file failed to load, defeating the guard `showDeleteConfirmation` and `PassageSelectActivity::onEnter` both install.
3. **On both paths, committed and cancelled**, reconcile the palette-dependent state:

```cpp
  // Captured immediately before startActivityForResult:
  //   const size_t tagsBefore = highlightDoc_.tags().size();
  if (highlightDoc_.tags().size() != tagsBefore) {
    // A range check is NOT enough: deleting a tag BELOW filterTagIndex_ leaves
    // the index in range but silently pointing at a different tag. Any size
    // change means the numbering moved, and there is no way to recover which
    // tag the user meant — so reset to "All".
    filterTagIndex_ = std::nullopt;
  }
  {
    RenderLock lock(*this);
    rebuildVisibleIndices();
    rebuildRowItems();
  }
  moveSelectionTo(std::clamp(activeNav().selected, 0, listCount() - 1));
```

Three things there matter:

- **Size-compare, not range-check.** v2 prescribed re-validating against `tags().size()`, which only catches the case where the filter tag was at or above the deletion *and* was last. It misses the common case the same paragraph diagnoses.
- **Hold `RenderLock` across the rebuilds.** They `clear()` and refill the `rowItems_` vector that `buildScreen` hands the render task as `rowItems_.data()` (`:289`). The result handler runs with the render lock explicitly released (`ActivityManager.cpp:130-131`), so nothing else serialises this. Task 3 Step 5 reasons about exactly this race; apply the same conclusion here.
- **Clamp the selection.** The rebuild can shrink `visibleIndices_` (filter reset, or the filtered tag deleted). `deleteHighlight` already ends this way (`:217`), and `moveSelectionTo` issues the `requestUpdate()` for you.

Point 3 matters because the picker persists palette changes itself (Task 2): a user who deletes a tag and then backs out has already changed the document on disk. v1's `isCancelled` early-return would have skipped every rebuild.

Guard `std::get` on `!result.isCancelled` — the build is `-fno-exceptions` (`platformio.ini:62`), so a mismatched alternative aborts with no recovery.

- [ ] **Step 5: Build both boards and commit**

---

### Task 5: Verification

- [ ] **Step 1: Full suite and both boards**

```bash
cmake --build build/test && ctest --test-dir build/test -j
pio run -e x4pro
pio run -e default
```

Expected **224** host tests; both SUCCESS. **Run the two `pio` invocations sequentially** — they race on a shared `idf_component.yml` during the Arduino-core rebuild.

- [ ] **Step 2: Confirm both orphans are connected**

```bash
grep -rn 'removeTag' src
grep -n 'TagPickerActivity' src/activities/reader/HighlightsActivity.cpp
```

Both must have hits. v1's second grep searched for the token `initialSelection` in `HighlightsActivity.cpp`, which a correct positional call never contains — it would have failed against correct code.

- [ ] **Step 3: Trace three flows in prose and record them**

1. Create a tag, then cancel the highlight — does the tag persist? (It should: the picker saves it.)
2. Delete a tag two highlights share — do both lose it, and do their *other* tags still resolve to the same names?
3. Delete a tag while a filter on a *later* tag is active — what does the filter show afterwards?

---

## Deferred to on-device verification

Joins the UI plan's Task 8 checklist (`docs/superpowers/plans/2026-08-19-highlights-ui.md:458`):

- Long-press discoverability now that it opens a chooser rather than acting directly.
- Whether the tag-deletion warning reads clearly enough that nobody strips a tag off 40 highlights by accident.
- Whether editing tags from the browser feels like it belongs there.

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Rollback deletes a pre-existing tag on a dedupe hit | High | Task 2 Step 2 rolls back only when the palette grew |
| Long-press silently dead (two unset opt-ins) | High | Task 3 Step 1 enumerates all four changes |
| Nested popups free the running callback | High | Task 4 Step 1 adds a second member |
| Clearing `selected_` wipes the edited highlight's tags | High | Task 3 Step 5 shifts, never clears |
| `filterTagIndex_` / `visibleIndices_` / `rowSubtitles_` stale after a palette change | High | Task 4 Step 4 rebuilds on both paths |
| Missing i18n key fails the build | Medium | Tasks 3 and 4 name the three new ids |
| Loop task erases `tags_` mid-render | Medium | Task 3 Step 5 wraps the mutation in a `RenderLock`; Task 4 Step 4 does the same for the rebuilds |
| Retag or tag-delete writes over a `saveDisabled_` book | High | Gated in Task 3 Step 4 and Task 4 Steps 1 and 4 |
| A failed tag-delete save cannot be rolled back | Medium | Unavoidable — `removeTag` is destructive and the affected highlights are not retained. Task 3 Step 5 surfaces the failure to the user and says so in a comment |
| Tag delete unreachable on a non-touch board | Medium | Task 3 Step 1 item 5 adds the held-Confirm path, or the plan declares it touch-only |
