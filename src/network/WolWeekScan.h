#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Week -> issue resolution for the weekly meeting publications, as published on
// wol.jw.org. Pure: no Arduino, no I/O, so the host suite exercises it directly.

// The two publications a meeting week can reference. Either may be absent: the
// Memorial week carries a Watchtower link and no workbook, and that recurs
// annually.
enum class MeetingPub : uint8_t {
  Watchtower,
  Workbook,
};

struct IsoWeek {
  uint16_t year = 0;
  uint8_t week = 0;
};

// 1-12 for a lowercase English month name, 0 for anything else.
uint8_t monthNumberFromName(const char* name, size_t len);

// ISO-8601 week-based year and week number for a UTC calendar date. Goes through
// gmtime_r so strftime's %G/%V see the tm_wday/tm_yday they read.
bool isoWeekFromUtcDate(uint16_t year, uint8_t month, uint8_t day, IsoWeek& out);

std::string meetingsPageUrl(const IsoWeek& week);
std::string pubMediaUrl(MeetingPub pub, const char* issue, const char* languageKey);

// Filename the CDN serves a publication under: the last path segment of its
// media url ("w_S_202607.epub"). Empty when the url ends in a separator.
std::string filenameFromUrl(const std::string& url);

/**
 * Recovers both publication issue numbers from the "Other Meeting Publications"
 * links on a wol.jw.org meetings page, fed in arbitrary chunks.
 *
 * The year in those paths is the *publication's* year, not the ISO year of the
 * week — week 2026/01 points at the-watchtower-2025 — so the scanner matches a
 * year-less prefix and parses the four digits it finds. Templating the device's
 * ISO year into the prefix silently finds nothing for the ~17% of weeks (early
 * January through early March) that reference the previous year.
 *
 * Matching is byte-driven rather than buffer-and-search, so a link split across
 * any chunk boundary is recovered without carrying a tail window.
 */
class WolWeekScanner {
 public:
  WolWeekScanner();

  void reset();
  void feed(const char* data, size_t len);

  bool has(MeetingPub pub) const;
  // "202607" once has() is true, "" before that.
  const char* issue(MeetingPub pub) const;
  // How many of the two publications this week references.
  int count() const;

 private:
  // "the-watchtower-" <year> "/study-edition/" <month>, and
  // "life-and-ministry-meeting-workbook-" <year> "/" <month>.
  enum class Stage : uint8_t { Prefix, Year, Middle, Month, Done };

  struct Matcher {
    const char* prefix;
    const char* middle;
    Stage stage;
    uint8_t prefixPos;
    uint8_t middlePos;
    uint8_t yearLen;
    uint8_t monthLen;
    char year[5];
    char month[12];
    char issue[7];

    void reset();
    void consume(char c);
    void restartPrefix(char c);
    void commit();
  };

  Matcher& matcherFor(MeetingPub pub);
  const Matcher& matcherFor(MeetingPub pub) const;

  Matcher watchtower_;
  Matcher workbook_;
};
