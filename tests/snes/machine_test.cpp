// The SNES machine: power-on state, one-instruction stepping, exact master-cycle
// budgeting, the CPU-to-APU clock interleave at both console rates, snapshot and
// restore, a halted core, and the reset line.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
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

TEST(SnesMachine, TakeFramesIntoABufferMatchesTheVectorDrain) {
  Snes m = loopMachine(Region::Ntsc);
  m.run(118125);  // 176 frames queued
  Snes ref = loopMachine(Region::Ntsc);
  ref.run(118125);
  const std::vector<StereoFrame> all = ref.takeFrames();
  ASSERT_EQ(all.size(), 176u);

  std::vector<StereoFrame> collected;
  std::array<StereoFrame, 32> buf{};
  for (;;) {
    const std::size_t n = m.takeFrames(buf);
    for (std::size_t i = 0; i < n; ++i) collected.push_back(buf[i]);
    if (n < buf.size()) break;  // a short fill drains the last of the queue
  }
  EXPECT_EQ(collected, all);  // the same frames, in the same order
}

TEST(SnesMachine, TakeFramesIntoASmallBufferLeavesTheRestQueued) {
  Snes m = loopMachine(Region::Ntsc);
  m.run(118125);  // 176 frames queued
  std::array<StereoFrame, 100> buf{};
  EXPECT_EQ(m.takeFrames(buf), 100u);     // the buffer filled
  EXPECT_EQ(m.takeFrames().size(), 76u);  // and the rest stayed queued
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

TEST(SnesMachine, RestoreReloadsTheAudioMachinesLiveCore) {
  // The sound CPU resumes from the snapshot's register set, not from wherever
  // the live core had run to: after the restore, the same cycles reach the same
  // program counter and instruction progress as the first time.
  Snes m = loopMachine();
  m.run(500);
  const SnesState snap = m.state();
  m.run(1500);
  const Spc700State after = m.state().apu.cpu;

  m.restore(snap);
  m.run(1500);
  EXPECT_EQ(m.state().apu.cpu.pc, after.pc);
  EXPECT_EQ(m.state().apu.cpu.tcu, after.tcu);
  EXPECT_EQ(m.state().apu.cpu.a, after.a);
}

TEST(SnesMachine, TheSnapshotIsTheStorageTheAudioMachineRunsIn) {
  // state() answers one object for the machine's life, and the audio machine's
  // progress is in it after a run.
  Snes m = loopMachine();
  const SnesState* const object = &m.state();
  m.run(2000);
  EXPECT_EQ(&m.state(), object);
  EXPECT_NE(m.state().apu.divider, 0u);
}

TEST(SnesMachine, AMovedMachineRunsOnIdentically) {
  // The audio machine follows the state to the new place: a moved machine and an
  // unmoved twin run the same 1500 cycles to the same key, the APU's counter
  // included.
  Snes a = loopMachine();
  Snes b = loopMachine();
  a.run(500);
  b.run(500);
  Snes moved(std::move(a));
  moved.run(1500);
  b.run(1500);
  EXPECT_EQ(key(moved), key(b));
  EXPECT_TRUE(moved.state().wram == b.state().wram);
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

// ---- the reset line --------------------------------------------------------
// Each case puts the machine somewhere a reset has to bring it back from — through
// a program where the live core matters, through a restored state where a register
// has to hold a value no short program leaves it at — then pulls the line.

// A machine's state is a quarter of a megabyte, so a case that holds two keeps
// them off its stack.
std::unique_ptr<SnesState> copyOf(const Snes& m) { return std::make_unique<SnesState>(m.state()); }

// Counts a byte of work RAM up once and stops: INC $0010; STP.
Snes countingMachine() {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xEEu;  // INC $0010
  rom[1] = 0x10u;
  rom[2] = 0x00u;
  rom[3] = 0xDBu;  // STP
  rom[0x7FFCu] = 0x00u;  // reset vector -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

void runUntilHalted(Snes& m, int cap = 64) {
  for (int i = 0; i < cap && m.state().cpu.run == CpuRunState::Running; ++i) m.step();
}

TEST(SnesMachine, ResetStartsAStoppedCpuAgainAtTheVectorAndWorkRamStands) {
  // Only a reset restarts a stopped core (the datasheet's section 2.25: "STP and
  // WAI instructions are cleared"), and the byte the first run counted is still
  // there for the second to count again.
  Snes m = countingMachine();
  runUntilHalted(m);
  ASSERT_EQ(m.state().cpu.run, CpuRunState::Stopped);
  ASSERT_EQ(m.state().wram[0x10], 1u);

  m.reset();
  EXPECT_EQ(m.state().cpu.run, CpuRunState::Running);
  EXPECT_EQ(m.state().cpu.pc, 0x8000u);
  runUntilHalted(m);
  EXPECT_EQ(m.state().wram[0x10], 2u);
}

TEST(SnesMachine, ResetLeavesTheCpuTheRegistersItsResetGives) {
  // The datasheet's reset table: D, DBR and PBR zero, SH = $01, XH = YH = 0, E = 1,
  // M = X = 1, the decimal flag 0, I = 1; A, XL, YL, SL and N V Z C not initialised.
  // The sequence's three stack cycles read, and leave SL three lower inside page 1.
  Snes m = loopMachine();
  std::unique_ptr<SnesState> st = copyOf(m);
  st->cpu.e = false;
  st->cpu.p = static_cast<std::uint8_t>(kCpuFlagN | kCpuFlagV | kCpuFlagD | kCpuFlagZ | kCpuFlagC);
  st->cpu.a = 0x9ABCu;
  st->cpu.x = 0x1234u;
  st->cpu.y = 0x5678u;
  st->cpu.d = 0x4321u;
  st->cpu.s = 0x1D01u;  // three lower wraps inside the page: $01 -> $FE
  st->cpu.dbr = 0x7Eu;
  st->cpu.pbr = 0x05u;
  st->cpu.pc = 0x9000u;
  m.restore(*st);

  m.reset();
  const Cpu65816State& c = m.state().cpu;
  EXPECT_EQ(c.pc, 0x8000u);
  EXPECT_EQ(c.pbr, 0x00u);
  EXPECT_EQ(c.dbr, 0x00u);
  EXPECT_EQ(c.d, 0x0000u);
  EXPECT_EQ(c.s, 0x01FEu);
  EXPECT_EQ(c.a, 0x9ABCu);
  EXPECT_EQ(c.x, 0x0034u);
  EXPECT_EQ(c.y, 0x0078u);
  EXPECT_TRUE(c.e);
  EXPECT_EQ(c.p, static_cast<std::uint8_t>(kCpuFlagN | kCpuFlagV | kCpuFlagM | kCpuFlagX |
                                           kCpuFlagI | kCpuFlagZ | kCpuFlagC));
  EXPECT_EQ(c.tcu, 0u);
}

TEST(SnesMachine, ResetDropsAnInterruptThatWasPending) {
  // The flags clear (the register document, $4210 and $4211: "cleared on power on or
  // reset") and $4200 goes to $00, so nothing holds either line; the first thing the
  // CPU does is the instruction at the vector. The image's NMI and IRQ vectors are
  // zero, so an interrupt taken here would leave the program counter in page zero.
  Snes m = loopMachine();
  std::unique_ptr<SnesState> st = copyOf(m);
  st->nmitimen = 0xB0u;
  st->vblankNmi = true;
  st->timeup = true;
  st->cpu.nmiPending = true;
  st->cpu.irqLine = true;
  st->cpu.p = static_cast<std::uint8_t>(st->cpu.p & ~kCpuFlagI);
  m.restore(*st);

  m.reset();
  EXPECT_FALSE(m.state().vblankNmi);
  EXPECT_FALSE(m.state().timeup);
  m.step();
  EXPECT_EQ(m.state().cpu.pc, 0x8001u);  // the NOP at the vector, and nothing else
  m.step();
  EXPECT_EQ(m.state().cpu.pc, 0x8000u);  // then the branch back
}

TEST(SnesMachine, ResetInitialisesTheRegistersTheDocumentsNameAndKeepsTheRest) {
  // fullsnes's I/O map gives each port its value on reset and brackets the ones a
  // reset leaves; the register document says the same of $4200, $4202-$420A, $420B,
  // $420C, the two flags and every $43xx register.
  Snes m = loopMachine();
  std::unique_ptr<SnesState> st = copyOf(m);
  st->nmitimen = 0xB1u;
  st->wrio = 0x00u;
  st->memsel = 1u;
  st->mdmaen = 0x04u;
  st->hdmaen = 0x02u;
  st->joyStrobe = true;
  st->joy = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  st->wmadd = 0x1ABCDu;
  st->wrmpya = 0x12u;
  st->wrmpyb = 0x34u;
  st->wrdiv = 0x5678u;
  st->wrdivb = 0x9Au;
  st->rddiv = 0x1111u;
  st->rdmpy = 0x2222u;
  st->htime = 0x0040u;
  st->vtime = 0x0050u;
  st->dma[3] = DmaChannel{.dmap = 0x41u, .bbad = 0x18u, .a1t = 0x1234u, .a1b = 0x7Eu,
                          .das = 0x0400u, .dasb = 0x7Fu, .a2a = 0x2345u, .nltr = 0x85u,
                          .unused = 0x5Au};
  m.restore(*st);

  m.reset();
  const SnesState& after = m.state();
  EXPECT_EQ(after.nmitimen, 0x00u);
  EXPECT_EQ(after.wrio, 0xFFu);
  EXPECT_EQ(after.memsel, 0x00u);
  EXPECT_EQ(after.mdmaen, 0x00u);
  EXPECT_EQ(after.hdmaen, 0x00u);
  EXPECT_FALSE(after.joyStrobe);
  EXPECT_TRUE(std::all_of(after.joy.begin(), after.joy.end(), [](std::uint8_t b) { return b == 0u; }));
  EXPECT_EQ(after.wmadd, 0u);

  EXPECT_EQ(after.wrmpya, 0x12u);
  EXPECT_EQ(after.wrmpyb, 0x34u);
  EXPECT_EQ(after.wrdiv, 0x5678u);
  EXPECT_EQ(after.wrdivb, 0x9Au);
  EXPECT_EQ(after.rddiv, 0x1111u);
  EXPECT_EQ(after.rdmpy, 0x2222u);
  EXPECT_EQ(after.htime, 0x0040u);
  EXPECT_EQ(after.vtime, 0x0050u);
  EXPECT_EQ(after.dma[3].dmap, 0x41u);
  EXPECT_EQ(after.dma[3].bbad, 0x18u);
  EXPECT_EQ(after.dma[3].a1t, 0x1234u);
  EXPECT_EQ(after.dma[3].a1b, 0x7Eu);
  EXPECT_EQ(after.dma[3].das, 0x0400u);
  EXPECT_EQ(after.dma[3].dasb, 0x7Fu);
  EXPECT_EQ(after.dma[3].a2a, 0x2345u);
  EXPECT_EQ(after.dma[3].nltr, 0x85u);
  EXPECT_EQ(after.dma[3].unused, 0x5Au);
}

TEST(SnesMachine, ResetAbandonsATransferAndAnArithmeticJobInProgress) {
  // $420B goes to $00, which leaves the engine nothing to move, and the result
  // registers keep what they held, which a job landing afterwards would not allow.
  Snes m = loopMachine();
  std::unique_ptr<SnesState> st = copyOf(m);
  st->mdmaen = 0x01u;
  st->dmaRunning = true;
  st->dmaOpened = true;
  st->dmaChannelOpened = true;
  st->dma[0].das = 0x0100u;
  st->mathOp = MathOp::Multiply;
  st->mathClocks = 5u;
  st->mathLeft = 7u;
  st->mathRight = 9u;
  st->rdmpy = 0x2222u;
  m.restore(*st);

  m.reset();
  m.run(2000u);
  EXPECT_FALSE(m.state().dmaRunning);
  EXPECT_EQ(m.state().dma[0].das, 0x0100u);  // not one byte moved
  EXPECT_EQ(m.state().rdmpy, 0x2222u);       // and no product landed
  EXPECT_EQ(m.state().cpu.pbr, 0x00u);
}

TEST(SnesMachine, ResetForcesBlankAndKeepsEverythingElseThePpuHolds) {
  // fullsnes gives INIDISP as 8xh on reset — forced blank, the brightness not
  // initialised — and every other PPU register as unknown or unchanged. The beam
  // begins its first line again, so the sprite pass and the mosaic's row begin with it.
  Snes m = loopMachine();
  std::unique_ptr<SnesState> st = copyOf(m);
  st->ppu.inidisp = 0x0Bu;
  st->ppu.bgmode = 0x09u;
  st->ppu.setini = 0x04u;
  st->ppu.mosaic = 0x31u;
  st->ppu.vram[0x1234] = 0x77u;
  st->ppu.cgram[0x40] = 0x1Fu;
  st->ppu.oam[0x10] = 0x99u;
  st->ppu.sprites.scanned = 77u;
  st->ppu.sprites.found = 5u;
  st->ppu.mosaicBlockLine = 50u;
  st->ppu.mosaicBlockSize = 9u;
  m.restore(*st);

  PpuState& expected = st->ppu;
  expected.inidisp = 0x8Bu;
  expected.sprites.scanned = 0u;
  expected.sprites.found = 0u;
  expected.mosaicBlockLine = 0u;
  expected.mosaicBlockSize = 3u;

  m.reset();
  EXPECT_EQ(m.state().ppu.inidisp, 0x8Bu);
  EXPECT_EQ(m.state().ppu.setini, 0x04u);
  EXPECT_EQ(m.state().ppu.sprites.scanned, 0u);
  EXPECT_EQ(m.state().ppu.mosaicBlockLine, 0u);
  EXPECT_TRUE(m.state().ppu == expected);
}

TEST(SnesMachine, ResetReturnsTheMachineToWhereConstructionStartsItInTime) {
  // fullsnes's timing chart: "H=0, V=0, F=0 — SNES starts at this time after
  // /RESET". A machine reset part-way down a frame stands where a new one stands,
  // and runs from there in step with it — the refresh, the irregular line and the
  // frame's length included, which is what the beam and the counter say together.
  Snes m = loopMachine();
  const std::uint64_t frame = consoleClock(Region::Ntsc).masterCyclesPerFrame;
  m.run(frame * 240u / 262u);
  ASSERT_TRUE(m.state().inVblank);
  ASSERT_EQ(m.state().field, 1u);  // the first frame runs with the parity set
  std::unique_ptr<SnesState> st = copyOf(m);
  st->dmaArm = 1u;
  st->dmaResumePad = true;
  st->hdmaActive = 0xFFu;
  st->hdmaEnded = 0x0Fu;
  st->hdmaDoWrite = 0xF0u;
  st->hdmaInited = true;
  st->hdmaLineFired = true;
  st->hdmaRunPending = true;
  st->hdmaIniting = true;
  st->autoJoyStart = 12345u;
  st->autoJoyClocked = 7u;
  st->counterLatchAt = st->master + 100000u;
  m.restore(*st);
  m.reset();

  Snes fresh = loopMachine();
  const auto sameInTime = [](const SnesState& a, const SnesState& b) {
    EXPECT_EQ(a.master, b.master);
    EXPECT_EQ(a.consumed, b.consumed);
    EXPECT_EQ(a.apuPhase, b.apuPhase);
    EXPECT_EQ(a.hpos, b.hpos);
    EXPECT_EQ(a.vpos, b.vpos);
    EXPECT_EQ(a.field, b.field);
    EXPECT_EQ(a.inVblank, b.inVblank);
    EXPECT_EQ(a.vblankBeginLine, b.vblankBeginLine);
    EXPECT_EQ(a.previousLineMaster, b.previousLineMaster);
    EXPECT_EQ(a.refreshAt, b.refreshAt);
    EXPECT_EQ(a.refreshLeft, b.refreshLeft);
    EXPECT_EQ(a.autoJoyStart, b.autoJoyStart);
    EXPECT_EQ(a.autoJoyClocked, b.autoJoyClocked);
    EXPECT_EQ(a.counterLatchAt, b.counterLatchAt);
    EXPECT_EQ(a.dmaArm, b.dmaArm);
    EXPECT_EQ(a.dmaResumePad, b.dmaResumePad);
    EXPECT_EQ(a.hdmaActive, b.hdmaActive);
    EXPECT_EQ(a.hdmaEnded, b.hdmaEnded);
    EXPECT_EQ(a.hdmaDoWrite, b.hdmaDoWrite);
    EXPECT_EQ(a.hdmaInited, b.hdmaInited);
    EXPECT_EQ(a.hdmaLineFired, b.hdmaLineFired);
    EXPECT_EQ(a.hdmaRunPending, b.hdmaRunPending);
    EXPECT_EQ(a.hdmaIniting, b.hdmaIniting);
    EXPECT_EQ(a.cpu.pc, b.cpu.pc);
  };
  sameInTime(m.state(), fresh.state());
  EXPECT_EQ(m.state().master, 0u);
  EXPECT_EQ(m.state().vpos, 0u);

  const std::uint64_t twoFrames = frame * 2u + 777u;
  m.run(twoFrames);
  fresh.run(twoFrames);
  sameInTime(m.state(), fresh.state());
}

TEST(SnesMachine, ResetKeepsWorkRamTheSaveAndThePads) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xEAu;
  rom[1] = 0x80u;
  rom[2] = 0xFDu;
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom, .saveRamBytes = std::size_t{2048}});
  std::unique_ptr<SnesState> st = copyOf(m);
  st->wram[0x1FFFF] = 0xABu;
  st->sram[0x07FF] = 0xCDu;
  st->mdr = 0x5Au;
  m.restore(*st);
  Joypad pad;
  pad.start = true;
  m.setJoypad(JoypadPort::Two, pad);

  m.reset();
  EXPECT_EQ(m.state().wram[0x1FFFF], 0xABu);
  EXPECT_EQ(m.state().sram[0x07FF], 0xCDu);
  EXPECT_EQ(m.joypad(JoypadPort::Two), std::optional<Joypad>(pad));
  EXPECT_EQ(m.joypad(JoypadPort::One), std::nullopt);
}

TEST(SnesMachine, ResetRunsTheAudioBootProgramAgainFromTheImageAndLeavesAudioRamAlone) {
  // The boot program is mapped over $FFC0-$FFFF and the RAM beneath it is a
  // driver's to scribble on, which a reset does not undo. The audio CPU still boots:
  // it fetches the image, and posts the ready bytes the main CPU waits for.
  Snes m = loopMachine();
  m.run(400000u);
  std::unique_ptr<SnesState> st = copyOf(m);
  for (std::size_t at = 0xFFC0u; at <= 0xFFFFu; ++at) st->apu.ram[at] = 0x00u;
  st->apu.ram[0x0200] = 0x5Au;
  st->apu.outputPorts = {0x11u, 0x22u, 0x33u, 0x44u};
  st->apu.control = 0x00u;  // the boot ROM switched off, as a driver leaves it
  m.restore(*st);

  m.reset();
  EXPECT_EQ(m.state().apu.cpu.pc, 0xFFC0u);
  EXPECT_EQ(m.state().apu.ram[0xFFC0], 0x00u);
  EXPECT_EQ(m.state().apu.ram[0x0200], 0x5Au);
  EXPECT_EQ(m.state().apu.outputPorts[0], 0x00u);
  m.run(400000u);
  EXPECT_EQ(m.state().apu.outputPorts[0], 0xAAu);
  EXPECT_EQ(m.state().apu.outputPorts[1], 0xBBu);
}

TEST(SnesMachine, ResetOnAMachineBuiltWithoutABootProgramLeavesTheAudioCpuReady) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xEAu;
  rom[1] = 0x80u;
  rom[2] = 0xFDu;
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom, .iplStub = false});
  const std::uint16_t readyPc = m.state().apu.cpu.pc;
  const std::uint8_t readyControl = m.state().apu.control;
  m.run(100000u);
  std::unique_ptr<SnesState> st = copyOf(m);
  st->apu.control = static_cast<std::uint8_t>(readyControl ^ 0x07u);  // a program started the timers
  m.restore(*st);
  m.reset();
  EXPECT_EQ(m.state().apu.control, readyControl);
  EXPECT_EQ(m.state().apu.cpu.pc, readyPc);
  EXPECT_EQ(m.state().apu.outputPorts[0], 0xAAu);
  EXPECT_EQ(m.state().apu.outputPorts[1], 0xBBu);
}

TEST(SnesMachine, AMovedMachineResetsAsTheOneItWasBuiltAs) {
  Snes built = loopMachine();
  Snes moved(std::move(built));
  moved.run(400000u);
  moved.reset();
  EXPECT_EQ(moved.state().apu.cpu.pc, 0xFFC0u);  // it runs a boot program, and still knows it
}

// Counts the frames a machine hands over and keeps the last one's shape.
struct FrameCount : FrameObserver {
  int frames = 0;
  unsigned width = 0;
  unsigned height = 0;
  bool black = true;
  void frame(const VideoFrame& picture) override {
    ++frames;
    width = picture.width;
    height = picture.height;
    for (std::size_t at = 0; at < picture.pixels.size(); at += 4u) {
      if (picture.pixels[at] != 0u || picture.pixels[at + 1u] != 0u || picture.pixels[at + 2u] != 0u) {
        black = false;
      }
    }
  }
};

TEST(SnesMachine, ResetNeverDeliversThePictureTheBeamWasPartWayDown) {
  // The next picture handed over is the first whole one drawn after the reset, and
  // under forced blank it is black from top to bottom.
  Snes m = loopMachine();
  FrameCount seen;
  m.setFrameObserver(&seen);
  std::unique_ptr<SnesState> st = copyOf(m);
  st->ppu.inidisp = 0x0Fu;                      // the screen on
  st->ppu.bgmode = 0x05u;                       // in a mode drawn in half-pixels
  st->ppu.cgram[0] = 0xFFu;                     // and a backdrop that is not black
  st->ppu.cgram[1] = 0x7Fu;
  m.restore(*st);

  const std::uint64_t frame = consoleClock(Region::Ntsc).masterCyclesPerFrame;
  m.run(frame / 2u);
  ASSERT_EQ(seen.frames, 0);
  m.reset();
  // The mode is the PPU's to keep, so the program's first act is stood in for here:
  // it sets a mode drawn in whole pixels, and the next picture is that wide.
  std::unique_ptr<SnesState> whole = copyOf(m);
  whole->ppu.bgmode = 0x01u;
  m.restore(*whole);
  m.run(frame - 2000u);
  EXPECT_EQ(seen.frames, 0);  // half a frame and most of one make no picture between them
  m.run(4000u);
  EXPECT_EQ(seen.frames, 1);
  EXPECT_EQ(seen.width, 256u);  // the half-drawn picture's width went with it
  EXPECT_EQ(seen.height, 224u);
  EXPECT_TRUE(seen.black);
  m.setFrameObserver(nullptr);
}

}  // namespace
}  // namespace snaggletooth
