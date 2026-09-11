#pragma once

// The toolkit's PNG face: an indexed-only reader and writer over lodepng. A tile
// sheet is a palette (indexed) image whose pixel values are colour numbers, so
// the face keeps those numbers exactly — it never converts colour. Decoding a
// truecolour, greyscale, alpha or 16-bit PNG is refused, naming the colour type;
// interlacing and filtering are the PNG's own encoding and are read.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "codec.h"

namespace snaggletooth::formats {

// An indexed image: one palette index per pixel in `indices` (row major, width
// entries a row), the palette those indexes name in `palette` (RGBA quadruples,
// `palette.size() / 4` colours), and `bitDepth` — 1, 2, 4 or 8 — the PNG's own
// depth, which bounds the index values.
struct IndexedImage {
  unsigned width = 0;
  unsigned height = 0;
  unsigned bitDepth = 8;
  std::vector<std::uint8_t> indices;
  std::vector<std::uint8_t> palette;
};

// An image decoded from a PNG, or the reason the file could not be one.
struct PngImage {
  IndexedImage image;
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

// Decodes an indexed PNG to its own indexes and palette, at the file's own bit
// depth. A non-indexed or 16-bit image is refused with a message naming the
// colour type and depth.
[[nodiscard]] PngImage decodePng(std::span<const std::uint8_t> file);

// Encodes `image` as an indexed PNG at `image.bitDepth` (1, 2, 4 or 8) carrying
// `image.palette`. Fails only for a depth outside {1,2,4,8} or an index a pixel
// holds that the depth cannot represent.
[[nodiscard]] Bytes encodePng(const IndexedImage& image);

}  // namespace snaggletooth::formats
