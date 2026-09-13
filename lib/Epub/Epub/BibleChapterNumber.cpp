#include "BibleChapterNumber.h"

namespace BibleChapterNumber {

bool Reader::feed(const char* chunk, const size_t length, const bool isFinal) {
  if (resolved()) return true;
  if (!scanner_.feed(chunk, length, isFinal)) return false;

  // take() hands back whatever the parse has seen so far and needs no final
  // chunk, so an anchor is visible the moment the chunk carrying it lands.
  const auto anchors = scanner_.take();
  if (!anchors.empty()) chapter_ = anchors[0].chapter;
  return true;
}

int scan(const char* xhtml, const size_t length) {
  Reader reader;
  if (!reader.feed(xhtml, length, /*isFinal=*/true)) return -1;
  return reader.chapter();
}

}  // namespace BibleChapterNumber
