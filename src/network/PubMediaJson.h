#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

/**
 * Pulls the EPUB url, size and MD5 out of a GETPUBMEDIALINKS response.
 *
 * StreamingJsonParser is a pure SAX tokenizer with no path addressing, so this
 * keeps its own container stack and only accepts values at
 * files.<language>.EPUB[0].{file.url, file.checksum, filesize}. The response
 * carries a pubImage.url (an empty string) ahead of files, so a first-url-wins
 * or last-key-wins matcher reads the wrong field. pubName is taken at the root,
 * where parentPubName sits immediately beside it, so the key match is exact.
 */
class PubMediaJsonParser {
 public:
  // languageKey is the JSON key under "files", e.g. "S" for Spanish. Not copied:
  // it must outlive the parser.
  explicit PubMediaJsonParser(const char* languageKey);

  PubMediaJsonParser(const PubMediaJsonParser&) = delete;
  PubMediaJsonParser& operator=(const PubMediaJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool found() const { return url_[0] != '\0'; }
  const char* url() const { return url_; }
  // Lowercase hex MD5, "" when the response omitted it.
  const char* checksum() const { return checksum_; }
  uint64_t filesize() const { return filesize_; }
  // Publication name as published, "" when the response omitted it. Sized for
  // the 196-byte worst case across languages (Khmer).
  const char* pubName() const { return pubName_; }

 private:
  static constexpr size_t MAX_DEPTH = 16;

  enum class Node : uint8_t {
    Other,
    Root,
    Files,
    Language,
    EpubArray,
    EpubEntry,
    EntryFile,
  };

  // Default-initialized so the whole stack is well-defined at construction:
  // reset() only rewinds depth_, leaving frames above it untouched.
  struct Frame {
    Node node = Node::Other;
    bool isArray = false;
    uint16_t index = 0;  // index of the element being read, for array frames
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  Node currentNode() const;
  bool keyIs(const char* name) const;
  void push(bool isArray);
  void pop();
  void valueComplete();

  StreamingJsonParser parser_;
  const char* languageKey_;

  Frame stack_[MAX_DEPTH];
  uint8_t depth_;
  char pendingKey_[64];

  char url_[160];
  char checksum_[33];
  char pubName_[208];
  uint64_t filesize_;
};
