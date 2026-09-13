#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "network/PubMediaJson.h"

namespace {

std::string loadFixture(const char* name) {
  const std::string path = std::string(PUB_MEDIA_FIXTURE_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "missing fixture " << path;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

void feedInChunks(PubMediaJsonParser& parser, const std::string& body, const size_t chunk) {
  for (size_t offset = 0; offset < body.size(); offset += chunk) {
    parser.feed(body.data() + offset, std::min(chunk, body.size() - offset));
  }
}

const std::vector<size_t> kChunkSizes = {1, 7, 4096};

}  // namespace

TEST(PubMediaJson, ReadsTheWatchtowerEpub) {
  const std::string body = loadFixture("pubmedia_w_202607_S.json");
  for (const size_t chunk : kChunkSizes) {
    PubMediaJsonParser parser("S");
    feedInChunks(parser, body, chunk);
    ASSERT_TRUE(parser.found()) << "chunk size " << chunk;
    EXPECT_STREQ(parser.url(), "https://cfp2.jw-cdn.org/a/717b307/1/o/w_S_202607.epub") << "chunk size " << chunk;
    EXPECT_EQ(parser.filesize(), 3718294u) << "chunk size " << chunk;
    EXPECT_STREQ(parser.checksum(), "c0208a4acad782f4295753992134bad5") << "chunk size " << chunk;
  }
}

TEST(PubMediaJson, ReadsTheWorkbookEpub) {
  const std::string body = loadFixture("pubmedia_mwb_202609_S.json");
  for (const size_t chunk : kChunkSizes) {
    PubMediaJsonParser parser("S");
    feedInChunks(parser, body, chunk);
    ASSERT_TRUE(parser.found()) << "chunk size " << chunk;
    EXPECT_STREQ(parser.url(), "https://cfp2.jw-cdn.org/a/24f3ed/1/o/mwb_S_202609.epub") << "chunk size " << chunk;
    EXPECT_EQ(parser.filesize(), 3335824u) << "chunk size " << chunk;
    EXPECT_STREQ(parser.checksum(), "9516fc6ae0c0c204a9883e9069ffdb18") << "chunk size " << chunk;
  }
}

// pubImage.url is an empty string that appears ahead of "files", so a
// first-url-wins or last-key-wins matcher reports no download url at all.
TEST(PubMediaJson, IgnoresPubImageUrl) {
  const std::string body = loadFixture("pubmedia_w_202607_S.json");
  ASSERT_NE(body.find("\"pubImage\""), std::string::npos);
  ASSERT_LT(body.find("\"pubImage\""), body.find("\"files\""));

  PubMediaJsonParser parser("S");
  parser.feed(body.data(), body.size());
  EXPECT_STRNE(parser.url(), "");
  EXPECT_STREQ(parser.url(), "https://cfp2.jw-cdn.org/a/717b307/1/o/w_S_202607.epub");
}

// trackImage.checksum is null and sits beside file.checksum inside the same
// EPUB entry; only the one under "file" may be taken.
TEST(PubMediaJson, TakesTheChecksumUnderFile) {
  const std::string body = R"({
    "files": {"S": {"EPUB": [{
      "file": {"url": "https://cdn/w.epub", "stream": "https://jw.org", "checksum": "aaaa"},
      "filesize": 42,
      "trackImage": {"url": "https://cdn/track.png", "checksum": "bbbb"}
    }]}}
  })";
  PubMediaJsonParser parser("S");
  parser.feed(body.data(), body.size());
  EXPECT_STREQ(parser.url(), "https://cdn/w.epub");
  EXPECT_STREQ(parser.checksum(), "aaaa");
  EXPECT_EQ(parser.filesize(), 42u);
}

TEST(PubMediaJson, TakesOnlyTheFirstEpubEntry) {
  const std::string body = R"({
    "files": {"S": {"EPUB": [
      {"file": {"url": "https://cdn/first.epub", "checksum": "1111"}, "filesize": 1},
      {"file": {"url": "https://cdn/second.epub", "checksum": "2222"}, "filesize": 2}
    ]}}
  })";
  PubMediaJsonParser parser("S");
  parser.feed(body.data(), body.size());
  EXPECT_STREQ(parser.url(), "https://cdn/first.epub");
  EXPECT_STREQ(parser.checksum(), "1111");
  EXPECT_EQ(parser.filesize(), 1u);
}

TEST(PubMediaJson, IgnoresOtherLanguages) {
  const std::string body = R"({
    "files": {"E": {"EPUB": [{"file": {"url": "https://cdn/w_E.epub"}, "filesize": 9}]}}
  })";
  PubMediaJsonParser parser("S");
  parser.feed(body.data(), body.size());
  EXPECT_FALSE(parser.found());
  EXPECT_EQ(parser.filesize(), 0u);
}

TEST(PubMediaJson, ResetClearsPriorResults) {
  const std::string body = loadFixture("pubmedia_w_202607_S.json");
  PubMediaJsonParser parser("S");
  parser.feed(body.data(), body.size());
  ASSERT_TRUE(parser.found());
  parser.reset();
  EXPECT_FALSE(parser.found());
  EXPECT_EQ(parser.filesize(), 0u);
  EXPECT_STREQ(parser.checksum(), "");
}
