#include "TagPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "../../util/HighlightFile.h"
#include "../util/KeyboardEntryActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TagPickerActivity::TagPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      HighlightDoc& highlightDoc, std::string bookPath, bool saveDisabled,
                                      std::vector<uint16_t> initialSelection)
    : UiListActivity("TagPicker", renderer, mappedInput),
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

int TagPickerActivity::listCount() const { return static_cast<int>(highlightDoc.tags().size()) + 1; }

const char* TagPickerActivity::headerTitle() const { return tr(STR_TAGS); }

void TagPickerActivity::drawFooter() {
  // Matches StatusBarSettingsActivity's toggle-list footer: most rows here
  // toggle in place rather than navigating anywhere.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TagPickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) +
                                                            metrics.buttonHintsHeight),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Read live every call (not cached from onEnter): "New tag..." can grow
  // the palette mid-visit, and a reallocation would strand any pointer
  // captured on an earlier visit -- see the rowItems_ comment in the header.
  const auto& tags = highlightDoc.tags();
  const int tagCount = static_cast<int>(tags.size());
  for (int i = 0; i < tagCount; ++i) {
    fui::ListItem item{};
    item.label = tags[static_cast<size_t>(i)].c_str();
    item.toggle = true;
    item.toggleChecked = selected_[i];
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }

  fui::ListItem newTagItem{};
  newTagItem.label = tr(STR_TAG_NEW);
  newTagItem.actionValue = static_cast<int16_t>(tagCount);
  rowItems_[tagCount] = newTagItem;

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(tagCount + 1);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void TagPickerActivity::activateIndex(const int index) {
  const int tagCount = static_cast<int>(highlightDoc.tags().size());
  if (index < 0 || index > tagCount) return;
  nav.selected = index;

  if (index == tagCount) {
    startNewTagFlow();
    return;
  }
  toggleTag(static_cast<size_t>(index));
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
    const auto tagIndex = highlightDoc.addTag(keyboard.text);
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
          // the user already had off every highlight in the book.
          highlightDoc.removeTag(*tagIndex);
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
