// The console's instruction watch: a host is told, before the instruction at
// a watched address runs, that the CPU has reached it, and can have a return
// stand at that address so the routine there never runs. Each case drives a
// real program and reads back what ran — a byte a store left, a register, the
// stack pointer, the master count — never "the symbol exists".
//
// The boundary cases pin where the watch fires: once per instruction the chip
// begins and not per cycle, once per byte of a block move, not while a
// transfer engine holds the bus, not on a halted core, and not at a boundary a
// hardware interrupt takes — the interrupted instruction is told when the
// handler returns to it. The stand-in cases pin that a return within the bank
// or across banks takes the caller back with the stack where it was and the
// body never run, spending exactly the cycles a real return spends, and that
// nothing but the fetch that begins the instruction sees it: peek, a data read
// and an alias all answer the cartridge. The re-entrancy cases pin what a host
// may do inside the call — read the live register file, write it, disarm the
// place, move the program counter. The last cases pin the machine that watches
// nothing: it runs a fixed program to a fixed budget and lands byte-identical
// to a plain machine, a watched run spends the same cycles as a plain one, and
// the watch survives a move.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kLdxImm = 0xA2u;
constexpr std::uint8_t kLdyImm = 0xA0u;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kIncAbs = 0xEEu;
constexpr std::uint8_t kJsrAbs = 0x20u;
constexpr std::uint8_t kJslLong = 0x22u;
constexpr std::uint8_t kRts = 0x60u;
constexpr std::uint8_t kRti = 0x40u;
constexpr std::uint8_t kMvn = 0x54u;
constexpr std::uint8_t kClc = 0x18u;
constexpr std::uint8_t kXce = 0xFBu;
constexpr std::uint8_t kRep = 0xC2u;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kWai = 0xCBu;
constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kBra = 0x80u;

// One instruction the watcher was told about, with what the machine held at
// the call: the live program counter and stack pointer, a marker byte in work
// RAM, the master count, and whether a transfer was running.
struct Told {
  std::uint32_t address = 0;
  std::uint16_t pc = 0;
  std::uint16_t s = 0;
  std::uint8_t marker = 0;
  std::uint64_t master = 0;
  bool dmaRunning = false;
};

// A watcher that records every instruction it is told, reading the machine it
// is set on at the call, and runs whatever `inside` the test gives it.
struct Watcher final : InstructionWatcher {
  Snes* machine = nullptr;
  std::uint32_t markerAddress = 0x7E0010u;
  std::vector<Told> told;
  void (*inside)(Snes&, std::uint32_t) = nullptr;

  void reached(std::uint32_t address) override {
    Told t;
    t.address = address;
    t.pc = machine->cpuState().pc;
    t.s = machine->cpuState().s;
    t.marker = machine->peek(markerAddress).value_or(0xFFu);
    t.master = machine->state().master;
    t.dmaRunning = machine->state().dmaRunning;
    told.push_back(t);
    if (inside != nullptr) inside(*machine, address);
  }
};

// A machine running `program` from $8000 in a one-bank LoROM image, with
// `routine` placed at $8100 — LDA #$22 ; RTS unless a case supplies its own.
Snes programMachine(std::initializer_list<std::uint8_t> program,
                    std::initializer_list<std::uint8_t> routine = {kLdaImm, 0x22u, kRts}) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  std::size_t at = 0x0100u;
  for (std::uint8_t byte : routine) rom[at++] = byte;
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

// LDA #$11 ; JSR $8100 ; STA !$0020 ; STP — the caller every stand-in case
// runs: $0020 ends holding $11 when the routine never ran and $22 when it did.
const std::initializer_list<std::uint8_t> kCaller = {kLdaImm, 0x11u, kJsrAbs, 0x00u, 0x81u,
                                                     kStaAbs, 0x20u, 0x00u, kStp};
constexpr std::uint32_t kRoutine = 0x008100u;

// ---- nothing armed: the watcher is not told, and the machine is unchanged ----

TEST(SnesInstructionWatch, NothingArmedIsNotTold) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  EXPECT_EQ(m.instructionWatcher(), &w);
  m.run(20000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(m.state().wram[0x20], 0x22u) << "the routine ran, as it always would";
}

// ---- where the watch fires --------------------------------------------------

TEST(SnesInstructionWatch, AnArmedInstructionIsToldOnceBeforeItRuns) {
  Snes m = programMachine({kLdaImm, 0x11u, kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  w.machine = &m;
  w.markerAddress = 0x7E0020u;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008002u, Standin::None);  // the STA
  m.step();  // LDA #$11
  EXPECT_EQ(w.told.size(), 0u);
  m.step();  // STA !$0020: four cycles, one call
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, 0x008002u);
  EXPECT_EQ(int{w.told[0].pc}, 0x8002) << "the live program counter, at the instruction";
  EXPECT_EQ(int{w.told[0].marker}, 0x00) << "told before the store landed";
  EXPECT_EQ(m.state().wram[0x20], 0x11u) << "and the instruction then ran";
  m.step();  // STP
  EXPECT_EQ(w.told.size(), 1u);
}

// A bus observer counting the CPU's opcode fetches of one address: how often
// the chip began the instruction there.
struct FetchCounter final : BusObserver {
  std::uint32_t at = 0;
  std::size_t fetches = 0;
  void access(const BusAccess& a) override {
    if (a.source == AccessSource::Cpu && a.kind == CycleKind::OpcodeFetch && a.address == at) ++fetches;
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};

TEST(SnesInstructionWatch, ToldExactlyAsOftenAsTheChipBeginsTheInstructionUnderHdma) {
  // NOP ; BRA back, with an HDMA channel delivering every visible line: the
  // engine's events take cycles between the two instructions, and the NOP is
  // told exactly once per fetch of its opcode.
  Snes m = programMachine({kNop, kBra, 0xFDu});
  SnesState s = m.state();
  s.hdmaen = 0x01u;
  s.dma[0].dmap = 0x00u;
  s.dma[0].bbad = 0x00u;   // $2100
  s.dma[0].a1t = 0x0100u;  // a one-line direct table at $7E:0100, repeated: count $01, value, ...
  s.dma[0].a1b = 0x7Eu;
  for (std::size_t line = 0; line < 240u; ++line) {
    s.wram[0x100u + line * 2u] = 0x01u;
    s.wram[0x101u + line * 2u] = 0x0Fu;
  }
  m.restore(s);
  FetchCounter fetches;
  fetches.at = 0x008000u;
  m.setObserver(&fetches);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008000u, Standin::None);
  m.run(3u * 262u * 1364u);  // three frames
  EXPECT_GT(w.told.size(), 100u);
  EXPECT_EQ(w.told.size(), fetches.fetches);
}

TEST(SnesInstructionWatch, ABlockMoveIsToldOncePerByteItMoves) {
  // MVN moves three bytes ($7E:0100-0102 to $7E:0200-0202) and begins each as
  // an instruction of its own, fetching its opcode again: told three times.
  Snes m = programMachine({kClc, kXce, kRep, 0x30u,  // native, 16-bit
                           kLdaImm, 0x02u, 0x00u,   // LDA #$0002 (three bytes)
                           kLdxImm, 0x00u, 0x01u,   // LDX #$0100
                           kLdyImm, 0x00u, 0x02u,   // LDY #$0200
                           kMvn, 0x7Eu, 0x7Eu,      // MVN $7E,$7E   (at $800D)
                           kStp});
  SnesState s = m.state();
  s.wram[0x100] = 0xAAu;
  s.wram[0x101] = 0xBBu;
  s.wram[0x102] = 0xCCu;
  m.restore(s);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x00800Du, Standin::None);
  m.run(20000u);
  EXPECT_EQ(w.told.size(), 3u);
  EXPECT_EQ(m.state().wram[0x200], 0xAAu);
  EXPECT_EQ(m.state().wram[0x202], 0xCCu);
}

TEST(SnesInstructionWatch, NotWhileATransferEngineHoldsTheBus) {
  // STA $420B arms a 256-byte transfer, which engages after one more CPU
  // cycle — the first NOP's fetch — and then holds the bus whole. The first NOP
  // is told before the transfer, the second after it, and neither while it
  // runs: no boundary exists inside it.
  Snes m = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kNop, kStp});
  SnesState s = m.state();
  s.dma[0].dmap = 0x00u;    // A->B, increment
  s.dma[0].bbad = 0x80u;    // $2180
  s.dma[0].a1t = 0x9000u;
  s.dma[0].a1b = 0x00u;
  s.dma[0].das = 0x0100u;   // 256 bytes
  s.wmadd = 0x001000u;
  m.restore(s);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008005u, Standin::None);
  m.watchInstruction(0x008006u, Standin::None);
  m.run(40000u);
  ASSERT_EQ(w.told.size(), 2u);
  EXPECT_FALSE(w.told[0].dmaRunning);
  EXPECT_FALSE(w.told[1].dmaRunning);
  EXPECT_GE(w.told[1].master - w.told[0].master, 256u * 8u) << "the whole transfer lay between them";
  EXPECT_EQ(m.state().mdmaen, 0u) << "the transfer ran";
}

TEST(SnesInstructionWatch, AHaltedCoreIsNotTold) {
  // STP halts the core at a boundary with the program counter on the byte
  // after it. Armed, that byte is never told: the core sits, it does not begin.
  Snes m = programMachine({kStp, kNop});
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008000u, Standin::None);
  m.watchInstruction(0x008001u, Standin::None);
  m.run(100000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, 0x008000u) << "the STP itself, before it ran";
}

TEST(SnesInstructionWatch, AnInterruptTakesTheBoundaryAndTheInterruptedInstructionIsToldOnReturn) {
  // NMI enabled, WAI, then a NOP. Vertical blank wakes the core at the NOP's
  // boundary and the interrupt sequence takes that boundary: the NOP is not
  // told there. The handler at $8100 marks $7E:0010 and returns to the NOP,
  // which is told then — once, with the marker already set.
  Snes m = programMachine({kLdaImm, 0x80u, kStaAbs, 0x00u, 0x42u,  // LDA #$80 ; STA $4200
                           kWai, kNop, kStp},                      // WAI ; NOP (at $8006) ; STP
                          {kIncAbs, 0x10u, 0x00u, kRti});          // the handler: INC !$0010 ; RTI
  m.poke(0x00FFFAu, 0x00u);  // the NMI vector -> $8100
  m.poke(0x00FFFBu, 0x81u);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008006u, Standin::None);
  m.run(2u * 262u * 1364u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, 0x008006u);
  EXPECT_EQ(int{w.told[0].marker}, 1) << "the handler had run when the NOP was told";
  EXPECT_EQ(m.state().wram[0x10], 1u);
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped) << "and the NOP then ran into the STP";
}

// ---- standing in: a return stands at the address and the body never runs ----

TEST(SnesInstructionWatch, ANearStandinReturnsToTheCallerAndTheBodyNeverRuns) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, kRoutine);
  EXPECT_EQ(int{w.told[0].s}, 0x01FD) << "the JSR's two bytes on the stack at the call";
  EXPECT_EQ(m.state().wram[0x20], 0x11u) << "the routine's LDA #$22 never ran";
  EXPECT_EQ(int{m.cpuState().s}, 0x01FF) << "the stack is back where it was before the JSR";
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped) << "the caller ran on to its STP";
}

TEST(SnesInstructionWatch, ALongStandinReturnsAcrossBanks) {
  // JSL >$808100 enters the routine through the bank-$80 mirror; a Long
  // stand-in returns with the bank byte, so the caller resumes in bank $00.
  Snes m = programMachine({kLdaImm, 0x11u, kJslLong, 0x00u, 0x81u, 0x80u,
                           kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Long);  // armed through bank $00, entered through $80
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, 0x808100u) << "the address the fetch drove";
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
  EXPECT_EQ(int{m.cpuState().pbr}, 0x00);
  EXPECT_EQ(int{m.cpuState().s}, 0x01FF);
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped);
}

TEST(SnesInstructionWatch, AStandinNoneLetsTheRoutineRun) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::None);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x22u) << "told, and the routine ran";
}

TEST(SnesInstructionWatch, AStandinStandsWithNoWatcherSet) {
  Snes m = programMachine(kCaller);
  m.watchInstruction(kRoutine, Standin::Near);
  m.run(20000u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
}

TEST(SnesInstructionWatch, AStandinSpendsExactlyWhatARealReturnSpends) {
  // Machine A stands a return in for the routine; machine B holds a real RTS
  // at the same byte of its image. The return's step costs the same on both,
  // and the two land on byte-identical states.
  Snes a = programMachine(kCaller);
  Snes b = programMachine(kCaller);
  b.poke(kRoutine, kRts);
  a.watchInstruction(kRoutine, Standin::Near);
  for (int i = 0; i < 2; ++i) {
    a.step();  // LDA, JSR
    b.step();
  }
  EXPECT_EQ(int{a.cpuState().pc}, 0x8100);
  const std::uint32_t ca = a.step();  // the stand-in
  const std::uint32_t cb = b.step();  // the real RTS
  EXPECT_EQ(ca, cb);
  EXPECT_TRUE(a.state() == b.state());
  for (int i = 0; i < 2; ++i) {
    a.step();  // STA, STP
    b.step();
  }
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.state().wram[0x20], 0x11u);
}

TEST(SnesInstructionWatch, TheStandinAnswersOnlyTheFetchThatBeginsTheInstruction) {
  // Armed, the byte is still the cartridge's to peek and to read as data: only
  // the fetch that begins the instruction there answers the return.
  Snes m = programMachine({kLdaAbs, 0x00u, 0x81u, kStp});  // LDA !$8100
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  EXPECT_EQ(int{m.peek(kRoutine).value_or(0)}, int{kLdaImm});
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), int{kLdaImm}) << "a data read sees the cartridge";
  EXPECT_EQ(w.told.size(), 0u) << "never begun as an instruction, never told";
}

TEST(SnesInstructionWatch, AnAliasArmedThroughBank7EIsHeardEnteredFromBank00) {
  // The routine lives in work RAM at $7E:0100 and is entered by JSR $0100 in
  // bank $00; armed through bank $7E, the stand-in answers the fetch at $000100.
  Snes m = programMachine({kLdaImm, 0x11u, kJsrAbs, 0x00u, 0x01u, kStaAbs, 0x20u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x100] = kLdaImm;
  s.wram[0x101] = 0x22u;
  s.wram[0x102] = kRts;
  m.restore(s);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x7E0100u, Standin::Near);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, 0x000100u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
  EXPECT_EQ(m.state().wram[0x100], kLdaImm) << "work RAM is untouched";
}

TEST(SnesInstructionWatch, AnAccessWatchOnTheSameByteIsToldTheStandinAsTheFetchsValue) {
  // The access watch sits after the stand-in: an opcode fetch of the armed
  // byte is told the return's opcode as the byte the machine answers.
  struct Fetches final : AccessWatcher {
    std::vector<std::uint8_t> values;
    AccessAnswer read(std::uint32_t, std::uint8_t value, AccessSource, CycleKind kind,
                      std::uint8_t) override {
      if (kind == CycleKind::OpcodeFetch) values.push_back(value);
      return AccessAnswer::proceed();
    }
    AccessAnswer write(std::uint32_t, std::uint8_t, AccessSource, CycleKind, std::uint8_t) override {
      return AccessAnswer::proceed();
    }
  };
  Snes m = programMachine(kCaller);
  Fetches f;
  m.setAccessWatcher(&f);
  m.watchAccess(kRoutine, 1, true, false);
  m.watchInstruction(kRoutine, Standin::Near);
  m.run(20000u);
  ASSERT_EQ(f.values.size(), 1u);
  EXPECT_EQ(int{f.values[0]}, int{kRts});
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
}

// ---- inside the call ----------------------------------------------------------

TEST(SnesInstructionWatch, AHostThatDisarmsInsideTheCallLetsTheRoutineRun) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  w.inside = [](Snes& machine, std::uint32_t address) { machine.unwatchInstruction(address); };
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x22u) << "disarmed inside the call, the routine ran";
}

TEST(SnesInstructionWatch, ARegisterFileWrittenInsideTheCallIsWhatTheInstructionRunsUnder) {
  Snes m = programMachine({kLdaImm, 0x11u, kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  w.machine = &m;
  w.inside = [](Snes& machine, std::uint32_t) {
    Cpu65816State regs = machine.cpuState();
    regs.a = 0x55u;
    machine.setCpuState(regs);
  };
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008002u, Standin::None);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x55u);
}

TEST(SnesInstructionWatch, TheReturnIsNotAppliedWhenTheHostMovesTheProgramCounter) {
  // The host answers the routine by moving the program counter to another
  // routine, at $8200: the fetch there is not the one the return was queued
  // for, so it runs as it stands, and returns to the caller.
  Snes m = programMachine(kCaller);
  m.poke(0x008200u, kLdaImm);
  m.poke(0x008201u, 0x33u);
  m.poke(0x008202u, kRts);
  Watcher w;
  w.machine = &m;
  w.inside = [](Snes& machine, std::uint32_t) {
    Cpu65816State regs = machine.cpuState();
    regs.pc = 0x8200u;
    machine.setCpuState(regs);
  };
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x33u);
}

// ---- arming shapes ------------------------------------------------------------

TEST(SnesInstructionWatch, ReArmingReplacesTheStandin) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  m.watchInstruction(kRoutine, Standin::None);  // the last arm stands
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x22u);
}

TEST(SnesInstructionWatch, ArmingTwiceThenDisarmingOnceClearsIt) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  m.watchInstruction(kRoutine, Standin::Near);
  m.unwatchInstruction(0x808100u);  // through an alias
  m.run(20000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(m.state().wram[0x20], 0x22u);
}

TEST(SnesInstructionWatch, ADisarmedMachineArmsAgainAndIsToldAgain) {
  Snes m = programMachine(kCaller);
  Watcher w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008000u, Standin::None);
  m.unwatchInstruction(0x008000u);  // the last one: the table is freed
  m.watchInstruction(kRoutine, Standin::Near);  // and built again
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(w.told[0].address, kRoutine);
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
}

TEST(SnesInstructionWatch, DefaultStandinIsNear) {
  Snes m = programMachine(kCaller);
  m.watchInstruction(kRoutine);
  m.run(20000u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
}

// ---- the machine that watches nothing behaves exactly as it does today -------

TEST(SnesInstructionWatch, WatcherSetNothingArmedRunsByteIdenticalToAPlainRun) {
  const std::initializer_list<std::uint8_t> loop = {kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u,
                                                     kNop, kBra, 0xF8u};  // BRA -8: back to $8000
  Snes a = programMachine(loop);
  Snes b = programMachine(loop);
  Watcher w;
  w.machine = &a;
  a.setInstructionWatcher(&w);
  a.watchInstruction(0x008000u, Standin::None);
  a.unwatchInstruction(0x008000u);  // armed and disarmed: the table came and went
  a.run(200000u);
  b.run(200000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(SnesInstructionWatch, AWatchedRunSpendsTheSameCyclesAsAPlainRun) {
  // Every instruction of the loop armed and told: the same program to the
  // same budget lands on the same master count, state and audio.
  const std::initializer_list<std::uint8_t> loop = {kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u,
                                                     kNop, kBra, 0xF8u};  // BRA -8: back to $8000
  Snes a = programMachine(loop);
  Snes b = programMachine(loop);
  Watcher w;
  w.machine = &a;
  a.setInstructionWatcher(&w);
  for (std::uint32_t address : {0x008000u, 0x008002u, 0x008005u, 0x008006u})
    a.watchInstruction(address, Standin::None);
  a.run(200000u);
  b.run(200000u);
  EXPECT_GT(w.told.size(), 1000u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(SnesInstructionWatch, TheWatchSurvivesAMove) {
  Snes m = programMachine(kCaller);
  Watcher w;
  m.setInstructionWatcher(&w);
  m.watchInstruction(kRoutine, Standin::Near);
  Snes moved = std::move(m);
  w.machine = &moved;
  moved.run(20000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(moved.state().wram[0x20], 0x11u);
}

}  // namespace
}  // namespace snaggletooth
