// The audio machine's calls into the sound program, in its own 16-bit
// vocabulary: a host runs a routine the machine holds, either in the program's
// own context — registers and stack as they stand, the whole register file put
// back afterwards — or in a frame of the host's own, whose result the host
// reads. Each case drives a real SPC700 program and reads back what ran — a
// byte a store left, a register, the stack pointer, the cycle count — never
// "the symbol exists".
//
// The return cases pin that the call ends when the routine returns and not
// before: the landing is pushed as CALL pushes its own, and both the stack
// pointer and the program counter must be back — a routine that jumps to the
// landing, or one that empties and refills its frame, does not end the call.
// The guard cases pin its unit: instructions, the RET counted, zero running
// nothing, a runaway abandoned with the file put back. The refusal cases pin
// that a refused call does nothing at all. The time cases pin that the
// routine's cycles are the machine's own — the same a CALL's routine spends —
// and that a sleeping core is called and left sleeping. The re-entrancy cases
// pin a call from inside a watcher's call, at two depths, and the way a host
// reads a routine's registers: live, inside a watch on its RET. The last cases
// pin the machine that calls nothing, and a call after a move.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuInstructionWatcher;
using snaggletooth::ApuStandin;
using snaggletooth::ApuState;
using snaggletooth::RunState;
using snaggletooth::Spc700State;

constexpr std::uint8_t kNop = 0x00u;      // NOP
constexpr std::uint8_t kMovAImm = 0xE8u;  // MOV A,#imm
constexpr std::uint8_t kMovXImm = 0xCDu;  // MOV X,#imm
constexpr std::uint8_t kMovYImm = 0x8Du;  // MOV Y,#imm
constexpr std::uint8_t kMovAbsA = 0xC5u;  // MOV !abs,A
constexpr std::uint8_t kIncAbs = 0xACu;   // INC !abs
constexpr std::uint8_t kIncA = 0xBCu;     // INC A
constexpr std::uint8_t kCallAbs = 0x3Fu;  // CALL !abs
constexpr std::uint8_t kJmpAbs = 0x5Fu;   // JMP !abs
constexpr std::uint8_t kRet = 0x6Fu;      // RET
constexpr std::uint8_t kPushA = 0x2Du;    // PUSH A
constexpr std::uint8_t kPopA = 0xAEu;     // POP A
constexpr std::uint8_t kSetc = 0x80u;     // SETC
constexpr std::uint8_t kBra = 0x2Fu;      // BRA rel
constexpr std::uint8_t kSleep = 0xEFu;    // SLEEP
constexpr std::uint8_t kStop = 0xFFu;     // STOP

constexpr std::uint16_t kEntry = 0x0300u;
constexpr std::uint16_t kRoutine = 0x0400u;
constexpr std::uint16_t kResult = 0x0250u;
constexpr std::uint16_t kMark = 0x0260u;
constexpr std::uint16_t kLanding = 0x0302u;

// A machine holding `code` at $0300 with the CPU pointed there, and `routine`
// at $0400 — INC !$0260 ; RET unless a case supplies its own.
Apu loaded(std::initializer_list<std::uint8_t> code,
           std::initializer_list<std::uint8_t> routine = {kIncAbs, 0x60u, 0x02u, kRet}) {
  Apu apu;
  std::uint16_t a = kEntry;
  for (std::uint8_t byte : code) apu.writeRam(a++, byte);
  a = kRoutine;
  for (std::uint8_t byte : routine) apu.writeRam(a++, byte);
  apu.setPc(kEntry);
  return apu;
}

void place(Apu& apu, std::uint16_t address, std::initializer_list<std::uint8_t> bytes) {
  for (std::uint8_t byte : bytes) apu.writeRam(address++, byte);
}

// MOV A,#$11 ; MOV !$0250,A ; STOP — the program every call interrupts, parked
// after the MOV with the program counter at $0302: the landing of every call.
const std::initializer_list<std::uint8_t> kProgram = {kMovAImm, 0x11u, kMovAbsA, 0x50u, 0x02u, kStop};

// The program parked after its MOV: between instructions, a = $11, sp = $EF.
Apu parked(std::initializer_list<std::uint8_t> routine = {kIncAbs, 0x60u, 0x02u, kRet}) {
  Apu apu = loaded(kProgram, routine);
  apu.step();
  return apu;
}

// ---- calling in the program's own context ---------------------------------------

TEST(ApuGuestCall, CallInContextRunsTheRoutineAndPutsTheFileBack) {
  Apu apu = parked();
  const Spc700State before = apu.cpuState();
  const std::uint16_t divider = apu.state().divider;
  EXPECT_EQ(int{before.pc}, int{kLanding});
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_EQ(apu.readRam(kMark), 1u) << "what the routine changed in RAM stands";
  EXPECT_TRUE(apu.cpuState() == before) << "the whole register file is back";
  EXPECT_NE(apu.state().divider, divider) << "the routine's cycles were spent";
  apu.run(2000u);
  EXPECT_EQ(apu.readRam(kResult), 0x11u) << "the program carried on unaware";
  EXPECT_EQ(apu.cpuState().run, RunState::Stopped);
}

TEST(ApuGuestCall, TheRegisterFileIsPutBackWhole) {
  Apu apu = parked({kMovAImm, 0x55u, kMovXImm, 0x66u, kMovYImm, 0x77u, kSetc, kRet});
  const Spc700State before = apu.cpuState();
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_TRUE(apu.cpuState() == before);
  EXPECT_EQ(int{apu.cpuState().a}, 0x11);
}

TEST(ApuGuestCall, TheLandingIsPushedAsACallPushesIts) {
  // CALL pushes the address of the next instruction, high byte at the stack
  // pointer's page-one address and low byte one below, and RET pulls it as it
  // stands.
  Apu apu = parked();
  EXPECT_EQ(int{apu.cpuState().sp}, 0xEF);
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_EQ(apu.readRam(0x01EFu), 0x03u);
  EXPECT_EQ(apu.readRam(0x01EEu), 0x02u);
  EXPECT_EQ(int{apu.cpuState().sp}, 0xEF);
}

// ---- the call ends when the routine returns, and not before ---------------------

TEST(ApuGuestCall, ARoutineThatJumpsToTheLandingDoesNotEndTheCall) {
  // JMP !$0302 lands on the landing with the frame still on the stack: the
  // program's own code runs from there — the store, then STOP — and the call
  // never ends until the guard trips.
  Apu apu = parked({kJmpAbs, 0x02u, 0x03u});
  const Spc700State before = apu.cpuState();
  EXPECT_FALSE(apu.callInContext(kRoutine, ApuStandin::Return, 20));
  EXPECT_EQ(apu.readRam(kResult), 0x11u) << "the store at the landing ran inside the call";
  EXPECT_TRUE(apu.cpuState() == before) << "the file came back, the STOP with it undone";
}

TEST(ApuGuestCall, TheStackPointerAloneDoesNotEndTheCall) {
  // The routine pops its frame — the stack pointer is back — with the program
  // counter still inside it, marks RAM, pushes the frame again and returns
  // through it: the call ends at the RET, not at the second POP.
  Apu apu = parked({kPopA, kPopA, kIncAbs, 0x60u, 0x02u, kMovAImm, 0x03u, kPushA, kMovAImm, 0x02u,
                    kPushA, kRet});
  const Spc700State before = apu.cpuState();
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_EQ(apu.readRam(kMark), 1u) << "the tail after the POPs ran";
  EXPECT_TRUE(apu.cpuState() == before);
}

// ---- a frame of the host's own ---------------------------------------------------

TEST(ApuGuestCall, CallOnStackLeavesTheRoutinesFileForTheHost) {
  // The host writes the presets, calls on its own stack, and reads the result
  // out of the register file the routine left; putting the program's file back
  // is the host's own act.
  Apu apu = parked();
  place(apu, 0x0500u, {kIncA, kMovAbsA, 0x61u, 0x02u, kRet});
  const Spc700State program = apu.cpuState();
  Spc700State presets = program;
  presets.a = 0x05u;
  apu.setCpuState(presets);
  EXPECT_TRUE(apu.callOnStack(0x0500u, 0xD0u, ApuStandin::Return, 100));
  EXPECT_EQ(int{apu.cpuState().a}, 0x06) << "the routine's result, live";
  EXPECT_EQ(int{apu.cpuState().sp}, 0xD0) << "the host's stack, back at its top";
  EXPECT_EQ(int{apu.cpuState().pc}, int{kLanding});
  EXPECT_EQ(apu.readRam(0x0261u), 0x06u);
  EXPECT_EQ(apu.readRam(0x01D0u), 0x03u) << "the landing, pushed from the host's top";
  EXPECT_EQ(apu.readRam(0x01CFu), 0x02u);
  apu.setCpuState(program);
  apu.run(2000u);
  EXPECT_EQ(apu.readRam(kResult), 0x11u) << "the program, restored by the host, carried on";
}

TEST(ApuGuestCall, CallOnStackAbandonedByTheGuardStaysWhereItStopped) {
  Apu apu = parked({kBra, 0xFEu});  // BRA -2: forever
  EXPECT_FALSE(apu.callOnStack(kRoutine, 0xD0u, ApuStandin::Return, 10));
  EXPECT_EQ(int{apu.cpuState().pc}, int{kRoutine}) << "at the routine's boundary";
  EXPECT_EQ(int{apu.cpuState().sp}, 0xCE) << "the frame still pushed";
}

// ---- the guard ---------------------------------------------------------------------

TEST(ApuGuestCall, TheGuardTripsOnARoutineThatNeverReturnsAndTheFileComesBack) {
  Apu apu = parked({kIncAbs, 0x60u, 0x02u, kBra, 0xFBu});  // INC ; BRA back to the INC
  const Spc700State before = apu.cpuState();
  EXPECT_FALSE(apu.callInContext(kRoutine, ApuStandin::Return, 40));
  EXPECT_EQ(apu.readRam(kMark), 20u) << "twenty INCs and twenty BRAs: forty instructions";
  EXPECT_TRUE(apu.cpuState() == before);
}

TEST(ApuGuestCall, TheGuardCountsInstructionsWithTheReturnAmongThem) {
  // Three NOPs and a RET: four instructions. A guard of four lets the routine
  // return; a guard of three abandons it at the RET.
  Apu a = parked({kNop, kNop, kNop, kRet});
  Apu b = parked({kNop, kNop, kNop, kRet});
  EXPECT_TRUE(a.callInContext(kRoutine, ApuStandin::Return, 4));
  EXPECT_FALSE(b.callInContext(kRoutine, ApuStandin::Return, 3));
}

TEST(ApuGuestCall, AZeroGuardRunsNothing) {
  Apu apu = parked();
  const Spc700State before = apu.cpuState();
  const std::uint16_t divider = apu.state().divider;
  EXPECT_FALSE(apu.callInContext(kRoutine, ApuStandin::Return, 0));
  EXPECT_EQ(apu.state().divider, divider) << "no cycle ran";
  EXPECT_EQ(apu.readRam(kMark), 0u);
  EXPECT_TRUE(apu.cpuState() == before);
}

// ---- a refused call does nothing -----------------------------------------------------

TEST(ApuGuestCall, StandinNoneIsRefused) {
  Apu apu = parked();
  const ApuState before = apu.state();
  EXPECT_FALSE(apu.callInContext(kRoutine, ApuStandin::None, 100));
  EXPECT_FALSE(apu.callOnStack(kRoutine, 0xD0u, ApuStandin::None, 100));
  EXPECT_TRUE(apu.state() == before);
}

TEST(ApuGuestCall, AMachineStoppedMidInstructionIsRefused) {
  // One cycle from the entry is the MOV's opcode fetch: the core is inside the
  // instruction, and a call there is refused.
  Apu apu = loaded(kProgram);
  apu.run(1u);
  ASSERT_NE(int{apu.cpuState().tcu}, 0);
  const ApuState before = apu.state();
  EXPECT_FALSE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_FALSE(apu.callOnStack(kRoutine, 0xD0u, ApuStandin::Return, 100));
  EXPECT_TRUE(apu.state() == before);
}

// ---- the routine is the machine running ----------------------------------------------

TEST(ApuGuestCall, ACallSpendsExactlyWhatTheRoutineSpends) {
  // Machine A is called into the routine; machine B reaches it through the
  // program's own CALL. The routine's own instructions — the INC and the RET —
  // cost the same cycles on both.
  Apu a = parked();
  Apu b = loaded({kMovAImm, 0x11u, kCallAbs, 0x00u, 0x04u, kMovAbsA, 0x50u, 0x02u, kStop});
  b.step();  // MOV
  b.step();  // CALL
  ASSERT_EQ(int{b.cpuState().pc}, int{kRoutine});
  const std::uint32_t routineOnB = b.step() + b.step();  // INC, RET
  ASSERT_EQ(int{b.cpuState().pc}, 0x0305);
  const std::uint16_t before = a.state().divider;
  EXPECT_TRUE(a.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_EQ(static_cast<std::uint32_t>(static_cast<std::uint16_t>(a.state().divider - before)),
            routineOnB);
}

TEST(ApuGuestCall, ARunAfterACallRunsItsWholeBudget) {
  // run() here runs exactly its budget from wherever the counter stands: a
  // call between two run() calls moves the counter by the routine's cycles,
  // and the run after it moves it by the budget on top.
  Apu apu = parked();
  const std::uint16_t start = apu.state().divider;
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  const std::uint16_t spent = static_cast<std::uint16_t>(apu.state().divider - start);
  ASSERT_NE(int{spent}, 0);
  apu.run(2000u);
  EXPECT_EQ(apu.state().divider, static_cast<std::uint16_t>(start + spent + 2000u));
}

TEST(ApuGuestCall, ASleepingCoreIsCalledAndComesBackSleeping) {
  Apu apu = loaded({kSleep, kNop});
  apu.step();  // SLEEP
  ASSERT_EQ(apu.cpuState().run, RunState::Sleeping);
  const Spc700State before = apu.cpuState();
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_EQ(apu.readRam(kMark), 1u);
  EXPECT_TRUE(apu.cpuState() == before);
  EXPECT_EQ(apu.cpuState().run, RunState::Sleeping);
}

// ---- a call from inside a watcher's call ---------------------------------------------

// A watcher that calls into the program from inside its own call: at $0400 it
// calls $0500, and at $0500 — reached inside that call — it calls $0600.
struct NestedCaller final : ApuInstructionWatcher {
  Apu* machine = nullptr;
  std::vector<std::uint16_t> told;
  std::vector<bool> returned;
  void reached(std::uint16_t address) override {
    told.push_back(address);
    if (address == 0x0400u) returned.push_back(machine->callInContext(0x0500u, ApuStandin::Return, 100));
    if (address == 0x0500u) returned.push_back(machine->callInContext(0x0600u, ApuStandin::Return, 100));
  }
};

TEST(ApuGuestCall, ACallFromInsideAWatchersCallIsTheSameCallAtAnyDepth) {
  Apu apu = loaded({kMovAImm, 0x11u, kCallAbs, 0x00u, 0x04u, kMovAbsA, 0x50u, 0x02u, kStop});
  place(apu, 0x0500u, {kIncAbs, 0x61u, 0x02u, kRet});
  place(apu, 0x0600u, {kIncAbs, 0x62u, 0x02u, kRet});
  NestedCaller w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0400u, ApuStandin::None);
  apu.watchInstruction(0x0500u, ApuStandin::None);
  apu.watchInstruction(0x0600u, ApuStandin::None);
  apu.run(2000u);
  ASSERT_EQ(w.told.size(), 3u);
  EXPECT_EQ(int{w.told[0]}, 0x0400);
  EXPECT_EQ(int{w.told[1]}, 0x0500) << "reached inside the first call";
  EXPECT_EQ(int{w.told[2]}, 0x0600) << "reached inside the second";
  ASSERT_EQ(w.returned.size(), 2u);
  EXPECT_TRUE(w.returned[0]);
  EXPECT_TRUE(w.returned[1]);
  EXPECT_EQ(apu.readRam(kMark), 1u) << "the program's own routine then ran";
  EXPECT_EQ(apu.readRam(0x0261u), 1u);
  EXPECT_EQ(apu.readRam(0x0262u), 1u);
  EXPECT_EQ(apu.readRam(kResult), 0x11u);
  EXPECT_EQ(apu.cpuState().run, RunState::Stopped);
}

// A watcher that reads the register file live where it is told.
struct RegisterReader final : ApuInstructionWatcher {
  Apu* machine = nullptr;
  std::vector<std::uint8_t> a;
  void reached(std::uint16_t) override { a.push_back(machine->cpuState().a); }
};

TEST(ApuGuestCall, ARoutinesRegistersAreReadLiveInsideAWatchOnItsReturn) {
  // The routine leaves $5A in A and callInContext puts the file back; the host
  // reads the result inside a watch on the routine's RET, before it goes back.
  Apu apu = parked({kMovAImm, 0x5Au, kRet});
  RegisterReader w;
  w.machine = &apu;
  apu.setInstructionWatcher(&w);
  apu.watchInstruction(0x0402u, ApuStandin::None);  // the RET
  EXPECT_TRUE(apu.callInContext(kRoutine, ApuStandin::Return, 100));
  ASSERT_EQ(w.a.size(), 1u);
  EXPECT_EQ(int{w.a[0]}, 0x5A);
  EXPECT_EQ(int{apu.cpuState().a}, 0x11) << "and the program's own A is back";
}

// ---- the machine that calls nothing behaves exactly as it does today ---------------

TEST(ApuGuestCall, RefusedCallsLeaveAMachineByteIdenticalToAPlainRun) {
  const std::initializer_list<std::uint8_t> loop = {kMovAImm, 0x77u, kMovAbsA,
                                                     0x50u,    0x02u, kBra, 0xF9u};
  Apu a = loaded(loop);
  Apu b = loaded(loop);
  EXPECT_FALSE(b.callInContext(kRoutine, ApuStandin::None, 100));
  EXPECT_FALSE(b.callOnStack(kRoutine, 0xD0u, ApuStandin::None, 100));
  b.run(1u);
  EXPECT_FALSE(b.callInContext(kRoutine, ApuStandin::Return, 100));  // mid-instruction
  b.run(50000u - 1u);
  a.run(50000u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(ApuGuestCall, ACallAfterAMove) {
  Apu apu = parked();
  Apu moved = std::move(apu);
  const Spc700State before = moved.cpuState();
  EXPECT_TRUE(moved.callInContext(kRoutine, ApuStandin::Return, 100));
  EXPECT_EQ(moved.readRam(kMark), 1u);
  EXPECT_TRUE(moved.cpuState() == before);
}

}  // namespace
