#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank and no save, so the save window's lower halves repeat the image as
// every other cartridge bank's do. Reset reads a byte through the window at
// `$70:1300`, sends thirty-two bytes from `$70:1300` to VRAM by DMA, then strobes
// the first controller and clocks one bit out: with B down it calls a routine at
// `$70:1340`, and it stops. With one bank every cartridge bank repeats it, so
// `$70:1300` reads offset `$1300` of the image, which the tree places at
// `$00:9300`, and the routine the call names sits at `$00:9340`. Every site is
// commented with its address.
inline std::vector<std::uint8_t> bareWindowImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0xAFu, 0x00u, 0x13u, 0x70u,          // $8000 LDA $70:1300    through the window: offset $1300 of the image
      0x8Du, 0x00u, 0x02u,                 // $8004 STA !$0200
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,   // $8007 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,   // $800C BBAD0 = $18: VMDATAL
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,   // $8011 A1T0 low
      0xA9u, 0x13u, 0x8Du, 0x03u, 0x43u,   // $8016 A1T0 high: $1300
      0xA9u, 0x70u, 0x8Du, 0x04u, 0x43u,   // $801B A1B0 = $70: the window's bank
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,   // $8020 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $8025 DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,   // $802A MDMAEN = $01 (the write at $802C)
      0xA9u, 0x01u,                        // $802F LDA #$01
      0x8Du, 0x16u, 0x40u,                 // $8031 STA !$4016      strobe high
      0x9Cu, 0x16u, 0x40u,                 // $8034 STZ !$4016      strobe low
      0xADu, 0x16u, 0x40u,                 // $8037 LDA !$4016      B
      0x29u, 0x01u,                        // $803A AND #$01
      0xF0u, 0x04u,                        // $803C BEQ $8042       B up: stop
      0x22u, 0x40u, 0x13u, 0x70u,          // $803E JSL $70:1340    into the window's lower half
      0xDBu,                               // $8042 STP
  });
  for (std::size_t i = 0; i < 32; ++i) rom[0x1300u + i] = static_cast<std::uint8_t>(0x10u + i);  // the tile bytes: $70:1300 through the window, $00:9300 in the tree
  put(rom, 0x1340u, {
      0xEEu, 0x00u, 0x02u,                 // $9340 INC !$0200      the routine the call names, as $70:1340
      0x6Bu,                               // $9343 RTL
  });
  return rom;
}

}  // namespace snaggletooth::examples
