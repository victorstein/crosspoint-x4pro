#pragma once

#include <string>

// Text rules for one row of the Highlights list. The row shows the reference in
// its label slot and this composed string in its subtitle slot, so the '\n' here
// is what produces the third line: the SDK's layout engine hard-breaks on it
// (FreeInkUICore.h:716-717).
//
// Free function in a header so the rules are host-testable — the activity that
// uses them cannot be built off-device.
namespace HighlightRowText {

// Replaces every control character with a space, so neither half can add a line
// the row was not measured for. A tag name is user-entered and a passage comes
// from book markup, so neither is trusted to be single-line.
inline std::string flatten(std::string text) {
  for (char& c : text) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  }
  return text;
}

// Joins a passage and a rendered tag list into one subtitle. The '\n' is
// inserted ONLY when both halves are non-empty: layoutText preserves a blank
// line for a leading '\n' (FreeInkUICore.h:773-776), which would render an empty
// first line on a row whose passage is missing.
inline std::string composeSubtitle(const std::string& passage, const std::string& tags) {
  const std::string flatPassage = flatten(passage);
  const std::string flatTags = flatten(tags);
  if (flatPassage.empty()) return flatTags;
  if (flatTags.empty()) return flatPassage;
  return flatPassage + "\n" + flatTags;
}

}  // namespace HighlightRowText
