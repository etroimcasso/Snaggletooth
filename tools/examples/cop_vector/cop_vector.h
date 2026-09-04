#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank whose header names the COP vector and the native BRK vector beside
// reset: `cop` and `brk` are mnemonics, so their handlers cannot carry the
// vector's name as a label. Reset stops at once; each handler returns.
inline std::vector<std::uint8_t> copVectorImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x34] = 0x10u;  // cop (emulation) -> $8010
  rom[site + 0x35] = 0x80u;
  rom[site + 0x26] = 0x12u;  // brk (native) -> $8012
  rom[site + 0x27] = 0x80u;
  put(rom, 0x0000u, {0xDBu});  // 8000 STP
  put(rom, 0x0010u, {0x40u});  // 8010 RTI
  put(rom, 0x0012u, {0x40u});  // 8012 RTI
  return rom;
}

}  // namespace snaggletooth::examples
