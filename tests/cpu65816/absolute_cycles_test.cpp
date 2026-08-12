#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The absolute and long instructions, cycle by cycle. The recorded vectors prove
// every opcode's whole trace; what is proven here is the documented shape behind
// them, with each expectation traced to the line of the W65C816S datasheet that
// states it: Table 5-7 rows 1a (absolute), 1d (absolute read-modify-write), 4a
// (absolute long), 5 (absolute long indexed), 6a (absolute indexed), 6b (absolute
// indexed read-modify-write) and 7 (absolute indexed with Y), their notes 1, 4 and
// 17, and sections 7.3 and 7.5 on absolute indexed addressing.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::cpu_vectors::kSignalMlb;
using snaggletooth::cpu_vectors::kSignalRw;
using snaggletooth::cpu_vectors::kSignalVda;
using snaggletooth::cpu_vectors::kSignalVpa;
using snaggletooth::cpu_vectors::RecordingBus;

RecordingBus busWith(
    std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

// Runs one instruction from $001000 and returns how many cycles it took, with the
// bus recording pin states from the processor as it goes.
std::uint32_t run(RecordingBus& bus, Cpu65816& cpu) {
  bus.cpu = &cpu;
  return cpu.stepInstruction(bus);
}

// The eight-bit widths as a processor status byte.
constexpr std::uint8_t kEightBit = kCpuFlagM | kCpuFlagX;

// ---- the operand bytes (Table 5-7 rows 1a and 4a) ----

TEST(Cpu65816Absolute, AnAbsoluteInstructionFetchesTwoOperandBytesThenTheData) {
  // Row 1a: the opcode, AAL, AAH, then the data at DBR,AA.
  auto bus = busWith(
      {{0x001000, 0xAD}, {0x001001, 0x34}, {0x001002, 0x12}, {0x001234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].address, 0x001002u);
  EXPECT_EQ(bus.trace[3].address, 0x001234u);
}

TEST(Cpu65816Absolute, ALongInstructionFetchesAThirdOperandByteForItsBank) {
  // Row 4a: a fourth cycle fetches AAB, and the data comes from AAB,AA rather than
  // from the data bank.
  auto bus = busWith({{0x001000, 0xAF},
                      {0x001001, 0x34},
                      {0x001002, 0x12},
                      {0x001003, 0x7E},
                      {0x7E1234, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x001003u);
  EXPECT_EQ(bus.trace[4].address, 0x7E1234u);
}

// ---- the indexing cycle (Table 5-7 note 4) ----

TEST(Cpu65816Absolute, AnEightBitIndexedReadPaysNothingWhenItStaysInThePage) {
  // Note 4 adds the cycle "for indexing across page boundaries, or write, or X=0" —
  // a read with eight-bit index registers that does not cross pays for none of the
  // three, so it is row 6a's shortest form.
  auto bus = busWith(
      {{0x001000, 0xBD}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001205, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x05, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Absolute, IndexingAcrossAPageBoundaryCostsACycle) {
  // Note 4, first clause: the same instruction whose addition carries out of the
  // low byte spends a cycle waiting for the carry.
  auto bus = busWith(
      {{0x001000, 0xBD}, {0x001001, 0xF8}, {0x001002, 0x12}, {0x001308, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x10, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Absolute, SixteenBitIndexRegistersAlwaysPayTheIndexingCycle) {
  // Note 4, third clause ("or X=0"): with sixteen-bit index registers the cycle is
  // paid whether or not the low-byte addition carries.
  auto bus = busWith(
      {{0x001000, 0xBD}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001305, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x0105, .p = kCpuFlagM});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Absolute, AnIndexedStoreAlwaysPaysTheIndexingCycle) {
  // Note 4, second clause ("or write"): a store pays even where the matching read
  // would not, because it cannot begin writing before the address is certain.
  auto bus = busWith({{0x001000, 0x9D}, {0x001001, 0x00}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .a = 0x7F, .x = 0x05, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(bus.mem[0x001205], 0x7F);
}

TEST(Cpu65816Absolute, AnIndexedReadModifyWriteAlwaysPaysTheIndexingCycle) {
  // Row 6b carries the indexing cycle unconditionally, with no note against it —
  // section 7.5 says as much in prose, naming the read-modify-write "when using
  // 8- or 16-bit Index Register modes".
  auto bus = busWith(
      {{0x001000, 0x1E}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001205, 0x42}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x05, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 7u);
  EXPECT_EQ(bus.mem[0x001205], 0x84);
}

TEST(Cpu65816Absolute, TheIndexingCycleDrivesTheAddressWithoutTheCarry) {
  // Row 6a gives that cycle's address as DBR,AAH,AAL+XL: the bank and high byte of
  // the absolute address with only the low byte of the index added. Section 7.5:
  // "The Page and Bank addresses could also be invalid. This will be due to low
  // byte addition only." So the cycle drives $001208 — a page below the $001308 the
  // instruction goes on to read — and drives neither a valid data nor a valid
  // program address.
  auto bus = busWith(
      {{0x001000, 0xBD}, {0x001001, 0xF8}, {0x001002, 0x12}, {0x001308, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x10, .p = kEightBit});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x001208u);
  EXPECT_FALSE(bus.trace[3].value.has_value());
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[3].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.trace[4].address, 0x001308u);
}

TEST(Cpu65816Absolute, TheIndexingCycleIgnoresTheIndexHighByte) {
  // The address is formed from AAL+XL alone, so a sixteen-bit index contributes
  // only its low byte to it — $001205 here, while the read reaches $001305.
  auto bus = busWith(
      {{0x001000, 0xBD}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001305, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x0105, .p = kCpuFlagM});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x001205u);
  EXPECT_EQ(bus.trace[4].address, 0x001305u);
}

TEST(Cpu65816Absolute, ALongIndexedInstructionNeverPaysAnIndexingCycle) {
  // Row 5 lists no such cycle and carries no note 4: the index is added to a full
  // 24-bit address, so there is no page carry to wait for.
  auto bus = busWith({{0x001000, 0xBF},
                      {0x001001, 0xF8},
                      {0x001002, 0x12},
                      {0x001003, 0x7E},
                      {0x7E1308, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x10, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
}

// ---- the absolute indexed addressing range (datasheet section 7.3) ----

TEST(Cpu65816Absolute, AnIndexedAddressRunsOnIntoTheNextBank) {
  // Section 7.3: "indexing from page ZZFFXX may result in ZZ+1,00YY when using the
  // W65C816S" — the addition carries into the bank rather than wrapping inside it.
  auto bus = busWith(
      {{0x001000, 0xBD}, {0x001001, 0xFF}, {0x001002, 0xFF}, {0x010000, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x01, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
  EXPECT_EQ(bus.trace.back().address, 0x010000u);
}

TEST(Cpu65816Absolute, AnAbsoluteAddressIsFormedFromTheDataBank) {
  // Row 1a reaches DBR,AA — the data bank register supplies the bank an absolute
  // operand does not carry.
  auto bus = busWith(
      {{0x001000, 0xAD}, {0x001001, 0x34}, {0x001002, 0x12}, {0x7E1234, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .dbr = 0x7E});
  run(bus, cpu);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
  EXPECT_EQ(bus.trace.back().address, 0x7E1234u);
}

// ---- read-modify-write (Table 5-7 rows 1d and 6b, and note 17) ----

TEST(Cpu65816Absolute, TheMemoryLockIsHeldAcrossTheWholeReadModifyWrite) {
  // Row 1d holds MLB low from the data read through the write back, the modify
  // cycle between them included, and never on the fetches.
  auto bus = busWith(
      {{0x001000, 0x0E}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001200, 0x42}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[0].signals[kSignalMlb], '-');
  EXPECT_EQ(bus.trace[1].signals[kSignalMlb], '-');
  EXPECT_EQ(bus.trace[2].signals[kSignalMlb], '-');
  EXPECT_EQ(bus.trace[3].signals[kSignalMlb], 'l');
  EXPECT_EQ(bus.trace[4].signals[kSignalMlb], 'l');
  EXPECT_EQ(bus.trace[5].signals[kSignalMlb], 'l');
  EXPECT_EQ(bus.mem[0x001200], 0x84);
}

TEST(Cpu65816Absolute, TheModifyCycleParksOnTheLastAddressRead) {
  // Row 1d cycle 5 drives DBR,AA+1 after a sixteen-bit read and DBR,AA after an
  // eight-bit one — the address the last read reached, either way.
  auto eight = busWith(
      {{0x001000, 0x0E}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001200, 0x42}});
  Cpu65816 narrow(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  run(eight, narrow);
  ASSERT_EQ(eight.trace.size(), 6u);
  EXPECT_EQ(eight.trace[4].address, 0x001200u);
  EXPECT_FALSE(eight.trace[4].value.has_value());

  auto sixteen = busWith({{0x001000, 0x0E},
                          {0x001001, 0x00},
                          {0x001002, 0x12},
                          {0x001200, 0x34},
                          {0x001201, 0x12}});
  Cpu65816 wide(Cpu65816State{.pc = 0x1000});
  run(sixteen, wide);
  ASSERT_EQ(sixteen.trace.size(), 8u);
  EXPECT_EQ(sixteen.trace[5].address, 0x001201u);
  EXPECT_FALSE(sixteen.trace[5].value.has_value());
}

TEST(Cpu65816Absolute, ASixteenBitReadModifyWriteWritesTheHighByteFirst) {
  // Row 1d: cycle 6a writes Data High at DBR,AA+1 before cycle 6 writes Data Low at
  // DBR,AA — the reverse of the order the two bytes were read in.
  auto bus = busWith({{0x001000, 0x0E},
                      {0x001001, 0x00},
                      {0x001002, 0x12},
                      {0x001200, 0x34},
                      {0x001201, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  EXPECT_EQ(run(bus, cpu), 8u);
  ASSERT_EQ(bus.trace.size(), 8u);
  EXPECT_EQ(bus.trace[3].address, 0x001200u);  // read low
  EXPECT_EQ(bus.trace[4].address, 0x001201u);  // read high
  EXPECT_EQ(bus.trace[6].address, 0x001201u);  // write high
  EXPECT_EQ(bus.trace[6].value, 0x24);
  EXPECT_EQ(bus.trace[7].address, 0x001200u);  // write low
  EXPECT_EQ(bus.trace[7].value, 0x68);
}

TEST(Cpu65816Absolute, EmulationModeWritesTheUnmodifiedByteOnTheModifyCycle) {
  // Note 17: "In the emulation mode, during a R-M-W instruction the RWB is low
  // during both write and modify cycles." The byte it drives is the one just read,
  // so the address is written twice — the unmodified value, then the result.
  auto bus = busWith(
      {{0x001000, 0x0E}, {0x001001, 0x00}, {0x001002, 0x12}, {0x001200, 0x42}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[4].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.trace[4].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[4].value, 0x42);
  EXPECT_EQ(bus.trace[4].address, 0x001200u);
  EXPECT_EQ(bus.trace[5].value, 0x84);
  EXPECT_EQ(bus.mem[0x001200], 0x84);
  // The address genuinely takes two writes, not one.
  EXPECT_EQ(bus.writes.size(), 2u);
}

// ---- operand width (Table 5-7 note 1) ----

TEST(Cpu65816Absolute, AnIndexInstructionIsSizedByTheIndexWidth) {
  // Note 1 adds the cycle for M=0 or X=0 — for LDX it is the index width that
  // decides, so a sixteen-bit index register reads two bytes while A stays narrow.
  auto bus = busWith({{0x001000, 0xAE},
                      {0x001001, 0x00},
                      {0x001002, 0x12},
                      {0x001200, 0x34},
                      {0x001201, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kCpuFlagM});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().x, 0x1234);
}

TEST(Cpu65816Absolute, AStoreWritesAsManyBytesAsItsRegisterIsWide) {
  // STZ stores zero at the accumulator width, so sixteen bits clear two bytes.
  auto bus = busWith({{0x001000, 0x9C},
                      {0x001001, 0x00},
                      {0x001002, 0x12},
                      {0x001200, 0xAA},
                      {0x001201, 0xBB}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kCpuFlagX});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(bus.mem[0x001200], 0x00);
  EXPECT_EQ(bus.mem[0x001201], 0x00);
}

// ---- the documented cycle ranges ----

TEST(Cpu65816Absolute, AnAbsoluteReadTakesFourOrFiveCycles) {
  // Row 1a: "3 bytes, 4 & 5 cycles" — one per operand byte at the accumulator width.
  const std::uint32_t expected[] = {4u, 5u};
  int i = 0;
  for (const std::uint8_t p : {kEightBit, std::uint8_t{0}}) {
    auto bus = busWith({{0x001000, 0xAD}, {0x001001, 0x00}, {0x001002, 0x12}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = p});
    EXPECT_EQ(run(bus, cpu), expected[i]) << "case " << i;
    ++i;
  }
}

TEST(Cpu65816Absolute, AnAbsoluteReadModifyWriteTakesSixOrEightCycles) {
  // Row 1d: "3 bytes, 6 & 8 cycles" — the sixteen-bit form reads and writes twice.
  const std::uint32_t expected[] = {6u, 8u};
  int i = 0;
  for (const std::uint8_t p : {kEightBit, std::uint8_t{0}}) {
    auto bus = busWith({{0x001000, 0x0E}, {0x001001, 0x00}, {0x001002, 0x12}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = p});
    EXPECT_EQ(run(bus, cpu), expected[i]) << "case " << i;
    ++i;
  }
}

TEST(Cpu65816Absolute, AnAbsoluteIndexedReadTakesFourToSixCycles) {
  // Row 6a: "3 bytes, 4,5 and 6 cycles" — the operand width and the indexing cycle
  // between them cover exactly that range.
  struct Case {
    std::uint8_t p;
    std::uint8_t aal;
    std::uint32_t cycles;
  };
  const Case cases[] = {{kEightBit, 0x00, 4u},
                        {kEightBit, 0xF8, 5u},
                        {kCpuFlagX, 0x00, 5u},
                        {kCpuFlagX, 0xF8, 6u}};
  int i = 0;
  for (const Case& c : cases) {
    auto bus = busWith({{0x001000, 0xBD}, {0x001001, c.aal}, {0x001002, 0x12}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x10, .p = c.p});
    EXPECT_EQ(run(bus, cpu), c.cycles) << "case " << i;
    ++i;
  }
}

TEST(Cpu65816Absolute, AnIndexedReadModifyWriteTakesSevenOrNineCycles) {
  // Row 6b: "6 OpCodes, 3 bytes, 7 and 9 cycles".
  const std::uint32_t expected[] = {7u, 9u};
  int i = 0;
  for (const std::uint8_t p : {kEightBit, std::uint8_t{0}}) {
    auto bus = busWith({{0x001000, 0x1E}, {0x001001, 0x00}, {0x001002, 0x12}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x05, .p = p});
    EXPECT_EQ(run(bus, cpu), expected[i]) << "case " << i;
    ++i;
  }
}

TEST(Cpu65816Absolute, ALongReadTakesFiveOrSixCycles) {
  // Rows 4a and 5: "4 bytes, 5 & 6 cycles" — the indexed form costs the same as the
  // plain one, since it never pays for the addition.
  const std::uint32_t expected[] = {5u, 6u};
  for (const std::uint8_t opcode : {std::uint8_t{0xAF}, std::uint8_t{0xBF}}) {
    int i = 0;
    for (const std::uint8_t p : {kEightBit, std::uint8_t{0}}) {
      auto bus = busWith({{0x001000, opcode},
                          {0x001001, 0xF8},
                          {0x001002, 0x12},
                          {0x001003, 0x7E}});
      Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x10, .p = p});
      EXPECT_EQ(run(bus, cpu), expected[i])
          << "opcode " << int{opcode} << " case " << i;
      ++i;
    }
  }
}

TEST(Cpu65816Absolute, IndexingWithYBehavesAsIndexingWithX) {
  // Row 7 carries the same note 4 as row 6a, so the Y-indexed modes pay the cycle
  // under the same three conditions and park at the same address.
  auto bus = busWith(
      {{0x001000, 0xB9}, {0x001001, 0xF8}, {0x001002, 0x12}, {0x001308, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .y = 0x10, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x001208u);
}

// ---- the instruction is a value part-way through ----

TEST(Cpu65816Absolute, AHalfFinishedAbsoluteInstructionSurvivesASnapshot) {
  // The progress fields carry the instruction, so a snapshot taken between two of
  // its cycles restores to the same cycle and finishes identically.
  auto bus = busWith({{0x001000, 0x1E},
                      {0x001001, 0x00},
                      {0x001002, 0x12},
                      {0x001205, 0x34},
                      {0x001206, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x05});
  bus.cpu = &cpu;
  for (int i = 0; i < 5; ++i) cpu.stepCycle(bus);
  ASSERT_FALSE(cpu.atInstructionBoundary());
  const Cpu65816State midway = cpu.state();

  while (!cpu.atInstructionBoundary()) cpu.stepCycle(bus);
  const std::uint8_t low = bus.mem[0x001205];
  const std::uint8_t high = bus.mem[0x001206];

  auto second = busWith({{0x001000, 0x1E},
                         {0x001001, 0x00},
                         {0x001002, 0x12},
                         {0x001205, 0x34},
                         {0x001206, 0x12}});
  Cpu65816 resumed(midway);
  second.cpu = &resumed;
  while (!resumed.atInstructionBoundary()) resumed.stepCycle(second);
  EXPECT_EQ(second.mem[0x001205], low);
  EXPECT_EQ(second.mem[0x001206], high);
  EXPECT_EQ(resumed.state().p, cpu.state().p);
}

// ---- the family's membership is its coverage ----

TEST(Cpu65816Absolute, EveryAbsoluteAndLongOpcodeRunsOnTheCycleEngine) {
  // The runner compares cycle by cycle only where the engine carries the opcode, so
  // naming the family here is what keeps its per-cycle coverage from narrowing.
  // Sixty-six opcodes: five addressing modes across loads, stores, arithmetic,
  // logic, BIT and the read-modify-writes.
  constexpr std::uint8_t kFamily[] = {
      // absolute, reading
      0x0D, 0x2C, 0x2D, 0x4D, 0x6D, 0xAC, 0xAD, 0xAE, 0xCC, 0xCD, 0xEC, 0xED,
      // absolute, writing
      0x8C, 0x8D, 0x8E, 0x9C,
      // absolute, read-modify-write
      0x0C, 0x0E, 0x1C, 0x2E, 0x4E, 0x6E, 0xCE, 0xEE,
      // absolute indexed with X
      0x1D, 0x3C, 0x3D, 0x5D, 0x7D, 0xBC, 0xBD, 0xDD, 0xFD, 0x9D, 0x9E,
      0x1E, 0x3E, 0x5E, 0x7E, 0xDE, 0xFE,
      // absolute indexed with Y
      0x19, 0x39, 0x59, 0x79, 0xB9, 0xBE, 0xD9, 0xF9, 0x99,
      // absolute long
      0x0F, 0x2F, 0x4F, 0x6F, 0xAF, 0xCF, 0xEF, 0x8F,
      // absolute long indexed
      0x1F, 0x3F, 0x5F, 0x7F, 0xBF, 0xDF, 0xFF, 0x9F,
  };
  static_assert(sizeof kFamily == 66);
  for (const std::uint8_t opcode : kFamily) {
    EXPECT_TRUE(Cpu65816::cycleStepped(opcode)) << "opcode " << int{opcode};
  }
}

}  // namespace
