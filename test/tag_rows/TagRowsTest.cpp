#include <gtest/gtest.h>

#include "activities/reader/TagRowMapping.h"

// The bug an adversarial review found: the row offset applied in some consumers
// and not others, so a held Confirm on "Done" entered the delete path and the
// last tag became undeletable. These pin the arithmetic.

TEST(TagRows, DoneRowIsNeverATag) {
  EXPECT_EQ(TagRows::tagIndexForRow(TagRows::DONE, 5), -1);
  EXPECT_EQ(TagRows::tagIndexForRow(0, 0), -1) << "Done is not a tag even with an empty palette";
}

TEST(TagRows, NewTagRowIsNeverATag) {
  EXPECT_EQ(TagRows::tagIndexForRow(TagRows::newTagRow(5), 5), -1);
  EXPECT_EQ(TagRows::tagIndexForRow(TagRows::newTagRow(0), 0), -1);
}

TEST(TagRows, TagRowsMapOntoThePaletteOffsetByTheDoneRow) {
  EXPECT_EQ(TagRows::tagIndexForRow(1, 5), 0) << "the first tag sits below Done";
  EXPECT_EQ(TagRows::tagIndexForRow(5, 5), 4) << "the LAST tag must be reachable";
}

TEST(TagRows, RowAndTagConversionsAreInverse) {
  for (int tagCount = 1; tagCount <= 100; tagCount++) {
    for (int tag = 0; tag < tagCount; tag++) {
      EXPECT_EQ(TagRows::tagIndexForRow(TagRows::rowForTagIndex(tag), tagCount), tag)
          << "tagCount " << tagCount << ", tag " << tag;
    }
  }
}

TEST(TagRows, EveryRowInRangeIsExactlyOneOfDoneTagOrNewTag) {
  constexpr int tagCount = 44;  // the real imported palette size
  int tags = 0;
  int nonTags = 0;
  for (int row = 0; row < TagRows::rowCount(tagCount); row++) {
    if (TagRows::tagIndexForRow(row, tagCount) >= 0) {
      tags++;
    } else {
      nonTags++;
    }
  }
  EXPECT_EQ(tags, tagCount);
  EXPECT_EQ(nonTags, 2) << "exactly Done and New tag...";
}

TEST(TagRows, OutOfRangeRowsAreRejectedRatherThanWrappingIntoThePalette) {
  EXPECT_EQ(TagRows::tagIndexForRow(-1, 5), -1);
  EXPECT_EQ(TagRows::tagIndexForRow(99, 5), -1);
}

TEST(TagRows, RowCountLeavesRoomForDoneAndNewTag) {
  EXPECT_EQ(TagRows::rowCount(0), 2);
  EXPECT_EQ(TagRows::rowCount(100), 102);
}
