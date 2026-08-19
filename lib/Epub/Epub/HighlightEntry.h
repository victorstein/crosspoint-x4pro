// lib/Epub/Epub/HighlightEntry.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "VisibleRange.h"

// One highlighted passage. The range is absolute visible-codepoint offsets within
// the spine item, so it survives re-pagination. `label` is display-only and must
// never be used to locate the passage.
struct HighlightEntry {
  uint16_t spineIndex = 0;
  VisibleRange range;
  std::vector<uint16_t> tagIndices;
  std::string label;
};
