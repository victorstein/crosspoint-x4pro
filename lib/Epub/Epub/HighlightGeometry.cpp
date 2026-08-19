#include "HighlightGeometry.h"

#include <algorithm>

namespace {

bool isCoveredByAnyRange(const HighlightWord& word, const std::vector<VisibleRange>& ranges) {
  for (const auto& range : ranges) {
    if (range.contains(word.offset)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<HighlightRect> highlightRects(const std::vector<HighlightWord>& words,
                                          const std::vector<VisibleRange>& ranges, int16_t gapTolerance) {
  std::vector<HighlightWord> covered;
  for (const auto& word : words) {
    if (isCoveredByAnyRange(word, ranges)) {
      covered.push_back(word);
    }
  }

  std::sort(covered.begin(), covered.end(), [](const HighlightWord& a, const HighlightWord& b) {
    if (a.y != b.y) {
      return a.y < b.y;
    }
    return a.x < b.x;
  });

  std::vector<HighlightRect> rects;
  for (const auto& word : covered) {
    if (!rects.empty()) {
      HighlightRect& last = rects.back();
      const int16_t rightEdge = static_cast<int16_t>(last.x + last.w);
      if (last.y == word.y && (word.x - rightEdge) <= gapTolerance) {
        const int16_t wordRight = static_cast<int16_t>(word.x + word.w);
        last.w = static_cast<int16_t>(std::max(wordRight, rightEdge) - last.x);
        continue;
      }
    }
    rects.push_back(HighlightRect{word.x, word.y, word.w, word.h});
  }

  return rects;
}
