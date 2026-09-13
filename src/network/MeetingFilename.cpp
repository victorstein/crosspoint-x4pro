#include "MeetingFilename.h"

#include "network/WolWeekScan.h"
#include "util/StringUtils.h"

namespace {

constexpr size_t FILENAME_BUDGET_BYTES = 100;
// " YYYY-MM", appended after sanitising so the cap can never eat the code.
constexpr size_t ISSUE_SUFFIX_BYTES = 8;

bool isIssueCode(const char* issue) {
  if (!issue) return false;
  for (size_t i = 0; i < 6; ++i) {
    if (issue[i] < '0' || issue[i] > '9') return false;
  }
  return issue[6] == '\0';
}

// sanitizeFilename drops control characters and trims spaces and dots, so a name
// made only of those survives as its "book" fallback rather than as nothing.
bool hasUsableName(const char* pubName) {
  if (!pubName) return false;
  for (const char* c = pubName; *c != '\0'; ++c) {
    const auto byte = static_cast<unsigned char>(*c);
    if (byte >= 32 && byte != ' ' && byte != '.') return true;
  }
  return false;
}

}  // namespace

std::string meetingPublicationFilename(const char* pubName, const char* issue, const std::string& url) {
  // sanitizeFilename never returns empty — its last resort is the literal "book"
  // — so an unusable name has to be caught before sanitising, or the Watchtower
  // and the workbook would both claim "book <YYYY-MM>.epub" in the same run.
  if (!hasUsableName(pubName) || !isIssueCode(issue)) return filenameFromUrl(url);

  std::string filename = StringUtils::sanitizeFilename(pubName, FILENAME_BUDGET_BYTES - ISSUE_SUFFIX_BYTES);
  filename += ' ';
  filename.append(issue, 4);
  filename += '-';
  filename.append(issue + 4, 2);
  filename += ".epub";
  return filename;
}
