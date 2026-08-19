#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "GfxRenderer/BitBlit.h"

namespace {

// 4 bytes per row (32 px), 4 rows.
constexpr int32_t kStride = 4;
constexpr int kRows = 4;

std::array<uint8_t, kStride * kRows> makeBuf(const uint8_t fill) {
  std::array<uint8_t, kStride * kRows> buf{};
  buf.fill(fill);
  return buf;
}

}  // namespace

TEST(BitBlit, InvertsASinglePixel) {
  auto buf = makeBuf(0x00);
  bitblit::invertRect(buf.data(), kStride, 0, 0, 0, 0);
  EXPECT_EQ(buf[0], 0x80) << "x=0 must be the most significant bit";
  EXPECT_EQ(buf[1], 0x00);
}

TEST(BitBlit, InvertsAPartialRangeInsideOneByte) {
  auto buf = makeBuf(0x00);
  // Pixels 2..5 -> bits 5,4,3,2 -> 0b00111100.
  bitblit::invertRect(buf.data(), kStride, 2, 0, 5, 0);
  EXPECT_EQ(buf[0], 0x3C);
}

TEST(BitBlit, InvertsARangeSpanningThreeBytes) {
  auto buf = makeBuf(0x00);
  // Pixels 4..20: tail of byte 0, all of byte 1, head of byte 2.
  bitblit::invertRect(buf.data(), kStride, 4, 0, 20, 0);
  EXPECT_EQ(buf[0], 0x0F);
  EXPECT_EQ(buf[1], 0xFF);
  EXPECT_EQ(buf[2], 0xF8);
  EXPECT_EQ(buf[3], 0x00);
}

TEST(BitBlit, InvertsARangeSpanningExactlyTwoBytes) {
  auto buf = makeBuf(0x00);
  // Pixels 6..9: tail of byte 0 (bits 1,0), head of byte 1 (bits 7,6).
  bitblit::invertRect(buf.data(), kStride, 6, 0, 9, 0);
  EXPECT_EQ(buf[0], 0x03);
  EXPECT_EQ(buf[1], 0xC0);
}

TEST(BitBlit, InvertsAFullByteAlignedToItsEdges) {
  auto buf = makeBuf(0x00);
  bitblit::invertRect(buf.data(), kStride, 8, 0, 15, 0);
  EXPECT_EQ(buf[1], 0xFF);
}

TEST(BitBlit, InvertsAFullRowWidth) {
  auto buf = makeBuf(0x00);
  bitblit::invertRect(buf.data(), kStride, 0, 0, 31, 0);
  EXPECT_EQ(buf[0], 0xFF);
  EXPECT_EQ(buf[1], 0xFF);
  EXPECT_EQ(buf[2], 0xFF);
  EXPECT_EQ(buf[3], 0xFF);
  EXPECT_EQ(buf[kStride], 0x00) << "row 1 untouched";
}

TEST(BitBlit, OnlyTouchesRowsInRange) {
  auto buf = makeBuf(0x00);
  bitblit::invertRect(buf.data(), kStride, 0, 1, 31, 2);
  EXPECT_EQ(buf[0], 0x00) << "row 0 untouched";
  EXPECT_EQ(buf[kStride], 0xFF);
  EXPECT_EQ(buf[kStride * 2], 0xFF);
  EXPECT_EQ(buf[kStride * 3], 0x00) << "row 3 untouched";
}

TEST(BitBlit, IsItsOwnInverse) {
  auto buf = makeBuf(0xA5);
  const auto original = buf;
  bitblit::invertRect(buf.data(), kStride, 3, 0, 27, 3);
  EXPECT_NE(buf, original);
  bitblit::invertRect(buf.data(), kStride, 3, 0, 27, 3);
  EXPECT_EQ(buf, original) << "inverting twice must restore the buffer exactly";
}

TEST(BitBlit, IgnoresEmptyRanges) {
  auto buf = makeBuf(0x5A);
  const auto original = buf;
  bitblit::invertRect(buf.data(), kStride, 5, 0, 4, 0);
  bitblit::invertRect(buf.data(), kStride, 0, 3, 7, 2);
  EXPECT_EQ(buf, original);
}
