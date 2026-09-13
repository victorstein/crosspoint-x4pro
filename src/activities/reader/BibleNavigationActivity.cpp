#include "BibleNavigationActivity.h"

#include <Epub/BibleNavScanner.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

constexpr size_t HTML_CHUNK_BYTES = 2048;

// Longest row label at the chapter and verse levels is "176".
constexpr int NUMBER_LABEL_BYTES = 8;

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
    : UiListActivity("BibleNavigation", renderer, mappedInput, /*wantsTouchLongPress=*/true), epub(epub) {}

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

// One forward pass over the inflated XHTML of `spineIndex`, feeding it to
// `sink` in bounded chunks. Going through Section rather than re-inflating from
// the zip means the chapter the user is about to open keeps the HTML cache this
// produced, so opening it does not pay the inflate twice.
bool BibleNavigationActivity::streamSpineHtml(const int spineIndex, const ChunkSink sink, void* ctx) {
  if (!epub || spineIndex < 0) return false;

  Section section(epub, spineIndex, renderer);
  std::string parsePath;
  bool promoted = true;
  std::string tmpHtmlPath;

  if (section.hasHtmlCache()) {
    parsePath = section.htmlCachePath();
  } else {
    const size_t spineBytes = epub->getCumulativeSpineItemSize(spineIndex) -
                              (spineIndex > 0 ? epub->getCumulativeSpineItemSize(spineIndex - 1) : 0);
    bool inflated;
    {
      RenderLock lock;
      // drawPopup refreshes the display itself, so it has to land before the
      // loan hands the framebuffer to the inflate's window.
      if (spineBytes > INFLATE_POPUP_BYTE_THRESHOLD) GUI.drawPopup(renderer, tr(STR_INDEXING));
      GfxRenderer::FrameBufferLoan loan(renderer);
      inflated = section.ensureHtmlCache(parsePath, promoted, tmpHtmlPath);
      loan.end();
    }
    if (!inflated) {
      LOG_ERR("BNV", "Failed to inflate spine %d", spineIndex);
      return false;
    }
  }

  auto buffer = makeUniqueNoThrow<char[]>(HTML_CHUNK_BYTES);
  if (!buffer) {
    LOG_ERR("BNV", "OOM: %d bytes", static_cast<int>(HTML_CHUNK_BYTES));
    if (!promoted) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  bool ok = false;
  {
    HalFile file;
    if (Storage.openFileForRead("BNV", parsePath, file)) {
      ok = true;
      size_t remaining = file.size();
      // An empty file still has to close the parse, or the scanner reports no
      // failure and hands back a silently empty list.
      if (remaining == 0) ok = sink(ctx, "", 0, true);
      while (ok && remaining > 0) {
        const size_t want = remaining < HTML_CHUNK_BYTES ? remaining : HTML_CHUNK_BYTES;
        const int got = file.read(buffer.get(), want);
        if (got <= 0) {
          ok = false;
          break;
        }
        remaining -= static_cast<size_t>(got);
        ok = sink(ctx, buffer.get(), static_cast<size_t>(got), remaining == 0);
      }
    }
  }
  // Only an un-promoted temp is ours; the promoted cache belongs to Section.
  if (!promoted) Storage.remove(tmpHtmlPath.c_str());
  return ok;
}

bool BibleNavigationActivity::loadBooks() {
  bookCount = 0;
  if (!epub) return false;

  BibleNav::Scanner scanner;
  if (!scanner.valid()) {
    LOG_ERR("BNV", "OOM: nav scanner");
    return false;
  }
  if (!streamSpineHtml(epub->getBibleBookNavSpineIndex(), feedNavScanner, &scanner)) return false;

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
  if (!streamSpineHtml(bookTargetSpine[bookIndex], feedNavScanner, &scanner)) return false;

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
  if (!streamSpineHtml(spineIndex, feedVerseScanner, &scanner)) return false;

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
    nav.follow(listCount());
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
    if (level == Level::Book) {
      windowLabels[i] = bookName[row];
    } else {
      char label[NUMBER_LABEL_BYTES];
      const unsigned number =
          level == Level::Verse ? static_cast<unsigned>(verseAnchors[row].verse) : static_cast<unsigned>(row + 1);
      snprintf(label, sizeof(label), "%u", number);
      windowLabels[i] = label;
    }
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
      if (bookIsDirect[index]) {
        finishWith(bookTargetSpine[index], std::nullopt);
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
      finishWith(chapterSpine[index], std::nullopt);
      return;
    case Level::Verse:
      finishWith(verseSpine, verseAnchors[index].offset);
      return;
  }
}

void BibleNavigationActivity::onRowLongPress(const int index) {
  if (index < 0 || index >= listCount()) return;

  switch (level) {
    case Level::Book:
      // Only the single-chapter books have verses to list from here; every
      // other book's long-press falls through to its chapter list.
      if (bookIsDirect[index]) {
        openVerseList(bookTargetSpine[index], -1);
        return;
      }
      activateIndex(index);
      return;
    case Level::Chapter:
      openVerseList(chapterSpine[index], index);
      return;
    case Level::Verse:
      activateIndex(index);
      return;
  }
}

bool BibleNavigationActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = nav.selected;
    if (selected < 0 || selected >= listCount()) return true;
    // Button hardware has no long-press gesture, so a held Confirm release
    // stands in for it -- the pattern HighlightsActivity uses for delete.
    if (mappedInput.getHeldTime() > OPEN_VERSE_LIST_MS) {
      onRowLongPress(selected);
    } else {
      activateIndex(selected);
    }
    return true;
  }

  return false;
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

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  // Tap descends a level; long-press on a chapter opens its verses. Physical
  // buttons stay in loop().
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  // Materialize the row window for the final viewport (syncListViewport just
  // applied follow/clamping to nav.top) and hand list() the window with its
  // absolute base index.
  refreshRowWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

void BibleNavigationActivity::drawChrome() {
  const char* title = tr(STR_SELECT_BOOK);
  if (level == Level::Chapter) title = tr(STR_SELECT_CHAPTER);
  if (level == Level::Verse) title = tr(STR_SELECT_VERSE);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, title);
}
