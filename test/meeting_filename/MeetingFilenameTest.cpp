#include <gtest/gtest.h>

#include <string>

#include "network/MeetingFilename.h"

namespace {

constexpr const char* kWatchtowerUrl = "https://cfp2.jw-cdn.org/a/717b307/1/o/w_S_202607.epub";
constexpr const char* kWorkbookUrl = "https://cfp2.jw-cdn.org/a/24f3ed/1/o/mwb_S_202609.epub";

constexpr const char* kWorkbookName = "Guía de actividades para la reunión Vida y Ministerio Cristianos";

// The 100-byte cap sanitizeFilename applies, less the " YYYY-MM" appended after it.
constexpr size_t kStemBudget = 92;

bool isValidUtf8(const std::string& text) {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    size_t length = 0;
    if (lead < 0x80) {
      length = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
    } else {
      return false;
    }
    if (i + length > text.size()) return false;
    for (size_t c = 1; c < length; ++c) {
      if ((static_cast<unsigned char>(text[i + c]) & 0xC0) != 0x80) return false;
    }
    i += length;
  }
  return true;
}

std::string repeated(const std::string& unit, const size_t times) {
  std::string out;
  out.reserve(unit.size() * times);
  for (size_t i = 0; i < times; ++i) out += unit;
  return out;
}

}  // namespace

TEST(MeetingFilename, UsesThePublicationNameAndIssueCode) {
  EXPECT_EQ(meetingPublicationFilename("La Atalaya (ed. estudio)", "202607", kWatchtowerUrl),
            "La Atalaya (ed. estudio) 2026-07.epub");
}

TEST(MeetingFilename, KeepsAccentsAndFitsTheBudget) {
  const std::string filename = meetingPublicationFilename(kWorkbookName, "202609", kWorkbookUrl);
  EXPECT_EQ(filename, std::string(kWorkbookName) + " 2026-09.epub");
  EXPECT_NE(filename.find("Guía"), std::string::npos);
  EXPECT_NE(filename.find("reunión"), std::string::npos);
  EXPECT_EQ(filename.size(), 79u);
}

TEST(MeetingFilename, ReplacesPathSeparatorsAndColons) {
  EXPECT_EQ(meetingPublicationFilename("Awake!: 1/2 edition", "202512", kWatchtowerUrl),
            "Awake!_ 1_2 edition 2025-12.epub");
}

// December must not read as month 2 of a 5-digit year.
TEST(MeetingFilename, FormatsADecemberIssue) {
  EXPECT_EQ(meetingPublicationFilename("The Watchtower", "202512", kWatchtowerUrl), "The Watchtower 2025-12.epub");
}

TEST(MeetingFilename, FallsBackToTheCdnNameWhenThePubNameIsAbsent) {
  EXPECT_EQ(meetingPublicationFilename(nullptr, "202607", kWatchtowerUrl), "w_S_202607.epub");
  EXPECT_EQ(meetingPublicationFilename("", "202607", kWatchtowerUrl), "w_S_202607.epub");
}

// sanitizeFilename never returns empty — it falls back to the literal "book" —
// so a whitespace-only name sanitised first would give the Watchtower and the
// workbook the same "book <YYYY-MM>.epub" in a single run.
TEST(MeetingFilename, NeverProducesTheSanitiserBookFallback) {
  for (const char* blank : {"   ", "...", " . . ", "\t\n"}) {
    const std::string watchtower = meetingPublicationFilename(blank, "202607", kWatchtowerUrl);
    const std::string workbook = meetingPublicationFilename(blank, "202609", kWorkbookUrl);
    EXPECT_EQ(watchtower, "w_S_202607.epub") << "blank name " << blank;
    EXPECT_EQ(workbook, "mwb_S_202609.epub") << "blank name " << blank;
    EXPECT_EQ(watchtower.find("book"), std::string::npos) << "blank name " << blank;
    EXPECT_NE(watchtower, workbook) << "blank name " << blank;
  }
}

TEST(MeetingFilename, FallsBackWhenTheIssueIsMalformed) {
  for (const char* issue : {"", "2026", "2026070", "20260a", "2026 7"}) {
    EXPECT_EQ(meetingPublicationFilename("The Watchtower", issue, kWatchtowerUrl), "w_S_202607.epub") << issue;
  }
  EXPECT_EQ(meetingPublicationFilename("The Watchtower", nullptr, kWatchtowerUrl), "w_S_202607.epub");
}

TEST(MeetingFilename, TruncatesOnACodepointBoundary) {
  // 3-byte codepoints against a 92-byte budget: a byte-wise cut lands two bytes
  // into the 31st character.
  const std::string longName = repeated("\xE1\x9E\x80", 60);
  const std::string filename = meetingPublicationFilename(longName.c_str(), "202609", kWorkbookUrl);

  EXPECT_TRUE(isValidUtf8(filename)) << filename;
  EXPECT_EQ(filename, repeated("\xE1\x9E\x80", kStemBudget / 3) + " 2026-09.epub");
}

TEST(MeetingFilename, KeepsTheIssueCodeWhenTheNameIsTruncated) {
  const std::string longName = repeated("\xE1\x9E\x80", 60);
  const std::string filename = meetingPublicationFilename(longName.c_str(), "202611", kWorkbookUrl);

  EXPECT_EQ(filename.size(), kStemBudget / 3 * 3 + 13u);
  EXPECT_EQ(filename.substr(filename.size() - 13), " 2026-11.epub");
}

// Appending the code after sanitising is what keeps this apart: truncating
// "<name> <code>" as a whole gives two issues one byte-identical filename.
TEST(MeetingFilename, TwoIssuesOfAnOverlongNameNeverCollide) {
  const std::string longName = repeated("\xE1\x9E\x80", 60);
  EXPECT_NE(meetingPublicationFilename(longName.c_str(), "202609", kWorkbookUrl),
            meetingPublicationFilename(longName.c_str(), "202611", kWorkbookUrl));
}

TEST(MeetingFilename, StaysWithinTheHundredByteCap) {
  const std::string longName = repeated("a", 300);
  const std::string filename = meetingPublicationFilename(longName.c_str(), "202607", kWatchtowerUrl);
  EXPECT_EQ(filename, repeated("a", kStemBudget) + " 2026-07.epub");
  EXPECT_LE(filename.size() - 5, 100u);
}
