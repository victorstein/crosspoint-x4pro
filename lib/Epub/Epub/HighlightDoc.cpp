#include "HighlightDoc.h"

#include <Utf8.h>

#include <utility>

std::optional<uint16_t> HighlightDoc::addTag(const std::string& name) {
  if (name.empty() || name.size() > MAX_TAG_NAME_BYTES) return std::nullopt;

  for (size_t i = 0; i < tags_.size(); ++i) {
    if (tags_[i] == name) return static_cast<uint16_t>(i);
  }

  if (tags_.size() >= static_cast<size_t>(MAX_TAGS)) return std::nullopt;

  tags_.push_back(name);
  return static_cast<uint16_t>(tags_.size() - 1);
}

void HighlightDoc::removeTag(uint16_t index) {
  if (static_cast<size_t>(index) >= tags_.size()) return;
  // Stub: erases the tag but does not renumber tagIndices in highlights_, so
  // any reference past `index` now points at the wrong palette entry. Full
  // renumbering is Task 5.
  tags_.erase(tags_.begin() + index);
}

bool HighlightDoc::addHighlight(HighlightEntry entry) {
  if (highlights_.size() >= static_cast<size_t>(MAX_HIGHLIGHTS)) return false;

  if (entry.tagIndices.size() > static_cast<size_t>(MAX_TAGS_PER_HIGHLIGHT)) {
    entry.tagIndices.resize(MAX_TAGS_PER_HIGHLIGHT);
  }
  entry.label = utf8SafeSummary(std::move(entry.label));

  highlights_.push_back(std::move(entry));
  return true;
}

bool HighlightDoc::removeHighlight(size_t index) {
  if (index >= highlights_.size()) return false;
  highlights_.erase(highlights_.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

std::vector<const HighlightEntry*> HighlightDoc::findBySpine(uint16_t spineIndex) const {
  std::vector<const HighlightEntry*> found;
  for (const auto& h : highlights_) {
    if (h.spineIndex == spineIndex) found.push_back(&h);
  }
  return found;
}

void HighlightDoc::toJson(JsonDocument& doc) const {
  doc.clear();
  doc["v"] = FORMAT_VERSION;

  JsonArray tagsArr = doc["tags"].to<JsonArray>();
  for (const auto& tag : tags_) tagsArr.add(tag);

  JsonArray highlightsArr = doc["highlights"].to<JsonArray>();
  for (const auto& h : highlights_) {
    JsonObject o = highlightsArr.add<JsonObject>();
    o["si"] = h.spineIndex;
    o["start"] = h.range.start;
    o["end"] = h.range.end;
    if (!h.tagIndices.empty()) {
      JsonArray t = o["t"].to<JsonArray>();
      for (const uint16_t idx : h.tagIndices) t.add(idx);
    }
    o["text"] = h.label;
  }
}

bool HighlightDoc::fromJson(JsonVariantConst doc) {
  const int version = doc["v"] | 0;
  if (version > FORMAT_VERSION) return false;

  std::vector<std::string> tags;
  const JsonArrayConst tagsArr = doc["tags"].as<JsonArrayConst>();
  for (const JsonVariantConst t : tagsArr) {
    if (tags.size() >= static_cast<size_t>(MAX_TAGS)) break;
    std::string name(t | "");
    if (name.size() > MAX_TAG_NAME_BYTES) name.resize(MAX_TAG_NAME_BYTES);
    tags.push_back(std::move(name));
  }

  std::vector<HighlightEntry> highlights;
  const JsonArrayConst highlightsArr = doc["highlights"].as<JsonArrayConst>();
  for (const JsonVariantConst item : highlightsArr) {
    if (highlights.size() >= static_cast<size_t>(MAX_HIGHLIGHTS)) break;

    const JsonObjectConst o = item.as<JsonObjectConst>();

    const int spineIndex = o["si"] | 0;
    if (spineIndex < 0 || spineIndex > 65535) continue;  // must not wrap into uint16_t range

    HighlightEntry entry;
    entry.spineIndex = static_cast<uint16_t>(spineIndex);
    entry.range.start = o["start"] | 0u;
    entry.range.end = o["end"] | 0u;

    const JsonArrayConst tagRefs = o["t"].as<JsonArrayConst>();
    for (const JsonVariantConst ref : tagRefs) {
      if (entry.tagIndices.size() >= static_cast<size_t>(MAX_TAGS_PER_HIGHLIGHT)) break;
      const int idx = ref | -1;
      if (idx < 0 || static_cast<size_t>(idx) >= tags.size()) continue;  // outside the palette
      entry.tagIndices.push_back(static_cast<uint16_t>(idx));
    }

    entry.label = utf8SafeSummary(std::string(o["text"] | ""));

    highlights.push_back(std::move(entry));
  }

  tags_ = std::move(tags);
  highlights_ = std::move(highlights);
  return true;
}
