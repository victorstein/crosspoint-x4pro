// Link-time host substitute for GfxRenderer.
//
// THIS IS NOT A SIMULATOR. It reproduces the *shape* of text metrics, not the
// values a real font produces. It exists so that pagination logic (ParsedText,
// TextBlock) can run on the host at all: GfxRenderer has no virtual functions,
// so there is no seam to inject a fake through, and GfxRenderer.h reaches
// <Arduino.h> via HalDisplay.h. The real GfxRenderer.h is still included here,
// unmodified -- so if layout code starts calling a renderer method this file
// does not define, the test link fails loudly rather than silently diverging
// from the firmware.
//
// What this fake CANNOT tell you, and what therefore remains an on-device
// concern:
//   - Fidelity: whether the device breaks lines where these tests say it does.
//     These tests assert self-consistency of the anchoring layer, not agreement
//     with real font metrics.
//   - fp4 fixed-point rounding, in particular getSpaceAdvance's documented
//     single-snap-vs-separate-snap +/-1 px behaviour.
//   - Real kerning: getKerning returns 0 here. That is safe only because
//     kerning affects x positions and never offset bookkeeping -- an invariant
//     this fake cannot itself police.
//   - SD-card font paths: isSdCardFont() is inline in the real header and
//     returns false against the empty map, so SD prewarming and font scaling
//     are untested here.

#include <GfxRenderer.h>

#include <cstdint>

namespace {

// CRITICAL: glyph advance is a function of fontId. Do NOT "simplify" this to a
// constant -- with a constant advance, changing the font size changes nothing,
// every repagination sweep collapses to a single layout, and the entire suite
// goes green while asserting nothing at all. The fontId IS the font size here.
//
// Widths are also per-codepoint-class rather than uniform. Asymmetric widths
// make word order observable in the geometry, which is what catches an offset
// computed from a visual index where a logical one was meant (and vice versa);
// uniform widths mask exactly that class of bug.
int classFactor(const uint32_t cp) {
  switch (cp) {
    case 'i':
    case 'l':
    case 't':
    case 'j':
    case 'I':
      return 2;  // narrow
    case 'm':
    case 'w':
    case 'M':
    case 'W':
      return 6;  // wide
    default:
      return 4;  // normal
  }
}

int advanceFor(const uint32_t cp, const int fontId) { return (fontId * classFactor(cp)) / 4; }

// Minimal UTF-8 decode; the fake must charge per codepoint, not per byte, or
// multi-byte scripts would measure several times too wide.
uint32_t nextCp(const unsigned char** p) {
  const unsigned char c = **p;
  if (c < 0x80) {
    ++(*p);
    return c;
  }
  int extra = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    extra = 2;
  } else {
    cp = c & 0x07;
    extra = 3;
  }
  ++(*p);
  for (int i = 0; i < extra && **p; ++i, ++(*p)) cp = (cp << 6) | (**p & 0x3F);
  return cp;
}

int measure(const char* text, const int fontId) {
  int total = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) total += advanceFor(nextCp(&p), fontId);
  return total;
}

}  // namespace

int GfxRenderer::getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }

int GfxRenderer::getSpaceWidth(const int fontId, EpdFontFamily::Style) const { return advanceFor(' ', fontId); }

int GfxRenderer::getSpaceAdvance(const int fontId, uint32_t, uint32_t, EpdFontFamily::Style) const {
  return advanceFor(' ', fontId);
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style) const {
  return measure(text, fontId);
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, EpdFontFamily::Style, BidiUtils::BidiBaseDir) const {
  return measure(text, fontId);
}

int GfxRenderer::getFontAscenderSize(const int fontId) const { return fontId; }

void GfxRenderer::ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
void GfxRenderer::ensureSdCardFontReady(int, const char*, uint8_t) const {}

// Draw calls are unreachable in a pagination test, but TextBlock.cpp references
// them, so the link needs bodies.
void GfxRenderer::drawText(int, int, int, const char*, bool, EpdFontFamily::Style, BidiUtils::BidiBaseDir) const {}
void GfxRenderer::drawLine(int, int, int, int, int, bool) const {}
bool GfxRenderer::isFontCacheScanning() const { return false; }

// Called by the inline ~GfxRenderer().
void GfxRenderer::freeBwBufferChunks() {}

// TextBlock::serialize/deserialize reference these. No host suite exercises
// page serialization, so they are inert.
size_t HalFile::read(void*, size_t) { return 0; }
size_t HalFile::write(const uint8_t*, size_t) { return 0; }
bool HalFile::seek(size_t) { return false; }
size_t HalFile::position() { return 0; }
size_t HalFile::size() { return 0; }
