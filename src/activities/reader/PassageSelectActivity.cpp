#include "PassageSelectActivity.h"

#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <climits>

#include "../../util/HighlightFile.h"
#include "CrossPointSettings.h"
#include "HighlightOverlay.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "TagPickerActivity.h"
#include "components/UITheme.h"

void PassageSelectActivity::onEnter() {
  Activity::onEnter();

  // The file may still hold the user's data (HighlightFile::LoadResult::Failed),
  // so a resident doc built from scratch this session must never be saved over
  // it. Bail before spending any effort on word extraction.
  if (saveDisabled) {
    ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_LOAD_FAILED));
    finish();
    return;
  }

  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  ascender = renderer.getFontAscenderSize(fontId);
  gapTolerance = static_cast<int16_t>(lineHeight / 4);

  // Worst case for the differential outline snapshot: a selection line can
  // span the full screen width, one line-height tall per selected line, up to
  // MAX_SELECTED_SNAPSHOT_LINES lines. This is this class's own worst case --
  // DictionaryWordSelectActivity::SNAPSHOT_CAPACITY is sized for one word and
  // raising it there would do nothing here, since this class isn't derived
  // from it. +8px covers screenRectToAlignedMemRect's byte alignment, which
  // can round the left edge down and the right edge up by up to 7px each.
  const int widthBytes = (renderer.getScreenWidth() + 8 + 7) / 8;
  snapshotCapacity = static_cast<size_t>(widthBytes) * static_cast<size_t>(lineHeight) *
                     static_cast<size_t>(MAX_SELECTED_SNAPSHOT_LINES);
  // No null check: on a non-PSRAM board (~50KB free heap) this allocation can
  // genuinely fail. A null snapshot just disables the differential fast path
  // -- drawSelectionOutline()'s readFramebufferRegion call is skipped and
  // snapshotValid stays false, so render() takes the full two-pass repaint
  // path on every call instead of the single-region diff. Selection still
  // works; it is just slower to redraw.
  snapshot = makeUniqueNoThrow<uint8_t[]>(snapshotCapacity);

  extractWords();

  // Highlights already saved on this page, so the outlined selection-in-progress
  // reads as visibly different from what's already committed.
  std::vector<VisibleRange> existingRanges;
  for (const auto* entry : highlightDoc.findBySpine(spineIndex)) {
    existingRanges.push_back(entry->range);
  }
  committedRects = HighlightOverlay::buildRects(*page, existingRanges, marginLeft, marginTop, columnRight, lineHeight,
                                                ascender, gapTolerance);

  if (!words.empty()) {
    const int initial = closestInRow(static_cast<uint16_t>(rowCount / 2), renderer.getScreenWidth() / 2);
    if (initial >= 0) cursor = initial;
  }
  requestUpdate();
}

void PassageSelectActivity::extractWords() {
  words.clear();
  words.reserve(96);
  rowCount = 0;

  // Mirrors HighlightOverlay::buildRects: every token is a selectable anchor
  // point (not just DictionaryWordSelectActivity's isSelectableToken subset),
  // and every position comes from the block's own stored layout, never
  // getTextAdvanceX -- see Constraints in the highlights UI plan.
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    const uint16_t wordCount = block->wordCount();
    if (wordCount == 0) continue;

    const int rubyShift = block->getRubyShift(ascender);
    const int16_t y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
    const int16_t lineRight = static_cast<int16_t>(columnRight - block->getBlockStyle().rightInset());

    for (uint16_t i = 0; i < wordCount; i++) {
      const int16_t x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      const int16_t w = (i + 1 < wordCount)
                            ? static_cast<int16_t>(block->wordXpos(i + 1) - block->wordXpos(i))
                            : static_cast<int16_t>(std::max<int>(1, lineRight - x));
      WordBox box;
      box.x = x;
      box.y = y;
      box.width = w;
      box.row = rowCount;
      box.offset = block->wordVisibleOffset(i);
      words.push_back(box);
    }
    rowCount++;
  }
}

int PassageSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the outline box (+2) plus finger error
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + lineHeight + SLOP) {
      return i;
    }
  }
  return -1;
}

int PassageSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void PassageSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[cursor];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != cursor) {
    cursor = best;
    requestUpdate();
  }
}

void PassageSelectActivity::commitAt(const int index) {
  if (phase == Phase::PickingStart) {
    anchorIndex = index;
    cursor = index;
    phase = Phase::PickingEnd;
    requestUpdate();
    return;
  }
  if (phase == Phase::PickingEnd) {
    showActionChooser(index);
  }
  // ChoosingAction: word taps/gestures don't reach here -- actionChooser
  // (active by then) absorbs all input first, in loop().
}

void PassageSelectActivity::showActionChooser(const int endIndex) {
  pendingEndIndex = endIndex;
  phase = Phase::ChoosingAction;

  // The tap or gesture that commits this second anchor is the SAME event
  // that opens the chooser, so -- unlike every other OptionPopup use in this
  // codebase, which always opens from a row/cursor position already drawn on
  // a prior frame -- there is no earlier frame that shows the corrected
  // two-anchor outline yet. Force one synchronous render of it now, with
  // actionChooser still inactive, so its "draw over the current screen, no
  // clear" contract (see OptionPopup's class comment) has the right pixels
  // underneath once it shows.
  requestUpdateAndWait();

  const char* options[] = {tr(STR_HIGHLIGHT), tr(STR_TAG), tr(STR_CANCEL)};
  actionChooser.show(tr(STR_HIGHLIGHT_PASSAGE), options, 3, 0, [this](const int choice) {
    switch (choice) {
      case 0:  // Highlight: save immediately, no tags.
        finalizeSelection(pendingEndIndex);
        break;
      case 1:  // Tag: pick tags first, then save with whatever comes back.
        startTagFlow(pendingEndIndex);
        break;
      default:  // Cancel: discard the selection, save nothing.
        finish();
        break;
    }
  });
  requestUpdate();
}

void PassageSelectActivity::startTagFlow(const int endIndex) {
  startActivityForResult(std::make_unique<TagPickerActivity>(renderer, mappedInput, highlightDoc),
                         [this, endIndex](const ActivityResult& result) {
                           // Cancelling the picker discards only the TAG
                           // selection, not the highlight itself -- the
                           // two-anchor passage was already committed before
                           // this sub-step opened, and TagPickerActivity's
                           // own contract (see its class comment) is that the
                           // caller decides what "no tags chosen" means. Here
                           // that means the same untagged save Highlight
                           // would have produced, not discarding the work the
                           // user already did picking two anchors.
                           std::vector<uint16_t> tagIndices;
                           if (!result.isCancelled) {
                             tagIndices = std::get<TagSelectionResult>(result.data).tagIndices;
                           }
                           finalizeSelection(endIndex, std::move(tagIndices));
                         });
}

void PassageSelectActivity::finalizeSelection(const int endIndex, std::vector<uint16_t> tagIndices) {
  const int lo = std::min(anchorIndex, endIndex);
  const int hi = std::max(anchorIndex, endIndex);

  // The two anchors are word-select indices in this page's reading-order
  // array, not offset order: TextBlock stores words in visual order, which
  // runs backwards on an RTL line, so the min/max offset in [lo, hi] must be
  // found by scanning rather than assumed from the endpoints.
  uint32_t minOffset = words[lo].offset;
  uint32_t maxOffset = words[lo].offset;
  for (int i = lo; i <= hi; i++) {
    minOffset = std::min(minOffset, words[i].offset);
    maxOffset = std::max(maxOffset, words[i].offset);
  }

  HighlightEntry entry;
  entry.spineIndex = spineIndex;
  // end = last word's offset + 1: contains() tests a word's start offset,
  // offsets are strictly increasing across tokens, and no path emits two
  // words at the same offset (verified; see the plan).
  entry.range = VisibleRange{minOffset, maxOffset + 1};
  // Truncated to MAX_TAGS_PER_HIGHLIGHT by addHighlight if ever oversized, but
  // TagPickerActivity already blocks picking a 9th tag, so this is a no-op in
  // practice, not a second, divergent cap.
  entry.tagIndices = std::move(tagIndices);

  if (!highlightDoc.addHighlight(std::move(entry))) {
    // HighlightDoc::MAX_HIGHLIGHTS reached -- nothing was appended.
    ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_TOO_LARGE));
    finish();
    return;
  }

  // addHighlight appends, so this is exactly the entry just added.
  const size_t addedIndex = highlightDoc.highlights().size() - 1;
  switch (HighlightFile::save(bookPath, highlightDoc)) {
    case HighlightFile::SaveResult::Ok:
      break;
    case HighlightFile::SaveResult::TooLarge:
      // Never leave the resident doc holding an entry that isn't actually on
      // disk -- it would render as a phantom highlight and get retried (and
      // fail again) on every future save this session.
      highlightDoc.removeHighlight(addedIndex);
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_TOO_LARGE));
      break;
    case HighlightFile::SaveResult::WriteFailed:
      highlightDoc.removeHighlight(addedIndex);
      ReaderUtils::showMessage(renderer, tr(STR_HIGHLIGHTS_SAVE_FAILED));
      break;
  }
  finish();
}

void PassageSelectActivity::loop() {
  // Checked first and unconditionally, in every phase: the touchscreen's
  // left-edge swipe routes through Button::Back regardless of this board
  // having no physical Back pin (MappedInputManager::wasBackGesture), so
  // cancelling out of either anchor never depends on a button that doesn't
  // exist on the X4 Pro.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // ChoosingAction: actionChooser (touch-driven) owns all input until it
  // fires or is dismissed; nothing below applies to word selection anymore.
  if (actionChooser.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (phase == Phase::ChoosingAction && !actionChooser.isActive()) {
    // Popup dismissed without a selection (tap released outside the dialog):
    // handleInput's dismiss path never invokes the choice callback, so
    // nothing above resets phase. Without this, every later tap/Confirm/Home
    // hits commitAt's ChoosingAction no-op forever, and the popup's pixels
    // are never repainted over since render() takes the differential
    // fast path. Back the flow out to PickingEnd instead of finishing --
    // mirrors HighlightsActivity::handleCustomInput's identical case.
    phase = Phase::PickingEnd;
    pendingEndIndex = -1;
    snapshotValid = false;
    requestUpdate();
    return;
  }

  if (words.empty()) return;

  // Touch: a touch-down moves the cursor to the touched word (differential
  // repaint), a tap on a word commits it as the current anchor in one go.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != cursor) {
      cursor = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      cursor = hit;
      commitAt(hit);
    }
    return;
  }

  // Boards with a real Confirm mapping (or the power-click Confirm path) get
  // it too; on the X4 Pro this rarely fires and handleHomeGesture() below is
  // the one that actually reaches users.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    commitAt(cursor);
    return;
  }

  const bool hasNextWord = cursor + 1 < static_cast<int>(words.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) && cursor > 0) {
    cursor--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) && hasNextWord) {
    cursor++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

bool PassageSelectActivity::handleHomeGesture() {
  // The X4 Pro has no physical Confirm; a capacitive Home-key tap is the
  // plan's designated Confirm gesture for this activity (Task 4 Step 2).
  // ActivityManager treats an unconsumed Home gesture as "go home", which
  // would abandon the selection with no warning -- always consume it here so
  // that never happens, committing the current cursor when there's something
  // to commit. During ChoosingAction, commitAt() is a no-op (see its own
  // comment): actionChooser's three rows are unambiguous single-tap targets,
  // so Home has nothing useful left to commit there and is simply absorbed.
  if (!words.empty()) commitAt(cursor);
  return true;
}

void PassageSelectActivity::drawSelectionOutline() {
  const int lo = (phase == Phase::PickingStart) ? cursor : std::min(anchorIndex, cursor);
  const int hi = (phase == Phase::PickingStart) ? cursor : std::max(anchorIndex, cursor);

  uint32_t minOffset = words[lo].offset;
  uint32_t maxOffset = words[lo].offset;
  for (int i = lo; i <= hi; i++) {
    minOffset = std::min(minOffset, words[i].offset);
    maxOffset = std::max(maxOffset, words[i].offset);
  }
  const VisibleRange pending{minOffset, maxOffset + 1};

  std::vector<HighlightWord> geomWords;
  geomWords.reserve(words.size());
  for (const auto& w : words) {
    geomWords.push_back(HighlightWord{w.offset, w.x, w.y, w.width, static_cast<int16_t>(lineHeight)});
  }
  const std::vector<HighlightRect> rects = highlightRects(geomWords, {pending}, gapTolerance);
  if (rects.empty()) return;

  int16_t minX = rects[0].x;
  int16_t minY = rects[0].y;
  int16_t maxX = static_cast<int16_t>(rects[0].x + rects[0].w);
  int16_t maxY = static_cast<int16_t>(rects[0].y + rects[0].h);
  for (const auto& r : rects) {
    minX = std::min(minX, r.x);
    minY = std::min(minY, r.y);
    maxX = std::max(maxX, static_cast<int16_t>(r.x + r.w));
    maxY = std::max(maxY, static_cast<int16_t>(r.y + r.h));
  }

  // Padded by the outline's own stroke width so save/restore covers the
  // border pixels themselves, not just the rect interior.
  constexpr int OUTLINE_STROKE = 2;
  int bx = minX - OUTLINE_STROKE;
  int by = minY - OUTLINE_STROKE;
  int bw = (maxX - minX) + OUTLINE_STROKE * 2;
  int bh = (maxY - minY) + OUTLINE_STROKE * 2;
  if (bx < 0) {
    bw += bx;
    bx = 0;
  }
  if (by < 0) {
    bh += by;
    by = 0;
  }

  bool saved = false;
  if (snapshot && bw > 0 && bh > 0) {
    saved = renderer.readFramebufferRegion(bx, by, bw, bh, snapshot.get(), snapshotCapacity) > 0;
  }
  snapshotX = static_cast<int16_t>(bx);
  snapshotY = static_cast<int16_t>(by);
  snapshotW = static_cast<int16_t>(bw);
  snapshotH = static_cast<int16_t>(bh);
  snapshotValid = saved;

  // Outlined, never filled: a committed highlight (drawn inverted, above) and
  // a selection-in-progress must never look the same.
  for (const auto& r : rects) {
    renderer.drawRect(r.x, r.y, r.w, r.h, OUTLINE_STROKE, true);
  }
}

void PassageSelectActivity::drawHints() const {
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT),
                                                        tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PassageSelectActivity::render(RenderLock&&) {
  // actionChooser draws over the current screen without clearing it (see
  // OptionPopup's class comment), so it must win before anything below
  // touches the framebuffer. By the time it is active, showActionChooser()
  // has already forced one synchronous render of the final two-anchor
  // outline (via requestUpdateAndWait()), so the pixels underneath are
  // always the correct, final selection, never mid-cursor-move.
  if (actionChooser.processRender(renderer, mappedInput)) return;

  // Differential fast path: only the outline moved and the framebuffer still
  // holds a clean baseline (committed highlights + page text). Restore the
  // pixels behind the old outline, draw the new one, and push -- skipping the
  // two-pass page render entirely.
  if (snapshotValid && !words.empty()) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    drawSelectionOutline();
    drawHints();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  for (const auto& rect : committedRects) {
    renderer.invertRect(rect.x, rect.y, rect.w, rect.h);
  }

  if (!words.empty()) {
    drawSelectionOutline();
  }

  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
