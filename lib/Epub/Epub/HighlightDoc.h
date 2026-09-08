// lib/Epub/Epub/HighlightDoc.h
#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "HighlightEntry.h"

// A book's highlights and tag palette, with all format rules and no storage
// access. The storage shell is src/util/HighlightFile.
//
// Bytes, not entry counts, are the safety invariant: per-entry serialised size
// varies by more than 5x with offset width, tag references and label escaping,
// so HighlightFile::save measures the document and refuses when it exceeds the
// budget. The caps below bound growth; they do not guarantee the byte bound.
class HighlightDoc {
 public:
  static constexpr int FORMAT_VERSION = 1;
  static constexpr size_t MAX_HIGHLIGHTS = 400;  // ~the spec's reachable ceiling
  static constexpr size_t MAX_TAGS = 100;
  static constexpr size_t MAX_TAGS_PER_HIGHLIGHT = 8;
  static constexpr size_t MAX_TAG_NAME_BYTES = 24;
  // A reference is composed from a book's own table-of-contents title
  // (PassageSelectActivity::verseReference), which is arbitrary text, so it is
  // bounded here rather than trusted.
  static constexpr size_t MAX_REFERENCE_BYTES = 48;

  const std::vector<std::string>& tags() const { return tags_; }
  const std::vector<HighlightEntry>& highlights() const { return highlights_; }

  // Adds the tag if new; returns its index, or nullopt when the palette is full
  // or the name is empty. Returns an optional rather than a sentinel because a
  // signed -1 stored into the uint16_t tagIndices becomes 65535 and then indexes
  // tags_ out of bounds.
  std::optional<uint16_t> addTag(const std::string& name);

  // Removes tag `index`, renumbering every reference so highlights still resolve
  // to the same tag NAMES. Out-of-range is a no-op.
  void removeTag(uint16_t index);

  bool addHighlight(HighlightEntry entry);
  bool removeHighlight(size_t index);

  // Replaces entry `index`'s tags. Returns false when `index` is out of range;
  // the entry is untouched in that case.
  //
  // Drops references outside the palette, collapses repeats, and caps at
  // MAX_TAGS_PER_HIGHLIGHT. Note this is deliberately STRICTER than the parse
  // path: fromJson drops out-of-range refs and caps, but does NOT dedupe
  // (HighlightDoc.cpp:111-117), so {"t":[0,0,0]} parses to three copies. A UI
  // caller can produce repeats by toggling; JSON on disk comes from toJson,
  // which never emits them.
  bool setTags(size_t index, std::vector<uint16_t> tagIndices);

  // Highlights in the given spine item, in stored order. The render pass needs
  // this every page turn.
  std::vector<const HighlightEntry*> findBySpine(uint16_t spineIndex) const;

  void toJson(JsonDocument& doc) const;

  // Parses and VALIDATES. Rejects a future format version. Drops tag references
  // that fall outside the palette, truncates at MAX_HIGHLIGHTS, re-normalises
  // labels, and range-checks spine indices — a hand-edited or foreign file must
  // not be able to produce an out-of-bounds read later.
  bool fromJson(JsonVariantConst doc);

 private:
  std::vector<std::string> tags_;
  std::vector<HighlightEntry> highlights_;
};
