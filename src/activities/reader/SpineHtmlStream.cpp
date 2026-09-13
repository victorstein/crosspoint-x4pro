#include "SpineHtmlStream.h"

#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace {
constexpr size_t HTML_CHUNK_BYTES = 2048;
}  // namespace

namespace SpineHtmlStream {

bool stream(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer, const ChunkSink sink,
            void* ctx, const WhenMissing whenMissing) {
  if (!epub || spineIndex < 0) return false;

  Section section(epub, spineIndex, renderer);
  std::string parsePath;
  bool promoted = true;
  std::string tmpHtmlPath;

  if (section.hasHtmlCache()) {
    parsePath = section.htmlCachePath();
  } else if (whenMissing == WhenMissing::Fail) {
    return false;
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
      LOG_ERR("SHS", "Failed to inflate spine %d", spineIndex);
      return false;
    }
  }

  auto buffer = makeUniqueNoThrow<char[]>(HTML_CHUNK_BYTES);
  if (!buffer) {
    LOG_ERR("SHS", "OOM: %d bytes", static_cast<int>(HTML_CHUNK_BYTES));
    if (!promoted) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  bool ok = false;
  {
    HalFile file;
    if (Storage.openFileForRead("SHS", parsePath, file)) {
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

}  // namespace SpineHtmlStream
