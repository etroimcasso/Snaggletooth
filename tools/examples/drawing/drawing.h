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
// vertical-blank interrupt on. The handler counts frames: on the first and
// the second it fills thirty-two bytes of work RAM from run-length blobs and
// sends them out, through one buffer, ten times — on the first, a blob of two
// parts unpacked one part at a time, each sent to word `$1300` inside BG1's
// name base (one source, two contents of one form); twenty-four bytes from
// one blob and eight from another at a lower address sent there together (one
// content built from two sources, unevenly); sixteen from each of two blobs
// sent there together (two sources, evenly); a second two-part blob whose
// first part goes to word `$5100` inside BG3's name base at two bits a pixel
// and whose second goes to `$1300` at four (one source, two depths); on the
// second, twenty-four bytes the routine clears itself and eight from a blob,
// sent together (a content mostly the routine's own); a buffer each byte of
// which adds two bytes of one blob to one of another (a byte from two
// sources, unevenly); then a blob to word `$1200` and a blob to the palette
// at entry sixteen; on the third it switches to Mode 7 and sends a block whose
// even bytes are a map and whose odd bytes are tiles to word `$0000`. After
// the reset code, with the interrupt held off for the upload's length, it
// speaks the audio upload protocol: a sound program to `$0200`, a sample
// directory and a two-block sample to `$0300`, from two places in the image,
// and starts the program, which sets the directory and two voices' sources,
// writes a second sample's two headers over the cleared memory at `$0330`, and
// keys both voices on at once — one sample the image holds, one it does not.
// Every site the tests name is commented with its address.
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
      0x4Cu, 0x00u, 0x82u,                     // $81B2 JMP $8200: the sound upload
  });
  // The sound upload, with the interrupt off while the byte index is in X —
  // the handler keeps nothing it uses — and on again once the program has
  // started: the protocol's ready bytes, the program to $0200 one acknowledged
  // byte at a time, the directory and the sample to $0300 the same way, then
  // the start. The data bank is $7E, so every port access is long.
  put(rom, 0x0200u, {
      0xA9u, 0x00u, 0x8Fu, 0x00u, 0x42u, 0x00u,  // $8200 NMITIMEN = $00
      0xAFu, 0x40u, 0x21u, 0x00u,              // $8206 LDA $00:2140     ; ready?
      0xC9u, 0xAAu,                            // $820A CMP #$AA
      0xD0u, 0xF8u,                            // $820C BNE $8206
      0xAFu, 0x41u, 0x21u, 0x00u,              // $820E LDA $00:2141
      0xC9u, 0xBBu,                            // $8212 CMP #$BB
      0xD0u, 0xF0u,                            // $8214 BNE $8206
      0xA9u, 0x00u, 0x8Fu, 0x42u, 0x21u, 0x00u,  // $8216 destination $0200, low
      0xA9u, 0x02u, 0x8Fu, 0x43u, 0x21u, 0x00u,  // $821C and high
      0xA9u, 0x01u, 0x8Fu, 0x41u, 0x21u, 0x00u,  // $8222 a transfer, not a start
      0xA9u, 0xCCu, 0x8Fu, 0x40u, 0x21u, 0x00u,  // $8228 the kick
      0xCFu, 0x40u, 0x21u, 0x00u,              // $822E CMP $00:2140     ; acknowledged?
      0xD0u, 0xFAu,                            // $8232 BNE $822E
      0xA2u, 0x00u,                            // $8234 LDX #$00
      0xBFu, 0x00u, 0xA7u, 0x00u,              // $8236 LDA $00:A700,X   ; the program's next byte
      0x8Fu, 0x41u, 0x21u, 0x00u,              // $823A STA $00:2141
      0x8Au,                                   // $823E TXA              ; its index
      0x8Fu, 0x40u, 0x21u, 0x00u,              // $823F STA $00:2140
      0xCFu, 0x40u, 0x21u, 0x00u,              // $8243 CMP $00:2140     ; acknowledged?
      0xD0u, 0xFAu,                            // $8247 BNE $8243
      0xE8u,                                   // $8249 INX
      0xE0u, 0x5Au,                            // $824A CPX #$5A         ; the program's 90 bytes
      0xD0u, 0xE8u,                            // $824C BNE $8236
      0xA9u, 0x00u, 0x8Fu, 0x42u, 0x21u, 0x00u,  // $824E destination $0300, low
      0xA9u, 0x03u, 0x8Fu, 0x43u, 0x21u, 0x00u,  // $8254 and high
      0xA9u, 0x01u, 0x8Fu, 0x41u, 0x21u, 0x00u,  // $825A a transfer
      0xA9u, 0x5Bu, 0x8Fu, 0x40u, 0x21u, 0x00u,  // $8260 two past the last index
      0xCFu, 0x40u, 0x21u, 0x00u,              // $8266 CMP $00:2140     ; acknowledged?
      0xD0u, 0xFAu,                            // $826A BNE $8266
      0xA2u, 0x00u,                            // $826C LDX #$00
      0xBFu, 0x80u, 0xA7u, 0x00u,              // $826E LDA $00:A780,X   ; the directory's and the sample's next byte
      0x8Fu, 0x41u, 0x21u, 0x00u,              // $8272 STA $00:2141
      0x8Au,                                   // $8276 TXA
      0x8Fu, 0x40u, 0x21u, 0x00u,              // $8277 STA $00:2140
      0xCFu, 0x40u, 0x21u, 0x00u,              // $827B CMP $00:2140
      0xD0u, 0xFAu,                            // $827F BNE $827B
      0xE8u,                                   // $8281 INX
      0xE0u, 0x1Au,                            // $8282 CPX #$1A         ; their 26 bytes
      0xD0u, 0xE8u,                            // $8284 BNE $826E
      0xA9u, 0x00u, 0x8Fu, 0x42u, 0x21u, 0x00u,  // $8286 start at $0200, low
      0xA9u, 0x02u, 0x8Fu, 0x43u, 0x21u, 0x00u,  // $828C and high
      0xA9u, 0x00u, 0x8Fu, 0x41u, 0x21u, 0x00u,  // $8292 zero starts the program
      0xA9u, 0x1Bu, 0x8Fu, 0x40u, 0x21u, 0x00u,  // $8298 two past the last index
      0xCFu, 0x40u, 0x21u, 0x00u,              // $829E CMP $00:2140     ; acknowledged?
      0xD0u, 0xFAu,                            // $82A2 BNE $829E
      0xA9u, 0x80u, 0x8Fu, 0x00u, 0x42u, 0x00u,  // $82A4 NMITIMEN = $80: the interrupt on again
      0x80u, 0xFEu,                            // $82AA BRA $82AA
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
      // ---- the second frame: the buffer filled and sent four times; the last two here, the first two in the routine ----
      0x20u, 0x80u, 0x84u,                     // $832C JSR !$8480      two fills sent to the tiles, then $A500 -> $7E:1000, 32 bytes
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
      0x80u, 0x52u,                            // $839E BRA $83F2
      0xADu, 0x10u, 0x00u,                     // $83A0 LDA !$0010
      0xC9u, 0x01u,                            // $83A3 CMP #$01
      0xD0u, 0x05u,                            // $83A5 BNE $83AC
      // ---- the first frame: the buffer filled and sent six times, all in the routine ----
      0x20u, 0x00u, 0x86u,                     // $83A7 JSR !$8600
      0x80u, 0x46u,                            // $83AA BRA $83F2
      0xC9u, 0x03u,                            // $83AC CMP #$03
      0xD0u, 0x42u,                            // $83AE BNE $83F2
      // ---- the third frame: Mode 7, and an interleaved block of 128 bytes to word $0000 ----
      0xA9u, 0x07u, 0x8Fu, 0x05u, 0x21u, 0x00u,  // $83B0 BGMODE = $07
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $83B6 VMADDL
      0xA9u, 0x00u, 0x8Fu, 0x17u, 0x21u, 0x00u,  // $83BC VMADDH: word $0000
      0xA9u, 0x01u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $83C2 DMAP0 = $01
      0xA9u, 0x18u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $83C8 BBAD0 = $18
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $83CE A1T0 low
      0xA9u, 0xA6u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $83D4 A1T0 high: $A600
      0xA9u, 0x00u, 0x8Fu, 0x04u, 0x43u, 0x00u,  // $83DA A1B0 = $00
      0xA9u, 0x80u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $83E0 DAS0 low: 128
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $83E6 DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $83EC MDMAEN = $01 (the write at $83EE)
      0xAFu, 0x10u, 0x42u, 0x00u,              // $83F2 LDA $00:4210: acknowledge
      0x40u,                                   // $83F6 RTI
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
  // The second frame's fills — the buffer cleared but for eight bytes from a
  // blob, and the sums of two blobs, each sent, then the blob at $A500
  // unpacked for the send in the handler — through the third decoder (X the
  // blob's address and Y the output cursor, both sixteen bits, each left where
  // it stopped so a call can go on from the last) and one send routine, whose
  // VRAM word's high byte is in the direct page at $01. The first frame's six
  // fills are the routine at $8600, since all ten would outlast the vertical
  // blank, and the picture drops what reaches VRAM outside it.
  put(rom, 0x0480u, {
      0xC2u, 0x10u,                // $8480 REP #$10        X16, Y16            sub_008480
      0xA9u, 0x13u,                // $8482 LDA #$13        the sends go to word $1300, BG1's name base
      0x85u, 0x01u,                // $8484 STA $01
      0xA2u, 0x00u, 0x00u,         // $8486 LDX #$0000      the buffer's first twenty-four bytes cleared by the routine itself
      0x9Eu, 0x00u, 0x10u,         // $8489 STZ !$1000,X
      0xE8u,                       // $848C INX
      0xE0u, 0x18u, 0x00u,         // $848D CPX #$0018
      0xD0u, 0xF7u,                // $8490 BNE $8489
      0xA0u, 0x18u, 0x00u,         // $8492 LDY #$0018      and its last eight from a blob
      0xA2u, 0xE0u, 0xA5u,         // $8495 LDX #$A5E0
      0x20u, 0x40u, 0x85u,         // $8498 JSR !$8540
      0x20u, 0x60u, 0x85u,         // $849B JSR !$8560
      0xE2u, 0x10u,                // $849E SEP #$10        X8, Y8
      0xA0u, 0x00u,                // $84A0 LDY #$00        a buffer each byte of which adds two bytes of one blob to one of another
      0x98u,                       // $84A2 TYA
      0x29u, 0x07u,                // $84A3 AND #$07
      0xAAu,                       // $84A5 TAX
      0xBFu, 0xE8u, 0xA5u, 0x00u,  // $84A6 LDA $00:A5E8,X  the first blob's byte
      0x18u,                       // $84AA CLC
      0x7Fu, 0xF4u, 0xA5u, 0x00u,  // $84AB ADC $00:A5F4,X  plus the second blob's
      0x85u, 0x02u,                // $84AF STA $02
      0xE8u,                       // $84B1 INX
      0x8Au,                       // $84B2 TXA
      0x29u, 0x07u,                // $84B3 AND #$07
      0xAAu,                       // $84B5 TAX
      0xBFu, 0xE8u, 0xA5u, 0x00u,  // $84B6 LDA $00:A5E8,X  plus the first blob's next
      0x18u,                       // $84BA CLC
      0x65u, 0x02u,                // $84BB ADC $02
      0x99u, 0x00u, 0x10u,         // $84BD STA !$1000,Y
      0xC8u,                       // $84C0 INY
      0xC0u, 0x20u,                // $84C1 CPY #$20
      0xD0u, 0xDDu,                // $84C3 BNE $84A2
      0x20u, 0x60u, 0x85u,         // $84C5 JSR !$8560      sent to word $1300
      0x20u, 0x00u, 0x84u,         // $84C8 JSR !$8400      unpack $A500 -> $7E:1000, 32 bytes
      0x60u,                       // $84CB RTS
  });
  put(rom, 0x0600u, {
      0xC2u, 0x10u,                // $8600 REP #$10        X16, Y16            sub_008600
      0xA9u, 0x13u,                // $8602 LDA #$13        the sends go to word $1300, BG1's name base
      0x85u, 0x01u,                // $8604 STA $01
      0xA2u, 0x40u, 0xA5u,         // $8606 LDX #$A540      the two-part blob
      0xA0u, 0x00u, 0x00u,         // $8609 LDY #$0000
      0x20u, 0x40u, 0x85u,         // $860C JSR !$8540      its first part -> 32 bytes
      0x20u, 0x60u, 0x85u,         // $860F JSR !$8560      sent
      0xA0u, 0x00u, 0x00u,         // $8612 LDY #$0000
      0x20u, 0x40u, 0x85u,         // $8615 JSR !$8540      its second part, X going on -> 32 bytes
      0x20u, 0x60u, 0x85u,         // $8618 JSR !$8560      sent again
      0xA2u, 0x80u, 0xA5u,         // $861B LDX #$A580      a blob of twenty-four
      0xA0u, 0x00u, 0x00u,         // $861E LDY #$0000
      0x20u, 0x40u, 0x85u,         // $8621 JSR !$8540
      0xA2u, 0x60u, 0xA5u,         // $8624 LDX #$A560      and one of eight, at the lower address, Y going on
      0x20u, 0x40u, 0x85u,         // $8627 JSR !$8540
      0x20u, 0x60u, 0x85u,         // $862A JSR !$8560      the two sent together
      0xA2u, 0xA0u, 0xA5u,         // $862D LDX #$A5A0      a blob of sixteen
      0xA0u, 0x00u, 0x00u,         // $8630 LDY #$0000
      0x20u, 0x40u, 0x85u,         // $8633 JSR !$8540
      0xA2u, 0xB0u, 0xA5u,         // $8636 LDX #$A5B0      and another of sixteen
      0x20u, 0x40u, 0x85u,         // $8639 JSR !$8540
      0x20u, 0x60u, 0x85u,         // $863C JSR !$8560      the two sent together
      0xA2u, 0xC0u, 0xA5u,         // $863F LDX #$A5C0      the two-part blob sent at two depths: its first part
      0xA0u, 0x00u, 0x00u,         // $8642 LDY #$0000
      0x20u, 0x40u, 0x85u,         // $8645 JSR !$8540
      0xA9u, 0x51u,                // $8648 LDA #$51        to word $5100, BG3's name base, two bits a pixel
      0x85u, 0x01u,                // $864A STA $01
      0x20u, 0x60u, 0x85u,         // $864C JSR !$8560
      0xA0u, 0x00u, 0x00u,         // $864F LDY #$0000
      0x20u, 0x40u, 0x85u,         // $8652 JSR !$8540      its second part
      0xA9u, 0x13u,                // $8655 LDA #$13        to word $1300 again, four bits
      0x85u, 0x01u,                // $8657 STA $01
      0x20u, 0x60u, 0x85u,         // $8659 JSR !$8560
      0xE2u, 0x10u,                // $865C SEP #$10        X8, Y8
      0x60u,                       // $865E RTS
  });
  put(rom, 0x0540u, {
      0xBFu, 0x00u, 0x00u, 0x00u,  // $8540 LDA $00:0000,X  the count, X the address    sub_008540
      0xF0u, 0x12u,                // $8544 BEQ $8558       zero ends the part
      0x85u, 0x00u,                // $8546 STA $00
      0xE8u,                       // $8548 INX
      0xBFu, 0x00u, 0x00u, 0x00u,  // $8549 LDA $00:0000,X  the value
      0xE8u,                       // $854D INX
      0x99u, 0x00u, 0x10u,         // $854E STA !$1000,Y    the output, at $7E:1000+Y
      0xC8u,                       // $8551 INY
      0xC6u, 0x00u,                // $8552 DEC $00
      0xD0u, 0xF8u,                // $8554 BNE $854E
      0x80u, 0xE8u,                // $8556 BRA $8540
      0xE8u,                       // $8558 INX             past the zero, to the next part
      0x60u,                       // $8559 RTS
  });
  put(rom, 0x0560u, {
      0xA9u, 0x00u, 0x8Fu, 0x16u, 0x21u, 0x00u,  // $8560 VMADDL                                  sub_008560
      0xA5u, 0x01u,                            // $8566 LDA $01
      0x8Fu, 0x17u, 0x21u, 0x00u,              // $8568 VMADDH: the word the caller chose
      0xA9u, 0x01u, 0x8Fu, 0x00u, 0x43u, 0x00u,  // $856C DMAP0 = $01
      0xA9u, 0x18u, 0x8Fu, 0x01u, 0x43u, 0x00u,  // $8572 BBAD0 = $18
      0xA9u, 0x00u, 0x8Fu, 0x02u, 0x43u, 0x00u,  // $8578 A1T0 low
      0xA9u, 0x10u, 0x8Fu, 0x03u, 0x43u, 0x00u,  // $857E A1T0 high: $1000
      0xA9u, 0x7Eu, 0x8Fu, 0x04u, 0x43u, 0x00u,  // $8584 A1B0 = $7E: work RAM
      0xA9u, 0x20u, 0x8Fu, 0x05u, 0x43u, 0x00u,  // $858A DAS0 low: 32
      0xA9u, 0x00u, 0x8Fu, 0x06u, 0x43u, 0x00u,  // $8590 DAS0 high
      0xA9u, 0x01u, 0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8596 MDMAEN = $01 (the write at $8598)
      0x60u,                                   // $859C RTS
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
  put(rom, 0x2540u, {0x08u, 0x12u, 0x08u, 0x34u, 0x08u, 0x56u, 0x08u, 0x78u, 0x00u,                              // $A540: a blob of two parts, four runs of eight each
                     0x08u, 0x9Au, 0x08u, 0xBCu, 0x08u, 0xDEu, 0x08u, 0xF0u, 0x00u});
  put(rom, 0x2560u, {0x08u, 0x87u, 0x00u});                                                                      // $A560: one run of eight
  put(rom, 0x2580u, {0x08u, 0x21u, 0x08u, 0x43u, 0x08u, 0x65u, 0x00u});                                          // $A580: three runs of eight
  put(rom, 0x25A0u, {0x08u, 0xA9u, 0x08u, 0xCBu, 0x00u});                                                        // $A5A0: two runs of eight
  put(rom, 0x25B0u, {0x08u, 0xEDu, 0x08u, 0x0Fu, 0x00u});                                                        // $A5B0: two runs of eight
  put(rom, 0x25C0u, {0x08u, 0x0Au, 0x08u, 0x0Bu, 0x08u, 0x0Cu, 0x08u, 0x0Du, 0x00u,                              // $A5C0: a blob of two parts, sent at two bits then at four
                     0x08u, 0x1Eu, 0x08u, 0x2Fu, 0x08u, 0x3Au, 0x08u, 0x4Bu, 0x00u});
  put(rom, 0x25E0u, {0x08u, 0xC3u, 0x00u});                                                                      // $A5E0: one run of eight, after twenty-four the routine cleared
  put(rom, 0x25E8u, {0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u, 0x80u});                                    // $A5E8: eight bytes, two of which go into every summed byte
  put(rom, 0x25F4u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u});                                    // $A5F4: eight bytes, one of which does
  for (std::size_t i = 0; i < 128; ++i) {                                                                       // $A600: Mode 7, a map in the even bytes and tiles in the odd
    rom[0x2600u + i] = static_cast<std::uint8_t>(i % 2u == 0u ? (i / 2u) & 0xFFu : (i * 71u + 3u) & 0xFFu);
  }
  // The sound program, 90 bytes, uploaded to $0200: the directory at $0300,
  // voice 0's source 0 and voice 1's source 1, voice 0 at full volume, both at
  // the sample's own rate under a direct full gain, the main volume up and the
  // DSP unmuted; then the second sample's two headers written over the zero
  // bytes the boot left at $0330 — a sample the image holds nowhere — and both
  // voices keyed on at once.
  put(rom, 0x2700u, {
      0x8Fu, 0x5Du, 0xF2u, 0x8Fu, 0x03u, 0xF3u,  // $0200 DIR = $03: the directory at $0300
      0x8Fu, 0x04u, 0xF2u, 0x8Fu, 0x00u, 0xF3u,  // $0206 V0SRCN = 0
      0x8Fu, 0x14u, 0xF2u, 0x8Fu, 0x01u, 0xF3u,  // $020C V1SRCN = 1
      0x8Fu, 0x00u, 0xF2u, 0x8Fu, 0x7Fu, 0xF3u,  // $0212 V0VOLL = $7F
      0x8Fu, 0x01u, 0xF2u, 0x8Fu, 0x7Fu, 0xF3u,  // $0218 V0VOLR = $7F
      0x8Fu, 0x03u, 0xF2u, 0x8Fu, 0x10u, 0xF3u,  // $021E V0PITCHH = $10: pitch $1000, the sample's own rate
      0x8Fu, 0x13u, 0xF2u, 0x8Fu, 0x10u, 0xF3u,  // $0224 V1PITCHH = $10
      0x8Fu, 0x07u, 0xF2u, 0x8Fu, 0x7Fu, 0xF3u,  // $022A V0GAIN = $7F: direct, full
      0x8Fu, 0x17u, 0xF2u, 0x8Fu, 0x7Fu, 0xF3u,  // $0230 V1GAIN = $7F
      0x8Fu, 0x0Cu, 0xF2u, 0x8Fu, 0x7Fu, 0xF3u,  // $0236 MVOLL = $7F
      0x8Fu, 0x1Cu, 0xF2u, 0x8Fu, 0x7Fu, 0xF3u,  // $023C MVOLR = $7F
      0x8Fu, 0x6Cu, 0xF2u, 0x8Fu, 0x20u, 0xF3u,  // $0242 FLG = $20: unmuted, echo writes off
      0xE8u, 0xC0u, 0xC5u, 0x30u, 0x03u,         // $0248 MOV A,#$C0 / MOV !$0330,A: the second sample's first header
      0xE8u, 0xC3u, 0xC5u, 0x39u, 0x03u,         // $024D MOV A,#$C3 / MOV !$0339,A: its last, with the end and loop flags
      0x8Fu, 0x4Cu, 0xF2u, 0x8Fu, 0x03u, 0xF3u,  // $0252 KON = $03: voices 0 and 1
      0x2Fu, 0xFEu,                              // $0258 BRA $0258
  });
  // The directory and the first sample, 26 bytes, uploaded to $0300: entry 0
  // starts at $0308 and loops at $0311, entry 1 at $0330 both; then the sample
  // of two blocks at $0308 — shift 11, filter 0, the second block carrying the
  // end and loop flags.
  put(rom, 0x2780u, {
      0x08u, 0x03u, 0x11u, 0x03u,                                            // $0300 entry 0: start $0308, loop $0311
      0x30u, 0x03u, 0x30u, 0x03u,                                            // $0304 entry 1: start $0330, loop $0330
      0xB0u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au, 0xBCu, 0xDEu, 0xF0u,         // $0308 the first block
      0xB3u, 0x0Fu, 0xEDu, 0xCBu, 0xA9u, 0x87u, 0x65u, 0x43u, 0x21u,         // $0311 the last block
  });
  return rom;
}

}  // namespace snaggletooth::examples
