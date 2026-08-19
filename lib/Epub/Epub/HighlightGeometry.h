#pragma once

#include <cstdint>
#include <vector>

#include "VisibleRange.h"

// A rendered word reduced to what highlighting needs. Screen coordinates,
// already including margins and ruby shift. Width is derived from the next
// word's x position, never measured through the renderer.
struct HighlightWord {
  uint32_t offset;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

struct HighlightRect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

// Rectangles to invert so every word inside any range is covered.
//
// Words on the same row are merged when the next box starts within `gapTolerance`
// of the previous box's right edge, so inter-word gaps invert too and a
// multi-word highlight reads as one block rather than striped text. The
// tolerance is a parameter because inter-word gaps scale with font size: a fixed
// value under-merges at large sizes and can merge across a paragraph indent at
// small ones. Rows never merge.
//
// A word covered by several ranges yields ONE rect. Inverting the same pixels
// twice restores them, so a double-covered word would render un-highlighted.
//
// Word order is NOT assumed ascending by offset: TextBlock stores words in
// visual order, which runs backwards on an RTL line.
std::vector<HighlightRect> highlightRects(const std::vector<HighlightWord>& words,
                                          const std::vector<VisibleRange>& ranges, int16_t gapTolerance);
