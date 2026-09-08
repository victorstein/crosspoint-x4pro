#include <gtest/gtest.h>

#include "VisibleOffsetCounter.h"

TEST(VisibleOffsetCounter, OpensTheGateCaseInsensitively) {
  VisibleOffsetCounter c;
  c.onStartElement("BODY");
  EXPECT_TRUE(c.insideBody) << "the parser uses strcasecmp on open (:397)";
}

TEST(VisibleOffsetCounter, ClosesTheGateCaseSensitively) {
  // Deliberate asymmetry, reproduced from ChapterHtmlSlimParser (strcasecmp at
  // :397, strcmp at :1542). Making these agree would change the offsets baked
  // into every cached section, so this test exists to stop a well-meaning tidy-up.
  VisibleOffsetCounter c;
  c.onStartElement("BODY");
  c.onEndElement("BODY");
  EXPECT_TRUE(c.insideBody) << "</BODY> does not close the gate in the real parser";
  c.onEndElement("body");
  EXPECT_FALSE(c.insideBody);
}

TEST(VisibleOffsetCounter, SkipsNonVisibleElementsRegardlessOfCase) {
  VisibleOffsetCounter c;
  c.onStartElement("body");
  c.onStartElement("TITLE");
  c.onCharacterData("skipme", 6);
  EXPECT_EQ(c.offset, 0u) << "isNonVisibleElement is case-insensitive";
  c.onEndElement("TITLE");
  c.onCharacterData("abc", 3);
  EXPECT_EQ(c.offset, 3u) << "the gate must reopen once the subtree closes";
}

TEST(VisibleOffsetCounter, DoesNotStripNamespacePrefixes) {
  VisibleOffsetCounter c;
  c.onStartElement("h:body");
  EXPECT_FALSE(c.insideBody) << "the parser matches the full name; h:body is not body";
}

TEST(VisibleOffsetCounter, CountsCodepointsNotBytes) {
  VisibleOffsetCounter c;
  c.onStartElement("body");
  c.onCharacterData("\xc3\xa9\xc3\xa9", 4);
  EXPECT_EQ(c.offset, 2u) << "two 2-byte codepoints advance by 2, not 4";
}

TEST(VisibleOffsetCounter, NestedNonVisibleSubtreesUnwindSymmetrically) {
  VisibleOffsetCounter c;
  c.onStartElement("body");
  c.onStartElement("head");
  c.onStartElement("span");  // every start inside a non-visible subtree increments
  c.onEndElement("span");
  c.onCharacterData("nope", 4);
  EXPECT_EQ(c.offset, 0u) << "still inside <head>";
  c.onEndElement("head");
  c.onCharacterData("abc", 3);
  EXPECT_EQ(c.offset, 3u);
}

TEST(VisibleOffsetCounter, CountsNothingBeforeBodyOpens) {
  VisibleOffsetCounter c;
  c.onCharacterData("preamble", 8);
  EXPECT_EQ(c.offset, 0u);
}
