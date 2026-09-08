// Repagination invariance for per-word visible-codepoint anchors.
//
// The property under test: a highlight recorded at font size A must resolve to
// the same words at font size B. Every tagged highlight in the reader rests on
// this. Line breaks move when the font size or viewport changes; the anchors
// must not.
//
// These tests run ParsedText's real layout against a fake renderer
// (GfxRendererFake.cpp). Read the header comment there before trusting a green
// run: the fake reproduces the shape of text metrics, not their values, so
// fidelity, fp4 rounding and SD-card font metrics remain on-device concerns.
// What IS covered here is the anchoring layer's internal consistency, which is
// the part that survives manual spot-checks and then fails on someone else's
// book at a font size nobody tried.

#include <Epub/ParsedText.h>
#include <Epub/VisibleRange.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// The stub HalDisplay carries no state; GfxRenderer only holds a reference.
HalDisplay halDisplay;

// fontId doubles as font size in the fake renderer, so this sweep is a real
// font-size sweep. Viewport widths vary independently: the same text at the
// same size in a narrower column is the other way repagination happens.
const std::vector<int> kFontIds = {8, 10, 12, 14, 16, 20, 24};
const std::vector<int> kViewports = {200, 260, 320, 400, 480};

uint32_t visibleCpCount(const std::string& s) {
  uint32_t n = 0;
  for (const unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

struct SourceWord {
  std::string text;
  uint32_t offset = 0;
  uint32_t cpLen = 0;

  VisibleRange range() const { return VisibleRange{offset, offset + cpLen}; }
};

// Assigns each word the visible-codepoint offset it would carry in a spine
// body, counting one codepoint for each separating space.
std::vector<SourceWord> makeSource(const std::vector<std::string>& texts) {
  std::vector<SourceWord> out;
  uint32_t offset = 0;
  for (const auto& t : texts) {
    const uint32_t len = visibleCpCount(t);
    out.push_back({t, offset, len});
    offset += len + 1;
  }
  return out;
}

struct PlacedWord {
  std::string text;
  uint32_t offset = 0;
  size_t line = 0;
};

struct Layout {
  std::vector<PlacedWord> words;
  size_t lineCount = 0;
};

Layout layoutAt(const std::vector<SourceWord>& source, const int fontId, const int viewport, const bool hyphenation,
                const BlockStyle& style = BlockStyle{}) {
  const GfxRenderer renderer(halDisplay);
  ParsedText parsed(/*extraParagraphSpacing=*/false, hyphenation, /*focusReading=*/false, style);
  for (const auto& w : source) {
    parsed.addWord(w.text, EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false, w.offset);
  }

  Layout out;
  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(viewport), [&](const std::shared_ptr<TextBlock> block, uint32_t) {
        if (!block || !block->valid()) return;
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          out.words.push_back({block->wordText(i), block->wordVisibleOffset(i), out.lineCount});
        }
        ++out.lineCount;
      });
  return out;
}

const std::vector<std::string> kParagraph = {
    "The",   "quick",  "brown",     "fox",       "jumps",  "over",    "the",     "lazy",      "dog",
    "while", "eleven", "wandering", "minstrels", "hummed", "quietly", "beneath", "windswept", "battlements",
    "and",   "then",   "some",      "more",      "words",  "follow",  "here",    "too"};

// Long enough to force hyphenation at narrow widths and large sizes.
const std::vector<std::string> kHyphenatable = {"Supercalifragilistic",   "antidisestablishmentarianism",
                                                "extraordinarily",        "incomprehensibilities",
                                                "counterrevolutionaries", "internationalization"};

// Maps each placed word back to the source word whose range contains its
// anchor. Returns nullptr when no source word claims it, which is itself a
// failure worth reporting.
const SourceWord* ownerOf(const std::vector<SourceWord>& source, const uint32_t offset) {
  for (const auto& w : source)
    if (w.range().contains(offset)) return &w;
  return nullptr;
}

std::string describe(const int fontId, const int viewport) {
  return "fontId=" + std::to_string(fontId) + " viewport=" + std::to_string(viewport);
}

// Layout splits an overlong word and appends a hyphen to the head. This happens
// even when hyphenation is disabled: a word wider than the whole column has to
// break somewhere. None of the corpora in this file contain a literal hyphen,
// so a trailing '-' on a non-final fragment is always layout's.
std::string withoutInsertedHyphen(const std::string& fragment, const bool isFinalFragment) {
  if (isFinalFragment || fragment.empty() || fragment.back() != '-') return fragment;
  return fragment.substr(0, fragment.size() - 1);
}

// The core invariant, checked against one layout.
//
// Cut the layout's fragments back into their source words by anchor alone, and
// the source paragraph must come back exactly: each word's first fragment sits
// at the word's own offset, each subsequent fragment sits exactly one head's
// worth of codepoints further on, and the concatenation is the original word.
// A fragment carrying the wrong anchor lands in the wrong word and fails to
// reconstruct -- which is what makes this sensitive to a visual index used
// where a logical one was meant.
void expectFragmentsReconstructSource(const std::vector<SourceWord>& source, const Layout& layout,
                                      const std::string& context) {
  std::map<uint32_t, std::vector<PlacedWord>> byOwner;
  for (const auto& w : layout.words) {
    const SourceWord* owner = ownerOf(source, w.offset);
    ASSERT_NE(owner, nullptr) << "anchor " << w.offset << " ('" << w.text << "') fell between source words at "
                              << context;
    byOwner[owner->offset].push_back(w);
  }

  EXPECT_EQ(byOwner.size(), source.size()) << "not every source word received a fragment at " << context;

  for (const auto& src : source) {
    const auto it = byOwner.find(src.offset);
    ASSERT_NE(it, byOwner.end()) << "source word '" << src.text << "' (offset " << src.offset
                                 << ") produced no fragment at " << context;

    std::vector<PlacedWord> fragments = it->second;
    std::sort(fragments.begin(), fragments.end(),
              [](const PlacedWord& a, const PlacedWord& b) { return a.offset < b.offset; });

    std::string rebuilt;
    uint32_t expectedNext = src.offset;
    for (size_t i = 0; i < fragments.size(); ++i) {
      EXPECT_EQ(fragments[i].offset, expectedNext)
          << "fragment '" << fragments[i].text << "' of '" << src.text << "' is anchored at " << fragments[i].offset
          << " but the preceding text implies " << expectedNext << " at " << context;
      const std::string body = withoutInsertedHyphen(fragments[i].text, i + 1 == fragments.size());
      rebuilt += body;
      expectedNext += visibleCpCount(body);
    }

    EXPECT_EQ(rebuilt, src.text) << "fragments did not reconstruct '" << src.text << "' at " << context;
    EXPECT_EQ(expectedNext, src.offset + src.cpLen)
        << "fragments of '" << src.text << "' do not cover it exactly at " << context;
  }
}

size_t countSplitWords(const std::vector<SourceWord>& source, const Layout& layout) {
  std::map<uint32_t, size_t> perOwner;
  for (const auto& w : layout.words) {
    if (const SourceWord* owner = ownerOf(source, w.offset)) perOwner[owner->offset]++;
  }
  size_t split = 0;
  for (const auto& [_, count] : perOwner)
    if (count > 1) ++split;
  return split;
}

}  // namespace

// ---------------------------------------------------------------------------
// The headline property.
// ---------------------------------------------------------------------------

// A highlight recorded at one font size must still name the same word after the
// text is repaginated at another size, and in another column width.
TEST(RepaginationInvariance, ARecordedAnchorStillNamesTheSameWordAfterRepagination) {
  const auto source = makeSource(kParagraph);

  const Layout reference = layoutAt(source, /*fontId=*/10, /*viewport=*/300, /*hyphenation=*/false);
  const auto foxIt =
      std::find_if(reference.words.begin(), reference.words.end(), [](const PlacedWord& w) { return w.text == "fox"; });
  ASSERT_NE(foxIt, reference.words.end()) << "reference layout did not place the probe word";
  const uint32_t recordedAnchor = foxIt->offset;

  for (const auto& [fontId, viewport] : std::vector<std::pair<int, int>>{{18, 300}, {10, 460}, {24, 200}}) {
    const Layout relaid = layoutAt(source, fontId, viewport, /*hyphenation=*/false);
    const auto it = std::find_if(relaid.words.begin(), relaid.words.end(),
                                 [&](const PlacedWord& w) { return w.offset == recordedAnchor; });
    ASSERT_NE(it, relaid.words.end()) << "anchor " << recordedAnchor << " vanished at " << describe(fontId, viewport);
    EXPECT_EQ(it->text, "fox") << "anchor " << recordedAnchor << " drifted to a different word at "
                               << describe(fontId, viewport);
  }

  // Anti-vacuity guard. The viewport is held at the reference's 300 so that
  // FONT SIZE is the only variable: if the fake's advance is ever changed to a
  // constant, every layout in this file collapses to the same line breaks and
  // the whole suite goes green while asserting nothing. Varying the viewport
  // here too would mask that, because the width alone would still move breaks.
  const Layout sameWidthLargerFont = layoutAt(source, /*fontId=*/24, /*viewport=*/300, /*hyphenation=*/false);
  EXPECT_NE(sameWidthLargerFont.lineCount, reference.lineCount)
      << "font size alone did not change the line count -- is the fake's advance still a function of fontId?";
}

// The full sweep: at every font size and column width, the anchors must cut the
// paragraph back into exactly the words it was built from.
TEST(RepaginationInvariance, AnchorsReconstructTheParagraphAtEverySize) {
  const auto source = makeSource(kParagraph);

  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/false);
      expectFragmentsReconstructSource(source, layout, describe(fontId, viewport));
    }
  }
}

// Every source word must remain addressable at every size -- no word may become
// unanchorable because of where a break landed.
TEST(RepaginationInvariance, EverySourceWordKeepsAnAnchorAtEverySize) {
  const auto source = makeSource(kParagraph);

  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/false);
      std::set<uint32_t> seen;
      for (const auto& w : layout.words) seen.insert(w.offset);
      for (const auto& w : source) {
        EXPECT_TRUE(seen.count(w.offset)) << "source word '" << w.text << "' (offset " << w.offset
                                          << ") had no anchor at " << describe(fontId, viewport);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Ordering and partitioning.
// ---------------------------------------------------------------------------

TEST(AnchorOrdering, AnchorsStrictlyIncreaseThroughAnLtrLayout) {
  const auto source = makeSource(kParagraph);

  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/true);
      for (size_t i = 1; i < layout.words.size(); ++i) {
        EXPECT_LT(layout.words[i - 1].offset, layout.words[i].offset)
            << "anchors went backwards between '" << layout.words[i - 1].text << "' and '" << layout.words[i].text
            << "' at " << describe(fontId, viewport);
      }
    }
  }
}

// Every anchor must fall inside exactly one source word, and every source word
// must be claimed: no gaps, no overlaps, nothing stranded between words.
TEST(AnchorOrdering, AnchorsPartitionTheParagraphWithoutGapsOrOverlaps) {
  const auto source = makeSource(kParagraph);

  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/true);
      std::set<uint32_t> claimed;
      for (const auto& w : layout.words) {
        const SourceWord* owner = ownerOf(source, w.offset);
        ASSERT_NE(owner, nullptr) << "anchor " << w.offset << " ('" << w.text << "') fell between source words at "
                                  << describe(fontId, viewport);
        claimed.insert(owner->offset);
      }
      for (const auto& w : source) {
        EXPECT_TRUE(claimed.count(w.offset))
            << "source word '" << w.text << "' unclaimed at " << describe(fontId, viewport);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Hyphenation: the fragile path.
// ---------------------------------------------------------------------------

// When a word is split across a line break, the next fragment's anchor must be
// the previous fragment's anchor advanced by exactly that fragment's visible
// codepoint count -- excluding the hyphen, which layout inserts and which is
// not part of the source text. A word can split more than once, so this is a
// chain, not a single step.
TEST(HyphenSplitAnchors, EachFragmentAnchorEqualsThePreviousPlusItsCodepoints) {
  const auto source = makeSource(kHyphenatable);

  size_t splitWordsObserved = 0;
  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/true);
      expectFragmentsReconstructSource(source, layout, describe(fontId, viewport));
      splitWordsObserved += countSplitWords(source, layout);

      // A split point must land strictly inside its word, never on an edge.
      for (const auto& w : layout.words) {
        const SourceWord* owner = ownerOf(source, w.offset);
        ASSERT_NE(owner, nullptr);
        if (w.offset == owner->offset) continue;
        EXPECT_GT(w.offset, owner->offset)
            << "split anchor at or before the word start at " << describe(fontId, viewport);
        EXPECT_LT(w.offset, owner->offset + owner->cpLen)
            << "split anchor past the end of '" << owner->text << "' at " << describe(fontId, viewport);
      }
    }
  }
  EXPECT_GT(splitWordsObserved, 0u) << "no word was ever split -- this test asserted nothing";
}

// ---------------------------------------------------------------------------
// RTL / bidi.
// ---------------------------------------------------------------------------

namespace {

// Distinct Hebrew words interleaved with Latin, so a reversed run is
// unambiguous: every word text appears exactly once.
const std::vector<std::string> kBidiParagraph = {
    "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "\xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d", "Reader",
    "\xd7\xa1\xd7\xa4\xd7\xa8",         "\xd7\x91\xd7\x99\xd7\xaa",         "Crosspoint",
    "\xd7\x9e\xd7\x99\xd7\x9d",         "\xd7\x90\xd7\x95\xd7\xa8"};

BlockStyle rtlStyle() {
  BlockStyle style;
  style.isRtl = true;
  style.directionDefined = true;
  return style;
}

}  // namespace

// The commit that populates per-word visible offsets does so through the same
// visual permutation that reorders the words on an RTL line. If it ever indexed
// the logical array with a visual index (or the reverse), the line would anchor
// to mirrored words -- and every LTR test above would still pass. This is that
// test: it fails unless each word carries the anchor of the word it actually
// is, whatever position it was moved to.
TEST(BidiAnchors, RtlLineAnchorsFollowLogicalWordsNotVisualPositions) {
  const auto source = makeSource(kBidiParagraph);

  size_t reorderedLines = 0;
  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/false, rtlStyle());
      expectFragmentsReconstructSource(source, layout, describe(fontId, viewport));

      // Lines whose anchors are not ascending are the visually reordered ones,
      // and they are the only ones that prove anything about bidi.
      for (size_t line = 0; line < layout.lineCount; ++line) {
        std::vector<uint32_t> onLine;
        for (const auto& w : layout.words)
          if (w.line == line) onLine.push_back(w.offset);
        if (!std::is_sorted(onLine.begin(), onLine.end())) ++reorderedLines;
      }
    }
  }

  EXPECT_GT(reorderedLines, 0u) << "no line was ever visually reordered -- this test asserted nothing about bidi";
}

// RTL anchors must survive repagination exactly as LTR ones do.
TEST(BidiAnchors, RtlAnchorsAreStableAcrossRepagination) {
  const auto source = makeSource(kBidiParagraph);

  const Layout reference = layoutAt(source, /*fontId=*/10, /*viewport=*/300, /*hyphenation=*/false, rtlStyle());
  std::map<std::string, uint32_t> referenceAnchors;
  for (const auto& w : reference.words) referenceAnchors[w.text] = w.offset;
  ASSERT_FALSE(referenceAnchors.empty());

  for (const int fontId : kFontIds) {
    for (const int viewport : kViewports) {
      const Layout layout = layoutAt(source, fontId, viewport, /*hyphenation=*/false, rtlStyle());
      for (const auto& w : layout.words) {
        const auto it = referenceAnchors.find(w.text);
        if (it == referenceAnchors.end()) continue;  // a fragment the reference layout did not produce
        EXPECT_EQ(w.offset, it->second) << "RTL word '" << w.text << "' moved from anchor " << it->second << " to "
                                        << w.offset << " at " << describe(fontId, viewport);
      }
    }
  }
}
