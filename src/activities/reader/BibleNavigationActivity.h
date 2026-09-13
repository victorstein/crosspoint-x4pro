#pragma once
#include <Epub.h>
#include <Epub/VerseAnchors.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "NumberGridLayout.h"
#include "activities/UiListActivity.h"

// Book -> chapter -> verse drill-down for NWT-shaped Bible publications, whose
// chapters run past 50 verses while a page holds four to six: reaching a known
// reference through the flat TOC costs a dozen page turns.
//
// One activity walks all three levels rather than three nested ones. Nesting
// would keep three activities and three row-buffer sets resident and would
// hand-propagate the verse result up two intermediate handlers.
//
// The book level is a vertical list (book names are long and variable width);
// the chapter and verse levels are paged number grids, so a high reference costs
// pages instead of screens. Tap or Confirm a chapter to list its verses.
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
  static constexpr int MAX_GRID_CELLS = NumberGrid::MAX_CELLS;
  // "176" plus its NUL: no chapter or verse number reaches four digits.
  static constexpr int CELL_LABEL_BYTES = 4;

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

  // Book level only: the row window the vertical list draws from.
  std::string windowLabels[ROW_WINDOW];
  freeink::ui::ListItem windowItems[ROW_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshRowWindow(int start);

  // Grid levels only. `grid` carries the last grid build's geometry, which the
  // loop task reads to page and to step the selection by a row.
  freeink::ui::KeyGridKey cells[MAX_GRID_CELLS] = {};
  char cellLabels[MAX_GRID_CELLS][CELL_LABEL_BYTES] = {};
  NumberGrid::Geometry grid{};
  bool isGridLevel() const { return level != Level::Book; }
  void buildNumberGrid(UiScreen& screen);
  // Move the selection and bring its page with it. The base moveSelectionTo
  // pulls a sliding row window instead, which would leave nav.top off a page
  // boundary.
  void moveGridSelection(int index);

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
  // Swipes page the grid by a whole page; the base scrolls by rows.
  bool handleCustomInput() override;
  // Grid levels step the selection by a row (a column count) and page on a
  // held button; the base steps by one row either way.
  void navigateButtons() override;
  void onBackButton() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;
};
