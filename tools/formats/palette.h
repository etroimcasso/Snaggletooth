#pragma once

// A palette as text. CGRAM holds one 15-bit colour per word — five bits each of
// blue, green and red, low to high — so the text writes each word verbatim as a
// hexadecimal number and, as a comment a person can read, its colour at eight
// bits a channel. The number is the data; the comment is not read back, so a
// person may recolour by editing the word. A run of an odd number of bytes ends
// with its lone byte.

#include <cstdint>
#include <span>

#include "codec.h"

namespace snaggletooth::formats {

// Encodes CGRAM bytes as palette text: one `$XXXX` word a line with `; r g b` at
// eight bits, a trailing `$XX` line for an odd last byte.
[[nodiscard]] Text encodePalette(std::span<const std::uint8_t> cgram);

// Decodes palette text back to CGRAM bytes. Comments and blank lines are ignored;
// each `$XXXX` is a little-endian word, each `$XX` a single byte. A line that is
// neither is an error naming the line.
[[nodiscard]] Bytes decodePalette(const std::string& text);

}  // namespace snaggletooth::formats
