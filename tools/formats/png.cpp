#include "png.h"

#include <array>

#include "lodepng.h"

namespace snaggletooth::formats {

namespace {

// PNG packs sub-byte samples most-significant first, and every scanline starts
// on a byte boundary. This spreads that packing out to one index per pixel.
std::vector<std::uint8_t> unpack(const std::vector<unsigned char>& packed,
                                 unsigned width, unsigned height, unsigned depth) {
  const unsigned mask = (1u << depth) - 1u;
  const unsigned stride = (width * depth + 7u) / 8u;  // bytes a row
  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(width) * height);
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const unsigned bit = x * depth;
      const unsigned byte = y * stride + bit / 8u;
      const unsigned shift = 8u - depth - (bit % 8u);
      out.push_back(static_cast<std::uint8_t>((packed[byte] >> shift) & mask));
    }
  }
  return out;
}

// The reverse: one index per pixel packed to `depth` bits, most-significant
// first, every scanline starting on a byte boundary.
std::vector<unsigned char> pack(const std::vector<std::uint8_t>& indices, unsigned width, unsigned height,
                                unsigned depth) {
  const unsigned stride = (width * depth + 7u) / 8u;
  std::vector<unsigned char> out(static_cast<std::size_t>(stride) * height, 0);
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const unsigned bit = x * depth;
      const unsigned byte = y * stride + bit / 8u;
      const unsigned shift = 8u - depth - (bit % 8u);
      out[byte] = static_cast<unsigned char>(out[byte] | (indices[static_cast<std::size_t>(y) * width + x] << shift));
    }
  }
  return out;
}

}  // namespace

PngImage decodePng(std::span<const std::uint8_t> file) {
  PngImage out;

  lodepng::State state;
  state.decoder.color_convert = 0;  // keep the file's own colour mode and depth

  std::vector<unsigned char> in(file.begin(), file.end());
  std::vector<unsigned char> packed;
  unsigned width = 0;
  unsigned height = 0;
  const unsigned err = lodepng::decode(packed, width, height, state, in);
  if (err) {
    out.error = std::string("not a readable PNG: ") + lodepng_error_text(err);
    return out;
  }

  const LodePNGColorMode& color = state.info_png.color;
  if (color.colortype != LCT_PALETTE) {
    out.error = "PNG colour type " + std::to_string(static_cast<int>(color.colortype)) +
                " is not indexed (palette); the toolkit writes and reads indexed PNGs only";
    return out;
  }
  if (color.bitdepth != 1 && color.bitdepth != 2 && color.bitdepth != 4 && color.bitdepth != 8) {
    out.error = "indexed PNG at bit depth " + std::to_string(color.bitdepth) +
                ", not one of 1, 2, 4, 8";
    return out;
  }

  out.image.width = width;
  out.image.height = height;
  out.image.bitDepth = color.bitdepth;
  out.image.indices = unpack(packed, width, height, color.bitdepth);
  out.image.palette.assign(color.palette, color.palette + color.palettesize * 4);
  return out;
}

Bytes encodePng(const IndexedImage& image) {
  Bytes out;

  if (image.bitDepth != 1 && image.bitDepth != 2 && image.bitDepth != 4 && image.bitDepth != 8) {
    out.error = "cannot write an indexed PNG at bit depth " + std::to_string(image.bitDepth);
    return out;
  }
  const unsigned limit = 1u << image.bitDepth;
  for (std::uint8_t index : image.indices) {
    if (index >= limit) {
      out.error = "pixel index " + std::to_string(index) + " does not fit " +
                  std::to_string(image.bitDepth) + "-bit depth";
      return out;
    }
  }

  // The raw buffer is handed over already packed at the file's depth, with the
  // same palette on both sides, so lodepng copies it as it is: a raw mode that
  // differs from the file's would be converted pixel by pixel through a lookup
  // of each colour in the palette, and a palette with two entries of one colour
  // — a run's palette RAM usually has many — would fold every pixel of the
  // second onto the first. auto_convert is off so the depth is honoured.
  lodepng::State state;
  state.info_raw.colortype = LCT_PALETTE;
  state.info_raw.bitdepth = image.bitDepth;
  state.info_png.color.colortype = LCT_PALETTE;
  state.info_png.color.bitdepth = image.bitDepth;
  state.encoder.auto_convert = 0;

  for (std::size_t i = 0; i + 3 < image.palette.size(); i += 4) {
    lodepng_palette_add(&state.info_raw, image.palette[i], image.palette[i + 1],
                        image.palette[i + 2], image.palette[i + 3]);
    lodepng_palette_add(&state.info_png.color, image.palette[i], image.palette[i + 1],
                        image.palette[i + 2], image.palette[i + 3]);
  }

  const std::vector<unsigned char> raw = pack(image.indices, image.width, image.height, image.bitDepth);
  std::vector<unsigned char> png;
  const unsigned err = lodepng::encode(png, raw, image.width, image.height, state);
  if (err) {
    out.error = std::string("could not encode PNG: ") + lodepng_error_text(err);
    return out;
  }
  out.bytes.assign(png.begin(), png.end());
  return out;
}

}  // namespace snaggletooth::formats
