#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset enables the NMI, then waits for it under a masked IRQ line; the
// handler counts; after two the program stops.
inline std::vector<std::uint8_t> waitingImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x80u,                // $8000 LDA #$80
      0x8Du, 0x00u, 0x42u,         // $8002 STA !$4200         NMI on
      0xCBu,                       // $8005 WAI
      0xADu, 0x04u, 0x01u,         // $8006 LDA !$0104
      0xC9u, 0x02u,                // $8009 CMP #$02
      0xD0u, 0xF8u,                // $800B BNE $8005
      0xDBu,                       // $800D STP
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210
      0xEEu, 0x04u, 0x01u,         // $8313 INC !$0104
      0x40u,                       // $8316 RTI
  });
  return rom;
}

}  // namespace snaggletooth::examples
