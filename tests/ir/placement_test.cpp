// Where a node places its cycles: every program fetch of the instruction's own
// bytes, and every cycle the chip spends with no access, in the order the chip
// spends them.
//
// Each case lifts one instruction (or an interrupt sequence), runs it through
// the interpreter with a clock beside the bus, and reads back one string — a
// program fetch is `P`, an idle `I`, a data read `r`, a data write `w` — in the
// order the effects reported them. The string is the whole instruction cycle by
// cycle, so a fetch that reads its bytes late, an index cycle a page cross adds,
// a read-modify-write's middle cycle and an interrupt's opening fetch each show
// where they fall.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_interpret.h"

namespace snaggletooth::ir {
namespace {

using disasm::Cpu65816Mode;

// One cycle: its kind, and for a data cycle its direction.
enum class Kind { Program, Idle, DataRead, DataWrite };
struct Entry {
  Kind kind;
  Address address = 0;
  friend bool operator==(const Entry&, const Entry&) = default;
};

// The bus the interpreter reads through: memory that answers zero where nothing
// was placed, recording every access as a cycle in the shared ordered log and in
// its own access list.
struct RecordBus final : Bus {
  std::map<std::uint32_t, std::uint8_t> mem;
  std::vector<Entry>* order = nullptr;
  std::vector<Entry> data;

  std::uint8_t read(Address address, Access) override {
    address &= 0xFFFFFFu;
    data.push_back({Kind::DataRead, address});
    if (order != nullptr) order->push_back({Kind::DataRead, address});
    const auto it = mem.find(address);
    return it == mem.end() ? std::uint8_t{0} : it->second;
  }
  void write(Address address, std::uint8_t value, Access) override {
    address &= 0xFFFFFFu;
    data.push_back({Kind::DataWrite, address});
    if (order != nullptr) order->push_back({Kind::DataWrite, address});
    mem[address] = value;
  }
};

// The clock beside the bus: a fetch is that many program cycles, an idle that
// many no-access cycles, each in order.
struct RecordClock final : Clock {
  std::vector<Entry>* order = nullptr;
  void fetch(unsigned cycles) override {
    for (unsigned i = 0; i < cycles; ++i) order->push_back({Kind::Program});
  }
  void idle(unsigned cycles) override {
    for (unsigned i = 0; i < cycles; ++i) order->push_back({Kind::Idle});
  }
};

// A native or emulation register state, the program in bank $12 at $8000, the
// data bank $34, the stack in page one.
Registers reg(bool e, bool m8, bool x8) {
  Registers r;
  r.pc = 0x8000;
  r.pbr = 0x12;
  r.dbr = 0x34;
  r.s = 0x01FF;
  r.e = e;
  r.p = static_cast<std::uint8_t>((m8 ? 0x20u : 0u) | (x8 ? 0x10u : 0u) | (e ? 0x30u : 0u));
  return r;
}

std::string trace(const std::vector<Entry>& es) {
  std::string s;
  for (const Entry& e : es) {
    s += e.kind == Kind::Program    ? 'P'
         : e.kind == Kind::Idle     ? 'I'
         : e.kind == Kind::DataRead ? 'r'
                                    : 'w';
  }
  return s;
}

struct Traced {
  std::vector<Entry> order;
  std::vector<Entry> data;
  Registers regs;
  std::uint32_t cycles = 0;
  Node node;
};

Traced runInstr(const std::vector<std::uint8_t>& bytes, Registers initial, Cpu65816Mode liftMode,
                bool withClock = true) {
  RecordBus bus;
  const Address pc = (static_cast<Address>(initial.pbr) << 16) | initial.pc;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bus.mem[(pc & 0xFF0000u) | ((initial.pc + i) & 0xFFFFu)] = bytes[i];
  }
  std::vector<std::uint8_t> window;
  for (std::uint32_t i = 0; i < 4; ++i) {
    const auto it = bus.mem.find((pc & 0xFF0000u) | ((initial.pc + i) & 0xFFFFu));
    window.push_back(it == bus.mem.end() ? std::uint8_t{0} : it->second);
  }
  Traced r;
  const std::optional<disasm::Instruction> decoded = disasm::decodeAt(window, pc, pc, liftMode);
  if (!decoded) {
    ADD_FAILURE() << "the instruction did not decode";
    return r;
  }
  r.node = liftInstruction(*decoded, liftMode);
  Interpreter interpreter;
  interpreter.registers = initial;
  RecordClock clock;
  if (withClock) {
    clock.order = &r.order;
    bus.order = &r.order;
    interpreter.clock = &clock;
  }
  r.cycles = interpreter.execute(r.node, bus);
  r.regs = interpreter.registers;
  r.data = std::move(bus.data);
  return r;
}

Traced runInterrupt(Interrupt which, Registers initial) {
  RecordBus bus;
  Traced r;
  Interpreter interpreter;
  interpreter.registers = initial;
  RecordClock clock;
  clock.order = &r.order;
  bus.order = &r.order;
  interpreter.clock = &clock;
  const std::vector<Effect> sequence = interruptSequence(which);
  r.cycles = interpreter.interrupt(sequence, bus);
  r.regs = interpreter.registers;
  r.data = std::move(bus.data);
  return r;
}

std::size_t countKind(const std::vector<Entry>& es, Kind k) {
  std::size_t n = 0;
  for (const Entry& e : es) n += e.kind == k ? 1u : 0u;
  return n;
}

// ---- a split fetch --------------------------------------------------------------

TEST(Placement, JslIsToldItsFetchesAndWritesInOrder) {
  // JSL $123456: the first three bytes fetched, the old program bank pushed, an
  // internal cycle, the target bank byte fetched, the return address pushed.
  const Traced r = runInstr({0x22, 0x56, 0x34, 0x12}, reg(false, true, true),
                         Cpu65816Mode::native(true, true));
  EXPECT_EQ(trace(r.order), "PPPwIPww");
  EXPECT_EQ(r.order.size(), std::size_t{r.cycles});  // the invariant
}

// ---- a clock is never read back -------------------------------------------------

TEST(Placement, ANullClockChangesNothing) {
  const std::vector<std::uint8_t> bytes = {0x22, 0x56, 0x34, 0x12};
  const Traced with = runInstr(bytes, reg(false, true, true), Cpu65816Mode::native(true, true), true);
  const Traced without =
      runInstr(bytes, reg(false, true, true), Cpu65816Mode::native(true, true), false);
  EXPECT_TRUE(without.order.empty());
  EXPECT_EQ(without.cycles, with.cycles);
  EXPECT_TRUE(without.regs == with.regs);
  EXPECT_EQ(without.data, with.data);
}

// ---- the index cycle, D-7's rows ------------------------------------------------

TEST(Placement, EightBitIndexReadPlacesTheIndexCycleOnlyOnAPageCross) {
  const Registers state = reg(false, true, true);  // eight-bit index
  const Traced noCross = runInstr({0xBD, 0x00, 0x20}, [&] { Registers s = state; s.x = 0x10; return s; }(),
                                  Cpu65816Mode::native(true, true));  // LDA $2000,X, no cross
  EXPECT_EQ(trace(noCross.order), "PPPr");
  const Traced crossed = runInstr({0xBD, 0xF0, 0x20}, [&] { Registers s = state; s.x = 0x20; return s; }(),
                                  Cpu65816Mode::native(true, true));  // LDA $20F0,X crosses the page
  EXPECT_EQ(trace(crossed.order), "PPPIr");
}

TEST(Placement, SixteenBitIndexReadAlwaysPlacesTheIndexCycle) {
  Registers state = reg(false, true, false);  // eight-bit accumulator, sixteen-bit index
  state.x = 0x10;
  const Traced r = runInstr({0xBD, 0x00, 0x20}, state, Cpu65816Mode::native(true, false));
  EXPECT_EQ(trace(r.order), "PPPIr");
}

TEST(Placement, AWriteAlwaysPlacesTheIndexCycle) {
  Registers state = reg(false, true, true);  // eight-bit, no cross
  state.x = 0x10;
  const Traced r = runInstr({0x9D, 0x00, 0x20}, state, Cpu65816Mode::native(true, true));  // STA $2000,X
  EXPECT_EQ(trace(r.order), "PPPIw");
}

TEST(Placement, TheIndexCycleUnderTheLiveFlagHoldsUnderBothWidths) {
  // Lifted with the index width unknown, the node places one index cycle whether
  // the flag makes it eight bits and the address crosses, or sixteen.
  Registers eight = reg(false, true, true);
  eight.x = 0x20;  // $20F0 + $20 crosses the page
  const Traced asEight =
      runInstr({0xBD, 0xF0, 0x20}, eight, Cpu65816Mode::nativeUnknown());
  EXPECT_EQ(countKind(asEight.order, Kind::Idle), 1u);

  Registers sixteen = reg(false, true, false);
  sixteen.x = 0x20;
  const Traced asSixteen = runInstr({0xBD, 0xF0, 0x20}, sixteen, Cpu65816Mode::nativeUnknown());
  EXPECT_EQ(countKind(asSixteen.order, Kind::Idle), 1u);
}

// ---- a read-modify-write's middle cycle, D-8 ------------------------------------

TEST(Placement, AReadModifyWriteSpendsAnIdleMiddleCycleUnderNative) {
  Registers state = reg(false, true, true);  // native, eight-bit
  const Traced r = runInstr({0x0E, 0x00, 0x20}, state, Cpu65816Mode::native(true, true));  // ASL $2000
  EXPECT_EQ(trace(r.order), "PPPrIw");
}

TEST(Placement, AReadModifyWriteSpendsADummyWriteUnderEmulation) {
  Registers state = reg(true, true, true);  // emulation
  const Traced r = runInstr({0x0E, 0x00, 0x20}, state, Cpu65816Mode::reset());
  EXPECT_EQ(trace(r.order), "PPPrww");
}

TEST(Placement, ASixteenBitReadModifyWriteSpendsAnUnconditionalIdle) {
  Registers state = reg(false, false, false);  // native, sixteen-bit accumulator
  const Traced r = runInstr({0x0E, 0x00, 0x20}, state, Cpu65816Mode::native(false, false));
  EXPECT_EQ(trace(r.order), "PPPrrIww");
}

// ---- WDM ------------------------------------------------------------------------

TEST(Placement, WdmReadsOneByteAndSpendsAnInternalCycle) {
  const Traced r = runInstr({0x42, 0x00}, reg(false, true, true), Cpu65816Mode::native(true, true));
  EXPECT_EQ(trace(r.order), "PI");
  EXPECT_EQ(r.cycles, 2u);
}

// ---- the hardware interrupt sequences, finding 4 --------------------------------

TEST(Placement, TheInterruptSequenceOpensWithOneFetchAndNoIdle) {
  for (const Interrupt which : {Interrupt::Nmi, Interrupt::Irq}) {
    const Traced native = runInterrupt(which, reg(false, true, true));
    EXPECT_EQ(trace(native.order), "Prwwwwrr");
    EXPECT_EQ(native.order.front().kind, Kind::Program);
    EXPECT_EQ(countKind(native.order, Kind::Program), 1u);
    EXPECT_EQ(countKind(native.order, Kind::Idle), 0u);
    EXPECT_EQ(native.order.size(), std::size_t{native.cycles});  // the invariant

    const Traced emulation = runInterrupt(which, reg(true, true, true));
    EXPECT_EQ(trace(emulation.order), "Prwwwrr");
    EXPECT_EQ(countKind(emulation.order, Kind::Idle), 0u);
    EXPECT_EQ(emulation.order.size(), std::size_t{emulation.cycles});
  }
}

// ---- a node's fetch is its first effect -----------------------------------------

TEST(Placement, AFetchIsTheFirstEffect) {
  const std::vector<std::vector<std::uint8_t>> programs = {
      {0xEA},                    // NOP
      {0xA9, 0x00},              // LDA #imm
      {0x22, 0x56, 0x34, 0x12},  // JSL
      {0x00, 0x00},              // BRK
  };
  for (const std::vector<std::uint8_t>& bytes : programs) {
    const Traced r = runInstr(bytes, reg(false, true, true), Cpu65816Mode::native(true, true));
    ASSERT_FALSE(r.node.effects.empty());
    EXPECT_EQ(r.node.effects.front().op, Op::Fetch);
  }
}

}  // namespace
}  // namespace snaggletooth::ir
