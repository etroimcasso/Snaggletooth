#include "ir/cpu65816_lift.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace snaggletooth::ir {
namespace {

using disasm::Cpu65816Addressing;
using disasm::Cpu65816Mode;

// ---- operands ---------------------------------------------------------------
Operand imm(std::uint32_t value) { return {.place = Place::Imm, .value = value}; }
Operand at(Place place) { return {.place = place, .value = 0}; }

Cond always() { return {}; }
Cond when(When condition) { return {.when = condition}; }
Cond flagSet(Place flag) { return {.when = When::FlagSet, .place = flag}; }
Cond flagClear(Place flag) { return {.when = When::FlagClear, .place = flag}; }

// The width a node's accumulator and index operations run at: a type where the
// trace settled it, a selection by the live flag where it did not.
Width widthM(const Cpu65816Mode& mode) {
  if (!mode.accumulatorKnown) return Width::ByM;
  return mode.accumulator8 ? Width::Byte : Width::Word;
}
Width widthX(const Cpu65816Mode& mode) {
  if (!mode.indexKnown) return Width::ByX;
  return mode.index8 ? Width::Byte : Width::Word;
}

// ---- the effect list under construction ----------------------------------------
struct Builder {
  std::vector<Effect> effects;

  Effect& emit(Op op) {
    effects.push_back(Effect{.op = op});
    return effects.back();
  }
  void set(Place dst, Operand a, Width width, Cond cond = always()) {
    Effect& e = emit(Op::Set);
    e.dst = at(dst);
    e.a = a;
    e.width = width;
    e.when = cond;
  }
  void setNZ(Place dst, Operand a, Width width) {
    Effect& e = emit(Op::SetNZ);
    e.dst = at(dst);
    e.a = a;
    e.width = width;
  }
  void alu(Op op, Place dst, Operand a, Operand b, Width width) {
    Effect& e = emit(op);
    e.dst = at(dst);
    e.a = a;
    e.b = b;
    e.width = width;
  }
  void unary(Op op, Place dst, Operand a, Width width) {
    Effect& e = emit(op);
    e.dst = at(dst);
    e.a = a;
    e.width = width;
  }
  void address(Op op, Place dst, Operand a, Place index = Place::None) {
    Effect& e = emit(op);
    e.dst = at(dst);
    e.a = a;
    e.b = at(index);
    e.width = Width::Long;
  }
  void load(Place dst, Place address, Width width, Step step, Access access = Access::Data) {
    Effect& e = emit(Op::Load);
    e.dst = at(dst);
    e.a = at(address);
    e.width = width;
    e.step = step;
    e.access = access;
  }
  void store(Place address, Operand value, Width width, Step step, Access access = Access::Data,
             Cond cond = always(), Op op = Op::Store) {
    Effect& e = emit(op);
    e.a = at(address);
    e.b = value;
    e.width = width;
    e.step = step;
    e.access = access;
    e.when = cond;
  }
  void push(Operand value, Width width, bool pinned, Cond cond = always()) {
    Effect& e = emit(Op::Push);
    e.a = value;
    e.width = width;
    e.pinned = pinned;
    e.when = cond;
  }
  void pull(Place dst, Width width, bool pinned, Cond cond = always()) {
    Effect& e = emit(Op::Pull);
    e.dst = at(dst);
    e.width = width;
    e.pinned = pinned;
    e.when = cond;
  }
  void settle() { emit(Op::SettleStack); }
  void cycles(std::uint32_t count, Cond cond) {
    Effect& e = emit(Op::Cycles);
    e.a = imm(count);
    e.when = cond;
  }
  void flag(Place flag, bool value) { set(flag, imm(value ? 1u : 0u), Width::Byte); }
};

// ---- the instruction layer ------------------------------------------------------
Addressing addressingOf(Cpu65816Addressing mode) {
  switch (mode) {
    case Cpu65816Addressing::Implied: return Addressing::Implied;
    case Cpu65816Addressing::Accumulator: return Addressing::Accumulator;
    case Cpu65816Addressing::ImmediateM: return Addressing::ImmediateM;
    case Cpu65816Addressing::ImmediateX: return Addressing::ImmediateX;
    case Cpu65816Addressing::ImmediateByte: return Addressing::ImmediateByte;
    case Cpu65816Addressing::Direct: return Addressing::Direct;
    case Cpu65816Addressing::DirectX: return Addressing::DirectX;
    case Cpu65816Addressing::DirectY: return Addressing::DirectY;
    case Cpu65816Addressing::DirectIndirect: return Addressing::DirectIndirect;
    case Cpu65816Addressing::DirectIndirectX: return Addressing::DirectIndirectX;
    case Cpu65816Addressing::DirectIndirectY: return Addressing::DirectIndirectY;
    case Cpu65816Addressing::DirectIndirectLong: return Addressing::DirectIndirectLong;
    case Cpu65816Addressing::DirectIndirectLongY: return Addressing::DirectIndirectLongY;
    case Cpu65816Addressing::StackRelative: return Addressing::StackRelative;
    case Cpu65816Addressing::StackRelativeY: return Addressing::StackRelativeY;
    case Cpu65816Addressing::Absolute: return Addressing::Absolute;
    case Cpu65816Addressing::AbsoluteX: return Addressing::AbsoluteX;
    case Cpu65816Addressing::AbsoluteY: return Addressing::AbsoluteY;
    case Cpu65816Addressing::AbsoluteLong: return Addressing::AbsoluteLong;
    case Cpu65816Addressing::AbsoluteLongX: return Addressing::AbsoluteLongX;
    case Cpu65816Addressing::AbsoluteIndirect: return Addressing::AbsoluteIndirect;
    case Cpu65816Addressing::AbsoluteIndirectLong: return Addressing::AbsoluteIndirectLong;
    case Cpu65816Addressing::AbsoluteIndexedIndirect: return Addressing::AbsoluteIndexedIndirect;
    case Cpu65816Addressing::Relative: return Addressing::Relative;
    case Cpu65816Addressing::RelativeLong: return Addressing::RelativeLong;
    case Cpu65816Addressing::BlockMove: return Addressing::BlockMove;
    case Cpu65816Addressing::PushAbsolute: return Addressing::PushAbsolute;
    case Cpu65816Addressing::PushRelative: return Addressing::PushRelative;
  }
  return Addressing::Implied;
}

Flow flowOf(disasm::Flow flow) {
  switch (flow) {
    case disasm::Flow::Continue: return Flow::Continue;
    case disasm::Flow::Branch: return Flow::Branch;
    case disasm::Flow::Jump: return Flow::Jump;
    case disasm::Flow::Call: return Flow::Call;
    case disasm::Flow::Return: return Flow::Return;
    case disasm::Flow::Halt: return Flow::Halt;
  }
  return Flow::Continue;
}

Mode irMode(const Cpu65816Mode& mode) {
  return {.emulation = mode.emulation,
          .accumulator8 = mode.accumulator8,
          .index8 = mode.index8,
          .accumulatorKnown = mode.accumulatorKnown,
          .indexKnown = mode.indexKnown};
}

// ---- the operation an instruction applies -------------------------------------
enum class Kind : std::uint8_t {
  ReadM,   // reads its operand at the accumulator width
  ReadX,   // reads its operand at the index width
  Write,   // stores a register
  Modify,  // reads, modifies and writes back
  Other,
};

Kind kindOf(std::string_view mnemonic) {
  static constexpr std::string_view kReadM[] = {"LDA", "ADC", "SBC", "AND",
                                                "EOR", "ORA", "CMP", "BIT"};
  static constexpr std::string_view kReadX[] = {"LDX", "LDY", "CPX", "CPY"};
  static constexpr std::string_view kWrite[] = {"STA", "STX", "STY", "STZ"};
  static constexpr std::string_view kModify[] = {"ASL", "LSR", "ROL", "ROR",
                                                 "INC", "DEC", "TSB", "TRB"};
  auto in = [&](const auto& list) {
    return std::find(std::begin(list), std::end(list), mnemonic) != std::end(list);
  };
  if (in(kReadM)) return Kind::ReadM;
  if (in(kReadX)) return Kind::ReadX;
  if (in(kWrite)) return Kind::Write;
  if (in(kModify)) return Kind::Modify;
  return Kind::Other;
}

bool isMemoryForm(Cpu65816Addressing mode) {
  switch (mode) {
    case Cpu65816Addressing::Direct:
    case Cpu65816Addressing::DirectX:
    case Cpu65816Addressing::DirectY:
    case Cpu65816Addressing::DirectIndirect:
    case Cpu65816Addressing::DirectIndirectX:
    case Cpu65816Addressing::DirectIndirectY:
    case Cpu65816Addressing::DirectIndirectLong:
    case Cpu65816Addressing::DirectIndirectLongY:
    case Cpu65816Addressing::StackRelative:
    case Cpu65816Addressing::StackRelativeY:
    case Cpu65816Addressing::Absolute:
    case Cpu65816Addressing::AbsoluteX:
    case Cpu65816Addressing::AbsoluteY:
    case Cpu65816Addressing::AbsoluteLong:
    case Cpu65816Addressing::AbsoluteLongX:
      return true;
    default:
      return false;
  }
}

bool isDirectForm(Cpu65816Addressing mode) {
  switch (mode) {
    case Cpu65816Addressing::Direct:
    case Cpu65816Addressing::DirectX:
    case Cpu65816Addressing::DirectY:
    case Cpu65816Addressing::DirectIndirect:
    case Cpu65816Addressing::DirectIndirectX:
    case Cpu65816Addressing::DirectIndirectY:
    case Cpu65816Addressing::DirectIndirectLong:
    case Cpu65816Addressing::DirectIndirectLongY:
      return true;
    default:
      return false;
  }
}

// The effective address of a memory form, left in T0, with the rule its data
// bytes step by. Every wrap the chip has is here: the direct page's within bank
// zero or within the page, a 6502-era pointer's within the page under the same
// conditions and a zero direct register, a bank-relative index carrying into the
// next bank, a long index likewise, and the stack's within bank zero. A direct
// form costs a cycle more with a low byte in the direct register; an indexed read
// with eight-bit index registers costs one more when the addition carries.
Step effectiveAddress(Builder& b, Cpu65816Addressing mode, std::uint32_t operand, bool read) {
  using M = Cpu65816Addressing;
  if (isDirectForm(mode)) b.cycles(1, when(When::DirectLowByte));
  switch (mode) {
    case M::Direct:
      b.address(Op::DirectAddress, Place::T0, imm(operand));
      return Step::Direct;
    case M::DirectX:
      b.address(Op::DirectAddress, Place::T0, imm(operand), Place::X);
      return Step::Direct;
    case M::DirectY:
      b.address(Op::DirectAddress, Place::T0, imm(operand), Place::Y);
      return Step::Direct;
    case M::DirectIndirect:
      b.address(Op::DirectAddress, Place::T1, imm(operand));
      b.load(Place::T2, Place::T1, Width::Word, Step::DirectPointer);
      b.address(Op::BankAddress, Place::T0, at(Place::T2));
      return Step::Flat;
    case M::DirectIndirectX:
      b.address(Op::DirectAddress, Place::T1, imm(operand), Place::X);
      b.load(Place::T2, Place::T1, Width::Word, Step::DirectPointer);
      b.address(Op::BankAddress, Place::T0, at(Place::T2));
      return Step::Flat;
    case M::DirectIndirectY:
      b.address(Op::DirectAddress, Place::T1, imm(operand));
      b.load(Place::T2, Place::T1, Width::Word, Step::DirectPointer);
      b.address(Op::BankAddress, Place::T0, at(Place::T2), Place::Y);
      if (read) b.cycles(1, when(When::IndexCrossed));
      return Step::Flat;
    case M::DirectIndirectLong:
      b.address(Op::DirectAddress, Place::T1, imm(operand));
      b.load(Place::T2, Place::T1, Width::Long, Step::Bank0);
      b.address(Op::LongAddress, Place::T0, at(Place::T2));
      return Step::Flat;
    case M::DirectIndirectLongY:
      b.address(Op::DirectAddress, Place::T1, imm(operand));
      b.load(Place::T2, Place::T1, Width::Long, Step::Bank0);
      b.address(Op::LongAddress, Place::T0, at(Place::T2), Place::Y);
      return Step::Flat;
    case M::StackRelative:
      b.address(Op::StackAddress, Place::T0, imm(operand));
      return Step::Bank0;
    case M::StackRelativeY:
      b.address(Op::StackAddress, Place::T1, imm(operand));
      b.load(Place::T2, Place::T1, Width::Word, Step::Bank0);
      b.address(Op::BankAddress, Place::T0, at(Place::T2), Place::Y);
      return Step::Flat;
    case M::Absolute:
      b.address(Op::BankAddress, Place::T0, imm(operand));
      return Step::Flat;
    case M::AbsoluteX:
      b.address(Op::BankAddress, Place::T0, imm(operand), Place::X);
      if (read) b.cycles(1, when(When::IndexCrossed));
      return Step::Flat;
    case M::AbsoluteY:
      b.address(Op::BankAddress, Place::T0, imm(operand), Place::Y);
      if (read) b.cycles(1, when(When::IndexCrossed));
      return Step::Flat;
    case M::AbsoluteLong:
      b.address(Op::LongAddress, Place::T0, imm(operand));
      return Step::Flat;
    case M::AbsoluteLongX:
      b.address(Op::LongAddress, Place::T0, imm(operand), Place::X);
      return Step::Flat;
    default:
      break;
  }
  throw std::logic_error("effectiveAddress: not a memory form");
}

// What an accumulator instruction does with the operand it read.
void applyRead(Builder& b, std::string_view mnemonic, Operand value, Width width) {
  if (mnemonic == "LDA") {
    b.setNZ(Place::A, value, width);
  } else if (mnemonic == "ADC") {
    b.alu(Op::Adc, Place::A, at(Place::A), value, width);
  } else if (mnemonic == "SBC") {
    b.alu(Op::Sbc, Place::A, at(Place::A), value, width);
  } else if (mnemonic == "AND" || mnemonic == "EOR" || mnemonic == "ORA") {
    const Op op = mnemonic == "AND" ? Op::And : mnemonic == "EOR" ? Op::Xor : Op::Or;
    b.alu(op, Place::T3, at(Place::A), value, width);
    b.setNZ(Place::A, at(Place::T3), width);
  } else if (mnemonic == "CMP") {
    b.alu(Op::Cmp, Place::None, at(Place::A), value, width);
  } else if (mnemonic == "BIT") {
    b.alu(value.place == Place::Imm ? Op::BitImm : Op::Bit, Place::None, at(Place::A), value,
          width);
  } else if (mnemonic == "LDX") {
    b.setNZ(Place::X, value, width);
  } else if (mnemonic == "LDY") {
    b.setNZ(Place::Y, value, width);
  } else if (mnemonic == "CPX") {
    b.alu(Op::Cmp, Place::None, at(Place::X), value, width);
  } else if (mnemonic == "CPY") {
    b.alu(Op::Cmp, Place::None, at(Place::Y), value, width);
  } else {
    throw std::logic_error("applyRead: not a read");
  }
}

// The operator a read-modify-write applies, on the accumulator or on a value in
// a temporary.
Op modifyOp(std::string_view mnemonic) {
  if (mnemonic == "ASL") return Op::Asl;
  if (mnemonic == "LSR") return Op::Lsr;
  if (mnemonic == "ROL") return Op::Rol;
  if (mnemonic == "ROR") return Op::Ror;
  if (mnemonic == "INC") return Op::Inc;
  if (mnemonic == "DEC") return Op::Dec;
  if (mnemonic == "TSB") return Op::Tsb;
  if (mnemonic == "TRB") return Op::Trb;
  throw std::logic_error("modifyOp: not a read-modify-write");
}

// The value a store writes.
Operand storeValue(std::string_view mnemonic) {
  if (mnemonic == "STA") return at(Place::A);
  if (mnemonic == "STX") return at(Place::X);
  if (mnemonic == "STY") return at(Place::Y);
  return imm(0);  // STZ
}

// The branch a conditional branch tests, as the flag and the state that takes it.
struct BranchTest {
  Place flag = Place::None;  // None: always taken
  bool whenSet = false;
};

BranchTest branchTest(std::uint8_t opcode) {
  switch (opcode) {
    case 0x10: return {Place::FlagN, false};  // BPL
    case 0x30: return {Place::FlagN, true};   // BMI
    case 0x50: return {Place::FlagV, false};  // BVC
    case 0x70: return {Place::FlagV, true};   // BVS
    case 0x90: return {Place::FlagC, false};  // BCC
    case 0xB0: return {Place::FlagC, true};   // BCS
    case 0xD0: return {Place::FlagZ, false};  // BNE
    case 0xF0: return {Place::FlagZ, true};   // BEQ
    default: return {};                        // BRA
  }
}

// ---- the families ------------------------------------------------------------
void liftMemory(Builder& b, std::string_view mnemonic, Cpu65816Addressing mode,
                std::uint32_t operand, Kind kind, const Cpu65816Mode& cpuMode) {
  const Width wM = widthM(cpuMode);
  const Width wX = widthX(cpuMode);
  const bool read = kind == Kind::ReadM || kind == Kind::ReadX;
  const Step step = effectiveAddress(b, mode, operand, read);
  switch (kind) {
    case Kind::ReadM:
      b.load(Place::T1, Place::T0, wM, step);
      applyRead(b, mnemonic, at(Place::T1), wM);
      break;
    case Kind::ReadX:
      b.load(Place::T1, Place::T0, wX, step);
      applyRead(b, mnemonic, at(Place::T1), wX);
      break;
    case Kind::Write:
      b.store(Place::T0, storeValue(mnemonic),
              (mnemonic == "STX" || mnemonic == "STY") ? wX : wM, step);
      break;
    case Kind::Modify:
      // Read; in emulation mode write the byte just read back once; modify; write
      // back — a sixteen-bit write-back high byte first. Emulation mode forces
      // eight bits, so the extra write is never emitted for a sixteen-bit node and
      // is guarded by the live flag for the rest.
      b.load(Place::T1, Place::T0, wM, step, Access::Rmw);
      if (wM != Width::Word) {
        b.store(Place::T0, at(Place::T1), Width::Byte, step, Access::RmwUnmodified,
                when(When::Emulation));
      }
      if (mnemonic == "TSB" || mnemonic == "TRB") {
        b.alu(modifyOp(mnemonic), Place::T1, at(Place::T1), at(Place::A), wM);
      } else {
        b.unary(modifyOp(mnemonic), Place::T1, at(Place::T1), wM);
      }
      b.store(Place::T0, at(Place::T1), wM, step, Access::Rmw, always(), Op::StoreRmw);
      break;
    case Kind::Other:
      throw std::logic_error("liftMemory: not a memory instruction");
  }
}

void liftImmediate(Builder& b, std::string_view mnemonic, std::uint32_t operand,
                   const Cpu65816Mode& cpuMode) {
  const Kind kind = kindOf(mnemonic);
  applyRead(b, mnemonic, imm(operand), kind == Kind::ReadX ? widthX(cpuMode) : widthM(cpuMode));
}

void liftImplied(Builder& b, std::uint8_t opcode, const Cpu65816Mode& cpuMode) {
  const Width wM = widthM(cpuMode);
  const Width wX = widthX(cpuMode);
  switch (opcode) {
    // Transfers: the destination's width decides how much moves and sets N and Z,
    // except into the stack pointer, which sets nothing.
    case 0xAA: b.setNZ(Place::X, at(Place::A), wX); break;  // TAX
    case 0xA8: b.setNZ(Place::Y, at(Place::A), wX); break;  // TAY
    case 0xBA: b.setNZ(Place::X, at(Place::S), wX); break;  // TSX
    case 0x8A: b.setNZ(Place::A, at(Place::X), wM); break;  // TXA
    case 0x98: b.setNZ(Place::A, at(Place::Y), wM); break;  // TYA
    case 0x9B: b.setNZ(Place::Y, at(Place::X), wX); break;  // TXY
    case 0xBB: b.setNZ(Place::X, at(Place::Y), wX); break;  // TYX
    case 0x9A:                                              // TXS
      b.set(Place::S, at(Place::X), Width::Word);
      b.settle();
      break;
    // The whole sixteen-bit accumulator moves to and from the direct and stack
    // registers whatever the accumulator width; TCS sets no flags.
    case 0x5B: b.setNZ(Place::D, at(Place::A), Width::Word); break;  // TCD
    case 0x7B: b.setNZ(Place::A, at(Place::D), Width::Word); break;  // TDC
    case 0x3B: b.setNZ(Place::A, at(Place::S), Width::Word); break;  // TSC
    case 0x1B:                                                       // TCS
      b.set(Place::S, at(Place::A), Width::Word);
      b.settle();
      break;

    case 0x1A: b.unary(Op::Inc, Place::A, at(Place::A), wM); break;  // INC A
    case 0x3A: b.unary(Op::Dec, Place::A, at(Place::A), wM); break;  // DEC A
    case 0xE8: b.unary(Op::Inc, Place::X, at(Place::X), wX); break;  // INX
    case 0xC8: b.unary(Op::Inc, Place::Y, at(Place::Y), wX); break;  // INY
    case 0xCA: b.unary(Op::Dec, Place::X, at(Place::X), wX); break;  // DEX
    case 0x88: b.unary(Op::Dec, Place::Y, at(Place::Y), wX); break;  // DEY

    case 0x0A: b.unary(Op::Asl, Place::A, at(Place::A), wM); break;  // ASL A
    case 0x4A: b.unary(Op::Lsr, Place::A, at(Place::A), wM); break;  // LSR A
    case 0x2A: b.unary(Op::Rol, Place::A, at(Place::A), wM); break;  // ROL A
    case 0x6A: b.unary(Op::Ror, Place::A, at(Place::A), wM); break;  // ROR A

    case 0x18: b.flag(Place::FlagC, false); break;  // CLC
    case 0x38: b.flag(Place::FlagC, true); break;   // SEC
    case 0x58: b.flag(Place::FlagI, false); break;  // CLI
    case 0x78: b.flag(Place::FlagI, true); break;   // SEI
    case 0xD8: b.flag(Place::FlagD, false); break;  // CLD
    case 0xF8: b.flag(Place::FlagD, true); break;   // SED
    case 0xB8: b.flag(Place::FlagV, false); break;  // CLV

    case 0xEB: b.emit(Op::Xba); break;  // XBA
    case 0xFB: b.emit(Op::Xce); break;  // XCE
    case 0xEA: break;                   // NOP
    case 0x42: break;                   // WDM: the program counter steps over its second byte, and nothing else happens
    case 0xCB:                          // WAI
      b.unary(Op::Halt, Place::None, imm(0), Width::Byte);
      break;
    case 0xDB:                          // STP
      b.unary(Op::Halt, Place::None, imm(1), Width::Byte);
      break;
    default:
      throw std::logic_error("liftImplied: not an implied instruction");
  }
}

void liftStack(Builder& b, std::uint8_t opcode, std::uint32_t operand,
               const Cpu65816Mode& cpuMode) {
  const Width wM = widthM(cpuMode);
  const Width wX = widthX(cpuMode);
  switch (opcode) {
    // The registers the 6502 pushed and pulled keep the stack in page one under
    // emulation; everything the 65816 added steps out and settles after.
    case 0x48: b.push(at(Place::A), wM, true); break;            // PHA
    case 0xDA: b.push(at(Place::X), wX, true); break;            // PHX
    case 0x5A: b.push(at(Place::Y), wX, true); break;            // PHY
    case 0x08: b.push(at(Place::P), Width::Byte, true); break;   // PHP
    case 0x8B: b.push(at(Place::DBR), Width::Byte, false); b.settle(); break;  // PHB
    case 0x4B: b.push(at(Place::PBR), Width::Byte, false); b.settle(); break;  // PHK
    case 0x0B: b.push(at(Place::D), Width::Word, false); b.settle(); break;    // PHD
    case 0xF4: b.push(imm(operand & 0xFFFFu), Width::Word, false); b.settle(); break;  // PEA
    case 0x62: b.push(imm(operand & 0xFFFFu), Width::Word, false); b.settle(); break;  // PER
    case 0xD4:                                                                          // PEI
      // The pointer's second byte steps out of the direct page whatever the mode.
      b.cycles(1, when(When::DirectLowByte));
      b.address(Op::DirectAddress, Place::T0, imm(operand));
      b.load(Place::T1, Place::T0, Width::Word, Step::Bank0);
      b.push(at(Place::T1), Width::Word, false);
      b.settle();
      break;

    case 0x68:  // PLA
      b.pull(Place::T0, wM, true);
      b.setNZ(Place::A, at(Place::T0), wM);
      break;
    case 0xFA:  // PLX
      b.pull(Place::T0, wX, true);
      b.setNZ(Place::X, at(Place::T0), wX);
      break;
    case 0x7A:  // PLY
      b.pull(Place::T0, wX, true);
      b.setNZ(Place::Y, at(Place::T0), wX);
      break;
    case 0x28:  // PLP
      b.pull(Place::T0, Width::Byte, true);
      b.unary(Op::WriteP, Place::None, at(Place::T0), Width::Byte);
      break;
    case 0xAB:  // PLB
      b.pull(Place::T0, Width::Byte, false);
      b.setNZ(Place::DBR, at(Place::T0), Width::Byte);
      b.settle();
      break;
    case 0x2B:  // PLD
      b.pull(Place::T0, Width::Word, false);
      b.setNZ(Place::D, at(Place::T0), Width::Word);
      b.settle();
      break;
    default:
      throw std::logic_error("liftStack: not a stack instruction");
  }
}

void liftControl(Builder& b, std::uint8_t opcode, Cpu65816Addressing mode, std::uint32_t operand,
                 Address next) {
  using M = Cpu65816Addressing;
  switch (mode) {
    // A branch costs a cycle when taken, and one more in emulation mode when the
    // destination lies in another page than the address after the branch.
    case M::Relative: {
      const BranchTest test = branchTest(opcode);
      const bool crosses = (operand & 0xFF00u) != (next & 0xFF00u);
      if (test.flag == Place::None) {
        b.set(Place::PC, imm(operand & 0xFFFFu), Width::Word);
        if (crosses) b.cycles(1, when(When::Emulation));
        break;
      }
      const Cond taken = test.whenSet ? flagSet(test.flag) : flagClear(test.flag);
      b.set(Place::PC, imm(operand & 0xFFFFu), Width::Word, taken);
      b.cycles(1, taken);
      if (crosses) {
        Cond both = taken;
        both.andEmulation = true;
        b.cycles(1, both);
      }
      break;
    }
    case M::RelativeLong:  // BRL
      b.set(Place::PC, imm(operand & 0xFFFFu), Width::Word);
      break;

    case M::Absolute:
      if (opcode == 0x4C) {  // JMP abs
        b.set(Place::PC, imm(operand), Width::Word);
      } else {  // JSR abs: the return address is the instruction's last byte
        b.alu(Op::Sub, Place::T0, at(Place::PC), imm(1), Width::Word);
        b.push(at(Place::T0), Width::Word, true);
        b.set(Place::PC, imm(operand), Width::Word);
      }
      break;
    case M::AbsoluteLong:
      if (opcode == 0x5C) {  // JML long
        b.set(Place::PBR, imm(operand >> 16), Width::Byte);
        b.set(Place::PC, imm(operand & 0xFFFFu), Width::Word);
      } else {  // JSL: the program bank first, then the return address, then settle
        b.push(at(Place::PBR), Width::Byte, false);
        b.alu(Op::Sub, Place::T0, at(Place::PC), imm(1), Width::Word);
        b.push(at(Place::T0), Width::Word, false);
        b.settle();
        b.set(Place::PBR, imm(operand >> 16), Width::Byte);
        b.set(Place::PC, imm(operand & 0xFFFFu), Width::Word);
      }
      break;
    case M::AbsoluteIndirect:  // JMP (abs): a pointer in bank zero, wrapping within it
      b.set(Place::T0, imm(operand & 0xFFFFu), Width::Long);
      b.load(Place::T1, Place::T0, Width::Word, Step::Bank0);
      b.set(Place::PC, at(Place::T1), Width::Word);
      break;
    case M::AbsoluteIndirectLong:  // JML [abs]: three bytes in bank zero
      b.set(Place::T0, imm(operand & 0xFFFFu), Width::Long);
      b.load(Place::T1, Place::T0, Width::Long, Step::Bank0);
      b.set(Place::PC, at(Place::T1), Width::Word);
      b.alu(Op::Shr, Place::T2, at(Place::T1), imm(16), Width::Byte);
      b.set(Place::PBR, at(Place::T2), Width::Byte);
      break;
    case M::AbsoluteIndexedIndirect:
      // The pointer is in the program bank, at the operand plus X, wrapping
      // within that bank. JSR (abs,X) pushes the return address before it reads
      // the pointer.
      if (opcode == 0xFC) {
        b.alu(Op::Sub, Place::T3, at(Place::PC), imm(1), Width::Word);
        b.push(at(Place::T3), Width::Word, true);
      }
      b.alu(Op::Add, Place::T0, imm(operand & 0xFFFFu), at(Place::X), Width::Word);
      b.address(Op::ProgramAddress, Place::T1, at(Place::T0));
      b.load(Place::T2, Place::T1, Width::Word, Step::Bank);
      b.set(Place::PC, at(Place::T2), Width::Word);
      break;

    case M::Implied:
      switch (opcode) {
        case 0x60:  // RTS: pull the address and step past the call's last byte
          b.pull(Place::T0, Width::Word, true);
          b.alu(Op::Add, Place::T1, at(Place::T0), imm(1), Width::Word);
          b.set(Place::PC, at(Place::T1), Width::Word);
          break;
        case 0x6B:  // RTL: the same, with the bank after it
          b.pull(Place::T0, Width::Word, false);
          b.pull(Place::T1, Width::Byte, false);
          b.settle();
          b.set(Place::PBR, at(Place::T1), Width::Byte);
          b.alu(Op::Add, Place::T2, at(Place::T0), imm(1), Width::Word);
          b.set(Place::PC, at(Place::T2), Width::Word);
          break;
        case 0x40:  // RTI: the status byte, the address as pushed, and in native mode the bank
          b.pull(Place::T0, Width::Byte, true);
          b.pull(Place::T1, Width::Word, true);
          b.pull(Place::T2, Width::Byte, true, when(When::Native));
          b.unary(Op::WriteP, Place::None, at(Place::T0), Width::Byte);
          b.set(Place::PC, at(Place::T1), Width::Word);
          b.set(Place::PBR, at(Place::T2), Width::Byte, when(When::Native));
          break;
        default:
          throw std::logic_error("liftControl: not a return");
      }
      break;
    default:
      throw std::logic_error("liftControl: not a control-flow form");
  }
}

// BRK and COP: the program bank in native mode, the address after the signature
// byte, and the status byte go on the stack; the handler is entered through the
// vector the emulation flag selects, in bank zero, in binary mode, with maskable
// requests disabled.
void liftSoftwareInterrupt(Builder& b, std::uint8_t opcode) {
  const std::uint32_t nativeVector = opcode == 0x02 ? 0xFFE4u : 0xFFE6u;
  const std::uint32_t emulationVector = opcode == 0x02 ? 0xFFF4u : 0xFFFEu;
  b.push(at(Place::PBR), Width::Byte, true, when(When::Native));
  b.push(at(Place::PC), Width::Word, true);
  b.push(at(Place::P), Width::Byte, true);
  b.flag(Place::FlagI, true);
  b.flag(Place::FlagD, false);
  b.set(Place::T0, imm(nativeVector), Width::Long);
  b.set(Place::T0, imm(emulationVector), Width::Long, when(When::Emulation));
  b.load(Place::T1, Place::T0, Width::Word, Step::Bank0, Access::Vector);
  b.set(Place::PC, at(Place::T1), Width::Word);
  b.set(Place::PBR, imm(0), Width::Byte);
}

// A block move carries one byte each time its node runs: the destination bank
// into the data bank register, the byte from the source bank at X to the data
// bank at Y, both indexes stepped at their width, the whole sixteen-bit
// accumulator counted down, and the program counter back on the instruction
// until the count runs past zero — so an interrupt between bytes is only an
// interrupt.
void liftBlockMove(Builder& b, std::uint8_t opcode, std::uint8_t sourceBank,
                   std::uint8_t destinationBank, Address address, const Cpu65816Mode& cpuMode) {
  const Width wX = widthX(cpuMode);
  const Op step = opcode == 0x54 ? Op::Add : Op::Sub;  // MVN counts up, MVP down
  b.set(Place::DBR, imm(destinationBank), Width::Byte);
  b.address(Op::LongAddress, Place::T0, imm(static_cast<std::uint32_t>(sourceBank) << 16),
            Place::X);
  b.load(Place::T1, Place::T0, Width::Byte, Step::Flat);
  b.address(Op::BankAddress, Place::T2, imm(0), Place::Y);
  b.store(Place::T2, at(Place::T1), Width::Byte, Step::Flat);
  b.alu(step, Place::X, at(Place::X), imm(1), wX);
  b.alu(step, Place::Y, at(Place::Y), imm(1), wX);
  b.alu(Op::Sub, Place::A, at(Place::A), imm(1), Width::Word);
  b.set(Place::PC, imm(address & 0xFFFFu), Width::Word,
        Cond{.when = When::PlaceIsNot, .place = Place::A, .value = 0xFFFFu});
}

// REP clears the bits its mask names and SEP sets them, through the same write
// that re-establishes the width invariants.
void liftMask(Builder& b, std::uint8_t opcode, std::uint8_t mask) {
  b.set(Place::T0, at(Place::P), Width::Byte);
  if (opcode == 0xC2) {
    b.alu(Op::And, Place::T0, at(Place::T0), imm(static_cast<std::uint8_t>(~mask)), Width::Byte);
  } else {
    b.alu(Op::Or, Place::T0, at(Place::T0), imm(mask), Width::Byte);
  }
  b.unary(Op::WriteP, Place::None, at(Place::T0), Width::Byte);
}

// ---- the cost ------------------------------------------------------------------
// The measured base under every setting of the widths the node may run with. A
// conditional branch's measured taken cost is checked to be its base plus one,
// which is what the branch's own `Cycles` effect adds.
Cost costOf(std::uint8_t opcode, const Cpu65816Mode& mode) {
  Cost cost;
  for (int a = 0; a < 2; ++a) {
    for (int x = 0; x < 2; ++x) {
      const bool accumulator8 = a == 0;
      const bool index8 = x == 0;
      const disasm::CycleCost measured =
          disasm::cpu65816CycleTable(mode.emulation, accumulator8, index8)[opcode];
      cost.base[costIndex(accumulator8, index8)] = measured.base;
      if (measured.taken != 0 && measured.taken != measured.base + 1) {
        throw std::logic_error("the measured taken cost is not the base plus one");
      }
    }
  }
  return cost;
}

// ---- a second reading of an address, from the trace's warning -----------------
// The framework reports an address two paths read two ways as a warning naming
// both modes the way the backend describes them. The second reading is decoded
// again under that mode.
struct Conflict {
  Address address = 0;
  Cpu65816Mode mode;
};

std::optional<Conflict> parseConflict(const std::string& warning) {
  // "$BB:XXXX is reached with e=E m=M x=X and with e=E m=M x=X"
  unsigned bank = 0;
  unsigned offset = 0;
  int consumed = 0;
  if (std::sscanf(warning.c_str(), "$%2x:%4x is reached with %n", &bank, &offset, &consumed) < 2 ||
      consumed == 0) {
    return std::nullopt;
  }
  const std::string rest = warning.substr(static_cast<std::size_t>(consumed));
  const std::size_t second = rest.find(" and with ");
  if (second == std::string::npos) return std::nullopt;
  const std::string text = rest.substr(second + 10);
  char m[4] = {};
  char x[4] = {};
  int e = 0;
  if (std::sscanf(text.c_str(), "e=%d m=%3s x=%3s", &e, m, x) != 3) return std::nullopt;
  Cpu65816Mode mode;
  mode.emulation = e != 0;
  mode.accumulatorKnown = m[0] != '?';
  mode.accumulator8 = m[0] != '1';  // "8" or "?" read as eight; "16" as sixteen
  mode.indexKnown = x[0] != '?';
  mode.index8 = x[0] != '1';
  mode.carryKnown = false;
  mode.carry = false;
  return Conflict{.address = (bank << 16) | offset, .mode = mode};
}

}  // namespace

Node liftInstruction(const disasm::Instruction& instruction, const Cpu65816Mode& mode,
                     bool patched) {
  const std::uint8_t opcode = instruction.opcode;
  const disasm::Cpu65816Opcode& row = disasm::cpu65816Opcodes()[opcode];
  const std::uint8_t first = instruction.bytes.size() > 1 ? instruction.bytes[1] : 0;
  const std::uint8_t second = instruction.bytes.size() > 2 ? instruction.bytes[2] : 0;
  const std::uint8_t third = instruction.bytes.size() > 3 ? instruction.bytes[3] : 0;
  const std::uint32_t word = static_cast<std::uint32_t>(first | (second << 8));
  const Address address = instruction.address;
  const Address bank = address & 0xFF0000u;
  const Address next = bank | ((address + instruction.length) & 0xFFFFu);

  Node node;
  node.mode = irMode(mode);
  node.patched = patched;
  node.instruction.address = address;
  node.instruction.length = instruction.length;
  node.instruction.mnemonic = row.mnemonic;
  node.instruction.addressing = addressingOf(row.mode);
  node.instruction.flow = flowOf(row.flow);
  node.instruction.target = instruction.target;
  node.cost = costOf(opcode, mode);

  using M = Cpu65816Addressing;
  std::uint32_t operand = 0;
  switch (row.mode) {
    case M::Implied:
    case M::Accumulator:
      break;
    case M::ImmediateM:
    case M::ImmediateX:
      operand = instruction.length == 3 ? word : first;
      break;
    case M::ImmediateByte:
    case M::Direct:
    case M::DirectX:
    case M::DirectY:
    case M::DirectIndirect:
    case M::DirectIndirectX:
    case M::DirectIndirectY:
    case M::DirectIndirectLong:
    case M::DirectIndirectLongY:
    case M::StackRelative:
    case M::StackRelativeY:
      operand = first;
      break;
    case M::Absolute:
    case M::AbsoluteX:
    case M::AbsoluteY:
    case M::AbsoluteIndirect:
    case M::AbsoluteIndirectLong:
    case M::AbsoluteIndexedIndirect:
    case M::PushAbsolute:
      operand = word;
      break;
    case M::AbsoluteLong:
    case M::AbsoluteLongX:
      operand = word | (static_cast<std::uint32_t>(third) << 16);
      node.registerName = disasm::cpu65816RegisterName(operand);
      break;
    case M::Relative:
      operand = bank | ((next + static_cast<Address>(static_cast<std::int8_t>(first))) & 0xFFFFu);
      break;
    case M::RelativeLong:
    case M::PushRelative:
      operand = bank | ((next + static_cast<Address>(static_cast<std::int16_t>(word))) & 0xFFFFu);
      break;
    case M::BlockMove:
      operand = second;  // the source bank; the destination bank is the first byte
      node.instruction.operand2 = first;
      break;
  }
  node.instruction.operand = operand;

  Builder b;
  // Every node begins by stepping the program counter past the instruction; what
  // follows may move it again.
  b.set(Place::PC, imm(next & 0xFFFFu), Width::Word);

  const Kind kind = kindOf(row.mnemonic);
  if (opcode == 0x00 || opcode == 0x02) {
    liftSoftwareInterrupt(b, opcode);
  } else if (opcode == 0x54 || opcode == 0x44) {
    liftBlockMove(b, opcode, second, first, address, mode);
  } else if (opcode == 0xC2 || opcode == 0xE2) {
    liftMask(b, opcode, first);
  } else if (row.mode == M::ImmediateM || row.mode == M::ImmediateX) {
    liftImmediate(b, row.mnemonic, operand, mode);
  } else if (isMemoryForm(row.mode) && kind != Kind::Other) {
    liftMemory(b, row.mnemonic, row.mode, operand, kind, mode);
  } else if (row.mode == M::Accumulator) {
    b.unary(modifyOp(row.mnemonic), Place::A, at(Place::A), widthM(mode));
  } else if (row.mode == M::Relative || row.mode == M::RelativeLong ||
             row.mode == M::AbsoluteIndirect || row.mode == M::AbsoluteIndirectLong ||
             row.mode == M::AbsoluteIndexedIndirect ||
             ((row.mode == M::Absolute || row.mode == M::AbsoluteLong) &&
              (row.flow == disasm::Flow::Jump || row.flow == disasm::Flow::Call)) ||
             opcode == 0x60 || opcode == 0x6B || opcode == 0x40) {
    liftControl(b, opcode, row.mode, operand, next);
  } else if (row.mode == M::PushAbsolute || row.mode == M::PushRelative ||
             opcode == 0x48 || opcode == 0xDA || opcode == 0x5A || opcode == 0x08 ||
             opcode == 0x8B || opcode == 0x4B || opcode == 0x0B || opcode == 0xD4 ||
             opcode == 0x68 || opcode == 0xFA || opcode == 0x7A || opcode == 0x28 ||
             opcode == 0xAB || opcode == 0x2B) {
    liftStack(b, opcode, operand, mode);
  } else {
    liftImplied(b, opcode, mode);
  }
  node.effects = std::move(b.effects);
  return node;
}

std::vector<Effect> interruptSequence(Interrupt interrupt) {
  const std::uint32_t nativeVector = interrupt == Interrupt::Nmi ? 0xFFEAu : 0xFFEEu;
  const std::uint32_t emulationVector = interrupt == Interrupt::Nmi ? 0xFFFAu : 0xFFFEu;
  Builder b;
  // The instruction interrupted is read and thrown away, and the program counter
  // stays on it, so the address saved is where execution resumes.
  b.address(Op::ProgramAddress, Place::T2, at(Place::PC));
  b.load(Place::T3, Place::T2, Width::Byte, Step::Bank);
  b.push(at(Place::PBR), Width::Byte, true, when(When::Native));
  b.push(at(Place::PC), Width::Word, true);
  // In emulation mode bit 4 of the saved status is the break flag, cleared for a
  // hardware request; native mode saves the register as it stands.
  b.set(Place::T0, at(Place::P), Width::Byte);
  b.alu(Op::And, Place::T0, at(Place::T0), imm(0xEFu), Width::Byte);
  b.effects.back().when = when(When::Emulation);
  b.push(at(Place::T0), Width::Byte, true);
  b.flag(Place::FlagI, true);
  b.flag(Place::FlagD, false);
  b.set(Place::T1, imm(nativeVector), Width::Long);
  b.set(Place::T1, imm(emulationVector), Width::Long, when(When::Emulation));
  b.load(Place::T2, Place::T1, Width::Word, Step::Bank0, Access::Vector);
  b.set(Place::PC, at(Place::T2), Width::Word);
  b.set(Place::PBR, imm(0), Width::Byte);
  b.cycles(7, always());
  b.cycles(1, when(When::Native));
  return std::move(b.effects);
}

Program lift65816(const disasm::Listing& listing, std::span<const std::uint8_t> image,
                  Address base) {
  Program program;
  for (const disasm::Line& line : listing.lines) {
    if (!line.isCode) continue;
    const bool patched = line.instruction.note.find("PATCHED at run time") != std::string::npos;
    program.nodes.push_back(
        liftInstruction(line.instruction, disasm::modeOf(line.context), patched));
  }

  if (!image.empty()) {
    for (const std::string& warning : listing.warnings) {
      const std::optional<Conflict> conflict = parseConflict(warning);
      if (!conflict) continue;
      const std::optional<disasm::Instruction> second =
          disasm::decodeAt(image, base, conflict->address, conflict->mode);
      if (!second) continue;
      auto first = std::find_if(program.nodes.begin(), program.nodes.end(), [&](const Node& n) {
        return n.instruction.address == conflict->address;
      });
      if (first == program.nodes.end()) continue;
      const bool patched = first->patched;
      program.nodes.insert(std::next(first), liftInstruction(*second, conflict->mode, patched));
    }
  }

  program.nmi = interruptSequence(Interrupt::Nmi);
  program.irq = interruptSequence(Interrupt::Irq);
  return program;
}

}  // namespace snaggletooth::ir
