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

TEST(SnesPpuStub, ObjectSelectAndScreenModeStore) {
  Snes m = run({
      0xA9, 0x63, 0x8D, 0x01, 0x21,  // OBSEL = $63
      0xA9, 0x09, 0x8D, 0x05, 0x21,  // BGMODE = $09
      0xA9, 0x35, 0x8D, 0x0C, 0x21,  // BG34NBA = $35
      0xDB,
  });
  EXPECT_EQ(m.state().objsel, 0x63u);
  EXPECT_EQ(m.state().bgmode, 0x09u);
  EXPECT_EQ(m.state().bg34nba, 0x35u);
}

// ---- OAM ------------------------------------------------------------------

TEST(SnesPpuStub, WritingTheOamAddressSetsTheReloadValueAndTheAddress) {
  // The reload value is the nine bits written (and the priority bit above
  // them); the address is the reload value doubled, so bit 0 is clear.
  Snes m = run({
      0xA9, 0x05, 0x8D, 0x02, 0x21,  // OAMADDL = $05
      0xA9, 0x81, 0x8D, 0x03, 0x21,  // OAMADDH = $81: the high bit and priority rotation
      0xDB,
  });
  EXPECT_EQ(m.state().oamadd, 0x8105u);
  EXPECT_EQ(m.state().oamAddress, 0x020Au);  // ($105 & $1FF) << 1
}

TEST(SnesPpuStub, OamDataWritesAWordThroughTheLatchAndStepsTheAddress) {
  Snes m = run({
      0xA9, 0x02, 0x8D, 0x02, 0x21,  // OAMADDL = 2 -> address 4
      0xA9, 0x00, 0x8D, 0x03, 0x21,  // OAMADDH = 0
      0xA9, 0x34, 0x8D, 0x04, 0x21,  // OAMDATA = $34: held, not written
      0xA9, 0x12, 0x8D, 0x04, 0x21,  // OAMDATA = $12: the word lands
      0xDB,
  });
  EXPECT_EQ(m.oam()[4], 0x34u);
  EXPECT_EQ(m.oam()[5], 0x12u);
  EXPECT_EQ(m.state().oamAddress, 6u);
}

TEST(SnesPpuStub, TheLowByteIsHeldUntilTheHighByteCommitsIt) {
  // One low byte written, then the address moved: the byte never lands.
  Snes m = run({
      0xA9, 0x00, 0x8D, 0x02, 0x21,  // OAMADDL = 0
      0xA9, 0x00, 0x8D, 0x03, 0x21,  // OAMADDH = 0
      0xA9, 0x99, 0x8D, 0x04, 0x21,  // OAMDATA = $99: held
      0xA9, 0x10, 0x8D, 0x02, 0x21,  // OAMADDL = $10: the address moves to $20
      0xA9, 0x34, 0x8D, 0x04, 0x21,  // OAMDATA = $34: held
      0xA9, 0x12, 0x8D, 0x04, 0x21,  // OAMDATA = $12: word $20 lands from the fresh pair
      0xDB,
  });
  EXPECT_EQ(m.oam()[0], 0x00u);
  EXPECT_EQ(m.oam()[0x20], 0x34u);
  EXPECT_EQ(m.oam()[0x21], 0x12u);
}

TEST(SnesPpuStub, OamDataAboveTheTableWritesTheByteAndTheTopMirrors) {
  // Above $1FF every write is one byte, and $220-$3FF mirror $200-$21F.
  Snes m = run({
      0xA9, 0x00, 0x8D, 0x02, 0x21,  // OAMADDL = 0
      0xA9, 0x01, 0x8D, 0x03, 0x21,  // OAMADDH = 1 -> address $200
      0xA9, 0xAA, 0x8D, 0x04, 0x21,  // OAMDATA = $AA -> $200
      0xA9, 0xBB, 0x8D, 0x04, 0x21,  // OAMDATA = $BB -> $201
      0xA9, 0x10, 0x8D, 0x02, 0x21,  // OAMADDL = $10 -> address $220, the mirror of $200
      0xA9, 0xCC, 0x8D, 0x04, 0x21,  // OAMDATA = $CC -> $200 again
      0xDB,
  });
  EXPECT_EQ(m.oam()[0x200], 0xCCu);
  EXPECT_EQ(m.oam()[0x201], 0xBBu);
  EXPECT_EQ(m.state().oamAddress, 0x221u);
}

TEST(SnesPpuStub, OamReadReturnsTheByteAndSteps) {
  Snes m = run({
      0xA9, 0x00, 0x8D, 0x02, 0x21,  // OAMADDL = 0
      0xA9, 0x00, 0x8D, 0x03, 0x21,  // OAMADDH = 0
      0xA9, 0xCD, 0x8D, 0x04, 0x21,  // low  = $CD
      0xA9, 0xAB, 0x8D, 0x04, 0x21,  // high = $AB
      0xA9, 0x00, 0x8D, 0x02, 0x21,  // OAMADDL = 0 again
      0xAD, 0x38, 0x21, 0x85, 0x60,  // LDA $2138; STA $60
      0xAD, 0x38, 0x21, 0x85, 0x61,  // LDA $2138; STA $61
      0xDB,
  });
  EXPECT_EQ(m.state().wram[0x60], 0xCDu);
  EXPECT_EQ(m.state().wram[0x61], 0xABu);
  EXPECT_EQ(m.state().oamAddress, 2u);
}

// A machine that has written two OAM bytes from address $20 (so the address
// stands at $22), with the screen as `inidisp` says, idling in a loop.
Snes oamMachine(std::uint8_t inidisp) {
  std::vector<std::uint8_t> rom = {
      0xA9, inidisp, 0x8D, 0x00, 0x21,  // INIDISP
      0xA9, 0x10, 0x8D, 0x02, 0x21,     // OAMADDL = $10 -> address $20
      0xA9, 0x00, 0x8D, 0x03, 0x21,     // OAMADDH = 0
      0xA9, 0x11, 0x8D, 0x04, 0x21,     // OAMDATA
      0xA9, 0x22, 0x8D, 0x04, 0x21,     // OAMDATA -> address $22
      0x80, 0xFE,                       // BRA *
  };
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  for (int i = 0; i < 10; ++i) m.step();  // the five loads and stores
  return m;
}

constexpr std::uint16_t kVblankStart = 225u;
constexpr std::uint32_t kLine = 1364u;

TEST(SnesPpuStub, TheOamAddressReloadsAtTheStartOfVblankWhenTheScreenIsOn) {
  Snes m = oamMachine(0x0Fu);
  EXPECT_EQ(m.state().oamAddress, 0x22u);
  while (m.state().vpos < kVblankStart) m.run(kLine);
  EXPECT_EQ(m.state().oamAddress, 0x20u) << "the reload value, doubled, at the start of vblank";
}

TEST(SnesPpuStub, TheOamAddressDoesNotReloadInForcedBlank) {
  Snes m = oamMachine(0x80u);
  while (m.state().vpos < kVblankStart) m.run(kLine);
  EXPECT_EQ(m.state().oamAddress, 0x22u);
}

// The machine of `oamMachine`, moved to line `vpos` in forced blank and about
// to run `LDA #$0F; STA INIDISP`.
Snes releasingMachine(std::uint16_t vpos) {
  std::vector<std::uint8_t> rom = {
      0xA9, 0x80, 0x8D, 0x00, 0x21,  // INIDISP = $80: forced blank
      0xA9, 0x10, 0x8D, 0x02, 0x21,  // OAMADDL = $10 -> address $20
      0xA9, 0x00, 0x8D, 0x03, 0x21,  // OAMADDH = 0
      0xA9, 0x11, 0x8D, 0x04, 0x21,  // OAMDATA
      0xA9, 0x22, 0x8D, 0x04, 0x21,  // OAMDATA -> address $22
      0xA9, 0x0F, 0x8D, 0x00, 0x21,  // INIDISP = $0F: forced blank released
      0xDB,
  };
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  for (int i = 0; i < 10; ++i) m.step();  // the five loads and stores
  SnesState s = m.state();
  s.vpos = vpos;
  s.hpos = 0;
  m.restore(s);
  m.step();  // LDA
  m.step();  // STA INIDISP
  return m;
}

TEST(SnesPpuStub, ReleasingForcedBlankDuringTheFirstVblankLineReloadsTheOamAddress) {
  Snes m = releasingMachine(kVblankStart);
  EXPECT_EQ(m.state().oamAddress, 0x20u);
}

TEST(SnesPpuStub, ReleasingForcedBlankOnAnyOtherLineLeavesTheOamAddressAlone) {
  Snes later = releasingMachine(kVblankStart + 1u);
  EXPECT_EQ(later.state().oamAddress, 0x22u);
  Snes earlier = releasingMachine(100u);
  EXPECT_EQ(earlier.state().oamAddress, 0x22u);
}

TEST(SnesPpuStub, WritingTheBrightnessWithTheScreenAlreadyOnDoesNotReload) {
  // The same write on line 225, but forced blank was already off: nothing
  // was released, so the address stands.
  std::vector<std::uint8_t> rom = {
      0xA9, 0x0F, 0x8D, 0x00, 0x21,  // INIDISP = $0F: the screen on
      0xA9, 0x10, 0x8D, 0x02, 0x21,  // OAMADDL = $10 -> address $20
      0xA9, 0x00, 0x8D, 0x03, 0x21,  // OAMADDH = 0
      0xA9, 0x11, 0x8D, 0x04, 0x21,  // OAMDATA
      0xA9, 0x22, 0x8D, 0x04, 0x21,  // OAMDATA -> address $22
      0xA9, 0x07, 0x8D, 0x00, 0x21,  // INIDISP = $07: the brightness alone
      0xDB,
  };
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  for (int i = 0; i < 10; ++i) m.step();
  SnesState s = m.state();
  s.vpos = kVblankStart;
  s.hpos = 0;
  m.restore(s);
  m.step();
  m.step();
  EXPECT_EQ(m.state().oamAddress, 0x22u);
}

}  // namespace
}  // namespace snaggletooth
