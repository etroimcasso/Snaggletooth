// The tile codec: planar SNES tile bytes round-trip through an indexed PNG at
// each depth, a file that is not a whole number of tiles keeps its bytes and
// zero-pads the rest, and the plane-and-bit layout is the PPU's.

#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "formats/png.h"
#include "formats/tiles.h"
#include "gtest/gtest.h"

namespace snaggletooth::formats {
namespace {

std::vector<std::uint8_t> ramp(unsigned count) {
  std::vector<std::uint8_t> out;
  for (unsigned i = 0; i < count; ++i) {
    const std::uint8_t v = count <= 1 ? 0 : static_cast<std::uint8_t>(i * 255u / (count - 1));
    out.push_back(v);
    out.push_back(v);
    out.push_back(v);
    out.push_back(255);
  }
  return out;
}

// A buffer of `n` bytes with a repeating pattern, so a wrong plane is visible.
std::vector<std::uint8_t> pattern(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>((i * 37 + 5) & 0xFF);
  return out;
}

// Decode yields whole tiles up to the sheet's last row: the original bytes come
// back exactly and any padding past them is zero, and a bank file clips the
// result to its length.
void expectRoundTrip(const std::vector<std::uint8_t>& planar, unsigned depth) {
  const Bytes png = encodeTiles(planar, depth, ramp(1u << depth));
  ASSERT_TRUE(png.ok()) << png.error;
  const Bytes back = decodeTiles(png.bytes);
  ASSERT_TRUE(back.ok()) << back.error;
  ASSERT_GE(back.bytes.size(), planar.size());
  for (std::size_t i = 0; i < planar.size(); ++i) EXPECT_EQ(back.bytes[i], planar[i]) << i;
  for (std::size_t i = planar.size(); i < back.bytes.size(); ++i) EXPECT_EQ(back.bytes[i], 0) << i;
}

TEST(Tiles, TwoBitWholeSheetIsExact) {
  const auto planar = pattern(16 * 16);  // one full row of sixteen 2bpp tiles
  const Bytes png = encodeTiles(planar, 2, ramp(4));
  ASSERT_TRUE(png.ok());
  const Bytes back = decodeTiles(png.bytes);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back.bytes, planar);  // a full row needs no padding
}

TEST(Tiles, TwoBitTilesRoundTrip) { expectRoundTrip(pattern(16 * 3), 2); }

TEST(Tiles, FourBitTilesRoundTrip) { expectRoundTrip(pattern(32 * 5), 4); }

TEST(Tiles, EightBitTilesRoundTrip) { expectRoundTrip(pattern(64 * 2), 8); }

TEST(Tiles, ManyTilesCrossRows) { expectRoundTrip(pattern(16 * 40), 2); }

TEST(Tiles, PartialLastTileKeepsBytesThenZeroPads) {
  expectRoundTrip(pattern(20), 2);  // 2bpp: one whole tile plus four bytes
}

// The sheet is as wide as its tiles up to sixteen: a file under a row is one
// row of exactly its tiles, and one over wraps at sixteen.
TEST(Tiles, ASheetIsAsWideAsItsTilesUpToSixteen) {
  const auto widthOf = [](std::size_t bytes, unsigned depth) {
    const Bytes png = encodeTiles(pattern(bytes), depth, ramp(1u << depth));
    EXPECT_TRUE(png.ok()) << png.error;
    const PngImage image = decodePng(png.bytes);
    EXPECT_TRUE(image.ok()) << image.error;
    return std::make_pair(image.image.width, image.image.height);
  };
  EXPECT_EQ(widthOf(16, 2), std::make_pair(8u, 8u)) << "one tile";
  EXPECT_EQ(widthOf(20, 2), std::make_pair(16u, 8u)) << "one tile and a partial second";
  EXPECT_EQ(widthOf(32 * 5, 4), std::make_pair(40u, 8u)) << "five tiles, one row";
  EXPECT_EQ(widthOf(64 * 16, 8), std::make_pair(128u, 8u)) << "a full row";
  EXPECT_EQ(widthOf(16 * 17, 2), std::make_pair(128u, 16u)) << "one over a row wraps at sixteen";
}

TEST(Tiles, ANarrowSheetRoundTrips) {
  expectRoundTrip(pattern(32 * 3), 4);  // three 4bpp tiles: a sheet twenty-four pixels wide
}

TEST(Tiles, PlaneAndBitLayoutIsThePpu) {
  std::vector<std::uint8_t> tile(16, 0);
  tile[0] = 0x80;  // plane 0, row 0: left-most pixel
  tile[1] = 0x40;  // plane 1, row 0: second pixel
  const Bytes png = encodeTiles(tile, 2, ramp(4));
  ASSERT_TRUE(png.ok());
  const PngImage image = decodePng(png.bytes);
  ASSERT_TRUE(image.ok());
  EXPECT_EQ(image.image.indices[0], 1u);  // plane 0 bit set -> colour 1
  EXPECT_EQ(image.image.indices[1], 2u);  // plane 1 bit set -> colour 2
  EXPECT_EQ(image.image.indices[2], 0u);
}

TEST(Tiles, DepthMustBe248) {
  const Bytes png = encodeTiles(pattern(16), 3, ramp(4));
  EXPECT_FALSE(png.ok());
}

TEST(Tiles, NonTileWidthIsRefused) {
  IndexedImage odd{5, 8, 2, std::vector<std::uint8_t>(5 * 8, 0), ramp(4)};
  const Bytes png = encodePng(odd);
  ASSERT_TRUE(png.ok());
  const Bytes back = decodeTiles(png.bytes);
  EXPECT_FALSE(back.ok());
}

}  // namespace
}  // namespace snaggletooth::formats
