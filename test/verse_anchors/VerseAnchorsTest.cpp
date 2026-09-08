#include <gtest/gtest.h>

#include <cstring>

#include "VerseAnchors.h"

namespace {
// Mirrors the shape real books use: empty marker spans, the verse number as a
// sibling, two verses sharing one paragraph.
const char* kDoc =
    "<html><head><title>skipme</title></head><body>"
    "<p id=\"p1\">"
    "<span id=\"chapter11_verse18\"></span><strong><sup>18</sup></strong> abcde"
    "<span id=\"chapter11_verse19\"></span><strong><sup>19</sup></strong> fghij"
    "</p></body></html>";
}  // namespace

TEST(VerseAnchorsScan, FindsEachMarkerInAscendingOffsetOrder) {
  const auto a = VerseAnchors::scan(kDoc, strlen(kDoc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[0].chapter, 11);
  EXPECT_EQ(a[0].verse, 18);
  EXPECT_EQ(a[1].verse, 19);
  EXPECT_LT(a[0].offset, a[1].offset);
}

TEST(VerseAnchorsScan, DoesNotCountTextOutsideBody) {
  const auto a = VerseAnchors::scan(kDoc, strlen(kDoc));
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a[0].offset, 0u) << "<title> sits in <head> and must not advance the count";
}

TEST(VerseAnchorsScan, CountsAKnownEntityAsExactlyOneCodepoint) {
  // The v1 defect: with no default handler this counted &nbsp; as nothing,
  // which is enough to name the previous verse. The DOCTYPE is load-bearing --
  // under XML_GE=0 expat only routes an undeclared entity to the default
  // handler once the document declares an external subset, and real XHTML
  // EPUB chapters do.
  const char* doc =
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"xhtml11.dtd\">"
      "<html><body><p><span id=\"chapter1_verse1\"></span>ab&nbsp;cd"
      "<span id=\"chapter1_verse2\"></span>x</p></body></html>";
  const auto a = VerseAnchors::scan(doc, strlen(doc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[1].offset, 5u) << "&nbsp; must advance the offset by exactly one";
}

TEST(VerseAnchorsScan, DegradesToEmptyWhenAnEntityCannotBeResolved) {
  // No DOCTYPE means an undeclared entity is a hard parse error. Returning
  // nothing costs the label; returning a partial list would misplace it.
  const char* doc =
      "<html><body><p><span id=\"chapter1_verse1\"></span>ab&nbsp;cd</p></body></html>";
  EXPECT_TRUE(VerseAnchors::scan(doc, strlen(doc)).empty());
}

TEST(VerseAnchorsScan, SkipsUppercaseNonVisibleElementsInsideBody) {
  const char* doc =
      "<html><body><TITLE>skipme</TITLE><span id=\"chapter1_verse1\"></span>abc</body></html>";
  const auto a = VerseAnchors::scan(doc, strlen(doc));
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].offset, 0u) << "the non-visible test is case-insensitive";
}

TEST(VerseAnchorsScan, CountsCodepointsNotBytes) {
  const char* doc =
      "<html><body><p><span id=\"chapter1_verse1\"></span>\xc3\xa9\xc3\xa9"
      "<span id=\"chapter1_verse2\"></span>x</p></body></html>";
  const auto a = VerseAnchors::scan(doc, strlen(doc));
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[1].offset, 2u) << "two 2-byte codepoints advance by 2, not 4";
}

TEST(VerseAnchorsScan, ReturnsNothingWhenTheDocumentIsMalformed) {
  const char* doc = "<html><body><span id=\"chapter1_verse1\"></span>abc</p></body>";
  EXPECT_TRUE(VerseAnchors::scan(doc, strlen(doc)).empty())
      << "a truncated list would resolve later highlights to a stale anchor";
}

TEST(VerseAnchorsScan, IgnoresIdsThatAreNotVerseMarkers) {
  const char* doc =
      "<html><body><p id=\"p188\"><span id=\"pos107452\"></span>"
      "<span id=\"footnotesource14\"></span><span id=\"chapter1_verse2x\"></span>abc</p></body></html>";
  EXPECT_TRUE(VerseAnchors::scan(doc, strlen(doc)).empty());
}

TEST(VerseAnchorsFind, ReturnsTheAnchorCoveringAnOffset) {
  const std::vector<VerseAnchors::VerseAnchor> anchors{{5, 1, 1}, {20, 1, 2}};
  EXPECT_EQ(VerseAnchors::find(anchors, 5)->verse, 1) << "exactly on a marker is inside it";
  EXPECT_EQ(VerseAnchors::find(anchors, 19)->verse, 1);
  EXPECT_EQ(VerseAnchors::find(anchors, 20)->verse, 2);
}

TEST(VerseAnchorsFind, ReturnsNullBeforeTheFirstAnchorAndForEmptyInput) {
  const std::vector<VerseAnchors::VerseAnchor> anchors{{5, 1, 1}};
  EXPECT_EQ(VerseAnchors::find(anchors, 4), nullptr);
  EXPECT_EQ(VerseAnchors::find({}, 0), nullptr);
}

TEST(VerseAnchorsFormat, RendersChapterColonVerseAndEmptyForNull) {
  const VerseAnchors::VerseAnchor a{0, 11, 19};
  EXPECT_EQ(VerseAnchors::format(&a), "11:19");
  EXPECT_EQ(VerseAnchors::format(nullptr), "");
}
