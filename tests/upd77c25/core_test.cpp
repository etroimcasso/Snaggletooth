// The µPD77C25 core against fullsnes, "SNES Cart DSP-n/ST010/ST011 - NEC uPD77C25":
// the three instruction forms, the sixteen ALU operations and their flags, the operand
// sources, every source and destination, the pointer adjustments, the multiplier, the
// jumps and the stack, reset, and the host's two ports. Every program is a few words
// encoded here field by field, so each case reads as the instruction it runs.

#include "snaggletooth/upd77c25/upd77c25.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ios>
#include <vector>

namespace {

using snaggletooth::Upd77c25;
using snaggletooth::Upd77c25AluOp;
using snaggletooth::Upd77c25Branch;
using snaggletooth::Upd77c25Destination;
using snaggletooth::Upd77c25Operand;
using snaggletooth::Upd77c25Program;
using snaggletooth::Upd77c25Source;
using snaggletooth::Upd77c25State;

using Op = Upd77c25AluOp;
using P = Upd77c25Operand;
using Src = Upd77c25Source;
using Dst = Upd77c25Destination;
using Br = Upd77c25Branch;

constexpr std::uint8_t kS1 = Upd77c25State::kFlagS1;
constexpr std::uint8_t kS0 = Upd77c25State::kFlagS0;
constexpr std::uint8_t kC = Upd77c25State::kFlagC;
constexpr std::uint8_t kZ = Upd77c25State::kFlagZ;
constexpr std::uint8_t kOv1 = Upd77c25State::kFlagOv1;
constexpr std::uint8_t kOv0 = Upd77c25State::kFlagOv0;
constexpr std::uint8_t kAllFlags = kS1 | kS0 | kC | kZ | kOv1 | kOv0;

constexpr std::uint16_t kRqm = Upd77c25State::kSrRqm;
constexpr std::uint16_t kDrs = Upd77c25State::kSrDrs;
constexpr std::uint16_t kDrc = Upd77c25State::kSrDrc;

enum class Acc { A, B };

// An ALU word: the operation on the accumulator `acc` with operand `p`, moving `src`
// to `dst`, with no pointer adjustment and no return.
std::uint32_t alu(Op op, P p, Acc acc, Src src, Dst dst) {
  return (static_cast<std::uint32_t>(p) << 20) | (static_cast<std::uint32_t>(op) << 16) |
         (acc == Acc::B ? 0x8000u : 0u) | (static_cast<std::uint32_t>(src) << 4) |
         static_cast<std::uint32_t>(dst);
}

// An ALU word that only moves `src` to `dst`.
std::uint32_t move(Src src, Dst dst) { return alu(Op::Nop, P::Ram, Acc::A, src, dst); }

// The ALU word's adjustment fields, added to a word.
std::uint32_t withDpl(std::uint32_t word, unsigned dpl) { return word | (dpl << 13); }
std::uint32_t withDph(std::uint32_t word, unsigned dph) { return word | (dph << 9); }
std::uint32_t withRpDecrement(std::uint32_t word) { return word | 0x100u; }
std::uint32_t withReturn(std::uint32_t word) { return word | 0x400000u; }

constexpr unsigned kDpInc = 1;
constexpr unsigned kDpDec = 2;
constexpr unsigned kDpClr = 3;

// A jump word.
std::uint32_t jp(Br branch, std::uint16_t nextAddress) {
  return 0x800000u | (static_cast<std::uint32_t>(branch) << 13) |
         (std::uint32_t{nextAddress} << 2);
}

// A jump word with a raw BRCH value.
std::uint32_t jpRaw(std::uint16_t branch, std::uint16_t nextAddress) {
  return 0x800000u | (std::uint32_t{branch} << 13) | (std::uint32_t{nextAddress} << 2);
}

// A load word.
std::uint32_t ld(std::uint16_t immediate, Dst dst) {
  return 0xC00000u | (std::uint32_t{immediate} << 6) | static_cast<std::uint32_t>(dst);
}

// A program holding `words` from address 0.
Upd77c25Program programOf(std::initializer_list<std::uint32_t> words) {
  Upd77c25Program program;
  std::size_t address = 0;
  for (std::uint32_t word : words) program.code[address++] = word;
  return program;
}

void run(Upd77c25& chip, int instructions) {
  for (int i = 0; i < instructions; ++i) chip.stepCycle();
}

// One ALU operation on AccA with the operand on the bus from TR, from `acc` and
// `flags`, with `otherFlags` in FlagB.
Upd77c25State aluOnA(Op op, std::uint16_t acc, std::uint16_t operand, std::uint8_t flags,
                     std::uint8_t otherFlags = 0) {
  const Upd77c25Program program = programOf({alu(op, P::Bus, Acc::A, Src::Tr, Dst::None)});
  Upd77c25State state;
  state.accA = acc;
  state.tr = operand;
  state.flagA = flags;
  state.flagB = otherFlags;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  return chip.state();
}

// ---------------------------------------------------------------------------------------
// The three forms

TEST(Upd77c25Core, AWordWithBit23ClearIsAnAluWordWhicheverBit22Is) {
  Upd77c25Program program = programOf({
      alu(Op::Add, P::Bus, Acc::A, Src::Tr, Dst::None),
      withReturn(alu(Op::Add, P::Bus, Acc::A, Src::Tr, Dst::None)),
  });
  Upd77c25State state;
  state.tr = 0x0010;
  state.stack[0] = 0x0123;
  state.sp = 1;
  Upd77c25 chip(program, state);
  run(chip, 2);
  EXPECT_EQ(chip.state().accA, 0x0020);  // both words added
  EXPECT_EQ(chip.state().pc, 0x0123);    // the second returned after it
}

TEST(Upd77c25Core, AWordWhoseTopBitsAreOneZeroIsAJump) {
  const Upd77c25Program program = programOf({jp(Br::Jmp, 0x2A5)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_EQ(chip.state().pc, 0x2A5);
  EXPECT_EQ(chip.state().accA, 0);
}

TEST(Upd77c25Core, AWordWhoseTopBitsAreOneOneIsALoadAndTheTopByteIsIgnored) {
  const Upd77c25Program program = programOf({0xFF000000u | ld(0xBEEF, Dst::AccB)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accB, 0xBEEF);
  EXPECT_EQ(chip.state().pc, 1);
}

// ---------------------------------------------------------------------------------------
// The sixteen operations

TEST(Upd77c25Core, NopChangesNeitherTheAccumulatorNorAFlag) {
  const Upd77c25State after = aluOnA(Op::Nop, 0x1234, 0x5678, kAllFlags);
  EXPECT_EQ(after.accA, 0x1234);
  EXPECT_EQ(after.flagA, kAllFlags);
}

TEST(Upd77c25Core, OrSetsS1AndS0FromTheSignAndClearsCarryAndOverflow) {
  const Upd77c25State after = aluOnA(Op::Or, 0x8200, 0x0034, kAllFlags);
  EXPECT_EQ(after.accA, 0x8234);
  EXPECT_EQ(after.flagA, kS1 | kS0);
}

TEST(Upd77c25Core, AndToZeroSetsZAlone) {
  const Upd77c25State after = aluOnA(Op::And, 0xFF00, 0x00FF, kAllFlags);
  EXPECT_EQ(after.accA, 0x0000);
  EXPECT_EQ(after.flagA, kZ);
}

TEST(Upd77c25Core, Xor) {
  const Upd77c25State after = aluOnA(Op::Xor, 0xAAAA, 0x5555, 0);
  EXPECT_EQ(after.accA, 0xFFFF);
  EXPECT_EQ(after.flagA, kS1 | kS0);
}

TEST(Upd77c25Core, SubSetsCarryOnABorrowAndKeepsS1AndOv1WithoutAnOverflow) {
  const Upd77c25State after = aluOnA(Op::Sub, 0x0001, 0x0002, kOv1);
  EXPECT_EQ(after.accA, 0xFFFF);
  EXPECT_EQ(after.flagA, kS0 | kC | kOv1);
}

TEST(Upd77c25Core, AddOverflowSetsOv0AndS1AndFlipsOv1) {
  const Upd77c25State after = aluOnA(Op::Add, 0x7FFF, 0x0001, 0);
  EXPECT_EQ(after.accA, 0x8000);
  EXPECT_EQ(after.flagA, kS1 | kS0 | kOv1 | kOv0);
}

TEST(Upd77c25Core, AnOverflowWithOv1SetClearsIt) {
  const Upd77c25State after = aluOnA(Op::Add, 0x7FFF, 0x0001, kOv1);
  EXPECT_EQ(after.accA, 0x8000);
  EXPECT_EQ(after.flagA, kS1 | kS0 | kOv0);
}

TEST(Upd77c25Core, ANegativeOverflowLeavesS1Clear) {
  // -8000h - 1 leaves the range downwards; the result's sign, 0, is the direction.
  const Upd77c25State after = aluOnA(Op::Sub, 0x8000, 0x0001, kS1);
  EXPECT_EQ(after.accA, 0x7FFF);
  EXPECT_EQ(after.flagA, kOv1 | kOv0);
}

TEST(Upd77c25Core, AnAdditionWithoutOverflowKeepsS1AndOv1) {
  const Upd77c25State after = aluOnA(Op::Add, 0x0001, 0x0001, kS1 | kOv1 | kOv0 | kC);
  EXPECT_EQ(after.accA, 0x0002);
  EXPECT_EQ(after.flagA, kS1 | kOv1);
}

TEST(Upd77c25Core, SbbSubtractsTheOtherAccumulatorsCarryNotItsOwn) {
  // AccA's own carry clear, AccB's set: 5 - 2 - 1.
  const Upd77c25State after = aluOnA(Op::Sbb, 0x0005, 0x0002, 0, kC);
  EXPECT_EQ(after.accA, 0x0002);
  // AccA's own carry set, AccB's clear: 5 - 2 - 0.
  const Upd77c25State own = aluOnA(Op::Sbb, 0x0005, 0x0002, kC, 0);
  EXPECT_EQ(own.accA, 0x0003);
  EXPECT_EQ(own.flagA, 0);
}

TEST(Upd77c25Core, AdcAddsTheOtherAccumulatorsCarryNotItsOwn) {
  const Upd77c25State after = aluOnA(Op::Adc, 0xFFFF, 0x0000, 0, kC);
  EXPECT_EQ(after.accA, 0x0000);
  EXPECT_EQ(after.flagA, kC | kZ);
  const Upd77c25State own = aluOnA(Op::Adc, 0x0001, 0x0001, kC, 0);
  EXPECT_EQ(own.accA, 0x0002);
}

TEST(Upd77c25Core, DecIgnoresTheOperandAndBorrowsFromZero) {
  const Upd77c25State after = aluOnA(Op::Dec, 0x0000, 0x1234, 0);
  EXPECT_EQ(after.accA, 0xFFFF);
  EXPECT_EQ(after.flagA, kS0 | kC);
}

TEST(Upd77c25Core, IncIgnoresTheOperandAndCarriesOutOfFfffh) {
  const Upd77c25State after = aluOnA(Op::Inc, 0xFFFF, 0x1234, 0);
  EXPECT_EQ(after.accA, 0x0000);
  EXPECT_EQ(after.flagA, kC | kZ);
}

TEST(Upd77c25Core, NotComplementsTheAccumulator) {
  const Upd77c25State after = aluOnA(Op::Not, 0x0F0F, 0x1234, kC | kZ | kOv1 | kOv0);
  EXPECT_EQ(after.accA, 0xF0F0);
  EXPECT_EQ(after.flagA, kS1 | kS0);
}

TEST(Upd77c25Core, Sar1KeepsTheSignAndCarriesOutBit0) {
  const Upd77c25State after = aluOnA(Op::Sar1, 0x8003, 0x1234, 0);
  EXPECT_EQ(after.accA, 0xC001);
  EXPECT_EQ(after.flagA, kS1 | kS0 | kC);
}

TEST(Upd77c25Core, Rcl1ShiftsInTheOtherAccumulatorsCarryAndCarriesOutBit15) {
  const Upd77c25State after = aluOnA(Op::Rcl1, 0x8000, 0x1234, 0, kC);
  EXPECT_EQ(after.accA, 0x0001);
  EXPECT_EQ(after.flagA, kC);
}

TEST(Upd77c25Core, Sll2ShiftsInOnesAndClearsCarry) {
  const Upd77c25State after = aluOnA(Op::Sll2, 0xE000, 0x1234, kC);
  EXPECT_EQ(after.accA, 0x8003);
  EXPECT_EQ(after.flagA, kS1 | kS0);
}

TEST(Upd77c25Core, Sll4ShiftsInOnes) {
  const Upd77c25State after = aluOnA(Op::Sll4, 0x1234, 0x0000, kC);
  EXPECT_EQ(after.accA, 0x234F);
  EXPECT_EQ(after.flagA, 0);
}

TEST(Upd77c25Core, XchgSwapsTheBytes) {
  const Upd77c25State after = aluOnA(Op::Xchg, 0x1280, 0x0000, 0);
  EXPECT_EQ(after.accA, 0x8012);
  EXPECT_EQ(after.flagA, kS1 | kS0);
}

TEST(Upd77c25Core, TheABitNamesAccBAndItsFlagsAlone) {
  const Upd77c25Program program = programOf({alu(Op::Add, P::Bus, Acc::B, Src::Tr, Dst::None)});
  Upd77c25State state;
  state.accA = 0x1111;
  state.flagA = kZ;
  state.accB = 0x7FFF;
  state.tr = 0x0001;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accB, 0x8000);
  EXPECT_EQ(chip.state().flagB, kS1 | kS0 | kOv1 | kOv0);
  EXPECT_EQ(chip.state().accA, 0x1111);
  EXPECT_EQ(chip.state().flagA, kZ);
}

// ---------------------------------------------------------------------------------------
// The operand

TEST(Upd77c25Core, TheOperandRamIsRamAtDp) {
  const Upd77c25Program program = programOf({alu(Op::Add, P::Ram, Acc::A, Src::Tr, Dst::None)});
  Upd77c25State state;
  state.dp = 0x37;
  state.ram[0x37] = 0x0005;
  state.tr = 0x0100;
  state.accA = 0x0001;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accA, 0x0006);
}

TEST(Upd77c25Core, TheOperandBusIsTheValueTheSourcePutOnIt) {
  const Upd77c25Program program = programOf({alu(Op::Add, P::Bus, Acc::A, Src::K, Dst::None)});
  Upd77c25State state;
  state.k = 0x0040;
  state.accA = 0x0002;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accA, 0x0042);
}

TEST(Upd77c25Core, TheOperandProductHighIsTheDoubledProductsHighWord) {
  const Upd77c25Program program = programOf({
      ld(0xFFFF, Dst::K),  // -1
      ld(0x0003, Dst::L),  // 3: K·L·2 is -6, FFFFFFFAh
      alu(Op::Or, P::ProductHigh, Acc::A, Src::Trb, Dst::None),
  });
  Upd77c25 chip(program);
  run(chip, 3);
  EXPECT_EQ(chip.state().product, 0xFFFFFFFAu);
  EXPECT_EQ(chip.state().accA, 0xFFFF);

  const Upd77c25Program positive = programOf({
      ld(0x4001, Dst::K),
      ld(0x0003, Dst::L),  // 4001h · 3 · 2 = 18006h
      alu(Op::Or, P::ProductHigh, Acc::A, Src::Trb, Dst::None),
  });
  Upd77c25 second(positive);
  run(second, 3);
  EXPECT_EQ(second.state().accA, 0x0001);
}

TEST(Upd77c25Core, TheOperandProductLowIsTheDoubledProductsLowWord) {
  const Upd77c25Program program = programOf({
      ld(0x4001, Dst::K),
      ld(0x0003, Dst::L),  // 4001h · 3 · 2 = 18006h
      alu(Op::Or, P::ProductLow, Acc::A, Src::Trb, Dst::None),
  });
  Upd77c25 chip(program);
  run(chip, 3);
  EXPECT_EQ(chip.state().product, 0x00018006u);
  EXPECT_EQ(chip.state().accA, 0x8006);
}

// ---------------------------------------------------------------------------------------
// The sources and the destinations

TEST(Upd77c25Core, EachPlainSourceReachesTheDestination) {
  Upd77c25Program program = programOf({});
  program.data[0x155] = 0x6060;
  Upd77c25State state;
  state.trb = 0x1010;
  state.accA = 0x2020;
  state.accB = 0x3030;
  state.dp = 0x44;
  state.rp = 0x155;
  state.sr = 0x0081;
  state.dr = 0x9090;
  state.k = 0xD0D0;
  state.l = 0xE0E0;
  state.ram[0x44] = 0xF0F0;
  struct Row {
    Src source;
    std::uint16_t expected;
  };
  const std::vector<Row> rows = {
      {Src::Trb, 0x1010}, {Src::AccA, 0x2020}, {Src::AccB, 0x3030}, {Src::Dp, 0x0044},
      {Src::Rp, 0x0155},  {Src::Ro, 0x6060},   {Src::Drnf, 0x9090}, {Src::Sr, 0x0081},
      {Src::Sim, 0x0000}, {Src::Sil, 0x0000},  {Src::K, 0xD0D0},    {Src::L, 0xE0E0},
      {Src::Mem, 0xF0F0},
  };
  for (const Row& row : rows) {
    program.code[0] = move(row.source, Dst::Tr);
    Upd77c25 chip(program, state);
    chip.stepCycle();
    EXPECT_EQ(chip.state().tr, row.expected) << "source " << static_cast<int>(row.source);
  }
  program.code[0] = move(Src::Tr, Dst::Trb);
  state.tr = 0x3C3C;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().trb, 0x3C3C);
}

TEST(Upd77c25Core, EachPlainDestinationTakesTheValueAtItsWidth) {
  struct Row {
    Dst destination;
    std::uint16_t Upd77c25State::*field;
    std::uint16_t expected;
  };
  const std::vector<Row> rows = {
      {Dst::AccA, &Upd77c25State::accA, 0xABCD}, {Dst::AccB, &Upd77c25State::accB, 0xABCD},
      {Dst::Tr, &Upd77c25State::tr, 0xABCD},     {Dst::Trb, &Upd77c25State::trb, 0xABCD},
      {Dst::K, &Upd77c25State::k, 0xABCD},       {Dst::L, &Upd77c25State::l, 0xABCD},
      {Dst::Rp, &Upd77c25State::rp, 0x03CD},
  };
  for (const Row& row : rows) {
    const Upd77c25Program program = programOf({ld(0xABCD, row.destination)});
    Upd77c25 chip(program);
    chip.stepCycle();
    EXPECT_EQ(chip.state().*row.field, row.expected)
        << "destination " << static_cast<int>(row.destination);
  }
  const Upd77c25Program toDp = programOf({ld(0xABCD, Dst::Dp), ld(0x1234, Dst::Mem)});
  Upd77c25 chip(toDp);
  run(chip, 2);
  EXPECT_EQ(chip.state().dp, 0xCD);
  EXPECT_EQ(chip.state().ram[0xCD], 0x1234);
}

TEST(Upd77c25Core, SgnIs8000hLessAccAsS1WhicheverAccumulatorTheABitNames) {
  const Upd77c25Program program = programOf({
      alu(Op::Nop, P::Ram, Acc::A, Src::Sgn, Dst::Tr),
      alu(Op::Nop, P::Ram, Acc::B, Src::Sgn, Dst::Trb),
  });
  Upd77c25State clear;
  clear.flagB = kS1;
  Upd77c25 a(program, clear);
  run(a, 2);
  EXPECT_EQ(a.state().tr, 0x8000);
  EXPECT_EQ(a.state().trb, 0x8000);

  Upd77c25State set;
  set.flagA = kS1;
  Upd77c25 b(program, set);
  run(b, 2);
  EXPECT_EQ(b.state().tr, 0x7FFF);
  EXPECT_EQ(b.state().trb, 0x7FFF);
}

TEST(Upd77c25Core, KlrTakesKFromTheValueAndLFromTheDataRomAtRp) {
  Upd77c25Program program = programOf({move(Src::Tr, Dst::Klr)});
  program.data[0x2F1] = 0x0707;
  Upd77c25State state;
  state.tr = 0x0003;
  state.rp = 0x2F1;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().k, 0x0003);
  EXPECT_EQ(chip.state().l, 0x0707);
  EXPECT_EQ(chip.state().product, 0x0003u * 0x0707u * 2u);
}

TEST(Upd77c25Core, KlmTakesLFromTheValueAndKFromRamAtDpOr40h) {
  const Upd77c25Program program = programOf({move(Src::Tr, Dst::Klm)});
  Upd77c25State state;
  state.tr = 0x0005;
  state.dp = 0x83;
  state.ram[0x83] = 0x1111;
  state.ram[0xC3] = 0x0020;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().l, 0x0005);
  EXPECT_EQ(chip.state().k, 0x0020);
  EXPECT_EQ(chip.state().product, 0x0005u * 0x0020u * 2u);
}

TEST(Upd77c25Core, ReadingDrSetsRqmAndDrnfDoesNot) {
  const Upd77c25Program program = programOf({move(Src::Drnf, Dst::Tr), move(Src::Dr, Dst::Trb)});
  Upd77c25State state;
  state.dr = 0x4242;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().tr, 0x4242);
  EXPECT_FALSE(chip.requestingHost());
  chip.stepCycle();
  EXPECT_EQ(chip.state().trb, 0x4242);
  EXPECT_TRUE(chip.requestingHost());
}

TEST(Upd77c25Core, WritingSrKeepsRqmAndDrsAndBits6To2ReadZero) {
  const Upd77c25Program program = programOf({ld(0xFFFF, Dst::Sr), ld(0x0000, Dst::Sr)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_EQ(chip.state().sr, 0x6F83);
  Upd77c25State waiting = chip.state();
  waiting.sr = static_cast<std::uint16_t>(waiting.sr | kRqm | kDrs);
  chip.restore(waiting);
  chip.stepCycle();
  EXPECT_EQ(chip.state().sr, kRqm | kDrs);
}

TEST(Upd77c25Core, TheSerialOutputsAndNoneChangeNothing) {
  const Upd77c25Program program = programOf({
      move(Src::Tr, Dst::Sol),
      move(Src::Tr, Dst::Som),
      move(Src::Tr, Dst::None),
  });
  Upd77c25State state;
  state.tr = 0x5A5A;
  Upd77c25 chip(program, state);
  run(chip, 3);
  Upd77c25State expected = state;
  expected.pc = 3;
  EXPECT_EQ(chip.state(), expected);
}

// ---------------------------------------------------------------------------------------
// The order inside an ALU word

TEST(Upd77c25Core, TheSourceIsReadBeforeTheOperationWritesTheAccumulator) {
  const Upd77c25Program program = programOf({alu(Op::Inc, P::Ram, Acc::A, Src::AccA, Dst::Tr)});
  Upd77c25State state;
  state.accA = 0x0100;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().tr, 0x0100);
  EXPECT_EQ(chip.state().accA, 0x0101);
}

TEST(Upd77c25Core, AMoveToTheAccumulatorTheOperationWritesLandsLast) {
  const Upd77c25Program program = programOf({alu(Op::Inc, P::Ram, Acc::A, Src::Tr, Dst::AccA)});
  Upd77c25State state;
  state.accA = 0x0100;
  state.tr = 0x7777;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accA, 0x7777);
  EXPECT_EQ(chip.state().flagA, 0);  // the flags are the increment's, of 0101h
}

TEST(Upd77c25Core, ThePointersAdjustAfterTheMoveReadsAndWritesThroughThem) {
  // RAM[DP] and ROM[RP] are read at the pointers as they stood, then DP steps.
  Upd77c25Program program = programOf({
      withRpDecrement(withDpl(move(Src::Ro, Dst::Mem), kDpInc)),
      withDpl(move(Src::Tr, Dst::Dp), kDpInc),
  });
  program.data[0x010] = 0x0BAD;
  Upd77c25State state;
  state.dp = 0x20;
  state.rp = 0x010;
  state.tr = 0x0057;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().ram[0x20], 0x0BAD);
  EXPECT_EQ(chip.state().dp, 0x21);
  EXPECT_EQ(chip.state().rp, 0x00F);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0x58);  // DP written 57h, then stepped
}

// ---------------------------------------------------------------------------------------
// The pointer adjustments

TEST(Upd77c25Core, DplStepsTheLowNibbleAndWrapsInsideIt) {
  const Upd77c25Program program = programOf({
      withDpl(move(Src::Trb, Dst::None), kDpInc),
      withDpl(move(Src::Trb, Dst::None), kDpDec),
      withDpl(move(Src::Trb, Dst::None), kDpDec),
  });
  Upd77c25State state;
  state.dp = 0x3F;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0x30);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0x3F);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0x3E);
}

TEST(Upd77c25Core, DplClearZeroesTheLowNibble) {
  const Upd77c25Program program = programOf({withDpl(move(Src::Trb, Dst::None), kDpClr)});
  Upd77c25State state;
  state.dp = 0x5A;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0x50);
}

TEST(Upd77c25Core, DphXorsTheHighNibble) {
  const Upd77c25Program program = programOf({
      withDph(move(Src::Trb, Dst::None), 0x3),
      withDph(move(Src::Trb, Dst::None), 0xC),
  });
  Upd77c25State state;
  state.dp = 0x5A;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0x6A);
  chip.stepCycle();
  EXPECT_EQ(chip.state().dp, 0xAA);
}

TEST(Upd77c25Core, RpDecrementsAndWrapsInTenBits) {
  const Upd77c25Program program = programOf({
      withRpDecrement(move(Src::Trb, Dst::None)),
      withRpDecrement(move(Src::Trb, Dst::None)),
  });
  Upd77c25State state;
  state.rp = 0x001;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().rp, 0x000);
  chip.stepCycle();
  EXPECT_EQ(chip.state().rp, 0x3FF);
}

// ---------------------------------------------------------------------------------------
// The multiplier

TEST(Upd77c25Core, TheProductFollowsTheInstructionThatWritesKOrL) {
  const Upd77c25Program program = programOf({ld(0x8000, Dst::K), ld(0x8000, Dst::L)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_EQ(chip.state().product, 0u);  // L is still 0
  chip.stepCycle();
  // -8000h · -8000h · 2 wraps to 80000000h.
  EXPECT_EQ(chip.state().product, 0x80000000u);
}

TEST(Upd77c25Core, TheInstructionThatWritesKReadsTheProductBeforeIt) {
  const Upd77c25Program program = programOf({
      alu(Op::Or, P::ProductLow, Acc::A, Src::Tr, Dst::K),
      alu(Op::Or, P::ProductLow, Acc::B, Src::Trb, Dst::None),
  });
  Upd77c25State state;
  state.k = 0x0002;
  state.l = 0x0003;
  state.product = 0x0000000Cu;
  state.tr = 0x0005;
  Upd77c25 chip(program, state);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accA, 0x000C);  // 2 · 3 · 2, the product before the write
  EXPECT_EQ(chip.state().product, 0x0000001Eu);
  chip.stepCycle();
  EXPECT_EQ(chip.state().accB, 0x001E);  // 5 · 3 · 2
}

// ---------------------------------------------------------------------------------------
// The jumps

struct JumpRow {
  Br branch;
  bool takenAtAll;    // a state exists in which the jump is taken
  bool skippedAtAll;  // a state exists in which it is not
  Upd77c25State taken;
  Upd77c25State skipped;
};

Upd77c25State stateWith(std::uint8_t flagA, std::uint8_t flagB, std::uint8_t dp,
                        std::uint16_t sr) {
  Upd77c25State state;
  state.flagA = flagA;
  state.flagB = flagB;
  state.dp = dp;
  state.sr = sr;
  return state;
}

// A row for a jump on one of AccA's or AccB's flags, taken when the flag is `wanted`.
// The other accumulator holds the opposite value, so a jump that tests the wrong
// accumulator fails the row.
JumpRow flagRow(Br branch, Acc acc, std::uint8_t flag, bool wanted) {
  const auto holding = [acc, flag](bool value) {
    const auto mine = static_cast<std::uint8_t>(value ? flag : 0);
    const auto other = static_cast<std::uint8_t>(value ? 0 : flag);
    return acc == Acc::A ? stateWith(mine, other, 0, 0) : stateWith(other, mine, 0, 0);
  };
  return JumpRow{branch, true, true, holding(wanted), holding(!wanted)};
}

TEST(Upd77c25Core, EachJumpIsTakenExactlyWhenItsConditionHolds) {
  const Upd77c25State none = stateWith(0, 0, 0, 0);
  const std::vector<JumpRow> rows = {
      {Br::Jmp, true, false, none, none},
      flagRow(Br::Jnca, Acc::A, kC, false),    flagRow(Br::Jca, Acc::A, kC, true),
      flagRow(Br::Jncb, Acc::B, kC, false),    flagRow(Br::Jcb, Acc::B, kC, true),
      flagRow(Br::Jnza, Acc::A, kZ, false),    flagRow(Br::Jza, Acc::A, kZ, true),
      flagRow(Br::Jnzb, Acc::B, kZ, false),    flagRow(Br::Jzb, Acc::B, kZ, true),
      flagRow(Br::Jnova0, Acc::A, kOv0, false), flagRow(Br::Jova0, Acc::A, kOv0, true),
      flagRow(Br::Jnovb0, Acc::B, kOv0, false), flagRow(Br::Jovb0, Acc::B, kOv0, true),
      flagRow(Br::Jnova1, Acc::A, kOv1, false), flagRow(Br::Jova1, Acc::A, kOv1, true),
      flagRow(Br::Jnovb1, Acc::B, kOv1, false), flagRow(Br::Jovb1, Acc::B, kOv1, true),
      flagRow(Br::Jnsa0, Acc::A, kS0, false),  flagRow(Br::Jsa0, Acc::A, kS0, true),
      flagRow(Br::Jnsb0, Acc::B, kS0, false),  flagRow(Br::Jsb0, Acc::B, kS0, true),
      flagRow(Br::Jnsa1, Acc::A, kS1, false),  flagRow(Br::Jsa1, Acc::A, kS1, true),
      flagRow(Br::Jnsb1, Acc::B, kS1, false),  flagRow(Br::Jsb1, Acc::B, kS1, true),
      {Br::Jdpl0, true, true, stateWith(0, 0, 0xA0, 0), stateWith(0, 0, 0x0F, 0)},
      {Br::Jdpln0, true, true, stateWith(0, 0, 0x01, 0), stateWith(0, 0, 0xF0, 0)},
      {Br::Jdplf, true, true, stateWith(0, 0, 0x0F, 0), stateWith(0, 0, 0xF0, 0)},
      {Br::Jdplnf, true, true, stateWith(0, 0, 0xFE, 0), stateWith(0, 0, 0x0F, 0)},
      {Br::Jnsiak, true, false, none, none},
      {Br::Jsiak, false, true, none, none},
      {Br::Jnsoak, true, false, none, none},
      {Br::Jsoak, false, true, none, none},
      {Br::Jnrqm, true, true, none, stateWith(0, 0, 0, kRqm)},
      {Br::Jrqm, true, true, stateWith(0, 0, 0, kRqm), none},
  };
  constexpr std::uint16_t kTarget = 0x5A5;
  for (const JumpRow& row : rows) {
    const Upd77c25Program program = programOf({jp(row.branch, kTarget)});
    const int code = static_cast<int>(row.branch);
    if (row.takenAtAll) {
      Upd77c25 chip(program, row.taken);
      chip.stepCycle();
      EXPECT_EQ(chip.state().pc, kTarget) << "branch " << std::hex << code << " not taken";
    }
    if (row.skippedAtAll) {
      Upd77c25 chip(program, row.skipped);
      chip.stepCycle();
      EXPECT_EQ(chip.state().pc, 1) << "branch " << std::hex << code << " taken";
    }
  }
}

TEST(Upd77c25Core, ABranchValueTheChipDoesNotDefineIsNeverTaken) {
  const Upd77c25State everything = stateWith(kAllFlags, kAllFlags, 0x00, kRqm);
  for (std::uint16_t branch : std::vector<std::uint16_t>{0x000, 0x101, 0x141, 0x081, 0x0BF,
                                                         0x1FF}) {
    const Upd77c25Program program = programOf({jpRaw(branch, 0x123)});
    Upd77c25 chip(program, everything);
    chip.stepCycle();
    EXPECT_EQ(chip.state().pc, 1) << "branch " << std::hex << branch;
    EXPECT_EQ(chip.state().sp, 0) << "branch " << std::hex << branch;
  }
}

// ---------------------------------------------------------------------------------------
// The stack

TEST(Upd77c25Core, CallsNestFourDeepAndRtReturnsPastEachCall) {
  Upd77c25Program program = programOf({jp(Br::Call, 0x010)});
  program.code[0x010] = jp(Br::Call, 0x020);
  program.code[0x020] = jp(Br::Call, 0x030);
  program.code[0x030] = jp(Br::Call, 0x040);
  program.code[0x040] = withReturn(move(Src::Trb, Dst::None));
  program.code[0x031] = withReturn(move(Src::Trb, Dst::None));
  program.code[0x021] = withReturn(move(Src::Trb, Dst::None));
  program.code[0x011] = withReturn(move(Src::Trb, Dst::None));
  Upd77c25 chip(program);
  run(chip, 4);
  EXPECT_EQ(chip.state().pc, 0x040);
  EXPECT_EQ(chip.state().sp, 0);
  for (std::uint16_t expected : std::vector<std::uint16_t>{0x031, 0x021, 0x011, 0x001}) {
    chip.stepCycle();
    EXPECT_EQ(chip.state().pc, expected);
  }
}

TEST(Upd77c25Core, AFifthCallWritesOverTheFirstEntry) {
  Upd77c25Program program = programOf({jp(Br::Call, 0x010)});
  program.code[0x010] = jp(Br::Call, 0x020);
  program.code[0x020] = jp(Br::Call, 0x030);
  program.code[0x030] = jp(Br::Call, 0x040);
  program.code[0x040] = jp(Br::Call, 0x050);
  for (std::uint16_t address : std::vector<std::uint16_t>{0x050, 0x041, 0x031, 0x021, 0x011}) {
    program.code[address] = withReturn(move(Src::Trb, Dst::None));
  }
  Upd77c25 chip(program);
  run(chip, 5);
  EXPECT_EQ(chip.state().pc, 0x050);
  EXPECT_EQ(chip.state().sp, 1);
  EXPECT_EQ(chip.state().stack[0], 0x041);
  for (std::uint16_t expected :
       std::vector<std::uint16_t>{0x041, 0x031, 0x021, 0x011, 0x041}) {
    chip.stepCycle();
    EXPECT_EQ(chip.state().pc, expected);
  }
}

// ---------------------------------------------------------------------------------------
// Reset

TEST(Upd77c25Core, ResetSetsPcFlagsSrAndRpAndKeepsTheRest) {
  const Upd77c25Program program = programOf({});
  Upd77c25State state;
  state.pc = 0x456;
  state.flagA = kAllFlags;
  state.flagB = kAllFlags;
  state.sr = 0xFFFF;
  state.rp = 0x012;
  state.dp = 0x34;
  state.sp = 2;
  state.stack[1] = 0x077;
  state.k = 0x1111;
  state.l = 0x2222;
  state.product = 0x01234568u;
  state.accA = 0x3333;
  state.accB = 0x4444;
  state.tr = 0x5555;
  state.trb = 0x6666;
  state.dr = 0x7777;
  state.ram[0xFF] = 0x8888;
  Upd77c25 chip(program, state);
  chip.reset();
  Upd77c25State expected = state;
  expected.pc = 0;
  expected.flagA = 0;
  expected.flagB = 0;
  expected.sr = 0;
  expected.rp = 0x3FF;
  EXPECT_EQ(chip.state(), expected);
}

// ---------------------------------------------------------------------------------------
// The host's ports

TEST(Upd77c25Core, TheStatusReadIsSrsHighByteAndChangesNothing) {
  const Upd77c25Program program = programOf({});
  Upd77c25State state;
  state.sr = static_cast<std::uint16_t>(kRqm | kDrc | 0x0081);
  Upd77c25 chip(program, state);
  EXPECT_EQ(chip.hostRead(true), 0x84);
  EXPECT_EQ(chip.state(), state);
}

TEST(Upd77c25Core, InEightBitModeOneByteCompletesTheAccess) {
  const Upd77c25Program program = programOf({
      ld(kDrc, Dst::Sr),
      ld(0x1234, Dst::Dr),
      move(Src::Dr, Dst::Tr),
  });
  Upd77c25 chip(program);
  run(chip, 2);
  EXPECT_TRUE(chip.requestingHost());
  EXPECT_EQ(chip.hostRead(false), 0x34);
  EXPECT_FALSE(chip.requestingHost());
  EXPECT_EQ(chip.state().sr & kDrs, 0);

  chip.stepCycle();
  EXPECT_TRUE(chip.requestingHost());
  chip.hostWrite(0xAB);
  EXPECT_FALSE(chip.requestingHost());
  EXPECT_EQ(chip.state().dr, 0x12AB);  // the high byte kept
}

TEST(Upd77c25Core, InSixteenBitModeTheHostReadsTheLowByteFirstAndTheSecondCompletes) {
  const Upd77c25Program program = programOf({ld(0xBEEF, Dst::Dr)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_EQ(chip.hostRead(true), 0x80);
  EXPECT_EQ(chip.hostRead(false), 0xEF);
  EXPECT_EQ(chip.hostRead(true), 0x90);  // RQM and DRS
  EXPECT_TRUE(chip.requestingHost());
  EXPECT_EQ(chip.hostRead(false), 0xBE);
  EXPECT_EQ(chip.hostRead(true), 0x00);
  EXPECT_FALSE(chip.requestingHost());
}

TEST(Upd77c25Core, InSixteenBitModeTheHostWritesTheLowByteFirstAndTheSecondCompletes) {
  const Upd77c25Program program = programOf({move(Src::Dr, Dst::Tr)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_TRUE(chip.requestingHost());
  chip.hostWrite(0x34);
  EXPECT_EQ(chip.state().sr, kRqm | kDrs);
  chip.hostWrite(0x12);
  EXPECT_EQ(chip.state().dr, 0x1234);
  EXPECT_EQ(chip.state().sr, 0);
}

TEST(Upd77c25Core, AHostAccessWithNoRequestMovesTheFragmentsAllTheSame) {
  const Upd77c25Program program = programOf({});
  Upd77c25State state;
  state.dr = 0x1234;
  Upd77c25 chip(program, state);
  EXPECT_EQ(chip.hostRead(false), 0x34);
  EXPECT_EQ(chip.state().sr, kDrs);
  EXPECT_EQ(chip.hostRead(false), 0x12);
  EXPECT_EQ(chip.state().sr, 0);
  chip.hostWrite(0xCD);
  chip.hostWrite(0xAB);
  EXPECT_EQ(chip.state().dr, 0xABCD);
  EXPECT_EQ(chip.state().sr, 0);
}

TEST(Upd77c25Core, TheProgramsDrAccessRestartsTheHostsFragmentsAtTheLowByte) {
  const Upd77c25Program program = programOf({ld(0x5566, Dst::Dr), ld(0x7788, Dst::Dr)});
  Upd77c25 chip(program);
  chip.stepCycle();
  EXPECT_EQ(chip.hostRead(false), 0x66);
  chip.stepCycle();
  EXPECT_EQ(chip.state().sr, kRqm);
  EXPECT_EQ(chip.hostRead(false), 0x88);
  EXPECT_EQ(chip.hostRead(false), 0x77);
  EXPECT_FALSE(chip.requestingHost());
}

TEST(Upd77c25Core, AProgramWaitingOnRqmRunsOnOnceTheHostHasReadDr) {
  const Upd77c25Program program = programOf({
      ld(0x00AA, Dst::Dr),
      jp(Br::Jrqm, 0x001),
      ld(0x0001, Dst::AccA),
  });
  Upd77c25 chip(program);
  run(chip, 5);
  EXPECT_EQ(chip.state().pc, 1);  // still waiting
  EXPECT_EQ(chip.hostRead(false), 0xAA);
  run(chip, 3);
  EXPECT_EQ(chip.state().pc, 1);  // one byte of two
  EXPECT_EQ(chip.hostRead(false), 0x00);
  run(chip, 2);
  EXPECT_EQ(chip.state().pc, 3);
  EXPECT_EQ(chip.state().accA, 0x0001);
}

// ---------------------------------------------------------------------------------------
// The state as a value

TEST(Upd77c25Core, AStateRestoredBetweenInstructionsRunsTheSameAgain) {
  const Upd77c25Program program = programOf({
      ld(0x0003, Dst::K),
      ld(0x0007, Dst::L),
      alu(Op::Add, P::ProductLow, Acc::A, Src::Trb, Dst::None),
      withDpl(alu(Op::Add, P::ProductLow, Acc::A, Src::AccA, Dst::Mem), kDpInc),
      jp(Br::Call, 0x000),
  });
  Upd77c25 chip(program);
  run(chip, 2);
  const Upd77c25State snapshot = chip.state();
  run(chip, 9);
  const Upd77c25State first = chip.state();
  chip.restore(snapshot);
  run(chip, 9);
  EXPECT_EQ(chip.state(), first);
  EXPECT_NE(first, snapshot);
}

}  // namespace
