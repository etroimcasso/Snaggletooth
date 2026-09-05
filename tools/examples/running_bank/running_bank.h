#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A HiROM cartridge that proves its data bank from the bank the CPU runs in,
// every way a path can arrive somewhere. The tree places every byte in bank
// `$C0`; the CPU enters at `$00:8000` from the reset vector and runs through the
// mirror, where `$1000` is work RAM — under `$C0` it is the image, and the byte
// there is `$5A`. Reset pushes its bank and pulls it into the data bank, loads
// through it and writes the display register; calls long into `$C0`, where the
// routine does the same and reads the image; both call one routine that proves
// its bank and jumps through a one-slot table, so the label after it sees two
// banks and the slot derives in both; reset comes back and proves its own bank
// again; builds a pointer in work RAM and jumps long through it, which only a run
// can follow; the destination jumps through a one-slot table; that target
// returns through a frame it built itself, and the landing proves its bank once
// more. Two more blocks exist for entries a person adds — one to be entered
// through the mirror, one where the tree places it — and one is the native NMI
// handler. Every site the tests name is commented with its address.
inline std::vector<std::uint8_t> runningBankImage() {
  std::vector<std::uint8_t> rom = hiRomImage(1);
  const std::size_t site = 0xFFC0u;
  rom[site + 0x2A] = 0x00u;  // native NMI -> $8800
  rom[site + 0x2B] = 0x88u;
  rom[0x1000u] = 0x5Au;      // $C0:1000: the image, where $00:1000 is work RAM

  put(rom, 0x8000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE       -> native, A8, X8
      0x4Bu, 0xABu,                // $8002 PHK / PLB       DBR = $00, the bank the CPU runs in
      0xADu, 0x00u, 0x10u,         // $8004 LDA !$1000      $00:1000 is work RAM: not known
      0x8Du, 0x00u, 0x21u,         // $8007 STA !$2100      INIDISP, no value
      0x22u, 0x00u, 0x82u, 0xC0u,  // $800A JSL $C0:8200    into the bank the tree places the code in
      0x20u, 0x00u, 0x8Au,         // $800E JSR !$8A00      the routine both banks call, from $00
      0x4Bu, 0xABu,                // $8011 PHK / PLB       the caller's bank again: $00
      0x4Cu, 0x20u, 0x80u,         // $8013 JMP !$8020
  });
  put(rom, 0x8020u, {
      0xA9u, 0x00u,                // $8020 LDA #$00        loc_C08020
      0x8Du, 0x10u, 0x00u,         // $8022 STA !$0010      a pointer built in work RAM: $00:8400
      0xA9u, 0x84u,                // $8025 LDA #$84
      0x8Du, 0x11u, 0x00u,         // $8027 STA !$0011
      0xA9u, 0x00u,                // $802A LDA #$00
      0x8Du, 0x12u, 0x00u,         // $802C STA !$0012
      0xDCu, 0x10u, 0x00u,         // $802F JML [!$0010]    to $00:8400, which only the run sees
  });
  put(rom, 0x8200u, {
      0x4Bu, 0xABu,                // $8200 PHK / PLB       sub_C08200: DBR = $C0
      0xADu, 0x00u, 0x10u,         // $8202 LDA !$1000      $C0:1000 is the image: $5A
      0x8Du, 0x00u, 0x21u,         // $8205 STA !$2100      INIDISP $5A
      0x20u, 0x00u, 0x8Au,         // $8208 JSR !$8A00      the routine both banks call, from $C0
      0x6Bu,                       // $820B RTL
  });
  put(rom, 0x8300u, {
      0x00u, 0x89u,                // $8300 the table: one slot, $8900
  });
  put(rom, 0x8400u, {
      0x4Bu, 0xABu,                // $8400 PHK / PLB       loc_C08400: arrived in $00
      0x4Cu, 0x10u, 0x84u,         // $8402 JMP !$8410
  });
  put(rom, 0x8410u, {
      0xA2u, 0x00u,                // $8410 LDX #$00        loc_C08410
      0x7Cu, 0x00u, 0x83u,         // $8412 JMP (!$8300,X)  derived: $00:8900 via $00:8300
  });
  put(rom, 0x8500u, {
      0x4Bu, 0xABu,                // $8500 PHK / PLB       loc_C08500: the landing, in $00
      0x4Cu, 0x10u, 0x85u,         // $8502 JMP !$8510
  });
  put(rom, 0x8510u, {
      0xDBu,                       // $8510 STP             loc_C08510
  });
  put(rom, 0x8600u, {
      0x4Bu, 0xABu,                // $8600 PHK / PLB       a person's entry, written $00:8600
      0x4Cu, 0x10u, 0x86u,         // $8602 JMP !$8610
  });
  put(rom, 0x8610u, {
      0xDBu,                       // $8610 STP             loc_C08610
  });
  put(rom, 0x8700u, {
      0x4Bu, 0xABu,                // $8700 PHK / PLB       a person's entry, written $C0:8700
      0x4Cu, 0x10u, 0x87u,         // $8702 JMP !$8710
  });
  put(rom, 0x8710u, {
      0xDBu,                       // $8710 STP             loc_C08710
  });
  put(rom, 0x8800u, {
      0x4Bu, 0xABu,                // $8800 PHK / PLB       nmi_native: the chip clears the bank to take it
      0x4Cu, 0x10u, 0x88u,         // $8802 JMP !$8810
  });
  put(rom, 0x8810u, {
      0x40u,                       // $8810 RTI             loc_C08810
  });
  put(rom, 0x8900u, {
      0x4Bu, 0xABu,                // $8900 PHK / PLB       loc_C08900: the derived target, in $00
      0xF4u, 0xFFu, 0x84u,         // $8902 PEA $84FF
      0x60u,                       // $8905 RTS             lands at $00:8500
  });
  put(rom, 0x8A00u, {
      0x4Bu, 0xABu,                // $8A00 PHK / PLB       sub_C08A00: called from $00 and from $C0
      0xA2u, 0x00u,                // $8A02 LDX #$00
      0x7Cu, 0x00u, 0x8Bu,         // $8A04 JMP (!$8B00,X)  derived in both banks: $00:8A10 and $C0:8A10
  });
  put(rom, 0x8A10u, {
      0x60u,                       // $8A10 RTS             loc_C08A10: DBR = $00|$C0
  });
  put(rom, 0x8B00u, {
      0x10u, 0x8Au,                // $8B00 the table: one slot, $8A10
  });
  return rom;
}

}  // namespace snaggletooth::examples
