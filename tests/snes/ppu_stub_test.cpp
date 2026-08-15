// The PPU register-file stub: the VRAM port with its address increment, translation
// and read-prefetch glitch; the CGRAM port with its two-byte word and open-bus top
// bit; and the plain background and display registers. Nothing renders — programs
// drive the ports and the video memory is read back through the host faces.

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Runs a cartridge that ends in STP and returns the settled machine.
Snes run(std::vector<std::uint8_t> rom) {
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  return m;
}

// ---- VRAM -----------------------------------------------------------------

TEST(SnesPpuStub, VramWriteReachesVideoMemory) {
  // VMAIN = increment after the high byte, step 1; write one word at address $0010.
  Snes m = run({
      0xA9, 0x80, 0x8D, 0x15, 0x21,  // LDA #$80; STA $2115 (VMAIN)
      0xA9, 0x10, 0x8D, 0x16, 0x21,  // VMADDL = $10
      0xA9, 0x00, 0x8D, 0x17, 0x21,  // VMADDH = $00  -> word $0010
      0xA9, 0x34, 0x8D, 0x18, 0x21,  // VMDATAL = $34
      0xA9, 0x12, 0x8D, 0x19, 0x21,  // VMDATAH = $12 (word written, address steps)
      0xDB,
  });
  EXPECT_EQ(m.vram()[0x20], 0x34u);   // word $0010 -> bytes $20/$21
  EXPECT_EQ(m.vram()[0x21], 0x12u);
  EXPECT_EQ(m.state().vmadd, 0x0011u);  // stepped by one word
}

TEST(SnesPpuStub, VramAddressStepsByTheSelectedAmount) {
  // Step field 1 selects an increment of 32 words.
  Snes m = run({
      0xA9, 0x81, 0x8D, 0x15, 0x21,  // VMAIN = inc-after-high, step 32
      0xA9, 0x00, 0x8D, 0x16, 0x21,  // VMADDL = 0
      0xA9, 0x01, 0x8D, 0x17, 0x21,  // VMADDH = 1  -> word $0100
      0xA9, 0xAA, 0x8D, 0x18, 0x21,  // VMDATAL
      0xA9, 0xBB, 0x8D, 0x19, 0x21,  // VMDATAH (steps by 32)
      0xDB,
  });
  EXPECT_EQ(m.state().vmadd, 0x0120u);  // $0100 + 32
}

TEST(SnesPpuStub, VramAddressTranslationRotatesTheLowBits) {
  // 8-bit translation left-rotates the low byte of the word address by three, so
  // address $0001 reaches word $0008.
  Snes m = run({
      0xA9, 0x84, 0x8D, 0x15, 0x21,  // VMAIN = inc-after-high, step 1, 8-bit translation
      0xA9, 0x01, 0x8D, 0x16, 0x21,  // VMADDL = 1
      0xA9, 0x00, 0x8D, 0x17, 0x21,  // VMADDH = 0
      0xA9, 0xAA, 0x8D, 0x18, 0x21,  // VMDATAL
      0xA9, 0xBB, 0x8D, 0x19, 0x21,  // VMDATAH
      0xDB,
  });
  EXPECT_EQ(m.vram()[0x10], 0xAAu);     // word $0008 -> bytes $10/$11
  EXPECT_EQ(m.vram()[0x11], 0xBBu);
  EXPECT_EQ(m.state().vmadd, 0x0002u);  // the raw address steps; translation is only applied to the access
}

TEST(SnesPpuStub, VramReadPrefetchReturnsTheFirstWordTwice) {
  // Fill two words, then read the low byte three times from address zero. The
  // documented prefetch glitch hands back the first word twice before advancing.
  Snes m = run({
      0xA9, 0x80, 0x8D, 0x15, 0x21,  // VMAIN = inc-after-high (write two whole words)
      0xA9, 0x00, 0x8D, 0x16, 0x21,  // VMADDL = 0
      0xA9, 0x00, 0x8D, 0x17, 0x21,  // VMADDH = 0
      0xA9, 0x34, 0x8D, 0x18, 0x21,  // word0 low  = $34
      0xA9, 0x12, 0x8D, 0x19, 0x21,  // word0 high = $12 (addr -> 1)
      0xA9, 0x78, 0x8D, 0x18, 0x21,  // word1 low  = $78
      0xA9, 0x56, 0x8D, 0x19, 0x21,  // word1 high = $56 (addr -> 2)
      0xA9, 0x00, 0x8D, 0x15, 0x21,  // VMAIN = inc-after-low (a $2139 read steps)
      0xA9, 0x00, 0x8D, 0x16, 0x21,  // VMADDL = 0
      0xA9, 0x00, 0x8D, 0x17, 0x21,  // VMADDH = 0 (prefetch loads word0)
      0xAD, 0x39, 0x21, 0x85, 0x50,  // LDA $2139; STA $50  (read #1)
      0xAD, 0x39, 0x21, 0x85, 0x51,  // LDA $2139; STA $51  (read #2)
      0xAD, 0x39, 0x21, 0x85, 0x52,  // LDA $2139; STA $52  (read #3)
      0xDB,
  });
  EXPECT_EQ(m.state().wram[0x50], 0x34u);  // word0 low
  EXPECT_EQ(m.state().wram[0x51], 0x34u);  // word0 low again — the glitch
  EXPECT_EQ(m.state().wram[0x52], 0x78u);  // then word1 low
}

// ---- CGRAM ----------------------------------------------------------------

TEST(SnesPpuStub, CgramWriteTakesTwoBytesToMakeAWord) {
  Snes m = run({
      0xA9, 0x10, 0x8D, 0x21, 0x21,  // CGADD = $10
      0xA9, 0x34, 0x8D, 0x22, 0x21,  // CGDATA low  = $34 (held)
      0xA9, 0x12, 0x8D, 0x22, 0x21,  // CGDATA high = $12 (word committed, address steps)
      0xDB,
  });
  EXPECT_EQ(m.cgram()[0x20], 0x34u);    // word $10 -> bytes $20/$21
  EXPECT_EQ(m.cgram()[0x21], 0x12u);
  EXPECT_EQ(m.state().cgadd, 0x11u);    // stepped by one word
}

TEST(SnesPpuStub, CgramHighByteKeepsOnlySevenBits) {
  Snes m = run({
      0xA9, 0x00, 0x8D, 0x21, 0x21,  // CGADD = 0
      0xA9, 0xFF, 0x8D, 0x22, 0x21,  // low  = $FF
      0xA9, 0xFF, 0x8D, 0x22, 0x21,  // high = $FF -> stored as $7F
      0xDB,
  });
  EXPECT_EQ(m.cgram()[0x00], 0xFFu);
  EXPECT_EQ(m.cgram()[0x01], 0x7Fu);  // the top bit is dropped
}

TEST(SnesPpuStub, WritingTheCgramAddressResetsTheByteFlipFlop) {
  // A stray low byte, then a fresh address, then a full pair: the stray must not
  // pair with the fresh write.
  Snes m = run({
      0xA9, 0x05, 0x8D, 0x21, 0x21,  // CGADD = 5
      0xA9, 0x99, 0x8D, 0x22, 0x21,  // stray low byte $99
      0xA9, 0x05, 0x8D, 0x21, 0x21,  // CGADD = 5 again (resets the flip-flop)
      0xA9, 0x34, 0x8D, 0x22, 0x21,  // low  = $34
      0xA9, 0x12, 0x8D, 0x22, 0x21,  // high = $12
      0xDB,
  });
  EXPECT_EQ(m.cgram()[0x0A], 0x34u);  // word 5 -> bytes $0A/$0B, from the fresh pair
  EXPECT_EQ(m.cgram()[0x0B], 0x12u);
}

TEST(SnesPpuStub, CgramReadReturnsBothBytesInTurn) {
  Snes m = run({
      0xA9, 0x08, 0x8D, 0x21, 0x21,  // CGADD = 8
      0xA9, 0xCD, 0x8D, 0x22, 0x21,  // low  = $CD
      0xA9, 0x2B, 0x8D, 0x22, 0x21,  // high = $2B (word 8 committed, addr -> 9)
      0xA9, 0x08, 0x8D, 0x21, 0x21,  // CGADD = 8 (re-point, reset flip-flop)
      0xAD, 0x3B, 0x21, 0x85, 0x60,  // LDA $213B; STA $60  (low)
      0xAD, 0x3B, 0x21, 0x85, 0x61,  // LDA $213B; STA $61  (high)
      0xDB,
  });
  EXPECT_EQ(m.state().wram[0x60], 0xCDu);
  EXPECT_EQ(m.state().wram[0x61], 0x2Bu);  // top bit clear, so the raw value reads back
}

// ---- the plain display registers ------------------------------------------

TEST(SnesPpuStub, ForcedBlankAndBackgroundRegistersStore) {
  Snes m = run({
      0xA9, 0x0F, 0x8D, 0x00, 0x21,  // INIDISP = $0F (blank off, full brightness)
      0xA9, 0xAB, 0x8D, 0x07, 0x21,  // BG1SC = $AB
      0xA9, 0xCD, 0x8D, 0x0B, 0x21,  // BG12NBA = $CD
      0xA9, 0x13, 0x8D, 0x2C, 0x21,  // TM = $13
      0xDB,
  });
  EXPECT_EQ(m.state().inidisp, 0x0Fu);
  EXPECT_EQ(m.state().bg1sc, 0xABu);
  EXPECT_EQ(m.state().bg12nba, 0xCDu);
  EXPECT_EQ(m.state().tm, 0x13u);
}

TEST(SnesPpuStub, PowerOnStartsInForcedBlank) {
  Snes m = run({0xDB});  // STP immediately, touch nothing
  EXPECT_NE(m.state().inidisp & 0x80u, 0u);  // the screen is off at power-on
}

}  // namespace
}  // namespace snaggletooth
