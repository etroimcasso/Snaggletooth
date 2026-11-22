#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Three banks. Reset switches to native mode with the accumulator eight bits
// and the index registers sixteen, and jumps long into bank $01. There the
// code loads X with zero and spins on a branch to itself; past the branch, a
// jump through the table at $01:8100 leads to $01:8200, which narrows the
// index registers and jumps long into bank $02. Bank $02 reaches $02:8008 two
// ways: from $02:8000, where the trace arrives from bank $01 with the index
// registers eight bits, and from $02:8004, where the jump through the table at
// $02:8100 leads with them sixteen. The bytes at $02:8008 read `LDX #$EA12`
// under the one width and `LDX #$12` then `NOP` under the other; both streams
// go on at $02:800B.
inline std::vector<std::uint8_t> secondPassImage() {
  std::vector<std::uint8_t> rom = loRomImage(3);
  put(rom, 0x0000u, {0x18u,                          // $00:8000 CLC
                     0xFBu,                          //          XCE
                     0xC2u, 0x30u,                   //          REP #$30
                     0xE2u, 0x20u,                   //          SEP #$20      (m=8, x=16)
                     0x5Cu, 0x00u, 0x80u, 0x01u});   //          JML $01:8000
  put(rom, 0x8000u, {0xA2u, 0x00u, 0x00u,            // $01:8000 LDX #$0000
                     0xF0u, 0xFEu,                   // $01:8003 BEQ $01:8003  (the run spins here)
                     0x7Cu, 0x00u, 0x81u});          // $01:8005 JMP (!$8100,X)
  put(rom, 0x8100u, {0x00u, 0x82u});                 // $01:8100 the table: $01:8200
  put(rom, 0x8200u, {0xE2u, 0x10u,                   // $01:8200 SEP #$10      (x=8)
                     0x5Cu, 0x00u, 0x80u, 0x02u});   //          JML $02:8000
  put(rom, 0x10000u, {0x80u, 0x06u,                  // $02:8000 BRA $02:8008
                      0xEAu, 0xEAu,                  // $02:8002 NOP, NOP
                      0x80u, 0x02u,                  // $02:8004 BRA $02:8008  (the table's target)
                      0xEAu, 0xEAu,                  // $02:8006 NOP, NOP
                      0xA2u, 0x12u, 0xEAu,           // $02:8008 LDX #$EA12, or LDX #$12 and NOP
                      0xC2u, 0x10u,                  // $02:800B REP #$10
                      0xA2u, 0x00u, 0x00u,           // $02:800D LDX #$0000
                      0xF0u, 0xFEu,                  // $02:8010 BEQ $02:8010
                      0x7Cu, 0x00u, 0x81u});         // $02:8012 JMP (!$8100,X)
  put(rom, 0x10100u, {0x04u, 0x80u});                // $02:8100 the table: $02:8004
  return rom;
}

}  // namespace snaggletooth::examples
