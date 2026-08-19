#pragma once

// Host-test stub for lib/hal/HalStorage.h, which pulls in <Print.h>,
// <freertos/semphr.h> and <common/FsApiConstants.h>. See HalDisplay.h in this
// directory for why a stub on the include path is sufficient.
//
// TextBlock.h, Bitmap.h and ImageBlock.h only take HalFile& in declarations, so
// an incomplete type would nearly do. The methods exist because TextBlock.cpp's
// serialize/deserialize call them; the host fake defines them as no-ops, since
// no host suite exercises page serialization.

#include <cstddef>
#include <cstdint>

class HalFile {
 public:
  size_t read(void* buf, size_t size);
  size_t write(const uint8_t* buf, size_t size);
  bool seek(size_t pos);
  size_t position();
  size_t size();
};
