#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset copies a two-instruction routine into work RAM, calls it there, and
// stops: the routine's address has no node, because the trace never reaches it.
inline std::vector<std::uint8_t> ramCodeImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x1Au,                // $8000 LDA #$1A           INC A
      0x8Fu, 0x00u, 0x20u, 0x7Eu,  // $8002 STA $7E:2000
      0xA9u, 0x6Bu,                // $8006 LDA #$6B           RTL
      0x8Fu, 0x01u, 0x20u, 0x7Eu,  // $8008 STA $7E:2001
      0x22u, 0x00u, 0x20u, 0x7Eu,  // $800C JSL $7E:2000
      0x8Du, 0x00u, 0x01u,         // $8010 STA !$0100
      0xDBu,                       // $8013 STP
  });
  return rom;
}

}  // namespace snaggletooth::examples
