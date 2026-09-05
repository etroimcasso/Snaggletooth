#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset sends bytes from the image to the hardware in every way the asset pass
// has a rule for, then enables one HDMA table and idles. Every file the pass
// lifts, every range it declines and every range it refuses has a case, and
// the pages' `asset` lines and `INCBIN` lines are this cartridge's.
//
// Two banks. Channel 0 carries a 64-byte tileset from `$9000` to VRAM, then
// sixteen bytes from inside it and thirty-two bytes across its end — three
// ranges sharing bytes, which are one file; a palette read downward from
// `$920F`; a 544-byte sprite table from `$9300`; eight bytes from `$9600` to
// the audio port; the same sixteen bytes at `$9800` to VRAM and then to CGRAM,
// which is a refusal; the reset routine's own first sixteen bytes to VRAM,
// another; a copy from `$9900` into work RAM through the port and a fill of VRAM
// from the one byte at `$9A00`, neither of which is an asset; a 32-byte tileset
// from `$01:8000`; thirty-two bytes from `$01:FFF0`, of which the last sixteen
// are read from `$01:0000` after the address wraps, which is not the image; a
// read of VRAM back into `$9B00`, which the image takes nothing of and the pass
// does not lift; and the same sixteen bytes at `$9C00` to `VMDATAL` and then to
// `VMDATAH`, a refusal of one class and two registers. Channel 1 walks an
// indirect HDMA table at `$9700` whose two entries point at two-byte blocks at
// `$9712` and `$9710`.
inline std::vector<std::uint8_t> liftingImage() {
  std::vector<std::uint8_t> rom = loRomImage(2);
  put(rom, 0x0000u, {
      // ---- channel 0: a 64-byte tileset, then a piece inside it and a piece across its end ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8000 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8005 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $800A A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $800F A1T0 high: $9000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8014 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $8019 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $801E DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8023 MDMAEN = $01 (the write at $8025)
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8028 DMAP0 = $01: the same, sixteen bytes inside the tileset
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $802D BBAD0 = $18
      0xA9u, 0x10u, 0x8Du, 0x02u, 0x43u,       // $8032 A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $8037 A1T0 high: $9010
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $803C A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8041 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8046 DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $804B MDMAEN = $01 (the write at $804D)
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8050 DMAP0 = $01: the same, thirty-two bytes across the tileset's end
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8055 BBAD0 = $18
      0xA9u, 0x30u, 0x8Du, 0x02u, 0x43u,       // $805A A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $805F A1T0 high: $9030
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8064 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8069 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $806E DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8073 MDMAEN = $01 (the write at $8075)
      // ---- channel 0: a palette read downward ----
      0xA9u, 0x10u, 0x8Du, 0x00u, 0x43u,       // $8078 DMAP0 = $10: A->B, decrement, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x01u, 0x43u,       // $807D BBAD0 = $22
      0xA9u, 0x0Fu, 0x8Du, 0x02u, 0x43u,       // $8082 A1T0 low
      0xA9u, 0x92u, 0x8Du, 0x03u, 0x43u,       // $8087 A1T0 high: $920F
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $808C A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8091 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8096 DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $809B MDMAEN = $01 (the write at $809D)
      // ---- channel 0: a sprite table from the image ----
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $80A0 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x04u, 0x8Du, 0x01u, 0x43u,       // $80A5 BBAD0 = $04
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $80AA A1T0 low
      0xA9u, 0x93u, 0x8Du, 0x03u, 0x43u,       // $80AF A1T0 high: $9300
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $80B4 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $80B9 DAS0 low
      0xA9u, 0x02u, 0x8Du, 0x06u, 0x43u,       // $80BE DAS0 high: 544
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $80C3 MDMAEN = $01 (the write at $80C5)
      // ---- channel 0: eight bytes to the audio port ----
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $80C8 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x40u, 0x8Du, 0x01u, 0x43u,       // $80CD BBAD0 = $40
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $80D2 A1T0 low
      0xA9u, 0x96u, 0x8Du, 0x03u, 0x43u,       // $80D7 A1T0 high: $9600
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $80DC A1B0 = $00
      0xA9u, 0x08u, 0x8Du, 0x05u, 0x43u,       // $80E1 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $80E6 DAS0 high: 8
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $80EB MDMAEN = $01 (the write at $80ED)
      // ---- channel 0: the same sixteen bytes to VRAM and then to CGRAM ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $80F0 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $80F5 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $80FA A1T0 low
      0xA9u, 0x98u, 0x8Du, 0x03u, 0x43u,       // $80FF A1T0 high: $9800
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8104 A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8109 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $810E DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8113 MDMAEN = $01 (the write at $8115)
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $8118 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x01u, 0x43u,       // $811D BBAD0 = $22
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8122 A1T0 low
      0xA9u, 0x98u, 0x8Du, 0x03u, 0x43u,       // $8127 A1T0 high: $9800
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $812C A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8131 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8136 DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $813B MDMAEN = $01 (the write at $813D)
      // ---- channel 0: the reset routine's own first sixteen bytes to VRAM ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8140 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8145 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $814A A1T0 low
      0xA9u, 0x80u, 0x8Du, 0x03u, 0x43u,       // $814F A1T0 high: $8000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8154 A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8159 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $815E DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8163 MDMAEN = $01 (the write at $8165)
      // ---- channel 0: a copy into work RAM through the port ----
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $8168 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x80u, 0x8Du, 0x01u, 0x43u,       // $816D BBAD0 = $80
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8172 A1T0 low
      0xA9u, 0x99u, 0x8Du, 0x03u, 0x43u,       // $8177 A1T0 high: $9900
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $817C A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8181 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8186 DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $818B MDMAEN = $01 (the write at $818D)
      // ---- channel 0: a fill of VRAM from one byte ----
      0xA9u, 0x09u, 0x8Du, 0x00u, 0x43u,       // $8190 DMAP0 = $09: A->B, fixed, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8195 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $819A A1T0 low
      0xA9u, 0x9Au, 0x8Du, 0x03u, 0x43u,       // $819F A1T0 high: $9A00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $81A4 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $81A9 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $81AE DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $81B3 MDMAEN = $01 (the write at $81B5)
      // ---- channel 0: a tileset in the second bank ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $81B8 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $81BD BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $81C2 A1T0 low
      0xA9u, 0x80u, 0x8Du, 0x03u, 0x43u,       // $81C7 A1T0 high: $8000
      0xA9u, 0x01u, 0x8Du, 0x04u, 0x43u,       // $81CC A1B0 = $01
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $81D1 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $81D6 DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $81DB MDMAEN = $01 (the write at $81DD)
      // ---- channel 0: a transfer that runs off the end of the second bank ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $81E0 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $81E5 BBAD0 = $18
      0xA9u, 0xF0u, 0x8Du, 0x02u, 0x43u,       // $81EA A1T0 low
      0xA9u, 0xFFu, 0x8Du, 0x03u, 0x43u,       // $81EF A1T0 high: $FFF0
      0xA9u, 0x01u, 0x8Du, 0x04u, 0x43u,       // $81F4 A1B0 = $01
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $81F9 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $81FE DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8203 MDMAEN = $01 (the write at $8205)
      // ---- channel 0: a read from VRAM back into the image, which takes nothing ----
      0xA9u, 0x81u, 0x8Du, 0x00u, 0x43u,       // $8208 DMAP0 = $81: B->A, increment, pattern 1
      0xA9u, 0x39u, 0x8Du, 0x01u, 0x43u,       // $820D BBAD0 = $39
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8212 A1T0 low
      0xA9u, 0x9Bu, 0x8Du, 0x03u, 0x43u,       // $8217 A1T0 high: $9B00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $821C A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8221 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8226 DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $822B MDMAEN = $01 (the write at $822D)
      // ---- channel 0: the same sixteen bytes to VMDATAL and then to VMDATAH ----
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $8230 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8235 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $823A A1T0 low
      0xA9u, 0x9Cu, 0x8Du, 0x03u, 0x43u,       // $823F A1T0 high: $9C00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8244 A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8249 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $824E DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8253 MDMAEN = $01 (the write at $8255)
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $8258 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x19u, 0x8Du, 0x01u, 0x43u,       // $825D BBAD0 = $19
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8262 A1T0 low
      0xA9u, 0x9Cu, 0x8Du, 0x03u, 0x43u,       // $8267 A1T0 high: $9C00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $826C A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8271 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8276 DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $827B MDMAEN = $01 (the write at $827D)
      // ---- channel 1: an indirect HDMA table to the palette port ----
      0xA9u, 0x41u, 0x8Du, 0x10u, 0x43u,       // $8280 DMAP1 = $41: indirect, pattern 1
      0xA9u, 0x21u, 0x8Du, 0x11u, 0x43u,       // $8285 BBAD1 = $21: CGADD then CGDATA
      0xA9u, 0x00u, 0x8Du, 0x12u, 0x43u,       // $828A A1T1 low
      0xA9u, 0x97u, 0x8Du, 0x13u, 0x43u,       // $828F A1T1 high: the table at $9700
      0xA9u, 0x00u, 0x8Du, 0x14u, 0x43u,       // $8294 A1B1 = $00
      0xA9u, 0x00u, 0x8Du, 0x17u, 0x43u,       // $8299 DASB1 = $00: the blocks' bank
      0xA9u, 0x02u, 0x8Du, 0x0Cu, 0x42u,       // $829E HDMAEN = $02 (the write at $82A0)
      0x80u, 0xFEu,                            // $82A3 BRA *: idle while the frames walk the table
  });
  for (std::size_t i = 0; i < 80; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x10u + i);   // the tileset and what follows it
  for (std::size_t i = 0; i < 16; ++i) rom[0x1200u + i] = static_cast<std::uint8_t>(0xE0u + i);   // the palette
  for (std::size_t i = 0; i < 544; ++i) rom[0x1300u + i] = static_cast<std::uint8_t>((i * 3u) & 0xFFu);  // the sprite table
  for (std::size_t i = 0; i < 8; ++i) rom[0x1600u + i] = static_cast<std::uint8_t>(0x50u + i);    // for the audio port
  put(rom, 0x1700u, {0x81u, 0x12u, 0x97u, 0x02u, 0x10u, 0x97u, 0x00u});  // $9700: -> $9712 on one line; -> $9710 for two; stop
  put(rom, 0x1710u, {0x00u, 0x1Fu});                                       // $9710: the second entry's block
  put(rom, 0x1712u, {0x01u, 0xE0u});                                       // $9712: the first entry's
  for (std::size_t i = 0; i < 16; ++i) rom[0x1800u + i] = static_cast<std::uint8_t>(0x60u + i);   // sent two places
  for (std::size_t i = 0; i < 32; ++i) rom[0x1900u + i] = static_cast<std::uint8_t>(0x70u + i);   // copied into work RAM
  rom[0x1A00u] = 0xAAu;                                                                           // the fill byte
  for (std::size_t i = 0; i < 16; ++i) rom[0x1B00u + i] = static_cast<std::uint8_t>(0xB0u + i);   // read back over, and untouched
  for (std::size_t i = 0; i < 16; ++i) rom[0x1C00u + i] = static_cast<std::uint8_t>(0xC0u + i);   // sent to two registers
  for (std::size_t i = 0; i < 32; ++i) rom[0x8000u + i] = static_cast<std::uint8_t>(0x80u + i);   // $01:8000: the second bank's tileset
  for (std::size_t i = 0; i < 16; ++i) rom[0xFFF0u + i] = static_cast<std::uint8_t>(0x90u + i);   // $01:FFF0: the bytes before the wrap
  return rom;
}

}  // namespace snaggletooth::examples
