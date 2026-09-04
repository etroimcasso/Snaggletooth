#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset arms HDMA on channel 0 — one write of $0F to $2100 at line 1, then
// stop — and idles through a frame counting NMIs before it stops.
inline std::vector<std::uint8_t> hdmaImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,   // $8000 DMAP0 = $00
      0xA9u, 0x00u, 0x8Du, 0x01u, 0x43u,   // $8005 BBAD0 = $00: $2100
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,   // $800A A1T0 low
      0xA9u, 0x81u, 0x8Du, 0x03u, 0x43u,   // $800F A1T0 high: the table at $00:8100
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,   // $8014 A1B0
      0xA9u, 0x01u, 0x8Du, 0x0Cu, 0x42u,   // $8019 HDMAEN = 1
      0xA9u, 0x80u, 0x8Du, 0x00u, 0x42u,   // $801E NMI on
      0xADu, 0x04u, 0x01u,                 // $8023 LDA !$0104
      0xC9u, 0x03u,                        // $8026 CMP #$03
      0xD0u, 0xF9u,                        // $8028 BNE $8023
      0xDBu,                               // $802A STP
  });
  put(rom, 0x0100u, {0x01u, 0x0Fu, 0x00u});  // one line, the value, stop
  put(rom, 0x0310u, {                        // the emulation handler: the program stays in emulation mode
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210
      0xEEu, 0x04u, 0x01u,         // $8313 INC !$0104
      0x40u,                       // $8316 RTI
  });
  return rom;
}

}  // namespace snaggletooth::examples
