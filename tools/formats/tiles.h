#pragma once

// Tiles as an indexed PNG. An 8x8 tile occupies 16, 32 or 64 bytes for 2, 4 or 8
// bits per pixel, the colour number of each pixel spread across the bit-planes
// the way the PPU reads them. The sheet is sixteen tiles wide, in file order left
// to right and down; the PNG's bit depth is the tile depth, so the file says its
// own depth and a person's editor shows the right number of colours.
//
// A file that is not a whole number of tiles — most are not — has its last tile
// padded with zero pixels, and the bank file's `INCBIN` carries the byte length,
// so the padding past the file's end is never assembled.

#include <cstdint>
#include <span>
#include <vector>

#include "codec.h"
#include "png.h"

namespace snaggletooth::formats {

// Encodes `planar` — SNES tile bytes at `depth` (2, 4 or 8) — as an indexed PNG
// sixteen tiles wide carrying `palette` (RGBA quadruples). A trailing partial
// tile is zero-padded to a whole tile.
[[nodiscard]] Bytes encodeTiles(std::span<const std::uint8_t> planar, unsigned depth,
                                const std::vector<std::uint8_t>& palette);

// Decodes an indexed PNG back to SNES tile bytes at the PNG's own depth, tiles in
// row-major order. The result runs to a whole number of tiles; a bank file clips
// it to the original length. Fails if the PNG's width or height is not a multiple
// of eight, naming the dimension.
[[nodiscard]] Bytes decodeTiles(std::span<const std::uint8_t> png);

}  // namespace snaggletooth::formats
