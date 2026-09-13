#include "PubMediaJson.h"

#include <Utf8.h>

#include <cstdlib>
#include <cstring>

namespace {

void safeCopy(char* dst, const size_t dstSize, const char* src, const size_t srcLen) {
  const size_t capacity = dstSize - 1;
  size_t n = srcLen;
  if (n > capacity) {
    // A byte-wise cut can land mid-sequence, and the dangling lead byte reaches
    // SdFat as an invalid UTF-8 filename under USE_UTF8_LONG_NAMES.
    n = static_cast<size_t>(utf8SafeTruncateBuffer(src, static_cast<int>(capacity)));
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

PubMediaJsonParser::PubMediaJsonParser(const char* languageKey)
    : parser_(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                            sOnArrayStart, sOnArrayEnd}),
      languageKey_(languageKey) {
  reset();
}

void PubMediaJsonParser::reset() {
  parser_.reset();
  depth_ = 0;
  pendingKey_[0] = '\0';
  url_[0] = '\0';
  checksum_[0] = '\0';
  pubName_[0] = '\0';
  filesize_ = 0;
}

void PubMediaJsonParser::feed(const char* data, const size_t len) { parser_.feed(data, len); }

PubMediaJsonParser::Node PubMediaJsonParser::currentNode() const {
  if (depth_ == 0 || depth_ > MAX_DEPTH) return Node::Other;
  return stack_[depth_ - 1].node;
}

bool PubMediaJsonParser::keyIs(const char* name) const { return strcmp(pendingKey_, name) == 0; }

void PubMediaJsonParser::push(const bool isArray) {
  const Node parent = currentNode();
  Node node = Node::Other;
  if (depth_ == 0) {
    if (!isArray) node = Node::Root;
  } else if (parent == Node::Root && !isArray && keyIs("files")) {
    node = Node::Files;
  } else if (parent == Node::Files && !isArray && keyIs(languageKey_)) {
    node = Node::Language;
  } else if (parent == Node::Language && isArray && keyIs("EPUB")) {
    node = Node::EpubArray;
  } else if (parent == Node::EpubArray && !isArray && stack_[depth_ - 1].index == 0) {
    node = Node::EpubEntry;
  } else if (parent == Node::EpubEntry && !isArray && keyIs("file")) {
    node = Node::EntryFile;
  }

  if (depth_ < MAX_DEPTH) stack_[depth_] = Frame{node, isArray, 0};
  // Depth still advances past MAX_DEPTH so pop() stays balanced; currentNode()
  // reports Other while over the limit.
  ++depth_;
  pendingKey_[0] = '\0';
}

void PubMediaJsonParser::pop() {
  if (depth_ > 0) --depth_;
  valueComplete();
}

void PubMediaJsonParser::valueComplete() {
  if (depth_ > 0 && depth_ <= MAX_DEPTH) {
    Frame& frame = stack_[depth_ - 1];
    if (frame.isArray) ++frame.index;
  }
  pendingKey_[0] = '\0';
}

void PubMediaJsonParser::sOnKey(void* ctx, const char* key, const size_t len) {
  auto* self = static_cast<PubMediaJsonParser*>(ctx);
  safeCopy(self->pendingKey_, sizeof(self->pendingKey_), key, len);
}

void PubMediaJsonParser::sOnString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<PubMediaJsonParser*>(ctx);
  if (self->currentNode() == Node::EntryFile) {
    if (self->keyIs("url")) {
      safeCopy(self->url_, sizeof(self->url_), value, len);
    } else if (self->keyIs("checksum")) {
      safeCopy(self->checksum_, sizeof(self->checksum_), value, len);
    }
  } else if (self->currentNode() == Node::Root && self->keyIs("pubName")) {
    safeCopy(self->pubName_, sizeof(self->pubName_), value, len);
  }
  self->valueComplete();
}

void PubMediaJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<PubMediaJsonParser*>(ctx);
  if (self->currentNode() == Node::EpubEntry && self->keyIs("filesize")) {
    self->filesize_ = strtoull(value, nullptr, 10);
  }
  self->valueComplete();
}

void PubMediaJsonParser::sOnBool(void* ctx, bool /*value*/) { static_cast<PubMediaJsonParser*>(ctx)->valueComplete(); }

void PubMediaJsonParser::sOnNull(void* ctx) { static_cast<PubMediaJsonParser*>(ctx)->valueComplete(); }

void PubMediaJsonParser::sOnObjectStart(void* ctx) { static_cast<PubMediaJsonParser*>(ctx)->push(false); }

void PubMediaJsonParser::sOnObjectEnd(void* ctx) { static_cast<PubMediaJsonParser*>(ctx)->pop(); }

void PubMediaJsonParser::sOnArrayStart(void* ctx) { static_cast<PubMediaJsonParser*>(ctx)->push(true); }

void PubMediaJsonParser::sOnArrayEnd(void* ctx) { static_cast<PubMediaJsonParser*>(ctx)->pop(); }
