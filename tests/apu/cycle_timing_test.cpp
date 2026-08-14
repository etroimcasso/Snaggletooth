// The machine's cycle: what happens on it, in what order, and where the budget
// stops.
//
// Three things are pinned here. The phase — the timers tick on the first slot of
// their frame and the DSP samples on the frame boundary, so the ticks land on
// counter values one past a multiple of 16 or 128 and the samples on multiples
// of 32. The order — within a cycle the clocked events run before the CPU's bus
// access, so a read sees the tick that shares its cycle and a write does not
// reach the sample that shares its own. And the budget — run() spends exactly
// the cycles it is given, stopping mid-instruction if that is where they run
// out, which is a state the machine can be snapshotted and resumed from.
//
// The phase is derived from the two staged references, which agree: the timer
// ticks are synchronized to the DSP's 32-cycle sample frame at its slots 1 and
// 17. The order is derived from the documented sub-slot layout of the chips'
// shared bus, on which the CPU's access is last.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;

// A machine seeded at $0200 with a stack pointer, over all-NOP RAM. The caller
// fills in the counter, timer and program fields it is exercising.
ApuState seed() {
  ApuState s{};
  s.cpu.sp = 0xEF;
  s.cpu.pc = 0x0200;
  return s;
}

// The CONTROL byte with timer `n` enabled, keeping the power-on bits.
std::uint8_t enableTimer(std::size_t n) {
  return static_cast<std::uint8_t>(0xB0 | (1u << n));
}

// ── The tick phase ───────────────────────────────────────────────────────────
// Timer 2 ticks on slots 1 and 17 of the sample frame, timers 0 and 1 on slot 1
// of every fourth frame — counter values ≡ 1 (mod 16) and ≡ 1 (mod 128).

TEST(ApuCyclePhase, TimerTwoTicksOnTheFirstSlotOfItsHalfFrame) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  Apu apu(std::move(s));
  apu.run(1);  // counter 1
  EXPECT_EQ(apu.state().timers[2].stage3, 1);
}

TEST(ApuCyclePhase, TimerTwoDoesNotTickOnTheFrameBoundary) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  s.divider = 15;
  Apu apu(std::move(s));
  apu.run(1);  // counter 16 — a frame boundary, not a tick slot
  EXPECT_EQ(apu.state().timers[2].stage3, 0);
  apu.run(1);  // counter 17 — the tick slot
  EXPECT_EQ(apu.state().timers[2].stage3, 1);
}

TEST(ApuCyclePhase, TimersZeroAndOneTickOnEveryFourthFrame) {
  ApuState s = seed();
  s.control = static_cast<std::uint8_t>(0xB0 | 0x03);
  s.timers[0].target = 1;
  s.timers[1].target = 1;
  Apu apu(std::move(s));
  apu.run(1);  // counter 1
  EXPECT_EQ(apu.state().timers[0].stage3, 1);
  EXPECT_EQ(apu.state().timers[1].stage3, 1);
  apu.run(127);  // counters 2-128: no further tick slot
  EXPECT_EQ(apu.state().timers[0].stage3, 1);
  EXPECT_EQ(apu.state().timers[1].stage3, 1);
  apu.run(1);  // counter 129
  EXPECT_EQ(apu.state().timers[0].stage3, 2);
  EXPECT_EQ(apu.state().timers[1].stage3, 2);
}

TEST(ApuCyclePhase, TheTickPhaseSurvivesTheCounterWrap) {
  // The counter wraps at 65536, a multiple of every period, so the slot after
  // the wrap is the frame boundary and the one after that is the tick.
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  s.divider = 65535;
  Apu apu(std::move(s));
  apu.run(1);  // counter 0
  EXPECT_EQ(apu.state().divider, 0);
  EXPECT_EQ(apu.state().timers[2].stage3, 0);
  apu.run(1);  // counter 1
  EXPECT_EQ(apu.state().divider, 1);
  EXPECT_EQ(apu.state().timers[2].stage3, 1);
}

TEST(ApuCyclePhase, SamplesLandOnEveryThirtySecondCounter) {
  Apu apu;  // RAM is all $00 (NOP), so the CPU runs harmlessly under the clock
  apu.setPc(0x0200);
  apu.run(31);
  EXPECT_EQ(apu.takeFrames().size(), 0u);
  apu.run(1);  // counter 32
  EXPECT_EQ(apu.takeFrames().size(), 1u);
  apu.run(32);
  EXPECT_EQ(apu.takeFrames().size(), 1u);
}

// ── The order within a cycle ─────────────────────────────────────────────────
// The clocked events go first and the CPU's access lands last, which decides
// every race between the two on a shared cycle.

TEST(ApuCycleOrder, ATimerDisabledOnItsTickCycleStillTakesThatTick) {
  // MOV $F1,#$B0 writes CONTROL on its fifth cycle. Seeded at counter 12 that
  // write lands on counter 17 — a tick slot — so the tick fires under the
  // enable state the write is about to clear.
  ApuState s = seed();
  s.control = enableTimer(2);
  s.divider = 12;
  s.ram[0x0200] = 0x8F; s.ram[0x0201] = 0xB0; s.ram[0x0202] = 0xF1;
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().divider, 17);
  EXPECT_EQ(apu.state().control, 0xB0);          // the write landed
  EXPECT_EQ(apu.state().timers[2].stage2, 1);    // and the tick counted before it did
}

TEST(ApuCycleOrder, ADspWriteOnTheFrameBoundaryJoinsTheNextFrame) {
  // The same fifth-cycle write, seeded so it lands on counter 32. The sample
  // runs first, under the old FLG, so the noise rate the write installs does not
  // reach it — the shared generator holds its seeded level.
  ApuState s = seed();
  s.dspAddr = 0x6C;  // FLG
  s.divider = 27;
  s.ram[0x0200] = 0x8F; s.ram[0x0201] = 0xFF; s.ram[0x0202] = 0xF3;  // MOV DSPDATA,#$FF
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().divider, 32);
  EXPECT_EQ(apu.state().dsp[0x6C], 0xFF);              // the write landed
  EXPECT_EQ(apu.state().dsp.noiseLevel, -0x4000);      // the sample it shared a cycle with did not see it
  apu.run(32);                                         // counter 64: the next frame does
  EXPECT_NE(apu.state().dsp.noiseLevel, -0x4000);
}

TEST(ApuCycleOrder, ADspWriteOneCycleEarlierReachesThatFrame) {
  // One counter back, the write lands on 31 and the frame boundary at 32 comes
  // after it — the same frame, at the same counter, now runs under the new FLG.
  ApuState s = seed();
  s.dspAddr = 0x6C;
  s.divider = 26;
  s.ram[0x0200] = 0x8F; s.ram[0x0201] = 0xFF; s.ram[0x0202] = 0xF3;
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().divider, 31);
  EXPECT_EQ(apu.state().dsp.noiseLevel, -0x4000);  // no frame has run yet
  apu.run(1);                                      // counter 32
  EXPECT_NE(apu.state().dsp.noiseLevel, -0x4000);
}

// ── The budget ───────────────────────────────────────────────────────────────
// run() spends exactly what it is given; the stop is a resting place like any
// other, and splitting a run changes nothing.

TEST(ApuCycleBudget, RunSpendsExactlyItsBudget) {
  ApuState s = seed();
  s.ram[0x0200] = 0xBA; s.ram[0x0201] = 0x40;  // MOVW YA,$40 (5 cycles)
  Apu apu(std::move(s));
  EXPECT_EQ(apu.run(3), 3u);
  EXPECT_EQ(apu.state().divider, 3);
  EXPECT_EQ(apu.state().cpu.tcu, 3);  // three cycles into a five-cycle instruction
}

TEST(ApuCycleBudget, SplitRunsMatchOneWholeRun) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  s.ram[0x0200] = 0xBA; s.ram[0x0201] = 0x40;  // MOVW YA,$40
  s.ram[0x0040] = 0x34; s.ram[0x0041] = 0x12;
  const ApuState image = s;

  Apu split(image);
  split.run(3);
  split.run(4);
  split.run(38);

  Apu whole(image);
  whole.run(45);

  const ApuState& a = split.state();
  const ApuState& b = whole.state();
  EXPECT_EQ(a.divider, b.divider);
  EXPECT_EQ(a.cpu.pc, b.cpu.pc);
  EXPECT_EQ(a.cpu.a, b.cpu.a);
  EXPECT_EQ(a.cpu.y, b.cpu.y);
  EXPECT_EQ(a.cpu.tcu, b.cpu.tcu);
  EXPECT_EQ(a.cpu.ir, b.cpu.ir);
  EXPECT_EQ(a.timers[2].stage2, b.timers[2].stage2);
  EXPECT_EQ(a.timers[2].stage3, b.timers[2].stage3);
  EXPECT_EQ(a.dsp.globalCounter, b.dsp.globalCounter);
  EXPECT_EQ(a.dsp.sampleIndex, b.dsp.sampleIndex);
  EXPECT_EQ(split.takeFrames().size(), whole.takeFrames().size());
}

TEST(ApuCycleBudget, ASnapshotTakenMidInstructionResumesOnItsNextCycle) {
  ApuState s = seed();
  s.ram[0x0200] = 0xBA; s.ram[0x0201] = 0x40;  // MOVW YA,$40
  s.ram[0x0040] = 0x34; s.ram[0x0041] = 0x12;
  Apu apu(std::move(s));
  apu.run(3);
  ASSERT_EQ(apu.state().cpu.tcu, 3);

  Apu resumed;
  resumed.restore(apu.state());  // the unfinished instruction rides the snapshot
  resumed.run(2);
  apu.run(2);

  EXPECT_EQ(resumed.state().cpu.a, 0x34);
  EXPECT_EQ(resumed.state().cpu.y, 0x12);
  EXPECT_EQ(resumed.state().cpu.pc, 0x0202);
  EXPECT_EQ(resumed.state().cpu.tcu, 0);
  EXPECT_EQ(resumed.state().divider, 5);
  EXPECT_EQ(resumed.state().cpu.a, apu.state().cpu.a);
  EXPECT_EQ(resumed.state().cpu.y, apu.state().cpu.y);
  EXPECT_EQ(resumed.state().cpu.pc, apu.state().cpu.pc);
}

TEST(ApuCycleBudget, StepFinishesAnInstructionRunStoppedInsideOf) {
  ApuState s = seed();
  s.ram[0x0200] = 0xBA; s.ram[0x0201] = 0x40;  // MOVW YA,$40
  s.ram[0x0040] = 0x34; s.ram[0x0041] = 0x12;
  Apu apu(std::move(s));
  apu.run(3);
  EXPECT_EQ(apu.step(), 2u);  // the two cycles the instruction still owed
  EXPECT_EQ(apu.state().cpu.pc, 0x0202);
  EXPECT_EQ(apu.state().divider, 5);
}

}  // namespace
