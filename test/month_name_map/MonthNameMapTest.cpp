#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "network/WolWeekScan.h"

namespace {

uint8_t lookup(const std::string& name) { return monthNumberFromName(name.data(), name.size()); }

}  // namespace

TEST(MonthNameMap, MapsEveryMonth) {
  EXPECT_EQ(lookup("january"), 1);
  EXPECT_EQ(lookup("february"), 2);
  EXPECT_EQ(lookup("march"), 3);
  EXPECT_EQ(lookup("april"), 4);
  EXPECT_EQ(lookup("may"), 5);
  EXPECT_EQ(lookup("june"), 6);
  EXPECT_EQ(lookup("july"), 7);
  EXPECT_EQ(lookup("august"), 8);
  EXPECT_EQ(lookup("september"), 9);
  EXPECT_EQ(lookup("october"), 10);
  EXPECT_EQ(lookup("november"), 11);
  EXPECT_EQ(lookup("december"), 12);
}

TEST(MonthNameMap, RejectsUnknownNames) {
  EXPECT_EQ(lookup("smarch"), 0);
  EXPECT_EQ(lookup(""), 0);
  EXPECT_EQ(lookup("enero"), 0);
  EXPECT_EQ(monthNumberFromName(nullptr, 0), 0);
}

// The scanner hands over an unterminated slice of the page, so the length has to
// decide the match, not a trailing NUL.
TEST(MonthNameMap, MatchesOnLengthNotTermination) {
  EXPECT_EQ(monthNumberFromName("julyXXXX", 4), 7);
  EXPECT_EQ(lookup("jul"), 0);
  EXPECT_EQ(lookup("julyish"), 0);
  EXPECT_EQ(lookup("JULY"), 0);
}
