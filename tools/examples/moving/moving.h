#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset moves bytes every way the transfer engines can, then idles while the
// vertical-blank handler sends a sprite table every frame. Every range the run
// records has a case, and the page's `moved` lines are this cartridge's.
//
// Channel 0 carries a 32-byte tileset from the image to VRAM, then fills 64
// bytes of VRAM from one image byte; channel 1 reads 16 bytes of VRAM back into
// work RAM; channels 2 and 3 are started by one write — 16 palette bytes read
// downward into CGRAM, and 8 bytes to a B-bus address no register has. Channel 4
// runs a direct HDMA table to the brightness register, channel 5 an indirect one
// to the palette port whose two blocks lie in the same bank as the table, the
// second entry's block ending where the first entry's begins, and channel 6 a
// 129-byte direct table the program first writes into work RAM. Channel 0 then
// carries a 48-byte tileset in three chunks: two started by one instruction in
// a subroutine, the third by a store to the start register through a mirror
// bank. Channel 7 is the sprite table: 544 bytes from work RAM to OAM, started
// from the handler on every frame.
inline std::vector<std::uint8_t> movingImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      // ---- channel 0: a tileset, image -> VRAM ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,   // $8000 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,   // $8005 BBAD0 = $18: VMDATAL
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,   // $800A A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,   // $800F A1T0 high: $9000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,   // $8014 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,   // $8019 DAS0 low = 32
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $801E DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,   // $8023 MDMAEN = $01     (the write at $8025)
      // ---- channel 0 again: a fill from one byte ----
      0xA9u, 0x09u, 0x8Du, 0x00u, 0x43u,   // $8028 DMAP0 = $09: A->B, fixed, pattern 1
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,   // $802D A1T0 low
      0xA9u, 0x91u, 0x8Du, 0x03u, 0x43u,   // $8032 A1T0 high: $9100
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,   // $8037 DAS0 low = 64
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $803C DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,   // $8041 MDMAEN = $01     (the write at $8043)
      // ---- channel 1: VRAM read back into work RAM ----
      0xA9u, 0x81u, 0x8Du, 0x10u, 0x43u,   // $8046 DMAP1 = $81: B->A, increment, pattern 1
      0xA9u, 0x39u, 0x8Du, 0x11u, 0x43u,   // $804B BBAD1 = $39: VMDATALREAD
      0xA9u, 0x00u, 0x8Du, 0x12u, 0x43u,   // $8050 A1T1 low
      0xA9u, 0x03u, 0x8Du, 0x13u, 0x43u,   // $8055 A1T1 high: $0300
      0xA9u, 0x7Eu, 0x8Du, 0x14u, 0x43u,   // $805A A1B1 = $7E
      0xA9u, 0x10u, 0x8Du, 0x15u, 0x43u,   // $805F DAS1 low = 16
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x43u,   // $8064 DAS1 high
      0xA9u, 0x02u, 0x8Du, 0x0Bu, 0x42u,   // $8069 MDMAEN = $02     (the write at $806B)
      // ---- channel 2: a palette, read downward ----
      0xA9u, 0x10u, 0x8Du, 0x20u, 0x43u,   // $806E DMAP2 = $10: A->B, decrement, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x21u, 0x43u,   // $8073 BBAD2 = $22: CGDATA
      0xA9u, 0x0Fu, 0x8Du, 0x22u, 0x43u,   // $8078 A1T2 low
      0xA9u, 0x92u, 0x8Du, 0x23u, 0x43u,   // $807D A1T2 high: $920F, the last byte first
      0xA9u, 0x00u, 0x8Du, 0x24u, 0x43u,   // $8082 A1B2 = $00
      0xA9u, 0x10u, 0x8Du, 0x25u, 0x43u,   // $8087 DAS2 low = 16
      0xA9u, 0x00u, 0x8Du, 0x26u, 0x43u,   // $808C DAS2 high
      // ---- channel 3: to a B-bus address no register has ----
      0xA9u, 0x00u, 0x8Du, 0x30u, 0x43u,   // $8091 DMAP3 = $00: A->B, increment, pattern 0
      0xA9u, 0x50u, 0x8Du, 0x31u, 0x43u,   // $8096 BBAD3 = $50: $2150
      0xA9u, 0x00u, 0x8Du, 0x32u, 0x43u,   // $809B A1T3 low
      0xA9u, 0x94u, 0x8Du, 0x33u, 0x43u,   // $80A0 A1T3 high: $9400
      0xA9u, 0x00u, 0x8Du, 0x34u, 0x43u,   // $80A5 A1B3 = $00
      0xA9u, 0x08u, 0x8Du, 0x35u, 0x43u,   // $80AA DAS3 low = 8
      0xA9u, 0x00u, 0x8Du, 0x36u, 0x43u,   // $80AF DAS3 high
      0xA9u, 0x0Cu, 0x8Du, 0x0Bu, 0x42u,   // $80B4 MDMAEN = $0C: channels 2 and 3 (the write at $80B6)
      0xA9u, 0x00u, 0x8Du, 0x0Bu, 0x42u,   // $80B9 MDMAEN = $00: nothing (the write at $80BB)
      // ---- channel 4: a direct HDMA table to the brightness register ----
      0xA9u, 0x00u, 0x8Du, 0x40u, 0x43u,   // $80BE DMAP4 = $00: direct, pattern 0
      0xA9u, 0x00u, 0x8Du, 0x41u, 0x43u,   // $80C3 BBAD4 = $00: INIDISP
      0xA9u, 0x00u, 0x8Du, 0x42u, 0x43u,   // $80C8 A1T4 low
      0xA9u, 0x95u, 0x8Du, 0x43u, 0x43u,   // $80CD A1T4 high: the table at $9500
      0xA9u, 0x00u, 0x8Du, 0x44u, 0x43u,   // $80D2 A1B4 = $00
      // ---- channel 5: an indirect HDMA table to the palette port ----
      0xA9u, 0x41u, 0x8Du, 0x50u, 0x43u,   // $80D7 DMAP5 = $41: indirect, pattern 1
      0xA9u, 0x21u, 0x8Du, 0x51u, 0x43u,   // $80DC BBAD5 = $21: CGADD then CGDATA
      0xA9u, 0x10u, 0x8Du, 0x52u, 0x43u,   // $80E1 A1T5 low
      0xA9u, 0x95u, 0x8Du, 0x53u, 0x43u,   // $80E6 A1T5 high: the table at $9510
      0xA9u, 0x00u, 0x8Du, 0x54u, 0x43u,   // $80EB A1B5 = $00
      0xA9u, 0x00u, 0x8Du, 0x57u, 0x43u,   // $80F0 DASB5 = $00: the blocks' bank
      0xA9u, 0x30u, 0x8Du, 0x0Cu, 0x42u,   // $80F5 HDMAEN = $30: channels 4 and 5 (the write at $80F7)
      // ---- channel 6: a direct table written into work RAM first ----
      0xA2u, 0x00u,                        // $80FA LDX #$00
      0x8Au,                               // $80FC TXA
      0x9Du, 0x01u, 0x04u,                 // $80FD STA !$0401,X   the values 0..255 at $0401
      0xE8u,                               // $8100 INX
      0xD0u, 0xF9u,                        // $8101 BNE $80FC
      0xA9u, 0xFFu, 0x8Du, 0x00u, 0x04u,   // $8103 $0400 = $FF: repeat on 127 lines
      0xA9u, 0x00u, 0x8Du, 0x80u, 0x04u,   // $8108 $0480 = $00: stop
      0xA9u, 0x00u, 0x8Du, 0x60u, 0x43u,   // $810D DMAP6 = $00: direct, pattern 0
      0xA9u, 0x00u, 0x8Du, 0x61u, 0x43u,   // $8112 BBAD6 = $00: INIDISP
      0xA9u, 0x00u, 0x8Du, 0x62u, 0x43u,   // $8117 A1T6 low
      0xA9u, 0x04u, 0x8Du, 0x63u, 0x43u,   // $811C A1T6 high: the table at $0400
      0xA9u, 0x7Eu, 0x8Du, 0x64u, 0x43u,   // $8121 A1B6 = $7E
      0xA9u, 0x70u, 0x8Du, 0x0Cu, 0x42u,   // $8126 HDMAEN = $70: channel 6 joins 4 and 5 (the write at $8128)
      // ---- channel 7: the sprite table, sent from the handler ----
      0xA9u, 0x00u, 0x8Du, 0x70u, 0x43u,   // $812B DMAP7 = $00: A->B, increment, pattern 0
      0xA9u, 0x04u, 0x8Du, 0x71u, 0x43u,   // $8130 BBAD7 = $04: OAMDATA
      0xA9u, 0x7Eu, 0x8Du, 0x74u, 0x43u,   // $8135 A1B7 = $7E
      // ---- channel 0 again: a tileset in three chunks ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,   // $813A DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,   // $813F A1T0 low
      0xA9u, 0x96u, 0x8Du, 0x03u, 0x43u,   // $8144 A1T0 high: $9600
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,   // $8149 DAS0 low = 16
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $814E DAS0 high
      0x20u, 0x8Eu, 0x81u,                 // $8153 JSR send
      0xA9u, 0x10u, 0x8Du, 0x02u, 0x43u,   // $8156 A1T0 low: $9610, where the first chunk ended
      0xA9u, 0x96u, 0x8Du, 0x03u, 0x43u,   // $815B A1T0 high
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,   // $8160 DAS0 low = 16
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $8165 DAS0 high
      0x20u, 0x8Eu, 0x81u,                 // $816A JSR send
      0xA9u, 0x20u, 0x8Du, 0x02u, 0x43u,   // $816D A1T0 low: $9620, where the second ended
      0xA9u, 0x96u, 0x8Du, 0x03u, 0x43u,   // $8172 A1T0 high
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,   // $8177 DAS0 low = 16
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $817C DAS0 high
      0xA9u, 0x01u,                        // $8181 LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x80u,          // $8183 STA >$80:420B: MDMAEN through a mirror bank
      0xA9u, 0x80u, 0x8Du, 0x00u, 0x42u,   // $8187 NMITIMEN = $80: NMI on
      0x80u, 0xFEu,                        // $818C BRA $818C
      0xA9u, 0x01u,                        // $818E send: LDA #$01
      0x8Du, 0x0Bu, 0x42u,                 // $8190 STA !MDMAEN      (the write at $8190, twice)
      0x60u,                               // $8193 RTS
  });
  put(rom, 0x0310u, {                        // the emulation handler: the program stays in emulation mode
      0xA9u, 0x00u, 0x8Du, 0x72u, 0x43u,   // $8310 A1T7 low
      0xA9u, 0x02u, 0x8Du, 0x73u, 0x43u,   // $8315 A1T7 high: $0200
      0xA9u, 0x20u, 0x8Du, 0x75u, 0x43u,   // $831A DAS7 low
      0xA9u, 0x02u, 0x8Du, 0x76u, 0x43u,   // $831F DAS7 high: 544 bytes
      0xA9u, 0x80u, 0x8Du, 0x0Bu, 0x42u,   // $8324 MDMAEN = $80   (the write at $8326)
      0xADu, 0x10u, 0x42u,                 // $8329 LDA !$4210     acknowledge
      0x40u,                               // $832C RTI
  });
  for (std::size_t i = 0; i < 32; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x10u + i);  // the tileset
  rom[0x1100u] = 0xAAu;                                                                          // the fill byte
  for (std::size_t i = 0; i < 16; ++i) rom[0x1200u + i] = static_cast<std::uint8_t>(0xE0u + i);  // the palette
  for (std::size_t i = 0; i < 8; ++i) rom[0x1400u + i] = static_cast<std::uint8_t>(0x50u + i);
  put(rom, 0x1500u, {0x02u, 0x0Au, 0x01u, 0x0Bu, 0x00u});          // $9500: $0A, wait two lines; $0B, wait one; stop
  put(rom, 0x1510u, {0x81u, 0x22u, 0x95u, 0x02u, 0x20u, 0x95u, 0x00u});  // $9510: -> $9522 on one line; -> $9520 once; stop
  put(rom, 0x1520u, {0x00u, 0x1Fu});                               // $9520: the second entry's block
  put(rom, 0x1522u, {0x01u, 0xE0u});                               // $9522: the first entry's, right after it
  for (std::size_t i = 0; i < 48; ++i) rom[0x1600u + i] = static_cast<std::uint8_t>(0x70u + i);  // the chunked tileset
  return rom;
}

}  // namespace snaggletooth::examples
