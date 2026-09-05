#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank, in emulation mode until its last few instructions. Reset writes a
// two-instruction routine — `INC A` and `RTL` — into work RAM, calls it there
// and stores the result; then rewrites its first byte to `DEC A`, points the
// direct register at the DMA channels, builds two return frames on the stack
// and enters the routine with `RTL`; the routine's own `RTL` lands on a store
// the bytes never name a path to. That store heads a loop run twice, which
// moves the direct register on each pass and lands past a `PEA`/`RTS` pair
// each time. After the loop a direct-page store lands on a channel register
// under the one direct register the run saw there, and an indexed one beside
// it; then reset goes native, builds a frame with both widths sixteen bits and
// returns through `RTI` into the stop. Every site the tests name is commented
// with its address.
inline std::vector<std::uint8_t> ramCodeImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x1Au,                // $8000 LDA #$1A           INC A
      0x8Fu, 0x00u, 0x20u, 0x7Eu,  // $8002 STA $7E:2000
      0xA9u, 0x6Bu,                // $8006 LDA #$6B           RTL
      0x8Fu, 0x01u, 0x20u, 0x7Eu,  // $8008 STA $7E:2001
      0x22u, 0x00u, 0x20u, 0x7Eu,  // $800C JSL $7E:2000       the routine, called
      0x8Du, 0x00u, 0x01u,         // $8010 STA !$0100         $1B
      0xA9u, 0x3Au,                // $8013 LDA #$3A           DEC A: the routine's first byte rewritten
      0x8Fu, 0x00u, 0x20u, 0x7Eu,  // $8015 STA $7E:2000
      0xF4u, 0x00u, 0x43u,         // $8019 PEA $4300
      0x2Bu,                       // $801C PLD                D = $4300, the DMA channels; every path proves it
      0x85u, 0x04u,                // $801D STA $04            A1B0, under the proven direct register
      0xA9u, 0x00u,                // $801F LDA #$00
      0x48u,                       // $8021 PHA                the bank the routine returns to
      0xF4u, 0x2Bu, 0x80u,         // $8022 PEA $802B          and the address, less one
      0xA9u, 0x7Eu,                // $8025 LDA #$7E
      0x48u,                       // $8027 PHA                the routine's bank
      0xF4u, 0xFFu, 0x1Fu,         // $8028 PEA $1FFF          its address, less one
      0x6Bu,                       // $802B RTL                -> $7E:2000, a landing in work RAM
                                   //                          DEC A ; RTL -> $802C, a landing the bytes do not name
      0x8Du, 0x02u, 0x01u,         // $802C STA !$0102         run under D = $4300, then D = $4310
      0x85u, 0x00u,                // $802F STA $00            DMAP0 on the first pass, DMAP1 on the second
      0x7Bu,                       // $8031 TDC
      0x18u,                       // $8032 CLC
      0x69u, 0x10u,                // $8033 ADC #$10
      0x5Bu,                       // $8035 TCD                D += $10
      0xF4u, 0x39u, 0x80u,         // $8036 PEA $8039
      0x60u,                       // $8039 RTS                -> $803A, on both passes
      0xC8u,                       // $803A INY
      0xC0u, 0x02u,                // $803B CPY #$02
      0xD0u, 0xEDu,                // $803D BNE $802C          twice round
      0x85u, 0x01u,                // $803F STA $01            BBAD2, under the one direct register the run saw: $4320
      0x95u, 0x03u,                // $8041 STA $03,X          an indexed form, X = 0
      0x18u,                       // $8043 CLC
      0xFBu,                       // $8044 XCE                -> native, both widths eight
      0xA9u, 0x00u,                // $8045 LDA #$00
      0x48u,                       // $8047 PHA                the bank the RTI lands in
      0xF4u, 0x4Du, 0x80u,         // $8048 PEA $804D          the address, exactly
      0x48u,                       // $804B PHA                the status byte: both widths sixteen
      0x40u,                       // $804C RTI                -> $804D, arriving with m=16 x=16
      0xDBu,                       // $804D STP
  });
  return rom;
}

}  // namespace snaggletooth::examples
