#include "HighlightsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>
#include <variant>

#include "../../util/HighlightFile.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "TagPickerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Matches EpubReaderBookmarksActivity's own threshold for "this Confirm
// release was a hold, not a tap" on boards with a physical Confirm button.
constexpr int ENTER_DELETE_MODE_MS = 700;
}  // namespace

HighlightsActivity::HighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       HighlightDoc& highlightDoc, std::string bookPath, const bool saveDisabled)
    : UiListActivity("Highlights", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      highlightDoc_(highlightDoc),
      bookPath_(std::move(bookPath)),
      saveDisabled_(saveDisabled) {}

void HighlightsActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildVisibleIndices();
  rebuildRowItems();
}

int HighlightsActivity::listCount() const { return static_cast<int>(visibleIndices_.size()) + 1; }

const char* HighlightsActivity::headerTitle() const { return tr(STR_HIGHLIGHTS); }

void HighlightsActivity::rebuildVisibleIndices() {
  visibleIndices_.clear();
  const auto& highlights = highlightDoc_.highlights();
  visibleIndices_.reserve(highlights.size());
  // Most recent first: addHighlight only ever appends, so storage order is
  // oldest-to-newest and "most recent" is the reverse walk.
  for (size_t i = highlights.size(); i-- > 0;) {
    if (filterTagIndex_) {
      const auto& tags = highlights[i].tagIndices;
      if (std::find(tags.begin(), tags.end(), *filterTagIndex_) == tags.end()) continue;
    }
    visibleIndices_.push_back(i);
  }
}

std::string HighlightsActivity::computeFilterSubtitle() const {
  if (!filterTagIndex_) return tr(STR_TAG_FILTER_ALL);
  const auto& tags = highlightDoc_.tags();
  if (*filterTagIndex_ >= tags.size()) return tr(STR_TAG_FILTER_ALL);  // defensive; should not happen
  return tags[*filterTagIndex_];
}

std::string HighlightsActivity::tagsSubtitleFor(const HighlightEntry& entry) const {
  if (entry.tagIndices.empty()) return std::string();
  const auto& tags = highlightDoc_.tags();
  std::string subtitle;
  for (const uint16_t idx : entry.tagIndices) {
    if (idx >= tags.size()) continue;  // defensive; should not happen
    if (!subtitle.empty()) subtitle += ", ";
    subtitle += tags[idx];
  }
  return subtitle;
}

void HighlightsActivity::rebuildRowItems() {
  rowSubtitles_.clear();
  rowItems_.clear();
  rowSubtitles_.reserve(visibleIndices_.size());
  rowItems_.reserve(visibleIndices_.size() + 1);

  filterSubtitle_ = computeFilterSubtitle();
  fui::ListItem filterRow{};
  filterRow.label = tr(STR_FILTER_BY_TAG);
  filterRow.subtitle = filterSubtitle_.c_str();
  filterRow.actionValue = 0;
  rowItems_.push_back(filterRow);

  const auto& highlights = highlightDoc_.highlights();
  for (size_t i = 0; i < visibleIndices_.size(); ++i) {
    const auto& entry = highlights[visibleIndices_[i]];
    rowSubtitles_.push_back(tagsSubtitleFor(entry));

    fui::ListItem item{};
    item.label = entry.label.empty() ? tr(STR_UNNAMED) : entry.label.c_str();
    item.subtitle = rowSubtitles_.back().c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    rowItems_.push_back(item);
  }
}

void HighlightsActivity::cycleTagFilter() {
  const auto& tags = highlightDoc_.tags();
  if (tags.empty()) return;  // nothing to filter by; stays on "All"

  if (!filterTagIndex_) {
    filterTagIndex_ = static_cast<uint16_t>(0);
  } else if (static_cast<size_t>(*filterTagIndex_ + 1) < tags.size()) {
    filterTagIndex_ = static_cast<uint16_t>(*filterTagIndex_ + 1);
  } else {
    filterTagIndex_.reset();
  }

  rebuildVisibleIndices();
  rebuildRowItems();
  moveSelectionTo(0);
}

void HighlightsActivity::jumpToHighlight(const size_t docIndex) {
  if (docIndex >= highlightDoc_.highlights().size()) return;
  const auto& entry = highlightDoc_.highlights()[docIndex];

  // hasVisibleTextOffset=true plus the stored start offset routes through the
  // reader's existing offset-based jump branch (immune to re-pagination); see
  // the class comment for why this bypasses progressChangeResultHandler.
  ProgressChangeResult result;
  result.spineIndex = static_cast<int>(entry.spineIndex);
  result.hasVisibleTextOffset = true;
  result.visibleTextOffset = entry.range.start;
  setResult(std::move(result));
  finish();
}

void HighlightsActivity::activateIndex(const int index) {
  if (confirmPopup_.isActive() || actionChooser_.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  activeNav().selected = index;

  if (index == 0) {
    cycleTagFilter();
    return;
  }

  // Leaving this screen for the reader; a lingering tap flash would gray an
  // unrelated row if the user comes back here later.
  app.clearTapFlash();
  jumpToHighlight(visibleIndices_[static_cast<size_t>(index - 1)]);
}

void HighlightsActivity::onRowLongPress(const int index) {
  if (confirmPopup_.isActive() || actionChooser_.isActive()) return;
  if (index <= 0 || index >= listCount()) return;  // row 0 is the filter control; nothing to act on
  app.clearTapFlash();
  activeNav().selected = index;
  showActionChooser(visibleIndices_[static_cast<size_t>(index - 1)]);
}

void HighlightsActivity::showActionChooser(const size_t docIndex) {
  if (confirmPopup_.isActive() || actionChooser_.isActive()) return;

  pendingActionIndex_ = docIndex;
  choosingAction_ = true;

  if (saveDisabled_) {
    // Retagging writes the document, so a book whose file failed to load
    // (the resident doc was built from scratch this session) must never see
    // "Tags..." at all -- omit it rather than offer it and bail inside
    // editTags. Delete still shows: showDeleteConfirmation carries its own
    // saveDisabled_ bail below, same as before this task.
    const char* options[] = {tr(STR_DELETE), tr(STR_CANCEL)};
    actionChooser_.show(tr(STR_HIGHLIGHT_ACTIONS), options, 2, 0, [this](const int idx) {
      choosingAction_ = false;
      if (idx == 0) {
        // See the "clean repaint" comment below for why this precedes
        // showDeleteConfirmation.
        requestUpdateAndWait();
        showDeleteConfirmation(pendingActionIndex_);
      }
      requestUpdate();
    });
  } else {
    const char* options[] = {tr(STR_EDIT_TAGS), tr(STR_DELETE), tr(STR_CANCEL)};
    actionChooser_.show(tr(STR_HIGHLIGHT_ACTIONS), options, 3, 0, [this](const int idx) {
      choosingAction_ = false;
      if (idx == 0) {
        // Pushes a whole new activity, which repaints the screen from
        // scratch on entry -- no leftover-chooser-pixels hazard here.
        editTags(pendingActionIndex_);
      } else if (idx == 1) {
        // actionChooser_ (3 rows) is taller than confirmPopup_ (2 rows) and
        // both draw over the current screen without clearing it
        // (OptionPopup's class comment), so confirmPopup_ would otherwise be
        // framed by actionChooser_'s leftover pixels. Force one synchronous
        // clean repaint of the underlying list -- with neither popup active
        // -- before showDeleteConfirmation shows confirmPopup_ on top of it.
        // Safe from inside this callback: it runs on the loop task with no
        // RenderLock held (ActivityManager.cpp's three requestUpdateAndWait
        // asserts all pass here), mirroring
        // PassageSelectActivity::showActionChooser's identical use.
        requestUpdateAndWait();
        showDeleteConfirmation(pendingActionIndex_);
      }
      requestUpdate();
    });
  }
  requestUpdate();
}

void HighlightsActivity::editTags(const size_t docIndex) {
  if (docIndex >= highlightDoc_.highlights().size()) return;  // stale index; nothing to do
  if (saveDisabled_) {
    // Defense in depth: showActionChooser already omits "Tags..." in this
    // state, but mirror showDeleteConfirmation's own bail so this method is
    // safe to call regardless of how it's reached.
    ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_LOAD_FAILED));
    requestUpdate();
    return;
  }

  // clearTapFlash is what the row-tap pushes do (activateIndex, above)
  // because a row flash is what lingers. This push comes from a popup
  // button, so it is optional here -- PassageSelectActivity::startTagFlow
  // pushes the same activity from a popup callback without it.
  app.clearTapFlash();
  const std::vector<uint16_t> initialSelection = highlightDoc_.highlights()[docIndex].tagIndices;
  const std::string filterTagName = filterTagIndex_ ? highlightDoc_.tags()[*filterTagIndex_] : std::string();
  startActivityForResult(
      std::make_unique<TagPickerActivity>(renderer, mappedInput, highlightDoc_, bookPath_, saveDisabled_,
                                          initialSelection),
      [this, docIndex, filterTagName](const ActivityResult& result) {
        applyTagEdit(docIndex, filterTagName, result);
      });
}

void HighlightsActivity::applyTagEdit(const size_t docIndex, const std::string& filterTagName,
                                      const ActivityResult& result) {
  // A size_t doc index is stable across the picker push: removeTag erases
  // from tags_ and rewrites tagIndices in place but never resizes or
  // reorders highlights_, and TagPickerActivity calls neither addHighlight
  // nor removeHighlight. Bounds-checked anyway, defensively.
  if (!result.isCancelled && docIndex < highlightDoc_.highlights().size()) {
    // -fno-exceptions means a mismatched alternative aborts with no
    // recovery, so this must never run on the cancelled path.
    const auto& selection = std::get<TagSelectionResult>(result.data);
    // Capture AFTER the picker returns: it can delete a palette entry, which
    // renumbers every tagIndices in place (HighlightDoc.cpp:30-32). A copy taken
    // before the push names tags by the old numbering.
    const std::vector<uint16_t> previousTags = highlightDoc_.highlights()[docIndex].tagIndices;
    highlightDoc_.setTags(docIndex, selection.tagIndices);

    if (!saveDisabled_) {
      switch (HighlightFile::save(bookPath_, highlightDoc_)) {
        case HighlightFile::SaveResult::Ok:
          break;
        case HighlightFile::SaveResult::TooLarge:
          // Never leave the resident doc holding tags that aren't actually on
          // disk -- restore the entry's previous tagIndices, mirroring
          // deleteHighlight's own rollback on a failed save.
          highlightDoc_.setTags(docIndex, previousTags);
          ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_TOO_LARGE));
          break;
        case HighlightFile::SaveResult::WriteFailed:
          highlightDoc_.setTags(docIndex, previousTags);
          ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_SAVE_FAILED));
          break;
      }
    }
  }

  // The picker persists palette changes (new tags, tag deletion) itself, so
  // even a user who backed out of this edit (isCancelled) may have already
  // changed highlightDoc_.tags() on disk -- reconcile below on BOTH paths.
  // Reconcile by name, not by index or count: the picker can delete AND add in
  // one visit, leaving the size unchanged while the numbering shifts underneath.
  if (filterTagIndex_) {
    const auto& tags = highlightDoc_.tags();
    const auto it = std::find(tags.begin(), tags.end(), filterTagName);
    filterTagIndex_ = (it == tags.end())
                          ? std::nullopt
                          : std::optional<uint16_t>(static_cast<uint16_t>(std::distance(tags.begin(), it)));
  }
  {
    // rebuildVisibleIndices/rebuildRowItems refill the vector buildScreen
    // hands the render task as rowItems_.data(); the render lock is
    // explicitly released before this handler runs (ActivityManager.cpp),
    // so nothing else serialises this against a concurrent render.
    RenderLock lock(*this);
    rebuildVisibleIndices();
    rebuildRowItems();
  }
  // The rebuild can shrink visibleIndices_ (filter reset above, or the
  // filtered tag itself deleted); moveSelectionTo issues its own
  // requestUpdate().
  moveSelectionTo(std::clamp(activeNav().selected, 0, listCount() - 1));
}

void HighlightsActivity::showDeleteConfirmation(const size_t docIndex) {
  if (confirmPopup_.isActive() || actionChooser_.isActive()) return;
  if (saveDisabled_) {
    // The file may still hold the user's data (HighlightFile::LoadResult::Failed);
    // never let a delete through in that state, matching PassageSelectActivity's
    // own saveDisabled bail.
    ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_LOAD_FAILED));
    requestUpdate();
    return;
  }

  pendingDeleteIndex_ = docIndex;
  confirmingDelete_ = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup_.show(tr(STR_CONFIRM_DELETE_HIGHLIGHT), options, 2, 0, [this](const int idx) {
    confirmingDelete_ = false;
    if (idx == 1) deleteHighlight(pendingDeleteIndex_);
    requestUpdate();
  });
  requestUpdate();
}

void HighlightsActivity::deleteHighlight(const size_t docIndex) {
  if (docIndex >= highlightDoc_.highlights().size()) return;  // stale index; nothing to do

  // Copy the one entry before removing it -- NOT the whole document -- so a
  // failed save can roll back without doubling HighlightDoc's resident
  // footprint (up to MAX_HIGHLIGHTS entries) on the C3. See the class comment
  // for why the rollback can land at the end of the vector rather than back
  // at its original position.
  HighlightEntry removed = highlightDoc_.highlights()[docIndex];
  highlightDoc_.removeHighlight(docIndex);
  // Rebuild before the SD save, not after: rowItems_[i].label aliases each
  // entry's std::string storage (see rebuildRowItems), and the render task
  // runs concurrently under RenderLock -- it must never see rows aliasing
  // the erased/moved storage while the save is in flight.
  rebuildVisibleIndices();
  rebuildRowItems();

  switch (HighlightFile::save(bookPath_, highlightDoc_)) {
    case HighlightFile::SaveResult::Ok:
      break;
    case HighlightFile::SaveResult::TooLarge:
      highlightDoc_.addHighlight(std::move(removed));
      // A rolled-back entry re-appended by addHighlight is a different
      // document index than the one just removed, so visibleIndices_ (and
      // everything derived from it) must not be patched in place -- only a
      // full rebuild from highlightDoc_ is safe here, again before rowItems_
      // can be read by a concurrent render.
      rebuildVisibleIndices();
      rebuildRowItems();
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_TOO_LARGE));
      break;
    case HighlightFile::SaveResult::WriteFailed:
      highlightDoc_.addHighlight(std::move(removed));
      rebuildVisibleIndices();
      rebuildRowItems();
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_SAVE_FAILED));
      break;
  }

  moveSelectionTo(std::clamp(activeNav().selected, 0, listCount() - 1));
}

bool HighlightsActivity::handleCustomInput() {
  if (actionChooser_.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (choosingAction_) {
    // Popup dismissed without a selection (Back button/gesture, or a tap
    // outside it): cancel the pending action, stay on this screen.
    choosingAction_ = false;
    requestUpdate();
    return true;
  }

  if (confirmPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete_) {
    // Popup dismissed without a selection (Back button/gesture, or a tap
    // outside it): cancel the pending delete, stay on this screen.
    confirmingDelete_ = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool HighlightsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected < 0 || selected >= listCount()) return true;
    // Matches EpubReaderBookmarksActivity: a held Confirm release on a
    // highlight row (not row 0, the filter control, which has nothing to
    // delete) opens the delete confirmation instead of jumping.
    if (selected > 0 && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
      onRowLongPress(selected);
    } else {
      activateIndex(selected);
    }
    return true;
  }

  return false;
}

void HighlightsActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void HighlightsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Nothing to browse or filter: skip the filter row entirely rather than
  // show a control that can only ever read "All" over an empty list.
  if (highlightDoc_.highlights().empty()) {
    screen.centeredText(tr(STR_NO_HIGHLIGHTS), screen.theme().bodyText);
    return;
  }

  // "Hold Open to Delete" names a physical button; on touch boards the row
  // long-press covers deletion instead, so the hint would be wrong there --
  // matches EpubReaderBookmarksActivity's own gating.
  if (!mappedInput.hasTouch()) {
    const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpLineHeight},
                     tr(STR_HOLD_OPEN_TO_DELETE));
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  // Tap opens/cycles; long-press deletes (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void HighlightsActivity::render(RenderLock&&) {
  // Duplicates UiListActivity::render()'s body (chrome, app, rebuild-retry
  // loop, footer, display) with the delete-confirmation popup interleaved
  // between the app render and the footer -- exactly where
  // EpubReaderBookmarksActivity's own render() puts the same check, and for
  // the same reason: the base render() has no seam to inject it into.
  renderer.clearScreen();
  drawChrome();
  renderUi();
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }

  // Chooser first: processRender paints the dialog AND calls
  // renderer.displayBuffer(), returning true (OptionPopup.h) -- calling both
  // and falling through would paint the footer over the dialog and trigger a
  // second e-ink refresh.
  if (actionChooser_.processRender(renderer, mappedInput)) return;
  if (confirmPopup_.processRender(renderer, mappedInput)) return;

  drawFooter();
  renderer.displayBuffer();
}
