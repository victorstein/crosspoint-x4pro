#pragma once

#include <Epub/HighlightDoc.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Multi-select picker over a book's tag palette (HighlightDoc::tags()), plus
// a "New tag..." row that pushes KeyboardEntryActivity and calls
// HighlightDoc::addTag. Returns the checked indices as a TagSelectionResult;
// the caller applies them to whichever highlight it is tagging.
//
// The X4 Pro has no physical Back/Confirm (see PassageSelectActivity's class
// comment for the general mechanism). Checking a row here never leaves the
// screen -- there is no button-handling path that finishes the activity on
// its own -- so, exactly like PassageSelectActivity, this activity
// repurposes the capacitive Home key (handleHomeGesture()) as the picker's
// Done gesture, committing the current selection. Back cancels
// (isCancelled=true, selection discarded), matching both PassageSelectActivity's
// precedent and the codebase-wide Back=cancel convention. A tag created via
// "New tag..." is added to the book's palette AND persisted to disk
// immediately (HighlightFile::save, unless saveDisabled) -- the palette is
// book-wide state, independent of which highlight ends up tagged, so a
// highlight cancelled after this point must not take the new tag down with
// it. If that save fails, the just-added tag is rolled back via removeTag;
// a dedupe hit (addTag returning an existing index rather than adding one)
// is never rolled back, since nothing new was added and removeTag would
// strip a pre-existing tag off every highlight in the book.
//
// A tag row can also be long-pressed (touch) or held-Confirm-released
// (physical buttons) to delete it from the palette entirely -- this removes
// it from EVERY highlight in the book that carries it, not just the one
// being tagged here, so a confirmation dialog names that scope explicitly.
// Unlike the add path above, a failed delete-save cannot be rolled back:
// removeTag rewrites every highlight's tag references in place and the set
// of highlights that carried the tag is not retained, so the failure is
// surfaced to the user instead.
class TagPickerActivity final : public UiListActivity {
 public:
  explicit TagPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                              std::string bookPath, bool saveDisabled, std::vector<uint16_t> initialSelection = {});

  void onEnter() override;
  bool handleHomeGesture() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int MAX_ROWS = static_cast<int>(HighlightDoc::MAX_TAGS) + 1;  // palette + "New tag..." row

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void toggleTag(size_t index);
  void startNewTagFlow();
  void reportAddTagFailure(const std::string& name);
  void commitAndFinish();
  void showDeleteConfirmation(size_t tagIndex);
  void deleteTag(size_t tagIndex);

  HighlightDoc& highlightDoc;
  const std::string bookPath_;
  const bool saveDisabled_;
  std::vector<uint16_t> initialSelection_;

  bool confirmingDelete_ = false;
  OptionPopup confirmPopup_;
  // Tag index captured when the delete confirmation opens, so the popup's
  // callback (which runs after further input has been processed) deletes the
  // exact row that was long-pressed rather than re-deriving it from whatever
  // nav.selected happens to be by the time the popup resolves.
  size_t pendingDeleteIndex_ = 0;

  // Index-aligned with highlightDoc.tags(). Fixed at MAX_TAGS capacity, but
  // the palette is no longer grow-only: a long-press/held-Confirm delete can
  // shrink it too. On delete, entries above the removed index are shifted
  // down (never simply cleared -- see TagPickerActivity.cpp's deleteTag) so
  // every remaining slot keeps meaning "is highlightDoc.tags()[i] checked".
  bool selected_[HighlightDoc::MAX_TAGS]{};

  // Rebuilt from highlightDoc.tags() on every buildScreen() call, never
  // cached across visits: a short tag name can live in std::string's small
  // buffer, whose address moves if the tags vector reallocates after
  // "New tag..." adds an entry -- caching label pointers across that round
  // trip would leave rowItems_ pointing at freed memory.
  freeink::ui::ListItem rowItems_[MAX_ROWS]{};
};
