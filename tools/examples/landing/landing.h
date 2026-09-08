#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset uploads to every video memory in forced blank, sets the screen mode and
// the bases afterwards, turns the screen on and idles; the vertical-blank
// handler then sends the sprite table every frame, flips a base behind two
// uploads, switches to Mode 7, and blanks the screen before one last upload.
// Every outcome of a `landed` line has a case, and the page's `landed` lines
// are this cartridge's.
//
// One bank. Channel 0 carries, from the image to VRAM through `VMDATAL` and
// `VMDATAH` in pairs: a 64-byte tileset to word `$3000`; a 64-byte map to
// `$0000`; sixty-four bytes to `$0FF0`, which end past the maps; thirty-two to
// `$5000`, which nothing addresses; a 64-byte sprite sheet to `$6000`;
// thirty-two bytes to `$2100` under the 8-bit address translation; and a fill
// of sixty-four bytes from the one byte at `$9C00` to `$5C00`. To CGRAM, a
// 32-byte palette from `$9500` at entry sixteen; to OAM, a 544-byte sprite
// table from `$A000` at byte zero; and thirty-two bytes from `$9B00` copied
// into work RAM through the port and sent from there to word `$0020`, a range
// the shadow names the source of. Then Mode 1 with BG1's screen at `$0000`,
// BG2's at `$0400` two screens wide, BG3's at `$0C00`, BG1's and BG2's tiles at
// `$1000`, BG3's at `$1000`, the sprites at `$6000` and, past a gap that wraps
// round the end of VRAM, at `$2000`; the screen on;
// the vertical-blank interrupt enabled. The handler counts frames and, while
// the count is under four, sends the sprite table again without writing the
// OAM address; on the second frame uploads a 64-byte map to `$7800` and
// another to `$0200`, then points BG2 at a four-screen map from `$7400`, whose
// last screen wraps round the end of VRAM and holds the second; on the third sets Mode 7, uploads sixty-four bytes
// to `$0000` and moves the OAM address to `$010` before that frame's sprite
// table; on the fourth turns forced blank on and uploads sixty-four bytes to
// `$0100`.
inline std::vector<std::uint8_t> landingImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x80u, 0x8Du, 0x15u, 0x21u,       // $8000 VMAIN = $80: increment after the high byte, by one word
      // ---- a tileset to $3000 ----
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $8005 VMADDL
      0xA9u, 0x30u, 0x8Du, 0x17u, 0x21u,       // $800A VMADDH: word $3000
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $800F DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8014 BBAD0 = $18: VMDATAL
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8019 A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $801E A1T0 high: $9000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8023 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $8028 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $802D DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8032 MDMAEN = $01 (the write at $8034)
      // ---- a map to $0000 ----
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $8037 VMADDL
      0xA9u, 0x00u, 0x8Du, 0x17u, 0x21u,       // $803C VMADDH: word $0000
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8041 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8046 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $804B A1T0 low
      0xA9u, 0x91u, 0x8Du, 0x03u, 0x43u,       // $8050 A1T0 high: $9100
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8055 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $805A DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $805F DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8064 MDMAEN = $01 (the write at $8066)
      // ---- sixty-four bytes from $0FF0, across the maps' end ----
      0xA9u, 0xF0u, 0x8Du, 0x16u, 0x21u,       // $8069 VMADDL
      0xA9u, 0x0Fu, 0x8Du, 0x17u, 0x21u,       // $806E VMADDH: word $0FF0
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8073 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8078 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $807D A1T0 low
      0xA9u, 0x92u, 0x8Du, 0x03u, 0x43u,       // $8082 A1T0 high: $9200
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8087 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $808C DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8091 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8096 MDMAEN = $01 (the write at $8098)
      // ---- thirty-two bytes to $5000, which nothing addresses ----
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $809B VMADDL
      0xA9u, 0x50u, 0x8Du, 0x17u, 0x21u,       // $80A0 VMADDH: word $5000
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $80A5 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $80AA BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $80AF A1T0 low
      0xA9u, 0x93u, 0x8Du, 0x03u, 0x43u,       // $80B4 A1T0 high: $9300
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $80B9 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $80BE DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $80C3 DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $80C8 MDMAEN = $01 (the write at $80CA)
      // ---- a sprite sheet to $6000 ----
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $80CD VMADDL
      0xA9u, 0x60u, 0x8Du, 0x17u, 0x21u,       // $80D2 VMADDH: word $6000
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $80D7 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $80DC BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $80E1 A1T0 low
      0xA9u, 0x94u, 0x8Du, 0x03u, 0x43u,       // $80E6 A1T0 high: $9400
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $80EB A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $80F0 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $80F5 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $80FA MDMAEN = $01 (the write at $80FC)
      // ---- a palette to entry sixteen ----
      0xA9u, 0x10u, 0x8Du, 0x21u, 0x21u,       // $80FF CGADD = $10
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $8104 DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x01u, 0x43u,       // $8109 BBAD0 = $22: CGDATA
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $810E A1T0 low
      0xA9u, 0x95u, 0x8Du, 0x03u, 0x43u,       // $8113 A1T0 high: $9500
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8118 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $811D DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8122 DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8127 MDMAEN = $01 (the write at $8129)
      // ---- a sprite table to OAM byte zero ----
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x21u,       // $812C OAMADDL = 0
      0xA9u, 0x00u, 0x8Du, 0x03u, 0x21u,       // $8131 OAMADDH = 0
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $8136 DMAP0 = $00
      0xA9u, 0x04u, 0x8Du, 0x01u, 0x43u,       // $813B BBAD0 = $04: OAMDATA
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8140 A1T0 low
      0xA9u, 0xA0u, 0x8Du, 0x03u, 0x43u,       // $8145 A1T0 high: $A000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $814A A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $814F DAS0 low
      0xA9u, 0x02u, 0x8Du, 0x06u, 0x43u,       // $8154 DAS0 high: 544
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8159 MDMAEN = $01 (the write at $815B)
      // ---- thirty-two bytes to $2100 under the 8-bit translation ----
      0xA9u, 0x84u, 0x8Du, 0x15u, 0x21u,       // $815E VMAIN = $84: the low eight bits of the address rotated
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $8163 VMADDL
      0xA9u, 0x21u, 0x8Du, 0x17u, 0x21u,       // $8168 VMADDH: word $2100
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $816D DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8172 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8177 A1T0 low
      0xA9u, 0x99u, 0x8Du, 0x03u, 0x43u,       // $817C A1T0 high: $9900
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8181 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8186 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $818B DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8190 MDMAEN = $01 (the write at $8192)
      0xA9u, 0x80u, 0x8Du, 0x15u, 0x21u,       // $8195 VMAIN = $80 again
      // ---- a fill of VRAM from one byte, to $5C00 ----
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $819A VMADDL
      0xA9u, 0x5Cu, 0x8Du, 0x17u, 0x21u,       // $819F VMADDH: word $5C00
      0xA9u, 0x09u, 0x8Du, 0x00u, 0x43u,       // $81A4 DMAP0 = $09: A->B, fixed, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $81A9 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $81AE A1T0 low
      0xA9u, 0x9Cu, 0x8Du, 0x03u, 0x43u,       // $81B3 A1T0 high: $9C00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $81B8 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $81BD DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $81C2 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $81C7 MDMAEN = $01 (the write at $81C9)
      // ---- thirty-two bytes staged through work RAM, then sent to $0020 ----
      0xA9u, 0x00u, 0x8Du, 0x81u, 0x21u,       // $81CC WMADDL
      0xA9u, 0x04u, 0x8Du, 0x82u, 0x21u,       // $81D1 WMADDM: the port at $7E:0400
      0xA9u, 0x00u, 0x8Du, 0x83u, 0x21u,       // $81D6 WMADDH
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $81DB DMAP0 = $00
      0xA9u, 0x80u, 0x8Du, 0x01u, 0x43u,       // $81E0 BBAD0 = $80: WMDATA
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $81E5 A1T0 low
      0xA9u, 0x9Bu, 0x8Du, 0x03u, 0x43u,       // $81EA A1T0 high: $9B00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $81EF A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $81F4 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $81F9 DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $81FE MDMAEN = $01 (the write at $8200)
      0xA9u, 0x20u, 0x8Du, 0x16u, 0x21u,       // $8203 VMADDL
      0xA9u, 0x00u, 0x8Du, 0x17u, 0x21u,       // $8208 VMADDH: word $0020
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $820D DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8212 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8217 A1T0 low
      0xA9u, 0x04u, 0x8Du, 0x03u, 0x43u,       // $821C A1T0 high: $0400
      0xA9u, 0x7Eu, 0x8Du, 0x04u, 0x43u,       // $8221 A1B0 = $7E: work RAM
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8226 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $822B DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8230 MDMAEN = $01 (the write at $8232)
      // ---- the mode and the bases, after every upload ----
      0xA9u, 0x01u, 0x8Du, 0x05u, 0x21u,       // $8235 BGMODE = $01: Mode 1
      0xA9u, 0x00u, 0x8Du, 0x07u, 0x21u,       // $823A BG1SC = $00: screen at $0000, one screen
      0xA9u, 0x05u, 0x8Du, 0x08u, 0x21u,       // $823F BG2SC = $05: screen at $0400, two screens wide
      0xA9u, 0x0Cu, 0x8Du, 0x09u, 0x21u,       // $8244 BG3SC = $0C: screen at $0C00, one screen
      0xA9u, 0x11u, 0x8Du, 0x0Bu, 0x21u,       // $8249 BG12NBA = $11: BG1's and BG2's tiles at $1000
      0xA9u, 0x01u, 0x8Du, 0x0Cu, 0x21u,       // $824E BG34NBA = $01: BG3's tiles at $1000
      0xA9u, 0x1Bu, 0x8Du, 0x01u, 0x21u,       // $8253 OBSEL = $1B: sprite tiles at $6000, the second half past a gap that wraps it round to $2000
      0xA9u, 0x0Fu, 0x8Du, 0x00u, 0x21u,       // $8258 INIDISP = $0F: the screen on
      0xA9u, 0x80u, 0x8Du, 0x00u, 0x42u,       // $825D NMITIMEN = $80: the vertical-blank interrupt on
      0x80u, 0xFEu,                            // $8262 BRA $8262
  });
  put(rom, 0x0310u, {                            // the emulation handler: the program stays in emulation mode
      0xEEu, 0x10u, 0x00u,                     // $8310 INC !$0010: the frame count
      0xADu, 0x10u, 0x00u,                     // $8313 LDA !$0010
      0xC9u, 0x02u,                            // $8316 CMP #$02
      0xD0u, 0x69u,                            // $8318 BNE $8383
      // ---- the second frame: a map to $7800, a map to $0200, then BG2 pointed at a screen over both ----
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $831A VMADDL
      0xA9u, 0x78u, 0x8Du, 0x17u, 0x21u,       // $831F VMADDH: word $7800
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8324 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8329 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $832E A1T0 low
      0xA9u, 0x97u, 0x8Du, 0x03u, 0x43u,       // $8333 A1T0 high: $9700
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8338 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $833D DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8342 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8347 MDMAEN = $01 (the write at $8349)
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $834C VMADDL
      0xA9u, 0x02u, 0x8Du, 0x17u, 0x21u,       // $8351 VMADDH: word $0200
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8356 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $835B BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8360 A1T0 low
      0xA9u, 0x9Du, 0x8Du, 0x03u, 0x43u,       // $8365 A1T0 high: $9D00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $836A A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $836F DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8374 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8379 MDMAEN = $01 (the write at $837B)
      0xA9u, 0x77u, 0x8Du, 0x08u, 0x21u,       // $837E BG2SC = $77: four screens from $7400, the last wrapping round the end of VRAM to $0000
      0xADu, 0x10u, 0x00u,                     // $8383 LDA !$0010
      0xC9u, 0x03u,                            // $8386 CMP #$03
      0xD0u, 0x3Cu,                            // $8388 BNE $83C6
      // ---- the third frame: Mode 7, sixty-four bytes to $0000, and the OAM address moved to $010 ----
      0xA9u, 0x07u, 0x8Du, 0x05u, 0x21u,       // $838A BGMODE = $07
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $838F VMADDL
      0xA9u, 0x00u, 0x8Du, 0x17u, 0x21u,       // $8394 VMADDH: word $0000
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8399 DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $839E BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $83A3 A1T0 low
      0xA9u, 0x98u, 0x8Du, 0x03u, 0x43u,       // $83A8 A1T0 high: $9800
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $83AD A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $83B2 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $83B7 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $83BC MDMAEN = $01 (the write at $83BE)
      0xA9u, 0x08u, 0x8Du, 0x02u, 0x21u,       // $83C1 OAMADDL = $08: the sprite table lands at $010 this frame
      0xADu, 0x10u, 0x00u,                     // $83C6 LDA !$0010
      0xC9u, 0x04u,                            // $83C9 CMP #$04
      0xD0u, 0x37u,                            // $83CB BNE $8404
      // ---- the fourth frame: forced blank, then sixty-four bytes to $0100 ----
      0xA9u, 0x80u, 0x8Du, 0x00u, 0x21u,       // $83CD INIDISP = $80: forced blank
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x21u,       // $83D2 VMADDL
      0xA9u, 0x01u, 0x8Du, 0x17u, 0x21u,       // $83D7 VMADDH: word $0100
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $83DC DMAP0 = $01
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $83E1 BBAD0 = $18
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $83E6 A1T0 low
      0xA9u, 0x9Au, 0x8Du, 0x03u, 0x43u,       // $83EB A1T0 high: $9A00
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $83F0 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $83F5 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $83FA DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $83FF MDMAEN = $01 (the write at $8401)
      // ---- every frame under four: the sprite table again, the OAM address not written ----
      0xADu, 0x10u, 0x00u,                     // $8404 LDA !$0010
      0xC9u, 0x04u,                            // $8407 CMP #$04
      0xB0u, 0x28u,                            // $8409 BCS $8433
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $840B DMAP0 = $00
      0xA9u, 0x04u, 0x8Du, 0x01u, 0x43u,       // $8410 BBAD0 = $04: OAMDATA
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8415 A1T0 low
      0xA9u, 0xA0u, 0x8Du, 0x03u, 0x43u,       // $841A A1T0 high: $A000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $841F A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8424 DAS0 low
      0xA9u, 0x02u, 0x8Du, 0x06u, 0x43u,       // $8429 DAS0 high: 544
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $842E MDMAEN = $01 (the write at $8430)
      0xADu, 0x10u, 0x42u,                     // $8433 LDA !$4210: acknowledge
      0x40u,                                   // $8436 RTI
  });
  for (std::size_t i = 0; i < 64; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x10u + i);   // the tileset
  for (std::size_t i = 0; i < 64; ++i) rom[0x1100u + i] = static_cast<std::uint8_t>(0x50u + i);   // the map
  for (std::size_t i = 0; i < 64; ++i) rom[0x1200u + i] = static_cast<std::uint8_t>(0x90u + i);   // across the maps' end
  for (std::size_t i = 0; i < 32; ++i) rom[0x1300u + i] = static_cast<std::uint8_t>(0xD0u + i);   // where nothing addresses
  for (std::size_t i = 0; i < 64; ++i) rom[0x1400u + i] = static_cast<std::uint8_t>(0x20u + i);   // the sprite sheet
  for (std::size_t i = 0; i < 32; ++i) rom[0x1500u + i] = static_cast<std::uint8_t>(0x60u + i);   // the palette
  for (std::size_t i = 0; i < 544; ++i) rom[0x2000u + i] = static_cast<std::uint8_t>((i * 5u) & 0xFFu);  // the sprite table
  for (std::size_t i = 0; i < 64; ++i) rom[0x1700u + i] = static_cast<std::uint8_t>(0x30u + i);   // the second frame's map, at $7800
  for (std::size_t i = 0; i < 64; ++i) rom[0x1D00u + i] = static_cast<std::uint8_t>(0xC0u + i);   // the second frame's map, at $0200
  for (std::size_t i = 0; i < 64; ++i) rom[0x1800u + i] = static_cast<std::uint8_t>(0x70u + i);   // the Mode 7 bytes
  for (std::size_t i = 0; i < 32; ++i) rom[0x1900u + i] = static_cast<std::uint8_t>(0xA0u + i);   // under the translation
  for (std::size_t i = 0; i < 64; ++i) rom[0x1A00u + i] = static_cast<std::uint8_t>(0xB0u + i);   // the last upload, never shown
  for (std::size_t i = 0; i < 32; ++i) rom[0x1B00u + i] = static_cast<std::uint8_t>(0xE0u + i);   // staged through work RAM
  rom[0x1C00u] = 0xAAu;                                                                           // the fill byte
  return rom;
}

}  // namespace snaggletooth::examples
