#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank. Reset switches to native mode and runs a spread of the effect
// layer's constructs — both widths, a direct-page access, a sixteen-bit
// read-modify-write, an indexed read, the stack, a block move, a call and a
// return — then enables the vertical-blank NMI and counts three of them before
// it stops. The NMI handler acknowledges the flag and counts.
inline std::vector<std::uint8_t> mixedImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC ; XCE          -> native
      0xC2u, 0x30u,                // $8002 REP #$30           -> A16 X16
      0xA2u, 0x02u, 0x00u,         // $8004 LDX #$0002
      0xA9u, 0x34u, 0x12u,         // $8007 LDA #$1234
      0x8Du, 0x00u, 0x01u,         // $800A STA !$0100
      0xEEu, 0x00u, 0x01u,         // $800D INC !$0100         a 16-bit read-modify-write
      0xE2u, 0x20u,                // $8010 SEP #$20           -> A8
      0xA9u, 0x05u,                // $8012 LDA #$05
      0x85u, 0x10u,                // $8014 STA $10            direct page
      0xBDu, 0xFFu, 0xFFu,         // $8016 LDA !$FFFF,X       indexed read, crossing into bank $01
      0x48u,                       // $8019 PHA
      0x68u,                       // $801A PLA
      0x20u, 0x40u, 0x80u,         // $801B JSR !$8040
      0xA9u, 0x80u,                // $801E LDA #$80
      0x8Du, 0x00u, 0x42u,         // $8020 STA !$4200         NMI on
      0xEEu, 0x02u, 0x01u,         // $8023 INC !$0102         loop: a counter the handler is not touching
      0xADu, 0x04u, 0x01u,         // $8026 LDA !$0104         the handler's count
      0xC9u, 0x03u,                // $8029 CMP #$03
      0xD0u, 0xF6u,                // $802B BNE $8023
      0xDBu,                       // $802D STP
  });
  put(rom, 0x0040u, {
      0xC2u, 0x20u,                // $8040 REP #$20           -> A16
      0xA9u, 0x01u, 0x00u,         // $8042 LDA #$0001         two bytes
      0xA2u, 0x00u, 0x81u,         // $8045 LDX #$8100
      0xA0u, 0x00u, 0x02u,         // $8048 LDY #$0200
      0x54u, 0x7Eu, 0x00u,         // $804B MVN $00,$7E        $00:8100.. -> $7E:0200..
      0xE2u, 0x20u,                // $804E SEP #$20
      0x4Bu, 0xABu,                // $8050 PHK ; PLB          the data bank back from $7E
      0x60u,                       // $8052 RTS
  });
  put(rom, 0x0100u, {0xAAu, 0xBBu});  // the bytes the block move copies
  put(rom, 0x0300u, {
      0xADu, 0x10u, 0x42u,         // $8300 LDA !$4210         acknowledge
      0xEEu, 0x04u, 0x01u,         // $8303 INC !$0104
      0x40u,                       // $8306 RTI
  });
  return rom;
}

}  // namespace snaggletooth::examples
