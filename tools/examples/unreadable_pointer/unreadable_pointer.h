#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A cartridge whose one indirect jump reads its pointer from a register.
inline std::vector<std::uint8_t> unreadablePointerImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0x6Cu, 0x40u, 0x21u,   // $8000 JMP (!$2140)  the APU port is not memory
                     0xDBu});               // $8003 STP
  return rom;
}

}  // namespace snaggletooth::examples
