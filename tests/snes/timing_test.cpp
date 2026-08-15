// The SNES machine's timing and interrupts: the H/V counters over the scanline
// structure, the vblank flag, the frame-parity short line, the vblank NMI, and the
// H/V-timer IRQ. Positions are driven by running the machine an exact number of
// master cycles; interrupts are observed by the handler's effect on work RAM.

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// A machine whose CPU is halted with the master and video counters zeroed, so run()
// advances it in exact six-cycle idle steps from the frame origin. A budget that is a
// whole multiple of six therefore lands on an exact master total, and the video
// position is a pure function of it.
Snes stoppedMachine(Region region = Region::Ntsc) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xDBu;         // STP
  rom[0x7FFCu] = 0x00u;   // reset vector -> $8000
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom, .region = region});
  m.step();               // execute the STP so the core is halted
  SnesState s = m.state();
  s.master = 0; s.consumed = 0; s.apuPhase = 0;  // start the clock at the frame origin
  s.hpos = 0; s.vpos = 0; s.field = 0;
  m.restore(s);
  return m;
}

// A machine that runs `program` from $8000 with the given interrupt vectors, used to
// observe an NMI or IRQ handler's effect. A zero vector is left as the loader set it.
Snes vectoredMachine(std::initializer_list<std::uint8_t> program,
                     std::uint16_t nmi = 0, std::uint16_t irq = 0) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;   // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  if (nmi != 0) { rom[0x7FFAu] = nmi & 0xFFu; rom[0x7FFBu] = nmi >> 8; }
  if (irq != 0) { rom[0x7FFEu] = irq & 0xFFu; rom[0x7FFFu] = irq >> 8; }
  return Snes(SnesConfig{.rom = rom});
}

constexpr std::uint32_t kLine = 1364u;

// ---- the H/V counters -----------------------------------------------------

TEST(SnesTiming, VerticalCounterAdvancesOneLinePerScanline) {
  Snes m = stoppedMachine();
  m.run(9u * kLine);  // nine whole scanlines (a multiple of three lines lands on six)
  EXPECT_EQ(m.state().vpos, 9u);
  EXPECT_EQ(m.state().hpos, 0u);
}

TEST(SnesTiming, HorizontalPositionIsTheMasterCycleWithinTheLine) {
  Snes m = stoppedMachine();
  m.run(3u * kLine + 600u);  // three lines and 600 master cycles in
  EXPECT_EQ(m.state().vpos, 3u);
  EXPECT_EQ(m.state().hpos, 600u);
}

// ---- vblank ---------------------------------------------------------------

TEST(SnesTiming, VblankFlagIsClearThroughTheVisiblePicture) {
  Snes m = stoppedMachine();
  m.run(224u * kLine);  // the last visible line
  EXPECT_EQ(m.state().vpos, 224u);
  EXPECT_FALSE(m.state().vblankNmi);
}

TEST(SnesTiming, VblankFlagSetsAtLine225) {
  Snes m = stoppedMachine();
  m.run(225u * kLine);
  EXPECT_EQ(m.state().vpos, 225u);
  EXPECT_TRUE(m.state().vblankNmi);
}

TEST(SnesTiming, VblankFlagClearsAtTheEndOfTheFrame) {
  Snes m = stoppedMachine();
  m.run(262u * kLine);  // one whole NTSC frame back to line 0
  EXPECT_EQ(m.state().vpos, 0u);
  EXPECT_FALSE(m.state().vblankNmi);
}

TEST(SnesTiming, ReadingRdnmiReturnsAndAcknowledgesTheVblankFlag) {
  // Spin until vblank, then read $4210 twice: the flag reads set, and the first read
  // acknowledges it, so the second sees it clear.
  Snes m = vectoredMachine({
      0xAD, 0x12, 0x42,  // $8000 LDA $4212  (HVBJOY: bit7 = vblank)
      0x10, 0xFB,        //       BPL -5      (loop until vblank)
      0xAD, 0x10, 0x42,  //       LDA $4210   (first read: bit7 set)
      0x85, 0x20,        //       STA $20
      0xAD, 0x10, 0x42,  //       LDA $4210   (second read: cleared by the first)
      0x85, 0x21,        //       STA $21
      0xDB,              //       STP
  });
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  EXPECT_NE(m.state().wram[0x20] & 0x80u, 0u);     // the flag was set
  EXPECT_EQ(m.state().wram[0x21] & 0x80u, 0u);     // and reading it cleared it
  EXPECT_EQ(m.state().wram[0x20] & 0x0Fu, 0x02u);  // CPU version 2 in the low nibble
}

// ---- the frame-parity short line ------------------------------------------

TEST(SnesTiming, OddFramesShortenLine240ToKeepColourSync) {
  // An even frame is 262*1364 master cycles; an odd frame drops four (line 240 runs
  // 1360 instead of 1364). Two frames therefore total 262*1364 + (262*1364 - 4) =
  // 714732, which is a whole multiple of the six-cycle idle step, so the machine
  // lands exactly back at the frame origin only if the short line was applied.
  Snes m = stoppedMachine();
  m.run(714732u);
  EXPECT_EQ(m.state().vpos, 0u);
  EXPECT_EQ(m.state().hpos, 0u);
  EXPECT_EQ(m.state().field, 0u);  // two frames -> parity back to even
}

TEST(SnesTiming, FrameParityTogglesEachFrame) {
  Snes m = stoppedMachine();
  EXPECT_EQ(m.state().field, 0u);
  m.run(262u * kLine);  // one even frame
  EXPECT_EQ(m.state().field, 1u);
}

TEST(SnesTiming, PalRunsThreeHundredTwelveLines) {
  Snes m = stoppedMachine(Region::Pal);
  m.run(311u * kLine);
  EXPECT_EQ(m.state().vpos, 311u);
  m.run(kLine);         // the line that wraps the PAL frame
  EXPECT_EQ(m.state().vpos, 0u);
}

// ---- the vblank NMI -------------------------------------------------------

TEST(SnesTiming, EnablingVblankNmiTakesTheHandlerOncePerFrame) {
  // Main loop enables NMI then spins; the handler at $8010 bumps a counter and
  // returns. One vblank is crossed, so the handler runs exactly once.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x0000] = 0xA9; rom[0x0001] = 0x80;                     // LDA #$80
  rom[0x0002] = 0x8D; rom[0x0003] = 0x00; rom[0x0004] = 0x42; // STA $4200 (enable vblank NMI)
  rom[0x0005] = 0x80; rom[0x0006] = 0xFE;                     // BRA -2 (spin)
  rom[0x0010] = 0xE6; rom[0x0011] = 0x30;                     // $8010 INC $30
  rom[0x0012] = 0x40;                                         //        RTI
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;                     // reset -> $8000
  rom[0x7FFA] = 0x10; rom[0x7FFB] = 0x80;                     // NMI  -> $8010
  Snes m(SnesConfig{.rom = rom});

  m.run(340000u);  // well into frame 0's vblank, short of frame 1
  EXPECT_EQ(m.state().wram[0x30], 1u);  // exactly one vblank NMI taken
}

// ---- the H/V-timer IRQ ----------------------------------------------------

TEST(SnesTiming, VerticalIrqFiresAtTheProgrammedLine) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  // $8000: set V-IRQ (mode 2) at line 100, clear the I flag, spin.
  rom[0x0000] = 0xA9; rom[0x0001] = 0x20;                     // LDA #$20 (H/V IRQ mode 2: V=V, H=0)
  rom[0x0002] = 0x8D; rom[0x0003] = 0x00; rom[0x0004] = 0x42; // STA $4200
  rom[0x0005] = 0xA9; rom[0x0006] = 0x64;                     // LDA #100
  rom[0x0007] = 0x8D; rom[0x0008] = 0x09; rom[0x0009] = 0x42; // STA $4209 (VTIME lo)
  rom[0x000A] = 0xA9; rom[0x000B] = 0x00;                     // LDA #0
  rom[0x000C] = 0x8D; rom[0x000D] = 0x0A; rom[0x000E] = 0x42; // STA $420A (VTIME hi)
  rom[0x000F] = 0x58;                                         // CLI
  rom[0x0010] = 0x80; rom[0x0011] = 0xFE;                     // BRA -2 (spin)
  rom[0x0020] = 0xE6; rom[0x0021] = 0x30;                     // $8020 INC $30
  rom[0x0022] = 0xAD; rom[0x0023] = 0x11; rom[0x0024] = 0x42; //        LDA $4211 (ack TIMEUP)
  rom[0x0025] = 0x40;                                         //        RTI
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;                     // reset
  rom[0x7FFE] = 0x20; rom[0x7FFF] = 0x80;                     // IRQ -> $8020
  Snes m(SnesConfig{.rom = rom});

  m.run(160000u);  // past line 100 (~117), short of the next frame
  EXPECT_EQ(m.state().wram[0x30], 1u);  // one IRQ at line 100
}

TEST(SnesTiming, HorizontalIrqLatchesTheTimeupFlagOncePerLine) {
  // Arm the H IRQ (mode 1) at a mid-line dot with the I flag still set, so the flag
  // latches without dispatching; then let the machine run and read the latch.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x0000] = 0xA9; rom[0x0001] = 0xC8;                     // LDA #200
  rom[0x0002] = 0x8D; rom[0x0003] = 0x07; rom[0x0004] = 0x42; // STA $4207 (HTIME lo = 200)
  rom[0x0005] = 0xA9; rom[0x0006] = 0x00;                     // LDA #0
  rom[0x0007] = 0x8D; rom[0x0008] = 0x08; rom[0x0009] = 0x42; // STA $4208 (HTIME hi = 0)
  rom[0x000A] = 0xA9; rom[0x000B] = 0x10;                     // LDA #$10 (H/V IRQ mode 1: H=H, any V)
  rom[0x000C] = 0x8D; rom[0x000D] = 0x00; rom[0x000E] = 0x42; // STA $4200
  rom[0x000F] = 0xDB;                                         // STP
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  Snes m(SnesConfig{.rom = rom});
  while (m.state().cpu.run == CpuRunState::Running) m.step();  // settle the setup
  m.run(kLine);  // cross dot 200 on the next line
  EXPECT_TRUE(m.state().timeup);
}

TEST(SnesTiming, ArmingNoIrqModeLeavesTheFlagClear) {
  // With the IRQ disabled (mode 0), no compare ever raises the flag.
  Snes m = stoppedMachine();
  m.run(200u * kLine);  // many lines, many would-be H matches
  EXPECT_FALSE(m.state().timeup);
}

// ---- restore does not mint a spurious NMI (F1.f refresh, amendment 2) ------

TEST(SnesTiming, RestoreInVblankDoesNotMintASecondNmi) {
  // The concrete defect the no-edge sync guards against: NMI taken, the $4210 flag
  // still asserted (unread) and NMI still enabled, snapshot mid-vblank, restore.
  // A restore that re-drove the line as a fresh edge would take a second NMI.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x0000] = 0xA9; rom[0x0001] = 0x80;                     // LDA #$80
  rom[0x0002] = 0x8D; rom[0x0003] = 0x00; rom[0x0004] = 0x42; // STA $4200 (enable vblank NMI)
  rom[0x0005] = 0x80; rom[0x0006] = 0xFE;                     // BRA -2 (spin)
  rom[0x0010] = 0xE6; rom[0x0011] = 0x30;                     // $8010 INC $30 (do NOT read $4210)
  rom[0x0012] = 0x40;                                         //        RTI
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  rom[0x7FFA] = 0x10; rom[0x7FFB] = 0x80;                     // NMI -> $8010
  Snes m(SnesConfig{.rom = rom});

  m.run(310000u);  // into vblank, after the NMI has been taken
  ASSERT_EQ(m.state().wram[0x30], 1u);   // the one NMI so far
  ASSERT_TRUE(m.state().vblankNmi);      // flag still asserted (the handler never read $4210)
  ASSERT_FALSE(m.state().cpu.nmiPending);  // and the pending latch is already consumed

  const SnesState snap = m.state();
  // The restored machine lives on the heap: a SnesState is a quarter of a megabyte,
  // and two machines plus a snapshot on the stack would overflow a small thread stack.
  auto restored = std::make_unique<Snes>(SnesConfig{.rom = rom});
  restored->restore(snap);
  restored->run(2000u);  // still within the same vblank; no new frame edge

  EXPECT_EQ(restored->state().wram[0x30], 1u);  // no spurious second NMI from the restore
}

}  // namespace
}  // namespace snaggletooth
