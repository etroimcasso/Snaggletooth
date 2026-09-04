// The lift, construct by construct, with the core as the oracle.
//
// Every case here places one instruction — or a few — in a flat memory, decodes
// it, lifts it, and runs the node through the interpreter beside the core running
// the same bytes from the same state. The two are then held equal on everything
// observable: the registers and flags after, every data access in order with its
// address, value and direction, the memory after, and the cycle count. The cases
// are chosen so each rule the effect layer states is the thing that would break:
// a wrap at a page or bank edge, a flag an operation leaves alone, the order of a
// read-modify-write's two writes, the stack pinned or not.
//
// The interpreter never sees the bytes. That is asserted here too, by reading
// its sources.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_interpret.h"
#include "snaggletooth/cpu/cpu65816.h"

#ifndef SNAGGLETOOTH_SOURCE_DIR
#define SNAGGLETOOTH_SOURCE_DIR ""
#endif

namespace snaggletooth::ir {
namespace {

using disasm::Cpu65816Mode;

// ---- the shared memory and the two buses --------------------------------------
struct AccessRecord {
  Address address;
  std::uint8_t value;
  bool write;
};

using Memory = std::map<std::uint32_t, std::uint8_t>;

// The core's side: a flat sparse memory, recording the data accesses — every
// write, and every read that is not an opcode or operand fetch.
struct CoreBus {
  Memory mem;
  std::vector<AccessRecord> log;

  std::uint8_t read(std::uint32_t address, CycleKind kind) {
    address &= 0xFFFFFFu;
    const auto it = mem.find(address);
    const std::uint8_t value = it == mem.end() ? std::uint8_t{0} : it->second;
    if (kind != CycleKind::OpcodeFetch && kind != CycleKind::OperandFetch) {
      log.push_back({address, value, false});
    }
    return value;
  }
  void write(std::uint32_t address, std::uint8_t value, CycleKind) {
    address &= 0xFFFFFFu;
    mem[address] = value;
    log.push_back({address, value, true});
  }
  void internal(std::uint32_t) {}
  void internal(std::uint32_t, CycleKind) {}
};

// The interpreter's side, over the same memory.
struct IrBus final : Bus {
  Memory mem;
  std::vector<AccessRecord> log;

  std::uint8_t read(Address address, Access) override {
    address &= 0xFFFFFFu;
    const auto it = mem.find(address);
    const std::uint8_t value = it == mem.end() ? std::uint8_t{0} : it->second;
    log.push_back({address, value, false});
    return value;
  }
  void write(Address address, std::uint8_t value, Access) override {
    address &= 0xFFFFFFu;
    mem[address] = value;
    log.push_back({address, value, true});
  }
};

Registers registersOf(const Cpu65816State& s) {
  Registers r;
  r.pc = s.pc;
  r.s = s.s;
  r.a = s.a;
  r.x = s.x;
  r.y = s.y;
  r.d = s.d;
  r.p = s.p;
  r.dbr = s.dbr;
  r.pbr = s.pbr;
  r.e = s.e;
  r.run = s.run == CpuRunState::Running   ? Run::Running
          : s.run == CpuRunState::Waiting ? Run::Waiting
                                          : Run::Stopped;
  return r;
}

// The mode a state runs under, every width known.
Cpu65816Mode modeOf(const Cpu65816State& s) {
  if (s.e) return Cpu65816Mode::reset();
  return Cpu65816Mode::native((s.p & kCpuFlagM) != 0, (s.p & kCpuFlagX) != 0);
}

// One scenario: the bytes at the program counter, the state to start from, the
// memory around it, and the mode to lift under (the state's own unless given).
struct Scenario {
  std::vector<std::uint8_t> bytes;
  Cpu65816State state;
  Memory memory;
  std::optional<Cpu65816Mode> liftMode;
  // Instructions to run: one by default; a block move runs its node once per byte.
  unsigned steps = 1;
  // A hardware request asserted before the step at this index (from 1), if any.
  std::optional<unsigned> nmiBefore;
};

struct Outcome {
  Registers core;
  Registers ir;
  std::uint32_t coreCycles = 0;
  std::uint32_t irCycles = 0;
  std::vector<AccessRecord> coreLog;
  std::vector<AccessRecord> irLog;
  Memory coreMem;
  Memory irMem;
  Node node;
};

Address pcOf(const Cpu65816State& s) {
  return (static_cast<Address>(s.pbr) << 16) | s.pc;
}

// Runs the scenario on both sides. The node is lifted from the instruction the
// core is about to run, decoded from the same memory, and lifted afresh at every
// step so a sequence of instructions runs as a program would.
Outcome run(const Scenario& sc) {
  Outcome out;
  CoreBus coreBus;
  IrBus irBus;
  coreBus.mem = sc.memory;
  for (std::size_t i = 0; i < sc.bytes.size(); ++i) {
    const Address at = (pcOf(sc.state) & 0xFF0000u) | ((sc.state.pc + i) & 0xFFFFu);
    coreBus.mem[at] = sc.bytes[i];
  }
  irBus.mem = coreBus.mem;

  Cpu65816 cpu(sc.state);
  Interpreter interpreter;
  interpreter.registers = registersOf(sc.state);

  Program program;
  program.nmi = interruptSequence(Interrupt::Nmi);

  for (unsigned step = 1; step <= sc.steps; ++step) {
    if (sc.nmiBefore && *sc.nmiBefore == step) {
      cpu.setNmiLine(true);
      out.coreCycles += cpu.stepInstruction(coreBus);
      out.irCycles += interpreter.interrupt(program.nmi, irBus);
      cpu.setNmiLine(false);
      continue;
    }
    // Decode what the interpreter is about to run from its own memory.
    const Registers& r = interpreter.registers;
    const Address address = (static_cast<Address>(r.pbr) << 16) | r.pc;
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t i = 0; i < 4; ++i) {
      const Address at = (address & 0xFF0000u) | ((r.pc + i) & 0xFFFFu);
      const auto it = irBus.mem.find(at);
      bytes.push_back(it == irBus.mem.end() ? std::uint8_t{0} : it->second);
    }
    Cpu65816Mode mode;
    if (sc.liftMode && step == 1) {
      mode = *sc.liftMode;
    } else {
      Cpu65816State s;
      s.e = r.e;
      s.p = r.p;
      mode = modeOf(s);
    }
    const std::optional<disasm::Instruction> decoded =
        disasm::decodeAt(bytes, address, address, mode);
    if (!decoded) {
      ADD_FAILURE() << "the instruction at step " << step << " did not decode";
      break;
    }
    out.node = liftInstruction(*decoded, mode);
    out.irCycles += interpreter.execute(out.node, irBus);
    out.coreCycles += cpu.stepInstruction(coreBus);
  }

  out.core = registersOf(cpu.state());
  out.ir = interpreter.registers;
  out.coreLog = std::move(coreBus.log);
  out.irLog = std::move(irBus.log);
  out.coreMem = std::move(coreBus.mem);
  out.irMem = std::move(irBus.mem);
  return out;
}

// Holds the two sides equal on everything observable.
void expectSame(const Outcome& out) {
  EXPECT_EQ(int{out.ir.pc}, int{out.core.pc}) << "pc";
  EXPECT_EQ(int{out.ir.s}, int{out.core.s}) << "s";
  EXPECT_EQ(int{out.ir.a}, int{out.core.a}) << "a";
  EXPECT_EQ(int{out.ir.x}, int{out.core.x}) << "x";
  EXPECT_EQ(int{out.ir.y}, int{out.core.y}) << "y";
  EXPECT_EQ(int{out.ir.d}, int{out.core.d}) << "d";
  EXPECT_EQ(int{out.ir.p}, int{out.core.p}) << "p";
  EXPECT_EQ(int{out.ir.dbr}, int{out.core.dbr}) << "dbr";
  EXPECT_EQ(int{out.ir.pbr}, int{out.core.pbr}) << "pbr";
  EXPECT_EQ(out.ir.e, out.core.e) << "e";
  EXPECT_EQ(static_cast<int>(out.ir.run), static_cast<int>(out.core.run)) << "run state";
  EXPECT_EQ(out.irCycles, out.coreCycles) << "cycles";
  ASSERT_EQ(out.irLog.size(), out.coreLog.size()) << "data accesses";
  for (std::size_t i = 0; i < out.irLog.size(); ++i) {
    EXPECT_EQ(out.irLog[i].address, out.coreLog[i].address) << "access " << i << " address";
    EXPECT_EQ(int{out.irLog[i].value}, int{out.coreLog[i].value}) << "access " << i << " value";
    EXPECT_EQ(out.irLog[i].write, out.coreLog[i].write) << "access " << i << " direction";
  }
  EXPECT_EQ(out.irMem, out.coreMem) << "memory after";
}

Outcome runSame(const Scenario& sc) {
  Outcome out = run(sc);
  expectSame(out);
  return out;
}

// A native state with the widths given, the stack in page one, the direct
// register zero, the program in bank $12 at $8000 and the data bank $34.
Cpu65816State native(bool a8, bool x8) {
  Cpu65816State s;
  s.pc = 0x8000;
  s.pbr = 0x12;
  s.dbr = 0x34;
  s.s = 0x01FF;
  s.p = static_cast<std::uint8_t>((a8 ? kCpuFlagM : 0) | (x8 ? kCpuFlagX : 0));
  s.e = false;
  return s;
}

Cpu65816State emulation() {
  Cpu65816State s = native(true, true);
  s.e = true;
  return s;
}

// The width an effect that reaches the named place runs at.
std::optional<Width> widthOfEffectOn(const Node& node, Op op, Place dst) {
  for (const Effect& e : node.effects) {
    if (e.op == op && e.dst.place == dst) return e.width;
  }
  return std::nullopt;
}

std::size_t count(const Node& node, Op op) {
  std::size_t n = 0;
  for (const Effect& e : node.effects) n += e.op == op ? 1u : 0u;
  return n;
}

// ---- register widths --------------------------------------------------------------
TEST(Lift, EightBitAccumulatorLoadKeepsTheHighByte) {
  Scenario sc;
  sc.state = native(true, false);
  sc.state.a = 0xBB00;
  sc.bytes = std::vector<std::uint8_t>{0xA9, 0x7F};  // LDA #$7F
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0xBB7F);
  EXPECT_EQ(widthOfEffectOn(out.node, Op::SetNZ, Place::A), Width::Byte);
}

TEST(Lift, EightBitIndexTransferClearsTheHighByte) {
  Scenario sc;
  sc.state = native(false, true);
  sc.state.a = 0x1234;
  sc.bytes = std::vector<std::uint8_t>{0xAA};  // TAX
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.x}, 0x34);
}

TEST(Lift, SixteenBitTransfersToDirectAndStackMoveTheWholeAccumulator) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 0x8001;
  sc.bytes = std::vector<std::uint8_t>{0x5B, 0x1B};  // TCD ; TCS
  sc.steps = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.d}, 0x8001);
  EXPECT_EQ(int{out.ir.s}, 0x8001);
}

// ---- REP / SEP and the flag-driven widths --------------------------------------------
TEST(Lift, RepWidensAndSepNarrowsThroughTheSameWrite) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.x = 0x00FF;
  sc.bytes = std::vector<std::uint8_t>{0xC2, 0x30, 0xA2, 0x34, 0x12, 0xE2, 0x10};  // REP #$30 ; LDX #$1234 ; SEP #$10
  sc.steps = 3;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.x}, 0x34) << "narrowing X clears its high byte";
  EXPECT_EQ(static_cast<int>(out.ir.p & (kCpuFlagM | kCpuFlagX)), int{kCpuFlagX});
}

TEST(Lift, RepMaskIsInertUnderEmulation) {
  Scenario sc;
  sc.state = emulation();
  sc.bytes = std::vector<std::uint8_t>{0xC2, 0x30};  // REP #$30
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.p & (kCpuFlagM | kCpuFlagX)), int{kCpuFlagM | kCpuFlagX});
}

// ---- PLP / RTI / XCE ----------------------------------------------------------------------
TEST(Lift, PlpNarrowingTheIndexClearsTheHighBytes) {
  Scenario sc;
  sc.state = native(false, false);
  sc.state.x = 0x1234;
  sc.state.y = 0xABCD;
  sc.state.s = 0x01FE;
  sc.memory[0x0001FF] = 0x30;  // the status byte to pull: M and X set
  sc.bytes = std::vector<std::uint8_t>{0x28};           // PLP
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.x}, 0x34);
  EXPECT_EQ(int{out.ir.y}, 0xCD);
}

TEST(Lift, WidthAfterPlpIsTheLiveFlagsWhenTheTraceDoesNotKnowIt) {
  // The same node, lifted with both widths unknown, runs right under both
  // settings of the accumulator flag.
  for (const bool a8 : {true, false}) {
    Scenario sc;
    sc.state = native(a8, true);
    sc.state.a = 0x1122;
    sc.memory[0x340010] = 0x55;
    sc.memory[0x340011] = 0x66;
    sc.bytes = std::vector<std::uint8_t>{0xAD, 0x10, 0x00};  // LDA !$0010
    sc.liftMode = Cpu65816Mode::nativeUnknown();
    const Outcome out = runSame(sc);
    EXPECT_EQ(widthOfEffectOn(out.node, Op::SetNZ, Place::A), Width::ByM);
    EXPECT_EQ(int{out.ir.a}, a8 ? 0x1155 : 0x6655);
  }
}

TEST(Lift, RtiInNativeModePullsTheBankAndAppliesTheStatusLast) {
  Scenario sc;
  sc.state = native(false, false);
  sc.state.s = 0x01FB;
  sc.memory[0x0001FC] = 0x35;  // P: M, X, I
  sc.memory[0x0001FD] = 0x00;  // PC low
  sc.memory[0x0001FE] = 0x90;  // PC high
  sc.memory[0x0001FF] = 0x7E;  // PBR
  sc.state.x = 0x4321;
  sc.bytes = std::vector<std::uint8_t>{0x40};  // RTI
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pbr}, 0x7E);
  EXPECT_EQ(int{out.ir.pc}, 0x9000);
  EXPECT_EQ(int{out.ir.x}, 0x21);
}

TEST(Lift, RtiInEmulationModeTakesNoBank) {
  Scenario sc;
  sc.state = emulation();
  sc.state.s = 0x01FC;
  sc.memory[0x0001FD] = 0x04;
  sc.memory[0x0001FE] = 0x00;
  sc.memory[0x0001FF] = 0x90;
  sc.bytes = std::vector<std::uint8_t>{0x40};  // RTI
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pbr}, 0x12);
  EXPECT_EQ(out.irLog.size(), 3u);
}

TEST(Lift, XceEnteringEmulationForcesTheWidthsAndPinsTheStack) {
  Scenario sc;
  sc.state = native(false, false);
  sc.state.p |= kCpuFlagC;
  sc.state.s = 0x1FFF;
  sc.state.x = 0x1234;
  sc.bytes = std::vector<std::uint8_t>{0xFB};  // XCE
  const Outcome out = runSame(sc);
  EXPECT_TRUE(out.ir.e);
  EXPECT_EQ(int{out.ir.s}, 0x01FF);
  EXPECT_EQ(int{out.ir.x}, 0x34);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagC), 0);
}

// ---- the flags -------------------------------------------------------------------------
TEST(Lift, CompareMovesNZCAndLeavesV) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 0x10;
  sc.state.p |= kCpuFlagV;
  sc.bytes = std::vector<std::uint8_t>{0xC9, 0x20};  // CMP #$20
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagV), int{kCpuFlagV});
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagC), 0);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagN), int{kCpuFlagN});
}

TEST(Lift, BitImmediateMovesZAloneAndBitFromMemoryTakesNVFromTheOperand) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 0x01;
  sc.memory[0x340020] = 0xC0;
  sc.bytes = std::vector<std::uint8_t>{0x89, 0xC0, 0x2C, 0x20, 0x00};  // BIT #$C0 ; BIT !$0020
  sc.steps = 1;
  Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.p & (kCpuFlagN | kCpuFlagV)), 0);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagZ), int{kCpuFlagZ});
  sc.steps = 2;
  out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.p & (kCpuFlagN | kCpuFlagV)), int{kCpuFlagN | kCpuFlagV});
}

TEST(Lift, TsbMovesZOnlyAndWritesTheBitsSet) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 0x81;
  sc.state.p |= kCpuFlagN | kCpuFlagC;
  sc.memory[0x000010] = 0x01;
  sc.bytes = std::vector<std::uint8_t>{0x04, 0x10};  // TSB $10
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.irMem.at(0x000010)}, 0x81);
  EXPECT_EQ(static_cast<int>(out.ir.p & (kCpuFlagN | kCpuFlagC)), int{kCpuFlagN | kCpuFlagC});
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagZ), 0);
}

TEST(Lift, IncMovesNZOnlyAndXbaSetsThemFromTheNewLowByte) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 0x80FF;
  sc.state.p |= kCpuFlagC;
  sc.bytes = std::vector<std::uint8_t>{0x1A, 0xEB};  // INC A ; XBA
  sc.steps = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x0080);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagN), int{kCpuFlagN});
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagC), int{kCpuFlagC});
}

TEST(Lift, PlbAndPldSetNZOnWhatTheyLoaded) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.s = 0x01FC;
  sc.memory[0x0001FD] = 0x80;  // PLB pulls $80
  sc.memory[0x0001FE] = 0x00;  // PLD pulls $0000
  sc.memory[0x0001FF] = 0x00;
  sc.bytes = std::vector<std::uint8_t>{0xAB, 0x2B};  // PLB ; PLD
  sc.steps = 1;
  Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagN), int{kCpuFlagN});
  sc.steps = 2;
  out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.p & kCpuFlagZ), int{kCpuFlagZ});
}

// ---- decimal mode ----------------------------------------------------------------------
TEST(Lift, DecimalAddAndSubtractAtEightAndSixteenBits) {
  const std::vector<std::pair<std::uint16_t, std::uint16_t>> pairs = {
      {0x0009, 0x0001}, {0x0099, 0x0001}, {0x00FF, 0x00FF}, {0x1234, 0x8877}, {0x9999, 0x0001},
      {0x0000, 0x0001}, {0x00AB, 0x00CD}, {0x4000, 0xC000}};
  for (const bool a8 : {true, false}) {
    for (const auto& [a, m] : pairs) {
      for (const std::uint8_t opcode : {std::uint8_t{0x69}, std::uint8_t{0xE9}}) {
        for (const bool carry : {false, true}) {
          Scenario sc;
          sc.state = native(a8, true);
          sc.state.p |= kCpuFlagD | (carry ? kCpuFlagC : 0);
          sc.state.a = a;
          sc.bytes = std::vector<std::uint8_t>{opcode, static_cast<std::uint8_t>(m)};
          if (!a8) sc.bytes.push_back(static_cast<std::uint8_t>(m >> 8));
          runSame(sc);
        }
      }
    }
  }
}

// ---- the direct page -----------------------------------------------------------------
TEST(Lift, DirectPageWrapsWithinBankZeroAndCostsACycleWithALowByte) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.d = 0xFFF1;
  sc.memory[0x000000] = 0x42;  // $FFF1 + $0F wraps to $0000
  sc.bytes = std::vector<std::uint8_t>{0xA5, 0x0F};     // LDA $0F
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0x42);
  EXPECT_EQ(out.irCycles, 4u);
}

TEST(Lift, DirectPageStaysInThePageUnderEmulationWithAPageAlignedRegister) {
  Scenario sc;
  sc.state = emulation();
  sc.state.d = 0x1200;
  sc.state.x = 0x10;
  sc.memory[0x001205] = 0x99;  // $F5 + $10 stays in page $12
  sc.bytes = std::vector<std::uint8_t>{0xB5, 0xF5};     // LDA $F5,X
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0x99);
}

TEST(Lift, SixteenBitDirectOperandSecondByteStepsWithinBankZero) {
  Scenario sc;
  sc.state = native(false, true);
  sc.state.d = 0xFFFF;
  sc.memory[0x00FFFF] = 0x34;
  sc.memory[0x000000] = 0x12;
  sc.bytes = std::vector<std::uint8_t>{0xA5, 0x00};  // LDA $00 at D = $FFFF
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x1234);
}

// ---- direct-page pointers ----------------------------------------------------------
TEST(Lift, SixtyFiveOhTwoPointerWrapsInThePageUnderEmulationWithDZero) {
  Scenario sc;
  sc.state = emulation();
  sc.state.d = 0;
  sc.state.y = 1;
  sc.memory[0x0000FF] = 0x00;  // pointer low at $FF ...
  sc.memory[0x000000] = 0x80;  // ... high wraps to $00: pointer $8000
  sc.memory[0x000100] = 0xEE;  // where the pointer would read if it stepped out
  sc.memory[0x348001] = 0x77;
  sc.bytes = std::vector<std::uint8_t>{0xB1, 0xFF};  // LDA ($FF),Y
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0x77);
}

TEST(Lift, LongPointerStepsOutOfThePageUnderTheSameConditions) {
  Scenario sc;
  sc.state = emulation();
  sc.state.d = 0;
  sc.memory[0x0000FF] = 0x00;
  sc.memory[0x000100] = 0x80;
  sc.memory[0x000101] = 0x7E;  // pointer $7E:8000
  sc.memory[0x7E8000] = 0x66;
  sc.bytes = std::vector<std::uint8_t>{0xA7, 0xFF};  // LDA [$FF]
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0x66);
}

TEST(Lift, PointerStepsOutWhenTheDirectRegisterIsNotZero) {
  Scenario sc;
  sc.state = emulation();
  sc.state.d = 0x0100;  // low byte zero, register not zero
  sc.memory[0x0001FF] = 0x00;
  sc.memory[0x000200] = 0x90;  // pointer $9000, read past the page
  sc.memory[0x349000] = 0x55;
  sc.bytes = std::vector<std::uint8_t>{0xB2, 0xFF};  // LDA ($FF)
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0x55);
}

// ---- absolute and long addressing ------------------------------------------------
TEST(Lift, IndexedAbsoluteCrossesIntoTheNextBank) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.x = 0x02;
  sc.memory[0x350001] = 0xAB;  // $34:FFFF + 2
  sc.bytes = std::vector<std::uint8_t>{0xBD, 0xFF, 0xFF};  // LDA !$FFFF,X
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0xAB);
  EXPECT_EQ(out.irCycles, 5u) << "the page crossed under an eight-bit index";
}

TEST(Lift, IndexedAbsoluteReadCostsNoExtraCycleWithoutACrossing) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.x = 0x02;
  sc.bytes = std::vector<std::uint8_t>{0xBD, 0x00, 0x10};  // LDA !$1000,X
  const Outcome out = runSame(sc);
  EXPECT_EQ(out.irCycles, 4u);
}

TEST(Lift, IndexedAbsoluteUnderASixteenBitIndexAlwaysPaysTheCycle) {
  Scenario sc;
  sc.state = native(true, false);
  sc.state.x = 0x0002;
  sc.bytes = std::vector<std::uint8_t>{0xBD, 0x00, 0x10};
  const Outcome out = runSame(sc);
  EXPECT_EQ(out.irCycles, 5u);
}

TEST(Lift, LongIndexedNeverWrapsAtABank) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.x = 0x01;
  sc.memory[0x7F0000] = 0xCD;
  sc.bytes = std::vector<std::uint8_t>{0xBF, 0xFF, 0xFF, 0x7E};  // LDA $7E:FFFF,X
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0xCD);
}

TEST(Lift, LongOperandCarriesTheRegisterName) {
  Scenario sc;
  sc.state = native(true, true);
  sc.bytes = std::vector<std::uint8_t>{0x8F, 0x00, 0x21, 0x00};  // STA $00:2100
  const Outcome out = runSame(sc);
  EXPECT_EQ(out.node.registerName, "INIDISP");
  Scenario absolute;
  absolute.state = native(true, true);
  absolute.bytes = std::vector<std::uint8_t>{0x8D, 0x00, 0x21};  // STA !$2100: the bank is the runtime's
  EXPECT_TRUE(runSame(absolute).node.registerName.empty());
}

TEST(Lift, IndirectJumpPointerWrapsWithinBankZero) {
  Scenario sc;
  sc.state = native(true, true);
  sc.memory[0x00FFFF] = 0x00;
  sc.memory[0x000000] = 0x90;
  sc.bytes = std::vector<std::uint8_t>{0x6C, 0xFF, 0xFF};  // JMP (!$FFFF)
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x9000);
}

TEST(Lift, IndexedIndirectJumpPointerWrapsWithinTheProgramBank) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.x = 0x01;
  sc.memory[0x12FFFF] = 0x00;
  sc.memory[0x120000] = 0xA0;
  sc.bytes = std::vector<std::uint8_t>{0x7C, 0xFE, 0xFF};  // JMP (!$FFFE,X)
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0xA000);
  EXPECT_EQ(int{out.ir.pbr}, 0x12);
}

TEST(Lift, LongIndirectJumpTakesTheBankFromTheThirdByte) {
  Scenario sc;
  sc.state = native(true, true);
  sc.memory[0x001000] = 0x00;
  sc.memory[0x001001] = 0xB0;
  sc.memory[0x001002] = 0x7E;
  sc.bytes = std::vector<std::uint8_t>{0xDC, 0x00, 0x10};  // JML [!$1000]
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pbr}, 0x7E);
  EXPECT_EQ(int{out.ir.pc}, 0xB000);
}

// ---- the program counter --------------------------------------------------------------
TEST(Lift, BranchWrapsWithinTheProgramBank) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.pc = 0xFFFE;
  sc.bytes = std::vector<std::uint8_t>{0x80, 0x10};  // BRA +$10 from $0000
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x0010);
  EXPECT_EQ(int{out.ir.pbr}, 0x12);
}

TEST(Lift, TakenBranchCostsACycleAndAPageCrossingOneMoreUnderEmulation) {
  Scenario native8;
  native8.state = native(true, true);
  native8.state.p |= kCpuFlagZ;
  native8.state.pc = 0x80F0;
  native8.bytes = std::vector<std::uint8_t>{0xF0, 0x20};  // BEQ across the page
  EXPECT_EQ(runSame(native8).irCycles, 3u);

  Scenario emulated = native8;
  emulated.state = emulation();
  emulated.state.p |= kCpuFlagZ;
  emulated.state.pc = 0x80F0;
  EXPECT_EQ(runSame(emulated).irCycles, 4u);

  Scenario untaken = emulated;
  untaken.state.p = static_cast<std::uint8_t>(untaken.state.p & ~kCpuFlagZ);
  EXPECT_EQ(runSame(untaken).irCycles, 2u);
}

TEST(Lift, BrlAndPerAddASignedDisplacementWithinTheBank) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.pc = 0x0002;
  sc.bytes = std::vector<std::uint8_t>{0x62, 0xF0, 0xFF, 0x82, 0xF0, 0xFF};  // PER -$10 ; BRL -$10
  sc.steps = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.irMem.at(0x0001FE)}, 0xF5);  // PER pushed $FFF5
  EXPECT_EQ(int{out.irMem.at(0x0001FF)}, 0xFF);
  EXPECT_EQ(int{out.ir.pc}, 0xFFF8);
}

// ---- the stack ----------------------------------------------------------------------------
TEST(Lift, PushOfASixOhTwoRegisterWrapsInsidePageOneUnderEmulation) {
  Scenario sc;
  sc.state = emulation();
  sc.state.s = 0x0100;
  sc.state.a = 0x5A;
  sc.bytes = std::vector<std::uint8_t>{0x48};  // PHA
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.s}, 0x01FF);
  EXPECT_EQ(int{out.irMem.at(0x000100)}, 0x5A);
}

TEST(Lift, PushOfADirectRegisterLeavesThePageAndSettlesBack) {
  Scenario sc;
  sc.state = emulation();
  sc.state.s = 0x0100;
  sc.state.d = 0x1234;
  sc.bytes = std::vector<std::uint8_t>{0x0B};  // PHD
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.irMem.at(0x000100)}, 0x12);
  EXPECT_EQ(int{out.irMem.at(0x0000FF)}, 0x34) << "the low byte lands below the page";
  EXPECT_EQ(int{out.ir.s}, 0x01FE);
}

TEST(Lift, TcsUnderEmulationWritesTheLowByteOnly) {
  Scenario sc;
  sc.state = emulation();
  sc.state.a = 0x7E55;
  sc.bytes = std::vector<std::uint8_t>{0x1B};  // TCS
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.s}, 0x0155);
}

TEST(Lift, IndexedIndirectCallStaysPinnedAtThePageEdgeUnderEmulation) {
  Scenario sc;
  sc.state = emulation();
  sc.state.s = 0x0100;
  sc.state.x = 0;
  sc.memory[0x128100] = 0x00;
  sc.memory[0x128101] = 0x90;
  sc.bytes = std::vector<std::uint8_t>{0xFC, 0x00, 0x81};  // JSR (!$8100,X)
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.s}, 0x01FE);
  EXPECT_EQ(int{out.irMem.at(0x000100)}, 0x80);
  EXPECT_EQ(int{out.irMem.at(0x0001FF)}, 0x02);
  EXPECT_EQ(int{out.ir.pc}, 0x9000);
}

// ---- calls and returns ------------------------------------------------------------------
TEST(Lift, CallAndReturnPairThroughTheLastByteOfTheCall) {
  Scenario sc;
  sc.state = native(true, true);
  sc.memory[0x129000] = 0x60;  // RTS at the target
  sc.bytes = std::vector<std::uint8_t>{0x20, 0x00, 0x90};  // JSR !$9000
  sc.steps = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x8003);
  EXPECT_EQ(int{out.ir.s}, 0x01FF);
}

TEST(Lift, LongCallAndReturnCarryTheBank) {
  Scenario sc;
  sc.state = native(true, true);
  sc.memory[0x7E9000] = 0x6B;  // RTL at the target
  sc.bytes = std::vector<std::uint8_t>{0x22, 0x00, 0x90, 0x7E};  // JSL $7E:9000
  sc.steps = 2;
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pbr}, 0x12);
  EXPECT_EQ(int{out.ir.pc}, 0x8004);
  sc.steps = 1;
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.pbr}, 0x7E);
  EXPECT_EQ(int{out.irMem.at(0x0001FF)}, 0x12);
}

TEST(Lift, PushEffectiveThenReturnDispatchesWithNoSpecialCase) {
  Scenario sc;
  sc.state = native(false, false);  // sixteen bits everywhere; PEA pushes sixteen regardless
  sc.bytes = std::vector<std::uint8_t>{0xF4, 0xFF, 0x8F, 0x60};  // PEA $8FFF ; RTS
  sc.steps = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x9000);
}

// ---- BRK, COP and the hardware interrupts ---------------------------------------------
TEST(Lift, BrkInNativeModeSavesTheBankAndEntersBankZero) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.p |= kCpuFlagD;
  sc.memory[0x00FFE6] = 0x00;
  sc.memory[0x00FFE7] = 0xC0;
  sc.bytes = std::vector<std::uint8_t>{0x00, 0x00};  // BRK
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pbr}, 0);
  EXPECT_EQ(int{out.ir.pc}, 0xC000);
  EXPECT_EQ(static_cast<int>(out.ir.p & (kCpuFlagD | kCpuFlagI)), int{kCpuFlagI});
  EXPECT_EQ(int{out.irMem.at(0x0001FF)}, 0x12);
  EXPECT_EQ(out.irCycles, 8u);
}

TEST(Lift, CopInEmulationModeSavesThreeBytesThroughItsOwnVector) {
  Scenario sc;
  sc.state = emulation();
  sc.memory[0x00FFF4] = 0x00;
  sc.memory[0x00FFF5] = 0xD0;
  sc.bytes = std::vector<std::uint8_t>{0x02, 0x00};  // COP
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0xD000);
  EXPECT_EQ(int{out.ir.s}, 0x01FC);
  EXPECT_EQ(out.irCycles, 7u);
}

TEST(Lift, HardwareInterruptIsAnInputRunAsTheSameEntrySequence) {
  for (const bool e : {false, true}) {
    Scenario sc;
    sc.state = e ? emulation() : native(true, true);
    sc.state.p |= kCpuFlagX;  // under emulation this bit is the break flag, cleared on the way
    sc.memory[0x00FFEA] = 0x00;
    sc.memory[0x00FFEB] = 0xE0;
    sc.memory[0x00FFFA] = 0x00;
    sc.memory[0x00FFFB] = 0xE1;
    sc.bytes = std::vector<std::uint8_t>{0xEA, 0xEA};  // NOP ; then the request lands ; NOP at the handler
    sc.steps = 2;
    sc.nmiBefore = 2;
    const Outcome out = runSame(sc);
    EXPECT_EQ(int{out.ir.pc}, e ? 0xE100 : 0xE000);
    EXPECT_EQ(int{out.ir.pbr}, 0);
  }
}

// ---- WAI and STP --------------------------------------------------------------------------
TEST(Lift, WaitAndStopHaltTheInterpreterAsTheyHaltTheCore) {
  Scenario wait;
  wait.state = native(true, true);
  wait.bytes = std::vector<std::uint8_t>{0xCB};  // WAI
  EXPECT_EQ(static_cast<int>(runSame(wait).ir.run), static_cast<int>(Run::Waiting));
  Scenario stop;
  stop.state = native(true, true);
  stop.bytes = std::vector<std::uint8_t>{0xDB};  // STP
  EXPECT_EQ(static_cast<int>(runSame(stop).ir.run), static_cast<int>(Run::Stopped));
}

// ---- read-modify-write -------------------------------------------------------------------
TEST(Lift, EmulationModeReadModifyWriteWritesItsAddressTwice) {
  Scenario sc;
  sc.state = emulation();
  sc.memory[0x342100] = 0x0F;
  sc.bytes = std::vector<std::uint8_t>{0xEE, 0x00, 0x21};  // INC !$2100, in the data bank
  const Outcome out = runSame(sc);
  ASSERT_EQ(out.irLog.size(), 3u);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(int{out.irLog[1].value}, 0x0F) << "the unmodified byte first";
  EXPECT_TRUE(out.irLog[2].write);
  EXPECT_EQ(int{out.irLog[2].value}, 0x10);
}

TEST(Lift, SixteenBitReadModifyWriteWritesTheHighByteFirst) {
  Scenario sc;
  sc.state = native(false, true);
  sc.memory[0x340020] = 0xFF;
  sc.memory[0x340021] = 0x00;
  sc.bytes = std::vector<std::uint8_t>{0xEE, 0x20, 0x00};  // INC !$0020
  const Outcome out = runSame(sc);
  ASSERT_EQ(out.irLog.size(), 4u);
  EXPECT_EQ(out.irLog[2].address, 0x340021u);
  EXPECT_EQ(out.irLog[3].address, 0x340020u);
  EXPECT_EQ(count(out.node, Op::Store), 0u) << "no emulation write on a sixteen-bit node";
}

// ---- block moves --------------------------------------------------------------------------
TEST(Lift, BlockMoveCarriesOneBytePerRunAndReentersUntilTheCountRunsOut) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 2;  // three bytes
  sc.state.x = 0x10;
  sc.state.y = 0x20;
  sc.memory[0x7E0010] = 1;
  sc.memory[0x7E0011] = 2;
  sc.memory[0x7E0012] = 3;
  sc.bytes = std::vector<std::uint8_t>{0x54, 0x7F, 0x7E};  // MVN $7E,$7F
  sc.steps = 3;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x8003);
  EXPECT_EQ(int{out.ir.a}, 0xFFFF);
  EXPECT_EQ(int{out.ir.dbr}, 0x7F);
  EXPECT_EQ(int{out.irMem.at(0x7F0022)}, 3);
  EXPECT_EQ(out.irCycles, 21u);
}

TEST(Lift, BlockMoveDownwardStepsTheIndexesAtTheirWidth) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 1;
  sc.state.x = 0x00;
  sc.state.y = 0x00;
  sc.memory[0x7E0000] = 0xAA;
  sc.memory[0x7E00FF] = 0xBB;
  sc.bytes = std::vector<std::uint8_t>{0x44, 0x7F, 0x7E};  // MVP $7E,$7F
  sc.steps = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.irMem.at(0x7F0000)}, 0xAA);
  EXPECT_EQ(int{out.irMem.at(0x7F00FF)}, 0xBB);
  EXPECT_EQ(int{out.ir.x}, 0xFE);
}

TEST(Lift, InterruptBetweenBlockMoveBytesIsOnlyAnInterrupt) {
  Scenario sc;
  sc.state = native(true, true);
  sc.state.a = 1;
  sc.state.x = 0x10;
  sc.state.y = 0x20;
  sc.memory[0x7E0010] = 1;
  sc.memory[0x7E0011] = 2;
  sc.memory[0x00FFEA] = 0x00;
  sc.memory[0x00FFEB] = 0xE0;
  sc.memory[0x00E000] = 0x40;  // RTI at the handler
  sc.bytes = std::vector<std::uint8_t>{0x54, 0x7F, 0x7E};
  sc.steps = 4;  // byte one, the request, RTI, byte two
  sc.nmiBefore = 2;
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.irMem.at(0x7F0021)}, 2);
  EXPECT_EQ(int{out.ir.pc}, 0x8003);
}

// ---- reads are the runtime's to answer ------------------------------------------------
TEST(Lift, AReadsValueIsWhateverTheBusAnswers) {
  Scenario sc;
  sc.state = native(true, true);
  sc.bytes = std::vector<std::uint8_t>{0xAD, 0x10, 0x42};  // LDA !$4210: nothing at that address here
  const Outcome out = runSame(sc);
  EXPECT_EQ(static_cast<int>(out.ir.a & 0xFFu), 0);
  EXPECT_EQ(out.irLog.front().address, 0x344210u);
}

// ---- the model -----------------------------------------------------------------------------
TEST(Lift, AConflictLiftsAsTwoNodesAndTheProgramFindsEachByTheLiveWidths) {
  // One address reached under two widths that read its bytes two ways.
  const std::vector<std::uint8_t> image = {0xA9, 0x12, 0x34, 0x60};  // LDA #... ; RTS
  disasm::Request request;
  request.image = image;
  request.base = 0x008000;
  request.entries = {0x008000, 0x008000};
  request.entryContexts = {disasm::contextOf(Cpu65816Mode::native(true, true)),
                           disasm::contextOf(Cpu65816Mode::native(false, true))};
  const disasm::Listing listing = disasm::trace(disasm::cpu65816Backend(), request);
  ASSERT_FALSE(listing.warnings.empty());
  const Program program = lift65816(listing, image, 0x008000);
  std::size_t at8000 = 0;
  for (const Node& node : program.nodes) at8000 += node.instruction.address == 0x008000 ? 1u : 0u;
  EXPECT_EQ(at8000, 2u);
  const Node* eight = program.find(0x008000, false, true, true);
  const Node* sixteen = program.find(0x008000, false, false, true);
  ASSERT_NE(eight, nullptr);
  ASSERT_NE(sixteen, nullptr);
  EXPECT_EQ(int{eight->instruction.length}, 2);
  EXPECT_EQ(int{sixteen->instruction.length}, 3);
  EXPECT_EQ(program.find(0x008000, true, true, true), nullptr);
}

TEST(Lift, ANodeWithALiveFlagWidthMatchesEitherSetting) {
  const std::vector<std::uint8_t> image = {0xAD, 0x00, 0x21};  // LDA !$2100
  disasm::Request request;
  request.image = image;
  request.base = 0x008000;
  request.context = disasm::contextOf(Cpu65816Mode::nativeUnknown());
  const Program program = lift65816(disasm::trace(disasm::cpu65816Backend(), request));
  ASSERT_EQ(program.nodes.size(), 1u);
  EXPECT_NE(program.find(0x008000, false, true, false), nullptr);
  EXPECT_NE(program.find(0x008000, false, false, true), nullptr);
  EXPECT_EQ(program.find(0x008000, true, true, true), nullptr);
}

TEST(Lift, APatchedByteIsItsOwnNode) {
  const std::vector<std::uint8_t> prior = {0xA9, 0x01, 0x60};  // LDA #$01 ; RTS
  const std::vector<std::uint8_t> after = {0xA9, 0x02, 0x60};  // LDA #$02 ; RTS
  disasm::Request request;
  request.image = after;
  request.priorImage = prior;
  request.base = 0x008000;
  request.context = disasm::contextOf(Cpu65816Mode::native(true, true));
  const Program patched = lift65816(disasm::trace(disasm::cpu65816Backend(), request));
  request.image = prior;
  request.priorImage = {};
  const Program original = lift65816(disasm::trace(disasm::cpu65816Backend(), request));
  ASSERT_EQ(patched.nodes.size(), 2u);
  ASSERT_EQ(original.nodes.size(), 2u);
  EXPECT_TRUE(patched.nodes[0].patched);
  EXPECT_FALSE(original.nodes[0].patched);
  EXPECT_EQ(patched.nodes[0].instruction.operand, 2u);
  EXPECT_EQ(original.nodes[0].instruction.operand, 1u);
}

TEST(Lift, AnImmediateUnderAnUnknownWidthHasNoNode) {
  const std::vector<std::uint8_t> image = {0xEA, 0xA9, 0x01, 0x60};  // NOP ; LDA # ; RTS
  disasm::Request request;
  request.image = image;
  request.base = 0x008000;
  request.context = disasm::contextOf(Cpu65816Mode::nativeUnknown());
  const Program program = lift65816(disasm::trace(disasm::cpu65816Backend(), request));
  ASSERT_EQ(program.nodes.size(), 1u);
  EXPECT_EQ(program.nodes[0].instruction.mnemonic, "NOP");
}

TEST(Lift, EveryOpcodeLiftsUnderEveryMode) {
  // Every opcode under every setting of the flags produces a node whose first
  // effect steps the program counter and whose cost is the measured table's.
  const std::vector<Cpu65816Mode> modes = {
      Cpu65816Mode::reset(), Cpu65816Mode::native(true, true), Cpu65816Mode::native(true, false),
      Cpu65816Mode::native(false, true), Cpu65816Mode::native(false, false),
      Cpu65816Mode::nativeUnknown()};
  for (const Cpu65816Mode& mode : modes) {
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
      const std::vector<std::uint8_t> image = {static_cast<std::uint8_t>(opcode), 0x10, 0x20,
                                               0x30};
      const std::optional<disasm::Instruction> decoded =
          disasm::decodeAt(image, 0x008000, 0x008000, mode);
      if (!decoded) continue;  // an immediate under an unknown width
      const Node node = liftInstruction(*decoded, mode);
      ASSERT_FALSE(node.effects.empty()) << "opcode " << opcode;
      EXPECT_EQ(static_cast<int>(node.effects.front().op), static_cast<int>(Op::Set));
      EXPECT_EQ(static_cast<int>(node.effects.front().dst.place), static_cast<int>(Place::PC));
      const disasm::CycleCost measured =
          disasm::cpu65816CycleTable(mode.emulation, true, true)[opcode];
      EXPECT_EQ(int{node.cost.base[costIndex(true, true)]}, int{measured.base});
    }
  }
}

// ---- the interface exposes no bytes ------------------------------------------------------
std::string sourceText(const std::string& relative) {
  std::ifstream in(std::string(SNAGGLETOOTH_SOURCE_DIR) + "/" + relative);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// The code of a source file: every line with its trailing comment cut off.
std::string codeOnly(const std::string& text) {
  std::string out;
  std::stringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    out += line.substr(0, line.find("//")) + "\n";
  }
  return out;
}

bool namesIdentifier(const std::string& code, const std::string& name) {
  auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
  for (std::size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1)) {
    const bool before = at > 0 && isWord(code[at - 1]);
    const bool after = at + name.size() < code.size() && isWord(code[at + name.size()]);
    if (!before && !after) return true;
  }
  return false;
}

TEST(Lift, TheInterpretersTranslationUnitIncludesNoDecoderAndNoListing) {
  for (const char* file : {"tools/ir/ir.h", "tools/ir/ir_interpret.h", "tools/ir/ir_interpret.cpp"}) {
    const std::string code = codeOnly(sourceText(file));
    ASSERT_FALSE(code.empty()) << file;
    EXPECT_EQ(code.find("disasm"), std::string::npos) << file;
    EXPECT_EQ(code.find("cpu65816"), std::string::npos) << file;
    EXPECT_EQ(code.find("snaggletooth/cpu"), std::string::npos) << file;
    EXPECT_FALSE(namesIdentifier(code, "bytes")) << file << " names bytes";
    EXPECT_FALSE(namesIdentifier(code, "opcode")) << file << " names an opcode";
  }
}

}  // namespace
}  // namespace snaggletooth::ir
