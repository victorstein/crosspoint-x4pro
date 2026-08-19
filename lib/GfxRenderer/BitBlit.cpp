#include "BitBlit.h"

namespace bitblit {

void invertRect(uint8_t* buf, const int32_t stride, const int x0, const int y0, const int x1, const int y1) {
  if (x1 < x0 || y1 < y0) return;

  const int byteStart = x0 >> 3;
  const int byteEnd = x1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (x0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (x1 & 7)));

  for (int y = y0; y <= y1; y++) {
    uint8_t* row = buf + static_cast<int32_t>(y) * stride;
    if (byteStart == byteEnd) {
      row[byteStart] ^= static_cast<uint8_t>(headMask & tailMask);
      continue;
    }
    row[byteStart] ^= headMask;
    for (int b = byteStart + 1; b < byteEnd; b++) {
      row[b] ^= 0xFFu;
    }
    row[byteEnd] ^= tailMask;
  }
}

}  // namespace bitblit
