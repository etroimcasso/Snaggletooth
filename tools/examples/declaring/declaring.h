#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset sets channel 0 up and starts it four times in one stretch of
// straight-line code, sets channel 6 up whole and never starts it, sends
// sixteen bytes on channel 7 from inside a block a later routine declares
// whole, and then reads the controller: only when a button is down does it
// call five routines that set channels 1 to 5 up whole and start them. The
// run's ports are empty, so the trace reaches the five routines and the run
// never does — every transfer they declare is one the code proves and the run
// never took.
//
// One bank, in emulation mode. Channel 0 carries 32 bytes from `$9000` to
// VRAM; a second start that rewrites only the source and the count carries
// sixteen from `$9040`; a palette is read downward from `$920F` to CGRAM; and
// VRAM is filled with 64 copies of the byte at `$9600`. Channel 6 is set up to
// carry sixteen bytes from `$9080` and never started; channel 7 carries the
// sixteen at `$9120` to VRAM. Behind the button: channel 1
// carries 48 bytes from `$9100` to VRAM; channel 2 carries the 544-byte sprite
// table at `$9300` to OAM; channel 3 fills VRAM from `$9600` with a count of
// zero, which is 65536; channel 4 has its source's low byte rewritten from a
// variable after it was written with a value, so its source is not proven;
// channel 5 is enabled as an HDMA table at `$9700` to `CGDATA` with a count
// written that the engine overwrites from the table.
inline std::vector<std::uint8_t> declaringImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      // ---- channel 0: a tileset, started, then a second start with only the source and the count rewritten ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8000 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8005 BBAD0 = $18 (the write at $8007)
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $800A A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $800F A1T0 high: $9000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8014 A1B0 = $00
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8019 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $801E DAS0 high: 32
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8023 MDMAEN = $01 (the write at $8025)
      0xA9u, 0x40u, 0x8Du, 0x02u, 0x43u,       // $8028 A1T0 low (the write at $802A)
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $802D A1T0 high: $9040
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8032 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8037 DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $803C MDMAEN = $01 (the write at $803E)
      // ---- channel 0: a palette read downward ----
      0xA9u, 0x10u, 0x8Du, 0x00u, 0x43u,       // $8041 DMAP0 = $10: A->B, decrement, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x01u, 0x43u,       // $8046 BBAD0 = $22 (the write at $8048)
      0xA9u, 0x0Fu, 0x8Du, 0x02u, 0x43u,       // $804B A1T0 low
      0xA9u, 0x92u, 0x8Du, 0x03u, 0x43u,       // $8050 A1T0 high: $920F
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8055 A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $805A DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $805F DAS0 high: 16
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8064 MDMAEN = $01 (the write at $8066)
      // ---- channel 0: a fill of VRAM from one byte ----
      0xA9u, 0x09u, 0x8Du, 0x00u, 0x43u,       // $8069 DMAP0 = $09: A->B, fixed, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $806E BBAD0 = $18 (the write at $8070)
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8073 A1T0 low
      0xA9u, 0x96u, 0x8Du, 0x03u, 0x43u,       // $8078 A1T0 high: $9600
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $807D A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,       // $8082 DAS0 low
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $8087 DAS0 high: 64
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $808C MDMAEN = $01 (the write at $808E)
      // ---- channel 6: set up whole, never started ----
      0xA9u, 0x01u, 0x8Du, 0x60u, 0x43u,       // $8091 DMAP6 = $01
      0xA9u, 0x18u, 0x8Du, 0x61u, 0x43u,       // $8096 BBAD6 = $18 (the write at $8098)
      0xA9u, 0x80u, 0x8Du, 0x62u, 0x43u,       // $809B A1T6 low
      0xA9u, 0x90u, 0x8Du, 0x63u, 0x43u,       // $80A0 A1T6 high: $9080
      0xA9u, 0x00u, 0x8Du, 0x64u, 0x43u,       // $80A5 A1B6 = $00
      0xA9u, 0x10u, 0x8Du, 0x65u, 0x43u,       // $80AA DAS6 low
      0xA9u, 0x00u, 0x8Du, 0x66u, 0x43u,       // $80AF DAS6 high: 16
      // ---- channel 7: sixteen bytes from inside channel 1's block, sent ----
      0xA9u, 0x01u, 0x8Du, 0x70u, 0x43u,       // $80B4 DMAP7 = $01
      0xA9u, 0x18u, 0x8Du, 0x71u, 0x43u,       // $80B9 BBAD7 = $18 (the write at $80BB)
      0xA9u, 0x20u, 0x8Du, 0x72u, 0x43u,       // $80BE A1T7 low
      0xA9u, 0x91u, 0x8Du, 0x73u, 0x43u,       // $80C3 A1T7 high: $9120
      0xA9u, 0x00u, 0x8Du, 0x74u, 0x43u,       // $80C8 A1B7 = $00
      0xA9u, 0x10u, 0x8Du, 0x75u, 0x43u,       // $80CD DAS7 low
      0xA9u, 0x00u, 0x8Du, 0x76u, 0x43u,       // $80D2 DAS7 high: 16
      0xA9u, 0x80u, 0x8Du, 0x0Bu, 0x42u,       // $80D7 MDMAEN = $80 (the write at $80D9)
      // ---- the controller: the routines below run only when a button is down ----
      0xA9u, 0x01u,                            // $80DC LDA #$01
      0x8Du, 0x16u, 0x40u,                     // $80DE STA !$4016   strobe high
      0x9Cu, 0x16u, 0x40u,                     // $80E1 STZ !$4016   strobe low
      0xADu, 0x16u, 0x40u,                     // $80E4 LDA !$4016   B
      0x29u, 0x01u,                            // $80E7 AND #$01
      0xF0u, 0x0Fu,                            // $80E9 BEQ $80FA
      0x20u, 0x00u, 0x81u,                     // $80EB JSR $8100
      0x20u, 0x40u, 0x81u,                     // $80EE JSR $8140
      0x20u, 0x80u, 0x81u,                     // $80F1 JSR $8180
      0x20u, 0xC0u, 0x81u,                     // $80F4 JSR $81C0
      0x20u, 0x00u, 0x82u,                     // $80F7 JSR $8200
      0x80u, 0xFEu,                            // $80FA BRA $80FA   idle
  });
  put(rom, 0x0100u, {
      // ---- channel 1: 48 bytes from $9100 to VRAM ----
      0xA9u, 0x01u, 0x8Du, 0x10u, 0x43u,       // $8100 DMAP1 = $01
      0xA9u, 0x18u, 0x8Du, 0x11u, 0x43u,       // $8105 BBAD1 = $18 (the write at $8107)
      0xA9u, 0x00u, 0x8Du, 0x12u, 0x43u,       // $810A A1T1 low
      0xA9u, 0x91u, 0x8Du, 0x13u, 0x43u,       // $810F A1T1 high: $9100
      0xA9u, 0x00u, 0x8Du, 0x14u, 0x43u,       // $8114 A1B1 = $00
      0xA9u, 0x30u, 0x8Du, 0x15u, 0x43u,       // $8119 DAS1 low
      0xA9u, 0x00u, 0x8Du, 0x16u, 0x43u,       // $811E DAS1 high: 48
      0xA9u, 0x02u, 0x8Du, 0x0Bu, 0x42u,       // $8123 MDMAEN = $02 (the write at $8125)
      0x60u,                                   // $8128 RTS
  });
  put(rom, 0x0140u, {
      // ---- channel 2: the 544-byte sprite table at $9300 to OAM ----
      0xA9u, 0x00u, 0x8Du, 0x20u, 0x43u,       // $8140 DMAP2 = $00
      0xA9u, 0x04u, 0x8Du, 0x21u, 0x43u,       // $8145 BBAD2 = $04 (the write at $8147)
      0xA9u, 0x00u, 0x8Du, 0x22u, 0x43u,       // $814A A1T2 low
      0xA9u, 0x93u, 0x8Du, 0x23u, 0x43u,       // $814F A1T2 high: $9300
      0xA9u, 0x00u, 0x8Du, 0x24u, 0x43u,       // $8154 A1B2 = $00
      0xA9u, 0x20u, 0x8Du, 0x25u, 0x43u,       // $8159 DAS2 low
      0xA9u, 0x02u, 0x8Du, 0x26u, 0x43u,       // $815E DAS2 high: 544
      0xA9u, 0x04u, 0x8Du, 0x0Bu, 0x42u,       // $8163 MDMAEN = $04 (the write at $8165)
      0x60u,                                   // $8168 RTS
  });
  put(rom, 0x0180u, {
      // ---- channel 3: a fill with a count of zero, which is 65536 ----
      0xA9u, 0x09u, 0x8Du, 0x30u, 0x43u,       // $8180 DMAP3 = $09: A->B, fixed, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x31u, 0x43u,       // $8185 BBAD3 = $18 (the write at $8187)
      0xA9u, 0x00u, 0x8Du, 0x32u, 0x43u,       // $818A A1T3 low
      0xA9u, 0x96u, 0x8Du, 0x33u, 0x43u,       // $818F A1T3 high: $9600
      0xA9u, 0x00u, 0x8Du, 0x34u, 0x43u,       // $8194 A1B3 = $00
      0xA9u, 0x00u, 0x8Du, 0x35u, 0x43u,       // $8199 DAS3 low
      0xA9u, 0x00u, 0x8Du, 0x36u, 0x43u,       // $819E DAS3 high: $0000
      0xA9u, 0x08u, 0x8Du, 0x0Bu, 0x42u,       // $81A3 MDMAEN = $08 (the write at $81A5)
      0x60u,                                   // $81A8 RTS
  });
  put(rom, 0x01C0u, {
      // ---- channel 4: the source's low byte rewritten from a variable ----
      0xA9u, 0x01u, 0x8Du, 0x40u, 0x43u,       // $81C0 DMAP4 = $01
      0xA9u, 0x18u, 0x8Du, 0x41u, 0x43u,       // $81C5 BBAD4 = $18 (the write at $81C7)
      0xA9u, 0x00u, 0x8Du, 0x42u, 0x43u,       // $81CA A1T4 low = $00, proven
      0xA9u, 0x98u, 0x8Du, 0x43u, 0x43u,       // $81CF A1T4 high: $98
      0xA9u, 0x00u, 0x8Du, 0x44u, 0x43u,       // $81D4 A1B4 = $00
      0xA5u, 0x10u,                            // $81D9 LDA $10        a variable
      0x8Du, 0x42u, 0x43u,                     // $81DB STA !$4342     A1T4 low again, from it
      0xA9u, 0x10u, 0x8Du, 0x45u, 0x43u,       // $81DE DAS4 low
      0xA9u, 0x00u, 0x8Du, 0x46u, 0x43u,       // $81E3 DAS4 high: 16
      0xA9u, 0x10u, 0x8Du, 0x0Bu, 0x42u,       // $81E8 MDMAEN = $10 (the write at $81EA)
      0x60u,                                   // $81ED RTS
  });
  put(rom, 0x0200u, {
      // ---- channel 5: an HDMA table to CGDATA, its count written and not a length ----
      0xA9u, 0x00u, 0x8Du, 0x50u, 0x43u,       // $8200 DMAP5 = $00: direct, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x51u, 0x43u,       // $8205 BBAD5 = $22 (the write at $8207)
      0xA9u, 0x00u, 0x8Du, 0x52u, 0x43u,       // $820A A1T5 low
      0xA9u, 0x97u, 0x8Du, 0x53u, 0x43u,       // $820F A1T5 high: the table at $9700
      0xA9u, 0x00u, 0x8Du, 0x54u, 0x43u,       // $8214 A1B5 = $00
      0xA9u, 0x03u, 0x8Du, 0x55u, 0x43u,       // $8219 DAS5 low
      0xA9u, 0x00u, 0x8Du, 0x56u, 0x43u,       // $821E DAS5 high: 3, which the engine overwrites
      0xA9u, 0x20u, 0x8Du, 0x0Cu, 0x42u,       // $8223 HDMAEN = $20 (the write at $8225)
      0x60u,                                   // $8228 RTS
  });
  for (std::size_t i = 0; i < 80; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x10u + i);   // the tileset and the sixteen after it
  for (std::size_t i = 0; i < 16; ++i) rom[0x1080u + i] = static_cast<std::uint8_t>(0xD0u + i);   // channel 6's, set up and never started
  for (std::size_t i = 0; i < 48; ++i) rom[0x1100u + i] = static_cast<std::uint8_t>(0x60u + i);   // channel 1's, declared and never sent
  for (std::size_t i = 0; i < 16; ++i) rom[0x1200u + i] = static_cast<std::uint8_t>(0xE0u + i);   // the palette
  for (std::size_t i = 0; i < 544; ++i) rom[0x1300u + i] = static_cast<std::uint8_t>((i * 3u) & 0xFFu);  // the sprite table, declared and never sent
  rom[0x1600u] = 0xAAu;                                                                           // the fill byte
  put(rom, 0x1700u, {0x01u, 0x0Fu, 0x00u});                                                       // $9700: one line of CGDATA = $0F, then stop
  for (std::size_t i = 0; i < 16; ++i) rom[0x1800u + i] = static_cast<std::uint8_t>(0xC0u + i);   // channel 4's, from a source the code does not prove
  return rom;
}

}  // namespace snaggletooth::examples
