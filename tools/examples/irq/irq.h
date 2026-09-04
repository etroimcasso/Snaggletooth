#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank, in emulation mode throughout, with the interrupt-disable flag set
// from power-on. Reset arms the H/V-timer IRQ at line $30 and spins for about
// two frames with the flag still set, so the timer's request latches and the
// line is held asserted but nothing is taken; then waits with `WAI`, which the
// asserted line ends without a dispatch; then clears the flag, and the request
// is taken at once. The handler acknowledges the timer and counts; after two
// the program stops.
inline std::vector<std::uint8_t> irqImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  const std::size_t site = 0x7FC0u;
  rom[site + 0x3E] = 0x20u;  // emulation IRQ -> $8320
  rom[site + 0x3F] = 0x83u;
  put(rom, 0x0000u, {
      0xA9u, 0x30u,                // $8000 LDA #$30
      0x8Du, 0x09u, 0x42u,         // $8002 STA !$4209         VTIME low: line $30
      0x9Cu, 0x0Au, 0x42u,         // $8005 STZ !$420A         VTIME high
      0xA9u, 0x20u,                // $8008 LDA #$20
      0x8Du, 0x00u, 0x42u,         // $800A STA !$4200         V=V IRQ on; I is still set
      0xA0u, 0x40u,                // $800D LDY #$40
      0xA2u, 0x00u,                // $800F LDX #$00           outer: two frames of spinning
      0xCAu,                       // $8011 DEX                inner
      0xD0u, 0xFDu,                // $8012 BNE $8011
      0x88u,                       // $8014 DEY
      0xD0u, 0xF8u,                // $8015 BNE $800F
      0xCBu,                       // $8017 WAI                the asserted line ends it, masked
      0x58u,                       // $8018 CLI                and now the request is taken
      0xADu, 0x06u, 0x01u,         // $8019 LDA !$0106         loop: the handler's count
      0xC9u, 0x02u,                // $801C CMP #$02
      0xD0u, 0xF9u,                // $801E BNE $8019
      0xDBu,                       // $8020 STP
  });
  put(rom, 0x0320u, {
      0xADu, 0x11u, 0x42u,         // $8320 LDA !$4211         acknowledge the timer
      0xEEu, 0x06u, 0x01u,         // $8323 INC !$0106
      0x40u,                       // $8326 RTI
  });
  return rom;
}

}  // namespace snaggletooth::examples
