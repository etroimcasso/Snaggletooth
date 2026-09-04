#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A two-bank cartridge that dispatches through every indirect form, each to a
// target nothing else names. Every site the tests name is commented with its
// address.
//
// Bank $00 (file $0000): reset switches to native mode, enables the vblank NMI,
// copies a `JML [!$8109]` into work RAM and runs it from there, then dispatches
// through a table with X, calls through the same table, jumps through a plain
// pointer, and jumps long through a three-byte pointer into bank $01. Bank $01
// takes one indirect jump sixty-five thousand times — long enough for NMIs to
// land on the jump itself — then stops.
inline std::vector<std::uint8_t> dispatchingImage() {
  std::vector<std::uint8_t> rom = loRomImage(2);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x2A] = 0x00u;  // native NMI -> $8300
  rom[site + 0x2B] = 0x83u;

  // The pointers, at $00:8100.
  put(rom, 0x0100u, {0x00u, 0x82u,          // $8100: -> $8200 (the routine)
                     0x10u, 0x82u,          // $8102: -> $8210 (the table's second target)
                     0x20u, 0x82u,          // $8104: -> $8220
                     0x00u, 0x80u, 0x01u,   // $8106: -> $01:8000 (three bytes)
                     0x30u, 0x82u, 0x00u,   // $8109: -> $00:8230 (three bytes; the copy in RAM jumps here)
                     0x30u, 0x80u});        // $810C: -> $8030 (bank $01's loop, read in bank zero)
  put(rom, 0x0000u, {
                        0x18u, 0xFBu,                // $8000 CLC / XCE      -> native
                        0xE2u, 0x30u,                // $8002 SEP #$30       -> A8, X8
                        0xA9u, 0x80u,                // $8004 LDA #$80
                        0x8Du, 0x00u, 0x42u,         // $8006 STA !$4200     NMI on
                        0xA9u, 0xDCu,                // $8009 LDA #$DC       `JML [!$8109]` -> $7E:2000
                        0x8Fu, 0x00u, 0x20u, 0x7Eu,  // $800B STA $7E:2000
                        0xA9u, 0x09u,                // $800F LDA #$09
                        0x8Fu, 0x01u, 0x20u, 0x7Eu,  // $8011 STA $7E:2001
                        0xA9u, 0x81u,                // $8015 LDA #$81
                        0x8Fu, 0x02u, 0x20u, 0x7Eu,  // $8017 STA $7E:2002
                        0x5Cu, 0x00u, 0x20u, 0x7Eu,  // $801B JML $7E:2000   run the copy
                    });
  put(rom, 0x0230u, {                                // $8230, reached from the copy in RAM
                        0xA2u, 0x02u,                // $8230 LDX #$02
                        0x7Cu, 0x00u, 0x81u,         // $8232 JMP (!$8100,X) -> $8210
                    });
  put(rom, 0x0210u, {                                // $8210
                        0xA2u, 0x00u,                // $8210 LDX #$00
                        0xFCu, 0x00u, 0x81u,         // $8212 JSR (!$8100,X) -> $8200, returns
                        0x6Cu, 0x04u, 0x81u,         // $8215 JMP (!$8104)   -> $8220
                    });
  put(rom, 0x0200u, {0xEAu, 0x60u});                 // $8200 NOP / RTS
  put(rom, 0x0220u, {                                // $8220
                        0xDCu, 0x06u, 0x81u,         // $8220 JML [!$8106]   -> $01:8000
                    });
  put(rom, 0x0300u, {0x40u});                        // $8300 RTI   (the NMI handler)
  // Bank $01 (file $8000). Its first jump is `(!abs,X)` through a table at
  // $01:8100 — the same offset as bank $00's table, which names a different
  // target, so a pointer read in the wrong bank lands in the wrong place.
  put(rom, 0x8000u, {
                        0xA2u, 0x00u,                // $01:8000 LDX #$00
                        0x7Cu, 0x00u, 0x81u,         // $01:8002 JMP (!$8100,X) -> $01:8020
                    });
  put(rom, 0x8100u, {0x20u, 0x80u});                 // $01:8100: -> $8020 (bank $00's $8100 -> $8200)
  put(rom, 0x8020u, {
                        0xC2u, 0x10u,                // $01:8020 REP #$10    -> X16
                        0xA0u, 0xFFu, 0xFFu,         // $01:8022 LDY #$FFFF
                        0x6Cu, 0x0Cu, 0x81u,         // $01:8025 JMP (!$810C) -> $01:8030, every time
                    });
  put(rom, 0x8030u, {
                        0x88u,                       // $01:8030 DEY
                        0xD0u, 0xF2u,                // $01:8031 BNE $01:8025  (-14 from $8033)
                        0xDBu,                       // $01:8033 STP
                    });
  return rom;
}

}  // namespace snaggletooth::examples
