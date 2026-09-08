#pragma once

// Row layout of TagPickerActivity's list: "Done", then the palette, then
// "New tag...". The mapping lives here as free functions so it can be tested
// on the host: an adversarial review found the row offset applied in some
// consumers and not others, and the only way to catch that on the device is
// to hold Confirm on exactly the right row and notice nothing was deleted.
namespace TagRows {

constexpr int DONE = 0;

// Row index -> index into the palette, or -1 for a row that is not a tag
// ("Done", "New tag...", or out of range).
constexpr int tagIndexForRow(const int row, const int tagCount) {
  if (row <= DONE || row > tagCount) return -1;
  return row - 1;
}

constexpr int rowForTagIndex(const int tagIndex) { return tagIndex + 1; }
constexpr int newTagRow(const int tagCount) { return tagCount + 1; }
constexpr int rowCount(const int tagCount) { return tagCount + 2; }

}  // namespace TagRows
