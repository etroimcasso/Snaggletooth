// The SNES machine: power-on state, one-instruction stepping, exact master-cycle
// budgeting, the CPU-to-APU clock interleave at both console rates, snapshot and
// restore, and a halted core.

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// A cartridge that loops forever: NOP, then branch back to itself. It exercises
// fetches and a taken branch every iteration, which is enough varied work to test
// budgeting and the interleave.
Snes loopMachine(Region region = Region::Ntsc) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xEAu;  // NOP
  rom[1] = 0x80u;  // BRA
  rom[2] = 0xFDu;  // -3 -> back to $8000
  rom[0x7FFCu] = 0x00u;  // reset vector -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom, .region = region});
}

// The comparable core of the machine state, for equivalence assertions.
struct Key {
  std::uint16_t pc;
  std::uint16_t a;
  std::uint16_t s;
  std::uint16_t x;
  std::uint16_t y;
  std::uint8_t p;
  std::uint8_t ir;
  std::uint8_t tcu;
  std::uint64_t master;
  std::uint16_t divider;
  bool operator==(const Key&) const = default;
};

Key key(const Snes& m) {
  const Cpu65816State& c = m.state().cpu;
  return Key{.pc = c.pc, .a = c.a, .s = c.s, .x = c.x, .y = c.y, .p = c.p,
             .ir = c.ir, .tcu = c.tcu, .master = m.state().master,
             .divider = m.state().apu.divider};
}

// ---- power-on -------------------------------------------------------------

TEST(SnesMachine, PowerOnProgramCounterIsTheResetVector) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x6Au;  // reset vector -> $806A
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  EXPECT_EQ(m.state().cpu.pc, 0x806Au);
}

TEST(SnesMachine, PowerOnIsEmulationModeWithInterruptsDisabled) {
  Snes m = loopMachine();
  EXPECT_TRUE(m.state().cpu.e);
  EXPECT_NE(m.state().cpu.p & kCpuFlagI, 0u);
}

// ---- stepping and budgeting ----------------------------------------------

TEST(SnesMachine, StepRunsOneInstructionAndReturnsItsMasterCost) {
  Snes m = loopMachine();
  const std::uint32_t cost = m.step();  // NOP: an opcode fetch (8) and one internal cycle (6)
  EXPECT_EQ(cost, 14u);
  EXPECT_EQ(m.state().cpu.pc, 0x8001u);
}

TEST(SnesMachine, RunZeroRunsNothing) {
  Snes m = loopMachine();
  m.run(0);
  EXPECT_EQ(m.state().master, 0u);
  EXPECT_EQ(m.state().cpu.pc, 0x8000u);
  EXPECT_EQ(m.state().apu.divider, 0u);
}

TEST(SnesMachine, RunIsAdditiveAcrossCalls) {
  Snes split = loopMachine();
  split.run(1000);
  split.run(2000);
  Snes whole = loopMachine();
  whole.run(3000);
  EXPECT_EQ(key(split), key(whole));
  EXPECT_TRUE(split.state().wram == whole.state().wram);
}

TEST(SnesMachine, RunConsumesExactlyTheBudget) {
  Snes m = loopMachine();
  m.run(1000);
  EXPECT_EQ(m.state().consumed, 1000u);
  EXPECT_GE(m.state().master, 1000u);
  EXPECT_LT(m.state().master, 1000u + 12u);  // the overshoot is at most one access
}

// ---- the CPU-to-APU interleave -------------------------------------------

TEST(SnesMachine, ApuAdvancesByTheNtscRatio) {
  // Over one full NTSC denominator of master cycles the APU takes exactly the
  // numerator of cycles: floor(118125 * 5632 / 118125) = 5632.
  Snes m = loopMachine(Region::Ntsc);
  m.run(118125);
  EXPECT_EQ(m.state().apu.divider, 5632u);
}

TEST(SnesMachine, ApuAdvancesByThePalRatio) {
  // The PAL denominator: floor(2128137 * 102400 / 2128137) = 102400 APU cycles,
  // and the APU's 16-bit counter wraps to 102400 - 65536 = 36864.
  Snes m = loopMachine(Region::Pal);
  m.run(2128137);
  EXPECT_EQ(m.state().apu.divider, 36864u);
}

TEST(SnesMachine, TheApuDeliversFramesAtItsSampleRate) {
  // 5632 APU cycles is 5632 / 32 = 176 sample frames.
  Snes m = loopMachine(Region::Ntsc);
  m.run(118125);
  EXPECT_EQ(m.takeFrames().size(), 176u);
}

TEST(SnesMachine, OneSecondOfMasterCyclesDeliversTheSampleRate) {
  // The APU is a 32 kHz source, so about one NTSC second of master cycles yields
  // 32000 stereo frames: floor(21477273 * 5632 / 118125) = 1024000 APU cycles / 32.
  Snes m = loopMachine(Region::Ntsc);
  m.run(21'477'273);
  EXPECT_EQ(m.takeFrames().size(), 32'000u);
}

TEST(SnesMachine, TakeFramesDrainsTheQueue) {
  Snes m = loopMachine(Region::Ntsc);
  m.run(118125);
  EXPECT_EQ(m.takeFrames().size(), 176u);
  EXPECT_TRUE(m.takeFrames().empty());
}

// ---- snapshot and restore -------------------------------------------------

TEST(SnesMachine, SnapshotAndRestoreRoundTrip) {
  Snes m = loopMachine();
  m.run(500);
  const SnesState snap = m.state();
  m.run(1500);
  const Key after = key(m);
  const auto wramAfter = m.state().wram;

  m.restore(snap);
  m.run(1500);
  EXPECT_EQ(key(m), after);
  EXPECT_TRUE(m.state().wram == wramAfter);
}

TEST(SnesMachine, RestoreResumesFromMidInstruction) {
  Snes m = loopMachine();
  m.run(20);  // stops part-way through the branch that follows the first NOP
  const SnesState snap = m.state();
  ASSERT_NE(snap.cpu.tcu, 0u);  // the snapshot is genuinely mid-instruction

  m.run(300);
  const Key after = key(m);

  m.restore(snap);
  m.run(300);
  EXPECT_EQ(key(m), after);
}

// ---- a halted core --------------------------------------------------------

TEST(SnesMachine, AStoppedCoreIdlesWithoutAdvancing) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xDBu;         // STP
  rom[0x7FFCu] = 0x00u;   // reset vector -> $8000
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});

  const std::uint32_t stpCost = m.step();  // an opcode fetch (8) and two internal cycles (6 each)
  EXPECT_EQ(stpCost, 20u);
  EXPECT_EQ(m.state().cpu.run, CpuRunState::Stopped);

  const std::uint16_t pc = m.state().cpu.pc;
  const std::uint32_t idle = m.step();     // one idle cycle at the fast rate
  EXPECT_EQ(idle, 6u);
  EXPECT_EQ(m.state().cpu.run, CpuRunState::Stopped);
  EXPECT_EQ(m.state().cpu.pc, pc);         // the CPU does not move while stopped
}

TEST(SnesMachine, RunAfterStepAdvancesTheFullBudget) {
  Snes m = loopMachine();
  m.step();  // one instruction; the budget accounting catches up to it
  const std::uint64_t base = m.state().master;
  m.run(100);
  EXPECT_GE(m.state().master, base + 100u);
  EXPECT_LT(m.state().master, base + 100u + 12u);
}

}  // namespace
}  // namespace snaggletooth
