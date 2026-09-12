// The palette codec: CGRAM words round-trip through text, an odd trailing byte is
// kept, the comment carries the eight-bit colour, and comments are ignored when
// reading.

#include <cstdint>
#include <string>
#include <vector>

#include "formats/palette.h"
#include "gtest/gtest.h"

namespace snaggletooth::formats {
namespace {

std::vector<std::uint8_t> roundTrip(const std::vector<std::uint8_t>& cgram) {
  const Text text = encodePalette(cgram);
  EXPECT_TRUE(text.ok()) << text.error;
  const Bytes back = decodePalette(text.text);
  EXPECT_TRUE(back.ok()) << back.error;
  return back.bytes;
}

TEST(Palette, WordsRoundTrip) {
  const std::vector<std::uint8_t> cgram = {0x00, 0x7F, 0xFF, 0x03, 0x21, 0x00};
  EXPECT_EQ(roundTrip(cgram), cgram);
}

TEST(Palette, OddTrailingByteIsKept) {
  const std::vector<std::uint8_t> cgram = {0x12, 0x34, 0x56};
  EXPECT_EQ(roundTrip(cgram), cgram);
}

TEST(Palette, CommentCarriesEightBitColour) {
  const std::vector<std::uint8_t> white = {0xFF, 0x7F};  // $7FFF, all channels 31
  const Text text = encodePalette(white);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.text.find("$7FFF"), std::string::npos);
  EXPECT_NE(text.text.find("255 255 255"), std::string::npos);  // 5-bit 31 -> 8-bit 255
}

TEST(Palette, CommentsAreIgnoredOnRead) {
  const Bytes back = decodePalette("$0102 ; anything at all\n$00AB\n");
  ASSERT_TRUE(back.ok());
  const std::vector<std::uint8_t> want = {0x02, 0x01, 0xAB, 0x00};
  EXPECT_EQ(back.bytes, want);
}

TEST(Palette, ABadTokenIsAnError) {
  const Bytes back = decodePalette("$0102\nnot-a-value\n");
  EXPECT_FALSE(back.ok());
}

}  // namespace
}  // namespace snaggletooth::formats
