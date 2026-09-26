#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank, the chipset byte naming a DSP and no save, so the board gives the
// lower halves of its cartridge banks to the chip. Reset reads a byte at
// `$60:1000` — the chip's half, where no image byte is — and stores it, then
// strobes the first controller and clocks one bit out: with B down it calls
// `$60:1000` and jumps long to `$20:6000`, the expansion area; with B up it
// stops. A run with no button down never takes the call or the jump, and the
// machine models no chip in the half, so the read returns open bus. Every site
// is commented with its address.
inline std::vector<std::uint8_t> chipHalfImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  rom[0x7FC0u + 0x16u] = 0x03u;  // the chipset byte: ROM and a DSP
  put(rom, 0x0000u, {
      0xAFu, 0x00u, 0x10u, 0x60u,   // $8000 LDA $60:1000    the chip's half
      0x8Du, 0x00u, 0x02u,          // $8004 STA !$0200
      0xA9u, 0x01u,                 // $8007 LDA #$01
      0x8Du, 0x16u, 0x40u,          // $8009 STA !$4016      strobe high
      0x9Cu, 0x16u, 0x40u,          // $800C STZ !$4016      strobe low
      0xADu, 0x16u, 0x40u,          // $800F LDA !$4016      B
      0x29u, 0x01u,                 // $8012 AND #$01
      0xF0u, 0x08u,                 // $8014 BEQ $801E       B up: stop
      0x22u, 0x00u, 0x10u, 0x60u,   // $8016 JSL $60:1000    into the chip's half
      0x5Cu, 0x00u, 0x60u, 0x20u,   // $801A JML $20:6000    into the expansion area
      0xDBu,                        // $801E STP
  });
  return rom;
}

}  // namespace snaggletooth::examples
