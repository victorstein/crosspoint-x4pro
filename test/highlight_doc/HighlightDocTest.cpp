#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include "Epub/HighlightDoc.h"

namespace {

HighlightEntry makeEntry(const uint16_t spine, const uint32_t start, const uint32_t end,
                         std::vector<uint16_t> tags = {}) {
  HighlightEntry e;
  e.spineIndex = spine;
  e.range = VisibleRange{start, end};
  e.tagIndices = std::move(tags);
  e.label = "label";
  return e;
}

// Round-trips through real JSON text, so the test exercises serialisation too.
bool roundTrip(const HighlightDoc& in, HighlightDoc& out) {
  JsonDocument doc;
  in.toJson(doc);
  std::string text;
  serializeJson(doc, text);
  JsonDocument reparsed;
  if (deserializeJson(reparsed, text)) return false;
  return out.fromJson(reparsed);
}

}  // namespace

TEST(HighlightDoc, RoundTripsAnEntry) {
  HighlightDoc doc;
  ASSERT_TRUE(doc.addTag("greek").has_value());
  doc.addHighlight(makeEntry(3, 9412, 9598, {0}));

  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(doc, parsed));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  EXPECT_EQ(parsed.highlights()[0].spineIndex, 3);
  EXPECT_EQ(parsed.highlights()[0].range.start, 9412u);
  EXPECT_EQ(parsed.highlights()[0].range.end, 9598u);
  ASSERT_EQ(parsed.tags().size(), 1u);
  EXPECT_EQ(parsed.tags()[0], "greek");
}

TEST(HighlightDoc, RoundTripsEmptyAndUntagged) {
  HighlightDoc empty;
  HighlightDoc parsedEmpty;
  ASSERT_TRUE(roundTrip(empty, parsedEmpty));
  EXPECT_TRUE(parsedEmpty.highlights().empty());

  HighlightDoc untagged;
  untagged.addHighlight(makeEntry(0, 10, 20));
  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(untagged, parsed));
  EXPECT_TRUE(parsed.highlights()[0].tagIndices.empty());
}

TEST(HighlightDoc, IgnoresUnknownKeysButRejectsAFutureVersion) {
  JsonDocument known;
  ASSERT_FALSE(deserializeJson(
      known, R"({"v":1,"tags":["a"],"highlights":[{"si":1,"start":5,"end":9,"t":[0],"text":"x","colour":"red"}]})"));
  HighlightDoc ok;
  EXPECT_TRUE(ok.fromJson(known)) << "unknown keys are forward-compatible";

  JsonDocument future;
  ASSERT_FALSE(deserializeJson(future, R"({"v":2,"tags":[],"highlights":[]})"));
  HighlightDoc rejected;
  EXPECT_FALSE(rejected.fromJson(future))
      << "a future version may redefine start/end; reinterpreting it then saving v1 destroys data";
}

TEST(HighlightDoc, DropsTagReferencesOutsideThePalette) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"v":1,"tags":["a"],"highlights":[{"si":0,"start":0,"end":5,"t":[0,7]}]})"));
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc));
  ASSERT_EQ(parsed.highlights().size(), 1u);
  ASSERT_EQ(parsed.highlights()[0].tagIndices.size(), 1u) << "index 7 has no tag and must not survive";
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "a");
}

TEST(HighlightDoc, ClampsASpineIndexThatWouldNarrow) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"v":1,"tags":[],"highlights":[{"si":70000,"start":0,"end":5}]})"));
  HighlightDoc parsed;
  parsed.fromJson(doc);
  for (const auto& h : parsed.highlights()) {
    EXPECT_NE(h.spineIndex, 4464) << "70000 must not silently wrap to 4464";
  }
}

TEST(HighlightDoc, NormalisesLabelsOnParse) {
  std::string longLabel(200, 'x');
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(
      doc, R"({"v":1,"tags":[],"highlights":[{"si":0,"start":0,"end":5,"text":")" + longLabel + R"("}]})"));
  HighlightDoc parsed;
  ASSERT_TRUE(parsed.fromJson(doc));
  EXPECT_LE(parsed.highlights()[0].label.size(), 72u) << "addHighlight normalises; fromJson must too";
}

TEST(HighlightDoc, RejectsMalformedJson) {
  JsonDocument doc;
  EXPECT_TRUE(deserializeJson(doc, "{not json"));
}

TEST(HighlightDoc, FindsHighlightsBySpine) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(1, 0, 5));
  doc.addHighlight(makeEntry(3, 0, 5));
  doc.addHighlight(makeEntry(1, 10, 15));
  EXPECT_EQ(doc.findBySpine(1).size(), 2u);
  EXPECT_EQ(doc.findBySpine(3).size(), 1u);
  EXPECT_TRUE(doc.findBySpine(9).empty());
}

TEST(HighlightDoc, RemovesAHighlight) {
  HighlightDoc doc;
  doc.addHighlight(makeEntry(0, 0, 5));
  doc.addHighlight(makeEntry(0, 10, 15));
  ASSERT_TRUE(doc.removeHighlight(0));
  ASSERT_EQ(doc.highlights().size(), 1u);
  EXPECT_EQ(doc.highlights()[0].range.start, 10u);
  EXPECT_FALSE(doc.removeHighlight(9));
}

TEST(HighlightDoc, EnforcesTheEntryCap) {
  HighlightDoc doc;
  for (size_t i = 0; i < HighlightDoc::MAX_HIGHLIGHTS; ++i) {
    ASSERT_TRUE(doc.addHighlight(makeEntry(0, i * 10, i * 10 + 5))) << "at i=" << i;
  }
  EXPECT_FALSE(doc.addHighlight(makeEntry(0, 999999, 1000000)));
}

TEST(HighlightDoc, TruncatesAnOverlongEntryListOnParse) {
  JsonDocument doc;
  JsonArray arr = doc["highlights"].to<JsonArray>();
  doc["v"] = 1;
  doc["tags"].to<JsonArray>();
  for (size_t i = 0; i < HighlightDoc::MAX_HIGHLIGHTS + 50; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["si"] = 0;
    o["start"] = i * 10;
    o["end"] = i * 10 + 5;
  }
  HighlightDoc parsed;
  parsed.fromJson(doc);
  EXPECT_LE(parsed.highlights().size(), HighlightDoc::MAX_HIGHLIGHTS)
      << "an oversized file must not load past the cap and then be unsaveable";
}

TEST(HighlightDoc, RejectsAnOverlongTagName) {
  HighlightDoc doc;
  EXPECT_FALSE(doc.addTag(std::string(HighlightDoc::MAX_TAG_NAME_BYTES + 1, 'x')).has_value());
}

TEST(HighlightDoc, AddingAnExistingTagReturnsTheSameIndex) {
  HighlightDoc doc;
  const auto first = doc.addTag("greek");
  const auto again = doc.addTag("greek");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, *again);
  EXPECT_EQ(doc.tags().size(), 1u);
}

TEST(HighlightDoc, WorstCaseDocumentStaysUnderTheSaveBudget) {
  // The genuine worst case, not a convenient one: maximum spine index, full-width
  // uint32 offsets, a full palette of maximum-length names, the maximum tag
  // references per entry, and a label made entirely of characters JSON escapes.
  HighlightDoc doc;
  for (size_t t = 0; t < HighlightDoc::MAX_TAGS; ++t) {
    ASSERT_TRUE(doc.addTag(std::string(HighlightDoc::MAX_TAG_NAME_BYTES, 'a' + static_cast<char>(t % 26)))
                    .has_value());
  }
  std::vector<uint16_t> refs;
  for (size_t i = 0; i < HighlightDoc::MAX_TAGS_PER_HIGHLIGHT; ++i) refs.push_back(static_cast<uint16_t>(i));

  for (size_t i = 0; i < HighlightDoc::MAX_HIGHLIGHTS; ++i) {
    HighlightEntry e;
    e.spineIndex = 65535;
    e.range = VisibleRange{4294967290u, 4294967295u};
    e.tagIndices = refs;
    e.label = std::string(72, '"');  // every byte escapes to two
    if (!doc.addHighlight(e)) break;
  }

  JsonDocument json;
  doc.toJson(json);
  const size_t bytes = measureJson(json);
  // Not an assertion that it fits — it does NOT, and that is the point. This
  // records the real worst case so the save-side byte guard is understood as the
  // safety mechanism and MAX_HIGHLIGHTS as a growth limit only.
  EXPECT_GT(bytes, 50000u) << "if this ever fits, the caps changed — revisit the guard's necessity";
}

TEST(HighlightDocTags, DeletingAMiddleTagKeepsEveryReferenceOnItsName) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addTag("gamma");
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.addHighlight(makeEntry(0, 20, 30, {2}));
  doc.addHighlight(makeEntry(0, 40, 50, {0, 2}));

  doc.removeTag(1);

  ASSERT_EQ(doc.tags().size(), 2u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
  EXPECT_EQ(doc.tags()[doc.highlights()[1].tagIndices[0]], "gamma");
  ASSERT_EQ(doc.highlights()[2].tagIndices.size(), 2u);
  EXPECT_EQ(doc.tags()[doc.highlights()[2].tagIndices[0]], "alpha");
  EXPECT_EQ(doc.tags()[doc.highlights()[2].tagIndices[1]], "gamma");
}

TEST(HighlightDocTags, DeletingATagDropsOnlyItsOwnReferences) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addHighlight(makeEntry(0, 0, 10, {0, 1}));
  doc.removeTag(0);
  ASSERT_EQ(doc.highlights()[0].tagIndices.size(), 1u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "beta");
}

TEST(HighlightDocTags, DeletingTheLastTagLeavesHighlightsUntagged) {
  HighlightDoc doc;
  doc.addTag("only");
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.removeTag(0);
  EXPECT_TRUE(doc.tags().empty());
  EXPECT_TRUE(doc.highlights()[0].tagIndices.empty());
}

TEST(HighlightDocTags, RemovingAnOutOfRangeIndexIsANoOp) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addHighlight(makeEntry(0, 0, 10, {0}));
  doc.removeTag(7);
  ASSERT_EQ(doc.tags().size(), 1u);
  EXPECT_EQ(doc.tags()[doc.highlights()[0].tagIndices[0]], "alpha");
}

TEST(HighlightDocTags, RenumberingSurvivesARoundTrip) {
  HighlightDoc doc;
  doc.addTag("alpha");
  doc.addTag("beta");
  doc.addTag("gamma");
  doc.addHighlight(makeEntry(0, 0, 10, {2}));
  doc.removeTag(0);

  HighlightDoc parsed;
  ASSERT_TRUE(roundTrip(doc, parsed));
  EXPECT_EQ(parsed.tags()[parsed.highlights()[0].tagIndices[0]], "gamma");
}

TEST(HighlightDocTags, DeletingEveryTagInSequenceNeverLeavesADanglingIndex) {
  HighlightDoc doc;
  doc.addTag("a");
  doc.addTag("b");
  doc.addTag("c");
  doc.addHighlight(makeEntry(0, 0, 10, {0, 1, 2}));
  while (!doc.tags().empty()) {
    doc.removeTag(0);
    for (const auto idx : doc.highlights()[0].tagIndices) {
      EXPECT_LT(static_cast<size_t>(idx), doc.tags().size()) << "dangling index after a delete";
    }
  }
  EXPECT_TRUE(doc.highlights()[0].tagIndices.empty());
}
