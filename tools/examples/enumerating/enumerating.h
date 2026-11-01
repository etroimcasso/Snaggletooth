#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A cartridge whose loaders build what they send from tables of entries, and
// select only some of the entries in the run. One bank, native mode, the data
// bank at `$7F`, the direct register at `$0000`, the stack at `$1FFF`, and the
// vertical-blank interrupt on, its handler at `$8E00` counting frames in
// `$7E:0020` and calling the fifth loader.
//
// The first loader, `sub_008100`, takes an index in X — A and X sixteen bits —
// reads a pointer from the table at `$8200`, and builds the blob it points at
// into `$7F:0000`: a blob is a kind byte, a length byte and the bytes. Kind 0
// is raw, copied a byte at a time; kind 1 is run-length, pairs of a count and
// a value; kind 3 writes `$80` to `INIDISP` and then copies raw; any other kind
// is a loop that never ends. It then sends the length to VRAM on channel 0.
// Reset loads an index from work RAM, masks it to three bits and doubles it,
// and calls the loader twice, for entries 2 and 5. The table's eight entries:
// raw at `$8300`, run-length at `$8340`, raw at `$8380`, kind 3 at `$83C0`, raw
// at `$8400`, run-length at `$8440`, the endless kind at `$8480`, and raw of
// 255 bytes at `$84C0`; every other entry builds 32.
//
// The second loader, `sub_008500`, is the same body over a table of four at
// `$8600`, its index loaded from `$7F:0012` inside it; reset calls it for the
// entry at `$8380`. Reset then jumps through the pointer at `$807C` to
// `loc_008700`, which copies sixteen bytes from `$9000` to `$7F:0100`, sends
// them to VRAM on channel 1, and jumps back to `$807E`. The third loader,
// `sub_008800`, takes an index in A at sixteen bits, which reset bounds with a
// compare against four and a branch on the carry: it copies sixteen bytes of
// the blob the table at `$8900` points at into `$7F:0200`, and reset sends them
// to CGRAM on channel 2. The fourth, `sub_008B00`, takes an index in A at eight
// bits, masked to two bits by the caller: it copies sixteen bytes of the blob
// the table at `$8C00` points at into `$7F:0300`, waits for the vertical
// blank, and sends them to OAM on channel 3. Both tables point at the four
// blobs at `$8A00`, `$8A20`, `$8A40` and `$8A60`. Reset calls them for entries
// 1 and 2, and idles.
//
// The fifth loader, `sub_008D00`, takes an index in A at sixteen bits and
// writes X before it reads it. It reads a pointer from the table at `$8F00`,
// whose blobs are a build length, a send length and the bytes; it copies the
// build length into `$7F:0400` and sends the send length to VRAM on channel
// 4. The handler, after counting the frame, loads an index from `$7E:0024`
// masked to three bits into X and one from `$7E:0022` into A, and calls the
// loader when A is below five; work RAM powers on zero, so every frame calls
// entry 0. The five entries: `$9100` builds 48 and sends 32; `$9140` builds
// 48 and sends them all; `$9180` builds 48 and sends 16; `$8A10` builds the
// 32 bytes from `$8A12`, across the blob at `$8A20`, and sends them all; and
// `$8A30` builds the 48 from `$8A32`, across the blobs at `$8A40` and `$8A60`,
// and sends 16. Every site the tests name is commented with its address.
inline std::vector<std::uint8_t> enumeratingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  rom[0x7FC0u + 0x2Au] = 0x00u;  // native NMI -> $8E00
  rom[0x7FC0u + 0x2Bu] = 0x8Eu;
  // Reset.
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE       -> native
      0xC2u, 0x30u,                // $8002 REP #$30        A16, X16
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS             S = $1FFF
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD             D = $0000
      0xE2u, 0x20u,                // $800C SEP #$20        A8
      0xA9u, 0x7Fu,                // $800E LDA #$7F
      0x48u, 0xABu,                // $8010 PHA / PLB       DBR = $7F
      0xA9u, 0x80u,                // $8012 LDA #$80
      0x8Fu, 0x15u, 0x21u, 0x00u,  // $8014 STA $00:2115    VMAIN: a word address, stepped after the high byte
      0xA9u, 0x00u,                // $8018 LDA #$00
      0x8Fu, 0x16u, 0x21u, 0x00u,  // $801A STA $00:2116    VMADDL
      0x8Fu, 0x17u, 0x21u, 0x00u,  // $801E STA $00:2117    VMADDH
      0x8Fu, 0x02u, 0x21u, 0x00u,  // $8022 STA $00:2102    OAMADDL
      0x8Fu, 0x03u, 0x21u, 0x00u,  // $8026 STA $00:2103    OAMADDH
      0x85u, 0x02u,                // $802A STA $02         the blob pointer's bank, $00
      0xA9u, 0x80u,                // $802C LDA #$80
      0x8Fu, 0x00u, 0x42u, 0x00u,  // $802E STA $00:4200    NMITIMEN: the vertical-blank interrupt on
      0xC2u, 0x30u,                // $8032 REP #$30
      0xA9u, 0x02u, 0x00u,         // $8034 LDA #$0002
      0x8Fu, 0x10u, 0x00u, 0x7Eu,  // $8037 STA $7E:0010    the first index, in work RAM
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $803B LDA $7E:0010
      0x29u, 0x07u, 0x00u,         // $803F AND #$0007
      0x0Au,                       // $8042 ASL A
      0xAAu,                       // $8043 TAX             X = 4
      0xA9u, 0x00u, 0x00u,         // $8044 LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $8047 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $804A JSR !$8100      sub_008100, entry 2
      0xC2u, 0x30u,                // $804D REP #$30
      0xA9u, 0x05u, 0x00u,         // $804F LDA #$0005
      0x8Fu, 0x10u, 0x00u, 0x7Eu,  // $8052 STA $7E:0010    the second index
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $8056 LDA $7E:0010
      0x29u, 0x07u, 0x00u,         // $805A AND #$0007
      0x0Au,                       // $805D ASL A
      0xAAu,                       // $805E TAX             X = 10
      0xA9u, 0x00u, 0x00u,         // $805F LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $8062 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8065 JSR !$8100      sub_008100, entry 5
      0xC2u, 0x30u,                // $8068 REP #$30
      0xA9u, 0x04u, 0x00u,         // $806A LDA #$0004
      0x8Du, 0x12u, 0x00u,         // $806D STA !$0012      $7F:0012: the second loader's index
      0xA2u, 0x00u, 0x00u,         // $8070 LDX #$0000
      0xA0u, 0x00u, 0x00u,         // $8073 LDY #$0000
      0x20u, 0x00u, 0x85u,         // $8076 JSR !$8500      sub_008500
      0x6Cu, 0x7Cu, 0x80u,         // $8079 JMP ($807C)     to loc_008700, through the pointer below
      0x00u, 0x87u,                // $807C .word $8700     the pointer
      0xC2u, 0x30u,                // $807E REP #$30        loc_00807E: loc_008700 comes back here
      0xA9u, 0x01u, 0x00u,         // $8080 LDA #$0001
      0x8Fu, 0x14u, 0x00u, 0x7Eu,  // $8083 STA $7E:0014    the third index
      0xA2u, 0x00u, 0x00u,         // $8087 LDX #$0000
      0xA0u, 0x00u, 0x00u,         // $808A LDY #$0000
      0xAFu, 0x14u, 0x00u, 0x7Eu,  // $808D LDA $7E:0014
      0xC9u, 0x04u, 0x00u,         // $8091 CMP #$0004
      0xB0u, 0x03u,                // $8094 BCS             four or more: no call
      0x20u, 0x00u, 0x88u,         // $8096 JSR !$8800      sub_008800, entry 1
      0xE2u, 0x20u,                // $8099 SEP #$20
      0xA9u, 0x00u,                // $809B LDA #$00
      0x8Fu, 0x21u, 0x21u, 0x00u,  // $809D STA $00:2121    CGADD = 0
      0x8Fu, 0x20u, 0x43u, 0x00u,  // $80A1 STA $00:4320    DMAP2 = $00: one register
      0x8Fu, 0x22u, 0x43u, 0x00u,  // $80A5 STA $00:4322    A1T2 low
      0x8Fu, 0x26u, 0x43u, 0x00u,  // $80A9 STA $00:4326    DAS2 high
      0xA9u, 0x22u,                // $80AD LDA #$22
      0x8Fu, 0x21u, 0x43u, 0x00u,  // $80AF STA $00:4321    BBAD2 = CGDATA
      0xA9u, 0x02u,                // $80B3 LDA #$02
      0x8Fu, 0x23u, 0x43u, 0x00u,  // $80B5 STA $00:4323    A1T2 high: $0200
      0xA9u, 0x7Fu,                // $80B9 LDA #$7F
      0x8Fu, 0x24u, 0x43u, 0x00u,  // $80BB STA $00:4324    A1B2
      0xA9u, 0x10u,                // $80BF LDA #$10
      0x8Fu, 0x25u, 0x43u, 0x00u,  // $80C1 STA $00:4325    DAS2 low: 16
      0xA9u, 0x04u,                // $80C5 LDA #$04
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $80C7 STA $00:420B    MDMAEN: channel 2
      0xA9u, 0x02u,                // $80CB LDA #$02
      0x8Fu, 0x16u, 0x00u, 0x7Eu,  // $80CD STA $7E:0016    the fourth index
      0xA2u, 0x00u, 0x00u,         // $80D1 LDX #$0000
      0xA0u, 0x00u, 0x00u,         // $80D4 LDY #$0000
      0xAFu, 0x16u, 0x00u, 0x7Eu,  // $80D7 LDA $7E:0016
      0x29u, 0x03u,                // $80DB AND #$03
      0x20u, 0x00u, 0x8Bu,         // $80DD JSR !$8B00      sub_008B00, entry 2, A8
      0xC2u, 0x20u,                // $80E0 REP #$20
      0x80u, 0xFEu,                // $80E2 BRA *           idle while the interrupts come
  });
  // The first loader. The pointer's bank is `$02`, which reset clears; the
  // length goes to `$04`, a run's count to `$06`.
  put(rom, 0x0100u, {
      0xBFu, 0x00u, 0x82u, 0x00u,  // $8100 LDA $00:8200,X   the blob's address, from the table
      0x85u, 0x00u,                // $8104 STA $00         the pointer, in the direct page
      0xE2u, 0x20u,                // $8106 SEP #$20        A8
      0x64u, 0x05u,                // $8108 STZ $05         the length's high byte
      0xA0u, 0x01u, 0x00u,         // $810A LDY #$0001
      0xB7u, 0x00u,                // $810D LDA [$00],Y     the length
      0x85u, 0x04u,                // $810F STA $04
      0xA7u, 0x00u,                // $8111 LDA [$00]       the kind
      0xF0u, 0x0Eu,                // $8113 BEQ             0: raw
      0xC9u, 0x01u,                // $8115 CMP #$01
      0xF0u, 0x1Du,                // $8117 BEQ             1: run-length
      0xC9u, 0x03u,                // $8119 CMP #$03
      0xD0u, 0x35u,                // $811B BNE             2: the loop that never ends
      0xA9u, 0x80u,                // $811D LDA #$80        3: forced blank first, then raw
      0x8Fu, 0x00u, 0x21u, 0x00u,  // $811F STA $00:2100    INIDISP
      0xA2u, 0x00u, 0x00u,         // $8123 LDX #$0000      the output index
      0xA0u, 0x02u, 0x00u,         // $8126 LDY #$0002      the blob's bytes after the header
      0xB7u, 0x00u,                // $8129 LDA [$00],Y
      0x9Du, 0x00u, 0x00u,         // $812B STA !$0000,X    $7F:0000+X
      0xE8u,                       // $812E INX
      0xC8u,                       // $812F INY
      0xE4u, 0x04u,                // $8130 CPX $04
      0xD0u, 0xF5u,                // $8132 BNE
      0x80u, 0x1Eu,                // $8134 BRA             send
      0xA2u, 0x00u, 0x00u,         // $8136 LDX #$0000
      0xA0u, 0x02u, 0x00u,         // $8139 LDY #$0002
      0xB7u, 0x00u,                // $813C LDA [$00],Y     the count
      0x85u, 0x06u,                // $813E STA $06
      0xC8u,                       // $8140 INY
      0xB7u, 0x00u,                // $8141 LDA [$00],Y     the value
      0xC8u,                       // $8143 INY
      0x9Du, 0x00u, 0x00u,         // $8144 STA !$0000,X    $7F:0000+X
      0xE8u,                       // $8147 INX
      0xC6u, 0x06u,                // $8148 DEC $06
      0xD0u, 0xF8u,                // $814A BNE
      0xE4u, 0x04u,                // $814C CPX $04
      0xD0u, 0xECu,                // $814E BNE
      0x80u, 0x02u,                // $8150 BRA             send
      0x80u, 0xFEu,                // $8152 BRA *
      0xA9u, 0x01u,                // $8154 LDA #$01
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $8156 STA $00:4300    DMAP0 = $01: a word, two registers
      0xA9u, 0x18u,                // $815A LDA #$18
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $815C STA $00:4301    BBAD0 = VMDATAL
      0xA9u, 0x00u,                // $8160 LDA #$00
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8162 STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $8166 STA $00:4303    A1T0 high: $0000
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $816A STA $00:4306    DAS0 high
      0xA9u, 0x7Fu,                // $816E LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $8170 STA $00:4304    A1B0
      0xA5u, 0x04u,                // $8174 LDA $04
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $8176 STA $00:4305    DAS0 low: the length
      0xA9u, 0x01u,                // $817A LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $817C STA $00:420B    MDMAEN: channel 0
      0x60u,                       // $8180 RTS
  });
  // The second loader: its index from work RAM, then the first loader's body
  // over the table at `$8600`.
  put(rom, 0x0500u, {
      0xAEu, 0x12u, 0x00u,         // $8500 LDX !$0012      the index, loaded from work RAM
      0xBFu, 0x00u, 0x86u, 0x00u,  // $8503 LDA $00:8600,X   the blob's address, from the table
      0x85u, 0x00u,                // $8507 STA $00         the pointer, in the direct page
      0xE2u, 0x20u,                // $8509 SEP #$20        A8
      0x64u, 0x05u,                // $850B STZ $05         the length's high byte
      0xA0u, 0x01u, 0x00u,         // $850D LDY #$0001
      0xB7u, 0x00u,                // $8510 LDA [$00],Y     the length
      0x85u, 0x04u,                // $8512 STA $04
      0xA7u, 0x00u,                // $8514 LDA [$00]       the kind
      0xF0u, 0x0Eu,                // $8516 BEQ             0: raw
      0xC9u, 0x01u,                // $8518 CMP #$01
      0xF0u, 0x1Du,                // $851A BEQ             1: run-length
      0xC9u, 0x03u,                // $851C CMP #$03
      0xD0u, 0x35u,                // $851E BNE             2: the loop that never ends
      0xA9u, 0x80u,                // $8520 LDA #$80        3: forced blank first, then raw
      0x8Fu, 0x00u, 0x21u, 0x00u,  // $8522 STA $00:2100    INIDISP
      0xA2u, 0x00u, 0x00u,         // $8526 LDX #$0000      the output index
      0xA0u, 0x02u, 0x00u,         // $8529 LDY #$0002      the blob's bytes after the header
      0xB7u, 0x00u,                // $852C LDA [$00],Y
      0x9Du, 0x00u, 0x00u,         // $852E STA !$0000,X    $7F:0000+X
      0xE8u,                       // $8531 INX
      0xC8u,                       // $8532 INY
      0xE4u, 0x04u,                // $8533 CPX $04
      0xD0u, 0xF5u,                // $8535 BNE
      0x80u, 0x1Eu,                // $8537 BRA             send
      0xA2u, 0x00u, 0x00u,         // $8539 LDX #$0000
      0xA0u, 0x02u, 0x00u,         // $853C LDY #$0002
      0xB7u, 0x00u,                // $853F LDA [$00],Y     the count
      0x85u, 0x06u,                // $8541 STA $06
      0xC8u,                       // $8543 INY
      0xB7u, 0x00u,                // $8544 LDA [$00],Y     the value
      0xC8u,                       // $8546 INY
      0x9Du, 0x00u, 0x00u,         // $8547 STA !$0000,X    $7F:0000+X
      0xE8u,                       // $854A INX
      0xC6u, 0x06u,                // $854B DEC $06
      0xD0u, 0xF8u,                // $854D BNE
      0xE4u, 0x04u,                // $854F CPX $04
      0xD0u, 0xECu,                // $8551 BNE
      0x80u, 0x02u,                // $8553 BRA             send
      0x80u, 0xFEu,                // $8555 BRA *
      0xA9u, 0x01u,                // $8557 LDA #$01
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $8559 STA $00:4300    DMAP0 = $01: a word, two registers
      0xA9u, 0x18u,                // $855D LDA #$18
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $855F STA $00:4301    BBAD0 = VMDATAL
      0xA9u, 0x00u,                // $8563 LDA #$00
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8565 STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $8569 STA $00:4303    A1T0 high: $0000
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $856D STA $00:4306    DAS0 high
      0xA9u, 0x7Fu,                // $8571 LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $8573 STA $00:4304    A1B0
      0xA5u, 0x04u,                // $8577 LDA $04
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $8579 STA $00:4305    DAS0 low: the length
      0xA9u, 0x01u,                // $857D LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $857F STA $00:420B    MDMAEN: channel 0
      0x60u,                       // $8583 RTS
  });
  // The routine reset jumps to.
  put(rom, 0x0700u, {
      0xE2u, 0x20u,                // $8700 SEP #$20        loc_008700
      0xA2u, 0x00u, 0x00u,         // $8702 LDX #$0000
      0xBFu, 0x00u, 0x90u, 0x00u,  // $8705 LDA $00:9000,X
      0x9Du, 0x00u, 0x01u,         // $8709 STA !$0100,X    $7F:0100+X
      0xE8u,                       // $870C INX
      0xE0u, 0x10u, 0x00u,         // $870D CPX #$0010
      0xD0u, 0xF3u,                // $8710 BNE
      0xA9u, 0x01u,                // $8712 LDA #$01
      0x8Fu, 0x10u, 0x43u, 0x00u,  // $8714 STA $00:4310    DMAP1 = $01
      0xA9u, 0x18u,                // $8718 LDA #$18
      0x8Fu, 0x11u, 0x43u, 0x00u,  // $871A STA $00:4311    BBAD1 = VMDATAL
      0xA9u, 0x00u,                // $871E LDA #$00
      0x8Fu, 0x12u, 0x43u, 0x00u,  // $8720 STA $00:4312    A1T1 low
      0x8Fu, 0x16u, 0x43u, 0x00u,  // $8724 STA $00:4316    DAS1 high
      0xA9u, 0x01u,                // $8728 LDA #$01
      0x8Fu, 0x13u, 0x43u, 0x00u,  // $872A STA $00:4313    A1T1 high: $0100
      0xA9u, 0x7Fu,                // $872E LDA #$7F
      0x8Fu, 0x14u, 0x43u, 0x00u,  // $8730 STA $00:4314    A1B1
      0xA9u, 0x10u,                // $8734 LDA #$10
      0x8Fu, 0x15u, 0x43u, 0x00u,  // $8736 STA $00:4315    DAS1 low: 16
      0xA9u, 0x02u,                // $873A LDA #$02
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $873C STA $00:420B    MDMAEN: channel 1
      0x4Cu, 0x7Eu, 0x80u,         // $8740 JMP !$807E      back into reset's sequence
  });
  // The third loader.
  put(rom, 0x0800u, {
      0x0Au,                       // $8800 ASL A           sub_008800: A16, X16
      0xAAu,                       // $8801 TAX
      0xBFu, 0x00u, 0x89u, 0x00u,  // $8802 LDA $00:8900,X  the blob's address
      0x85u, 0x00u,                // $8806 STA $00
      0xE2u, 0x20u,                // $8808 SEP #$20
      0xA0u, 0x00u, 0x00u,         // $880A LDY #$0000
      0xB7u, 0x00u,                // $880D LDA [$00],Y
      0x99u, 0x00u, 0x02u,         // $880F STA !$0200,Y    $7F:0200+Y
      0xC8u,                       // $8812 INY
      0xC0u, 0x10u, 0x00u,         // $8813 CPY #$0010
      0xD0u, 0xF5u,                // $8816 BNE
      0xC2u, 0x20u,                // $8818 REP #$20
      0x60u,                       // $881A RTS
  });
  // The fourth loader.
  put(rom, 0x0B00u, {
      0xC2u, 0x20u,                // $8B00 REP #$20        sub_008B00: entered A8, X16
      0x29u, 0xFFu, 0x00u,         // $8B02 AND #$00FF
      0x0Au,                       // $8B05 ASL A
      0xAAu,                       // $8B06 TAX
      0xBFu, 0x00u, 0x8Cu, 0x00u,  // $8B07 LDA $00:8C00,X  the blob's address
      0x85u, 0x00u,                // $8B0B STA $00
      0xE2u, 0x20u,                // $8B0D SEP #$20
      0xA0u, 0x00u, 0x00u,         // $8B0F LDY #$0000
      0xB7u, 0x00u,                // $8B12 LDA [$00],Y
      0x99u, 0x00u, 0x03u,         // $8B14 STA !$0300,Y    $7F:0300+Y
      0xC8u,                       // $8B17 INY
      0xC0u, 0x10u, 0x00u,         // $8B18 CPY #$0010
      0xD0u, 0xF5u,                // $8B1B BNE
      0xCBu,                       // $8B1D WAI             the sprite table goes in the vertical blank
      0xA9u, 0x00u,                // $8B1E LDA #$00
      0x8Fu, 0x30u, 0x43u, 0x00u,  // $8B20 STA $00:4330    DMAP3 = $00: one register
      0x8Fu, 0x32u, 0x43u, 0x00u,  // $8B24 STA $00:4332    A1T3 low
      0x8Fu, 0x36u, 0x43u, 0x00u,  // $8B28 STA $00:4336    DAS3 high
      0xA9u, 0x04u,                // $8B2C LDA #$04
      0x8Fu, 0x31u, 0x43u, 0x00u,  // $8B2E STA $00:4331    BBAD3 = OAMDATA
      0xA9u, 0x03u,                // $8B32 LDA #$03
      0x8Fu, 0x33u, 0x43u, 0x00u,  // $8B34 STA $00:4333    A1T3 high: $0300
      0xA9u, 0x7Fu,                // $8B38 LDA #$7F
      0x8Fu, 0x34u, 0x43u, 0x00u,  // $8B3A STA $00:4334    A1B3
      0xA9u, 0x10u,                // $8B3E LDA #$10
      0x8Fu, 0x35u, 0x43u, 0x00u,  // $8B40 STA $00:4335    DAS3 low: 16
      0xA9u, 0x08u,                // $8B44 LDA #$08
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8B46 STA $00:420B    MDMAEN: channel 3
      0x60u,                       // $8B4A RTS
  });
  // The fifth loader: the index in A, a build length and a send length.
  put(rom, 0x0D00u, {
      0x0Au,                       // $8D00 ASL A           sub_008D00: A16, X16
      0xAAu,                       // $8D01 TAX
      0xBFu, 0x00u, 0x8Fu, 0x00u,  // $8D02 LDA $00:8F00,X  the blob's address
      0x85u, 0x00u,                // $8D06 STA $00
      0xE2u, 0x20u,                // $8D08 SEP #$20
      0x64u, 0x05u,                // $8D0A STZ $05
      0x64u, 0x07u,                // $8D0C STZ $07
      0xA7u, 0x00u,                // $8D0E LDA [$00]       the build length
      0x85u, 0x04u,                // $8D10 STA $04
      0xA0u, 0x01u, 0x00u,         // $8D12 LDY #$0001
      0xB7u, 0x00u,                // $8D15 LDA [$00],Y     the send length
      0x85u, 0x06u,                // $8D17 STA $06
      0xA2u, 0x00u, 0x00u,         // $8D19 LDX #$0000
      0xA0u, 0x02u, 0x00u,         // $8D1C LDY #$0002
      0xB7u, 0x00u,                // $8D1F LDA [$00],Y
      0x9Du, 0x00u, 0x04u,         // $8D21 STA !$0400,X    $7F:0400+X
      0xE8u,                       // $8D24 INX
      0xC8u,                       // $8D25 INY
      0xE4u, 0x04u,                // $8D26 CPX $04
      0xD0u, 0xF5u,                // $8D28 BNE
      0xA9u, 0x01u,                // $8D2A LDA #$01
      0x8Fu, 0x40u, 0x43u, 0x00u,  // $8D2C STA $00:4340    DMAP4 = $01: a word, two registers
      0xA9u, 0x18u,                // $8D30 LDA #$18
      0x8Fu, 0x41u, 0x43u, 0x00u,  // $8D32 STA $00:4341    BBAD4 = VMDATAL
      0xA9u, 0x00u,                // $8D36 LDA #$00
      0x8Fu, 0x42u, 0x43u, 0x00u,  // $8D38 STA $00:4342    A1T4 low
      0x8Fu, 0x46u, 0x43u, 0x00u,  // $8D3C STA $00:4346    DAS4 high
      0xA9u, 0x04u,                // $8D40 LDA #$04
      0x8Fu, 0x43u, 0x43u, 0x00u,  // $8D42 STA $00:4343    A1T4 high: $0400
      0xA9u, 0x7Fu,                // $8D46 LDA #$7F
      0x8Fu, 0x44u, 0x43u, 0x00u,  // $8D48 STA $00:4344    A1B4
      0xA5u, 0x06u,                // $8D4C LDA $06
      0x8Fu, 0x45u, 0x43u, 0x00u,  // $8D4E STA $00:4345    DAS4 low: the send length
      0xA9u, 0x10u,                // $8D52 LDA #$10
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8D54 STA $00:420B    MDMAEN: channel 4
      0xC2u, 0x20u,                // $8D58 REP #$20
      0x60u,                       // $8D5A RTS
  });
  // The vertical-blank handler.
  put(rom, 0x0E00u, {
      0x08u,                       // $8E00 PHP             the vertical-blank handler
      0xE2u, 0x20u,                // $8E01 SEP #$20
      0x48u,                       // $8E03 PHA
      0xAFu, 0x10u, 0x42u, 0x00u,  // $8E04 LDA $00:4210    RDNMI
      0xAFu, 0x20u, 0x00u, 0x7Eu,  // $8E08 LDA $7E:0020
      0x1Au,                       // $8E0C INC A
      0x8Fu, 0x20u, 0x00u, 0x7Eu,  // $8E0D STA $7E:0020    the frame count
      0xC2u, 0x30u,                // $8E11 REP #$30
      0xDAu,                       // $8E13 PHX
      0x5Au,                       // $8E14 PHY
      0xAFu, 0x24u, 0x00u, 0x7Eu,  // $8E15 LDA $7E:0024
      0x29u, 0x07u, 0x00u,         // $8E19 AND #$0007
      0xAAu,                       // $8E1C TAX             X in {0..7}
      0xAFu, 0x22u, 0x00u, 0x7Eu,  // $8E1D LDA $7E:0022
      0xC9u, 0x05u, 0x00u,         // $8E21 CMP #$0005
      0xB0u, 0x06u,                // $8E24 BCS             five or more: no call
      0xA0u, 0x00u, 0x00u,         // $8E26 LDY #$0000
      0x20u, 0x00u, 0x8Du,         // $8E29 JSR !$8D00      sub_008D00, entry 0
      0x7Au,                       // $8E2C PLY
      0xFAu,                       // $8E2D PLX
      0xE2u, 0x20u,                // $8E2E SEP #$20
      0x68u,                       // $8E30 PLA
      0x28u,                       // $8E31 PLP
      0x40u,                       // $8E32 RTI
  });
  // The tables.
  const auto words = [&](std::size_t at, std::initializer_list<std::uint16_t> values) {
    for (const std::uint16_t value : values) {
      rom[at++] = static_cast<std::uint8_t>(value & 0xFFu);
      rom[at++] = static_cast<std::uint8_t>(value >> 8);
    }
  };
  words(0x0200u, {0x8300u, 0x8340u, 0x8380u, 0x83C0u, 0x8400u, 0x8440u, 0x8480u, 0x84C0u});  // $8200
  words(0x0600u, {0x8300u, 0x8340u, 0x8380u, 0x83C0u});                                      // $8600
  words(0x0900u, {0x8A00u, 0x8A20u, 0x8A40u, 0x8A60u});                                      // $8900
  words(0x0C00u, {0x8A00u, 0x8A20u, 0x8A40u, 0x8A60u});                                      // $8C00
  words(0x0F00u, {0x9100u, 0x9140u, 0x9180u, 0x8A10u, 0x8A30u});                             // $8F00
  // The first table's blobs: a kind, a length, the bytes.
  const auto raw = [&](std::size_t at, std::uint8_t kind, std::uint8_t length, std::uint8_t first) {
    rom[at] = kind;
    rom[at + 1u] = length;
    for (std::size_t i = 0; i < length; ++i) rom[at + 2u + i] = static_cast<std::uint8_t>(first + i);
  };
  raw(0x0300u, 0x00u, 0x20u, 0x00u);                     // $8300: entry 0, raw
  put(rom, 0x0340u, {0x01u, 0x20u, 0x04u, 0xA1u, 0x04u, 0xA2u, 0x04u, 0xA3u, 0x04u, 0xA4u,
                     0x04u, 0xA5u, 0x04u, 0xA6u, 0x04u, 0xA7u, 0x04u, 0xA8u});  // $8340: entry 1, run-length
  raw(0x0380u, 0x00u, 0x20u, 0x20u);                     // $8380: entry 2, raw
  raw(0x03C0u, 0x03u, 0x20u, 0x60u);                     // $83C0: entry 3, forced blank first
  raw(0x0400u, 0x00u, 0x20u, 0x80u);                     // $8400: entry 4, raw
  put(rom, 0x0440u, {0x01u, 0x20u, 0x02u, 0xC1u, 0x06u, 0xC2u, 0x04u, 0xC3u, 0x04u, 0xC4u,
                     0x08u, 0xC5u, 0x02u, 0xC6u, 0x03u, 0xC7u, 0x03u, 0xC8u});  // $8440: entry 5, run-length
  put(rom, 0x0480u, {0x02u, 0x20u});                     // $8480: entry 6, the endless kind
  put(rom, 0x04C0u, {0x00u, 0xFFu});                     // $84C0: entry 7, raw of 255 bytes, over what follows
  // The blobs the third and fourth loaders copy, sixteen bytes each.
  for (std::size_t blob = 0; blob < 4; ++blob) {
    for (std::size_t i = 0; i < 16; ++i) rom[0x0A00u + 0x20u * blob + i] = static_cast<std::uint8_t>(0x10u * (blob + 4u) + i);
  }
  for (std::size_t i = 0; i < 16; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0xF0u + i);  // $9000: loc_008700's copy
  // The fifth loader's blobs: a build length, a send length, the bytes. The
  // last two entries point into the blobs at `$8A00`: `$8A10` builds 32 bytes
  // from `$8A12`, `$8A30` builds 48 from `$8A32`.
  const auto sized = [&](std::size_t at, std::uint8_t build, std::uint8_t send, std::uint8_t first) {
    rom[at] = build;
    rom[at + 1u] = send;
    for (std::size_t i = 0; i < build; ++i) rom[at + 2u + i] = static_cast<std::uint8_t>(first + i);
  };
  sized(0x1100u, 0x30u, 0x20u, 0x00u);  // $9100: entry 0
  sized(0x1140u, 0x30u, 0x30u, 0x40u);  // $9140: entry 1
  sized(0x1180u, 0x30u, 0x10u, 0x80u);  // $9180: entry 2
  put(rom, 0x0A10u, {0x20u, 0x20u});    // $8A10: entry 3
  put(rom, 0x0A30u, {0x30u, 0x10u});    // $8A30: entry 4
  return rom;
}

}  // namespace snaggletooth::examples
