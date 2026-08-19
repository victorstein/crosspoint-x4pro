#include <gtest/gtest.h>

#include "Epub/VisibleRange.h"

TEST(VisibleRange, ContainsIsHalfOpen) {
  constexpr VisibleRange r{100, 200};
  EXPECT_TRUE(r.contains(100)) << "start is inclusive";
  EXPECT_TRUE(r.contains(199));
  EXPECT_FALSE(r.contains(200)) << "end is exclusive";
  EXPECT_FALSE(r.contains(99));
}

TEST(VisibleRange, AdjacentRangesDoNotShareAWord) {
  constexpr VisibleRange left{100, 200};
  constexpr VisibleRange right{200, 300};
  EXPECT_FALSE(left.contains(200));
  EXPECT_TRUE(right.contains(200));
  EXPECT_FALSE(left.overlaps(right)) << "touching at a boundary is not overlapping";
}

TEST(VisibleRange, EmptyRangeContainsNothing) {
  constexpr VisibleRange empty{150, 150};
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_FALSE(empty.contains(150));
  EXPECT_FALSE(empty.overlaps(VisibleRange{100, 200}));
}

TEST(VisibleRange, InvertedRangeIsTreatedAsEmpty) {
  constexpr VisibleRange inverted{300, 100};
  EXPECT_TRUE(inverted.isEmpty());
  EXPECT_FALSE(inverted.contains(200)) << "a reversed range must not match the span between its ends";
}

TEST(VisibleRange, DetectsGenuineOverlap) {
  constexpr VisibleRange a{100, 200};
  EXPECT_TRUE(a.overlaps(VisibleRange{150, 250}));
  EXPECT_TRUE(a.overlaps(VisibleRange{50, 150}));
  EXPECT_TRUE(a.overlaps(VisibleRange{120, 130})) << "fully contained";
  EXPECT_TRUE(a.overlaps(VisibleRange{50, 250})) << "fully containing";
  EXPECT_FALSE(a.overlaps(VisibleRange{200, 300}));
}

TEST(VisibleRange, HandlesOffsetsBeyond16Bits) {
  // Absolute spine-body offsets routinely exceed 65535 in a long chapter, which
  // is why the stored form is 32-bit.
  constexpr VisibleRange r{70000, 70010};
  EXPECT_TRUE(r.contains(70005));
  EXPECT_FALSE(r.contains(65535));
}
