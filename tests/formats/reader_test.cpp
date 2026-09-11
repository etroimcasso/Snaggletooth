// The encoding reader: an included asset is decoded to its SNES bytes by
// extension, a `.bin` (or any other extension) passes through unchanged, a file
// that does not decode reports its reason and reads as nothing, and a file the
// inner reader cannot find is nothing with no reason.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "formats/palette.h"
#include "formats/reader.h"
#include "formats/tiles.h"
#include "gtest/gtest.h"

namespace snaggletooth::formats {
namespace {

std::string bytesOf(const std::vector<std::uint8_t>& v) { return std::string(v.begin(), v.end()); }

// An inner reader answering from a fixed set of files.
assembler::Reader fixed(std::map<std::string, std::string> files) {
  return [files = std::move(files)](const std::string& path) -> std::optional<std::string> {
    const auto it = files.find(path);
    if (it == files.end()) return std::nullopt;
    return it->second;
  };
}

std::vector<std::uint8_t> palette4() {
  std::vector<std::uint8_t> p;
  for (int i = 0; i < 4; ++i) p.insert(p.end(), {0, 0, 0, 255});
  return p;
}

TEST(Reader, PngIncludeDecodesToTileBytes) {
  const std::vector<std::uint8_t> planar = {0x80, 0x40, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 0};
  const Bytes png = encodeTiles(planar, 2, palette4());
  ASSERT_TRUE(png.ok());
  const Bytes expected = decodeTiles(png.bytes);
  ASSERT_TRUE(expected.ok());

  auto reader = encodingReader(fixed({{"t.png", bytesOf(png.bytes)}}));
  const auto out = reader("t.png");
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, bytesOf(expected.bytes));
}

TEST(Reader, PalIncludeDecodesToCgramBytes) {
  const std::vector<std::uint8_t> cgram = {0x00, 0x7F, 0xFF, 0x03};
  const Text pal = encodePalette(cgram);
  auto reader = encodingReader(fixed({{"c.pal", pal.text}}));
  const auto out = reader("c.pal");
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, bytesOf(cgram));
}

TEST(Reader, MapAndOamAndHdmaDecodeToBytes) {
  auto reader = encodingReader(fixed({
      {"m.map", "0123:2:PH\n"},
      {"s.oam", "$10 $20 055 0:0:-\n"},
      {"h.hdma", "unit 2 direct\nlines 1 $AA $BB\nend\n"},
  }));
  EXPECT_TRUE(reader("m.map").has_value());
  EXPECT_TRUE(reader("s.oam").has_value());
  EXPECT_TRUE(reader("h.hdma").has_value());
}

TEST(Reader, BinPassesThrough) {
  const std::string raw = "\x01\x02\x03\xFF";
  auto reader = encodingReader(fixed({{"blob.bin", raw}}));
  const auto out = reader("blob.bin");
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, raw);
}

TEST(Reader, UnknownExtensionPassesThrough) {
  const std::string raw = "abc";
  auto reader = encodingReader(fixed({{"notes.txt", raw}}));
  const auto out = reader("notes.txt");
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, raw);
}

TEST(Reader, BadAssetReportsAReasonAndReadsAsNothing) {
  std::string reason;
  auto reader = encodingReader(fixed({{"bad.png", "not a png at all"}}),
                               [&](const std::string& m) { reason = m; });
  const auto out = reader("bad.png");
  EXPECT_FALSE(out.has_value());
  EXPECT_NE(reason.find("bad.png"), std::string::npos);
}

TEST(Reader, MissingFileIsNothingWithNoReason) {
  bool called = false;
  auto reader = encodingReader(fixed({}), [&](const std::string&) { called = true; });
  const auto out = reader("gone.png");
  EXPECT_FALSE(out.has_value());
  EXPECT_FALSE(called);  // the inner reader missed; the codec never ran
}

}  // namespace
}  // namespace snaggletooth::formats
