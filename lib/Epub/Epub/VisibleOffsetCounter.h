#pragma once

#include <cstdint>
#include <cstring>

#include "VisibleTextUtils.h"

// The canonical visible-codepoint counter: zero-based Unicode codepoints in
// visible <body> text, immune to re-pagination because it depends only on the
// character stream.
//
// This exists so a second walk over the same document (VerseAnchors) counts
// identically rather than approximating it. A re-implementation cannot be kept
// in agreement by testing, because whoever writes the drift also writes the test.
//
// Callers own the two things outside the state machine: gating on their own
// synthetic-text flag before calling onCharacterData, and routing expanded
// entities through it (ChapterHtmlSlimParser::defaultHandlerExpand) -- an
// uncounted entity shifts every later offset.
struct VisibleOffsetCounter {
  uint32_t offset = 0;
  uint16_t nonVisibleDepth = 0;
  bool insideBody = false;

  // Case-INsensitive on open, case-SENSITIVE on close. That asymmetry is
  // ChapterHtmlSlimParser's actual behaviour (strcasecmp at :397, strcmp at
  // :1542) and is reproduced deliberately, not inherited by accident: these
  // offsets are serialized into every cached section, so changing which
  // documents close the gate would silently invalidate every book on every
  // device. Fix it, if ever, behind a SECTION_FILE_VERSION bump.
  void onStartElement(const char* name) {
    if (strcasecmp(name, "body") == 0) insideBody = true;
    if (insideBody && (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name))) nonVisibleDepth++;
  }

  void onEndElement(const char* name) {
    if (nonVisibleDepth > 0) nonVisibleDepth--;
    if (strcmp(name, "body") == 0) insideBody = false;
  }

  bool counting() const { return insideBody && nonVisibleDepth == 0; }

  void onCharacterData(const char* s, const int len) {
    if (!counting()) return;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    for (int i = 0; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) offset++;  // UTF-8 continuation bytes are not codepoints
    }
  }
};
