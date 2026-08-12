#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The direct-page instructions, cycle by cycle. The recorded vectors prove every
// opcode's whole trace; what is proven here is the documented shape behind them,
// with each expectation traced to the line of the W65C816S datasheet that states it:
// Table 5-7 rows 10a (direct), 10b (direct read-modify-write), 16a (direct indexed
// with X), 16b (direct indexed read-modify-write) and 17 (direct indexed with Y),
// their notes 1, 2 and 17, and section 7.2 on the direct addressing range.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::cpu_vectors::kSignalMlb;
using snaggletooth::cpu_vectors::kSignalRw;
using snaggletooth::cpu_vectors::kSignalVda;
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

// The eight-bit widths, and the sixteen-bit ones, as processor status bytes.
constexpr std::uint8_t kEightBit = kCpuFlagM | kCpuFlagX;

// ---- the direct-register cycle (Table 5-7 note 2) ----

TEST(Cpu65816DirectPage, ADirectRegisterLowByteCostsACycle) {
  // Note 2: "Add 1 cycle for direct register low (DL) not equal 0."
  auto bus = busWith({{0x001000, 0xA5}, {0x001001, 0x10}, {0x001244, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1234, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a, 0x7E);
}

TEST(Cpu65816DirectPage, APageAlignedDirectRegisterCostsNothing) {
  // The same instruction with DL zero: row 10a's shortest form, three cycles.
  auto bus = busWith({{0x001000, 0xA5}, {0x001001, 0x10}, {0x001210, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  EXPECT_EQ(cpu.state().a, 0x7E);
}

TEST(Cpu65816DirectPage, TheDirectRegisterCycleParksOnTheOffsetItJustRead) {
  // Row 10a cycle 2a drives PBR,PC+1 — the address the offset came from — with
  // neither a valid data nor a valid program address.
  auto bus = busWith({{0x001000, 0xA5}, {0x001001, 0x10}, {0x001244, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1234, .p = kEightBit});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_FALSE(bus.trace[2].value.has_value());
  EXPECT_EQ(bus.trace[2].signals[kSignalVda], '-');
}

// ---- the indexing cycle (Table 5-7 rows 16a, 16b and 17) ----

TEST(Cpu65816DirectPage, IndexingAlwaysCostsACycle) {
  // Row 16a carries cycle 3 unconditionally — an indexed direct-page instruction
  // pays for the addition whatever the index width, unlike the absolute modes.
  auto bus = busWith({{0x001000, 0xB5}, {0x001001, 0x10}, {0x001215, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 5, .d = 0x1200, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a, 0x7E);
}

TEST(Cpu65816DirectPage, BothInternalCyclesParkOnTheOffsetAddress) {
  // Row 16b: cycle 2a for the direct register and cycle 3 for the indexing, both
  // driving PBR,PC+1.
  auto bus = busWith({{0x001000, 0xB5}, {0x001001, 0x10}, {0x001249, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 5, .d = 0x1234, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[3].address, 0x001001u);
  EXPECT_FALSE(bus.trace[2].value.has_value());
  EXPECT_FALSE(bus.trace[3].value.has_value());
}

TEST(Cpu65816DirectPage, TheIndexRegisterIsAddedAtItsOwnWidth) {
  // A sixteen-bit index reaches past the page the eight-bit one would stop in.
  auto bus = busWith({{0x001000, 0xB5}, {0x001001, 0x10}, {0x001310, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x0100, .d = 0x1200, .p = kCpuFlagM});
  run(bus, cpu);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7E);
}

// ---- the direct addressing range (datasheet section 7.2) ----

TEST(Cpu65816DirectPage, EmulationModeConfinesAnIndexedAddressToTheDirectPage) {
  // Section 7.2.2: in emulation mode with DL zero, the direct addressing range is
  // 00DH00 to 00DHFF — the indexed sum wraps inside the page rather than leaving it.
  auto bus = busWith({{0x001000, 0xB5}, {0x001001, 0x20}, {0x001210, 0x7E}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .x = 0xF0, .d = 0x1200, .p = kEightBit, .e = true});
  run(bus, cpu);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7E);
  EXPECT_EQ(bus.trace.back().address, 0x001210u);
}

TEST(Cpu65816DirectPage, EmulationModeWithADirectRegisterLowByteDoesNotConfine) {
  // Section 7.2.3: with DL not equal to zero the range is 000000 to 00FFFF again,
  // so the same addition leaves the page.
  auto bus = busWith({{0x001000, 0xB5}, {0x001001, 0x20}, {0x001344, 0x7E}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .x = 0xF0, .d = 0x1234, .p = kEightBit, .e = true});
  run(bus, cpu);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7E);
  EXPECT_EQ(bus.trace.back().address, 0x001344u);
}

TEST(Cpu65816DirectPage, NativeModeNeverConfinesTheAddressToThePage) {
  // Section 7.2.1: the native-mode range is 000000 to 00FFFF whatever DL holds, so
  // the addition that emulation mode wraps runs on into the next page here.
  auto bus = busWith({{0x001000, 0xB5}, {0x001001, 0x20}, {0x001310, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0xF0, .d = 0x1200, .p = kEightBit});
  run(bus, cpu);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7E);
  EXPECT_EQ(bus.trace.back().address, 0x001310u);
}

// ---- read-modify-write (Table 5-7 rows 10b and 16b, and note 17) ----

TEST(Cpu65816DirectPage, AReadModifyWriteReadsModifiesAndWrites) {
  // Row 10b at eight bits: the data read, the modify cycle, then the write back.
  auto bus = busWith({{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x42}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(bus.mem[0x001210], 0x84);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[2].signals[kSignalRw], 'r');
  EXPECT_EQ(bus.trace[4].signals[kSignalRw], 'w');
}

TEST(Cpu65816DirectPage, TheMemoryLockIsHeldAcrossTheWholeReadModifyWrite) {
  // Row 10b holds MLB low from the data read through the write back, the modify
  // cycle between them included.
  auto bus = busWith({{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x42}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200, .p = kEightBit});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[0].signals[kSignalMlb], '-');  // the opcode fetch
  EXPECT_EQ(bus.trace[1].signals[kSignalMlb], '-');  // the offset fetch
  EXPECT_EQ(bus.trace[2].signals[kSignalMlb], 'l');
  EXPECT_EQ(bus.trace[3].signals[kSignalMlb], 'l');
  EXPECT_EQ(bus.trace[4].signals[kSignalMlb], 'l');
}

TEST(Cpu65816DirectPage, TheModifyCycleReachesNoAddressInNativeMode) {
  // Row 10b cycle 4 drives neither a valid data nor a valid program address, and
  // moves no byte: the value changes inside the chip.
  auto bus = busWith({{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x42}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200, .p = kEightBit});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[3].signals[kSignalRw], 'r');
  EXPECT_FALSE(bus.trace[3].value.has_value());
}

TEST(Cpu65816DirectPage, EmulationModeWritesTheUnmodifiedByteOnTheModifyCycle) {
  // Note 17: "In the emulation mode, during a R-M-W instruction the RWB is low
  // during both write and modify cycles." The byte it drives is the one just read,
  // so the address is written twice — the unmodified value, then the result.
  auto bus = busWith({{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x42}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .d = 0x1200, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 5u);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[3].value, 0x42);
  EXPECT_EQ(bus.trace[3].address, 0x001210u);
  EXPECT_EQ(bus.trace[4].value, 0x84);
  EXPECT_EQ(bus.mem[0x001210], 0x84);
  // The address genuinely takes two writes, not one.
  EXPECT_EQ(bus.writes.size(), 2u);
}

TEST(Cpu65816DirectPage, ASixteenBitReadModifyWriteWritesTheHighByteFirst) {
  // Row 10b: cycle 5a writes Data High at 0,D+DO+1 before cycle 5 writes Data Low
  // at 0,D+DO — the reverse of the order the two bytes were read in.
  auto bus = busWith(
      {{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x34}, {0x001211, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200});
  EXPECT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[2].address, 0x001210u);  // read low
  EXPECT_EQ(bus.trace[3].address, 0x001211u);  // read high
  EXPECT_EQ(bus.trace[5].address, 0x001211u);  // write high
  EXPECT_EQ(bus.trace[5].value, 0x24);
  EXPECT_EQ(bus.trace[6].address, 0x001210u);  // write low
  EXPECT_EQ(bus.trace[6].value, 0x68);
}

TEST(Cpu65816DirectPage, ASixteenBitModifyCycleParksOnTheHighAddress) {
  // Row 10b cycle 4 drives 0,D+DO+1 — the address the last read reached.
  auto bus = busWith(
      {{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x34}, {0x001211, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[4].address, 0x001211u);
  EXPECT_FALSE(bus.trace[4].value.has_value());
  EXPECT_EQ(bus.trace[4].signals[kSignalMlb], 'l');
}

// ---- operand width (Table 5-7 note 1) ----

TEST(Cpu65816DirectPage, AnIndexInstructionIsSizedByTheIndexWidth) {
  // Note 1 adds the cycle for M=0 or X=0 — for LDX it is the index width that
  // decides, so a sixteen-bit index register reads two bytes while A stays narrow.
  auto bus = busWith(
      {{0x001000, 0xA6}, {0x001001, 0x10}, {0x001210, 0x34}, {0x001211, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200, .p = kCpuFlagM});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().x, 0x1234);
}

TEST(Cpu65816DirectPage, AnAccumulatorInstructionIsSizedByTheAccumulatorWidth) {
  auto bus = busWith(
      {{0x001000, 0xA5}, {0x001001, 0x10}, {0x001210, 0x34}, {0x001211, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200, .p = kCpuFlagX});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a, 0x1234);
}

TEST(Cpu65816DirectPage, AStoreWritesAsManyBytesAsItsRegisterIsWide) {
  // STZ stores zero at the accumulator width, so sixteen bits clear two bytes.
  auto bus = busWith({{0x001000, 0x64}, {0x001001, 0x10}, {0x001210, 0xAA},
                      {0x001211, 0xBB}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(bus.mem[0x001210], 0x00);
  EXPECT_EQ(bus.mem[0x001211], 0x00);
}

// ---- the documented cycle ranges ----

TEST(Cpu65816DirectPage, ADirectReadTakesThreeToFiveCycles) {
  // Row 10a: "2 bytes, 3, 4 & 5 cycles" — the four combinations of operand width
  // and direct-register low byte cover exactly that range.
  const std::uint32_t expected[] = {3u, 4u, 4u, 5u};
  int i = 0;
  for (const std::uint8_t p : {kEightBit, kEightBit, std::uint8_t{0}, std::uint8_t{0}}) {
    const std::uint16_t d = (i % 2 == 0) ? 0x1200 : 0x1234;
    auto bus = busWith({{0x001000, 0xA5}, {0x001001, 0x10}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = d, .p = p});
    EXPECT_EQ(run(bus, cpu), expected[i]) << "case " << i;
    ++i;
  }
}

TEST(Cpu65816DirectPage, ADirectReadModifyWriteTakesFiveToEightCycles) {
  // Row 10b: "8 OpCodes, 2 bytes, 5,6,7 and 8 cycles".
  const std::uint32_t expected[] = {5u, 6u, 7u, 8u};
  int i = 0;
  for (const std::uint8_t p : {kEightBit, kEightBit, std::uint8_t{0}, std::uint8_t{0}}) {
    const std::uint16_t d = (i % 2 == 0) ? 0x1200 : 0x1234;
    auto bus = busWith({{0x001000, 0x06}, {0x001001, 0x10}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = d, .p = p});
    EXPECT_EQ(run(bus, cpu), expected[i]) << "case " << i;
    ++i;
  }
}

TEST(Cpu65816DirectPage, AnIndexedReadModifyWriteTakesSixToNineCycles) {
  // Row 16b: "6 OpCodes, 2 bytes, 6,7,8 and 9 cycles".
  const std::uint32_t expected[] = {6u, 7u, 8u, 9u};
  int i = 0;
  for (const std::uint8_t p : {kEightBit, kEightBit, std::uint8_t{0}, std::uint8_t{0}}) {
    const std::uint16_t d = (i % 2 == 0) ? 0x1200 : 0x1234;
    auto bus = busWith({{0x001000, 0x16}, {0x001001, 0x10}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 4, .d = d, .p = p});
    EXPECT_EQ(run(bus, cpu), expected[i]) << "case " << i;
    ++i;
  }
}

// ---- the instruction is a value part-way through ----

TEST(Cpu65816DirectPage, AHalfFinishedDirectPageInstructionSurvivesASnapshot) {
  // The progress fields carry the instruction, so a snapshot taken between two of
  // its cycles restores to the same cycle and finishes identically.
  auto bus = busWith(
      {{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x34}, {0x001211, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x1200});
  bus.cpu = &cpu;
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  ASSERT_FALSE(cpu.atInstructionBoundary());
  const Cpu65816State midway = cpu.state();

  while (!cpu.atInstructionBoundary()) cpu.stepCycle(bus);
  const std::uint8_t low = bus.mem[0x001210];
  const std::uint8_t high = bus.mem[0x001211];

  auto second = busWith(
      {{0x001000, 0x06}, {0x001001, 0x10}, {0x001210, 0x34}, {0x001211, 0x12}});
  Cpu65816 resumed(midway);
  second.cpu = &resumed;
  while (!resumed.atInstructionBoundary()) resumed.stepCycle(second);
  EXPECT_EQ(second.mem[0x001210], low);
  EXPECT_EQ(second.mem[0x001211], high);
  EXPECT_EQ(resumed.state().p, cpu.state().p);
}

TEST(Cpu65816DirectPage, EveryDirectPageOpcodeRunsOnTheCycleEngine) {
  // The runner compares cycle by cycle only where the engine carries the opcode, so
  // the family's membership is the coverage. Forty-four opcodes: the three
  // addressing modes across loads, stores, arithmetic, logic and the shifts.
  int carried = 0;
  for (int op = 0; op < 256; ++op) {
    if (Cpu65816::cycleStepped(static_cast<std::uint8_t>(op))) ++carried;
  }
  // The forty-seven the engine already carried, plus this family.
  EXPECT_EQ(carried, 47 + 44);
}

}  // namespace
