#pragma once

#include <cstdint>
#include <vector>

#include "Epub/HighlightGeometry.h"
#include "Epub/VisibleRange.h"

class Page;

// Turns a page's already-laid-out words plus a set of visible-offset ranges into
// the rectangles to invert, in one walk. Pure page-data read: never calls
// getTextAdvanceX or ensureSdCardFontReady, so it is safe to call from inside a
// render pass (see EpubReaderActivity::renderContents, which computes this once
// per page turn and replays the result across every grayscale strip instead of
// re-walking the page ~12 times).
namespace HighlightOverlay {

// `marginLeft`/`marginTop` match the offsets `page->render` was called with.
// `columnRight` is the screen-space x of the content column's right edge
// (viewport right minus the reader's right margin), in that same coordinate
// space. TextBlock stores only each word's start x with no end-of-line
// sentinel, so unlike every other word on a line, the last word's width can't
// be derived from the next word's position -- its right edge is instead taken
// from the line's own extent: `columnRight` less that line's block-level right
// inset. `lineHeight`/`ascender` are the font metrics `page->render` itself
// used to lay the page out.
std::vector<HighlightRect> buildRects(const Page& page, const std::vector<VisibleRange>& ranges, int marginLeft,
                                      int marginTop, int columnRight, int lineHeight, int ascender,
                                      int16_t gapTolerance);

}  // namespace HighlightOverlay
