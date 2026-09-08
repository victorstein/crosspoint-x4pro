#include "VerseAnchors.h"

#include <expat.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "VisibleOffsetCounter.h"
#include "htmlEntities.h"

namespace VerseAnchors {
namespace {

struct State {
  VisibleOffsetCounter counter;
  std::vector<VerseAnchor> anchors;
};

void XMLCALL onText(void* userData, const XML_Char* text, const int len) {
  static_cast<State*>(userData)->counter.onCharacterData(text, len);
}

void XMLCALL onStart(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<State*>(userData);
  self->counter.onStartElement(name);
  if (!self->counter.insideBody) return;

  for (int i = 0; atts && atts[i]; i += 2) {
    if (strcmp(atts[i], "id") != 0) continue;
    unsigned chapter = 0;
    unsigned verse = 0;
    char tail = '\0';
    // The %c catches trailing junk, so "chapter1_verse2x" is not a verse marker.
    if (sscanf(atts[i + 1], "chapter%u_verse%u%c", &chapter, &verse, &tail) == 2 && chapter <= UINT16_MAX &&
        verse <= UINT16_MAX) {
      self->anchors.push_back(
          {self->counter.offset, static_cast<uint16_t>(chapter), static_cast<uint16_t>(verse)});
    }
    break;
  }
}

void XMLCALL onEnd(void* userData, const XML_Char* name) {
  static_cast<State*>(userData)->counter.onEndElement(name);
}

// Mirrors ChapterHtmlSlimParser::defaultHandlerExpand. Under XML_GE=0 expat
// reports every undeclared general entity here rather than to the character
// handler, so without this every `&nbsp;` before a highlight would leave the
// count one codepoint short -- enough to name the previous verse.
void XMLCALL onDefault(void* userData, const XML_Char* s, const int len) {
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (value != nullptr) {
      onText(userData, value, static_cast<int>(strlen(value)));
      return;
    }
    // Unknown entity: the parser preserves the literal run, so count it whole.
    onText(userData, s, len);
  }
}

}  // namespace

Scanner::Scanner() {
  auto* state = new (std::nothrow) State();
  if (!state) return;
  state->anchors.reserve(64);  // a long chapter runs to ~176 verses; 64 covers most

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    delete state;
    return;
  }
  XML_SetUserData(parser, state);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);
  XML_SetDefaultHandlerExpand(parser, onDefault);

  parser_ = parser;
  state_ = state;
}

Scanner::~Scanner() {
  if (parser_) XML_ParserFree(static_cast<XML_Parser>(parser_));
  delete static_cast<State*>(state_);
}

bool Scanner::feed(const char* chunk, const size_t length, const bool isFinal) {
  if (!parser_ || failed_) return false;
  const XML_Status status =
      XML_Parse(static_cast<XML_Parser>(parser_), chunk, static_cast<int>(length), isFinal ? 1 : 0);
  if (status == XML_STATUS_ERROR) {
    failed_ = true;
    return false;
  }
  return true;
}

std::vector<VerseAnchor> Scanner::take() {
  if (!state_ || failed_) return {};
  return std::move(static_cast<State*>(state_)->anchors);
}

std::vector<VerseAnchor> scan(const char* xhtml, const size_t length) {
  Scanner scanner;
  if (!scanner.valid()) return {};
  if (!scanner.feed(xhtml, length, /*isFinal=*/true)) return {};
  return scanner.take();
}

const VerseAnchor* find(const std::vector<VerseAnchor>& anchors, const uint32_t offset) {
  const VerseAnchor* best = nullptr;
  for (const auto& anchor : anchors) {
    if (anchor.offset > offset) break;  // ascending by construction
    best = &anchor;
  }
  return best;
}

std::string format(const VerseAnchor* anchor) {
  if (!anchor) return {};
  char buf[16];
  snprintf(buf, sizeof(buf), "%u:%u", static_cast<unsigned>(anchor->chapter), static_cast<unsigned>(anchor->verse));
  return buf;
}

}  // namespace VerseAnchors
