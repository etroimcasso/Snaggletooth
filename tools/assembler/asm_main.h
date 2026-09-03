#pragma once

// What the two assembler command lines share: the arguments, the file in, the
// diagnostics out, and the image written. Each command line is one call to
// `assemblerMain` with its dialect.
//
//   <tool> <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
//
// The output is a flat image. Without --base and --size it runs from the first
// byte the source emitted to the last; with them it is the window asked for, and
// a byte the source placed outside it is an error. Gaps hold --fill, $00 by
// default. Numbers are decimal, or hexadecimal with a 0x or $ prefix.

#include "assembler/assembler.h"

namespace snaggletooth::assembler {

// Runs a command line over `dialect`. Returns the process exit code: 0 when the
// image was written, 1 when the source had errors or a file could not be read or
// written, 2 when the arguments were wrong.
[[nodiscard]] int assemblerMain(int argc, char** argv, Dialect& dialect);

}  // namespace snaggletooth::assembler
