#pragma once

#include <Epub.h>

#include <cstddef>
#include <cstdint>
#include <memory>

class GfxRenderer;

// Shared forward pass over one spine item's inflated XHTML, for the callers
// that need the source bytes rather than a laid-out page: the Bible navigator's
// book/verse scans and the reader's chapter-number lookup.
namespace SpineHtmlStream {

// Chunk sink. Returning false stops the pass. A function pointer rather than
// std::function: the latter heap-allocates its closure and costs KBs of binary
// per signature.
using ChunkSink = bool (*)(void* ctx, const char* chunk, size_t length, bool isFinal);

// Above this an inflate is slow enough to want the indexing popup. Nav pages
// top out around 14 KB and never reach it; a long chapter (Psalm 119 at
// ~69 KB) does.
constexpr size_t INFLATE_POPUP_BYTE_THRESHOLD = 32 * 1024;

// What to do when this spine item's XHTML is not on SD yet.
enum class WhenMissing : uint8_t {
  // Inflate it from the zip: a multi-second stall on a large spine, and it
  // takes the render lock to borrow the framebuffer for the popup.
  Inflate,
  // Give up instead. The only safe choice for a caller that already holds the
  // render lock -- it is a plain FreeRTOS mutex, so taking it twice deadlocks.
  Fail,
};

// Feeds `sink` the XHTML of `spineIndex` in bounded chunks. Going through
// Section rather than re-inflating from the zip means the chapter the user is
// about to open keeps the HTML cache this produced, so opening it does not pay
// the inflate twice.
bool stream(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer, ChunkSink sink, void* ctx,
            WhenMissing whenMissing = WhenMissing::Inflate);

}  // namespace SpineHtmlStream
