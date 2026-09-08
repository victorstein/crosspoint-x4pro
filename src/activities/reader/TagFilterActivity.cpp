#include "TagFilterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "../../util/HighlightFile.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "TagRowMapping.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TagFilterActivity::TagFilterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                                     std::string bookPath, const bool saveDisabled)
    : UiListActivity("TagFilter", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      highlightDoc_(highlightDoc),
      bookPath_(std::move(bookPath)),
      saveDisabled_(saveDisabled) {}

int TagFilterActivity::listCount() const { return static_cast<int>(highlightDoc_.tags().size()) + 1; }

const char* TagFilterActivity::headerTitle() const { return tr(STR_FILTER_BY_TAG); }

void TagFilterActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TagFilterActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Rebuilt every call rather than cached: label pointers borrow the palette's
  // std::string buffers, which a caller-side edit could have moved since the
  // last visit.
  const auto& tags = highlightDoc_.tags();
  rowItems_.clear();
  rowItems_.reserve(tags.size() + 1);

  fui::ListItem allItem{};
  allItem.label = tr(STR_TAG_FILTER_ALL);
  allItem.actionValue = 0;
  rowItems_.push_back(allItem);

  for (size_t i = 0; i < tags.size(); ++i) {
    fui::ListItem item{};
    item.label = tags[i].c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    rowItems_.push_back(item);
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void TagFilterActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool TagFilterActivity::handleHomeGesture() {
  // Consumed rather than left to ActivityManager's "go home", which would
  // abandon the reader entirely from a filter screen. Cancelling matches Back.
  onBackButton();
  return true;
}

void TagFilterActivity::onRowLongPress(const int row) {
  if (confirmPopup_.isActive()) return;
  // Row 0 is "all tags" and maps to no palette entry. TagRows is the same
  // conversion the picker uses and is host-tested.
  const int tagIndex = TagRows::tagIndexForRow(row, static_cast<int>(highlightDoc_.tags().size()));
  if (tagIndex < 0) return;
  app.clearTapFlash();
  nav.selected = row;
  showDeleteConfirmation(static_cast<size_t>(tagIndex));
}

void TagFilterActivity::showDeleteConfirmation(const size_t tagIndex) {
  if (confirmPopup_.isActive()) return;
  if (saveDisabled_) {
    // The file may still hold the user's data; never let a destructive palette
    // change through in that state, matching the picker's own bail.
    ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_LOAD_FAILED));
    requestUpdate();
    return;
  }

  pendingDeleteIndex_ = tagIndex;
  confirmingDelete_ = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup_.show(tr(STR_CONFIRM_DELETE_TAG), options, 2, 0, [this](const int idx) {
    confirmingDelete_ = false;
    if (idx == 1) deleteTag(pendingDeleteIndex_);
    requestUpdate();
  });
  requestUpdate();
}

void TagFilterActivity::deleteTag(const size_t tagIndex) {
  if (tagIndex >= highlightDoc_.tags().size()) return;  // stale index; nothing to do

  {
    // rowItems_ borrows label pointers from the palette's std::string buffers
    // and is rebuilt inline in buildScreen, so the mutation itself must be
    // fenced against the render task -- same reasoning as the picker's delete.
    RenderLock lock(*this);
    highlightDoc_.removeTag(static_cast<uint16_t>(tagIndex));
  }
  requestUpdate();

  // SD write once the lock is released: save is read-only on the document.
  switch (HighlightFile::save(bookPath_, highlightDoc_)) {
    case HighlightFile::SaveResult::Ok:
      break;
    case HighlightFile::SaveResult::TooLarge:
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_TOO_LARGE));
      break;
    case HighlightFile::SaveResult::WriteFailed:
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_SAVE_FAILED));
      break;
  }
}

bool TagFilterActivity::handleCustomInput() {
  if (confirmPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete_) {
    // Popup dismissed without choosing (Back, or a tap outside it): drop the
    // pending delete and stay here.
    confirmingDelete_ = false;
    requestUpdate();
    return true;
  }
  return false;
}

void TagFilterActivity::render(RenderLock&&) {
  // Mirrors UiListActivity::render()'s body with the confirmation interleaved
  // between the app render and the footer; the base render has no seam for it.
  renderer.clearScreen();
  drawChrome();
  renderUi();
  for (int pass = 0; nav.consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }

  if (confirmPopup_.processRender(renderer, mappedInput)) return;

  drawFooter();
  renderer.displayBuffer();
}

void TagFilterActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  nav.selected = index;

  TagSelectionResult result;
  // Row 0 is "all tags" and returns an empty selection; every later row maps
  // back to tags()[index - 1].
  if (index > 0) result.tagIndices.push_back(static_cast<uint16_t>(index - 1));
  setResult(std::move(result));
  finish();
}
