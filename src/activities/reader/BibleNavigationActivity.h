#pragma once
#include <Epub.h>
#include <Epub/VerseAnchors.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Book -> chapter -> verse drill-down for NWT-shaped Bible publications, whose
// chapters run past 50 verses while a page holds four to six: reaching a known
// reference through the flat TOC costs a dozen page turns.
//
// One activity walks all three levels rather than three nested ones. Nesting
// would keep three activities and three row-buffer sets resident and would
// hand-propagate the verse result up two intermediate handlers.
//
// Tap or Confirm a chapter to open it at verse 1; long-press (or hold Confirm)
// to list its verses instead.
class BibleNavigationActivity final : public UiListActivity {
 public:
  BibleNavigationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::shared_ptr<Epub>& epub);
  void onEnter() override;

 private:
  enum class Level : uint8_t { Book, Chapter, Verse };

  static constexpr int MAX_BOOKS = 66;
  static constexpr int MAX_CHAPTERS = 150;  // Psalms
  // Sized in UTF-8 BYTES, not characters. The longest joined TOC name measured
  // across the shipped publications is "El Cantar de los Cantares" at 25 B, and
  // Cyrillic/Greek renderings of the same books run to ~42 B.
  static constexpr int BOOK_NAME_BYTES = 48;
  // Matches EpubReaderChapterSelectionActivity: only the rows around the
  // viewport are materialized, and refreshing the window batch-prewarms its
  // fallback glyphs so repaints inside it stay RAM-only.
  static constexpr int ROW_WINDOW = 24;
  // A held Confirm release opens the verse list, as in HighlightsActivity.
  static constexpr int OPEN_VERSE_LIST_MS = 700;
  // Above this an inflate is slow enough to want the indexing popup. Nav pages
  // top out around 14 KB and never reach it; a long chapter (Psalm 119 at
  // ~69 KB) does.
  static constexpr size_t INFLATE_POPUP_BYTE_THRESHOLD = 32 * 1024;

  // Chunk sink for streamSpineHtml. A function pointer rather than
  // std::function: the latter heap-allocates its closure and costs KBs of
  // binary per signature.
  using ChunkSink = bool (*)(void* ctx, const char* chunk, size_t length, bool isFinal);

  std::shared_ptr<Epub> epub;
  Level level = Level::Book;

  // Only the display name and the resolved spine target are kept: chapter rows
  // are literally 1..N, so no hrefs need storing past the one sweep that
  // resolved them.
  char bookName[MAX_BOOKS][BOOK_NAME_BYTES] = {};
  int16_t bookTargetSpine[MAX_BOOKS] = {};
  // The five single-chapter books (Obadiah, Philemon, 2-3 John, Jude) have no
  // chapter-nav page; their row points straight at the chapter spine item.
  bool bookIsDirect[MAX_BOOKS] = {};
  int bookCount = 0;
  int selectedBook = -1;

  int16_t chapterSpine[MAX_CHAPTERS] = {};
  int chapterCount = 0;
  // Row the verse list was opened from, or -1 when it came straight off a
  // single-chapter book -- which is also what Back from the verse level reads
  // to know whether a chapter level sits underneath it.
  int selectedChapterRow = -1;

  std::vector<VerseAnchors::VerseAnchor> verseAnchors;
  int verseSpine = -1;

  std::string windowLabels[ROW_WINDOW];
  freeink::ui::ListItem windowItems[ROW_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshRowWindow(int start);

  bool streamSpineHtml(int spineIndex, ChunkSink sink, void* ctx);
  bool loadBooks();
  bool loadChapters(int bookIndex);
  bool loadVerses(int spineIndex);
  // Enter `next`, resetting the row window and placing the selection on
  // `selected` (clamped into that level's rows).
  void enterLevel(Level next, int selected);
  void openVerseList(int spineIndex, int chapterRow);
  void finishWith(int spineIndex, std::optional<uint32_t> offsetJump);
  void cancel();

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleButtons() override;
  void onBackButton() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;
};
