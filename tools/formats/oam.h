#pragma once

// An OAM table as text. The low table is 128 four-byte sprites — X, Y, a 9-bit
// tile number, and attributes (palette, priority, flip). The high table that
// follows holds two bits a sprite: an object-size bit and the ninth X bit. The
// corpus sends these tables a few sprites at a time, so a file is usually a
// partial low table; the text writes each whole sprite as `$XX $YY tile attr`
// (attr = `palette:priority:flags`, flags `H`/`V` or `-`), any leftover low-table
// byte as `$XX`, and, when the file reaches into the high table, each high-table
// byte as four `size:x9` pairs after a blank line — the bytes as they lie, so a
// file that is only part of a table round-trips exactly.

#include <cstdint>
#include <span>
#include <string>

#include "codec.h"

namespace snaggletooth::formats {

// Encodes OAM bytes as text.
[[nodiscard]] Text encodeOam(std::span<const std::uint8_t> oam);

// Decodes OAM text back to bytes. A malformed line is an error naming it.
[[nodiscard]] Bytes decodeOam(const std::string& text);

}  // namespace snaggletooth::formats
