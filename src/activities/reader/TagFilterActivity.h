#pragma once

#include <Epub/HighlightDoc.h>

#include <vector>

#include "activities/UiListActivity.h"

// Single-select tag chooser for the highlights browser's filter row. Returns a
// TagSelectionResult holding no elements for "all tags" or exactly one for a
// specific tag; a cancelled result means the caller keeps its current filter.
//
// Unlike TagPickerActivity this never mutates the palette, so no tag
// renumbering can happen across the push and the returned index stays valid
// against the same tags() the caller read before launching.
class TagFilterActivity final : public UiListActivity {
 public:
  TagFilterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const HighlightDoc& highlightDoc);

 private:
  // Both exits MUST set a result. UiListActivity::onBackButton is a bare
  // finish(), which leaves ActivityResult default-constructed: isCancelled
  // false and the variant holding monostate. A consumer that then reads its
  // own alternative calls std::get on the wrong one, and with -fno-exceptions
  // that aborts.
  void onBackButton() override;
  bool handleHomeGesture() override;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawFooter() override;

  const HighlightDoc& highlightDoc_;
  // Sized to the live palette rather than MAX_TAGS: the cap is 100 and a fixed
  // array would cost ~5KB for a palette that is usually a fraction of that.
  std::vector<freeink::ui::ListItem> rowItems_;
};
