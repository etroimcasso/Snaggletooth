#pragma once

// The encoding reader: the seam by which the assembler includes an asset. The
// assembler reads a file's bytes through an `assembler::Reader`; this wraps one so
// that, by the path's extension, an asset file is turned back into the SNES bytes
// it was made from before the assembler places them — a `.png` to tile bytes, a
// `.pal` to CGRAM words, a `.map`, `.oam` or `.hdma` to its table. Every other
// extension passes through unchanged, so a `.bin` include is the bytes on disk.
//
// A file that does not decode — a `.png` that is not indexed, a line that does not
// parse — reports its reason through the error sink (if one is given) and reads as
// nothing, so the assembler names the include unreadable.

#include <functional>
#include <optional>
#include <string>

#include "assembler/assembler.h"

namespace snaggletooth::formats {

// Called with a one-line reason when an asset the reader was asked for does not
// decode.
using ErrorSink = std::function<void(const std::string&)>;

// Wraps `inner` so an included asset is decoded to its SNES bytes by extension.
[[nodiscard]] assembler::Reader encodingReader(assembler::Reader inner, ErrorSink onError = {});

}  // namespace snaggletooth::formats
