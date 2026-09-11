// The indexed PNG face: an image round-trips through encode and decode at each
// bit depth keeping its indexes and palette, and a PNG that is not an indexed
// image is refused with a message naming the colour type.

#include <cstdint>
#include <vector>

#include "formats/png.h"
#include "gtest/gtest.h"
#include "lodepng.h"

namespace snaggletooth::formats {
namespace {

// A ramp of `count` opaque colours, black to white, as RGBA quadruples.
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

IndexedImage roundTrip(const IndexedImage& in) {
  const Bytes png = encodePng(in);
  EXPECT_TRUE(png.ok()) << png.error;
  const PngImage back = decodePng(png.bytes);
  EXPECT_TRUE(back.ok()) << back.error;
  return back.image;
}

TEST(Png, TwoBitImageKeepsIndexesAndDepth) {
  IndexedImage in{4, 2, 2, {0, 1, 2, 3, 3, 2, 1, 0}, ramp(4)};
  const IndexedImage out = roundTrip(in);
  EXPECT_EQ(out.width, 4u);
  EXPECT_EQ(out.height, 2u);
  EXPECT_EQ(out.bitDepth, 2u);
  EXPECT_EQ(out.indices, in.indices);
}

TEST(Png, OneBitDepth) {
  IndexedImage in{8, 1, 1, {0, 1, 1, 0, 1, 0, 0, 1}, ramp(2)};
  const IndexedImage out = roundTrip(in);
  EXPECT_EQ(out.bitDepth, 1u);
  EXPECT_EQ(out.indices, in.indices);
}

TEST(Png, FourBitDepth) {
  IndexedImage in{4, 1, 4, {0, 5, 10, 15}, ramp(16)};
  const IndexedImage out = roundTrip(in);
  EXPECT_EQ(out.bitDepth, 4u);
  EXPECT_EQ(out.indices, in.indices);
}

TEST(Png, EightBitDepth) {
  IndexedImage in{3, 1, 8, {0, 128, 255}, ramp(256)};
  const IndexedImage out = roundTrip(in);
  EXPECT_EQ(out.bitDepth, 8u);
  EXPECT_EQ(out.indices, in.indices);
}

TEST(Png, PaletteSurvivesEncode) {
  const std::vector<std::uint8_t> pal = {10, 20, 30, 255, 40, 50, 60, 255,
                                         70, 80, 90, 255, 100, 110, 120, 255};
  IndexedImage in{2, 2, 2, {0, 1, 2, 3}, pal};
  const IndexedImage out = roundTrip(in);
  EXPECT_EQ(out.palette, pal);
}

TEST(Png, TruecolourIsRefused) {
  // Force a real RGB PNG: with auto_convert on, lodepng would optimise a low-colour
  // image down to a palette, which is exactly what this test must not produce.
  std::vector<unsigned char> rgb(4 * 4 * 3, 128);
  lodepng::State state;
  state.info_raw.colortype = LCT_RGB;
  state.info_raw.bitdepth = 8;
  state.info_png.color.colortype = LCT_RGB;
  state.info_png.color.bitdepth = 8;
  state.encoder.auto_convert = 0;
  std::vector<unsigned char> png;
  ASSERT_EQ(lodepng::encode(png, rgb, 4, 4, state), 0u);
  const PngImage back = decodePng(std::vector<std::uint8_t>(png.begin(), png.end()));
  EXPECT_FALSE(back.ok());
  EXPECT_NE(back.error.find("indexed"), std::string::npos);
}

TEST(Png, GarbageIsRefused) {
  const std::vector<std::uint8_t> junk = {1, 2, 3, 4, 5, 6, 7, 8};
  const PngImage back = decodePng(junk);
  EXPECT_FALSE(back.ok());
}

TEST(Png, IndexPastDepthIsRefused) {
  IndexedImage in{2, 1, 1, {0, 5}, ramp(2)};  // index 5 does not fit 1 bit
  const Bytes png = encodePng(in);
  EXPECT_FALSE(png.ok());
}

}  // namespace
}  // namespace snaggletooth::formats
