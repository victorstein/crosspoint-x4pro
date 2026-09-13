#include "BibleNavigationActivity.h"

#include <Epub/BibleNavScanner.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "SpineHtmlStream.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

void copyTruncated(char* dest, const size_t destBytes, const std::string& source) {
  const size_t fit = source.size() < destBytes - 1 ? source.size() : destBytes - 1;
  // A byte-cut would feed drawText an incomplete UTF-8 sequence, which renders
  // as a replacement character.
  const int safe = utf8SafeTruncateBuffer(source.data(), static_cast<int>(fit));
  memcpy(dest, source.data(), static_cast<size_t>(safe));
  dest[safe] = '\0';
}

bool feedNavScanner(void* ctx, const char* chunk, const size_t length, const bool isFinal) {
  return static_cast<BibleNav::Scanner*>(ctx)->feed(chunk, length, isFinal);
}

bool feedVerseScanner(void* ctx, const char* chunk, const size_t length, const bool isFinal) {
  return static_cast<VerseAnchors::Scanner*>(ctx)->feed(chunk, length, isFinal);
}

}  // namespace

BibleNavigationActivity::BibleNavigationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const std::shared_ptr<Epub>& epub)
    : UiListActivity("BibleNavigation", renderer, mappedInput, /*wantsTouchLongPress=*/false), epub(epub) {}

void BibleNavigationActivity::onEnter() {
  UiListActivity::onEnter();

  // The reader underneath pins its page-render glyph arenas while this overlay
  // is up; freeing them gives the row window room to keep its own fallback
  // glyphs resident. Mirrors EpubReaderChapterSelectionActivity.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }

  if (!loadBooks()) {
    LOG_ERR("BNV", "Failed to read the book list");
  }
}

bool BibleNavigationActivity::loadBooks() {
  bookCount = 0;
  if (!epub) return false;

  BibleNav::Scanner scanner;
  if (!scanner.valid()) {
    LOG_ERR("BNV", "OOM: nav scanner");
    return false;
  }
  if (!SpineHtmlStream::stream(epub, epub->getBibleBookNavSpineIndex(), renderer, feedNavScanner, &scanner))
    return false;

  std::vector<std::string> targets = scanner.take();
  if (targets.empty()) return false;
  if (targets.size() > MAX_BOOKS) targets.resize(MAX_BOOKS);
  bookCount = static_cast<int>(targets.size());

  auto spineIndices = makeUniqueNoThrow<int[]>(static_cast<size_t>(bookCount));
  if (!spineIndices) {
    LOG_ERR("BNV", "OOM: %d spine indices", bookCount);
    bookCount = 0;
    return false;
  }
  epub->resolveFilenamesToSpineIndices(targets.data(), spineIndices.get(), bookCount);

  std::vector<std::string> names(bookCount);
  const int tocCount = epub->getTocItemsCount();
  for (int i = 0; i < tocCount; i++) {
    const auto tocItem = epub->getTocItem(i);
    const int match = BibleNav::findTargetByHref(targets.data(), bookCount, tocItem.href);
    if (match >= 0 && names[match].empty()) names[match] = tocItem.title;
  }

  for (int i = 0; i < bookCount; i++) {
    bookTargetSpine[i] = static_cast<int16_t>(spineIndices[i]);
    bookIsDirect[i] = !BibleNav::isChapterNav(targets[i]);
    copyTruncated(bookName[i], BOOK_NAME_BYTES, names[i]);
  }
  return true;
}

bool BibleNavigationActivity::loadChapters(const int bookIndex) {
  chapterCount = 0;
  if (!epub || bookIndex < 0 || bookIndex >= bookCount) return false;

  BibleNav::Scanner scanner;
  if (!scanner.valid()) {
    LOG_ERR("BNV", "OOM: nav scanner");
    return false;
  }
  if (!SpineHtmlStream::stream(epub, bookTargetSpine[bookIndex], renderer, feedNavScanner, &scanner)) return false;

  std::vector<std::string> targets = scanner.take();
  BibleNav::dropBookNavLinks(targets);
  if (targets.empty()) return false;
  if (targets.size() > MAX_CHAPTERS) targets.resize(MAX_CHAPTERS);
  chapterCount = static_cast<int>(targets.size());

  auto spineIndices = makeUniqueNoThrow<int[]>(static_cast<size_t>(chapterCount));
  if (!spineIndices) {
    LOG_ERR("BNV", "OOM: %d spine indices", chapterCount);
    chapterCount = 0;
    return false;
  }
  epub->resolveFilenamesToSpineIndices(targets.data(), spineIndices.get(), chapterCount);
  for (int i = 0; i < chapterCount; i++) {
    chapterSpine[i] = static_cast<int16_t>(spineIndices[i]);
  }
  return true;
}

bool BibleNavigationActivity::loadVerses(const int spineIndex) {
  verseAnchors.clear();
  verseSpine = spineIndex;

  VerseAnchors::Scanner scanner;
  if (!scanner.valid()) {
    LOG_ERR("BNV", "OOM: verse scanner");
    return false;
  }
  if (!SpineHtmlStream::stream(epub, spineIndex, renderer, feedVerseScanner, &scanner)) return false;

  verseAnchors = scanner.take();
  return !verseAnchors.empty();
}

int BibleNavigationActivity::listCount() const {
  switch (level) {
    case Level::Book:
      return bookCount;
    case Level::Chapter:
      return chapterCount;
    case Level::Verse:
      return static_cast<int>(verseAnchors.size());
  }
  return 0;
}

void BibleNavigationActivity::enterLevel(const Level next, const int selected) {
  {
    // The render task reads level/window/nav mid-build, so the whole switch
    // has to land before it can see any part of it.
    RenderLock lock;
    level = next;
    windowStart = -1;
    windowCount = 0;
    nav.reset();
    nav.selected = selected < 0 || selected >= listCount() ? 0 : selected;
    if (isGridLevel()) {
      // reset() leaves visibleRows at 1 and only syncToProps -- the list path,
      // which no grid level takes -- ever writes it, so follow(), scrollBy()
      // and pageRows() would all treat a single cell as a whole viewport. One
      // grid "row" is one page. The geometry is the last grid build's; the
      // first build fixes it up (buildNumberGrid).
      nav.visibleRows = grid.cellsPerPage() > 0 ? grid.cellsPerPage() : 1;
      nav.top = NumberGrid::pageStartFor(nav.selected, listCount(), nav.visibleRows);
    } else {
      nav.follow(listCount());
    }
  }
  requestUpdate();
}

void BibleNavigationActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = start;
  if (clamped > total - ROW_WINDOW) clamped = total - ROW_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == windowStart) return;

  windowCount = total - clamped < ROW_WINDOW ? total - clamped : ROW_WINDOW;
  for (int i = 0; i < windowCount; i++) {
    const int row = clamped + i;
    windowLabels[i] = bookName[row];
    fui::ListItem item;
    item.label = windowLabels[i].c_str();
    item.actionValue = static_cast<int16_t>(row);
    windowItems[i] = item;
  }
  windowStart = clamped;

  struct PrewarmCtx {
    const std::string* labels;
    int count;
  } prewarmCtx{windowLabels, windowCount};
  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < static_cast<uint32_t>(c->count) ? c->labels[i].c_str() : nullptr;
      },
      &prewarmCtx, static_cast<uint32_t>(windowCount));
}

void BibleNavigationActivity::finishWith(const int spineIndex, const std::optional<uint32_t> offsetJump) {
  if (spineIndex < 0) {
    LOG_ERR("BNV", "Row resolved to no spine item");
    return;
  }
  app.clearTapFlash();
  setResult(ChapterResult{spineIndex, "", offsetJump});
  finish();
}

void BibleNavigationActivity::openVerseList(const int spineIndex, const int chapterRow) {
  if (!loadVerses(spineIndex)) {
    // Every chapter in these publications carries verse markers, so this is
    // defence rather than a known path: fall back to the top of the chapter.
    LOG_DBG("BNV", "No verse markers in spine %d", spineIndex);
    finishWith(spineIndex, std::nullopt);
    return;
  }
  selectedChapterRow = chapterRow;
  enterLevel(Level::Verse, 0);
}

void BibleNavigationActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;

  switch (level) {
    case Level::Book:
      // The five single-chapter books have no chapter level, so their row is
      // the only route to their verses.
      if (bookIsDirect[index]) {
        openVerseList(bookTargetSpine[index], -1);
        return;
      }
      selectedBook = index;
      if (!loadChapters(index)) {
        LOG_ERR("BNV", "Failed to read the chapter list for book %d", index);
        requestUpdate();
        return;
      }
      enterLevel(Level::Chapter, 0);
      return;
    case Level::Chapter:
      openVerseList(chapterSpine[index], index);
      return;
    case Level::Verse:
      finishWith(verseSpine, verseAnchors[index].offset);
      return;
  }
}

void BibleNavigationActivity::moveGridSelection(const int index) {
  const int count = listCount();
  if (count <= 0) return;
  const int clamped = std::clamp(index, 0, count - 1);
  {
    // Same nav-vs-render race moveSelectionTo guards: the render task reads the
    // selection and its page together mid-build.
    RenderLock lock;
    nav.selected = clamped;
    nav.top = NumberGrid::pageStartFor(clamped, count, grid.cellsPerPage());
  }
  requestUpdate();
}

bool BibleNavigationActivity::handleCustomInput() {
  // Reading the gesture at the book level would take it away from the base
  // loop's row scrolling.
  if (!isGridLevel()) return false;
  const int cellsPerPage = grid.cellsPerPage();
  if (cellsPerPage <= 0) return false;

  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::Up && swipe != MappedInputManager::SwipeDir::Down) return false;

  const int count = listCount();
  const int page = NumberGrid::pageOfIndex(nav.top, cellsPerPage);
  const int next = swipe == MappedInputManager::SwipeDir::Up ? page + 1 : page - 1;
  // Consumed either way: the base loop would otherwise scroll the viewport a
  // single cell off its page boundary.
  if (next >= 0 && next < NumberGrid::pageCount(count, cellsPerPage)) {
    moveGridSelection(NumberGrid::pageFirstCell(next, cellsPerPage));
  }
  return true;
}

void BibleNavigationActivity::navigateButtons() {
  if (!isGridLevel() || !grid.valid()) {
    UiListActivity::navigateButtons();
    return;
  }

  const int count = listCount();
  const int cols = grid.cols;
  const int cellsPerPage = grid.cellsPerPage();
  buttonNavigator.onNextRelease([this, cols] { moveGridSelection(nav.selected + cols); });
  buttonNavigator.onPreviousRelease([this, cols] { moveGridSelection(nav.selected - cols); });
  buttonNavigator.onNextContinuous([this, count, cellsPerPage] {
    moveGridSelection(ButtonNavigator::nextPageIndex(nav.selected, count, cellsPerPage));
  });
  buttonNavigator.onPreviousContinuous([this, count, cellsPerPage] {
    moveGridSelection(ButtonNavigator::previousPageIndex(nav.selected, count, cellsPerPage));
  });
}

void BibleNavigationActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BibleNavigationActivity::onBackButton() {
  switch (level) {
    case Level::Book:
      cancel();
      return;
    case Level::Chapter:
      enterLevel(Level::Book, selectedBook);
      return;
    case Level::Verse:
      // A verse list reached from a single-chapter book has no chapter level
      // underneath it.
      if (selectedChapterRow < 0) {
        enterLevel(Level::Book, selectedBook);
      } else {
        enterLevel(Level::Chapter, selectedChapterRow);
      }
      return;
  }
}

void BibleNavigationActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  if (isGridLevel()) {
    buildNumberGrid(screen);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  // Tap descends a level; physical buttons stay in loop().
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  // Materialize the row window for the final viewport (syncListViewport just
  // applied follow/clamping to nav.top) and hand list() the window with its
  // absolute base index.
  refreshRowWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

void BibleNavigationActivity::buildNumberGrid(UiScreen& screen) {
  const fui::Rect body = screen.body();
  grid = NumberGrid::geometryFor(body.width, body.height);
  const int cellsPerPage = grid.cellsPerPage();
  const int count = listCount();

  // An orientation change re-pages around the selection rather than leaving
  // nav.top on a page the new geometry no longer has.
  if (nav.visibleRows != cellsPerPage) {
    nav.visibleRows = cellsPerPage;
    nav.top = NumberGrid::pageStartFor(nav.selected, count, cellsPerPage);
  }
  const int pageFirst = NumberGrid::pageStartFor(nav.top, count, cellsPerPage);
  nav.top = pageFirst;

  for (int i = 0; i < cellsPerPage; i++) {
    const int row = pageFirst + i;
    fui::KeyGridKey cell;
    if (row < count) {
      const unsigned number =
          level == Level::Verse ? static_cast<unsigned>(verseAnchors[row].verse) : static_cast<unsigned>(row + 1);
      snprintf(cellLabels[i], CELL_LABEL_BYTES, "%u", number);
      cell.label = cellLabels[i];
      // ACTION_ROW dispatch (onRowAction) indexes the level by this value, so
      // it is the absolute row, not the cell's place on the page.
      cell.value = static_cast<int16_t>(row);
    } else {
      // The page stays rectangular; a disabled cell registers no interaction.
      cell.kind = fui::KeyKind::Disabled;
      cell.enabled = false;
    }
    cells[i] = cell;
  }

  fui::KeyGridProps props;
  props.keys = cells;
  props.rows = static_cast<uint8_t>(grid.rows);
  props.cols = static_cast<uint8_t>(grid.cols);
  // keyGrid compares this against a page-relative cell index, unlike the
  // absolute value each cell carries.
  props.selectedIndex = static_cast<int16_t>(NumberGrid::pageRelativeIndex(nav.selected, pageFirst, cellsPerPage));
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.gap = NumberGrid::GAP;
  // Above the cell size, ensureMinTouchRect would grow each hit rect past its
  // own cell and neighbouring numbers would swallow each other's taps.
  props.minTouchSize = static_cast<int16_t>(NumberGrid::cellSizeFor(body.width, body.height, grid));
  props.labelText = screen.theme().bodyText;
  props.labelText.align = fui::TextAlign::Center;
  props.keyStyles = screen.theme().key;
  fui::keyGrid(screen.frame(), body, props);
}

void BibleNavigationActivity::drawChrome() {
  const char* title = tr(STR_SELECT_BOOK);
  if (level == Level::Chapter) title = tr(STR_SELECT_CHAPTER);
  if (level == Level::Verse) title = tr(STR_SELECT_VERSE);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, title);
}
