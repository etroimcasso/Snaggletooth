// The console's calls into the guest: a host runs a routine the machine holds,
// either in the guest's own context — registers and stack as they stand, the
// whole register file put back afterwards — or in a frame of the host's own,
// whose result the host reads. Each case drives a real program and reads back
// what ran — a byte a store left, a register, the stack pointer, the master
// count — never "the symbol exists".
//
// The return cases pin that the call ends when the routine returns and not
// before: the landing is pushed as JSR or JSL push theirs, a near return comes
// back within the entry's bank and a long one across banks, and both the stack
// pointer and the program counter must be back — a routine that branches
// through the landing, or one that empties and refills its frame, does not end
// the call. The guard cases pin its unit: instructions, the return counted,
// zero running nothing, a runaway abandoned with the file put back. The
// refusal cases pin that a refused call does nothing at all. The time cases
// pin that the routine's cycles are the machine's own — the same a JSR spends,
// an interrupt due taken inside it and not again afterwards, a waiting core
// called and left waiting. The re-entrancy cases pin a call from inside a
// watcher's call, at two depths, and the way a host reads a routine's
// registers: live, inside a watch on its return. The last cases pin the
// machine that calls nothing, and a call after a move.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
constexpr std::uint8_t kIncA = 0x1Au;
constexpr std::uint8_t kJsrAbs = 0x20u;
constexpr std::uint8_t kJmpAbs = 0x4Cu;
constexpr std::uint8_t kRts = 0x60u;
constexpr std::uint8_t kRtl = 0x6Bu;
constexpr std::uint8_t kRti = 0x40u;
constexpr std::uint8_t kPla = 0x68u;
constexpr std::uint8_t kPea = 0xF4u;
constexpr std::uint8_t kSec = 0x38u;
constexpr std::uint8_t kBeq = 0xF0u;
constexpr std::uint8_t kBra = 0x80u;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kWai = 0xCBu;
constexpr std::uint8_t kStp = 0xDBu;

// A machine running `program` from $8000 in a one-bank LoROM image, with
// `routine` placed at $8100 — INC !$0030 ; RTS unless a case supplies its own.
// Further routines go into the image through poke.
Snes programMachine(std::initializer_list<std::uint8_t> program,
                    std::initializer_list<std::uint8_t> routine = {kIncAbs, 0x30u, 0x00u, kRts}) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  std::size_t at = 0x0100u;
  for (std::uint8_t byte : routine) rom[at++] = byte;
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

void place(Snes& m, std::uint32_t address, std::initializer_list<std::uint8_t> bytes) {
  for (std::uint8_t byte : bytes) m.poke(address++, byte);
}

// LDA #$11 ; STA !$0020 ; STP — the guest every call interrupts, parked after
// the LDA with the program counter at $8002: the landing of every call.
const std::initializer_list<std::uint8_t> kGuest = {kLdaImm, 0x11u, kStaAbs, 0x20u, 0x00u, kStp};
constexpr std::uint32_t kRoutine = 0x008100u;
constexpr std::uint16_t kLanding = 0x8002u;

// The guest parked after its LDA: between instructions, a = $11.
Snes parkedGuest(std::initializer_list<std::uint8_t> routine = {kIncAbs, 0x30u, 0x00u, kRts}) {
  Snes m = programMachine(kGuest, routine);
  m.step();
  return m;
}

// ---- calling in the guest's own context ---------------------------------------

TEST(SnesGuestCall, CallInContextRunsTheRoutineAndPutsTheFileBack) {
  Snes m = parkedGuest();
  const Cpu65816State before = m.cpuState();
  const std::uint64_t master = m.state().master;
  EXPECT_EQ(int{before.pc}, int{kLanding});
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_EQ(m.state().wram[0x30], 1u) << "what the routine changed in memory stands";
  EXPECT_TRUE(m.cpuState() == before) << "the whole register file is back";
  EXPECT_GT(m.state().master, master) << "the routine's cycles were spent";
  m.run(20000u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u) << "the guest carried on unaware";
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped);
}

TEST(SnesGuestCall, TheRegisterFileIsPutBackWhole) {
  Snes m = parkedGuest({kLdaImm, 0x55u, kLdxImm, 0x66u, kLdyImm, 0x77u, kSec, kRts});
  const Cpu65816State before = m.cpuState();
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_TRUE(m.cpuState() == before);
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x11);
}

TEST(SnesGuestCall, TheLandingIsPushedAsAJsrPushesIts) {
  // JSR pushes the address of its own last byte, high byte first, and RTS
  // steps past it: the landing $8002 goes on as $8001.
  Snes m = parkedGuest();
  EXPECT_EQ(int{m.cpuState().s}, 0x01FF);
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_EQ(m.state().wram[0x1FF], 0x80u);
  EXPECT_EQ(m.state().wram[0x1FE], 0x01u);
  EXPECT_EQ(int{m.cpuState().s}, 0x01FF);
}

TEST(SnesGuestCall, ALongCallReturnsAcrossBanks) {
  // The routine is entered through the bank-$80 mirror and ends in RTL; the
  // landing goes on as JSL pushes it — the bank, then the address — and the
  // guest comes back in bank $00.
  Snes m = parkedGuest();
  place(m, 0x008200u, {kIncAbs, 0x30u, 0x00u, kRtl});
  const Cpu65816State before = m.cpuState();
  EXPECT_TRUE(m.callInContext(0x808200u, Standin::Long, 100));
  EXPECT_EQ(m.state().wram[0x30], 1u);
  EXPECT_EQ(m.state().wram[0x1FF], 0x00u) << "the guest's bank";
  EXPECT_EQ(m.state().wram[0x1FE], 0x80u);
  EXPECT_EQ(m.state().wram[0x1FD], 0x01u);
  EXPECT_TRUE(m.cpuState() == before);
  EXPECT_EQ(int{m.cpuState().pbr}, 0x00);
}

TEST(SnesGuestCall, TheWrongReturnLeavesTheStackOffAndTheGuardTrips) {
  // A routine ending in RTL called as Near pulls a third byte the call never
  // pushed: the stack pointer is never back at its value, so the call does
  // not end, and the guard abandons it.
  Snes m = parkedGuest();
  place(m, 0x008200u, {kIncAbs, 0x30u, 0x00u, kRtl});
  const Cpu65816State before = m.cpuState();
  EXPECT_FALSE(m.callInContext(0x008200u, Standin::Near, 50));
  EXPECT_EQ(m.state().wram[0x30], 1u) << "the routine ran until the guard tripped";
  EXPECT_TRUE(m.cpuState() == before) << "and the file came back";
}

// ---- the call ends when the routine returns, and not before ---------------------

TEST(SnesGuestCall, ARoutineThatBranchesThroughTheLandingDoesNotEndTheCall) {
  // JMP $8002 lands on the landing with the frame still on the stack: the
  // guest's own code runs from there — STA, then STP — and the call never
  // ends until the guard trips.
  Snes m = parkedGuest({kJmpAbs, 0x02u, 0x80u});
  const Cpu65816State before = m.cpuState();
  EXPECT_FALSE(m.callInContext(kRoutine, Standin::Near, 20));
  EXPECT_EQ(m.state().wram[0x20], 0x11u) << "the STA at the landing ran inside the call";
  EXPECT_TRUE(m.cpuState() == before) << "the file came back, the STP with it undone";
}

TEST(SnesGuestCall, TheStackPointerAloneDoesNotEndTheCall) {
  // The routine empties its frame — the stack pointer is back — with the
  // program counter still inside it, marks memory, pushes the frame again with
  // PEA and returns through it: the call ends at the RTS, not at the PLA.
  Snes m = parkedGuest({kPla, kPla, kIncAbs, 0x30u, 0x00u, kPea, 0x01u, 0x80u, kRts});
  const Cpu65816State before = m.cpuState();
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_EQ(m.state().wram[0x30], 1u) << "the tail after the PLAs ran";
  EXPECT_TRUE(m.cpuState() == before);
}

// ---- a frame of the host's own ---------------------------------------------------

TEST(SnesGuestCall, CallOnStackLeavesTheRoutinesFileForTheHost) {
  // The host writes the presets, calls on its own stack, and reads the result
  // out of the register file the routine left; putting the guest's file back
  // is the host's own act.
  Snes m = parkedGuest();
  place(m, 0x008300u, {kIncA, kStaAbs, 0x21u, 0x00u, kRts});
  const Cpu65816State guest = m.cpuState();
  Cpu65816State presets = guest;
  presets.a = 0x0005u;
  m.setCpuState(presets);
  EXPECT_TRUE(m.callOnStack(0x008300u, 0x01F0u, Standin::Near, 100));
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x06) << "the routine's result, live";
  EXPECT_EQ(int{m.cpuState().s}, 0x01F0) << "the host's stack, back at its top";
  EXPECT_EQ(int{m.cpuState().pc}, int{kLanding});
  EXPECT_EQ(m.state().wram[0x21], 0x06u);
  EXPECT_EQ(m.state().wram[0x1F0], 0x80u) << "the landing, pushed from the host's top";
  EXPECT_EQ(m.state().wram[0x1EF], 0x01u);
  m.setCpuState(guest);
  m.run(20000u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u) << "the guest, restored by the host, carried on";
}

TEST(SnesGuestCall, CallOnStackAbandonedByTheGuardStaysWhereItStopped) {
  Snes m = parkedGuest({kBra, 0xFEu});  // BRA -2: forever
  EXPECT_FALSE(m.callOnStack(kRoutine, 0x01F0u, Standin::Near, 10));
  EXPECT_EQ(int{m.cpuState().pc}, 0x8100) << "at the routine's boundary";
  EXPECT_EQ(int{m.cpuState().s}, 0x01EE) << "the frame still pushed";
}

// ---- the guard ---------------------------------------------------------------------

TEST(SnesGuestCall, TheGuardTripsOnARoutineThatNeverReturnsAndTheFileComesBack) {
  Snes m = parkedGuest({kIncAbs, 0x30u, 0x00u, kBra, 0xFBu});  // INC ; BRA back to the INC
  const Cpu65816State before = m.cpuState();
  const std::uint64_t master = m.state().master;
  EXPECT_FALSE(m.callInContext(kRoutine, Standin::Near, 40));
  EXPECT_EQ(m.state().wram[0x30], 20u) << "twenty INCs and twenty BRAs: forty instructions";
  EXPECT_TRUE(m.cpuState() == before);
  EXPECT_GT(m.state().master, master);
}

TEST(SnesGuestCall, TheGuardCountsInstructionsWithTheReturnAmongThem) {
  // Three NOPs and an RTS: four instructions. A guard of four lets the
  // routine return; a guard of three abandons it at the RTS.
  Snes a = parkedGuest({kNop, kNop, kNop, kRts});
  Snes b = parkedGuest({kNop, kNop, kNop, kRts});
  EXPECT_TRUE(a.callInContext(kRoutine, Standin::Near, 4));
  EXPECT_FALSE(b.callInContext(kRoutine, Standin::Near, 3));
}

TEST(SnesGuestCall, AZeroGuardRunsNothing) {
  Snes m = parkedGuest();
  const Cpu65816State before = m.cpuState();
  const std::uint64_t master = m.state().master;
  EXPECT_FALSE(m.callInContext(kRoutine, Standin::Near, 0));
  EXPECT_EQ(m.state().master, master) << "no cycle ran";
  EXPECT_EQ(m.state().wram[0x30], 0u);
  EXPECT_TRUE(m.cpuState() == before);
}

// ---- a refused call does nothing -----------------------------------------------------

TEST(SnesGuestCall, AnEntryTheMachineDoesNotMapIsRefused) {
  // $00:6000 is open bus on this cartridge: not addressable, so the call
  // returns false having pushed nothing, run nothing and touched no register.
  Snes m = parkedGuest();
  const SnesState before = m.state();
  EXPECT_FALSE(m.addressable(0x006000u, 1));
  EXPECT_FALSE(m.callInContext(0x006000u, Standin::Near, 100));
  EXPECT_FALSE(m.callOnStack(0x006000u, 0x01F0u, Standin::Near, 100));
  EXPECT_TRUE(m.state() == before);
}

TEST(SnesGuestCall, StandinNoneIsRefused) {
  Snes m = parkedGuest();
  const SnesState before = m.state();
  EXPECT_FALSE(m.callInContext(kRoutine, Standin::None, 100));
  EXPECT_FALSE(m.callOnStack(kRoutine, 0x01F0u, Standin::None, 100));
  EXPECT_TRUE(m.state() == before);
}

TEST(SnesGuestCall, AMachineStoppedMidInstructionIsRefused) {
  // One master cycle from reset is the LDA's opcode fetch: the core is inside
  // the instruction, and a call there is refused.
  Snes m = programMachine(kGuest);
  m.run(1u);
  ASSERT_NE(int{m.cpuState().tcu}, 0);
  const SnesState before = m.state();
  EXPECT_FALSE(m.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_FALSE(m.callOnStack(kRoutine, 0x01F0u, Standin::Near, 100));
  EXPECT_TRUE(m.state() == before);
}

TEST(SnesGuestCall, AStackTheFaceDoesNotReachIsRefused) {
  // A stack pointer in the register file: the landing would land on $2100,
  // which poke refuses, so the call refuses before writing anything.
  Snes m = parkedGuest();
  const SnesState before = m.state();
  EXPECT_FALSE(m.callOnStack(kRoutine, 0x2100u, Standin::Near, 100));
  EXPECT_TRUE(m.state() == before);
}

// ---- the routine is the machine running ----------------------------------------------

TEST(SnesGuestCall, ACallSpendsExactlyWhatTheRoutineSpends) {
  // Machine A is called into the routine; machine B reaches it through the
  // guest's own JSR. The routine's own instructions — the INC and the RTS —
  // cost the same master cycles on both.
  Snes a = parkedGuest();
  Snes b = programMachine({kLdaImm, 0x11u, kJsrAbs, 0x00u, 0x81u, kStaAbs, 0x20u, 0x00u, kStp});
  b.step();  // LDA
  b.step();  // JSR
  ASSERT_EQ(int{b.cpuState().pc}, 0x8100);
  const std::uint64_t routineOnB = b.step() + b.step();  // INC, RTS
  ASSERT_EQ(int{b.cpuState().pc}, 0x8005);
  const std::uint64_t before = a.state().master;
  EXPECT_TRUE(a.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_EQ(a.state().master - before, routineOnB);
}

TEST(SnesGuestCall, AnInterruptDueInsideTheRoutineIsTakenThereAndNotAgainAfterwards) {
  // NMI enabled; the routine spins until the handler has marked $7E:0010,
  // which takes the frame's vertical blank. The interrupt is taken inside the
  // call; afterwards the guest does not take it a second time.
  Snes m = programMachine({kLdaImm, 0x80u, kStaAbs, 0x00u, 0x42u,  // LDA #$80 ; STA $4200
                           kLdaImm, 0x11u, kStaAbs, 0x20u, 0x00u, kStp},
                          {kLdaAbs, 0x10u, 0x00u, kBeq, 0xFBu, kRts});  // LDA !$0010 ; BEQ back ; RTS
  place(m, 0x008200u, {kIncAbs, 0x10u, 0x00u, kRti});  // the handler
  m.poke(0x00FFFAu, 0x00u);                             // the NMI vector -> $8200
  m.poke(0x00FFFBu, 0x82u);
  m.step();  // LDA #$80
  m.step();  // STA $4200
  m.step();  // LDA #$11
  const Cpu65816State before = m.cpuState();
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 200000));
  EXPECT_EQ(m.state().wram[0x10], 1u) << "the handler ran inside the call";
  EXPECT_FALSE(m.cpuState().nmiPending) << "the edge the routine's run took is not pending again";
  Cpu65816State expected = before;
  expected.nmiPending = false;
  EXPECT_TRUE(m.cpuState() == expected);
  m.step();  // an interrupt pending here would take this boundary instead of the STA
  m.step();
  EXPECT_EQ(m.state().wram[0x10], 1u) << "not taken a second time";
  EXPECT_EQ(m.state().wram[0x20], 0x11u) << "the guest's STA and STP ran";
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped);
}

TEST(SnesGuestCall, TheRoutinesCyclesComeOutOfTheBudgetTheHostRunsNext) {
  // The master counter is the machine's one clock: a call between two run()
  // calls spends its cycles from the budget the host runs next, so the run
  // after it lands where the budget alone would have.
  Snes m = parkedGuest();
  const std::uint64_t start = m.state().master;
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  const std::uint64_t spent = m.state().master - start;
  ASSERT_GT(spent, 0u);
  ASSERT_LT(spent, 20000u);
  m.run(20000u);
  // run() stops at the first cycle boundary at or past its budget, and a
  // cycle is at most twelve master cycles.
  EXPECT_GE(m.state().master, start + 20000u);
  EXPECT_LT(m.state().master, start + 20000u + 12u);
}

TEST(SnesGuestCall, AWaitingCoreIsCalledAndComesBackWaiting) {
  Snes m = programMachine({kLdaImm, 0x80u, kStaAbs, 0x00u, 0x42u,  // NMI enabled
                           kWai, kLdaImm, 0x11u, kStaAbs, 0x20u, 0x00u, kStp});
  place(m, 0x008200u, {kIncAbs, 0x10u, 0x00u, kRti});
  m.poke(0x00FFFAu, 0x00u);
  m.poke(0x00FFFBu, 0x82u);
  m.step();
  m.step();
  m.step();  // WAI
  ASSERT_EQ(m.cpuState().run, CpuRunState::Waiting);
  const Cpu65816State before = m.cpuState();
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_EQ(m.state().wram[0x30], 1u);
  EXPECT_TRUE(m.cpuState() == before);
  EXPECT_EQ(m.cpuState().run, CpuRunState::Waiting);
  m.run(2u * 262u * 1364u);  // vertical blank wakes it: the handler, then the guest's tail
  EXPECT_EQ(m.state().wram[0x10], 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped);
}

// ---- a call from inside a watcher's call ---------------------------------------------

// A watcher that calls into the guest from inside its own call: at $8100 it
// calls $8200, and at $8200 — reached inside that call — it calls $8300.
struct NestedCaller final : InstructionWatcher {
  Snes* machine = nullptr;
  std::vector<std::uint32_t> told;
  std::vector<bool> returned;
  void reached(std::uint32_t address) override {
    told.push_back(address);
    if (address == 0x008100u) returned.push_back(machine->callInContext(0x008200u, Standin::Near, 100));
    if (address == 0x008200u) returned.push_back(machine->callInContext(0x008300u, Standin::Near, 100));
  }
};

TEST(SnesGuestCall, ACallFromInsideAWatchersCallIsTheSameCallAtAnyDepth) {
  Snes m = programMachine({kLdaImm, 0x11u, kJsrAbs, 0x00u, 0x81u, kStaAbs, 0x20u, 0x00u, kStp});
  place(m, 0x008200u, {kIncAbs, 0x31u, 0x00u, kRts});
  place(m, 0x008300u, {kIncAbs, 0x32u, 0x00u, kRts});
  NestedCaller w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008100u, Standin::None);
  m.watchInstruction(0x008200u, Standin::None);
  m.watchInstruction(0x008300u, Standin::None);
  m.run(20000u);
  ASSERT_EQ(w.told.size(), 3u);
  EXPECT_EQ(w.told[0], 0x008100u);
  EXPECT_EQ(w.told[1], 0x008200u) << "reached inside the first call";
  EXPECT_EQ(w.told[2], 0x008300u) << "reached inside the second";
  ASSERT_EQ(w.returned.size(), 2u);
  EXPECT_TRUE(w.returned[0]);
  EXPECT_TRUE(w.returned[1]);
  EXPECT_EQ(m.state().wram[0x30], 1u) << "the guest's own routine then ran";
  EXPECT_EQ(m.state().wram[0x31], 1u);
  EXPECT_EQ(m.state().wram[0x32], 1u);
  EXPECT_EQ(m.state().wram[0x20], 0x11u);
  EXPECT_EQ(m.cpuState().run, CpuRunState::Stopped);
}

// A watcher that reads the register file live where it is told.
struct RegisterReader final : InstructionWatcher {
  Snes* machine = nullptr;
  std::vector<std::uint16_t> a;
  void reached(std::uint32_t) override { a.push_back(machine->cpuState().a); }
};

TEST(SnesGuestCall, ARoutinesRegistersAreReadLiveInsideAWatchOnItsReturn) {
  // The routine leaves $5A in A and callInContext puts the file back; the host
  // reads the result inside a watch on the routine's RTS, before it goes back.
  Snes m = parkedGuest({kLdaImm, 0x5Au, kRts});
  RegisterReader w;
  w.machine = &m;
  m.setInstructionWatcher(&w);
  m.watchInstruction(0x008102u, Standin::None);  // the RTS
  EXPECT_TRUE(m.callInContext(kRoutine, Standin::Near, 100));
  ASSERT_EQ(w.a.size(), 1u);
  EXPECT_EQ(static_cast<int>(w.a[0] & 0xFFu), 0x5A);
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x11) << "and the guest's own A is back";
}

// ---- the machine that calls nothing behaves exactly as it does today ---------------

TEST(SnesGuestCall, RefusedCallsLeaveAMachineByteIdenticalToAPlainRun) {
  const std::initializer_list<std::uint8_t> loop = {kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u,
                                                     kNop, kBra, 0xF8u};  // BRA -8: back to $8000
  Snes a = programMachine(loop);
  Snes b = programMachine(loop);
  EXPECT_FALSE(b.callInContext(kRoutine, Standin::None, 100));
  EXPECT_FALSE(b.callOnStack(0x006000u, 0x01F0u, Standin::Near, 100));
  b.run(1u);
  EXPECT_FALSE(b.callInContext(kRoutine, Standin::Near, 100));  // mid-instruction
  b.run(200000u - 1u);
  a.run(200000u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(SnesGuestCall, ACallAfterAMove) {
  Snes m = parkedGuest();
  Snes moved = std::move(m);
  const Cpu65816State before = moved.cpuState();
  EXPECT_TRUE(moved.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_EQ(moved.state().wram[0x30], 1u);
  EXPECT_TRUE(moved.cpuState() == before);
}

}  // namespace
}  // namespace snaggletooth
