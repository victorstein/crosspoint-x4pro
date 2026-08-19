#include "HighlightOverlay.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>

#include <algorithm>

namespace HighlightOverlay {

std::vector<HighlightRect> buildRects(const Page& page, const std::vector<VisibleRange>& ranges,
                                      const int marginLeft, const int marginTop, const int columnRight,
                                      const int lineHeight, const int ascender, const int16_t gapTolerance) {
  if (ranges.empty()) return {};

  std::vector<HighlightWord> words;
  words.reserve(64);

  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    const uint16_t wordCount = block->wordCount();
    if (wordCount == 0) continue;

    const int rubyShift = block->getRubyShift(ascender);
    const int16_t y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
    // Shared by every word on this line: its block's own right inset
    // subtracted from the content column's right edge. See the header for why
    // the last word needs this instead of the next word's x.
    const int16_t lineRight = static_cast<int16_t>(columnRight - block->getBlockStyle().rightInset());

    for (uint16_t i = 0; i < wordCount; i++) {
      const int16_t x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      const int16_t w = (i + 1 < wordCount)
                            ? static_cast<int16_t>(block->wordXpos(i + 1) - block->wordXpos(i))
                            : static_cast<int16_t>(std::max<int>(1, lineRight - x));
      words.push_back(HighlightWord{block->wordVisibleOffset(i), x, y, w, static_cast<int16_t>(lineHeight)});
    }
  }

  return highlightRects(words, ranges, gapTolerance);
}

}  // namespace HighlightOverlay
