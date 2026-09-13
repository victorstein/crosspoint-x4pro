#pragma once

#include <algorithm>
#include <cstdint>

// Paged number-grid arithmetic for the Bible chapter and verse levels: how many
// cells a content rect earns, and how a flat 1..N range maps onto pages of them.
// Deliberately free of FreeInkUI, Arduino and GfxRenderer so the host suite can
// exercise it (test/number_grid).
namespace NumberGrid {

constexpr int GAP = 8;
constexpr int MIN_COLS = 4;
constexpr int MAX_COLS = 8;
// A page must leave interaction slots for the header, footer and any other
// chrome sharing the frame: UiAppHost budgets 64, and 64 cells would spend all
// of it -- past the budget hit rects are dropped silently and those cells
// become untappable.
constexpr int MAX_CELLS = 48;
// Comfortable tap target on the 800x480 panel; only device iteration settles it.
constexpr int MIN_CELL = 56;

struct Geometry {
  int cols = 0;
  int rows = 0;

  constexpr int cellsPerPage() const { return cols * rows; }
  constexpr bool valid() const { return cols > 0 && rows > 0; }
};

// Columns follow the width, then rows are clamped so cols*rows stays within
// MAX_CELLS. Clamping rows rather than cols keeps the column count the width
// earned, so cells stay square-ish.
inline Geometry geometryFor(const int contentW, const int contentH, const int minCell = MIN_CELL, const int gap = GAP) {
  const int stride = minCell + gap;
  const int fitCols = contentW > 0 ? (contentW + gap) / stride : 0;
  const int cols = std::clamp(fitCols, MIN_COLS, MAX_COLS);
  const int fitRows = contentH > 0 ? (contentH + gap) / stride : 0;
  const int rows = std::min(std::max(fitRows, 1), MAX_CELLS / cols);
  return Geometry{cols, rows};
}

// The cell size keyGrid derives from the same rect, as its smaller side.
inline int cellSizeFor(const int rectW, const int rectH, const Geometry& geometry, const int gap = GAP) {
  if (!geometry.valid()) return 0;
  const int cellW = (rectW - (geometry.cols - 1) * gap) / geometry.cols;
  const int cellH = (rectH - (geometry.rows - 1) * gap) / geometry.rows;
  return std::max(std::min(cellW, cellH), 0);
}

inline int pageCount(const int count, const int cellsPerPage) {
  if (count <= 0 || cellsPerPage <= 0) return 0;
  return (count + cellsPerPage - 1) / cellsPerPage;
}

inline int pageOfIndex(const int index, const int cellsPerPage) {
  if (index <= 0 || cellsPerPage <= 0) return 0;
  return index / cellsPerPage;
}

inline int pageFirstCell(const int page, const int cellsPerPage) {
  if (page <= 0 || cellsPerPage <= 0) return 0;
  return page * cellsPerPage;
}

// Cells on the page starting at pageFirst that carry a real number; the rest of
// the page is padded so the grid stays rectangular.
inline int cellsOnPage(const int count, const int pageFirst, const int cellsPerPage) {
  if (cellsPerPage <= 0) return 0;
  return std::clamp(count - pageFirst, 0, cellsPerPage);
}

// keyGrid matches its selectedIndex against a PAGE-RELATIVE cell index, while
// the cells carry absolute indexes as their action value. A selection off the
// current page must render as no selection at all.
inline int pageRelativeIndex(const int absolute, const int pageFirst, const int cellsPerPage) {
  if (cellsPerPage <= 0 || absolute < pageFirst || absolute >= pageFirst + cellsPerPage) return -1;
  return absolute - pageFirst;
}

// First cell of the page holding `index`, clamped into a count that may have
// shrunk -- or a geometry that may have changed pages out from under it.
inline int pageStartFor(const int index, const int count, const int cellsPerPage) {
  if (count <= 0 || cellsPerPage <= 0) return 0;
  const int clampedIndex = std::clamp(index, 0, count - 1);
  return pageFirstCell(pageOfIndex(clampedIndex, cellsPerPage), cellsPerPage);
}

}  // namespace NumberGrid
