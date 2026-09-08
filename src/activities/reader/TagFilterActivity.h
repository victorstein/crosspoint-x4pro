#pragma once

#include <Epub/HighlightDoc.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Single-select tag chooser for the highlights browser's filter row. Returns a
// TagSelectionResult holding no elements for "all tags" or exactly one for a
// specific tag; a cancelled result means the caller keeps its current filter.
//
// A long-press deletes a tag from the palette, so this DOES mutate it and
// removeTag renumbers every index in place. The returned index is therefore
// resolved against the palette as it stands when the row is activated, and the
// caller must re-resolve its own active filter by NAME rather than trusting an
// index it captured before the push -- the bug 1210b1d4 fixed.
class TagFilterActivity final : public UiListActivity {
 public:
  TagFilterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                    std::string bookPath, bool saveDisabled);

 private:
  // Both exits MUST set a result. UiListActivity::onBackButton is a bare
  // finish(), which leaves ActivityResult default-constructed: isCancelled
  // false and the variant holding monostate. A consumer that then reads its
  // own alternative calls std::get on the wrong one, and with -fno-exceptions
  // that aborts.
  void onBackButton() override;
  bool handleHomeGesture() override;
  // Long-press a tag row to delete it from the palette. Book-wide, like the
  // picker's own delete, so it is confirmed first.
  void onRowLongPress(int row) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void showDeleteConfirmation(size_t tagIndex);
  void deleteTag(size_t tagIndex);

  HighlightDoc& highlightDoc_;
  const std::string bookPath_;
  const bool saveDisabled_;

  bool confirmingDelete_ = false;
  OptionPopup confirmPopup_;
  // Captured when the confirmation opens so the callback deletes the row that
  // was long-pressed, not whatever nav.selected became by the time it resolves.
  size_t pendingDeleteIndex_ = 0;
  // Sized to the live palette rather than MAX_TAGS: the cap is 100 and a fixed
  // array would cost ~5KB for a palette that is usually a fraction of that.
  std::vector<freeink::ui::ListItem> rowItems_;
};
