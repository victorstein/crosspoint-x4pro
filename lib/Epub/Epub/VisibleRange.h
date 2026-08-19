#pragma once

#include <cstdint>

// A half-open range of visible-codepoint offsets within one spine item, as stored
// by a highlight. Dependency-free so the selection rule can be unit tested on the
// host, away from the renderer and storage layers.
struct VisibleRange {
  uint32_t start = 0;
  uint32_t end = 0;  // exclusive

  constexpr bool isEmpty() const { return end <= start; }

  // True when a word at `offset` falls inside the range. Half-open so two
  // adjacent highlights cannot both claim the same word.
  constexpr bool contains(const uint32_t offset) const { return offset >= start && offset < end; }

  constexpr bool overlaps(const VisibleRange& other) const {
    return !isEmpty() && !other.isEmpty() && start < other.end && other.start < end;
  }
};
