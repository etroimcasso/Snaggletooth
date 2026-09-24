// The audio machine's instruction watch, in its own 16-bit vocabulary: a host
// is told, before the instruction at a watched address runs, that the sound
// CPU has reached it, and can have a return stand at that address so the
// routine there never runs. Each case drives a real SPC700 program and reads
// back what ran — a byte a store left, a register, the stack pointer, the
// cycles a step took — never "the symbol exists".
//
// The boundary cases pin where the watch fires: once per instruction and not
// per cycle, and not on a sleeping or stopped core. The stand-in cases pin
// that a return takes the caller back with the stack where it was and the body
// never run, spending exactly the cycles a real RET spends, and that nothing
// but the fetch that begins the instruction sees it: peek and a data read
// answer RAM. The re-entrancy cases pin what a host may do inside the call —
// read the live register file, write it, disarm the place, move the program
// counter. The last cases pin the machine that watches nothing: it runs a
// fixed program to a fixed budget and lands byte-identical to a plain machine,
// a watched run spends the same cycles as a plain one, and the watch survives
// a move.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::AccessAnswer;
using snaggletooth::Apu;
using snaggletooth::ApuAccessWatcher;
using snaggletooth::ApuInstructionWatcher;
using snaggletooth::ApuStandin;
using snaggletooth::RunState;
using snaggletooth::Spc700State;
using snaggletooth::StereoFrame;

constexpr std::uint8_t kMovAImm = 0xE8u;  // MOV A,#imm
constexpr std::uint8_t kMovAAbs = 0xE5u;  // MOV A,!abs
constexpr std::uint8_t kMovAbsA = 0xC5u;  // MOV !abs,A
constexpr std::uint8_t kCallAbs = 0x3Fu;  // CALL !abs
constexpr std::uint8_t kRet = 0x6Fu;      // RET
constexpr std::uint8_t kBra = 0x2Fu;      // BRA rel
constexpr std::uint8_t kSleep = 0xEFu;    // SLEEP
constexpr std::uint8_t kStop = 0xFFu;     // STOP

constexpr std::uint16_t kEntry = 0x0300u;
constexpr std::uint16_t kRoutine = 0x0400u;
constexpr std::uint16_t kResult = 0x0250u;

// One instruction the watcher was told about, with what the machine held at
// the call: the live program counter and stack pointer, and the result byte.
struct Told {
  std::uint16_t address = 0;
  std::uint16_t pc = 0;
  std::uint8_t sp = 0;
  std::uint8_t result = 0;
};

// A watcher that records every instruction it is told, reading the machine it
// is set on at the call, and runs whatever `inside` the test gives it.
struct Watcher final : ApuInstructionWatcher {
  Apu* machine = nullptr;
  std::vector<Told> told;
  void (*inside)(Apu&, std::uint16_t) = nullptr;

  void reached(std::uint16_t address) override {
    Told t;
    t.address = address;
    t.pc = machine->cpuState().pc;
    t.sp = machine->cpuState().sp;
    t.result = machine->readRam(kResult);
    told.push_back(t);
    if (inside != nullptr) inside(*machine, address);
  }
};

// A machine holding `code` at $0300 with the CPU pointed there, and `routine`
// at $0400 — MOV A,#$22 ; RET unless a case supplies its own.
Apu loaded(std::initializer_list<std::uint8_t> code,
           std::initializer_list<std::uint8_t> routine = {kMovAImm, 0x22u, kRet}) {
  Apu apu;
  std::uint16_t a = kEntry;
  for (std::uint8_t byte : code) apu.writeRam(a++, byte);
  a = kRoutine;
  for (std::uint8_t byte : routine) apu.writeRam(a++, byte);
  apu.setPc(kEntry);
  return apu;
}

// MOV A,#$11 ; CALL !$0400 ; MOV !$0250,A ; STOP — the caller every stand-in
// case runs: $0250 ends holding $11 when the routine never ran and $22 when
// it did.
const std::initializer_list<std::uint8_t> kCaller = {kMovAImm, 0x11u, kCallAbs, 0x00u, 0x04u,
                                                     kMovAbsA, 0x50u,  0x02u,    kStop};

// ---- nothing armed: the watcher is not told, and the machine is unchanged ----

TEST(ApuInstructionWatch, NothingArmedIsNotTold) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  EXPECT_EQ(apu.instructionWatcher(), &w);
  apu.run(2000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(apu.readRam(kResult), 0x22u) << "the routine ran, as it always would";
}

// ---- where the watch fires --------------------------------------------------

TEST(ApuInstructionWatch, AnArmedInstructionIsToldOnceBeforeItRuns) {
  Apu apu = loaded({kMovAImm, 0x11u, kMovAbsA, 0x50u, 0x02u, kStop});
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0302u, ApuStandin::None);  // the store
  apu.step();  // MOV A,#$11
  EXPECT_EQ(w.told.size(), 0u);
  apu.step();  // MOV !$0250,A: five cycles, one call
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(int{w.told[0].address}, 0x0302);
  EXPECT_EQ(int{w.told[0].pc}, 0x0302) << "the live program counter, at the instruction";
  EXPECT_EQ(int{w.told[0].result}, 0x00) << "told before the store landed";
  EXPECT_EQ(apu.readRam(kResult), 0x11u) << "and the instruction then ran";
  apu.step();  // STOP
  EXPECT_EQ(w.told.size(), 1u);
}

TEST(ApuInstructionWatch, ToldOncePerInstructionOverALoop) {
  // MOV A,#$77 ; MOV !$0250,A ; BRA back — every instruction armed and 300
  // instructions stepped: 300 calls.
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u, kBra, 0xF9u});
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0300u, ApuStandin::None);
  apu.watchInstruction(0x0302u, ApuStandin::None);
  apu.watchInstruction(0x0305u, ApuStandin::None);
  for (int i = 0; i < 300; ++i) apu.step();
  EXPECT_EQ(w.told.size(), 300u);
}

TEST(ApuInstructionWatch, AStoppedCoreIsNotTold) {
  // STOP halts the core at a boundary with the program counter on the byte
  // after it. Armed, that byte is never told: the core sits, it does not begin.
  Apu apu = loaded({kStop, 0x00u});
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0300u, ApuStandin::None);
  apu.watchInstruction(0x0301u, ApuStandin::None);
  apu.run(10000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(int{w.told[0].address}, 0x0300) << "the STOP itself, before it ran";
  EXPECT_EQ(apu.cpuState().run, RunState::Stopped);
}

TEST(ApuInstructionWatch, ASleepingCoreIsNotTold) {
  Apu apu = loaded({kSleep, 0x00u});
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0301u, ApuStandin::None);
  apu.run(10000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(apu.cpuState().run, RunState::Sleeping);
}

// ---- standing in: a return stands at the address and the body never runs ----

TEST(ApuInstructionWatch, AReturnStandinReturnsToTheCallerAndTheBodyNeverRuns) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(int{w.told[0].address}, int{kRoutine});
  EXPECT_EQ(int{w.told[0].sp}, 0xED) << "the CALL's two bytes on the stack at the call";
  EXPECT_EQ(apu.readRam(kResult), 0x11u) << "the routine's MOV A,#$22 never ran";
  EXPECT_EQ(int{apu.cpuState().sp}, 0xEF) << "the stack is back where it was before the CALL";
  EXPECT_EQ(apu.cpuState().run, RunState::Stopped) << "the caller ran on to its STOP";
}

TEST(ApuInstructionWatch, AStandinNoneLetsTheRoutineRun) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::None);
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(apu.readRam(kResult), 0x22u) << "told, and the routine ran";
}

TEST(ApuInstructionWatch, AStandinStandsWithNoWatcherSet) {
  Apu apu = loaded(kCaller);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.run(2000u);
  EXPECT_EQ(apu.readRam(kResult), 0x11u);
}

TEST(ApuInstructionWatch, AStandinSpendsExactlyWhatARealReturnSpends) {
  // Machine A stands a return in for the routine; machine B holds a real RET
  // at the same byte of its RAM. The return's step costs the same on both, and
  // the two land on the same registers and the same master count.
  Apu a = loaded(kCaller);
  Apu b = loaded(kCaller);
  b.writeRam(kRoutine, kRet);
  a.watchInstruction(kRoutine, ApuStandin::Return);
  for (int i = 0; i < 2; ++i) {
    a.step();  // MOV, CALL
    b.step();
  }
  EXPECT_EQ(int{a.cpuState().pc}, int{kRoutine});
  const std::uint32_t ca = a.step();  // the stand-in
  const std::uint32_t cb = b.step();  // the real RET
  EXPECT_EQ(ca, cb);
  EXPECT_TRUE(a.cpuState() == b.cpuState());
  EXPECT_EQ(a.state().divider, b.state().divider);
  for (int i = 0; i < 2; ++i) {
    a.step();  // MOV !$0250,A ; STOP
    b.step();
  }
  EXPECT_TRUE(a.cpuState() == b.cpuState());
  EXPECT_EQ(a.readRam(kResult), 0x11u);
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(ApuInstructionWatch, TheStandinAnswersOnlyTheFetchThatBeginsTheInstruction) {
  // Armed, the byte is still RAM's to peek and to read as data: only the fetch
  // that begins the instruction there answers the return.
  Apu apu = loaded({kMovAAbs, 0x00u, 0x04u, kStop});  // MOV A,!$0400
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  EXPECT_EQ(int{apu.peek(kRoutine)}, int{kMovAImm});
  apu.step();
  EXPECT_EQ(int{apu.cpuState().a}, int{kMovAImm}) << "a data read sees RAM";
  EXPECT_EQ(w.told.size(), 0u) << "never begun as an instruction, never told";
  EXPECT_EQ(apu.readRam(kRoutine), kMovAImm) << "RAM is untouched";
}

TEST(ApuInstructionWatch, AnAccessWatchOnTheSameAddressIsToldTheStandinAsTheFetchsValue) {
  // The access watch sits after the stand-in: the fetch of the armed address
  // is told RET as the byte the machine answers.
  struct Fetches final : ApuAccessWatcher {
    std::vector<std::uint8_t> values;
    AccessAnswer read(std::uint16_t, std::uint8_t value, std::uint8_t cycle) override {
      if (cycle == 0u) values.push_back(value);
      return AccessAnswer::proceed();
    }
    AccessAnswer write(std::uint16_t, std::uint8_t, std::uint8_t) override {
      return AccessAnswer::proceed();
    }
  };
  Apu apu = loaded(kCaller);
  Fetches f;
  apu.setAccessWatcher(&f);
  apu.watchAccess(kRoutine, 1, true, false);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.run(2000u);
  ASSERT_EQ(f.values.size(), 1u);
  EXPECT_EQ(int{f.values[0]}, int{kRet});
  EXPECT_EQ(apu.readRam(kResult), 0x11u);
}

// ---- inside the call ----------------------------------------------------------

TEST(ApuInstructionWatch, AHostThatDisarmsInsideTheCallLetsTheRoutineRun) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  w.inside = [](Apu& machine, std::uint16_t address) { machine.unwatchInstruction(address); };
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(apu.readRam(kResult), 0x22u) << "disarmed inside the call, the routine ran";
}

TEST(ApuInstructionWatch, ARegisterFileWrittenInsideTheCallIsWhatTheInstructionRunsUnder) {
  Apu apu = loaded({kMovAImm, 0x11u, kMovAbsA, 0x50u, 0x02u, kStop});
  Watcher w;
  w.machine = &apu;
  w.inside = [](Apu& machine, std::uint16_t) {
    Spc700State regs = machine.cpuState();
    regs.a = 0x55u;
    machine.setCpuState(regs);
  };
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0302u, ApuStandin::None);
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(apu.readRam(kResult), 0x55u);
}

TEST(ApuInstructionWatch, TheReturnIsNotAppliedWhenTheHostMovesTheProgramCounter) {
  // The host answers the routine by moving the program counter to another
  // routine, at $0500: the fetch there is not the one the return was queued
  // for, so it runs as it stands, and returns to the caller.
  Apu apu = loaded(kCaller);
  apu.writeRam(0x0500u, kMovAImm);
  apu.writeRam(0x0501u, 0x33u);
  apu.writeRam(0x0502u, kRet);
  Watcher w;
  w.machine = &apu;
  w.inside = [](Apu& machine, std::uint16_t) {
    Spc700State regs = machine.cpuState();
    regs.pc = 0x0500u;
    machine.setCpuState(regs);
  };
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(apu.readRam(kResult), 0x33u);
}

// ---- arming shapes ------------------------------------------------------------

TEST(ApuInstructionWatch, ReArmingReplacesTheStandin) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.watchInstruction(kRoutine, ApuStandin::None);  // the last arm stands
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(apu.readRam(kResult), 0x22u);
}

TEST(ApuInstructionWatch, ArmingTwiceThenDisarmingOnceClearsIt) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  apu.unwatchInstruction(kRoutine);
  apu.run(2000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(apu.readRam(kResult), 0x22u);
}

TEST(ApuInstructionWatch, ADisarmedMachineArmsAgainAndIsToldAgain) {
  Apu apu = loaded(kCaller);
  Watcher w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kEntry, ApuStandin::None);
  apu.unwatchInstruction(kEntry);  // the last one: the table is freed
  apu.watchInstruction(kRoutine, ApuStandin::Return);  // and built again
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(int{w.told[0].address}, int{kRoutine});
  EXPECT_EQ(apu.readRam(kResult), 0x11u);
}

TEST(ApuInstructionWatch, DefaultStandinIsReturn) {
  Apu apu = loaded(kCaller);
  apu.watchInstruction(kRoutine);
  apu.run(2000u);
  EXPECT_EQ(apu.readRam(kResult), 0x11u);
}

// ---- the machine that watches nothing behaves exactly as it does today -------

TEST(ApuInstructionWatch, WatcherSetNothingArmedRunsByteIdenticalToAPlainRun) {
  const std::initializer_list<std::uint8_t> loop = {kMovAImm, 0x77u, kMovAbsA,
                                                     0x50u,    0x02u, kBra, 0xF9u};
  Apu a = loaded(loop);
  Apu b = loaded(loop);
  Watcher w;
  w.machine = &a;
  a.setInstructionWatcher(&w);
  a.watchInstruction(kEntry, ApuStandin::None);
  a.unwatchInstruction(kEntry);  // armed and disarmed: the table came and went
  a.run(50000u);
  b.run(50000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(ApuInstructionWatch, AWatchedRunSpendsTheSameCyclesAsAPlainRun) {
  // Every instruction of the loop armed and told: the same program to the
  // same budget lands on the same state and audio.
  const std::initializer_list<std::uint8_t> loop = {kMovAImm, 0x77u, kMovAbsA,
                                                     0x50u,    0x02u, kBra, 0xF9u};
  Apu a = loaded(loop);
  Apu b = loaded(loop);
  Watcher w;
  w.machine = &a;
  a.setInstructionWatcher(&w);
  a.watchInstruction(0x0300u, ApuStandin::None);
  a.watchInstruction(0x0302u, ApuStandin::None);
  a.watchInstruction(0x0305u, ApuStandin::None);
  a.run(50000u);
  b.run(50000u);
  EXPECT_GT(w.told.size(), 1000u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(ApuInstructionWatch, TheWatchSurvivesAMove) {
  Apu apu = loaded(kCaller);
  Watcher w;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(kRoutine, ApuStandin::Return);
  Apu moved = std::move(apu);
  w.machine = &moved;
  moved.run(2000u);
  ASSERT_EQ(w.told.size(), 1u);
  EXPECT_EQ(moved.readRam(kResult), 0x11u);
}

}  // namespace
