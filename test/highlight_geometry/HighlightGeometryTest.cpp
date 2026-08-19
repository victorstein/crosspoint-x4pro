#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "Epub/HighlightGeometry.h"

namespace {

// Three words on one row, then two on the next. Widths derived from the next
// word's x, as the real walk does.
std::vector<HighlightWord> sampleWords() {
  return {
      {100, 0, 0, 50, 40},    // offset 100, x 0,   w 50 -> right edge 50
      {110, 60, 0, 40, 40},   // offset 110, x 60,  w 40 -> right edge 100
      {120, 110, 0, 30, 40},  // offset 120, x 110, w 30
      {130, 0, 40, 45, 40},   // next row
      {140, 55, 40, 35, 40},
  };
}

constexpr int16_t kGap = 12;  // merge tolerance wider than the 10px inter-word gap

}  // namespace

TEST(HighlightGeometry, EmptyRangeCoversNothing) {
  EXPECT_TRUE(highlightRects(sampleWords(), {VisibleRange{120, 120}}, kGap).empty());
}

TEST(HighlightGeometry, ASingleWordYieldsOneRect) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{110, 111}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 60);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[0].w, 40);
  EXPECT_EQ(rects[0].h, 40);
}

TEST(HighlightGeometry, AdjacentWordsOnOneRowMergeIntoOneRect) {
  // Words at 100 and 110 occupy x 0..50 and 60..100 — a 10px gap. Merging makes
  // the gap invert too, so the highlight reads as one block, not striped text.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].w, 100);
}

TEST(HighlightGeometry, AGapWiderThanTheToleranceDoesNotMerge) {
  // The same two words with a tolerance below the 10px gap must stay separate.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}}, 4);
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].w, 50);
  EXPECT_EQ(rects[1].x, 60);
}

TEST(HighlightGeometry, ARangeSpanningTwoRowsYieldsARectPerRow) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{110, 140}}, kGap);
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[1].y, 40) << "rows never merge — they are not contiguous in y";
}

TEST(HighlightGeometry, EndIsExclusive) {
  // 100..120 takes the words at 100 and 110; the word at exactly 120 is excluded,
  // so the rect stops at x=100 rather than extending to 140.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x + rects[0].w, 100) << "the word at offset 120 must not be covered";
}

TEST(HighlightGeometry, WordsOutsideEveryRangeAreIgnored) {
  EXPECT_TRUE(highlightRects(sampleWords(), {VisibleRange{500, 600}}, kGap).empty());
}

TEST(HighlightGeometry, MultipleRangesEachContribute) {
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 105}, VisibleRange{140, 145}}, kGap);
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[1].y, 40);
}

TEST(HighlightGeometry, OverlappingRangesDoNotDoubleCoverAWord) {
  // Inverting the same pixels twice restores them, so a word covered by two
  // ranges would render UN-highlighted. Both ranges cover offsets 100 and 110,
  // which merge to exactly one rect.
  const auto rects = highlightRects(sampleWords(), {VisibleRange{100, 120}, VisibleRange{105, 125}}, kGap);
  ASSERT_EQ(rects.size(), 1u) << "the two ranges together cover 100,110,120 on one row";
  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].x + rects[0].w, 140) << "including the word at 120, which the second range reaches";
}

TEST(HighlightGeometry, VisualOrderInputStillProducesCorrectRects) {
  // TextBlock word order is VISUAL, not logical — on an RTL line the offsets run
  // backwards. The geometry must not assume ascending offsets.
  auto words = sampleWords();
  std::reverse(words.begin(), words.end());
  const auto rects = highlightRects(words, {VisibleRange{110, 111}}, kGap);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0].x, 60);
}
