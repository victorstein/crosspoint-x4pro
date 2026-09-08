#include <gtest/gtest.h>

#include "Serialization/DocReadStatus.h"

TEST(DocReadStatus, AbsentFileIsMissing) {
  EXPECT_EQ(classifyDocRead(false, false, false), DocReadStatus::Missing);
  EXPECT_EQ(classifyDocRead(false, true, true), DocReadStatus::Missing)
      << "absence dominates: nothing else was observed";
}

TEST(DocReadStatus, EmptyContentIsUnreadable) {
  EXPECT_EQ(classifyDocRead(true, true, false), DocReadStatus::Unreadable);
}

TEST(DocReadStatus, ParseFailureIsReported) {
  EXPECT_EQ(classifyDocRead(true, false, true), DocReadStatus::ParseError);
}

TEST(DocReadStatus, GoodReadIsOk) { EXPECT_EQ(classifyDocRead(true, false, false), DocReadStatus::Ok); }

TEST(DocReadStatus, OnlyMissingIsSafeToOverwrite) {
  // The rule the caller depends on: exactly one failure status means "no data
  // was ever there". The other two mean data may exist and must be preserved.
  EXPECT_EQ(classifyDocRead(false, false, false), DocReadStatus::Missing);
  EXPECT_NE(classifyDocRead(true, true, false), DocReadStatus::Missing);
  EXPECT_NE(classifyDocRead(true, false, true), DocReadStatus::Missing);
}
