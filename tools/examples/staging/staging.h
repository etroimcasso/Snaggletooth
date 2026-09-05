#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A cartridge that builds what it sends in work RAM first, every way the
// shadow has a rule for, then sends it. Reset goes native with the data bank
// at `$7F` and calls, in order: a run-length decoder that unpacks eleven bytes
// at `$9000` — five runs of one value each, then a zero — into thirty-two at
// `$7F:0000`; a copy of thirty-two bytes from `$9100` to `$7F:0100`; a loop that
// carries sixteen bytes from `$9200` to `CGDATA` one store at a time; a fill of
// sixteen bytes of `$AA` at `$7F:0300`; a transfer of thirty-two bytes from
// `$9300` into `$7E:0400` through the work-RAM port; two stores through the
// same port, of the first two bytes of `$9100`, into `$7E:0500`; and a copy of
// a three-byte HDMA table from `$9400` to `$7F:0600`. Then five transfers send
// each range to the hardware: `$7F:0000` and `$7F:0100` to VRAM, `$7F:0300` to
// OAM, `$7E:0400` to VRAM, `$7E:0500` to VRAM — and channel 1 walks the table
// in `$7F:0600` to `INIDISP` every frame while the program idles. Every site
// the tests name is commented with its address.
inline std::vector<std::uint8_t> stagingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE       -> native
      0xE2u, 0x30u,                // $8002 SEP #$30        A8, X8
      0xA9u, 0x7Fu,                // $8004 LDA #$7F
      0x48u, 0xABu,                // $8006 PHA / PLB       DBR = $7F
      0x20u, 0x00u, 0x81u,         // $8008 JSR !$8100      unpack $9000 -> $7F:0000
      0x20u, 0x40u, 0x81u,         // $800B JSR !$8140      copy $9100 -> $7F:0100
      0x20u, 0x80u, 0x81u,         // $800E JSR !$8180      stream $9200 -> CGDATA
      0x20u, 0xA0u, 0x81u,         // $8011 JSR !$81A0      fill $7F:0300 from a constant
      0x20u, 0xC0u, 0x81u,         // $8014 JSR !$81C0      transfer $9300 -> $7E:0400 through the port
      0x20u, 0x00u, 0x83u,         // $8017 JSR !$8300      store $9100, $9101 -> $7E:0500 through the port
      0x20u, 0x10u, 0x82u,         // $801A JSR !$8210      $7F:0000 -> VRAM
      0x20u, 0x50u, 0x82u,         // $801D JSR !$8250      $7F:0100 -> VRAM
      0x20u, 0x90u, 0x82u,         // $8020 JSR !$8290      $7F:0300 -> OAM
      0x20u, 0xD0u, 0x82u,         // $8023 JSR !$82D0      $7E:0400 -> VRAM
      0x20u, 0x40u, 0x83u,         // $8026 JSR !$8340      $7E:0500 -> VRAM
      0x20u, 0x80u, 0x83u,         // $8029 JSR !$8380      copy $9400 -> $7F:0600 and walk it to INIDISP
      0x80u, 0xFEu,                // $802C BRA *           idle while the frames walk the table
  });
  // The decoder: pairs of a count and a value until a count of zero. The count
  // goes to a counter in the direct page; only the value reaches the output.
  put(rom, 0x0100u, {
      0xC2u, 0x10u,                // $8100 REP #$10        sub_008100: X16, Y16
      0xA2u, 0x00u, 0x00u,         // $8102 LDX #$0000      the source index
      0xA0u, 0x00u, 0x00u,         // $8105 LDY #$0000      the output index
      0xBFu, 0x00u, 0x90u, 0x00u,  // $8108 LDA $00:9000,X  the count
      0xF0u, 0x12u,                // $810C BEQ $8120       zero ends it
      0x85u, 0x00u,                // $810E STA $00         the counter
      0xE8u,                       // $8110 INX
      0xBFu, 0x00u, 0x90u, 0x00u,  // $8111 LDA $00:9000,X  the value
      0xE8u,                       // $8115 INX
      0x99u, 0x00u, 0x00u,         // $8116 STA !$0000,Y    the output, at $7F:0000+Y
      0xC8u,                       // $8119 INY
      0xC6u, 0x00u,                // $811A DEC $00
      0xD0u, 0xF8u,                // $811C BNE $8116
      0x80u, 0xE8u,                // $811E BRA $8108
      0xE2u, 0x10u,                // $8120 SEP #$10
      0x60u,                       // $8122 RTS
  });
  put(rom, 0x0140u, {
      0xC2u, 0x10u,                // $8140 REP #$10        sub_008140
      0xA2u, 0x00u, 0x00u,         // $8142 LDX #$0000
      0xBFu, 0x00u, 0x91u, 0x00u,  // $8145 LDA $00:9100,X
      0x9Du, 0x00u, 0x01u,         // $8149 STA !$0100,X    $7F:0100+X
      0xE8u,                       // $814C INX
      0xE0u, 0x20u, 0x00u,         // $814D CPX #$0020
      0xD0u, 0xF3u,                // $8150 BNE $8145
      0xE2u, 0x10u,                // $8152 SEP #$10
      0x60u,                       // $8154 RTS
  });
  put(rom, 0x0180u, {
      0xA9u, 0x00u,                // $8180 LDA #$00        sub_008180
      0x8Fu, 0x21u, 0x21u, 0x00u,  // $8182 STA $00:2121    CGADD = 0
      0xC2u, 0x10u,                // $8186 REP #$10
      0xA2u, 0x00u, 0x00u,         // $8188 LDX #$0000
      0xBFu, 0x00u, 0x92u, 0x00u,  // $818B LDA $00:9200,X
      0x8Fu, 0x22u, 0x21u, 0x00u,  // $818F STA $00:2122    CGDATA: the stream's site
      0xE8u,                       // $8193 INX
      0xE0u, 0x10u, 0x00u,         // $8194 CPX #$0010
      0xD0u, 0xF2u,                // $8197 BNE $818B
      0xE2u, 0x10u,                // $8199 SEP #$10
      0x60u,                       // $819B RTS
  });
  put(rom, 0x01A0u, {
      0xC2u, 0x10u,                // $81A0 REP #$10        sub_0081A0
      0xA2u, 0x00u, 0x00u,         // $81A2 LDX #$0000
      0xA9u, 0xAAu,                // $81A5 LDA #$AA
      0x9Du, 0x00u, 0x03u,         // $81A7 STA !$0300,X    $7F:0300+X, from a constant
      0xE8u,                       // $81AA INX
      0xE0u, 0x10u, 0x00u,         // $81AB CPX #$0010
      0xD0u, 0xF7u,                // $81AE BNE $81A7
      0xE2u, 0x10u,                // $81B0 SEP #$10
      0x60u,                       // $81B2 RTS
  });
  // A transfer into work RAM through the port: the port's address, then the
  // channel, then the start.
  put(rom, 0x01C0u, {
      0xA9u, 0x00u,                // $81C0 LDA #$00        sub_0081C0
      0x8Fu, 0x81u, 0x21u, 0x00u,  // $81C2 STA $00:2181    WMADDL
      0xA9u, 0x04u,                // $81C6 LDA #$04
      0x8Fu, 0x82u, 0x21u, 0x00u,  // $81C8 STA $00:2182    WMADDM: $7E:0400
      0xA9u, 0x00u,                // $81CC LDA #$00
      0x8Fu, 0x83u, 0x21u, 0x00u,  // $81CE STA $00:2183    WMADDH
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $81D2 STA $00:4300    DMAP0 = $00: A->B, increment, pattern 0
      0xA9u, 0x80u,                // $81D6 LDA #$80
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $81D8 STA $00:4301    BBAD0 = $80: the port
      0xA9u, 0x00u,                // $81DC LDA #$00
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $81DE STA $00:4302    A1T0 low
      0xA9u, 0x93u,                // $81E2 LDA #$93
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $81E4 STA $00:4303    A1T0 high: $9300
      0xA9u, 0x00u,                // $81E8 LDA #$00
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $81EA STA $00:4304    A1B0
      0xA9u, 0x20u,                // $81EE LDA #$20
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $81F0 STA $00:4305    DAS0 low: 32
      0xA9u, 0x00u,                // $81F4 LDA #$00
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $81F6 STA $00:4306    DAS0 high
      0xA9u, 0x01u,                // $81FA LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $81FC STA $00:420B    MDMAEN (the write at $81FC)
      0x60u,                       // $8200 RTS
  });
  // The five transfers out: each sets one channel and starts it. The write to
  // `MDMAEN` is at the routine's start plus $2C.
  const auto transfer = [&](std::size_t at, std::uint8_t dmap, std::uint8_t bbad,
                            std::uint8_t low, std::uint8_t high, std::uint8_t bank,
                            std::uint8_t length) {
    put(rom, at, {
        0xA9u, dmap,                 // +$00 LDA #dmap
        0x8Fu, 0x00u, 0x43u, 0x00u,  // +$02 STA $00:4300    DMAP0
        0xA9u, bbad,                 // +$06 LDA #bbad
        0x8Fu, 0x01u, 0x43u, 0x00u,  // +$08 STA $00:4301    BBAD0
        0xA9u, low,                  // +$0C LDA #low
        0x8Fu, 0x02u, 0x43u, 0x00u,  // +$0E STA $00:4302    A1T0 low
        0xA9u, high,                 // +$12 LDA #high
        0x8Fu, 0x03u, 0x43u, 0x00u,  // +$14 STA $00:4303    A1T0 high
        0xA9u, bank,                 // +$18 LDA #bank
        0x8Fu, 0x04u, 0x43u, 0x00u,  // +$1A STA $00:4304    A1B0
        0xA9u, length,               // +$1E LDA #length
        0x8Fu, 0x05u, 0x43u, 0x00u,  // +$20 STA $00:4305    DAS0 low
        0xA9u, 0x00u,                // +$24 LDA #$00
        0x8Fu, 0x06u, 0x43u, 0x00u,  // +$26 STA $00:4306    DAS0 high
        0xA9u, 0x01u,                // +$2A LDA #$01
        0x8Fu, 0x0Bu, 0x42u, 0x00u,  // +$2C STA $00:420B    MDMAEN
        0x60u,                       // +$30 RTS
    });
  };
  transfer(0x0210u, 0x01u, 0x18u, 0x00u, 0x00u, 0x7Fu, 0x20u);  // $8210: $7F:0000, 32 -> VMDATAL (the write at $823C)
  transfer(0x0250u, 0x01u, 0x18u, 0x00u, 0x01u, 0x7Fu, 0x20u);  // $8250: $7F:0100, 32 -> VMDATAL (the write at $827C)
  transfer(0x0290u, 0x00u, 0x04u, 0x00u, 0x03u, 0x7Fu, 0x10u);  // $8290: $7F:0300, 16 -> OAMDATA (the write at $82BC)
  transfer(0x02D0u, 0x01u, 0x18u, 0x00u, 0x04u, 0x7Eu, 0x20u);  // $82D0: $7E:0400, 32 -> VMDATAL (the write at $82FC)
  // Two stores through the port, from the image.
  put(rom, 0x0300u, {
      0xA9u, 0x00u,                // $8300 LDA #$00        sub_008300
      0x8Fu, 0x81u, 0x21u, 0x00u,  // $8302 STA $00:2181    WMADDL
      0xA9u, 0x05u,                // $8306 LDA #$05
      0x8Fu, 0x82u, 0x21u, 0x00u,  // $8308 STA $00:2182    WMADDM: $7E:0500
      0xA9u, 0x00u,                // $830C LDA #$00
      0x8Fu, 0x83u, 0x21u, 0x00u,  // $830E STA $00:2183    WMADDH
      0xAFu, 0x00u, 0x91u, 0x00u,  // $8312 LDA $00:9100
      0x8Fu, 0x80u, 0x21u, 0x00u,  // $8316 STA $00:2180    -> $7E:0500
      0xAFu, 0x01u, 0x91u, 0x00u,  // $831A LDA $00:9101
      0x8Fu, 0x80u, 0x21u, 0x00u,  // $831E STA $00:2180    -> $7E:0501
      0x60u,                       // $8322 RTS
  });
  transfer(0x0340u, 0x01u, 0x18u, 0x00u, 0x05u, 0x7Eu, 0x02u);  // $8340: $7E:0500, 2 -> VMDATAL (the write at $836C)
  // The table copied into work RAM, then walked by HDMA: one line of one
  // value, then the end.
  put(rom, 0x0380u, {
      0xC2u, 0x10u,                // $8380 REP #$10        sub_008380
      0xA2u, 0x00u, 0x00u,         // $8382 LDX #$0000
      0xBFu, 0x00u, 0x94u, 0x00u,  // $8385 LDA $00:9400,X
      0x9Du, 0x00u, 0x06u,         // $8389 STA !$0600,X    $7F:0600+X
      0xE8u,                       // $838C INX
      0xE0u, 0x03u, 0x00u,         // $838D CPX #$0003
      0xD0u, 0xF3u,                // $8390 BNE $8385
      0xE2u, 0x10u,                // $8392 SEP #$10
      0xA9u, 0x00u,                // $8394 LDA #$00
      0x8Fu, 0x10u, 0x43u, 0x00u,  // $8396 STA $00:4310    DMAP1 = $00: direct, one register
      0x8Fu, 0x11u, 0x43u, 0x00u,  // $839A STA $00:4311    BBAD1 = $00: INIDISP
      0x8Fu, 0x12u, 0x43u, 0x00u,  // $839E STA $00:4312    A1T1 low
      0xA9u, 0x06u,                // $83A2 LDA #$06
      0x8Fu, 0x13u, 0x43u, 0x00u,  // $83A4 STA $00:4313    A1T1 high: $0600
      0xA9u, 0x7Fu,                // $83A8 LDA #$7F
      0x8Fu, 0x14u, 0x43u, 0x00u,  // $83AA STA $00:4314    A1B1 = $7F
      0xA9u, 0x02u,                // $83AE LDA #$02
      0x8Fu, 0x0Cu, 0x42u, 0x00u,  // $83B0 STA $00:420C    HDMAEN = $02 (the write at $83B0)
      0x60u,                       // $83B4 RTS
  });
  // The data.
  put(rom, 0x1000u, {0x08u, 0x11u, 0x08u, 0x22u, 0x04u, 0x33u, 0x04u, 0x44u, 0x08u, 0x55u, 0x00u});  // $9000: five runs, then the end
  for (std::size_t i = 0; i < 32; ++i) rom[0x1100u + i] = static_cast<std::uint8_t>(0x40u + i);  // $9100: copied whole
  for (std::size_t i = 0; i < 16; ++i) rom[0x1200u + i] = static_cast<std::uint8_t>(0xE0u + i);  // $9200: the palette streamed
  for (std::size_t i = 0; i < 32; ++i) rom[0x1300u + i] = static_cast<std::uint8_t>(0x70u + i);  // $9300: through the port
  put(rom, 0x1400u, {0x01u, 0x0Fu, 0x00u});  // $9400: the HDMA table: one line, brightness $0F, end
  return rom;
}

}  // namespace snaggletooth::examples
