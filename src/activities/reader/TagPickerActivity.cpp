#include "TagPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "../../util/HighlightFile.h"
#include "../util/KeyboardEntryActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "TagRowMapping.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Matches HighlightsActivity's own threshold for "this Confirm release was a
// hold, not a tap" on boards with a physical Confirm button.
constexpr int ENTER_DELETE_MODE_MS = 700;
}  // namespace

TagPickerActivity::TagPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HighlightDoc& highlightDoc,
                                     std::string bookPath, bool saveDisabled, std::vector<uint16_t> initialSelection)
    : UiListActivity("TagPicker", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      highlightDoc(highlightDoc),
      bookPath_(std::move(bookPath)),
      saveDisabled_(saveDisabled),
      initialSelection_(std::move(initialSelection)) {}

void TagPickerActivity::onEnter() {
  UiListActivity::onEnter();

  const size_t tagCount = highlightDoc.tags().size();
  for (const uint16_t index : initialSelection_) {
    if (index < tagCount) selected_[index] = true;
  }
}

int TagPickerActivity::listCount() const { return static_cast<int>(highlightDoc.tags().size()) + 2; }

int TagPickerActivity::tagIndexForRow(const int row) const {
  return TagRows::tagIndexForRow(row, static_cast<int>(highlightDoc.tags().size()));
}

const char* TagPickerActivity::headerTitle() const { return tr(STR_TAGS); }

void TagPickerActivity::drawFooter() {
  // Matches StatusBarSettingsActivity's toggle-list footer: most rows here
  // toggle in place rather than navigating anywhere.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TagPickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Read live every call (not cached from onEnter): "New tag..." can grow
  // the palette mid-visit, and a reallocation would strand any pointer
  // captured on an earlier visit -- see the rowItems_ comment in the header.
  const auto& tags = highlightDoc.tags();
  const int tagCount = static_cast<int>(tags.size());
  // actionValue carries the ROW, not the tag: UiListActivity hands it straight
  // back to activateIndex/onRowLongPress and assigns it to nav.selected, so a
  // tag index here would desync the viewport from the list.
  fui::ListItem doneItem{};
  doneItem.label = tr(STR_DONE);
  doneItem.actionValue = static_cast<int16_t>(DONE_ROW);
  rowItems_[DONE_ROW] = doneItem;

  for (int i = 0; i < tagCount; ++i) {
    fui::ListItem item{};
    item.label = tags[static_cast<size_t>(i)].c_str();
    item.toggle = true;
    item.toggleChecked = selected_[i];
    item.actionValue = static_cast<int16_t>(i + 1);
    rowItems_[i + 1] = item;
  }

  fui::ListItem newTagItem{};
  newTagItem.label = tr(STR_TAG_NEW);
  newTagItem.actionValue = static_cast<int16_t>(tagCount + 1);
  rowItems_[tagCount + 1] = newTagItem;

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(tagCount + 2);
  props.action = ACTION_ROW;
  // Tap toggles/opens; long-press deletes (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void TagPickerActivity::activateIndex(const int row) {
  const int tagCount = static_cast<int>(highlightDoc.tags().size());
  if (row < 0 || row > tagCount + 1) return;
  nav.selected = row;

  if (row == DONE_ROW) {
    commitAndFinish();
    return;
  }
  if (row == tagCount + 1) {
    startNewTagFlow();
    return;
  }
  toggleTag(static_cast<size_t>(tagIndexForRow(row)));
}

void TagPickerActivity::toggleTag(const size_t index) {
  if (index >= HighlightDoc::MAX_TAGS) return;

  if (!selected_[index]) {
    const size_t checked = static_cast<size_t>(std::count(selected_, selected_ + HighlightDoc::MAX_TAGS, true));
    if (checked >= HighlightDoc::MAX_TAGS_PER_HIGHLIGHT) {
      ReaderUtils::showMessage(renderer, tr(STR_TAG_LIMIT_PER_HIGHLIGHT));
      requestUpdate();
      return;
    }
  }

  selected_[index] = !selected_[index];
  requestUpdate();
}

void TagPickerActivity::startNewTagFlow() {
  // The row is activated via tap or Confirm and either way we're leaving
  // this screen for the keyboard; a lingering tap flash would gray an
  // unrelated row on return.
  app.clearTapFlash();

  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto& keyboard = std::get<KeyboardResult>(result.data);

    const size_t before = highlightDoc.tags().size();
    std::optional<uint16_t> tagIndex;
    {
      // buildScreen (render task) latches item.label = tags[i].c_str() every
      // call; push_back may reallocate tags_, stranding any pointer a
      // concurrent render already took. Fence the mutation itself, matching
      // deleteTag's own RenderLock below.
      RenderLock lock(*this);
      tagIndex = highlightDoc.addTag(keyboard.text);
    }
    if (!tagIndex) {
      reportAddTagFailure(keyboard.text);
      return;
    }

    // addTag dedupes by name, so an existing tag with this name comes back
    // as its existing index rather than a new row. Either way, going
    // through "New tag..." reads as intent to apply it to this highlight.
    const bool grew = highlightDoc.tags().size() > before;
    if (grew && !saveDisabled_) {
      switch (HighlightFile::save(bookPath_, highlightDoc)) {
        case HighlightFile::SaveResult::Ok:
          break;
        case HighlightFile::SaveResult::TooLarge:
        case HighlightFile::SaveResult::WriteFailed:
          // Roll back ONLY a genuinely new tag. A dedupe hit added nothing,
          // so there is nothing to undo -- and removeTag would strip a tag
          // the user already had off every highlight in the book. erase()
          // destroys a std::string and shifts the rest, dangling any
          // const char* a concurrent buildScreen already latched -- fence it,
          // but never across HighlightFile::save (see deleteTag's own note).
          {
            RenderLock lock(*this);
            highlightDoc.removeTag(*tagIndex);
          }
          ReaderUtils::showMessage(renderer, tr(STR_TAG_SAVE_FAILED));
          requestUpdate();
          return;
      }
    }

    selected_[*tagIndex] = true;
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TAG_NAME_PROMPT), std::string(),
                                              HighlightDoc::MAX_TAG_NAME_BYTES, InputType::Text),
      handler);
}

void TagPickerActivity::reportAddTagFailure(const std::string& name) {
  // Mirrors HighlightDoc::addTag's own precedence (name validity is checked
  // before the palette-full case) so this needs no failure code out of
  // addTag to say something true. The too-long branch is normally
  // unreachable through this activity -- KeyboardEntryActivity's maxLength
  // already blocks typing past MAX_TAG_NAME_BYTES -- kept for defense in
  // depth against any other caller of this same helper later.
  if (name.empty()) {
    ReaderUtils::showMessage(renderer, tr(STR_TAG_NAME_EMPTY));
  } else if (name.size() > HighlightDoc::MAX_TAG_NAME_BYTES) {
    ReaderUtils::showMessage(renderer, tr(STR_TAG_NAME_TOO_LONG));
  } else {
    ReaderUtils::showMessage(renderer, tr(STR_TAG_PALETTE_FULL));
  }
  requestUpdate();
}

void TagPickerActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool TagPickerActivity::handleHomeGesture() {
  // A dialog open means the current selection is mid-edit; committing it out
  // from under the confirmation would finish the activity with a result the
  // user never confirmed leaving.
  if (confirmPopup_.isActive()) return true;
  commitAndFinish();
  return true;
}

void TagPickerActivity::commitAndFinish() {
  TagSelectionResult result;
  const auto& tags = highlightDoc.tags();
  for (size_t i = 0; i < tags.size(); ++i) {
    if (selected_[i]) result.tagIndices.push_back(static_cast<uint16_t>(i));
  }
  setResult(std::move(result));
  finish();
}

void TagPickerActivity::onRowLongPress(const int row) {
  if (confirmPopup_.isActive()) return;
  // Delivered for every row within listCount(), including "Done" and
  // "New tag...", neither of which has anything to delete.
  const int tagIndex = tagIndexForRow(row);
  if (tagIndex < 0) return;
  app.clearTapFlash();
  nav.selected = row;
  showDeleteConfirmation(static_cast<size_t>(tagIndex));
}

void TagPickerActivity::showDeleteConfirmation(const size_t tagIndex) {
  if (confirmPopup_.isActive()) return;
  if (saveDisabled_) {
    // The file may still hold the user's data (HighlightFile::LoadResult::Failed);
    // never let a destructive palette change through in that state, matching
    // HighlightsActivity::showDeleteConfirmation's own saveDisabled bail.
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

void TagPickerActivity::deleteTag(const size_t tagIndex) {
  if (tagIndex >= highlightDoc.tags().size()) return;  // stale index; nothing to do

  {
    // The render task reads selected_/highlightDoc.tags() mid-buildScreen
    // (item.label = tags[i].c_str(), item.toggleChecked = selected_[i]);
    // TagPickerActivity has no rebuild-then-save recipe to fall back on --
    // rowItems_ is populated inline in buildScreen and never cached (see the
    // header's rowItems_ comment) -- so the mutation itself must be fenced,
    // the same tool UiListActivity::moveSelectionTo uses for the identical
    // loop-vs-render race.
    RenderLock lock(*this);
    highlightDoc.removeTag(static_cast<uint16_t>(tagIndex));
    // selected_ is index-aligned with the palette; shift it to match so a
    // still-checked tag above the deleted one keeps meaning the same tag.
    // Never just clear selected_[tagIndex] and leave the rest -- that would
    // silently reassign every higher slot to the wrong tag.
    for (size_t j = tagIndex; j + 1 < HighlightDoc::MAX_TAGS; ++j) selected_[j] = selected_[j + 1];
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
      // of highlights that carried the tag was not retained. So memory and
      // disk diverge here and the next successful save commits the deletion.
      // Tell the user rather than failing silently.
      ReaderUtils::showMessage(renderer, tr(STR_TAG_SAVE_FAILED));
      requestUpdate();
      break;
  }
}

bool TagPickerActivity::handleCustomInput() {
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

bool TagPickerActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = nav.selected;
    if (selected < 0 || selected >= listCount()) return true;
    // Matches HighlightsActivity: a held Confirm release on a TAG row opens the
    // delete confirmation instead of toggling. Gated on the row->tag conversion,
    // never on a raw count: comparing a row index against a tag count would put
    // "Done" inside the delete range and leave the last tag unreachable.
    if (tagIndexForRow(selected) >= 0 && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
      onRowLongPress(selected);
    } else {
      activateIndex(selected);
    }
    return true;
  }

  return false;
}

void TagPickerActivity::render(RenderLock&&) {
  // Duplicates UiListActivity::render()'s body (chrome, app, rebuild-retry
  // loop, footer, display) with the delete-confirmation popup interleaved
  // between the app render and the footer -- exactly where
  // HighlightsActivity's own render() puts the same check, and for the same
  // reason: the base render() has no seam to inject it into.
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
