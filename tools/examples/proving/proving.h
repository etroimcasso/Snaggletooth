#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank. Reset proves each register the analysis follows the way real code
// does — `LDX #`/`TXS` for the stack pointer, `LDA #`/`TCD` for the direct
// register, `PEA`/`PLB`/`PLB` and `PHK`/`PLB` for the data bank — writes a DMA
// channel through the direct page while the direct register points at it, calls
// a routine that saves and restores the accumulator around its own work, and
// dispatches through three tables: one whose index a mask bounds, one whose
// index a compare and a branch on the carry bound, and one nothing bounds. Two
// of the first table's targets set the direct register to different values and
// meet; the rest agree, and the last breaks to the software-interrupt vector on
// its way. One of the second table's targets calls a routine that jumps through a
// pointer in work RAM, which the analysis cannot follow, another calls the
// saving routine a second time with a different accumulator, and a third jumps
// straight to the label after the unfollowable call. The NMI
// handler saves and restores the data bank and writes the screen through the
// direct page, whose register it never proves. Every address the cases name is
// commented.
inline std::vector<std::uint8_t> provingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x3A] = 0x00u;  // emulation NMI -> $8600
  rom[site + 0x3B] = 0x86u;

  put(rom, 0x0000u, {
                        0x18u,                     // $8000 CLC
                        0xFBu,                     // $8001 XCE            -> native
                        0xC2u, 0x30u,              // $8002 REP #$30       -> A16 X16
                        0xA2u, 0xFFu, 0x1Fu,       // $8004 LDX #$1FFF
                        0x9Au,                     // $8007 TXS            S = $1FFF
                        0xA9u, 0x00u, 0x43u,       // $8008 LDA #$4300
                        0x5Bu,                     // $800B TCD            D = $4300, the DMA channels
                        0xE2u, 0x20u,              // $800C SEP #$20       -> A8
                        0xA9u, 0x01u,              // $800E LDA #$01
                        0x85u, 0x00u,              // $8010 STA $00        DMAP0 = $01, through the direct page
                        0xA9u, 0x18u,              // $8012 LDA #$18
                        0x85u, 0x01u,              // $8014 STA $01        BBAD0 = $18
                        0x20u, 0x00u, 0x81u,       // $8016 JSR !$8100     sub_008100 gives A back
                        0x85u, 0x02u,              // $8019 STA $02        A1T0L = $18, proven across the call
                        0xC2u, 0x20u,              // $801B REP #$20
                        0xA9u, 0x00u, 0x00u,       // $801D LDA #$0000
                        0x5Bu,                     // $8020 TCD            D = $0000
                        0xE2u, 0x20u,              // $8021 SEP #$20
                        0xF4u, 0x7Eu, 0x7Eu,       // $8023 PEA $7E7E
                        0xABu,                     // $8026 PLB
                        0xABu,                     // $8027 PLB            DBR = $7E
                        0x80u, 0x00u,              // $8028 BRA $802A      a label, so the state shows
                        0x4Bu,                     // $802A PHK            loc_00802A
                        0xABu,                     // $802B PLB            DBR = $00
                        0xADu, 0x10u, 0x00u,       // $802C LDA !$0010     a value the bytes do not know
                        0x29u, 0x07u,              // $802F AND #$07       bounded to eight values
                        0x0Au,                     // $8031 ASL A
                        0xAAu,                     // $8032 TAX
                        0x7Cu, 0x00u, 0x82u,       // $8033 JMP (!$8200,X) eight derived destinations
                    });
  // The routine that gives the accumulator back.
  put(rom, 0x0100u, {
                        0x48u,                     // $8100 PHA
                        0xA9u, 0xFFu,              // $8101 LDA #$FF
                        0x8Du, 0x21u, 0x21u,       // $8103 STA !$2121     CGADD
                        0x68u,                     // $8106 PLA
                        0x60u,                     // $8107 RTS
                    });
  // The routine the analysis cannot follow.
  put(rom, 0x0120u, {
                        0x6Cu, 0x20u, 0x00u,       // $8120 JMP (!$0020)   a pointer in work RAM
                    });
  // The three tables.
  put(rom, 0x0200u, {0x00u, 0x83u, 0x10u, 0x83u, 0x20u, 0x83u, 0x30u, 0x83u,   // $8200: -> $8300..$8370
                     0x40u, 0x83u, 0x50u, 0x83u, 0x60u, 0x83u, 0x70u, 0x83u});
  put(rom, 0x0220u, {0x00u, 0x84u, 0x10u, 0x84u, 0x20u, 0x84u});             // $8220: -> $8400..$8420
  put(rom, 0x0240u, {0x30u, 0x84u});                                          // $8240: -> $8430
  // The first table's targets: two set the direct register apart and meet at
  // $8380; six set it to zero and agree at $8390.
  // Each: REP #$20; LDA #value; TCD; LDA #$0000, so the accumulator's high byte
  // is clear for the eight-bit compare and the transfer after it; SEP #$20; BRA.
  put(rom, 0x0300u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x01u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x73u});  // $8300 D = $0100; BRA $8380
  put(rom, 0x0310u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x02u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x63u});  // $8310 D = $0200; BRA $8380
  put(rom, 0x0320u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x00u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x63u});  // $8320 D = $0000; BRA $8390
  put(rom, 0x0330u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x00u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x53u});  // $8330
  put(rom, 0x0340u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x00u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x43u});  // $8340
  put(rom, 0x0350u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x00u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x33u});  // $8350
  put(rom, 0x0360u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x00u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u, 0x80u, 0x23u});  // $8360
  // The eighth breaks to the software-interrupt vector before it goes on.
  put(rom, 0x0370u, {0xC2u, 0x20u, 0xA9u, 0x00u, 0x00u, 0x5Bu, 0xA9u, 0x00u, 0x00u, 0xE2u, 0x20u,
                     0x00u, 0x00u,              // $837B BRK #$00
                     0x80u, 0x35u});            // $837D BRA $83B4
  put(rom, 0x0380u, {
                        0x85u, 0x00u,              // $8380 STA $00        loc_008380: the paths disagree on D
                        0x80u, 0x1Cu,              // $8382 BRA $83A0
                    });
  put(rom, 0x0390u, {
                        0x85u, 0x00u,              // $8390 STA $00        loc_008390: the paths agree, D = $0000
                        0x80u, 0x0Cu,              // $8392 BRA $83A0
                    });
  put(rom, 0x03A0u, {
                        0xADu, 0x11u, 0x00u,       // $83A0 LDA !$0011     loc_0083A0
                        0xC9u, 0x03u,              // $83A3 CMP #$03
                        0xB0u, 0x05u,              // $83A5 BCS $83AC      three values fall through
                        0x0Au,                     // $83A7 ASL A
                        0xAAu,                     // $83A8 TAX
                        0x7Cu, 0x20u, 0x82u,       // $83A9 JMP (!$8220,X) three derived destinations
                        0xADu, 0x12u, 0x00u,       // $83AC LDA !$0012     loc_0083AC
                        0x0Au,                     // $83AF ASL A
                        0xAAu,                     // $83B0 TAX
                        0x7Cu, 0x40u, 0x82u,       // $83B1 JMP (!$8240,X) nothing bounds X: a stop
                        0x85u, 0x00u,              // $83B4 STA $00        loc_0083B4: after the break, nothing is known
                        0xDBu,                     // $83B6 STP
                    });
  // The second table's targets.
  put(rom, 0x0400u, {
                        0x20u, 0x20u, 0x81u,       // $8400 JSR !$8120     the routine the analysis cannot follow
                        0x4Cu, 0x00u, 0x85u,       // $8403 JMP !$8500
                    });
  put(rom, 0x0410u, {
                        0xA9u, 0x80u,              // $8410 LDA #$80       loc_008410
                        0x20u, 0x00u, 0x81u,       // $8412 JSR !$8100     the same routine, a second caller
                        0x8Du, 0x00u, 0x21u,       // $8415 STA !$2100     INIDISP = $80, its own value back
                        0xDBu,                     // $8418 STP
                    });
  put(rom, 0x0420u, {0x4Cu, 0x00u, 0x85u});        // $8420 JMP !$8500     a second path into the label after the call
  put(rom, 0x0430u, {0xDBu});                      // $8430 STP, the third table's one target
  put(rom, 0x0500u, {
                        0x85u, 0x00u,              // $8500 STA $00        loc_008500: nothing is known here
                        0xDBu,                     // $8502 STP
                    });
  // The NMI handler.
  put(rom, 0x0600u, {
                        0x8Bu,                     // $8600 PHB
                        0x4Bu,                     // $8601 PHK
                        0xABu,                     // $8602 PLB
                        0xA9u, 0x80u,              // $8603 LDA #$80
                        0x8Du, 0x00u, 0x21u,       // $8605 STA !$2100     INIDISP = $80
                        0xA9u, 0x05u,              // $8608 LDA #$05
                        0x85u, 0x00u,              // $860A STA $00        the direct register is not known here
                        0xABu,                     // $860C PLB
                        0x40u,                     // $860D RTI
                    });
  return rom;
}

}  // namespace snaggletooth::examples
