#pragma once

#include <cstddef>

#include "VerseAnchors.h"

// The chapter number of an NWT-shaped Bible chapter. These publications give
// the TOC one entry per BOOK, so the chapter is the spine item itself and its
// number survives only inside the file, in the `chapter<N>_verse<M>` marker ids
// VerseAnchors already knows how to read.
namespace BibleChapterNumber {

// Chunk-fed so a caller can stop reading as soon as the number is known: the
// first marker sits near the top of a file that runs to 69 KB for Psalm 119.
class Reader {
 public:
  // False once the document is malformed; the caller should stop and discard.
  // Feeding past the first marker is a no-op, so a caller that overshoots by a
  // chunk still reports the same chapter.
  bool feed(const char* chunk, size_t length, bool isFinal);
  bool resolved() const { return chapter_ > 0; }
  // -1 until the first verse marker is seen.
  int chapter() const { return chapter_; }

 private:
  VerseAnchors::Scanner scanner_;
  int chapter_ = -1;
};

// Whole-document form. -1 when the document carries no verse markers at all,
// which is how a non-Bible spine item reads.
int scan(const char* xhtml, size_t length);

}  // namespace BibleChapterNumber
