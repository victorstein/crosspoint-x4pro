#pragma once

#include <Epub/HighlightDoc.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Browse this book's highlights (most recent first) and jump to one, filter
// them by tag, or delete one. HighlightDoc is passed by reference and is
// always this ONE book's document -- there is no cross-book list here, and
// the tag filter only ever cycles through highlightDoc.tags(), this book's
// own palette.
//
// Row 0 is a persistent filter control, not a highlight: tapping/confirming
// it cycles filterTagIndex_ through nullopt ("All") -> tag 0 -> tag 1 -> ...
// -> back to "All", narrowing which highlights rows 1.. show. It does NOT
// reuse TagPickerActivity: that picker enforces
// HighlightDoc::MAX_TAGS_PER_HIGHLIGHT (8) and offers "New tag...", both
// correct for tagging one highlight but wrong for a filter, which has no
// reason to cap how many tags narrow the list and has nothing to gain from
// minting a tag no highlight has yet.
//
// A long-press (touch) or a held Confirm release (physical buttons) on a
// highlight row opens a Tags.../Delete/Cancel OptionPopup (actionChooser_),
// not the delete confirmation directly -- editing tags is the other action
// this screen offers. Choosing "Tags..." pushes TagPickerActivity seeded with
// the entry's current tagIndices as its initialSelection; choosing "Delete"
// forces a synchronous clean repaint (requestUpdateAndWait) before opening
// confirmPopup_, since actionChooser_'s three rows are taller than
// confirmPopup_'s two and would otherwise frame it with leftover pixels.
// actionChooser_ and confirmPopup_ are two separate OptionPopup members --
// never the same one reused -- because OptionPopup::show() reassigns
// onSelectCallback and is invoked AS that member, so calling show() again
// from inside a running callback would destroy the closure still executing.
//
// Deleting is the only destructive, irreversible-on-disk action in the
// feature: on confirm, HighlightDoc::removeHighlight runs first and
// HighlightFile::save second; if the save fails the just-removed entry is
// re-added via addHighlight so the resident doc never diverges from what's on
// disk (matching PassageSelectActivity::finalizeSelection's own rollback on a
// failed save). Because addHighlight only appends, a rolled-back entry lands
// at the end of the vector rather than back at its original position --
// accepted here since the alternative (copying the whole document, up to
// HighlightDoc::MAX_HIGHLIGHTS entries, to save one) is the exact memory
// pressure this feature is built to avoid on the C3. This path only runs on
// a save failure, which is rare. Retagging (HighlightDoc::setTags) is
// likewise rolled back to the entry's previous tagIndices on a failed save.
//
// Jumping does NOT route through EpubReaderActivity's
// progressChangeResultHandler -- that lambda opens with
// std::get<ProgressChangeResult>(result.data), which is only safe because
// every OTHER caller of it always returns that alternative; it also calls
// loadCachedBookmarks() and reopens the reader menu on cancel, both wrong for
// a browse screen. Instead this activity returns a ProgressChangeResult with
// hasVisibleTextOffset=true and the highlight's start offset directly; the
// reader's existing offset-based jump branch (immune to re-pagination)
// applies unchanged. Wiring the launch site and its handler is Task 7's job,
// not this activity's.
//
// Home: NOT overridden, unlike PassageSelectActivity and TagPickerActivity.
// Both of those override handleHomeGesture() because the screen has no OTHER
// way to leave carrying a result -- selecting a passage or checking tags
// never itself finishes the activity. Here, activating a row (touch tap, or
// physical Confirm release) already IS the "jump" gesture and already leaves
// the screen, on every board, exactly like EpubReaderBookmarksActivity (the
// same browse-plus-delete shape) which also leaves Home unoverridden. Home
// therefore keeps its ordinary meaning -- ActivityManager::loop() takes an
// unconsumed Home gesture straight to the home screen -- rather than being
// repurposed as a second, redundant "jump" affordance that would make Home
// behave inconsistently with every other browse list in the app.
class HighlightsActivity final : public UiListActivity {
 public:
  explicit HighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                              std::string bookPath, bool saveDisabled);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  // Rebuilds visibleIndices_ from highlightDoc_.highlights() + filterTagIndex_.
  // Indices only, most-recent-first (reverse insertion order) -- never the
  // findBySpine-style raw HighlightEntry* pointers, which addHighlight's
  // push_back (rollback on a failed delete-save) or removeHighlight's erase
  // would invalidate out from under a held pointer.
  void rebuildVisibleIndices();
  // Rebuilds rowSubtitles_/rowItems_ from visibleIndices_ + filterTagIndex_.
  // Called only when the underlying data changes (onEnter, filter cycle,
  // delete), not on every repaint -- mirrors
  // EpubReaderBookmarksActivity::rebuildBookmarkRowItems.
  void rebuildRowItems();
  std::string computeFilterSubtitle() const;
  std::string tagsSubtitleFor(const HighlightEntry& entry) const;

  // Pushes TagFilterActivity and applies its pick. Stepping one tag per tap
  // stopped scaling once the palette cap rose past a handful of tags.
  void openTagFilter();
  void jumpToHighlight(size_t docIndex);
  void showActionChooser(size_t docIndex);
  void editTags(size_t docIndex);
  void applyTagEdit(size_t docIndex, const std::string& filterTagName, const ActivityResult& result);
  void showDeleteConfirmation(size_t docIndex);
  void deleteHighlight(size_t docIndex);

  HighlightDoc& highlightDoc_;
  const std::string bookPath_;
  const bool saveDisabled_;

  // nullopt = no filter ("All"); otherwise an index into highlightDoc_.tags().
  std::optional<uint16_t> filterTagIndex_;

  // Indices into highlightDoc_.highlights(), most-recent-first, filtered by
  // filterTagIndex_. See rebuildVisibleIndices()'s comment for why these are
  // indices and not pointers.
  std::vector<size_t> visibleIndices_;

  // Row 0 mirrors the filter control; rows 1.. mirror visibleIndices_.
  std::string filterSubtitle_;
  std::vector<std::string> rowSubtitles_;
  std::vector<freeink::ui::ListItem> rowItems_;

  bool confirmingDelete_ = false;
  OptionPopup confirmPopup_;
  // Doc index captured when the delete confirmation opens, so the popup's
  // callback (which runs after further input has been processed) deletes the
  // exact entry that was long-pressed rather than re-deriving it from
  // whatever nav.selected happens to be by the time the popup resolves.
  size_t pendingDeleteIndex_ = 0;

  // Separate from confirmPopup_ on purpose -- see the class comment.
  bool choosingAction_ = false;
  OptionPopup actionChooser_;
  // Doc index captured when the action chooser opens, so its callback (Tags...
  // or Delete) dispatches on the exact entry that was long-pressed, same
  // reasoning as pendingDeleteIndex_ above.
  size_t pendingActionIndex_ = 0;
};
