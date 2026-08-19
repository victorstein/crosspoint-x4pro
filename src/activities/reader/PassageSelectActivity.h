#pragma once

#include <Epub/HighlightDoc.h>
#include <Epub/HighlightGeometry.h>
#include <Epub/Page.h>
#include <Epub/VisibleRange.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Two-anchor passage selection over the current reader page, for creating a
// highlight. Tap or arrow to the first word and confirm, then tap or arrow to
// the last word and confirm; a third step then asks Highlight / Tag / Cancel
// via an OptionPopup (the same modal-choice widget EpubReaderMenuActivity and
// HighlightsActivity already use) -- Highlight saves with no tags (the
// original behaviour), Tag pushes TagPickerActivity and applies whatever it
// returns, Cancel discards the selection entirely. The X4 Pro has no physical
// Back/Confirm: Back comes from the touchscreen's left-edge swipe
// (MappedInputManager routes it through Button::Back regardless of hardware
// mapping) and is checked first in every loop() iteration, unconditionally,
// so it cancels out of any phase -- including the chooser, where it discards
// the whole selection exactly like picking Cancel. Confirm has no reliable
// physical or GPIO path on this board, so handleHomeGesture() is overridden
// to repurpose the capacitive Home-key tap as Confirm for the two anchor
// picks instead of letting ActivityManager treat it as "go home" -- see
// BoardConfig.h:1392-1398 and MappedInputManager::wasHomeGesture's doc
// comment. While the chooser is up, Home is still consumed (never falls
// through to "go home") but otherwise a no-op: unlike a single word, the
// three chooser rows are unambiguous single-tap targets, so there is no
// cursor-plus-confirm dance to repurpose Home for there, and OptionPopup
// itself already fully drives via touch.
class PassageSelectActivity final : public Activity {
 public:
  explicit PassageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                                 int marginLeft, int marginTop, int columnRight, HighlightDoc& highlightDoc,
                                 std::string bookPath, uint16_t spineIndex, bool saveDisabled)
      : Activity("PassageSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        columnRight(columnRight),
        highlightDoc(highlightDoc),
        bookPath(std::move(bookPath)),
        spineIndex(spineIndex),
        saveDisabled(saveDisabled) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
  // Redraws the reader's page underneath the selection, so it follows the
  // reading surface's night-mode polarity like DictionaryWordSelectActivity.
  bool appliesNightMode() const override { return true; }

 private:
  // One selectable token's screen box plus the visible-codepoint offset it
  // carries, per Constraint "Widths come from stored positions, not the
  // renderer": x/width come from wordXpos, never getTextAdvanceX.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    uint32_t offset;
  };

  enum class Phase : uint8_t { PickingStart, PickingEnd, ChoosingAction };

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void commitAt(int index);
  void showActionChooser(int endIndex);
  void startTagFlow(int endIndex);
  void finalizeSelection(int endIndex, std::vector<uint16_t> tagIndices = {});
  void drawSelectionOutline();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  const int columnRight;
  HighlightDoc& highlightDoc;
  const std::string bookPath;
  const uint16_t spineIndex;
  const bool saveDisabled;

  int fontId = 0;
  int lineHeight = 0;
  int ascender = 0;
  int16_t gapTolerance = 0;

  std::vector<WordBox> words;
  uint16_t rowCount = 0;
  // Rects for highlights already saved on this page, drawn inverted (like the
  // reader) so they read as visibly different from the outlined
  // selection-in-progress -- otherwise the user can't tell what is already
  // saved from what they are about to save.
  std::vector<HighlightRect> committedRects;

  Phase phase = Phase::PickingStart;
  int cursor = 0;
  int anchorIndex = -1;
  // The just-committed second anchor, held only across the ChoosingAction
  // phase so the OptionPopup's callback (and the TagPickerActivity result
  // handler it may lead to) can reach finalizeSelection with it.
  int pendingEndIndex = -1;
  OptionPopup actionChooser;

  // Differential repaint of the selection outline only: the pixels behind its
  // bounding box (border included), so a cursor move restores them and
  // redraws just the new outline instead of a full two-pass page render.
  // Sized from this class's own worst case -- widthBytes x lineHeight x
  // MAX_SELECTED_SNAPSHOT_LINES -- never DictionaryWordSelectActivity's
  // SNAPSHOT_CAPACITY, which is sized for one word. Allocated with
  // makeUniqueNoThrow: on a non-PSRAM board a large allocation can genuinely
  // fail, and when it does, `snapshot` stays null, `snapshotValid` stays
  // false, and every render() call falls back to the full two-pass repaint.
  static constexpr int MAX_SELECTED_SNAPSHOT_LINES = 8;
  size_t snapshotCapacity = 0;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  bool snapshotValid = false;
};
