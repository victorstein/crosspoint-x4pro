#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "BibleChapterNumber.h"

namespace {

// Marker shape the real publications use: an empty span carrying the id, the
// verse number as a sibling.
std::string verse(const int chapter, const int verseNumber) {
  return "<span id=\"chapter" + std::to_string(chapter) + "_verse" + std::to_string(verseNumber) +
         "\"></span><strong><sup>" + std::to_string(verseNumber) + "</sup></strong> text ";
}

std::string chapterDoc(const int chapter, const std::string& lead = "") {
  return "<html><body><p>" + lead + verse(chapter, 1) + verse(chapter, 2) + "</p></body></html>";
}

// Feed the document in fixed-size pieces, stopping the moment the number
// resolves -- exactly what the reader does against the file on SD.
int scanInChunks(const std::string& doc, const size_t chunkBytes) {
  BibleChapterNumber::Reader reader;
  size_t offset = 0;
  while (offset < doc.size()) {
    const size_t take = std::min(chunkBytes, doc.size() - offset);
    const bool isFinal = offset + take == doc.size();
    if (!reader.feed(doc.data() + offset, take, isFinal)) break;
    if (reader.resolved()) break;
    offset += take;
  }
  return reader.chapter();
}

}  // namespace

TEST(BibleChapterNumber, ReadsTheChapterFromTheFirstMarker) {
  const std::string doc = chapterDoc(5);
  EXPECT_EQ(BibleChapterNumber::scan(doc.data(), doc.size()), 5);
}

TEST(BibleChapterNumber, TakesTheFirstMarkerNotTheLast) {
  // Guards the early exit: a chapter file's markers all name the same chapter,
  // but reading [0] rather than back() is what lets the scan stop early.
  const std::string doc = "<html><body><p>" + verse(5, 1) + verse(5, 2) + verse(5, 3) + "</p></body></html>";
  EXPECT_EQ(BibleChapterNumber::scan(doc.data(), doc.size()), 5);
}

TEST(BibleChapterNumber, ReportsNotFoundWhenTheDocumentHasNoMarkers) {
  // How a non-Bible spine item reads; the reader falls back to the TOC title.
  const char* doc = "<html><body><p>Chapter One</p><p>It was a dark and stormy night.</p></body></html>";
  EXPECT_EQ(BibleChapterNumber::scan(doc, strlen(doc)), -1);
}

TEST(BibleChapterNumber, ReportsNotFoundOnAMalformedDocument) {
  const char* doc = "<html><body><p><span id=\"chapter5_verse1\"></p></body>";
  EXPECT_EQ(BibleChapterNumber::scan(doc, strlen(doc)), -1);
}

TEST(BibleChapterNumber, FindsAMarkerFarIntoTheFile) {
  // A chapter that opens with a long superscription (Psalms) pushes the first
  // marker well past the early chunks.
  const std::string doc = chapterDoc(119, std::string(40000, 'x'));
  ASSERT_GT(doc.size(), 40000u);
  EXPECT_EQ(BibleChapterNumber::scan(doc.data(), doc.size()), 119);
  EXPECT_EQ(scanInChunks(doc, 4096), 119);
}

TEST(BibleChapterNumber, ResolvesAcrossEveryChunkBoundary) {
  const std::string doc = chapterDoc(5);
  for (const size_t chunkBytes : {size_t{1}, size_t{7}, size_t{4096}}) {
    EXPECT_EQ(scanInChunks(doc, chunkBytes), 5) << "chunk size " << chunkBytes;
  }
}

TEST(BibleChapterNumber, ResolvesWhenAChunkBoundarySplitsTheIdAttribute) {
  const std::string doc = chapterDoc(5);
  const size_t idPos = doc.find("chapter5_verse1");
  ASSERT_NE(idPos, std::string::npos);

  // Cut inside "chapter5_verse1" itself, so the digit and the attribute that
  // carries it arrive in separate XML_Parse calls.
  for (size_t cut = idPos; cut < idPos + strlen("chapter5_verse1"); cut++) {
    BibleChapterNumber::Reader reader;
    ASSERT_TRUE(reader.feed(doc.data(), cut, /*isFinal=*/false)) << "cut at " << cut;
    ASSERT_FALSE(reader.resolved()) << "cut at " << cut;
    ASSERT_TRUE(reader.feed(doc.data() + cut, doc.size() - cut, /*isFinal=*/true)) << "cut at " << cut;
    EXPECT_EQ(reader.chapter(), 5) << "cut at " << cut;
  }
}

TEST(BibleChapterNumber, KeepsTheFirstChapterWhenFedPastIt) {
  // The streaming caller can overshoot by one chunk before it checks
  // resolved(); the extra bytes must not move the answer.
  const std::string doc = chapterDoc(5);
  BibleChapterNumber::Reader reader;
  ASSERT_TRUE(reader.feed(doc.data(), doc.size(), /*isFinal=*/true));
  ASSERT_EQ(reader.chapter(), 5);
  EXPECT_TRUE(reader.feed("<html><body>", 12, /*isFinal=*/false));
  EXPECT_EQ(reader.chapter(), 5);
}
