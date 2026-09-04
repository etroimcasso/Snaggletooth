#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank. Reset enables the vertical-blank NMI and the auto-read and idles. The
// NMI handler waits for the auto-read to finish, and if Start is down in $4219
// jumps through the pointer at $8100 to $8200; otherwise it strobes the serial
// port, clocks nine bits out — the ninth is A — and if A is down jumps through the
// pointer at $8102 to $8210. Neither target is named by any instruction.
inline std::vector<std::uint8_t> buttonDispatchImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x3A] = 0x00u;  // emulation NMI -> $8300
  rom[site + 0x3B] = 0x83u;
  put(rom, 0x0100u, {0x00u, 0x82u,     // $8100: -> $8200
                     0x10u, 0x82u});   // $8102: -> $8210
  put(rom, 0x0000u, {
                        0xA9u, 0x81u,                // $8000 LDA #$81       NMI on, auto-read on
                        0x8Du, 0x00u, 0x42u,         // $8002 STA !$4200
                        0x80u, 0xFEu,                // $8005 BRA $8005      idle
                    });
  put(rom, 0x0300u, {
                        0xADu, 0x10u, 0x42u,         // $8300 LDA !$4210     acknowledge
                        0xADu, 0x12u, 0x42u,         // $8303 LDA !$4212
                        0x29u, 0x01u,                // $8306 AND #$01
                        0xD0u, 0xF9u,                // $8308 BNE $8303      until the auto-read is done
                        0xADu, 0x19u, 0x42u,         // $830A LDA !$4219
                        0x29u, 0x10u,                // $830D AND #$10       Start
                        0xF0u, 0x03u,                // $830F BEQ $8314
                        0x6Cu, 0x00u, 0x81u,         // $8311 JMP (!$8100)   -> $8200
                        0xA9u, 0x01u,                // $8314 LDA #$01
                        0x8Du, 0x16u, 0x40u,         // $8316 STA !$4016     strobe high
                        0x9Cu, 0x16u, 0x40u,         // $8319 STZ !$4016     strobe low
                        0xADu, 0x16u, 0x40u,         // $831C LDA !$4016     B
                        0xADu, 0x16u, 0x40u,         // $831F                Y
                        0xADu, 0x16u, 0x40u,         // $8322                Select
                        0xADu, 0x16u, 0x40u,         // $8325                Start
                        0xADu, 0x16u, 0x40u,         // $8328                Up
                        0xADu, 0x16u, 0x40u,         // $832B                Down
                        0xADu, 0x16u, 0x40u,         // $832E                Left
                        0xADu, 0x16u, 0x40u,         // $8331                Right
                        0xADu, 0x16u, 0x40u,         // $8334 LDA !$4016     A
                        0x29u, 0x01u,                // $8337 AND #$01
                        0xF0u, 0x03u,                // $8339 BEQ $833E
                        0x6Cu, 0x02u, 0x81u,         // $833B JMP (!$8102)   -> $8210
                        0x40u,                       // $833E RTI
                    });
  put(rom, 0x0200u, {0xA9u, 0x01u, 0x8Du, 0x20u, 0x00u, 0x40u});  // $8200 LDA #$01 / STA $0020 / RTI
  put(rom, 0x0210u, {0xA9u, 0x02u, 0x8Du, 0x21u, 0x00u, 0x40u});  // $8210 LDA #$02 / STA $0021 / RTI
  return rom;
}

}  // namespace snaggletooth::examples
