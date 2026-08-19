#pragma once

// Host-test stub for lib/hal/HalDisplay.h, which pulls in <Arduino.h> and the
// whole freeink-sdk panel-driver chain (EInkDisplay -> FreeInkDisplay ->
// BoardConfig -> EpdBus, SPI, driver/gpio, esp_rom_sys).
//
// The real header is at lib/hal/HalDisplay.h and is included as <HalDisplay.h>,
// resolved off the include path. Host test targets never put lib/hal on that
// path, so this file is the SOLE provider of the name -- it is not shadowing
// anything, and there is no include-order hazard to get wrong.
//
// GfxRenderer.h needs exactly this much of HalDisplay: one reference member,
// four dimension constants used as default member initializers, and the
// RefreshMode enum in a handful of signatures.

#include <cstdint>

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  static constexpr uint16_t DISPLAY_WIDTH = 480;
  static constexpr uint16_t DISPLAY_HEIGHT = 800;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
};
