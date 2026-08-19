#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

TEST(Utf8SafeSummary, CollapsesWhitespaceAndTrims) {
  EXPECT_EQ(utf8SafeSummary("  the   spirit  of  Jehovah "), "the spirit of Jehovah");
}

TEST(Utf8SafeSummary, StripsNewlines) {
  EXPECT_EQ(utf8SafeSummary("first\nsecond"), "firstsecond");
}

TEST(Utf8SafeSummary, LeavesShortAsciiUnchanged) {
  EXPECT_EQ(utf8SafeSummary("short"), "short");
}

TEST(Utf8SafeSummary, NeverSplitsAMixedWidthSequence) {
  // 1 ASCII + 24x U+65E5 (3 bytes each) = 73 bytes. A naive resize(72) lands two
  // bytes into the 24th character; the only correct cut is at 70.
  //
  // Mixed width is essential: a homogeneous 2-, 3- or 4-byte string is invisible
  // to this bug, because 72 is divisible by 2, 3 and 4 alike.
  std::string mixed = "x";
  for (int i = 0; i < 24; ++i) mixed += "\xe6\x97\xa5";
  ASSERT_EQ(mixed.size(), 73u);

  std::string expected = "x";
  for (int i = 0; i < 23; ++i) expected += "\xe6\x97\xa5";
  ASSERT_EQ(expected.size(), 70u);

  EXPECT_EQ(utf8SafeSummary(mixed), expected);
}

TEST(Utf8SafeSummary, DoesNotOverReadAShortBuffer) {
  // Regression guard: an implementation that passes the 72-byte budget straight
  // to utf8SafeTruncateBuffer reads far past the end of a short string.
  EXPECT_EQ(utf8SafeSummary("ab"), "ab");
  EXPECT_EQ(utf8SafeSummary(""), "");
}

TEST(Utf8SafeSummary, CapsAtSeventyTwoBytes) {
  EXPECT_EQ(utf8SafeSummary(std::string(200, 'x')).size(), 72u);
}
