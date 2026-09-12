#pragma once

// An HDMA table as text. A table is a program the DMA engine runs down the frame:
// each entry is a count byte — how many scanlines it lasts, and whether it repeats
// — then either the data itself (direct mode, one transfer unit, or one unit a
// line in repeat mode) or a pointer to it (indirect mode). A zero count ends the
// table. The transfer unit and the mode are the one fact the bytes do not carry,
// so the text states them on its first line: `unit <1|2|4> <direct|indirect>`.
// Each entry is a line `lines <n> [repeat]` followed by its bytes as `$XX` (or a
// `$XXXX` pointer in indirect mode); the terminator is `end`; bytes the file holds
// past the table's end are `$XX` lines.

#include <cstdint>
#include <span>
#include <string>

#include "codec.h"

namespace snaggletooth::formats {

// Encodes an HDMA table read under `unit` (1, 2 or 4) in direct or indirect mode.
// Fails, so the caller keeps the bytes as they are, if an entry's data runs past
// the end of the buffer — the table cannot be read to its end under this unit.
[[nodiscard]] Text encodeHdma(std::span<const std::uint8_t> table, unsigned unit, bool indirect);

// Decodes HDMA text back to bytes. A malformed line, or a unit outside {1,2,4}, is
// an error naming it.
[[nodiscard]] Bytes decodeHdma(const std::string& text);

}  // namespace snaggletooth::formats
