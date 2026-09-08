#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Resolves a visible-codepoint offset inside one spine item to a Bible verse,
// for labelling a highlight. Verse markers in these EPUBs are empty spans with
// ids shaped `chapter<N>_verse<M>`, sitting at the verse boundary.
//
// The scan counts through VisibleOffsetCounter -- the same unit the layout
// parser uses -- and registers the same three expat handlers, including the
// default handler that expands entities. Counting one entity short shifts every
// later offset, and because a verse marker sits exactly on the boundary, a
// deficit of a single codepoint resolves a highlight to the PREVIOUS verse.
namespace VerseAnchors {

struct VerseAnchor {
  uint32_t offset;
  uint16_t chapter;
  uint16_t verse;
};

// Ascending by offset. Empty when the document has no verse markers, and also
// empty when the parse fails part-way: a truncated list would silently resolve
// later highlights to a stale anchor with no way for the caller to notice.
std::vector<VerseAnchor> scan(const char* xhtml, size_t length);

// Chunk-fed form, so a caller can stream a spine item instead of holding it
// whole. Deliberately takes bytes rather than a file handle: HalStorage is
// firmware-only and this must stay host-testable.
class Scanner {
 public:
  Scanner();
  ~Scanner();
  Scanner(const Scanner&) = delete;
  Scanner& operator=(const Scanner&) = delete;

  bool valid() const { return parser_ != nullptr; }
  // Returns false once the document is malformed; the caller should stop and
  // discard. `isFinal` marks the last chunk.
  bool feed(const char* chunk, size_t length, bool isFinal);
  // Anchors collected so far. Empty after a failed feed.
  std::vector<VerseAnchor> take();

 private:
  void* parser_ = nullptr;  // XML_Parser; opaque here to keep expat out of the header
  void* state_ = nullptr;   // State
  bool failed_ = false;
};

// The anchor covering `offset` -- the greatest one at or below it -- or nullptr.
const VerseAnchor* find(const std::vector<VerseAnchor>& anchors, uint32_t offset);

// "11:19", or empty for nullptr.
std::string format(const VerseAnchor* anchor);

}  // namespace VerseAnchors
