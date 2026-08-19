#pragma once

#include <string>

// Book-path helpers shared by the bookmark and highlight stores. Free of Arduino
// and storage dependencies so the rule can be unit tested on the host.
namespace pathflatten {

// Maps a book path to the flat filename stem used under /.crosspoint/<store>/.
// Strips the first character, replaces '/' and '\' with '_', and drops
// everything from the last '.' onward.
//
// Two inherited quirks, preserved deliberately so bookmarks and highlights agree:
// the last '.' is sought in the FLATTENED name, so a dot in a directory
// truncates it (/v1.0/mybook -> "v1"); and the first character is removed
// unconditionally, so a path without a leading '/' loses a real character.
// Changing either would orphan every existing bookmark file.
std::string toCacheName(const std::string& bookPath);

}  // namespace pathflatten
