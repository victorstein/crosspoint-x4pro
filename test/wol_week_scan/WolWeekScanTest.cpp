#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "network/WolWeekScan.h"

namespace {

std::string loadFixture(const char* name) {
  const std::string path = std::string(WOL_FIXTURE_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "missing fixture " << path;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

void feedInChunks(WolWeekScanner& scanner, const std::string& body, const size_t chunk) {
  for (size_t offset = 0; offset < body.size(); offset += chunk) {
    scanner.feed(body.data() + offset, std::min(chunk, body.size() - offset));
  }
}

// The chunk sizes the transport actually produces, plus the two pathological
// ones: every publication link straddles a boundary at 1 and 7 bytes.
const std::vector<size_t> kChunkSizes = {1, 7, 4096};

}  // namespace

TEST(WolWeekScan, CurrentWeekYieldsBothPublications) {
  const std::string body = loadFixture("wol_2026_37.html");
  for (const size_t chunk : kChunkSizes) {
    WolWeekScanner scanner;
    feedInChunks(scanner, body, chunk);
    EXPECT_EQ(scanner.count(), 2) << "chunk size " << chunk;
    ASSERT_TRUE(scanner.has(MeetingPub::Watchtower)) << "chunk size " << chunk;
    ASSERT_TRUE(scanner.has(MeetingPub::Workbook)) << "chunk size " << chunk;
    EXPECT_STREQ(scanner.issue(MeetingPub::Watchtower), "202607") << "chunk size " << chunk;
    EXPECT_STREQ(scanner.issue(MeetingPub::Workbook), "202609") << "chunk size " << chunk;
  }
}

// Regression for templating the device's ISO year into the scan prefix: ISO week
// 2026/01 studies publications dated 2025.
TEST(WolWeekScan, FirstWeekOfYearReferencesPreviousYear) {
  const std::string body = loadFixture("wol_2026_01.html");
  for (const size_t chunk : kChunkSizes) {
    WolWeekScanner scanner;
    feedInChunks(scanner, body, chunk);
    ASSERT_TRUE(scanner.has(MeetingPub::Watchtower)) << "chunk size " << chunk;
    ASSERT_TRUE(scanner.has(MeetingPub::Workbook)) << "chunk size " << chunk;
    EXPECT_STREQ(scanner.issue(MeetingPub::Watchtower), "202510") << "chunk size " << chunk;
    EXPECT_STREQ(scanner.issue(MeetingPub::Workbook), "202511") << "chunk size " << chunk;
  }
}

// Memorial week publishes a Watchtower link and no workbook; that is a normal
// outcome, not a failed scan.
TEST(WolWeekScan, MemorialWeekHasNoWorkbook) {
  const std::string body = loadFixture("wol_2026_14.html");
  for (const size_t chunk : kChunkSizes) {
    WolWeekScanner scanner;
    feedInChunks(scanner, body, chunk);
    EXPECT_EQ(scanner.count(), 1) << "chunk size " << chunk;
    ASSERT_TRUE(scanner.has(MeetingPub::Watchtower)) << "chunk size " << chunk;
    EXPECT_STREQ(scanner.issue(MeetingPub::Watchtower), "202601") << "chunk size " << chunk;
    EXPECT_FALSE(scanner.has(MeetingPub::Workbook)) << "chunk size " << chunk;
    EXPECT_STREQ(scanner.issue(MeetingPub::Workbook), "") << "chunk size " << chunk;
  }
}

TEST(WolWeekScan, EmptyBodyFindsNothing) {
  WolWeekScanner scanner;
  scanner.feed("", 0);
  EXPECT_EQ(scanner.count(), 0);
}

TEST(WolWeekScan, UnknownMonthIsRejected) {
  WolWeekScanner scanner;
  const std::string body = R"(<a href="/x/the-watchtower-2026/study-edition/smarch">)";
  scanner.feed(body.data(), body.size());
  EXPECT_FALSE(scanner.has(MeetingPub::Watchtower));
}

TEST(WolWeekScan, NonNumericYearIsRejected) {
  WolWeekScanner scanner;
  const std::string body = R"(<a href="/x/the-watchtower-simplified/study-edition/july">)";
  scanner.feed(body.data(), body.size());
  EXPECT_FALSE(scanner.has(MeetingPub::Watchtower));
}

TEST(WolWeekScan, FirstOccurrenceWins) {
  WolWeekScanner scanner;
  const std::string body = R"("the-watchtower-2026/study-edition/july" "the-watchtower-2027/study-edition/march")";
  scanner.feed(body.data(), body.size());
  ASSERT_TRUE(scanner.has(MeetingPub::Watchtower));
  EXPECT_STREQ(scanner.issue(MeetingPub::Watchtower), "202607");
}

TEST(WolWeekScan, ResetClearsPriorMatches) {
  const std::string body = loadFixture("wol_2026_37.html");
  WolWeekScanner scanner;
  scanner.feed(body.data(), body.size());
  ASSERT_EQ(scanner.count(), 2);
  scanner.reset();
  EXPECT_EQ(scanner.count(), 0);
  EXPECT_STREQ(scanner.issue(MeetingPub::Watchtower), "");
}

TEST(IsoWeek, MatchesKnownDates) {
  IsoWeek week;
  ASSERT_TRUE(isoWeekFromUtcDate(2026, 9, 12, week));
  EXPECT_EQ(week.year, 2026);
  EXPECT_EQ(week.week, 37);

  // 2026-01-01 is a Thursday, so it belongs to ISO week 2026/01.
  ASSERT_TRUE(isoWeekFromUtcDate(2026, 1, 1, week));
  EXPECT_EQ(week.year, 2026);
  EXPECT_EQ(week.week, 1);

  // 2027-01-01 is a Friday, so it falls in the last week of ISO year 2026.
  ASSERT_TRUE(isoWeekFromUtcDate(2027, 1, 1, week));
  EXPECT_EQ(week.year, 2026);
  EXPECT_EQ(week.week, 53);
}

TEST(IsoWeek, RejectsImpossibleDates) {
  IsoWeek week;
  EXPECT_FALSE(isoWeekFromUtcDate(2026, 0, 12, week));
  EXPECT_FALSE(isoWeekFromUtcDate(2026, 13, 12, week));
  EXPECT_FALSE(isoWeekFromUtcDate(2026, 9, 0, week));
  EXPECT_FALSE(isoWeekFromUtcDate(1970 - 1, 9, 12, week));
}

TEST(MeetingUrls, PadTheWeekAndCarryTheIssue) {
  EXPECT_EQ(meetingsPageUrl(IsoWeek{2026, 1}), "https://wol.jw.org/en/wol/meetings/r1/lp-e/2026/01");
  EXPECT_EQ(meetingsPageUrl(IsoWeek{2026, 37}), "https://wol.jw.org/en/wol/meetings/r1/lp-e/2026/37");

  EXPECT_EQ(pubMediaUrl(MeetingPub::Watchtower, "202607", "S"),
            "https://b.jw-cdn.org/apis/pub-media/"
            "GETPUBMEDIALINKS?output=json&pub=w&langwritten=S&fileformat=EPUB&issue=202607");
  EXPECT_EQ(pubMediaUrl(MeetingPub::Workbook, "202609", "S"),
            "https://b.jw-cdn.org/apis/pub-media/"
            "GETPUBMEDIALINKS?output=json&pub=mwb&langwritten=S&fileformat=EPUB&issue=202609");
}

TEST(MeetingUrls, FilenameComesFromTheLastPathSegment) {
  EXPECT_EQ(filenameFromUrl("https://cfp2.jw-cdn.org/a/717b307/1/o/w_S_202607.epub"), "w_S_202607.epub");
  EXPECT_EQ(filenameFromUrl("mwb_S_202609.epub"), "mwb_S_202609.epub");
  EXPECT_EQ(filenameFromUrl("https://example.com/"), "");
  EXPECT_EQ(filenameFromUrl(""), "");
}
