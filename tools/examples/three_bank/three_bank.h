#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Three banks. Reset switches to native mode with both widths sixteen, calls a
// routine in bank $01, and jumps to bank $02 through its mirror at $82. The
// bank-$01 routine calls back into bank $00, at an address only it reaches. The
// bank-$02 code narrows the accumulator, calls into work RAM, and ends in a
// jump through a table; the table's one target sits at $02:8100.
inline std::vector<std::uint8_t> threeBankImage() {
  std::vector<std::uint8_t> rom = loRomImage(3);
  put(rom, 0x0000u, {0x18u,                       // CLC
                     0xFBu,                       // XCE
                     0xC2u, 0x30u,                // REP #$30
                     0x22u, 0x00u, 0x80u, 0x01u,  // JSL $01:8000
                     0x5Cu, 0x00u, 0x80u, 0x82u});  // JML $82:8000
  put(rom, 0x0100u, {0xEAu,    // $00:8100 NOP   (reached only from bank $01)
                     0x6Bu});  //          RTL
  put(rom, 0x8000u, {0xA9u, 0x34u, 0x12u,          // LDA #$1234
                     0x22u, 0x00u, 0x81u, 0x00u,   // JSL $00:8100
                     0x6Bu});                      // RTL
  put(rom, 0x10000u, {0xE2u, 0x20u,                // SEP #$20
                      0xA9u, 0x12u,                // LDA #$12
                      0x22u, 0x00u, 0x20u, 0x7Eu,  // JSL $7E:2000
                      0x7Cu, 0x00u, 0x81u});       // JMP (!$8100,X)
  put(rom, 0x10100u, {0xEAu,          // NOP        (reached only through the table)
                      0x00u, 0x01u,   // BRK #$01   (continues at the BRK vector's handler)
                      0x6Bu});        // RTL
  return rom;
}

}  // namespace snaggletooth::examples
