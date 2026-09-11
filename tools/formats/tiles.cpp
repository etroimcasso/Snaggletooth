#include "tiles.h"

#include <algorithm>
#include <string>

namespace snaggletooth::formats {

namespace {

constexpr unsigned kTileWidth = 8;
constexpr unsigned kTileHeight = 8;
constexpr unsigned kTilesPerRow = 16;

// The byte within a tile that holds bit-plane `plane` of pixel row `row`: planes
// 0 and 1 interleave in the first sixteen bytes, 2 and 3 in the next sixteen,
// and so on, two bytes a row.
constexpr unsigned planeRowByte(unsigned plane, unsigned row) {
  return (plane / 2u) * 16u + (plane % 2u) + row * 2u;
}

// One tile's bytes to its 8x8 indices. In each plane byte bit 7 is the left-most
// pixel; plane p carries bit p of the colour number.
void tileToIndices(std::span<const std::uint8_t> tile, unsigned depth,
                   std::vector<std::uint8_t>& out, std::size_t base, unsigned stride) {
  for (unsigned row = 0; row < kTileHeight; ++row) {
    for (unsigned x = 0; x < kTileWidth; ++x) {
      unsigned index = 0;
      for (unsigned plane = 0; plane < depth; ++plane) {
        const std::uint8_t byte = tile[planeRowByte(plane, row)];
        index |= static_cast<unsigned>((byte >> (7u - x)) & 1u) << plane;
      }
      out[base + row * stride + x] = static_cast<std::uint8_t>(index);
    }
  }
}

// The reverse: 8x8 indices from a sheet back to one tile's bytes.
void indicesToTile(const std::vector<std::uint8_t>& indices, std::size_t base,
                   unsigned stride, unsigned depth, std::vector<std::uint8_t>& tile) {
  for (unsigned row = 0; row < kTileHeight; ++row) {
    for (unsigned x = 0; x < kTileWidth; ++x) {
      const unsigned index = indices[base + row * stride + x];
      for (unsigned plane = 0; plane < depth; ++plane) {
        if ((index >> plane) & 1u) {
          tile[planeRowByte(plane, row)] |= static_cast<std::uint8_t>(1u << (7u - x));
        }
      }
    }
  }
}

}  // namespace

Bytes encodeTiles(std::span<const std::uint8_t> planar, unsigned depth,
                  const std::vector<std::uint8_t>& palette) {
  Bytes out;
  if (depth != 2 && depth != 4 && depth != 8) {
    out.error = "cannot encode tiles at depth " + std::to_string(depth) + " (2, 4 or 8)";
    return out;
  }

  const unsigned bytesPerTile = depth * kTileHeight;
  const unsigned tileCount =
      static_cast<unsigned>((planar.size() + bytesPerTile - 1) / bytesPerTile);
  const unsigned rows = (tileCount + kTilesPerRow - 1) / kTilesPerRow;

  IndexedImage image;
  image.bitDepth = depth;
  image.width = kTilesPerRow * kTileWidth;
  image.height = rows * kTileHeight;
  image.indices.assign(static_cast<std::size_t>(image.width) * image.height, 0);
  image.palette = palette;

  std::vector<std::uint8_t> tile(bytesPerTile, 0);
  for (unsigned t = 0; t < tileCount; ++t) {
    std::fill(tile.begin(), tile.end(), std::uint8_t{0});
    const std::size_t start = static_cast<std::size_t>(t) * bytesPerTile;
    for (unsigned b = 0; b < bytesPerTile && start + b < planar.size(); ++b) {
      tile[b] = planar[start + b];
    }
    const unsigned tileX = t % kTilesPerRow;
    const unsigned tileY = t / kTilesPerRow;
    const std::size_t base =
        static_cast<std::size_t>(tileY) * kTileHeight * image.width + tileX * kTileWidth;
    tileToIndices(tile, depth, image.indices, base, image.width);
  }

  return encodePng(image);
}

Bytes decodeTiles(std::span<const std::uint8_t> png) {
  Bytes out;
  PngImage decoded = decodePng(png);
  if (!decoded.ok()) {
    out.error = decoded.error;
    return out;
  }
  const IndexedImage& image = decoded.image;
  if (image.width % kTileWidth != 0) {
    out.error = "tile sheet width " + std::to_string(image.width) + " is not a multiple of 8";
    return out;
  }
  if (image.height % kTileHeight != 0) {
    out.error = "tile sheet height " + std::to_string(image.height) + " is not a multiple of 8";
    return out;
  }

  const unsigned depth = image.bitDepth;
  const unsigned bytesPerTile = depth * kTileHeight;
  const unsigned tilesPerRow = image.width / kTileWidth;
  const unsigned tileRows = image.height / kTileHeight;
  const unsigned tileCount = tilesPerRow * tileRows;

  out.bytes.reserve(static_cast<std::size_t>(tileCount) * bytesPerTile);
  std::vector<std::uint8_t> tile(bytesPerTile, 0);
  for (unsigned t = 0; t < tileCount; ++t) {
    std::fill(tile.begin(), tile.end(), std::uint8_t{0});
    const unsigned tileX = t % tilesPerRow;
    const unsigned tileY = t / tilesPerRow;
    const std::size_t base =
        static_cast<std::size_t>(tileY) * kTileHeight * image.width + tileX * kTileWidth;
    indicesToTile(image.indices, base, image.width, depth, tile);
    out.bytes.insert(out.bytes.end(), tile.begin(), tile.end());
  }
  return out;
}

}  // namespace snaggletooth::formats
