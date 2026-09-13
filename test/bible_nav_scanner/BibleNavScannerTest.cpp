#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BibleNavScanner.h"

namespace {

// The five books with no chapter-nav page of their own; biblebooknav.xhtml
// points each straight at its single chapter's spine item.
const std::vector<int> kDirectBookNumbers = {31, 57, 63, 64, 65};
const std::vector<std::string> kDirectBookFiles = {"1001061135.xhtml", "1001061161.xhtml", "1001061167.xhtml",
                                                   "1001061168.xhtml", "1001061169.xhtml"};

// Mirrors the real page: a <link> to css/epubs.css in the head that the scan
// must not pick up, then 66 <a> rows in canonical order.
std::string bookNavFixture() {
  std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
      "<head><title>Bible Books</title>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"css/epubs.css\"/></head>\n"
      "<body><nav epub:type=\"toc\"><ol>\n";
  size_t directIndex = 0;
  for (int book = 1; book <= 66; book++) {
    std::string href;
    if (directIndex < kDirectBookNumbers.size() && kDirectBookNumbers[directIndex] == book) {
      href = kDirectBookFiles[directIndex];
      directIndex++;
    } else {
      href = "biblechapternav" + std::to_string(book) + ".xhtml";
    }
    xhtml += "<li><a href=\"" + href + "\">Book " + std::to_string(book) + "</a></li>\n";
  }
  xhtml += "</ol></nav></body></html>\n";
  return xhtml;
}

// The first <a> is always the back-link to biblebooknav.xhtml.
std::string chapterNavFixture(const int book, const int chapters, const int firstSpineFile) {
  std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
      "<head><link rel=\"stylesheet\" type=\"text/css\" href=\"css/epubs.css\"/></head>\n"
      "<body><a href=\"biblebooknav.xhtml\">Book " +
      std::to_string(book) + "</a><ol>\n";
  for (int c = 0; c < chapters; c++) {
    xhtml +=
        "<li><a href=\"" + std::to_string(firstSpineFile + c) + ".xhtml\">" + std::to_string(c + 1) + "</a></li>\n";
  }
  xhtml += "</ol></body></html>\n";
  return xhtml;
}

std::vector<std::string> scanInChunks(const std::string& xhtml, const size_t chunkBytes) {
  BibleNav::Scanner scanner;
  EXPECT_TRUE(scanner.valid());
  for (size_t offset = 0; offset < xhtml.size(); offset += chunkBytes) {
    const size_t length = std::min(chunkBytes, xhtml.size() - offset);
    const bool isFinal = offset + length >= xhtml.size();
    if (!scanner.feed(xhtml.data() + offset, length, isFinal)) return {};
  }
  return scanner.take();
}

TEST(BibleNavScanner, BookNavYieldsSixtySixLinksAndSkipsStylesheet) {
  const std::string xhtml = bookNavFixture();
  const auto links = BibleNav::scan(xhtml.data(), xhtml.size());
  ASSERT_EQ(links.size(), 66u);
  for (const auto& link : links) {
    EXPECT_NE(link, "epubs.css");
  }
  EXPECT_EQ(links.front(), "biblechapternav1.xhtml");
  EXPECT_EQ(links.back(), "biblechapternav66.xhtml");
}

TEST(BibleNavScanner, FiveBooksWithoutAChapterNavPageAreClassifiedDirect) {
  const std::string xhtml = bookNavFixture();
  const auto links = BibleNav::scan(xhtml.data(), xhtml.size());
  ASSERT_EQ(links.size(), 66u);

  std::vector<int> direct;
  for (size_t i = 0; i < links.size(); i++) {
    if (!BibleNav::isChapterNav(links[i])) direct.push_back(static_cast<int>(i) + 1);
  }
  EXPECT_EQ(direct, kDirectBookNumbers);
  for (size_t i = 0; i < kDirectBookNumbers.size(); i++) {
    EXPECT_EQ(links[kDirectBookNumbers[i] - 1], kDirectBookFiles[i]);
  }
}

TEST(BibleNavScanner, ChapterNavDropsTheBackLinkToBookNav) {
  const std::string xhtml = chapterNavFixture(19, 150, 1001061000);
  auto links = BibleNav::scan(xhtml.data(), xhtml.size());
  ASSERT_EQ(links.size(), 151u);
  EXPECT_EQ(links.front(), "biblebooknav.xhtml");

  BibleNav::dropBookNavLinks(links);
  ASSERT_EQ(links.size(), 150u);
  EXPECT_EQ(links.front(), "1001061000.xhtml");
  EXPECT_EQ(links.back(), "1001061149.xhtml");
}

TEST(BibleNavScanner, ChunkBoundariesDoNotChangeTheResult) {
  const std::string book = bookNavFixture();
  const auto whole = BibleNav::scan(book.data(), book.size());
  for (const size_t chunkBytes : {size_t{1}, size_t{7}, size_t{4096}}) {
    EXPECT_EQ(scanInChunks(book, chunkBytes), whole) << "chunk size " << chunkBytes;
  }

  const std::string chapters = chapterNavFixture(1, 50, 1001061300);
  const auto wholeChapters = BibleNav::scan(chapters.data(), chapters.size());
  ASSERT_EQ(wholeChapters.size(), 51u);
  for (const size_t chunkBytes : {size_t{1}, size_t{7}, size_t{4096}}) {
    EXPECT_EQ(scanInChunks(chapters, chunkBytes), wholeChapters) << "chunk size " << chunkBytes;
  }
}

TEST(BibleNavScanner, MalformedDocumentYieldsNothing) {
  const std::string xhtml = "<html><body><a href=\"biblechapternav1.xhtml\">Genesis</body></html>";
  EXPECT_TRUE(BibleNav::scan(xhtml.data(), xhtml.size()).empty());
}

TEST(BibleNavScanner, FilenameTailStripsDirectoriesAndFragments) {
  EXPECT_EQ(BibleNav::filenameTail("OEBPS/biblechapternav1.xhtml"), "biblechapternav1.xhtml");
  EXPECT_EQ(BibleNav::filenameTail("biblechapternav1.xhtml"), "biblechapternav1.xhtml");

  const std::string xhtml = "<html><body><a href=\"OEBPS/1001061300.xhtml#chapter1_verse1\">1</a></body></html>";
  const auto links = BibleNav::scan(xhtml.data(), xhtml.size());
  ASSERT_EQ(links.size(), 1u);
  EXPECT_EQ(links.front(), "1001061300.xhtml");
}

}  // namespace
