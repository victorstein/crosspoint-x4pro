#include <gtest/gtest.h>

#include <string>

#include "activities/reader/HighlightRowText.h"

TEST(ComposeSubtitle, JoinsBothHalvesWithANewline) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("Porque fuimos salvados", "esperanza"),
            "Porque fuimos salvados\nesperanza");
}

TEST(ComposeSubtitle, NoLeadingNewlineWhenThePassageIsEmpty) {
  // layoutText preserves a blank line for a leading '\n', which would render an
  // empty first row line.
  EXPECT_EQ(HighlightRowText::composeSubtitle("", "esperanza"), "esperanza");
}

TEST(ComposeSubtitle, NoTrailingNewlineWhenThereAreNoTags) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("Porque fuimos salvados", ""),
            "Porque fuimos salvados");
}

TEST(ComposeSubtitle, BothEmptyYieldsAnEmptyString) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("", ""), "");
}

TEST(ComposeSubtitle, ATagNameCannotInjectAnExtraRowLine) {
  // A newline reaching the tag half would add a fourth line to the row.
  EXPECT_EQ(HighlightRowText::composeSubtitle("passage", "a\nb"), "passage\na b");
}

TEST(ComposeSubtitle, ControlCharactersInThePassageAreNeutralisedToo) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("one\ttwo", "tag"), "one two\ntag");
}

TEST(ComposeSubtitle, AccentedUtf8CharactersInThePassagePassThroughUnchanged) {
  EXPECT_EQ(HighlightRowText::composeSubtitle("la salvación está aquí", "fe"), "la salvación está aquí\nfe");
}
