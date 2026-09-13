#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "BibleNavScanner.h"

namespace {

struct Book {
  std::string target;   // bare, as biblebooknav.xhtml yields it
  std::string outline;  // the book's outline page, a different spine item
  std::string name;
};

// Every book appears in toc.xhtml twice: an outline entry and the book entry.
// TocNavParser stores both base-prefixed, which is the form these fixtures use
// -- a literal == join against the bare booknav hrefs matches none of them.
struct TocFixture {
  std::vector<std::string> hrefs;
  std::vector<std::string> titles;
};

TocFixture buildToc(const std::vector<Book>& books, const std::string& outlineSuffix) {
  TocFixture toc;
  for (const auto& book : books) {
    toc.hrefs.push_back("OEBPS/" + book.outline);
    toc.titles.push_back(book.name + outlineSuffix);
    toc.hrefs.push_back("OEBPS/" + book.target);
    toc.titles.push_back(book.name);
  }
  return toc;
}

std::vector<Book> englishBooks() {
  static const std::vector<std::string> kNames = {"Genesis",
                                                  "Exodus",
                                                  "Leviticus",
                                                  "Numbers",
                                                  "Deuteronomy",
                                                  "Joshua",
                                                  "Judges",
                                                  "Ruth",
                                                  "1 Samuel",
                                                  "2 Samuel",
                                                  "1 Kings",
                                                  "2 Kings",
                                                  "1 Chronicles",
                                                  "2 Chronicles",
                                                  "Ezra",
                                                  "Nehemiah",
                                                  "Esther",
                                                  "Job",
                                                  "Psalms",
                                                  "Proverbs",
                                                  "Ecclesiastes",
                                                  "Song of Solomon",
                                                  "Isaiah",
                                                  "Jeremiah",
                                                  "Lamentations",
                                                  "Ezekiel",
                                                  "Daniel",
                                                  "Hosea",
                                                  "Joel",
                                                  "Amos",
                                                  "Obadiah",
                                                  "Jonah",
                                                  "Micah",
                                                  "Nahum",
                                                  "Habakkuk",
                                                  "Zephaniah",
                                                  "Haggai",
                                                  "Zechariah",
                                                  "Malachi",
                                                  "Matthew",
                                                  "Mark",
                                                  "Luke",
                                                  "John",
                                                  "Acts",
                                                  "Romans",
                                                  "1 Corinthians",
                                                  "2 Corinthians",
                                                  "Galatians",
                                                  "Ephesians",
                                                  "Philippians",
                                                  "Colossians",
                                                  "1 Thessalonians",
                                                  "2 Thessalonians",
                                                  "1 Timothy",
                                                  "2 Timothy",
                                                  "Titus",
                                                  "Philemon",
                                                  "Hebrews",
                                                  "James",
                                                  "1 Peter",
                                                  "2 Peter",
                                                  "1 John",
                                                  "2 John",
                                                  "3 John",
                                                  "Jude",
                                                  "Revelation"};

  static const std::vector<int> kDirect = {31, 57, 63, 64, 65};
  static const std::vector<std::string> kDirectFiles = {"1001061135.xhtml", "1001061161.xhtml", "1001061167.xhtml",
                                                        "1001061168.xhtml", "1001061169.xhtml"};

  std::vector<Book> books;
  books.reserve(kNames.size());
  size_t directIndex = 0;
  for (size_t i = 0; i < kNames.size(); i++) {
    const int number = static_cast<int>(i) + 1;
    Book book;
    if (directIndex < kDirect.size() && kDirect[directIndex] == number) {
      book.target = kDirectFiles[directIndex];
      directIndex++;
    } else {
      book.target = "biblechapternav" + std::to_string(number) + ".xhtml";
    }
    book.outline = "100106" + std::to_string(1300 + number) + ".xhtml";
    book.name = kNames[i];
    books.push_back(book);
  }
  return books;
}

std::vector<Book> spanishBooks() {
  auto books = englishBooks();
  books[0].name = "Génesis";
  books[18].name = "Salmos";
  books[21].name = "El Cantar de los Cantares";
  books[30].name = "Abdías";
  books[65].name = "Revelación";
  return books;
}

std::vector<std::string> targetsOf(const std::vector<Book>& books) {
  std::vector<std::string> targets;
  targets.reserve(books.size());
  for (const auto& book : books) targets.push_back(book.target);
  return targets;
}

std::vector<std::string> join(const std::vector<Book>& books, const TocFixture& toc) {
  const auto targets = targetsOf(books);
  std::vector<std::string> names(books.size());
  BibleNav::joinBookNames(targets.data(), static_cast<int>(targets.size()), toc.hrefs.data(), toc.titles.data(),
                          static_cast<int>(toc.hrefs.size()), names.data());
  return names;
}

TEST(BibleBookJoin, EnglishTocNamesAllSixtySixBooks) {
  const auto books = englishBooks();
  const auto names = join(books, buildToc(books, " Outline"));

  ASSERT_EQ(names.size(), 66u);
  for (size_t i = 0; i < names.size(); i++) {
    EXPECT_EQ(names[i], books[i].name) << "book " << i + 1;
  }
}

TEST(BibleBookJoin, SpanishTocNamesAllSixtySixBooks) {
  const auto books = spanishBooks();
  const auto names = join(books, buildToc(books, ""));

  ASSERT_EQ(names.size(), 66u);
  for (size_t i = 0; i < names.size(); i++) {
    EXPECT_EQ(names[i], books[i].name) << "book " << i + 1;
  }
  EXPECT_EQ(names[21], "El Cantar de los Cantares");
  EXPECT_EQ(names[21].size(), 25u);
}

// The Spanish outline entry is "Contenido de <book>", which carries no shared
// suffix with the English "<book> Outline" -- the join must never depend on
// either, only on the outline pointing at a different file.
TEST(BibleBookJoin, OutlineEntryNeverWins) {
  const auto books = spanishBooks();
  TocFixture toc;
  for (const auto& book : books) {
    toc.hrefs.push_back("OEBPS/" + book.outline);
    toc.titles.push_back("Contenido de " + book.name);
    toc.hrefs.push_back("OEBPS/" + book.target);
    toc.titles.push_back(book.name);
  }

  const auto targets = targetsOf(books);
  std::vector<std::string> names(books.size());
  BibleNav::joinBookNames(targets.data(), static_cast<int>(targets.size()), toc.hrefs.data(), toc.titles.data(),
                          static_cast<int>(toc.hrefs.size()), names.data());

  for (size_t i = 0; i < names.size(); i++) {
    EXPECT_EQ(names[i], books[i].name);
    EXPECT_EQ(names[i].rfind("Contenido de ", 0), std::string::npos);
  }
}

// The five single-chapter books point at a chapter spine item rather than a
// chapter-nav page; they must still pick up a name.
TEST(BibleBookJoin, DirectBooksAreNamedToo) {
  const auto books = englishBooks();
  const auto names = join(books, buildToc(books, " Outline"));

  EXPECT_EQ(names[30], "Obadiah");
  EXPECT_EQ(names[56], "Philemon");
  EXPECT_EQ(names[62], "2 John");
  EXPECT_EQ(names[63], "3 John");
  EXPECT_EQ(names[64], "Jude");
}

TEST(BibleBookJoin, BarePrefixedComparisonWouldMatchNothing) {
  const auto books = englishBooks();
  const auto toc = buildToc(books, " Outline");
  const auto targets = targetsOf(books);

  int literalMatches = 0;
  for (const auto& href : toc.hrefs) {
    for (const auto& target : targets) {
      if (href == target) literalMatches++;
    }
  }
  EXPECT_EQ(literalMatches, 0);
  EXPECT_EQ(
      BibleNav::findTargetByHref(targets.data(), static_cast<int>(targets.size()), "OEBPS/biblechapternav19.xhtml"),
      18);
}

TEST(BibleBookJoin, UnmatchedTargetKeepsAnEmptyName) {
  const std::string targets[] = {"biblechapternav1.xhtml", "biblechapternav2.xhtml"};
  const std::string tocHrefs[] = {"OEBPS/biblechapternav2.xhtml"};
  const std::string tocTitles[] = {"Exodus"};
  std::string names[2];

  BibleNav::joinBookNames(targets, 2, tocHrefs, tocTitles, 1, names);

  EXPECT_TRUE(names[0].empty());
  EXPECT_EQ(names[1], "Exodus");
}

}  // namespace
