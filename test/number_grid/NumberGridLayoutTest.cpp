#include <gtest/gtest.h>

#include "activities/reader/NumberGridLayout.h"

// Reaching Psalm 119:145 has to cost pages, not screens, and every cell on
// every page has to be tappable and render its own selection. Both properties
// are arithmetic: how many cells a rect earns (over the cap, cells would be
// silently dropped from the interaction table) and how absolute indexes map to
// the page-relative index keyGrid compares against.

namespace {

// The real content rects the reader leaves for the grid on the 800x480 panel:
// touch boards get the whole safe area because UITheme zeroes buttonHintsHeight.
constexpr int PORTRAIT_W = 480;
constexpr int PORTRAIT_H = 695;
constexpr int LANDSCAPE_W = 800;
constexpr int LANDSCAPE_H = 375;

constexpr int PSALM_119_VERSES = 176;

}  // namespace

TEST(NumberGridGeometry, PortraitRectStaysWithinTheCellCap) {
  const auto geometry = NumberGrid::geometryFor(PORTRAIT_W, PORTRAIT_H);

  EXPECT_EQ(geometry.cols, 7);
  EXPECT_LE(geometry.cellsPerPage(), NumberGrid::MAX_CELLS);
  EXPECT_GE(geometry.rows, 1);
}

TEST(NumberGridGeometry, LandscapeRectStaysWithinTheCellCap) {
  const auto geometry = NumberGrid::geometryFor(LANDSCAPE_W, LANDSCAPE_H);

  EXPECT_EQ(geometry.cols, NumberGrid::MAX_COLS);
  EXPECT_LE(geometry.cellsPerPage(), NumberGrid::MAX_CELLS);
}

TEST(NumberGridGeometry, RowClampFiresInsteadOfOverflowing) {
  // The rows the height alone earns, before the clamp.
  const int unclampedRows = (PORTRAIT_H + NumberGrid::GAP) / (NumberGrid::MIN_CELL + NumberGrid::GAP);
  const auto geometry = NumberGrid::geometryFor(PORTRAIT_W, PORTRAIT_H);

  EXPECT_GT(unclampedRows * geometry.cols, NumberGrid::MAX_CELLS);
  EXPECT_LT(geometry.rows, unclampedRows);
  EXPECT_EQ(geometry.rows, NumberGrid::MAX_CELLS / geometry.cols);
}

TEST(NumberGridGeometry, NarrowRectKeepsTheMinimumColumns) {
  const auto geometry = NumberGrid::geometryFor(120, 200);

  EXPECT_EQ(geometry.cols, NumberGrid::MIN_COLS);
  EXPECT_GE(geometry.rows, 1);
  EXPECT_LE(geometry.cellsPerPage(), NumberGrid::MAX_CELLS);
}

TEST(NumberGridGeometry, DegenerateRectStillYieldsOneRow) {
  const auto geometry = NumberGrid::geometryFor(0, 0);

  EXPECT_EQ(geometry.cols, NumberGrid::MIN_COLS);
  EXPECT_EQ(geometry.rows, 1);
  EXPECT_TRUE(geometry.valid());
}

TEST(NumberGridGeometry, EveryOrientationHonoursTheCellCap) {
  for (int width = 8; width <= 800; width += 8) {
    for (int height = 8; height <= 800; height += 8) {
      const auto geometry = NumberGrid::geometryFor(width, height);
      EXPECT_LE(geometry.cellsPerPage(), NumberGrid::MAX_CELLS) << width << "x" << height;
      EXPECT_GE(geometry.cols, NumberGrid::MIN_COLS) << width << "x" << height;
      EXPECT_LE(geometry.cols, NumberGrid::MAX_COLS) << width << "x" << height;
      EXPECT_GE(geometry.rows, 1) << width << "x" << height;
    }
  }
}

TEST(NumberGridGeometry, CellSizeBoundsTheTouchTargetFloor) {
  const auto geometry = NumberGrid::geometryFor(PORTRAIT_W, PORTRAIT_H);
  const int cell = NumberGrid::cellSizeFor(PORTRAIT_W, PORTRAIT_H, geometry);

  // A minTouchSize above this would expand hit rects past their cell and
  // adjacent numbers would swallow each other's taps.
  const int cellW = (PORTRAIT_W - (geometry.cols - 1) * NumberGrid::GAP) / geometry.cols;
  const int cellH = (PORTRAIT_H - (geometry.rows - 1) * NumberGrid::GAP) / geometry.rows;
  EXPECT_EQ(cell, std::min(cellW, cellH));
  EXPECT_GT(cell, 0);
}

TEST(NumberGridPaging, Psalm119TakesFourPages) { EXPECT_EQ(NumberGrid::pageCount(PSALM_119_VERSES, 48), 4); }

TEST(NumberGridPaging, LastPageIsPartlyPadded) {
  const int lastPageFirst = NumberGrid::pageFirstCell(3, 48);

  EXPECT_EQ(lastPageFirst, 144);
  EXPECT_EQ(NumberGrid::cellsOnPage(PSALM_119_VERSES, lastPageFirst, 48), 32);
  EXPECT_EQ(48 - NumberGrid::cellsOnPage(PSALM_119_VERSES, lastPageFirst, 48), 16);
}

TEST(NumberGridPaging, ExactlyOnePageHasNoEmptyTrailingPage) {
  EXPECT_EQ(NumberGrid::pageCount(48, 48), 1);
  EXPECT_EQ(NumberGrid::cellsOnPage(48, 0, 48), 48);
  EXPECT_EQ(NumberGrid::pageCount(49, 48), 2);
}

TEST(NumberGridPaging, HandlesOneAndZeroCounts) {
  EXPECT_EQ(NumberGrid::pageCount(1, 48), 1);
  EXPECT_EQ(NumberGrid::cellsOnPage(1, 0, 48), 1);
  EXPECT_EQ(NumberGrid::pageCount(0, 48), 0);
  EXPECT_EQ(NumberGrid::cellsOnPage(0, 0, 48), 0);
  EXPECT_EQ(NumberGrid::pageStartFor(0, 0, 48), 0);
}

TEST(NumberGridPaging, IndexRoundTripsOnEveryPage) {
  constexpr int cellsPerPage = 42;
  for (int index = 0; index < PSALM_119_VERSES; index++) {
    const int page = NumberGrid::pageOfIndex(index, cellsPerPage);
    const int pageFirst = NumberGrid::pageFirstCell(page, cellsPerPage);
    const int cell = NumberGrid::pageRelativeIndex(index, pageFirst, cellsPerPage);

    ASSERT_GE(cell, 0) << index;
    ASSERT_LT(cell, cellsPerPage) << index;
    EXPECT_EQ(pageFirst + cell, index);
    EXPECT_EQ(NumberGrid::pageStartFor(index, PSALM_119_VERSES, cellsPerPage), pageFirst);
  }
}

TEST(NumberGridPaging, PageStartClampsAPageThatNoLongerExists) {
  // An orientation change shrinks the page; a selection kept from the wider
  // geometry must land on a page the new count still has.
  EXPECT_EQ(NumberGrid::pageStartFor(144, PSALM_119_VERSES, 48), 144);
  EXPECT_EQ(NumberGrid::pageStartFor(144, 40, 48), 0);
  EXPECT_EQ(NumberGrid::pageStartFor(-5, PSALM_119_VERSES, 48), 0);
}

TEST(NumberGridSelection, SelectedIndexIsPageRelative) {
  EXPECT_EQ(NumberGrid::pageRelativeIndex(144, NumberGrid::pageFirstCell(3, 48), 48), 0);
  EXPECT_EQ(NumberGrid::pageRelativeIndex(175, NumberGrid::pageFirstCell(3, 48), 48), 31);
  EXPECT_EQ(NumberGrid::pageRelativeIndex(0, 0, 48), 0);
}

TEST(NumberGridSelection, SelectionOffThePageRendersAsNoSelection) {
  const int pageFirst = NumberGrid::pageFirstCell(3, 48);

  EXPECT_EQ(NumberGrid::pageRelativeIndex(143, pageFirst, 48), -1);
  EXPECT_EQ(NumberGrid::pageRelativeIndex(192, pageFirst, 48), -1);
  EXPECT_EQ(NumberGrid::pageRelativeIndex(0, pageFirst, 48), -1);
}
