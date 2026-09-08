#include "TagFilterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TagFilterActivity::TagFilterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const HighlightDoc& highlightDoc)
    : UiListActivity("TagFilter", renderer, mappedInput, /*wantsTouchLongPress=*/false),
      highlightDoc_(highlightDoc) {}

int TagFilterActivity::listCount() const { return static_cast<int>(highlightDoc_.tags().size()) + 1; }

const char* TagFilterActivity::headerTitle() const { return tr(STR_FILTER_BY_TAG); }

void TagFilterActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TagFilterActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                     static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                     static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) +
                                                          metrics.buttonHintsHeight),
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
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
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
