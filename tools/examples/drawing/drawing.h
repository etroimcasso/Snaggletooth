#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A cartridge that sends the hardware one of everything an editable form has
// a grammar for, so every form is shown on bytes that are ours to publish.
// Reset goes native with the data bank at `$7E`, and in forced blank sends
// from the image: a 4-bit tileset of three tiles and a half to word `$1000`,
// BG1's name base; a 2-bit tileset of four tiles to `$5000`, BG3's; a sprite
// sheet of two tiles to `$7000`, the second half of the sprite tiles; a
// palette of sixteen words, one with its top
// bit set, to entry zero; a one-screen tilemap that uses every flag to word
// `$0000`, BG1's screen; and the whole 544-byte sprite table to OAM. Then Mode
// 1 with those bases, the screen on, two HDMA channels enabled — channel 1
// walks a direct table of unit 2 with a repeat entry to the window registers,
// channel 2 an indirect table of unit 1 to the brightness register — and the
// vertical-blank interrupt on. The handler counts frames: on the second it
// decompresses a run-length blob into thirty-two bytes of work RAM and sends
// them to word `$1200`, inside BG1's name base, then decompresses a second
// blob into the same thirty-two bytes and sends them to the palette at entry
// sixteen — one extent carried out with two contents; on the third it
// switches to Mode 7 and sends a block whose even bytes are a map and whose
// odd bytes are tiles to word `$0000`. Every site the tests name is commented
// with its address.
inline std::vector<std::uint8_t> drawingImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                            // $8000 CLC / XCE       -> native
      0xE2u, 0x30u,                            // $8002 SEP #$30        A8, X8
      0xA9u, 0x7Eu, 0x48u, 0xABu,              // $8004 LDA #$7E / PHA / PLB   DBR = $7E
      0xA9u, 0x80u, 0x8Fu, 0x15u, 0x21u, 0x00u,  // $8008 VMAIN = $80: increment after the high byte, by one word
      // ---- the 4-bit tileset, 112 bytes, to word $1000 ----
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $800E VMADDL
      0xA9u, 0x10u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $8014 VMADDH: word $1000
      0xA9u, 0x01u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $801A DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $8020 BBAD0 = $18: VMDATAL
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $8026 A1T0 low
      0xA9u, 0x90u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $802C A1T0 high: $9000
      0xA9u, 0x00u, 0x8Fu, 0x04u, 0x43u, 0x00u,  // $8032 A1B0 = $00
      0xA9u, 0x70u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $8038 DAS0 low: 112
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $803E DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8044 MDMAEN = $01 (the write at $8046)
      // ---- the 2-bit tileset, 64 bytes, to word $5000 ----
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $804A VMADDL
      0xA9u, 0x50u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $8050 VMADDH: word $5000
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $8056 A1T0 low
      0xA9u, 0x91u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $805C A1T0 high: $9100
      0xA9u, 0x40u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $8062 DAS0 low: 64
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $8068 DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $806E MDMAEN = $01 (the write at $8070)
      // ---- the sprite sheet, 64 bytes, to word $7000 ----
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $8074 VMADDL
      0xA9u, 0x70u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $807A VMADDH: word $7000
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $8080 A1T0 low
      0xA9u, 0x92u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $8086 A1T0 high: $9200
      0xA9u, 0x40u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $808C DAS0 low: 64
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $8092 DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8098 MDMAEN = $01 (the write at $809A)
      // ---- the palette, 32 bytes, to entry zero ----
      0xA9u, 0x00u, 0x8Fu, 0x21u, 0x21u, 0x00u,  // $809E CGADD = 0
      0xA9u, 0x00u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $80A4 DMAP0 = $00: pattern 0
      0xA9u, 0x22u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $80AA BBAD0 = $22: CGDATA
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $80B0 A1T0 low
      0xA9u, 0x93u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $80B6 A1T0 high: $9300
      0xA9u, 0x20u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $80BC DAS0 low: 32
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $80C2 DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $80C8 MDMAEN = $01 (the write at $80CA)
      // ---- the tilemap, 2048 bytes, to word $0000 ----
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $80CE VMADDL
      0xA9u, 0x00u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $80D4 VMADDH: word $0000
      0xA9u, 0x01u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $80DA DMAP0 = $01
      0xA9u, 0x18u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $80E0 BBAD0 = $18
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $80E6 A1T0 low
      0xA9u, 0x94u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $80EC A1T0 high: $9400
      0xA9u, 0x00u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $80F2 DAS0 low
      0xA9u, 0x08u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $80F8 DAS0 high: 2048
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $80FE MDMAEN = $01 (the write at $8100)
      // ---- the sprite table, 544 bytes, to OAM byte zero ----
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x21u, 0x00u,  // $8104 OAMADDL = 0
      0xA9u, 0x00u, 0x8Fu, 0x03u, 0x21u, 0x00u,  // $810A OAMADDH = 0
      0xA9u, 0x00u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $8110 DMAP0 = $00
      0xA9u, 0x04u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $8116 BBAD0 = $04: OAMDATA
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $811C A1T0 low
      0xA9u, 0x9Cu, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $8122 A1T0 high: $9C00
      0xA9u, 0x20u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $8128 DAS0 low
      0xA9u, 0x02u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $812E DAS0 high: 544
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8134 MDMAEN = $01 (the write at $8136)
      // ---- the mode and the bases ----
      0xA9u, 0x01u, 0x8Fu, 0x05u, 0x21u, 0x00u,  // $813A BGMODE = $01: Mode 1
      0xA9u, 0x00u, 0x8Fu, 0x07u, 0x21u, 0x00u,  // $8140 BG1SC = $00: screen at $0000, one screen
      0xA9u, 0x04u, 0x8Fu, 0x09u, 0x21u, 0x00u,  // $8146 BG3SC = $04: screen at $0400
      0xA9u, 0x11u, 0x8Fu, 0x0Bu, 0x21u, 0x00u,  // $814C BG12NBA = $11: BG1's and BG2's tiles at $1000
      0xA9u, 0x05u, 0x8Fu, 0x0Cu, 0x21u, 0x00u,  // $8152 BG34NBA = $05: BG3's tiles at $5000
      0xA9u, 0x03u, 0x8Fu, 0x01u, 0x21u, 0x00u,  // $8158 OBSEL = $03: sprite tiles at $6000, the second half at $7000
      // ---- channel 1: a direct table of unit 2 to the window registers ----
      0xA9u, 0x01u, 0x8Fu, 0x10u, 0x43u, 0x00u,  // $815E DMAP1 = $01: direct, pattern 1: two registers, a byte each
      0xA9u, 0x26u, 0x8Fu, 0x11u, 0x43u, 0x00u,  // $8164 BBAD1 = $26: WH0, then WH1
      0xA9u, 0x00u, 0x8Fu, 0x12u, 0x43u, 0x00u,  // $816A A1T1 low
      0xA9u, 0xA4u, 0x8Fu, 0x13u, 0x43u, 0x00u,  // $8170 A1T1 high: $A400
      0xA9u, 0x00u, 0x8Fu, 0x14u, 0x43u, 0x00u,  // $8176 A1B1 = $00
      // ---- channel 2: an indirect table of unit 1 to the brightness register ----
      0xA9u, 0x40u, 0x8Fu, 0x20u, 0x43u, 0x00u,  // $817C DMAP2 = $40: indirect, pattern 0
      0xA9u, 0x00u, 0x8Fu, 0x21u, 0x43u, 0x00u,  // $8182 BBAD2 = $00: INIDISP
      0xA9u, 0x10u, 0x8Fu, 0x22u, 0x43u, 0x00u,  // $8188 A1T2 low
      0xA9u, 0xA4u, 0x8Fu, 0x23u, 0x43u, 0x00u,  // $818E A1T2 high: $A410
      0xA9u, 0x00u, 0x8Fu, 0x24u, 0x43u, 0x00u,  // $8194 A1B2 = $00
      0xA9u, 0x00u, 0x8Fu, 0x27u, 0x43u, 0x00u,  // $819A DASB2 = $00: the blocks' bank
      0xA9u, 0x0Fu, 0x8Fu, 0x00u, 0x21u, 0x00u,  // $81A0 INIDISP = $0F: the screen on
      0xA9u, 0x06u, 0x8Fu, 0x0Cu, 0x42u, 0x00u,  // $81A6 HDMAEN = $06: channels 1 and 2 (the write at $81A8)
      0xA9u, 0x80u, 0x8Fu, 0x00u, 0x42u, 0x00u,  // $81AC NMITIMEN = $80: the vertical-blank interrupt on
      0x80u, 0xFEu,                            // $81B2 BRA $81B2
  });
  // The native vertical-blank handler, at $8320 — the vector is pointed at it
  // below — opening by setting both widths, since an interrupt's entry carries
  // whatever widths the interrupted code had.
  put(rom, 0x0320u, {
      0xE2u, 0x30u,                            // $8320 SEP #$30        A8, X8
      0xEEu, 0x10u, 0x00u,                     // $8322 INC !$0010: the frame count, at $7E:0010
      0xADu, 0x10u, 0x00u,                     // $8325 LDA !$0010
      0xC9u, 0x02u,                            // $8328 CMP #$02
      0xD0u, 0x74u,                            // $832A BNE $83A0
      // ---- the second frame: a blob to tiles, then a blob to the palette, through one buffer ----
      0x20u, 0x00u, 0x84u,                     // $832C JSR !$8400      unpack $A500 -> $7E:1000, 32 bytes
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $832F VMADDL
      0xA9u, 0x12u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $8335 VMADDH: word $1200
      0xA9u, 0x01u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $833B DMAP0 = $01
      0xA9u, 0x18u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $8341 BBAD0 = $18
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $8347 A1T0 low
      0xA9u, 0x10u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $834D A1T0 high: $1000
      0xA9u, 0x7Eu, 0x8Fu, 0x04u, 0x43u, 0x00u,  // $8353 A1B0 = $7E: work RAM
      0xA9u, 0x20u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $8359 DAS0 low: 32
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $835F DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8365 MDMAEN = $01 (the write at $8367)
      0x20u, 0x40u, 0x84u,                     // $836B JSR !$8440      unpack $A520 -> $7E:1000, 32 bytes
      0xA9u, 0x10u, 0x8Fu, 0x21u, 0x21u, 0x00u,  // $836E CGADD = $10
      0xA9u, 0x00u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $8374 DMAP0 = $00
      0xA9u, 0x22u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $837A BBAD0 = $22: CGDATA
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $8380 A1T0 low: the transfer consumed the address and the count
      0xA9u, 0x10u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $8386 A1T0 high: $1000 again
      0xA9u, 0x20u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $838C DAS0 low: 32 again
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $8392 DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8398 MDMAEN = $01 (the write at $839A)
      0x80u, 0x49u,                            // $839E BRA $83E9
      0xADu, 0x10u, 0x00u,                     // $83A0 LDA !$0010
      0xC9u, 0x03u,                            // $83A3 CMP #$03
      0xD0u, 0x42u,                            // $83A5 BNE $83E9
      // ---- the third frame: Mode 7, and an interleaved block of 128 bytes to word $0000 ----
      0xA9u, 0x07u, 0x8Fu, 0x05u, 0x21u, 0x00u,  // $83A7 BGMODE = $07
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $83AD VMADDL
      0xA9u, 0x00u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $83B3 VMADDH: word $0000
      0xA9u, 0x01u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $83B9 DMAP0 = $01
      0xA9u, 0x18u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $83BF BBAD0 = $18
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $83C5 A1T0 low
      0xA9u, 0xA6u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $83CB A1T0 high: $A600
      0xA9u, 0x00u, 0x8Fu, 0x04u, 0x43u, 0x00u,  // $83D1 A1B0 = $00
      0xA9u, 0x80u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $83D7 DAS0 low: 128
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $83DD DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $83E3 MDMAEN = $01 (the write at $83E5)
      0xAFu, 0x10u, 0x42u, 0x00u,              // $83E9 LDA $00:4210: acknowledge
      0x40u,                                   // $83ED RTI
  });
  rom[0x7FC0u + 0x2Au] = 0x20u;  // native NMI -> $8320
  rom[0x7FC0u + 0x2Bu] = 0x83u;
  // The two decoders: pairs of a count and a value until a count of zero, the
  // count in the direct page, the value stored to $7E:1000+Y.
  put(rom, 0x0400u, {
      0xA2u, 0x00u,                // $8400 LDX #$00        sub_008400
      0xA0u, 0x00u,                // $8402 LDY #$00
      0xBFu, 0x00u, 0xA5u, 0x00u,  // $8404 LDA $00:A500,X  the count
      0xF0u, 0x12u,                // $8408 BEQ $841C       zero ends it
      0x85u, 0x00u,                // $840A STA $00         the counter
      0xE8u,                       // $840C INX
      0xBFu, 0x00u, 0xA5u, 0x00u,  // $840D LDA $00:A500,X  the value
      0xE8u,                       // $8411 INX
      0x99u, 0x00u, 0x10u,         // $8412 STA !$1000,Y    the output, at $7E:1000+Y
      0xC8u,                       // $8415 INY
      0xC6u, 0x00u,                // $8416 DEC $00
      0xD0u, 0xF8u,                // $8418 BNE $8412
      0x80u, 0xE8u,                // $841A BRA $8404
      0x60u,                       // $841C RTS
  });
  put(rom, 0x0440u, {
      0xA2u, 0x00u,                // $8440 LDX #$00        sub_008440
      0xA0u, 0x00u,                // $8442 LDY #$00
      0xBFu, 0x20u, 0xA5u, 0x00u,  // $8444 LDA $00:A520,X  the count
      0xF0u, 0x12u,                // $8448 BEQ $845C
      0x85u, 0x00u,                // $844A STA $00
      0xE8u,                       // $844C INX
      0xBFu, 0x20u, 0xA5u, 0x00u,  // $844D LDA $00:A520,X  the value
      0xE8u,                       // $8451 INX
      0x99u, 0x00u, 0x10u,         // $8452 STA !$1000,Y
      0xC8u,                       // $8455 INY
      0xC6u, 0x00u,                // $8456 DEC $00
      0xD0u, 0xF8u,                // $8458 BNE $8452
      0x80u, 0xE8u,                // $845A BRA $8444
      0x60u,                       // $845C RTS
  });
  // The data.
  for (std::size_t i = 0; i < 112; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);  // $9000: 4-bit tiles, three and a half
  for (std::size_t i = 0; i < 64; ++i) rom[0x1100u + i] = static_cast<std::uint8_t>((i * 29u + 5u) & 0xFFu);    // $9100: 2-bit tiles, four
  for (std::size_t i = 0; i < 64; ++i) rom[0x1200u + i] = static_cast<std::uint8_t>((i * 53u + 7u) & 0xFFu);    // $9200: the sprite sheet, two tiles
  for (std::size_t i = 0; i < 16; ++i) {                                                                        // $9300: sixteen palette words
    const std::uint16_t word = static_cast<std::uint16_t>((i * 0x0421u) | (i == 5u ? 0x8000u : 0u));
    rom[0x1300u + 2u * i] = static_cast<std::uint8_t>(word & 0xFFu);
    rom[0x1300u + 2u * i + 1u] = static_cast<std::uint8_t>(word >> 8);
  }
  for (std::size_t e = 0; e < 1024; ++e) {                                                                      // $9400: one screen, every flag
    const std::uint16_t word = static_cast<std::uint16_t>((e & 0x3FFu) | ((e % 8u) << 10) | ((e % 8u) << 13));
    rom[0x1400u + 2u * e] = static_cast<std::uint8_t>(word & 0xFFu);
    rom[0x1400u + 2u * e + 1u] = static_cast<std::uint8_t>(word >> 8);
  }
  for (std::size_t s = 0; s < 128; ++s) {                                                                       // $9C00: the sprite table
    rom[0x1C00u + 4u * s] = static_cast<std::uint8_t>(s * 2u);
    rom[0x1C00u + 4u * s + 1u] = static_cast<std::uint8_t>(s);
    rom[0x1C00u + 4u * s + 2u] = static_cast<std::uint8_t>(s * 3u);
    rom[0x1C00u + 4u * s + 3u] = static_cast<std::uint8_t>(((s % 8u) << 1) | ((s % 4u) << 4) | ((s & 1u) << 6) | ((s & 2u) << 6) | (s & 1u));
  }
  for (std::size_t i = 0; i < 32; ++i) rom[0x1E00u + i] = static_cast<std::uint8_t>((i * 0x5Bu) & 0xFFu);        // the high table
  put(rom, 0x2400u, {0x02u, 0xAAu, 0xBBu, 0x83u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x00u});             // $A400: the direct table
  put(rom, 0x2410u, {0x03u, 0x20u, 0xA4u, 0x83u, 0x21u, 0xA4u, 0x00u});                                          // $A410: the indirect table
  put(rom, 0x2420u, {0x0Fu, 0x08u, 0x07u, 0x06u});                                                              // $A420: the blocks it points at
  put(rom, 0x2500u, {0x08u, 0x11u, 0x08u, 0x22u, 0x08u, 0x33u, 0x08u, 0x44u, 0x00u});                            // $A500: a blob: four runs of eight, then the end
  put(rom, 0x2520u, {0x04u, 0x1Fu, 0x04u, 0x7Cu, 0x04u, 0xE0u, 0x04u, 0x03u,                                     // $A520: a blob: eight runs of four
                     0x04u, 0x7Fu, 0x04u, 0xFFu, 0x04u, 0x00u, 0x04u, 0x55u, 0x00u});
  for (std::size_t i = 0; i < 128; ++i) {                                                                       // $A600: Mode 7, a map in the even bytes and tiles in the odd
    rom[0x2600u + i] = static_cast<std::uint8_t>(i % 2u == 0u ? (i / 2u) & 0xFFu : (i * 71u + 3u) & 0xFFu);
  }
  return rom;
}

}  // namespace snaggletooth::examples
