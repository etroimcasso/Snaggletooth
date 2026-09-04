#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank, in emulation mode. Reset enables the vertical-blank NMI and the
// controller auto-read, then waits for twelve interrupts. The handler counts
// them, and on exactly the tenth waits for the auto-read to finish and, if
// Start is down in $4219, sets a flag. After the twelve: if the flag is set the
// program stops; if not it waits forever, one `WAI` per frame. So a Start
// pressed on the one frame the tenth interrupt reads ends the run, and a Start
// pressed a frame earlier or later does not.
inline std::vector<std::uint8_t> framePressImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x81u,                // $8000 LDA #$81           NMI on, auto-read on
      0x8Du, 0x00u, 0x42u,         // $8002 STA !$4200
      0xADu, 0x04u, 0x01u,         // $8005 LDA !$0104         loop: the interrupt count
      0xC9u, 0x0Cu,                // $8008 CMP #$0C
      0xD0u, 0xF9u,                // $800A BNE $8005
      0xADu, 0x20u, 0x00u,         // $800C LDA !$0020         the flag
      0xF0u, 0x01u,                // $800F BEQ $8012
      0xDBu,                       // $8011 STP                seen: stop
      0xCBu,                       // $8012 WAI                not seen: wait, every frame, forever
      0x80u, 0xFDu,                // $8013 BRA $8012
  });
  put(rom, 0x0310u, {
      0xADu, 0x10u, 0x42u,         // $8310 LDA !$4210         acknowledge
      0xEEu, 0x04u, 0x01u,         // $8313 INC !$0104
      0xADu, 0x04u, 0x01u,         // $8316 LDA !$0104
      0xC9u, 0x0Au,                // $8319 CMP #$0A           the tenth?
      0xD0u, 0x11u,                // $831B BNE $832E
      0xADu, 0x12u, 0x42u,         // $831D LDA !$4212         until the auto-read is done
      0x29u, 0x01u,                // $8320 AND #$01
      0xD0u, 0xF9u,                // $8322 BNE $831D
      0xADu, 0x19u, 0x42u,         // $8324 LDA !$4219
      0x29u, 0x10u,                // $8327 AND #$10           Start
      0xF0u, 0x03u,                // $8329 BEQ $832E
      0x8Du, 0x20u, 0x00u,         // $832B STA !$0020         the flag: Start's bit, non-zero
      0x40u,                       // $832E RTI
  });
  return rom;
}

}  // namespace snaggletooth::examples
