// The three timers: the shared free-running stage-1 divider, the per-timer
// stage-2 comparator, the 4-bit stage-3 output and its read-clear, the
// enable-transition resets, the documented reset deltas, and the cycle-budget
// run surface.
//
// Every assertion is derived from the SNESdev SPC700 register documentation (the
// reverse-derived contract), §$FA-$FF (the 3-stage timer model), §$F1 (CONTROL
// and the enable resets), and the instruction-set notes (the MOV-to-memory dummy
// read). The timers advance through real stepping: RAM defaults to $00 (NOP), so
// a stepped or run machine accumulates cycles on the shared divider.

#include <cstdint>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;
using snaggletooth::RunState;

// A machine seeded at $0200 with a stack pointer, over all-NOP RAM. The caller
// fills in the timer/control/divider fields it is exercising.
ApuState seed() {
  ApuState s{};
  s.cpu.sp = 0xEF;
  s.cpu.pc = 0x0200;
  return s;
}

// The CONTROL byte with timer `n` enabled (bit 7 is the inert IPL-mapping bit,
// carried to match the power-on value).
std::uint8_t enableTimer(std::size_t n) {
  return static_cast<std::uint8_t>(0xB0 | (1u << n));
}

// ── Stage cadence: the base rates and the comparator ────────────────────────
// [SPC] §$FA-$FF: "two (#0 and #1) with a base rate of 128 clock cycles and one
// (#2) with a base rate of 16 clock cycles." Stage 2 increments each stage-1
// tick while enabled; on reaching TnTARGET it ticks stage 3 and zeroes.

TEST(ApuTimerCadence, T2TicksEverySixteenCycles) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;  // divide by 1: every stage-1 tick advances stage 3
  Apu apu(std::move(s));
  apu.run(16);  // one T2 stage-1 tick
  EXPECT_EQ(apu.state().timers[2].stage3, 1);
  EXPECT_EQ(apu.state().timers[2].stage2, 0);
}

TEST(ApuTimerCadence, T2CountsStageTwoUpToItsTarget) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 3;  // three stage-1 ticks per stage-3 tick
  Apu apu(std::move(s));
  apu.run(48);  // three T2 stage-1 ticks
  EXPECT_EQ(apu.state().timers[2].stage3, 1);
  EXPECT_EQ(apu.state().timers[2].stage2, 0);
}

TEST(ApuTimerCadence, T2DoesNotTickBeforeReachingItsTarget) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 3;
  Apu apu(std::move(s));
  apu.run(32);  // two of the three ticks needed
  EXPECT_EQ(apu.state().timers[2].stage3, 0);
  EXPECT_EQ(apu.state().timers[2].stage2, 2);
}

TEST(ApuTimerCadence, T0TicksEveryOneHundredTwentyEightCycles) {
  ApuState s = seed();
  s.control = enableTimer(0);
  s.timers[0].target = 1;
  Apu apu(std::move(s));
  apu.run(128);  // one T0 stage-1 tick
  EXPECT_EQ(apu.state().timers[0].stage3, 1);
}

TEST(ApuTimerCadence, T0AndT1TickOnTheSameBoundary) {
  // [SPC] §$FA-$FF: "Stage 1 ticks for T0 and T1 occur at the same time."
  ApuState s = seed();
  s.control = static_cast<std::uint8_t>(0xB0 | 0x03);  // T0 and T1 enabled
  s.timers[0].target = 1;
  s.timers[1].target = 1;
  Apu apu(std::move(s));
  apu.run(128);
  EXPECT_EQ(apu.state().timers[0].stage3, 1);
  EXPECT_EQ(apu.state().timers[1].stage3, 1);
}

TEST(ApuTimerCadence, TargetZeroDividesByTwoHundredFiftySix) {
  // [SPC] §$FA-$FF: "a target value of $00 corresponds to 256 ticks." It falls
  // out of the 0-255 wraparound, not a special case.
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 0;
  Apu apu(std::move(s));
  apu.run(256 * 16);  // 256 T2 stage-1 ticks
  EXPECT_EQ(apu.state().timers[2].stage3, 1);
}

TEST(ApuTimerCadence, StageThreeIsFourBitAndWraps) {
  // [SPC] §$FA-$FF: "the output value is limited to 4 bits." Seventeen ticks wrap
  // to one.
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  Apu apu(std::move(s));
  apu.run(16 * 17);
  EXPECT_EQ(apu.state().timers[2].stage3, 1);  // 17 & 0x0F
}

TEST(ApuTimerCadence, DisabledTimerDoesNotAdvanceItsStages) {
  // [SPC] §$FA-$FF: "Stage 2 increments each 'tick' of Stage 1 when the timer is
  // enabled."
  ApuState s = seed();
  s.control = 0xB0;  // no enables
  s.timers[2].target = 1;
  Apu apu(std::move(s));
  apu.run(64);
  EXPECT_EQ(apu.state().timers[2].stage2, 0);
  EXPECT_EQ(apu.state().timers[2].stage3, 0);
}

// ── The shared stage-1 divider ──────────────────────────────────────────────
// [SPC] §$FA-$FF: "Stage 1 runs constantly, and cannot be stopped or reset."

TEST(ApuTimerDivider, RunsEvenWhenEveryTimerIsDisabled) {
  ApuState s = seed();
  s.control = 0xB0;  // all timers disabled
  Apu apu(std::move(s));
  apu.run(40);
  EXPECT_EQ(apu.state().divider, 40);
}

TEST(ApuTimerDivider, WrapsWhilePreservingTheTickPhase) {
  // The counter wraps at a multiple of both base periods, so a boundary landing
  // exactly on the wrap still produces its tick.
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  s.divider = 65520;  // 16 short of the 65536 wrap, which is a T2 boundary
  Apu apu(std::move(s));
  apu.run(16);
  EXPECT_EQ(apu.state().divider, 0);          // 65536 & 0xFFFF
  EXPECT_EQ(apu.state().timers[2].stage3, 1);  // the boundary still ticked
}

// ── Enable transitions ($F1) ────────────────────────────────────────────────
// [SPC] §$F1: "When transitioning from 0 to 1, the timer Stage 2 and 3 counters
// are both reset to 0 ... the Stage 1 'counter' is not reset."

TEST(ApuTimerEnable, ZeroToOneResetsStagesTwoAndThree) {
  ApuState s = seed();
  s.control = 0xB0;          // T2 disabled
  s.timers[2].stage2 = 5;    // stale counts a real enable clears
  s.timers[2].stage3 = 7;
  s.ram[0x0200] = 0x8F; s.ram[0x0201] = enableTimer(2); s.ram[0x0202] = 0xF1;  // MOV $F1,#$B4
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().timers[2].stage2, 0);
  EXPECT_EQ(apu.state().timers[2].stage3, 0);
}

TEST(ApuTimerEnable, DoesNotResetTheStageOneDivider) {
  ApuState s = seed();
  s.control = 0xB0;
  s.divider = 100;
  s.ram[0x0200] = 0x8F; s.ram[0x0201] = 0x04; s.ram[0x0202] = 0xF1;  // MOV $F1,#$04 (enable T2), 5 cycles
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().divider, 105);  // 100 + the instruction's 5 cycles, not reset
}

TEST(ApuTimerEnable, WritingAnAlreadyEnabledBitLeavesStagesAlone) {
  // A 1->1 write is not a transition, so the stage counters are untouched.
  ApuState s = seed();
  s.control = enableTimer(2);  // T2 already enabled
  s.timers[2].stage2 = 4;
  s.timers[2].stage3 = 3;
  s.ram[0x0200] = 0x8F; s.ram[0x0201] = enableTimer(2); s.ram[0x0202] = 0xF1;  // MOV $F1,#$B4 again
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().timers[2].stage2, 4);
  EXPECT_EQ(apu.state().timers[2].stage3, 3);
}

// ── Timer output ($FD-$FF) ──────────────────────────────────────────────────
// [SPC] §$FA-$FF: "Stage 3 may be read from TnOUT, and the value is zeroed on
// read ... Writes to TnOUT registers have no effect."

TEST(ApuTimerOutput, ReadingTnOutReturnsStageThreeThenClearsIt) {
  Apu apu;  // power on: TnOUT reads $F, T2 disabled so no tick refills it
  apu.writeRam(0x0200, 0xE4); apu.writeRam(0x0201, 0xFF);  // MOV A,$FF
  apu.writeRam(0x0202, 0xE4); apu.writeRam(0x0203, 0xFF);  // MOV A,$FF
  apu.setPc(0x0200);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x0F);          // the power-on stage-3 value
  EXPECT_EQ(apu.state().timers[2].stage3, 0);  // the read cleared it
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x00);          // now empty
}

TEST(ApuTimerOutput, WritingTnOutDoesNotChangeWhatIsRead) {
  // A MOV dd,ds writes its destination without reading it, so the output keeps
  // its value; a later read returns the stage-3 counter, not the written byte.
  Apu apu;  // power on: T2 stage 3 = $F
  apu.writeRam(0x0200, 0xFA); apu.writeRam(0x0201, 0x00); apu.writeRam(0x0202, 0xFF);  // MOV $FF,$00
  apu.writeRam(0x0203, 0xE4); apu.writeRam(0x0204, 0xFF);  // MOV A,$FF
  apu.setPc(0x0200);
  apu.step();  // the write lands in RAM, not the timer
  apu.step();  // the read returns stage 3
  EXPECT_EQ(apu.state().cpu.a, 0x0F);
}

// ── The MOV-to-memory dummy read (the phantom-read proof) ────────────────────
// [SPC] instruction-set notes: "MOV $ff, #$00 will read from $ff ... and
// therefore will reset T2OUT. OTOH, MOV $ff, $00 won't." $FF is T2OUT.

TEST(ApuTimerPhantomRead, MovImmediateToMemoryReadsAndClearsT2Out) {
  Apu apu;  // power on: T2 stage 3 = $F, T2 disabled
  apu.writeRam(0x0200, 0x8F); apu.writeRam(0x0201, 0x00); apu.writeRam(0x0202, 0xFF);  // MOV $FF,#$00
  apu.setPc(0x0200);
  apu.step();
  EXPECT_EQ(apu.state().timers[2].stage3, 0);  // the dummy read of $FF cleared it
}

TEST(ApuTimerPhantomRead, MovMemoryToMemoryDoesNotClearT2Out) {
  Apu apu;  // power on: T2 stage 3 = $F
  apu.writeRam(0x0200, 0xFA); apu.writeRam(0x0201, 0x00); apu.writeRam(0x0202, 0xFF);  // MOV $FF,$00
  apu.setPc(0x0200);
  apu.step();
  EXPECT_EQ(apu.state().timers[2].stage3, 0x0F);  // no read of $FF, so it is untouched
}

TEST(ApuTimerPhantomRead, ReadModifyWriteStrikesTheOverlay) {
  // A read/modify/write opcode reads its destination too. TSET1 $00FF reads
  // T2OUT before writing it back, so it clears the output.
  Apu apu;  // power on: T2 stage 3 = $F
  apu.writeRam(0x0200, 0x0E); apu.writeRam(0x0201, 0xFF); apu.writeRam(0x0202, 0x00);  // TSET1 $00FF
  apu.setPc(0x0200);
  apu.step();
  EXPECT_EQ(apu.state().timers[2].stage3, 0);
}

// ── Instruction-granular ordering ───────────────────────────────────────────
// The timers advance after the instruction, so a mid-instruction read sees the
// pre-step timer state; the tick lands afterward.

TEST(ApuTimerOrdering, MidInstructionReadSeesPreStepStateThenTheTickApplies) {
  ApuState s = seed();
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  s.timers[2].stage3 = 5;
  s.divider = 14;  // the next T2 boundary (16) is two cycles into this instruction
  s.ram[0x0200] = 0xE4; s.ram[0x0201] = 0xFF;  // MOV A,$FF (3 cycles), reads T2OUT
  Apu apu(std::move(s));
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 5);             // the read observed the pre-step stage 3
  EXPECT_EQ(apu.state().timers[2].stage3, 1);  // the tick applied after, over the cleared counter
}

// ── Reset deltas ────────────────────────────────────────────────────────────
// [SPC] §$FA-$FF: "On reset, [TnOUT] are $0 ... [TnTARGET] retain their old
// values." The stage-1 divider cannot be reset; RAM above zero page is retained.

TEST(ApuReset, ClearsTimerOutputs) {
  Apu apu;  // power on: every TnOUT is $F
  apu.reset();
  for (int i = 0; i < 3; ++i) EXPECT_EQ(apu.state().timers[i].stage3, 0x00);
}

TEST(ApuReset, OutputsDifferFromPowerOn) {
  Apu apu;
  EXPECT_EQ(apu.state().timers[0].stage3, 0x0F);  // power-on value
  apu.reset();
  EXPECT_EQ(apu.state().timers[0].stage3, 0x00);  // reset value
}

TEST(ApuReset, RetainsTimerTargets) {
  ApuState s = seed();
  s.timers[0].target = 0x11;
  s.timers[1].target = 0x22;
  s.timers[2].target = 0x33;
  Apu apu(std::move(s));
  apu.reset();
  EXPECT_EQ(apu.state().timers[0].target, 0x11);
  EXPECT_EQ(apu.state().timers[1].target, 0x22);
  EXPECT_EQ(apu.state().timers[2].target, 0x33);
}

TEST(ApuReset, RetainsTheStageOneDivider) {
  ApuState s = seed();
  s.divider = 1234;
  Apu apu(std::move(s));
  apu.reset();
  EXPECT_EQ(apu.state().divider, 1234);
}

TEST(ApuReset, ClearsZeroPageButRetainsHigherRam) {
  Apu apu;
  apu.writeRam(0x0050, 0xAA);
  apu.writeRam(0x4000, 0xBB);
  apu.reset();
  EXPECT_EQ(apu.readRam(0x0050), 0x00);  // zero page cleared
  EXPECT_EQ(apu.readRam(0x4000), 0xBB);  // the rest of RAM retained
}

TEST(ApuReset, ReSeedsControlTestPortsAndCpu) {
  ApuState s = seed();
  s.cpu.sp = 0x12; s.cpu.pc = 0x9999; s.cpu.a = 0x77; s.cpu.x = 0x66; s.cpu.y = 0x55;
  s.cpu.psw = 0x44;
  s.control = 0x0F;
  s.test = 0x00;
  s.outputPorts = {0x01, 0x02, 0x03, 0x04};
  s.inputPorts = {0x05, 0x06, 0x07, 0x08};
  Apu apu(std::move(s));
  apu.reset();
  const ApuState& r = apu.state();
  EXPECT_EQ(r.control, 0xB0);
  EXPECT_EQ(r.test, 0x0A);
  EXPECT_EQ(r.outputPorts[0], 0xAA);
  EXPECT_EQ(r.outputPorts[1], 0xBB);
  EXPECT_EQ(r.outputPorts[2], 0x00);
  EXPECT_EQ(r.outputPorts[3], 0x00);
  EXPECT_EQ(r.inputPorts[0], 0x00);
  EXPECT_EQ(r.inputPorts[3], 0x00);
  EXPECT_EQ(r.cpu.sp, 0xEF);
  EXPECT_EQ(r.cpu.pc, 0x0000);
  EXPECT_EQ(r.cpu.a, 0x00);
  EXPECT_EQ(r.cpu.x, 0x00);
  EXPECT_EQ(r.cpu.y, 0x00);
  EXPECT_EQ(r.cpu.psw, 0x00);
  EXPECT_EQ(r.cpu.run, RunState::Running);
}

// ── The cycle-budget run surface ────────────────────────────────────────────

TEST(ApuRun, ReturnsTheCyclesActuallyRun) {
  Apu apu;  // all-NOP RAM: each instruction is 2 cycles
  apu.setPc(0x0200);
  EXPECT_EQ(apu.run(10), 10u);  // exactly five NOPs
  EXPECT_EQ(apu.state().cpu.pc, 0x0205);
}

TEST(ApuRun, OvershootsOnTheFinalInstruction) {
  ApuState s = seed();
  for (std::uint16_t i = 0; i < 10; ++i) {
    s.ram[0x0200 + 2 * i] = 0xBA;  // MOVW YA,dp (5 cycles)
    s.ram[0x0201 + 2 * i] = 0x00;
  }
  Apu apu(std::move(s));
  EXPECT_EQ(apu.run(12), 15u);  // two whole instructions reach 10; the third overshoots to 15
}

TEST(ApuRun, RunZeroDoesNothing) {
  Apu apu;
  apu.setPc(0x0200);
  EXPECT_EQ(apu.run(0), 0u);
  EXPECT_EQ(apu.state().cpu.pc, 0x0200);
}

TEST(ApuRun, HaltedCoreStillTicksTimers) {
  // A Stopped core delivers 2 cycles per step and issues no bus access, but the
  // timers keep advancing on the delivered cycles.
  ApuState s = seed();
  s.cpu.run = RunState::Stopped;
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  Apu apu(std::move(s));
  EXPECT_EQ(apu.run(64), 64u);
  EXPECT_EQ(apu.state().timers[2].stage3, 4);   // 64 / 16 stage-1 ticks
  EXPECT_EQ(apu.state().cpu.pc, 0x0200);         // the CPU never advanced
}

TEST(ApuRun, HaltedCoreStepReturnsTwoAndTicks) {
  ApuState s = seed();
  s.cpu.run = RunState::Sleeping;
  s.control = enableTimer(2);
  s.timers[2].target = 1;
  Apu apu(std::move(s));
  EXPECT_EQ(apu.step(), 2u);
  EXPECT_EQ(apu.state().divider, 2);            // the divider advanced
  EXPECT_EQ(apu.state().timers[2].stage3, 0);   // but 2 cycles is short of a T2 tick
}

}  // namespace
