// The DSP register file inside DspState, and the sample clock that drives it.
//
// The register file is owned by DspState and indexed through the overlay. ENDX
// ($7C) is the one register whose write acknowledges all end flags. The sample
// clock is the machine's master counter: it counts every machine cycle, aligns
// to zero at power-on, and is retained across reset, so the sample phase is
// continuous. Assertions are derived from fullsnes's DSP register notes and the
// DSP's 32 kHz sample-clock model.

#include <cstdint>
#include <initializer_list>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;

// Loads `code` at $0200, points the CPU there, and runs `instructions` of it.
Apu run(std::initializer_list<std::uint8_t> code, int instructions) {
  Apu apu;
  std::uint16_t addr = 0x0200;
  for (std::uint8_t byte : code) apu.writeRam(addr++, byte);
  apu.setPc(0x0200);
  for (int i = 0; i < instructions; ++i) apu.step();
  return apu;
}

// ── ENDX write-acknowledges-all ─────────────────────────────────────────────

TEST(DspRegisters, EndxWriteAcknowledgesAllBits) {
  // "Any write to this register will clear ALL bits, no matter what value is
  // written" (fullsnes ENDX, line 3087). Seed some end flags, then write $AA.
  ApuState s{};
  s.dsp[0x7C] = 0xFF;
  s.cpu.pc = 0x0200;
  const std::uint8_t code[] = {0x8F, 0x7C, 0xF2,   // MOV $F2,#$7C  (select ENDX)
                               0x8F, 0xAA, 0xF3};   // MOV $F3,#$AA  (write it)
  for (std::size_t i = 0; i < sizeof(code); ++i) s.ram[0x0200 + i] = code[i];
  Apu apu(s);
  apu.step();  // select
  apu.step();  // write -> acks all
  EXPECT_EQ(apu.state().dsp[0x7C], 0x00);
}

TEST(DspRegisters, OrdinaryRegisterStoresTheWrittenValue) {
  // A non-ENDX register keeps DSPDATA's RAM-like store: $10 holds what was
  // written, so ENDX's acknowledge is specific to $7C.
  Apu apu = run({0x8F, 0x10, 0xF2,   // select $10
                 0x8F, 0xAA, 0xF3},  // write $AA
                2);
  EXPECT_EQ(apu.state().dsp[0x10], 0xAA);
}

// ── The 32 kHz sample clock ─────────────────────────────────────────────────

TEST(DspSampleClock, PowerOnAlignsToZero) {
  Apu apu;
  EXPECT_EQ(apu.state().divider, 0);
}

TEST(DspSampleClock, AdvancesByDeliveredCycles) {
  // The counter counts machine cycles toward the 32-cycle sample period. Three
  // NOPs deliver 2 cycles each.
  Apu apu = run({0x00, 0x00, 0x00}, 3);  // NOP NOP NOP
  EXPECT_EQ(apu.state().divider, 6);
}

TEST(DspSampleClock, KeepsCountingWhileTheCoreIsHalted) {
  // A halted core still delivers 2 cycles per step, so the DSP keeps sampling.
  // SLEEP runs 7 cycles and halts; the next (halted) step delivers 2 more.
  Apu apu;
  apu.writeRam(0x0200, 0xEF);  // SLEEP
  apu.setPc(0x0200);
  apu.step();  // SLEEP: 7 cycles, core now sleeping
  apu.step();  // halted: 2 cycles
  EXPECT_EQ(apu.state().divider, 9);
  EXPECT_EQ(apu.state().cpu.run, snaggletooth::RunState::Sleeping);
}

TEST(DspSampleClock, IsRetainedAcrossReset) {
  // The free-running counter cannot be reset — reset() keeps its running phase,
  // so both the sample boundaries and the timer ticks stay on their slots.
  Apu apu = run({0x00, 0x00, 0x00}, 3);  // counter = 6
  ASSERT_EQ(apu.state().divider, 6);
  apu.reset();
  EXPECT_EQ(apu.state().divider, 6);
}

// ── Snapshot carries the DSP state ──────────────────────────────────────────

TEST(DspSnapshot, RoundTripsRegisterFileAndDivider) {
  Apu apu = run({0x8F, 0x21, 0xF2,   // select DSP $21
                 0x8F, 0x9C, 0xF3,   // dsp[$21] := $9C
                 0x00, 0x00}, 4);    // two NOPs -> the counter advances
  const ApuState snap = apu.state();
  Apu fresh;
  fresh.restore(snap);
  EXPECT_EQ(fresh.state().dsp[0x21], 0x9C);
  EXPECT_EQ(fresh.state().divider, snap.divider);
}

}  // namespace
