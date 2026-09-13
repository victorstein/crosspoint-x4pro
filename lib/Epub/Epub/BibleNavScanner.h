#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Reads the navigation pages of an NWT-shaped Bible EPUB: `biblebooknav.xhtml`
// lists the 66 books in canonical order, `biblechapternav<N>.xhtml` lists one
// book's chapters.
//
// Shaped like VerseAnchors::Scanner rather than TocNavParser: chunk-fed, expat
// behind an opaque pointer, and free of Print / BookMetadataCache / HalStorage
// so it stays host-testable.
namespace BibleNav {

// The feature gate. Absent from an EPUB means it is not a Bible we can walk,
// and nothing about that book changes.
inline constexpr const char* BOOK_NAV_FILENAME = "biblebooknav.xhtml";
inline constexpr const char* CHAPTER_NAV_PREFIX = "biblechapternav";

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
  // Filename tail of every `<a href>` in document order. Empty after a failed
  // feed. Scoped to `<a>` because the page also carries a `<link>` to
  // css/epubs.css, which is not a navigation target.
  std::vector<std::string> take();

 private:
  void* parser_ = nullptr;  // XML_Parser; opaque here to keep expat out of the header
  void* state_ = nullptr;   // State
  bool failed_ = false;
};

std::vector<std::string> scan(const char* xhtml, size_t length);

// Everything after the last '/'. TOC hrefs are stored base-prefixed
// ("OEBPS/biblechapternav1.xhtml", TocNavParser.cpp:129) while the nav pages
// yield bare ones, so every join between the two compares tails.
std::string_view filenameTail(std::string_view path);

// True when the book row points at a chapter-nav page. Five books (Obadiah,
// Philemon, 2 John, 3 John, Jude) have only one chapter and no nav page of
// their own, pointing straight at their chapter spine item instead -- so
// filtering book rows on the `biblechapternav` prefix silently drops them.
bool isChapterNav(std::string_view filename);

// Removes the back-link to biblebooknav.xhtml that every chapter-nav page
// opens with, leaving one entry per chapter in order.
void dropBookNavLinks(std::vector<std::string>& links);

// Index in `targets` whose filename tail matches `href`'s, or -1.
int findTargetByHref(const std::string* targets, int count, std::string_view href);

// Fills `names[i]` with the title of the TOC entry that resolves to
// `targets[i]`, leaving entries with no match untouched. Each book appears in
// the TOC twice -- an outline entry and the book entry -- but the outline
// points at a different file, so only the book entry can match and no
// language-specific string matching is needed.
void joinBookNames(const std::string* targets, int targetCount, const std::string* tocHrefs,
                   const std::string* tocTitles, int tocCount, std::string* names);

}  // namespace BibleNav
