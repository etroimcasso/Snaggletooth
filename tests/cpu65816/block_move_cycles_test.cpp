#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The two block moves, cycle by cycle. The recorded vectors prove both opcodes' whole
// traces; what is proven here is the documented shape behind them, with each
// expectation traced to the line that states it: W65C816S datasheet Table 5-7 rows 9a
// (MVN) and 9b (MVP), section 3.5.9 on block move addressing, section 7.8.3.2 on the
// emulated range, and section 7.18 on the data bank register. Eyes & Lichty's "Block
// Move Next" and "Block Move Previous" pages give the count's width and the rule for
// resuming an interrupted move.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::InterruptRequest;
using snaggletooth::kCpuFlagI;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::cpu_vectors::RecordingBus;

RecordingBus busWith(
    std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

// Runs one byte of a move — the seven cycles between one instruction boundary and the
// next — and returns how many cycles that took, with the bus recording pin states
// from the processor as it goes.
std::uint32_t runOneByte(RecordingBus& bus, Cpu65816& cpu) {
  bus.cpu = &cpu;
  return cpu.stepInstruction(bus);
}

// Runs a whole move, however many bytes it carries, and returns the total cycles. The
// ceiling is far above anything these tests move: a move that never let go of its own
// address would otherwise run forever, and the caller's cycle count catches it.
constexpr std::uint32_t kRunawayCycles = 1000;

std::uint32_t runWholeMove(RecordingBus& bus, Cpu65816& cpu) {
  bus.cpu = &cpu;
  std::uint32_t cycles = 0;
  // The count lives in the accumulator and reads $FFFF once the move is spent, so the
  // move is over when the program counter has left the instruction behind.
  const std::uint16_t start = cpu.state().pc;
  do {
    cycles += cpu.stepInstruction(bus);
  } while (cpu.state().pc == start && cycles < kRunawayCycles);
  return cycles;
}

// MVN $12,$34 at $001000 — destination bank $12, source bank $34 — over a bus whose
// source bytes are laid out from $340000.
RecordingBus moveBus(std::uint8_t opcode) {
  return busWith({{0x001000, opcode},
                  {0x001001, 0x12},  // the destination bank, the first operand byte
                  {0x001002, 0x34},  // the source bank, the second
                  {0x340040, 0xAA},
                  {0x340041, 0xBB},
                  {0x340042, 0xCC},
                  {0x34003F, 0x99},
                  {0x34003E, 0x88}});
}

constexpr std::uint8_t kMvn = 0x54;
constexpr std::uint8_t kMvp = 0x44;

// ---- where the cycles go (Table 5-7 rows 9a and 9b) ----

TEST(Cpu65816BlockMove, OneByteTakesSevenCyclesInTheDocumentedShape) {
  // Row 9a, cycle for cycle: the opcode, the destination bank, the source bank, the
  // byte read at the source, the byte written at the destination, and two cycles
  // parked on the address just written.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0000, .x = 0x0040, .y = 0x0080});
  EXPECT_EQ(runOneByte(bus, cpu), 7u);

  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[0].address, 0x001000u);  // PBR,PC   — the opcode
  EXPECT_EQ(bus.trace[1].address, 0x001001u);  // PBR,PC+1 — the destination bank
  EXPECT_EQ(bus.trace[2].address, 0x001002u);  // PBR,PC+2 — the source bank
  EXPECT_EQ(bus.trace[3].address, 0x340040u);  // SBA,X    — the byte
  EXPECT_EQ(bus.trace[4].address, 0x120080u);  // DBA,Y    — where it lands
  EXPECT_EQ(bus.trace[5].address, 0x120080u);  // DBA,Y    — and two cycles parked
  EXPECT_EQ(bus.trace[6].address, 0x120080u);  //            on that same address

  EXPECT_EQ(bus.trace[0].signals, "dp-r----");  // an opcode fetch: VDA and VPA
  EXPECT_EQ(bus.trace[1].signals, "-p-r----");  // an operand fetch: VPA alone
  EXPECT_EQ(bus.trace[2].signals, "-p-r----");
  EXPECT_EQ(bus.trace[3].signals, "d--r----");  // a data read: VDA alone
  EXPECT_EQ(bus.trace[4].signals, "d--w----");  // a data write
  EXPECT_EQ(bus.trace[5].signals, "---r----");  // no valid access at all
  EXPECT_EQ(bus.trace[6].signals, "---r----");

  EXPECT_EQ(bus.mem[0x120080], 0xAA);
}

TEST(Cpu65816BlockMove, TheByteReadIsTheByteWritten) {
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0000, .x = 0x0040, .y = 0x0080});
  runOneByte(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 7u);
  ASSERT_TRUE(bus.trace[3].value.has_value());
  ASSERT_TRUE(bus.trace[4].value.has_value());
  EXPECT_EQ(*bus.trace[4].value, *bus.trace[3].value);
}

TEST(Cpu65816BlockMove, TheOperandBytesNameTheDestinationThenTheSource) {
  // Section 3.5.9: the second byte of the instruction carries the destination bank
  // and the third the source bank — the reverse of the order they are written in.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0000, .x = 0x0040, .y = 0x0080});
  runOneByte(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(*bus.trace[3].address >> 16, 0x34u);  // read from the second operand byte
  EXPECT_EQ(*bus.trace[4].address >> 16, 0x12u);  // written to the first
}

TEST(Cpu65816BlockMove, TheInternalCyclesParkOnTheDestinationBeforeTheIndexesStep) {
  // Rows 9a and 9b give cycles 6 and 7 as DBA,Y — the address the byte just went to,
  // not the one the next byte will use. The indexes have not moved yet.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0001, .x = 0x0040, .y = 0x0080});
  runOneByte(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[5].address, 0x120080u);
  EXPECT_EQ(bus.trace[6].address, 0x120080u);
  // Only once the byte is finished do they step.
  EXPECT_EQ(cpu.state().x, 0x0041);
  EXPECT_EQ(cpu.state().y, 0x0081);
}

TEST(Cpu65816BlockMove, TheDataBankRegisterTakesTheDestinationOnItsOwnFetchCycle) {
  // Section 7.18: the instruction loads the data bank register with its destination
  // bank. It happens on the cycle that reads the byte, not at the end of the move.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .a = 0x0000, .x = 0x0040, .y = 0x0080, .dbr = 0xEE});
  bus.cpu = &cpu;
  cpu.stepCycle(bus);  // the opcode
  EXPECT_EQ(cpu.state().dbr, 0xEE);
  cpu.stepCycle(bus);  // the destination bank
  EXPECT_EQ(cpu.state().dbr, 0x12);
}

// ---- the move as a loop ----

TEST(Cpu65816BlockMove, TheCountIsTheAccumulatorPlusOne) {
  // Three bytes moved for a count of two, at seven cycles each.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0002, .x = 0x0040, .y = 0x0080});
  EXPECT_EQ(runWholeMove(bus, cpu), 21u);
  EXPECT_EQ(bus.mem[0x120080], 0xAA);
  EXPECT_EQ(bus.mem[0x120081], 0xBB);
  EXPECT_EQ(bus.mem[0x120082], 0xCC);
  EXPECT_EQ(cpu.state().a, 0xFFFF);
  EXPECT_EQ(cpu.state().x, 0x0043);
  EXPECT_EQ(cpu.state().y, 0x0083);
}

TEST(Cpu65816BlockMove, TheCountIsTheWholeAccumulatorAtAnEightBitWidth) {
  // Eyes & Lichty: "the value in C is always used, regardless of the setting of the m
  // flag". A count of $0002 under an 8-bit accumulator still moves three bytes.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .a = 0x0002, .x = 0x0040, .y = 0x0080, .p = kCpuFlagM});
  EXPECT_EQ(runWholeMove(bus, cpu), 21u);
  EXPECT_EQ(cpu.state().a, 0xFFFF);
}

TEST(Cpu65816BlockMove, TheMoveEndsWhenTheCountRunsPastZero) {
  // A count of zero moves one byte, and the accumulator is left at $FFFF.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0000, .x = 0x0040, .y = 0x0080});
  EXPECT_EQ(runWholeMove(bus, cpu), 7u);
  EXPECT_EQ(cpu.state().a, 0xFFFF);
  EXPECT_EQ(cpu.state().pc, 0x1003);
}

TEST(Cpu65816BlockMove, ThePositiveMoveCountsTheIndexesDown) {
  // Row 9b: MVP steps both indexes down instead of up, so it copies from the top of a
  // block towards the bottom.
  auto bus = moveBus(kMvp);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0001, .x = 0x0040, .y = 0x0080});
  EXPECT_EQ(runWholeMove(bus, cpu), 14u);
  EXPECT_EQ(bus.mem[0x120080], 0xAA);
  EXPECT_EQ(bus.mem[0x12007F], 0x99);
  EXPECT_EQ(cpu.state().x, 0x003E);
  EXPECT_EQ(cpu.state().y, 0x007E);
}

TEST(Cpu65816BlockMove, TheProgramCounterHoldsOnTheInstructionUntilTheCountDrains) {
  // Rows 9a and 9b start every byte at PBR,PC — the block move's own address. The
  // three fetches step the program counter and the end of the byte steps it back, so
  // between bytes it stands where the instruction does.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0001, .x = 0x0040, .y = 0x0080});
  runOneByte(bus, cpu);
  EXPECT_EQ(cpu.state().pc, 0x1000);
  runOneByte(bus, cpu);
  EXPECT_EQ(cpu.state().pc, 0x1003);  // the last byte leaves the instruction behind
}

TEST(Cpu65816BlockMove, EveryByteRefetchesTheOpcodeAndBothBankBytes) {
  // The second byte's first three cycles read the same three addresses as the first
  // byte's, because the move re-enters itself rather than carrying the operands over.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0001, .x = 0x0040, .y = 0x0080});
  EXPECT_EQ(runWholeMove(bus, cpu), 14u);
  ASSERT_EQ(bus.trace.size(), 14u);
  EXPECT_EQ(bus.trace[7].address, 0x001000u);
  EXPECT_EQ(bus.trace[8].address, 0x001001u);
  EXPECT_EQ(bus.trace[9].address, 0x001002u);
  EXPECT_EQ(bus.trace[7].signals, "dp-r----");  // a full opcode fetch, every time
}

TEST(Cpu65816BlockMove, TheMoveSitsOnAnInstructionBoundaryBetweenBytes) {
  // The re-entry is what makes the move interruptible: between bytes the core is
  // exactly where it is between any two instructions.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0005, .x = 0x0040, .y = 0x0080});
  bus.cpu = &cpu;
  for (int i = 0; i < 7; ++i) cpu.stepCycle(bus);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(cpu.state().pc, 0x1000);
  EXPECT_EQ(cpu.state().a, 0x0004);  // and the count carries how far it got
}

// ---- narrow index registers ----

TEST(Cpu65816BlockMove, NarrowIndexesAddressWithinTheirBanksLowPage) {
  // Section 7.8.3.2: with 8-bit index registers the move reaches only $0000-$00FF of
  // each bank, the high bytes being zero. The banks themselves still come from the
  // operand bytes.
  auto bus = busWith({{0x001000, kMvn}, {0x001001, 0x12}, {0x001002, 0x34},
                      {0x340040, 0xAA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000,
                             .a = 0x0000,
                             .x = 0x0040,
                             .y = 0x0080,
                             .p = kCpuFlagM | kCpuFlagX});
  runOneByte(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[3].address, 0x340040u);
  EXPECT_EQ(bus.trace[4].address, 0x120080u);
}

TEST(Cpu65816BlockMove, ANarrowIndexStepsInItsLowByteAlone) {
  // Stepping past $FF wraps within the page rather than reaching into the high byte,
  // which stays zero while the index registers are 8-bit.
  auto bus = busWith({{0x001000, kMvn}, {0x001001, 0x12}, {0x001002, 0x34}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000,
                             .a = 0x0001,
                             .x = 0x00FF,
                             .y = 0x00FF,
                             .p = kCpuFlagM | kCpuFlagX});
  runOneByte(bus, cpu);
  EXPECT_EQ(cpu.state().x, 0x0000);
  EXPECT_EQ(cpu.state().y, 0x0000);
  // The second byte therefore addresses the bottom of each bank.
  runOneByte(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 14u);
  EXPECT_EQ(bus.trace[10].address, 0x340000u);
  EXPECT_EQ(bus.trace[11].address, 0x120000u);
}

TEST(Cpu65816BlockMove, AWideIndexStepsThroughItsWholeWidth) {
  // With 16-bit index registers the same step carries into the high byte, and the
  // move crosses within its bank rather than wrapping in a page.
  auto bus = busWith({{0x001000, kMvn}, {0x001001, 0x12}, {0x001002, 0x34}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0001, .x = 0x00FF, .y = 0x00FF});
  runOneByte(bus, cpu);
  EXPECT_EQ(cpu.state().x, 0x0100);
  EXPECT_EQ(cpu.state().y, 0x0100);
}

TEST(Cpu65816BlockMove, AWideIndexWrapsWithinItsBank) {
  auto bus = busWith({{0x001000, kMvn}, {0x001001, 0x12}, {0x001002, 0x34}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0001, .x = 0xFFFF, .y = 0xFFFF});
  runOneByte(bus, cpu);
  EXPECT_EQ(cpu.state().x, 0x0000);
  EXPECT_EQ(cpu.state().y, 0x0000);
}

// ---- a move in progress is a value ----

TEST(Cpu65816BlockMove, AMidByteSnapshotRestoresToTheSameCycle) {
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0002, .x = 0x0040, .y = 0x0080});
  bus.cpu = &cpu;
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);  // part-way through the first byte
  ASSERT_FALSE(cpu.atInstructionBoundary());
  const Cpu65816State midway = cpu.state();

  while (!cpu.atInstructionBoundary()) cpu.stepCycle(bus);
  const Cpu65816State finished = cpu.state();

  auto second = moveBus(kMvn);
  Cpu65816 resumed(midway);
  second.cpu = &resumed;
  while (!resumed.atInstructionBoundary()) resumed.stepCycle(second);
  EXPECT_EQ(resumed.state().a, finished.a);
  EXPECT_EQ(resumed.state().x, finished.x);
  EXPECT_EQ(resumed.state().y, finished.y);
  EXPECT_EQ(resumed.state().pc, finished.pc);
  EXPECT_EQ(second.mem[0x120080], bus.mem[0x120080]);
}

TEST(Cpu65816BlockMove, AMoveInProgressIsCarriedByTheOrdinaryRegisters) {
  // Nothing hidden tracks how far a move has got: the indexes address the next byte,
  // the accumulator counts what is left, and the program counter names the
  // instruction to re-enter.
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x0002, .x = 0x0040, .y = 0x0080});
  runOneByte(bus, cpu);

  auto elsewhere = moveBus(kMvn);
  Cpu65816 fresh(cpu.state());
  EXPECT_EQ(runWholeMove(elsewhere, fresh), 14u);
  EXPECT_EQ(elsewhere.mem[0x120081], 0xBB);
  EXPECT_EQ(elsewhere.mem[0x120082], 0xCC);
}

// ---- interrupting a move ----

TEST(Cpu65816BlockMove, ARequestIsTakenBetweenBytesAndSavesTheMovesOwnAddress) {
  // Eyes & Lichty: "The value pushed onto the stack when a block move is interrupted
  // is the address of the block move instruction. The current byte-move is completed
  // before the interrupt is serviced." Both fall out of the re-entry — a request that
  // arrives part-way through a byte waits for the boundary that byte ends on, and the
  // program counter is the move's own there.
  auto bus = moveBus(kMvn);
  bus.mem[0x00FFEE] = 0x00;  // the native interrupt request vector
  bus.mem[0x00FFEF] = 0x20;
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .a = 0x0002,
                             .x = 0x0040, .y = 0x0080});
  bus.cpu = &cpu;

  for (int i = 0; i < 3; ++i) cpu.stepCycle(bus);  // part-way through the first byte
  cpu.setIrqLine(true);
  ASSERT_FALSE(cpu.atInstructionBoundary());
  while (!cpu.atInstructionBoundary()) cpu.stepCycle(bus);
  EXPECT_EQ(bus.mem[0x120080], 0xAA);  // the byte under way finished first

  cpu.stepInstruction(bus);            // and the request is taken next
  EXPECT_EQ(cpu.state().pc, 0x2000);
  EXPECT_EQ(cpu.state().servicing, InterruptRequest::None);
  // The saved address is the move's own, so the handler returns into it. The bank,
  // the high byte and the low byte go down in that order from $0001FF.
  EXPECT_EQ(bus.mem[0x0001FE], 0x10);
  EXPECT_EQ(bus.mem[0x0001FD], 0x00);
  // The second byte has not moved: the move stopped on a byte boundary, not inside
  // one.
  EXPECT_EQ(bus.mem[0x120081], 0x00);
}

TEST(Cpu65816BlockMove, AnInterruptedMoveResumesAndFinishes) {
  // With the registers intact, returning to the saved address carries the move on
  // from where it stopped: the remaining bytes land and none is moved twice.
  auto bus = moveBus(kMvn);
  bus.mem[0x00FFEE] = 0x00;
  bus.mem[0x00FFEF] = 0x20;
  bus.mem[0x002000] = 0x40;  // the handler is a bare RTI
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .a = 0x0002,
                             .x = 0x0040, .y = 0x0080});
  bus.cpu = &cpu;

  cpu.stepInstruction(bus);  // the first byte, uninterrupted
  cpu.setIrqLine(true);
  cpu.stepInstruction(bus);  // the sequence, taken at the boundary after it
  ASSERT_EQ(cpu.state().pc, 0x2000);
  cpu.setIrqLine(false);
  cpu.stepInstruction(bus);  // the RTI
  ASSERT_EQ(cpu.state().pc, 0x1000);

  runWholeMove(bus, cpu);
  EXPECT_EQ(bus.mem[0x120080], 0xAA);
  EXPECT_EQ(bus.mem[0x120081], 0xBB);
  EXPECT_EQ(bus.mem[0x120082], 0xCC);
  EXPECT_EQ(cpu.state().a, 0xFFFF);
  EXPECT_EQ(cpu.state().pc, 0x1003);
}

TEST(Cpu65816BlockMove, AMaskedRequestLeavesTheMoveAlone) {
  auto bus = moveBus(kMvn);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .a = 0x0002, .x = 0x0040,
                             .y = 0x0080, .p = kCpuFlagI});
  bus.cpu = &cpu;
  cpu.setIrqLine(true);
  EXPECT_EQ(runWholeMove(bus, cpu), 21u);
  EXPECT_EQ(bus.mem[0x120082], 0xCC);
}

// ---- the family's membership ----

TEST(Cpu65816BlockMove, BothBlockMovesRunOnTheCycleEngine) {
  for (const std::uint8_t opcode : {kMvn, kMvp}) {
    auto bus = busWith({{0x001000, opcode}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF});
    const std::uint32_t cycles = runOneByte(bus, cpu);
    EXPECT_EQ(cycles, 7u) << "opcode " << int{opcode};
    EXPECT_EQ(bus.trace.size(), cycles) << "opcode " << int{opcode};
  }
}

}  // namespace
