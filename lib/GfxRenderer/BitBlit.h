#pragma once

#include <cstdint>

// Bit-level framebuffer operations, deliberately free of display and Arduino
// dependencies so they can be unit tested on the host. The layout matches
// GfxRenderer's framebuffer: 1bpp, MSB-first within a byte (x -> bit 7 - (x & 7)),
// rows of `stride` bytes.
namespace bitblit {

// Flips every bit in the inclusive pixel range [x0, x1] on each row in [y0, y1].
// Coordinates are physical framebuffer coordinates and MUST already be clipped:
// out-of-range input writes out of bounds. x0 and y0 must be >= 0, and stride
// must be positive — a negative stride would silently walk rows backward.
void invertRect(uint8_t* buf, int32_t stride, int x0, int y0, int x1, int y1);

}  // namespace bitblit
