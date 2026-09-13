#pragma once

#include <string>

// On-disk filename for a downloaded meeting publication: "<pubName> <YYYY-MM>.epub",
// e.g. "La Atalaya (ed. estudio) 2026-07.epub". Publication first so a file browser
// groups by publication, the numeric issue code second so issues sort
// chronologically within a group.
//
// `issue` is the six-digit code the week scan recovered ("202607"); the YYYY-MM
// suffix is derived from it rather than from the API's formattedDate, which
// carries HTML entities. Falls back to the CDN's own filename when the response
// published no usable pubName or the issue is malformed.
//
// Pure: no I/O, no globals.
std::string meetingPublicationFilename(const char* pubName, const char* issue, const std::string& url);
