#pragma once

// A tilemap as text. A BG map entry is a 16-bit word: a 10-bit tile number, a
// 3-bit palette, and priority, horizontal-flip and vertical-flip bits. The text
// writes each entry as `tile:palette:flags` — the tile in hexadecimal, the
// palette 0-7, the flags `P` (priority), `H`, `V` in that order or `-` for none.
// Thirty-two entries a line make one screen row; a blank line follows every
// thirty-two rows, one screen. An odd trailing byte ends with a `$XX` line.

#include <cstdint>
#include <span>
#include <string>

#include "codec.h"

namespace snaggletooth::formats {

// Encodes tilemap bytes as text.
[[nodiscard]] Text encodeTilemap(std::span<const std::uint8_t> map);

// Decodes tilemap text back to bytes. An entry out of range (tile over 3FF,
// palette over 7, an unknown flag letter) is an error naming it.
[[nodiscard]] Bytes decodeTilemap(const std::string& text);

}  // namespace snaggletooth::formats
