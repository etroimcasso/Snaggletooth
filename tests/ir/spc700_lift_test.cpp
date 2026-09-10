// The SPC700 lift, construct by construct, with the core as the oracle.
//
// Every case here places one instruction — or a few — in a flat memory,
// decodes it, lifts it, and runs the node through the sound CPU's interpreter
// beside the core running the same bytes from the same state. The two are then
// held equal on everything observable: the registers and flags after, every
// data access in order with its address, value and direction, the memory
// after, and the cycle count. The cases are chosen so each rule the effect
// layer states is the thing that would break: a wrap inside the direct page,
// the read a store makes before it writes, the flag an operation leaves alone,
// the order of a word instruction's four accesses.
//
// The interpreter never sees the bytes. That is asserted here too, by reading
// its sources.

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "disasm/disasm.h"
#include "ir/ir.h"
#include "ir/ir_interpret.h"
#include "ir/spc700_lift.h"
#include "snaggletooth/apu/spc700.h"
#include "spc700_disasm.h"

#ifndef SNAGGLETOOTH_SOURCE_DIR
#define SNAGGLETOOTH_SOURCE_DIR ""
#endif

namespace snaggletooth::ir {
namespace {

// ---- the shared memory and the two buses --------------------------------------
struct AccessRecord {
  std::uint16_t address;
  std::uint8_t value;
  bool write;
};

using Memory = std::map<std::uint16_t, std::uint8_t>;

// The core's side: a flat memory recording every access. Which reads are the
// instruction's own fetches the run decides, a cycle at a time, from where the
// program counter stood and where it went.
struct CoreBus {
  Memory mem;
  std::vector<AccessRecord> log;

  std::uint8_t read(std::uint16_t address) {
    const auto it = mem.find(address);
    const std::uint8_t value = it == mem.end() ? std::uint8_t{0} : it->second;
    log.push_back({address, value, false});
    return value;
  }
  void write(std::uint16_t address, std::uint8_t value) {
    mem[address] = value;
    log.push_back({address, value, true});
  }
};

// The interpreter's side, over the same memory.
struct IrBus final : Bus {
  Memory mem;
  std::vector<AccessRecord> log;

  std::uint8_t read(Address address, Access) override {
    const auto at = static_cast<std::uint16_t>(address & 0xFFFFu);
    const auto it = mem.find(at);
    const std::uint8_t value = it == mem.end() ? std::uint8_t{0} : it->second;
    log.push_back({at, value, false});
    return value;
  }
  void write(Address address, std::uint8_t value, Access) override {
    const auto at = static_cast<std::uint16_t>(address & 0xFFFFu);
    mem[at] = value;
    log.push_back({at, value, true});
  }
};

Spc700Registers registersOf(const Spc700State& s) {
  Spc700Registers r;
  r.pc = s.pc;
  r.a = s.a;
  r.x = s.x;
  r.y = s.y;
  r.sp = s.sp;
  r.psw = s.psw;
  r.run = s.run == RunState::Running    ? Run::Running
          : s.run == RunState::Sleeping ? Run::Waiting
                                        : Run::Stopped;
  return r;
}

// The bytes at the program counter, as a whole vector.
std::vector<std::uint8_t> code(std::initializer_list<std::uint8_t> bytes) { return bytes; }

// One scenario: the bytes at the program counter, the state to start from, and
// the memory around it.
struct Scenario {
  std::vector<std::uint8_t> bytes;
  Spc700State state;
  Memory memory;
  unsigned steps = 1;
};

struct Outcome {
  Spc700Registers core;
  Spc700Registers ir;
  std::uint32_t coreCycles = 0;
  std::uint32_t irCycles = 0;
  std::vector<AccessRecord> coreLog;  // the data accesses, fetches left out
  std::vector<AccessRecord> irLog;
  Memory coreMem;
  Memory irMem;
  Node node;
};

// Runs the scenario on both sides: the core a cycle at a time, so a fetch is
// known by the counter it was read at; the node lifted afresh at every step
// from the interpreter's own memory.
Outcome run(const Scenario& sc) {
  Outcome out;
  CoreBus coreBus;
  IrBus irBus;
  coreBus.mem = sc.memory;
  for (std::size_t i = 0; i < sc.bytes.size(); ++i) {
    coreBus.mem[static_cast<std::uint16_t>(sc.state.pc + i)] = sc.bytes[i];
  }
  irBus.mem = coreBus.mem;

  Spc700 cpu(sc.state);
  Spc700Interpreter interpreter;
  interpreter.registers = registersOf(sc.state);

  for (unsigned step = 1; step <= sc.steps; ++step) {
    const Spc700Registers& r = interpreter.registers;
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t i = 0; i < 3; ++i) {
      const auto at = static_cast<std::uint16_t>(r.pc + i);
      const auto it = irBus.mem.find(at);
      bytes.push_back(it == irBus.mem.end() ? std::uint8_t{0} : it->second);
    }
    const std::optional<disasm::Instruction> decoded = disasm::decodeAt(bytes, r.pc, r.pc);
    if (!decoded) {
      ADD_FAILURE() << "the instruction at step " << step << " did not decode";
      break;
    }
    out.node = liftSpc700Instruction(*decoded);
    out.irCycles += interpreter.execute(out.node, irBus);

    // The core, a cycle at a time: a read at the counter that steps it is a fetch.
    if (cpu.state().run != RunState::Running) {
      out.coreCycles += cpu.stepInstruction(coreBus);
      continue;
    }
    const std::uint16_t start = cpu.state().pc;
    do {
      const std::uint16_t before = cpu.state().pc;
      const std::size_t narrated = coreBus.log.size();
      cpu.stepCycle(coreBus);
      ++out.coreCycles;
      // A fetch: a read at the counter that steps it, or of the instruction's
      // last byte as a jump moves the counter to its destination.
      const bool stepped = cpu.state().pc == static_cast<std::uint16_t>(before + 1u);
      const bool lastByte = cpu.atInstructionBoundary() &&
                            static_cast<std::uint16_t>(before - start) < out.node.instruction.length;
      if (coreBus.log.size() != narrated && !coreBus.log.back().write &&
          coreBus.log.back().address == before && (stepped || lastByte)) {
        coreBus.log.pop_back();
      }
    } while (!cpu.atInstructionBoundary());
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
  EXPECT_EQ(int{out.ir.a}, int{out.core.a}) << "a";
  EXPECT_EQ(int{out.ir.x}, int{out.core.x}) << "x";
  EXPECT_EQ(int{out.ir.y}, int{out.core.y}) << "y";
  EXPECT_EQ(int{out.ir.sp}, int{out.core.sp}) << "sp";
  EXPECT_EQ(int{out.ir.psw}, int{out.core.psw}) << "psw";
  EXPECT_EQ(static_cast<int>(out.ir.run), static_cast<int>(out.core.run)) << "run state";
  EXPECT_EQ(out.irCycles, out.coreCycles) << "cycles";
  ASSERT_EQ(out.irLog.size(), out.coreLog.size()) << "data accesses";
  for (std::size_t i = 0; i < out.irLog.size(); ++i) {
    EXPECT_EQ(int{out.irLog[i].address}, int{out.coreLog[i].address}) << "access " << i << " address";
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

// A state at $0500 with the stack at $EF, the direct page at zero.
Spc700State at0500() {
  Spc700State s;
  s.pc = 0x0500;
  s.sp = 0xEF;
  return s;
}

std::size_t count(const Node& node, Op op) {
  std::size_t n = 0;
  for (const Effect& e : node.effects) n += e.op == op ? 1u : 0u;
  return n;
}

// The reads in a log, as addresses in order.
std::vector<std::uint16_t> reads(const std::vector<AccessRecord>& log) {
  std::vector<std::uint16_t> out;
  for (const AccessRecord& a : log) {
    if (!a.write) out.push_back(a.address);
  }
  return out;
}

// ---- the instruction layer ----------------------------------------------------------
TEST(Spc700Lift, EveryOpcodeHasAMnemonicAndAFormThatNameItAlone) {
  std::set<std::pair<std::string, std::string>> seen;
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    const auto byte = static_cast<std::uint8_t>(opcode);
    const std::string mnemonic(disasm::spc700Mnemonic(byte));
    const std::string form(disasm::spc700Form(byte));
    EXPECT_FALSE(mnemonic.empty()) << "opcode " << opcode;
    EXPECT_TRUE(seen.insert({mnemonic, form}).second) << mnemonic << " " << form << " names two opcodes";
    const std::optional<std::uint8_t> back = disasm::spc700OpcodeOf(mnemonic, form);
    ASSERT_TRUE(back.has_value()) << mnemonic << " " << form;
    EXPECT_EQ(int{*back}, int{byte});
  }
  EXPECT_EQ(disasm::spc700Form(0xE7), "A,[dp+X]");
  EXPECT_EQ(disasm::spc700Form(0x63), "dp.3,rel");
  EXPECT_EQ(disasm::spc700Form(0xBA), "YA,dp");
  EXPECT_EQ(disasm::spc700Form(0x0A), "C,abs.bit");
  EXPECT_EQ(disasm::spc700Form(0x8F), "dp,#imm");
  EXPECT_EQ(disasm::spc700Form(0xFA), "dp,dp");
  EXPECT_EQ(disasm::spc700Form(0x4F), "upage");
  EXPECT_EQ(disasm::spc700Form(0x01), "0");
  EXPECT_EQ(disasm::spc700Form(0x6F), "");
  EXPECT_FALSE(disasm::spc700OpcodeOf("MOV", "A,dp+Y").has_value());
}

TEST(Spc700Lift, TheInstructionLayerCarriesTheOperandsAsTheDialectWritesThem) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0xAA, 0x34, 0x72});  // MOV1 C,!$1234.3
  Outcome out = runSame(sc);
  EXPECT_EQ(out.node.instruction.mnemonic, "MOV1");
  EXPECT_EQ(out.node.instruction.form, "C,abs.bit");
  EXPECT_EQ(out.node.instruction.operand, 0x1234u);
  EXPECT_EQ(int{out.node.instruction.operand2}, 3);
  EXPECT_EQ(int{out.node.instruction.length}, 3);
  EXPECT_EQ(out.node.instruction.addressing, Addressing::Implied);

  sc.bytes = code({0x8F, 0x5A, 0x40});  // MOV $40,#$5A
  out = runSame(sc);
  EXPECT_EQ(out.node.instruction.form, "dp,#imm");
  EXPECT_EQ(out.node.instruction.operand, 0x5Au);
  EXPECT_EQ(int{out.node.instruction.operand2}, 0x40);

  sc.bytes = code({0x2E, 0x10, 0x05});  // CBNE $10,$0508
  out = runSame(sc);
  EXPECT_EQ(out.node.instruction.form, "dp,rel");
  EXPECT_EQ(out.node.instruction.operand, 0x10u);
  ASSERT_TRUE(out.node.instruction.target.has_value());
  EXPECT_EQ(*out.node.instruction.target, 0x0508u);
  EXPECT_EQ(out.node.instruction.flow, Flow::Branch);
}

TEST(Spc700Lift, ARegisterOperandCarriesItsName) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0xE4, 0xFD});  // MOV A,$FD — T0OUT
  Outcome out = runSame(sc);
  EXPECT_EQ(out.node.registerName, "T0OUT");
  sc.bytes = code({0xE4, 0x40});
  out = runSame(sc);
  EXPECT_TRUE(out.node.registerName.empty());
}

TEST(Spc700Lift, TheCostIsTheMeasuredBaseInEverySlot) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0xCF});  // MUL YA
  const Outcome out = runSame(sc);
  for (const std::uint8_t c : out.node.cost.base) EXPECT_EQ(int{c}, 9);
}

// ---- the direct page --------------------------------------------------------------
TEST(Spc700Lift, TheDirectPageFollowsThePFlag) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x5A;
  sc.bytes = code({0xC4, 0x40});  // MOV $40,A
  Outcome out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0040), 0x5A);
  sc.state.psw = kFlagP;
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0140), 0x5A);
  EXPECT_EQ(count(out.node, Op::PageAddress), 1u);
}

TEST(Spc700Lift, AnIndexedDirectOperandWrapsInsideThePage) {
  Scenario sc;
  sc.state = at0500();
  sc.state.x = 0x10;
  sc.state.psw = kFlagP;
  sc.memory[0x0105] = 0x77;
  sc.bytes = code({0xF4, 0xF5});  // MOV A,$F5+X → $0105, not $0205
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x77);
}

TEST(Spc700Lift, AnIndexedIndirectPointerIsReadInsideThePage) {
  Scenario sc;
  sc.state = at0500();
  sc.state.x = 0x01;
  sc.memory[0x00FF] = 0x34;
  sc.memory[0x0000] = 0x12;  // the pointer's high byte wraps to the page's start
  sc.memory[0x1234] = 0x99;
  sc.bytes = code({0xE7, 0xFE});  // MOV A,[$FE+X]
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x99);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x00FF, 0x0000, 0x1234}));
}

TEST(Spc700Lift, AnIndirectIndexedAddressWrapsAtTheTopOfMemory) {
  Scenario sc;
  sc.state = at0500();
  sc.state.y = 0x02;
  sc.memory[0x0040] = 0xFF;
  sc.memory[0x0041] = 0xFF;
  sc.memory[0x0001] = 0x42;
  sc.bytes = code({0xF7, 0x40});  // MOV A,[$40]+Y → $FFFF + 2 = $0001
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x42);
}

// ---- the reads a store makes -----------------------------------------------------------
TEST(Spc700Lift, AStoreReadsItsDestinationBeforeWriting) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x11;
  sc.bytes = code({0xC5, 0x00, 0x20});  // MOV !$2000,A
  const Outcome out = runSame(sc);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(int{out.irLog[0].address}, 0x2000);
}

TEST(Spc700Lift, TheAutoIncrementingStoreReadsNothingAndStepsX) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x22;
  sc.state.x = 0x30;
  sc.bytes = code({0xAF});  // MOV (X)+,A
  const Outcome out = runSame(sc);
  // The byte after the opcode is read and thrown away; the destination is not.
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501}));
  EXPECT_EQ(int{out.ir.x}, 0x31);
  EXPECT_EQ(out.coreMem.at(0x0030), 0x22);
}

TEST(Spc700Lift, TheAutoIncrementingLoadStepsXAfterTheRead) {
  Scenario sc;
  sc.state = at0500();
  sc.state.x = 0x30;
  sc.memory[0x0030] = 0x66;
  sc.bytes = code({0xBF});  // MOV A,(X)+
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x66);
  EXPECT_EQ(int{out.ir.x}, 0x31);
}

TEST(Spc700Lift, TheTwoOperandMoveReadsItsSourceAndNeverItsDestination) {
  Scenario sc;
  sc.state = at0500();
  sc.memory[0x0010] = 0xAB;
  sc.bytes = code({0xFA, 0x10, 0x20});  // MOV $20,$10
  const Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0010}));
  EXPECT_EQ(out.coreMem.at(0x0020), 0xAB);
}

TEST(Spc700Lift, TheImmediateMoveToMemoryReadsItsDestinationFirst) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0x8F, 0x5A, 0x40});  // MOV $40,#$5A
  const Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0040}));
  EXPECT_EQ(out.coreMem.at(0x0040), 0x5A);
}

TEST(Spc700Lift, TheIndirectToIndirectFormReadsTheSourceThenTheTarget) {
  Scenario sc;
  sc.state = at0500();
  sc.state.x = 0x10;
  sc.state.y = 0x20;
  sc.memory[0x0010] = 0x0F;
  sc.memory[0x0020] = 0x01;
  sc.bytes = code({0x99});  // ADC (X),(Y)
  const Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501, 0x0020, 0x0010}));
  EXPECT_EQ(out.coreMem.at(0x0010), 0x10);
}

// ---- the flags ---------------------------------------------------------------------
TEST(Spc700Lift, AddSetsTheHalfCarryAndKnowsNoDecimalMode) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x09;
  sc.bytes = code({0x88, 0x08});  // ADC A,#$08
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x11);
  EXPECT_NE(out.ir.psw & kFlagH, 0);
  sc.state.a = 0x7F;
  sc.bytes = code({0x88, 0x01});
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagV, 0);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
}

TEST(Spc700Lift, SubtractBorrowsThroughTheCarry) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x10;
  sc.state.psw = 0;  // a borrow in
  sc.bytes = code({0xA8, 0x01});  // SBC A,#$01
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x0E);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
}

TEST(Spc700Lift, CompareMovesNZCAndLeavesVAndH) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x05;
  sc.state.psw = kFlagV | kFlagH;
  sc.bytes = code({0x68, 0x06});  // CMP A,#$06
  const Outcome out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
  EXPECT_EQ(out.ir.psw & kFlagC, 0);
  EXPECT_NE(out.ir.psw & kFlagV, 0);
  EXPECT_NE(out.ir.psw & kFlagH, 0);
}

TEST(Spc700Lift, LogicOnAMovesNZOnly) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0xF0;
  sc.state.psw = kFlagC | kFlagV;
  sc.bytes = code({0x28, 0x0F});  // AND A,#$0F
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
  EXPECT_NE(out.ir.psw & kFlagV, 0);
}

TEST(Spc700Lift, AMoveToTheStackPointerMovesNoFlag) {
  Scenario sc;
  sc.state = at0500();
  sc.state.x = 0x00;
  sc.state.psw = 0;
  sc.bytes = code({0xBD});  // MOV SP,X
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.sp}, 0);
  EXPECT_EQ(out.ir.psw & kFlagZ, 0);
  sc.bytes = code({0x9D});  // MOV X,SP sets N Z
  sc.state.sp = 0;
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);
}

TEST(Spc700Lift, ShiftsAndRotatesMoveTheBitOutIntoTheCarry) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x81;
  sc.state.psw = kFlagC;
  sc.bytes = code({0x3C});  // ROL A
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x03);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
  sc.bytes = code({0x5C});  // LSR A
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x40);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
  EXPECT_EQ(out.ir.psw & kFlagN, 0);
}

TEST(Spc700Lift, TheNibbleExchangeComposesFromShifts) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0xA5;
  sc.bytes = code({0x9F});  // XCN A
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x5A);
  EXPECT_EQ(count(out.node, Op::Shl), 1u);
  EXPECT_EQ(count(out.node, Op::Shr), 1u);
}

TEST(Spc700Lift, AReadModifyWriteReadsThenWritesTheSameAddress) {
  Scenario sc;
  sc.state = at0500();
  sc.memory[0x2000] = 0x7F;
  sc.bytes = code({0xAC, 0x00, 0x20});  // INC !$2000
  const Outcome out = runSame(sc);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(out.coreMem.at(0x2000), 0x80);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
}

TEST(Spc700Lift, TheByteAfterAOneByteOpcodeIsReadAndThrownAway) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x01;
  sc.state.x = 0x02;
  sc.bytes = code({0x7D, 0xEE});  // MOV A,X, then a byte the chip reads and discards
  const Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501}));
  EXPECT_EQ(int{out.ir.a}, 0x02);
}

TEST(Spc700Lift, AReadsValueIsWhateverTheBusAnswers) {
  Scenario sc;
  sc.state = at0500();
  sc.memory[0x0040] = 0xC3;
  sc.bytes = code({0xE4, 0x40});  // MOV A,$40
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0xC3);
  for (const Effect& e : out.node.effects) {
    if (e.op == Op::Load) {
      EXPECT_EQ(e.dst.place, Place::T1);
    }
  }
}

TEST(Spc700Lift, ASequenceRunsAsAProgramWould) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0xCD, 0x10,    // MOV X,#$10
                   0x8D, 0x03,    // MOV Y,#$03
                   0xE8, 0x40,    // MOV A,#$40
                   0xD4, 0x20,    // MOV $20+X,A
                   0xF4, 0x20});  // MOV A,$20+X
  sc.steps = 5;
  const Outcome out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0030), 0x40);
  EXPECT_EQ(int{out.ir.a}, 0x40);
  EXPECT_EQ(int{out.ir.pc}, 0x050A);
}

// ---- the word instructions ------------------------------------------------------------
TEST(Spc700Lift, TheWordStoreReadsOnlyItsLowByteBeforeWriting) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x34;
  sc.state.y = 0x12;
  sc.bytes = code({0xDA, 0x40});  // MOVW $40,YA
  const Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0040}));
  ASSERT_EQ(out.irLog.size(), 3u);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(int{out.irLog[1].address}, 0x0040);
  EXPECT_EQ(int{out.irLog[2].address}, 0x0041);
  EXPECT_EQ(out.coreMem.at(0x0040), 0x34);
  EXPECT_EQ(out.coreMem.at(0x0041), 0x12);
}

TEST(Spc700Lift, TheWordLoadStaysInsideThePageAndTakesZeroFromTheWholeWord) {
  Scenario sc;
  sc.state = at0500();
  sc.state.psw = kFlagP;
  sc.memory[0x01FF] = 0x00;
  sc.memory[0x0100] = 0x01;  // the high byte wraps to the page's start
  sc.bytes = code({0xBA, 0xFF});   // MOVW YA,$FF
  const Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x01FF, 0x0100}));
  EXPECT_EQ(int{out.ir.a}, 0x00);
  EXPECT_EQ(int{out.ir.y}, 0x01);
  EXPECT_EQ(out.ir.psw & kFlagZ, 0);  // A is zero; the word is not
}

TEST(Spc700Lift, TheWordIncrementWritesEachByteBackBeforeReadingTheNext) {
  Scenario sc;
  sc.state = at0500();
  sc.memory[0x0040] = 0xFF;
  sc.memory[0x0041] = 0x00;
  sc.bytes = code({0x3A, 0x40});  // INCW $40
  Outcome out = runSame(sc);
  ASSERT_EQ(out.irLog.size(), 4u);
  EXPECT_EQ(int{out.irLog[0].address}, 0x0040);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_EQ(int{out.irLog[1].address}, 0x0040);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(int{out.irLog[2].address}, 0x0041);
  EXPECT_FALSE(out.irLog[2].write);
  EXPECT_EQ(int{out.irLog[3].address}, 0x0041);
  EXPECT_TRUE(out.irLog[3].write);
  EXPECT_EQ(out.coreMem.at(0x0040), 0x00);
  EXPECT_EQ(out.coreMem.at(0x0041), 0x01);
  EXPECT_EQ(out.ir.psw & kFlagZ, 0);
  // The high byte moves only when the low one wrapped.
  sc.memory[0x0040] = 0x01;
  sc.memory[0x0041] = 0x7F;
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0041), 0x7F);
  // The flags are the whole word's.
  sc.memory[0x0040] = 0xFF;
  sc.memory[0x0041] = 0xFF;
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);
  sc.bytes = code({0x1A, 0x40});  // DECW $40, from zero
  sc.memory[0x0040] = 0x00;
  sc.memory[0x0041] = 0x00;
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0040), 0xFF);
  EXPECT_EQ(out.coreMem.at(0x0041), 0xFF);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
}

TEST(Spc700Lift, TheWordAddIgnoresTheCarryInAndTakesHAndVFromTheHighByte) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0xFF;
  sc.state.y = 0x0F;  // YA = $0FFF
  sc.state.psw = kFlagC;  // a carry in does not take
  sc.memory[0x0040] = 0x01;
  sc.memory[0x0041] = 0x00;
  sc.bytes = code({0x7A, 0x40});  // ADDW YA,$40
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.y}, 0x10);
  EXPECT_EQ(int{out.ir.a}, 0x00);
  EXPECT_NE(out.ir.psw & kFlagH, 0);  // $0F plus the low byte's carry crosses the nibble
  EXPECT_EQ(out.ir.psw & kFlagC, 0);
  sc.state.y = 0x7F;  // $7FFF + 1
  sc.state.psw = 0;
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagV, 0);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
  sc.state.y = 0xFF;  // $FFFF + 1
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);
  EXPECT_EQ(count(out.node, Op::Adc), 1u);
}

TEST(Spc700Lift, TheWordSubtractBorrowsNothingIn) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x05;
  sc.state.y = 0x00;
  sc.state.psw = 0;  // a borrow in does not take
  sc.memory[0x0040] = 0x03;
  sc.bytes = code({0x9A, 0x40});  // SUBW YA,$40
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x02);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
  sc.memory[0x0040] = 0x06;
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0xFF);
  EXPECT_EQ(int{out.ir.y}, 0xFF);
  EXPECT_EQ(out.ir.psw & kFlagC, 0);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
}

TEST(Spc700Lift, TheWordCompareMovesNZCAndLeavesVAndH) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x00;
  sc.state.y = 0x01;  // $0100
  sc.state.psw = kFlagV | kFlagH;
  sc.memory[0x0040] = 0x00;
  sc.memory[0x0041] = 0x02;  // $0200
  sc.bytes = code({0x5A, 0x40});   // CMPW YA,$40
  const Outcome out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
  EXPECT_EQ(out.ir.psw & kFlagC, 0);
  EXPECT_NE(out.ir.psw & kFlagV, 0);
  EXPECT_NE(out.ir.psw & kFlagH, 0);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_FALSE(out.irLog[1].write);
}

// ---- the bits --------------------------------------------------------------------------
// An absolute bit operand: thirteen bits of address, the bit index above them.
std::vector<std::uint8_t> absoluteBit(std::uint8_t opcode, std::uint16_t address, unsigned bit) {
  const auto word = static_cast<std::uint16_t>(address | (bit << 13));
  return {opcode, static_cast<std::uint8_t>(word & 0xFFu), static_cast<std::uint8_t>(word >> 8)};
}

TEST(Spc700Lift, TheTestAndSetPairReadsTwiceAndTakesItsFlagsFromTheDifference) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x0F;
  sc.memory[0x2000] = 0xF0;
  sc.bytes = code({0x0E, 0x00, 0x20});  // TSET1 !$2000
  Outcome out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x2000, 0x2000}));
  EXPECT_EQ(out.coreMem.at(0x2000), 0xFF);
  EXPECT_EQ(out.ir.psw & (kFlagN | kFlagZ), 0);  // A − m = $1F
  sc.state.a = 0x10;
  sc.memory[0x2000] = 0x10;
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);  // A − m = 0
  EXPECT_EQ(out.coreMem.at(0x2000), 0x10);
  sc.state.a = 0x80;
  sc.memory[0x2000] = 0x81;
  sc.bytes = code({0x4E, 0x00, 0x20});  // TCLR1 !$2000
  out = runSame(sc);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x2000, 0x2000}));
  EXPECT_EQ(out.coreMem.at(0x2000), 0x01);
  EXPECT_NE(out.ir.psw & kFlagN, 0);  // A − m = $FF
}

TEST(Spc700Lift, OneBitOfAByteIsSetClearedOrFlippedInPlaceAndMovesNoFlag) {
  Scenario sc;
  sc.state = at0500();
  sc.state.psw = kFlagN | kFlagZ | kFlagC;
  sc.memory[0x0040] = 0x00;
  sc.bytes = code({0xA2, 0x40});  // SET1 $40.5
  Outcome out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0040), 0x20);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});
  sc.memory[0x0040] = 0xFF;
  sc.bytes = code({0xB2, 0x40});  // CLR1 $40.5
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0040), 0xDF);
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});
  sc.memory[0x1000] = 0x00;
  sc.bytes = absoluteBit(0xEA, 0x1000, 7);  // NOT1 !$1000.7
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x1000), 0x80);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});
}

TEST(Spc700Lift, TheCarryCombinesWithOneBitOfAnAbsoluteByte) {
  struct Case {
    std::uint8_t opcode;
    unsigned bit;
    std::uint8_t carryIn;
    bool carryOut;
  };
  // The byte at $1000 has bit 2 set and bit 3 clear.
  const Case cases[] = {
      {0x4A, 2, 1, true},   // AND1 C,m.2: 1 and 1
      {0x4A, 3, 1, false},  // AND1 C,m.3: 1 and 0
      {0x6A, 3, 1, true},   // AND1 C,/m.3: 1 and not 0
      {0x0A, 3, 0, false},  // OR1 C,m.3: 0 or 0
      {0x2A, 3, 0, true},   // OR1 C,/m.3: 0 or not 0
      {0x8A, 2, 1, false},  // EOR1 C,m.2: 1 xor 1
      {0x8A, 3, 1, true},   // EOR1 C,m.3: 1 xor 0
      {0xAA, 2, 0, true},   // MOV1 C,m.2
      {0xAA, 3, 1, false},  // MOV1 C,m.3
  };
  for (const Case& c : cases) {
    Scenario sc;
    sc.state = at0500();
    sc.state.psw = static_cast<std::uint8_t>(kFlagN | kFlagZ | c.carryIn);
    sc.memory[0x1000] = 0x04;
    sc.bytes = absoluteBit(c.opcode, 0x1000, c.bit);
    const Outcome out = runSame(sc);
    EXPECT_EQ((out.ir.psw & kFlagC) != 0, c.carryOut) << "opcode " << int{c.opcode} << " bit " << c.bit;
    EXPECT_EQ(int{out.ir.psw & (kFlagN | kFlagZ)}, int{kFlagN | kFlagZ});
    EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x1000}));
    EXPECT_EQ(out.coreMem.at(0x1000), 0x04);
  }
}

TEST(Spc700Lift, TheCarryStoredIntoOneBitReadsTheByteOnce) {
  Scenario sc;
  sc.state = at0500();
  sc.state.psw = kFlagC;
  sc.memory[0x1000] = 0x00;
  sc.bytes = absoluteBit(0xCA, 0x1000, 2);  // MOV1 !$1000.2,C
  Outcome out = runSame(sc);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_FALSE(out.irLog[0].write);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(out.coreMem.at(0x1000), 0x04);
  sc.state.psw = 0;
  sc.memory[0x1000] = 0xFF;
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x1000), 0xFB);
}

// ---- multiply, divide and the decimal adjusts ---------------------------------------
TEST(Spc700Lift, MultiplyTakesItsFlagsFromTheHighByte) {
  Scenario sc;
  sc.state = at0500();
  sc.state.y = 0x10;
  sc.state.a = 0x10;
  sc.bytes = code({0xCF});  // MUL YA
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.y}, 0x01);
  EXPECT_EQ(int{out.ir.a}, 0x00);
  EXPECT_EQ(out.ir.psw & (kFlagN | kFlagZ), 0);  // A is zero; Y is not
  sc.state.y = 0x01;
  sc.state.a = 0x01;
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x01);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);  // Y is zero
  sc.state.y = 0xFF;
  sc.state.a = 0xFF;
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.y}, 0xFE);
  EXPECT_EQ(int{out.ir.a}, 0x01);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
  EXPECT_EQ(count(out.node, Op::Mul), 1u);
}

TEST(Spc700Lift, DivideLeavesTheQuotientInAAndTheRemainderInY) {
  Scenario sc;
  sc.state = at0500();
  sc.state.y = 0x00;
  sc.state.a = 0x64;  // 100
  sc.state.x = 0x07;
  sc.bytes = code({0x9E});  // DIV YA,X
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 14);
  EXPECT_EQ(int{out.ir.y}, 2);
  EXPECT_EQ(out.ir.psw & kFlagV, 0);
  EXPECT_EQ(count(out.node, Op::Div), 1u);
  // A quotient past a byte: the overflow flag, and whatever the chip leaves.
  sc.state.y = 0x10;
  sc.state.a = 0x00;
  sc.state.x = 0x01;
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagV, 0);
  // The half carry compares the low nibbles of X and Y as they stood, equal included.
  sc.state.y = 0x05;
  sc.state.x = 0x15;
  out = runSame(sc);
  EXPECT_NE(out.ir.psw & kFlagH, 0);
  sc.state.x = 0x17;
  out = runSame(sc);
  EXPECT_EQ(out.ir.psw & kFlagH, 0);
}

TEST(Spc700Lift, TheDecimalAdjustsFollowTheCarryAndTheHalfCarry) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x9A;  // past ninety-nine
  sc.state.psw = 0;
  sc.bytes = code({0xDF});  // DAA A
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x00);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
  EXPECT_NE(out.ir.psw & kFlagZ, 0);
  sc.state.a = 0x15;
  sc.state.psw = kFlagH;  // the half carry adds six
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x1B);
  EXPECT_EQ(out.ir.psw & kFlagC, 0);
  EXPECT_EQ(count(out.node, Op::Daa), 1u);
  sc.bytes = code({0xBE});  // DAS A
  sc.state.a = 0x15;
  sc.state.psw = kFlagC | kFlagH;  // nothing to adjust
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x15);
  sc.state.psw = kFlagC;  // no half carry: six comes off
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0x0F);
  sc.state.psw = kFlagH;  // no carry: sixty comes off
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.a}, 0xB5);
  EXPECT_NE(out.ir.psw & kFlagN, 0);
  EXPECT_EQ(count(out.node, Op::Das), 1u);
}

// ---- control flow -----------------------------------------------------------------------
TEST(Spc700Lift, EveryRelativeBranchCostsTwoMoreCyclesWhenTaken) {
  struct Case {
    std::uint8_t opcode;
    std::uint8_t flag;
    bool whenSet;
  };
  const Case cases[] = {{0xF0, kFlagZ, true}, {0xD0, kFlagZ, false}, {0xB0, kFlagC, true},
                        {0x90, kFlagC, false}, {0x70, kFlagV, true}, {0x50, kFlagV, false},
                        {0x30, kFlagN, true}, {0x10, kFlagN, false}};
  for (const Case& c : cases) {
    Scenario sc;
    sc.state = at0500();
    sc.bytes = code({c.opcode, 0xFE});  // back onto the opcode itself
    sc.state.psw = c.whenSet ? c.flag : std::uint8_t{0};
    const Outcome taken = runSame(sc);
    EXPECT_EQ(int{taken.ir.pc}, 0x0500) << "opcode " << int{c.opcode};
    sc.state.psw = c.whenSet ? std::uint8_t{0} : c.flag;
    const Outcome fallen = runSame(sc);
    EXPECT_EQ(int{fallen.ir.pc}, 0x0502) << "opcode " << int{c.opcode};
    EXPECT_EQ(taken.irCycles, fallen.irCycles + 2u) << "opcode " << int{c.opcode};
    EXPECT_EQ(count(taken.node, Op::Cycles), 1u);
  }
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0x2F, 0x10});  // BRA $0512: always taken, so its cost has one value
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x0512);
  EXPECT_EQ(count(out.node, Op::Cycles), 0u);
}

TEST(Spc700Lift, TheBitBranchesReadTheByteOnceAndPayOnlyWhenTaken) {
  Scenario sc;
  sc.state = at0500();
  sc.memory[0x0040] = 0x08;
  sc.bytes = code({0x63, 0x40, 0x10});  // BBS $40.3,$0513
  const Outcome set = runSame(sc);
  EXPECT_EQ(int{set.ir.pc}, 0x0513);
  EXPECT_EQ(reads(set.irLog), (std::vector<std::uint16_t>{0x0040}));
  sc.bytes = code({0x73, 0x40, 0x10});  // BBC $40.3,$0513
  const Outcome clear = runSame(sc);
  EXPECT_EQ(int{clear.ir.pc}, 0x0503);
  EXPECT_EQ(set.irCycles, clear.irCycles + 2u);
  sc.memory[0x0040] = 0x00;
  const Outcome clearTaken = runSame(sc);
  EXPECT_EQ(int{clearTaken.ir.pc}, 0x0513);
  EXPECT_EQ(clearTaken.irCycles, set.irCycles);
}

TEST(Spc700Lift, CompareAndBranchInBothFormsMovesNoFlag) {
  Scenario sc;
  sc.state = at0500();
  sc.state.a = 0x42;
  sc.state.psw = kFlagZ | kFlagC;
  sc.memory[0x0040] = 0x42;
  sc.bytes = code({0x2E, 0x40, 0x10});  // CBNE $40,$0513
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x0503);  // equal: not taken
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});
  const std::uint32_t fallen = out.irCycles;
  sc.memory[0x0040] = 0x43;
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x0513);
  EXPECT_EQ(out.irCycles, fallen + 2u);
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});
  sc.state.x = 0x10;
  sc.state.psw = kFlagP;
  sc.memory[0x0150] = 0x42;
  sc.bytes = code({0xDE, 0x40, 0x10});  // CBNE $40+X,$0513: $0150 under P
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x0503);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0150}));
  EXPECT_EQ(int{out.ir.psw}, int{kFlagP});
}

TEST(Spc700Lift, DecrementAndBranchWritesTheByteBackWhetherOrNotItBranches) {
  Scenario sc;
  sc.state = at0500();
  sc.state.psw = kFlagZ | kFlagN;
  sc.memory[0x0040] = 0x01;
  sc.bytes = code({0x6E, 0x40, 0x10});  // DBNZ $40,$0513
  Outcome out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0040), 0x00);
  EXPECT_EQ(int{out.ir.pc}, 0x0503);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_TRUE(out.irLog[1].write);
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});  // the zero settles in no flag
  const std::uint32_t fallen = out.irCycles;
  sc.memory[0x0040] = 0x02;
  out = runSame(sc);
  EXPECT_EQ(out.coreMem.at(0x0040), 0x01);
  EXPECT_EQ(int{out.ir.pc}, 0x0513);
  EXPECT_EQ(out.irCycles, fallen + 2u);
}

TEST(Spc700Lift, DecrementYAndBranchReadsTheDisplacementTwice) {
  Scenario sc;
  sc.state = at0500();
  sc.state.y = 0x01;
  sc.state.psw = kFlagZ | kFlagN;
  sc.bytes = code({0xFE, 0x10});  // DBNZ Y,$0512
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.y}, 0x00);
  EXPECT_EQ(int{out.ir.pc}, 0x0502);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501}));  // the displacement, again, as data
  EXPECT_EQ(int{out.ir.psw}, int{sc.state.psw});
  sc.state.y = 0x00;  // wraps, and branches
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.y}, 0xFF);
  EXPECT_EQ(int{out.ir.pc}, 0x0512);
}

TEST(Spc700Lift, TheIndexedJumpReadsItsPointerAcrossAPageBoundary) {
  Scenario sc;
  sc.state = at0500();
  sc.state.x = 0x01;
  sc.memory[0x10FF] = 0x34;
  sc.memory[0x1100] = 0x12;
  sc.bytes = code({0x1F, 0xFE, 0x10});  // JMP [!$10FE+X]
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x1234);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x10FF, 0x1100}));
}

TEST(Spc700Lift, ACallPushesTheReturnAddressHighByteFirst) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0x3F, 0x34, 0x12});  // CALL !$1234
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x1234);
  EXPECT_EQ(int{out.ir.sp}, 0xED);
  ASSERT_EQ(out.irLog.size(), 2u);
  EXPECT_TRUE(out.irLog[0].write);
  EXPECT_EQ(int{out.irLog[0].address}, 0x01EF);
  EXPECT_EQ(int{out.irLog[0].value}, 0x05);
  EXPECT_EQ(int{out.irLog[1].address}, 0x01EE);
  EXPECT_EQ(int{out.irLog[1].value}, 0x03);
}

TEST(Spc700Lift, ThePageCallLandsInTheTopPage) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0x4F, 0x12});  // PCALL $12
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0xFF12);
  EXPECT_EQ(out.coreMem.at(0x01EF), 0x05);
  EXPECT_EQ(out.coreMem.at(0x01EE), 0x02);
}

TEST(Spc700Lift, TheVectorCallReadsItsEntryFromTheTableBelowTheTopOfMemory) {
  Scenario sc;
  sc.state = at0500();
  sc.memory[0xFFDE] = 0x34;
  sc.memory[0xFFDF] = 0x12;
  sc.memory[0xFFC0] = 0x78;
  sc.memory[0xFFC1] = 0x56;
  sc.bytes = code({0x01});  // TCALL 0
  Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x1234);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501, 0xFFDE, 0xFFDF}));
  EXPECT_EQ(out.coreMem.at(0x01EF), 0x05);
  EXPECT_EQ(out.coreMem.at(0x01EE), 0x01);
  sc.bytes = code({0xF1});  // TCALL 15
  out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x5678);
}

TEST(Spc700Lift, BreakPushesTheCounterThenTheStatusAndTakesTheVector) {
  Scenario sc;
  sc.state = at0500();
  sc.state.psw = kFlagI | kFlagC;
  sc.memory[0xFFDE] = 0x34;
  sc.memory[0xFFDF] = 0x12;
  sc.bytes = code({0x0F});  // BRK
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x1234);
  EXPECT_EQ(int{out.ir.sp}, 0xEC);
  EXPECT_EQ(out.coreMem.at(0x01EF), 0x05);
  EXPECT_EQ(out.coreMem.at(0x01EE), 0x01);
  EXPECT_EQ(out.coreMem.at(0x01ED), kFlagI | kFlagC);  // the status as it stood
  EXPECT_NE(out.ir.psw & kFlagB, 0);
  EXPECT_EQ(out.ir.psw & kFlagI, 0);
  EXPECT_NE(out.ir.psw & kFlagC, 0);
}

TEST(Spc700Lift, ReturnPullsTheCounterLowByteFirst) {
  Scenario sc;
  sc.state = at0500();
  sc.state.sp = 0xED;
  sc.memory[0x01EE] = 0x34;
  sc.memory[0x01EF] = 0x12;
  sc.bytes = code({0x6F});  // RET
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x1234);
  EXPECT_EQ(int{out.ir.sp}, 0xEF);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501, 0x01EE, 0x01EF}));
}

TEST(Spc700Lift, ReturnFromInterruptPullsTheStatusBeforeTheCounter) {
  Scenario sc;
  sc.state = at0500();
  sc.state.sp = 0xEC;
  sc.state.psw = 0;
  sc.memory[0x01ED] = 0xFF;  // every bit, the break and the page included
  sc.memory[0x01EE] = 0x34;
  sc.memory[0x01EF] = 0x12;
  sc.bytes = code({0x7F});  // RET1
  const Outcome out = runSame(sc);
  EXPECT_EQ(int{out.ir.pc}, 0x1234);
  EXPECT_EQ(int{out.ir.psw}, 0xFF);
  EXPECT_EQ(int{out.ir.sp}, 0xEF);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501, 0x01ED, 0x01EE, 0x01EF}));
  EXPECT_EQ(count(out.node, Op::Pull), 2u);
}

TEST(Spc700Lift, PushesAndPopsStayInPageOneAndWrapTheStackPointer) {
  struct Case {
    std::uint8_t push;
    std::uint8_t pop;
    std::uint8_t pushed;
  };
  const Case cases[] = {{0x2D, 0xAE, 0xA1}, {0x4D, 0xCE, 0xB2}, {0x6D, 0xEE, 0xC3}, {0x0D, 0x8E, 0xD4}};
  for (const Case& c : cases) {
    Scenario sc;
    sc.state = at0500();
    sc.state.sp = 0x00;
    sc.state.a = 0xA1;
    sc.state.x = 0xB2;
    sc.state.y = 0xC3;
    sc.state.psw = 0xD4;
    sc.bytes = code({c.push});
    const Outcome pushed = runSame(sc);
    EXPECT_EQ(int{pushed.ir.sp}, 0xFF) << "push " << int{c.push};
    ASSERT_EQ(pushed.irLog.size(), 2u);  // the byte after the opcode, then the push
    EXPECT_TRUE(pushed.irLog[1].write);
    EXPECT_EQ(int{pushed.irLog[1].address}, 0x0100);
    EXPECT_EQ(int{pushed.irLog[1].value}, int{c.pushed});
    // The pop reads where the push wrote, and a register's pop moves no flag.
    Scenario back;
    back.state = at0500();
    back.state.sp = 0xFF;
    back.state.psw = kFlagN;
    back.memory[0x0100] = c.pop == 0x8E ? std::uint8_t{0x5A} : std::uint8_t{0x00};
    back.bytes = code({c.pop});
    const Outcome popped = runSame(back);
    EXPECT_EQ(int{popped.ir.sp}, 0x00) << "pop " << int{c.pop};
    EXPECT_EQ(reads(popped.irLog), (std::vector<std::uint16_t>{0x0501, 0x0100}));
    EXPECT_EQ(int{popped.ir.psw}, c.pop == 0x8E ? 0x5A : int{kFlagN}) << "pop " << int{c.pop};
  }
}

TEST(Spc700Lift, TheFlagInstructionsMoveTheirOwnBitAlone) {
  struct Case {
    std::uint8_t opcode;
    std::uint8_t before;
    std::uint8_t after;
  };
  const Case cases[] = {
      {0x60, 0xFF, 0xFF & ~kFlagC},             // CLRC
      {0x80, 0x00, kFlagC},                     // SETC
      {0xED, kFlagC | kFlagZ, kFlagZ},          // NOTC
      {0xED, 0x00, kFlagC},                     // NOTC
      {0x20, 0xFF, 0xFF & ~kFlagP},             // CLRP
      {0x40, 0x00, kFlagP},                     // SETP
      {0xE0, 0xFF, 0xFF & ~(kFlagV | kFlagH)},  // CLRV clears the half carry too
      {0xA0, 0x00, kFlagI},                     // EI
      {0xC0, 0xFF, 0xFF & ~kFlagI},             // DI
      {0x00, 0xA5, 0xA5},                       // NOP
  };
  for (const Case& c : cases) {
    Scenario sc;
    sc.state = at0500();
    sc.state.psw = c.before;
    sc.bytes = code({c.opcode, 0xEE});
    const Outcome out = runSame(sc);
    EXPECT_EQ(int{out.ir.psw}, int{c.after}) << "opcode " << int{c.opcode};
    EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501}));
  }
}

TEST(Spc700Lift, SleepAndStopHaltTheInterpreterAsTheyHaltTheCore) {
  Scenario sc;
  sc.state = at0500();
  sc.bytes = code({0xEF, 0xEE});  // SLEEP
  Outcome out = runSame(sc);
  EXPECT_EQ(out.ir.run, Run::Waiting);
  EXPECT_EQ(reads(out.irLog), (std::vector<std::uint16_t>{0x0501, 0x0501, 0x0501}));
  EXPECT_EQ(count(out.node, Op::Halt), 1u);
  sc.bytes = code({0xFF, 0xEE});  // STOP
  out = runSame(sc);
  EXPECT_EQ(out.ir.run, Run::Stopped);
  EXPECT_EQ(int{out.ir.pc}, 0x0501);
}

// ---- every opcode -----------------------------------------------------------------------
// One state, one operand pair, all 256 opcodes: each decodes, lifts, and runs to
// the same registers, accesses, memory and cycles as the core.
TEST(Spc700Lift, EveryOpcodeLiftsAndRunsBesideTheCore) {
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    SCOPED_TRACE("opcode " + std::to_string(opcode));
    Scenario sc;
    sc.state = at0500();
    sc.state.a = 0x5A;
    sc.state.x = 0x03;
    sc.state.y = 0x21;
    sc.state.psw = kFlagC;
    sc.bytes = code({static_cast<std::uint8_t>(opcode), 0x40, 0x06});
    sc.memory[0x0040] = 0x7E;
    sc.memory[0x0041] = 0x01;
    sc.memory[0x0640] = 0x33;
    const Outcome out = runSame(sc);
    EXPECT_FALSE(out.node.instruction.mnemonic.empty());
    ASSERT_FALSE(out.node.effects.empty());
    EXPECT_EQ(out.node.effects.front().op, Op::Set);
    EXPECT_EQ(out.node.effects.front().dst.place, Place::PC);
  }
}

// ---- the interpreter's sources ----------------------------------------------------
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

TEST(Spc700Lift, TheInterpretersTranslationUnitIncludesNoDecoderAndNoListing) {
  const std::string code = codeOnly(sourceText("tools/ir/ir_interpret_spc700.cpp"));
  ASSERT_FALSE(code.empty());
  EXPECT_EQ(code.find("disasm"), std::string::npos);
  EXPECT_EQ(code.find("spc700_"), std::string::npos);
  EXPECT_EQ(code.find("snaggletooth/apu"), std::string::npos);
  EXPECT_FALSE(namesIdentifier(code, "bytes")) << "names bytes";
  EXPECT_FALSE(namesIdentifier(code, "opcode")) << "names an opcode";
}

}  // namespace
}  // namespace snaggletooth::ir
