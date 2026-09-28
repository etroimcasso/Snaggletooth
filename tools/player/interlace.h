#pragma once

// Weaving the machine's fields into what the player shows.
//
// The machine draws one field a frame — half the interlaced picture's lines,
// each with its parity (`snaggletooth/snes/video_frame.h`) — and leaves the
// weaving to whoever is watching. A run the cartridge did not interlace hands
// over a whole picture a frame, which this passes straight through. A run it did
// interlace hands over alternate parities, which this composes into the picture
// the console's signal carried, three ways:
//
//   Weave — the two fields interleaved, field F's line i at row 2i+F, for the
//           full-height picture. Sharp, and combs where the two fields differ
//           under motion. It is what the console drew, and the default.
//   Bob   — each field at its own vertical position with the gap filled from
//           itself, a new picture every field. No combing; a shimmer where the
//           fields differ.
//   Off   — the field as it arrives, half height: the machine's own output.
//
// It holds the composed picture between fields, so a weave keeps the parity it
// is not handed this frame. It opens no device and reads no clock — a field goes
// in and the pixels to show come out — so the suite checks it. Only an
// interlaced run is composed; a progressive one is shown as it arrives whatever
// the mode, since there is nothing to weave.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace snaggletooth::player {

enum class Interlace : std::uint8_t { Weave, Bob, Off };

// The pixels to show and their shape. The pointer is the weaver's own buffer (or
// the field passed in, when a run is not composed); it is valid until the next
// present().
struct Shown {
  const std::uint8_t* pixels;
  unsigned width;
  unsigned height;
};

class FieldWeaver {
 public:
  explicit FieldWeaver(Interlace mode) noexcept : mode_(mode) {}

  // One field the machine finished: `pixels` is width*height*4 bytes, RGBA, top
  // line first, and `field` is its parity. `interlaced` is whether the cartridge
  // has interlace on now. A progressive run, or the Off mode, shows the field
  // unchanged; otherwise the field is composed into the full-height picture and
  // that is returned.
  [[nodiscard]] Shown present(const std::uint8_t* pixels, unsigned width, unsigned height,
                              std::uint8_t field, bool interlaced) {
    if (!interlaced || mode_ == Interlace::Off) {
      return Shown{.pixels = pixels, .width = width, .height = height};
    }
    const unsigned tall = height * 2u;
    // A shape change rebuilds the held picture; what the other parity last wrote
    // is dropped and the next field of that parity fills it again.
    if (width != width_ || tall != height_) {
      width_ = width;
      height_ = tall;
      buffer_.assign(static_cast<std::size_t>(width_) * height_ * 4u, 0u);
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    const unsigned parity = field & 1u;
    for (unsigned i = 0; i < height; ++i) {
      const std::uint8_t* src = pixels + static_cast<std::size_t>(i) * rowBytes;
      std::memcpy(rowAt(2u * i + parity), src, rowBytes);
      // Bob fills the other parity's row from this field, so each field is a
      // whole picture at its own vertical place; weave leaves it for the field
      // of that parity to write.
      if (mode_ == Interlace::Bob) std::memcpy(rowAt(2u * i + (parity ^ 1u)), src, rowBytes);
    }
    return Shown{.pixels = buffer_.data(), .width = width_, .height = height_};
  }

 private:
  [[nodiscard]] std::uint8_t* rowAt(unsigned row) noexcept {
    return buffer_.data() + static_cast<std::size_t>(row) * width_ * 4u;
  }

  Interlace mode_;
  std::vector<std::uint8_t> buffer_;
  unsigned width_ = 0;
  unsigned height_ = 0;
};

// The mode a name asks for, or nothing for a name that is none of them.
[[nodiscard]] inline std::optional<Interlace> parseInterlace(std::string_view name) noexcept {
  if (name == "weave") return Interlace::Weave;
  if (name == "bob") return Interlace::Bob;
  if (name == "off") return Interlace::Off;
  return std::nullopt;
}

}  // namespace snaggletooth::player
