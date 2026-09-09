#include "ir/spc700_lift.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace snaggletooth::ir {
namespace {

using disasm::Spc700Operands;

// ---- operands ---------------------------------------------------------------
Operand imm(std::uint32_t value) {
  Operand o;
  o.place = Place::Imm;
  o.value = value;
  return o;
}
Operand at(Place place) {
  Operand o;
  o.place = place;
  return o;
}

Cond always() { return {}; }
Cond flagTest(When condition, Place flag) {
  Cond c;
  c.when = condition;
  c.place = flag;
  return c;
}
Cond placeTest(When condition, Place place, std::uint32_t value) {
  Cond c;
  c.when = condition;
  c.place = place;
  c.value = value;
  return c;
}

// ---- the effect list under construction ----------------------------------------
struct Builder {
  std::vector<Effect> effects;

  Effect& emit(Op op) {
    Effect e;
    e.op = op;
    effects.push_back(e);
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
  void alu(Op op, Place dst, Operand a, Operand b, Width width, Cond cond = always()) {
    Effect& e = emit(op);
    e.dst = at(dst);
    e.a = a;
    e.b = b;
    e.width = width;
    e.when = cond;
  }
  void unary(Op op, Place dst, Operand a, Width width) {
    Effect& e = emit(op);
    e.dst = at(dst);
    e.a = a;
    e.width = width;
  }
  // `dst` ← the direct page's base plus `offset + index`, within the page.
  void page(Place dst, Operand offset, Place index = Place::None) {
    Effect& e = emit(Op::PageAddress);
    e.dst = at(dst);
    e.a = offset;
    e.b = at(index);
    e.width = Width::Word;
  }
  void load(Place dst, Operand address, Width width, Step step = Step::Flat) {
    Effect& e = emit(Op::Load);
    e.dst = at(dst);
    e.a = address;
    e.width = width;
    e.step = step;
  }
  void store(Operand address, Operand value, Width width, Step step = Step::Flat) {
    Effect& e = emit(Op::Store);
    e.a = address;
    e.b = value;
    e.width = width;
    e.step = step;
  }
  void push(Operand value, Width width) {
    Effect& e = emit(Op::Push);
    e.a = value;
    e.width = width;
    e.pinned = true;
  }
  void pull(Place dst, Width width) {
    Effect& e = emit(Op::Pull);
    e.dst = at(dst);
    e.width = width;
    e.pinned = true;
  }
  void cycles(std::uint32_t count, Cond cond) {
    if (count == 0) return;
    Effect& e = emit(Op::Cycles);
    e.a = imm(count);
    e.when = cond;
  }
  void flag(Place flag, bool value) { set(flag, imm(value ? 1u : 0u), Width::Byte); }
  // The read of the byte after the opcode that the chip makes and throws away.
  void discard(std::uint32_t address) { load(Place::T3, imm(address), Width::Byte); }
};

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

// ---- the shapes the chip runs ---------------------------------------------------
// The routing the core's cycle engine uses: where a memory instruction's operand
// lives, what it does there, and how it reaches its destination. Instructions
// sharing a mode share its accesses, so an opcode's own contribution is the
// payload applied once its operand has arrived.
enum class AddrMode : std::uint8_t {
  Implied,
  Immediate,
  Dp,
  DpX,
  DpY,
  Abs,
  AbsX,
  AbsY,
  AbsBit,
  Indirect,            // (X)
  IndirectIncrement,   // (X)+
  IndexedIndirect,     // [dp+X]
  IndirectIndexed,     // [dp]+Y
  IndirectToIndirect,  // (X),(Y): the Y side the source, the X side the target
  DpToDp,              // source offset, destination offset
  ImmediateToDp,       // an immediate, then a destination offset
};

enum class MemAccess : std::uint8_t { Read, Write, Modify, Internal };

// What happens between the settled address and the write: most forms read the
// byte once, a modify because it needs it and a store discarding it but still
// making the access; the test-and-set pair reads it twice and keeps the first;
// the bit store reads it once and waits; `MOV (X)+,A` waits and never reads; the
// two-operand moves reach the destination only to write.
enum class Destination : std::uint8_t { None, Internal, Read, ReadThenWait, ReadThenRead };

struct MemForm {
  AddrMode mode = AddrMode::Implied;
  MemAccess access = MemAccess::Read;
  Destination destination = Destination::Read;
};

bool memoryForm(std::uint8_t opcode, MemForm& form) {
  auto is = [&](AddrMode mode, MemAccess access = MemAccess::Read,
                Destination destination = Destination::Read) {
    form = MemForm{.mode = mode, .access = access, .destination = destination};
    return true;
  };
  switch (opcode) {
    // ---- 8-bit move: memory to register ----
    case 0xE8: case 0xCD: case 0x8D: return is(AddrMode::Immediate);
    case 0xE4: case 0xF8: case 0xEB: return is(AddrMode::Dp);
    case 0xF4: case 0xFB: return is(AddrMode::DpX);
    case 0xF9: return is(AddrMode::DpY);
    case 0xE5: case 0xE9: case 0xEC: return is(AddrMode::Abs);
    case 0xF5: return is(AddrMode::AbsX);
    case 0xF6: return is(AddrMode::AbsY);
    case 0xE7: return is(AddrMode::IndexedIndirect);
    case 0xF7: return is(AddrMode::IndirectIndexed);
    case 0xE6: return is(AddrMode::Indirect);
    case 0xBF: return is(AddrMode::IndirectIncrement);

    // ---- 8-bit move: register to memory ----
    case 0xC4: case 0xCB: case 0xD8: return is(AddrMode::Dp, MemAccess::Write);
    case 0xD4: case 0xDB: return is(AddrMode::DpX, MemAccess::Write);
    case 0xD9: return is(AddrMode::DpY, MemAccess::Write);
    case 0xC5: case 0xC9: case 0xCC: return is(AddrMode::Abs, MemAccess::Write);
    case 0xD5: return is(AddrMode::AbsX, MemAccess::Write);
    case 0xD6: return is(AddrMode::AbsY, MemAccess::Write);
    case 0xC7: return is(AddrMode::IndexedIndirect, MemAccess::Write);
    case 0xD7: return is(AddrMode::IndirectIndexed, MemAccess::Write);
    case 0xC6: return is(AddrMode::Indirect, MemAccess::Write);
    case 0xAF: return is(AddrMode::IndirectIncrement, MemAccess::Write, Destination::Internal);

    // ---- 8-bit move: register to register, and the two-operand direct-page moves ----
    case 0x7D: case 0xDD: case 0x5D: case 0xFD: case 0x9D: case 0xBD: return is(AddrMode::Implied);
    case 0xFA: return is(AddrMode::DpToDp, MemAccess::Write, Destination::None);
    case 0x8F: return is(AddrMode::ImmediateToDp, MemAccess::Write);

    // ---- 8-bit arithmetic, logic and comparison against a register ----
    case 0x88: case 0xA8: case 0x68: case 0xC8: case 0xAD:
    case 0x28: case 0x08: case 0x48: return is(AddrMode::Immediate);
    case 0x84: case 0xA4: case 0x64: case 0x3E: case 0x7E:
    case 0x24: case 0x04: case 0x44: return is(AddrMode::Dp);
    case 0x94: case 0xB4: case 0x74: case 0x34: case 0x14: case 0x54: return is(AddrMode::DpX);
    case 0x85: case 0xA5: case 0x65: case 0x1E: case 0x5E:
    case 0x25: case 0x05: case 0x45: return is(AddrMode::Abs);
    case 0x95: case 0xB5: case 0x75: case 0x35: case 0x15: case 0x55: return is(AddrMode::AbsX);
    case 0x96: case 0xB6: case 0x76: case 0x36: case 0x16: case 0x56: return is(AddrMode::AbsY);
    case 0x87: case 0xA7: case 0x67: case 0x27: case 0x07: case 0x47: return is(AddrMode::IndexedIndirect);
    case 0x97: case 0xB7: case 0x77: case 0x37: case 0x17: case 0x57: return is(AddrMode::IndirectIndexed);
    case 0x86: case 0xA6: case 0x66: case 0x26: case 0x06: case 0x46: return is(AddrMode::Indirect);

    // ---- 8-bit increment, decrement, shift and rotation of a register ----
    case 0xBC: case 0x3D: case 0xFC: case 0x9C: case 0x1D: case 0xDC:
    case 0x1C: case 0x5C: case 0x3C: case 0x7C:
    case 0x9F: return is(AddrMode::Implied);

    // ---- 8-bit increment, decrement, shift and rotation of a byte in memory ----
    case 0xAB: case 0x8B: case 0x0B: case 0x4B: case 0x2B: case 0x6B: return is(AddrMode::Dp, MemAccess::Modify);
    case 0xBB: case 0x9B: case 0x1B: case 0x5B: case 0x3B: case 0x7B: return is(AddrMode::DpX, MemAccess::Modify);
    case 0xAC: case 0x8C: case 0x0C: case 0x4C: case 0x2C: case 0x6C: return is(AddrMode::Abs, MemAccess::Modify);

    // ---- 8-bit arithmetic and logic between two bytes in memory ----
    case 0x99: case 0xB9: case 0x39: case 0x19: case 0x59: return is(AddrMode::IndirectToIndirect, MemAccess::Modify);
    case 0x79: return is(AddrMode::IndirectToIndirect, MemAccess::Internal);
    case 0x89: case 0xA9: case 0x29: case 0x09: case 0x49: return is(AddrMode::DpToDp, MemAccess::Modify);
    case 0x69: return is(AddrMode::DpToDp, MemAccess::Internal);
    case 0x98: case 0xB8: case 0x38: case 0x18: case 0x58: return is(AddrMode::ImmediateToDp, MemAccess::Modify);
    case 0x78: return is(AddrMode::ImmediateToDp, MemAccess::Internal);

    // ---- multiply, divide, decimal adjust, the flags and NOP ----
    case 0xCF: case 0x9E: case 0xDF: case 0xBE:
    case 0x00: case 0x20: case 0x40: case 0x60: case 0x80: case 0xE0:
    case 0xED: case 0xA0: case 0xC0: return is(AddrMode::Implied);

    // ---- one bit of a direct-page byte, set or cleared ----
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2:
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
      return is(AddrMode::Dp, MemAccess::Modify);

    // ---- test and set or clear the bits of an absolute byte ----
    case 0x0E: case 0x4E: return is(AddrMode::Abs, MemAccess::Modify, Destination::ReadThenRead);

    // ---- the carry flag against one bit of an absolute byte ----
    case 0x4A: case 0x6A: case 0xAA: return is(AddrMode::AbsBit);
    case 0x0A: case 0x2A: case 0x8A: return is(AddrMode::AbsBit, MemAccess::Internal);
    case 0xEA: return is(AddrMode::AbsBit, MemAccess::Modify);
    case 0xCA: return is(AddrMode::AbsBit, MemAccess::Modify, Destination::ReadThenWait);

    default: return false;
  }
}

// The direct-page word instructions: one offset, two bytes, and what happens
// between them.
enum class WordForm : std::uint8_t { None, Read, Compare, Modify, Store };

WordForm wordForm(std::uint8_t opcode) {
  switch (opcode) {
    case 0x7A: case 0x9A: case 0xBA: return WordForm::Read;
    case 0x5A: return WordForm::Compare;
    case 0x1A: case 0x3A: return WordForm::Modify;
    case 0xDA: return WordForm::Store;
    default: return WordForm::None;
  }
}

// The instructions that move the program counter.
enum class Control : std::uint8_t {
  None,
  Branch,
  BitBranch,       // BBS / BBC dp.b, CBNE dp
  CompareIndexed,  // CBNE dp+X
  DecrementDp,     // DBNZ dp
  DecrementY,      // DBNZ Y
  Jump,
  JumpIndexed,
  Call,
  CallPage,
  CallVector,
  Break,
  Return,
  ReturnInterrupt,
  Push,
  Pop,
  Halt,
};

Control controlForm(std::uint8_t opcode) {
  switch (opcode) {
    case 0x2F: case 0xF0: case 0xD0: case 0xB0: case 0x90:
    case 0x70: case 0x50: case 0x30: case 0x10: return Control::Branch;
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3:
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3:
    case 0x2E: return Control::BitBranch;
    case 0xDE: return Control::CompareIndexed;
    case 0x6E: return Control::DecrementDp;
    case 0xFE: return Control::DecrementY;
    case 0x5F: return Control::Jump;
    case 0x1F: return Control::JumpIndexed;
    case 0x3F: return Control::Call;
    case 0x4F: return Control::CallPage;
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1:
      return Control::CallVector;
    case 0x0F: return Control::Break;
    case 0x6F: return Control::Return;
    case 0x7F: return Control::ReturnInterrupt;
    case 0x2D: case 0x4D: case 0x6D: case 0x0D: return Control::Push;
    case 0xAE: case 0xCE: case 0xEE: case 0x8E: return Control::Pop;
    case 0xEF: case 0xFF: return Control::Halt;
    default: return Control::None;
  }
}

// ---- the payloads ----------------------------------------------------------------
// The register a move loads, or the accumulator an arithmetic form applies to.
Place readTarget(std::uint8_t opcode) {
  switch (opcode) {
    case 0xCD: case 0xF8: case 0xF9: case 0xE9: case 0xC8: case 0x3E: case 0x1E: return Place::X;
    case 0x8D: case 0xEB: case 0xFB: case 0xEC: case 0xAD: case 0x7E: case 0x5E: return Place::Y;
    default: return Place::A;
  }
}

// What an instruction that read a byte does with it. The bit index of a bit
// operation is `bit`.
void applyRead(Builder& b, std::uint8_t opcode, Operand value, std::uint32_t bit) {
  switch (opcode) {
    // MOV A/X/Y,operand
    case 0xE8: case 0xE6: case 0xBF: case 0xE4: case 0xF4: case 0xE5: case 0xF5: case 0xF6:
    case 0xE7: case 0xF7: case 0xCD: case 0xF8: case 0xF9: case 0xE9: case 0x8D: case 0xEB:
    case 0xFB: case 0xEC:
      b.setNZ(readTarget(opcode), value, Width::Byte);
      break;
    // ADC A,operand
    case 0x88: case 0x86: case 0x84: case 0x94: case 0x85: case 0x95: case 0x96: case 0x87: case 0x97:
      b.alu(Op::Adc, Place::A, at(Place::A), value, Width::Byte);
      break;
    // SBC A,operand
    case 0xA8: case 0xA6: case 0xA4: case 0xB4: case 0xA5: case 0xB5: case 0xB6: case 0xA7: case 0xB7:
      b.alu(Op::Sbc, Place::A, at(Place::A), value, Width::Byte);
      break;
    // CMP A/X/Y,operand
    case 0x68: case 0x66: case 0x64: case 0x74: case 0x65: case 0x75: case 0x76: case 0x67: case 0x77:
    case 0xC8: case 0x3E: case 0x1E: case 0xAD: case 0x7E: case 0x5E:
      b.alu(Op::Cmp, Place::None, at(readTarget(opcode)), value, Width::Byte);
      break;
    // AND / OR / EOR A,operand
    case 0x28: case 0x26: case 0x24: case 0x34: case 0x25: case 0x35: case 0x36: case 0x27: case 0x37:
      b.alu(Op::And, Place::T3, at(Place::A), value, Width::Byte);
      b.setNZ(Place::A, at(Place::T3), Width::Byte);
      break;
    case 0x08: case 0x06: case 0x04: case 0x14: case 0x05: case 0x15: case 0x16: case 0x07: case 0x17:
      b.alu(Op::Or, Place::T3, at(Place::A), value, Width::Byte);
      b.setNZ(Place::A, at(Place::T3), Width::Byte);
      break;
    case 0x48: case 0x46: case 0x44: case 0x54: case 0x45: case 0x55: case 0x56: case 0x47: case 0x57:
      b.alu(Op::Xor, Place::T3, at(Place::A), value, Width::Byte);
      b.setNZ(Place::A, at(Place::T3), Width::Byte);
      break;
    // AND1 C,m.b / AND1 C,/m.b / MOV1 C,m.b: the bit, shifted down and masked
    case 0x4A: case 0x6A: case 0xAA:
      b.alu(Op::Shr, Place::T3, value, imm(bit), Width::Byte);
      b.alu(Op::And, Place::T3, at(Place::T3), imm(1), Width::Byte);
      if (opcode == 0x6A) b.alu(Op::Xor, Place::T3, at(Place::T3), imm(1), Width::Byte);
      if (opcode == 0xAA) {
        b.set(Place::FlagC, at(Place::T3), Width::Byte);
      } else {
        b.alu(Op::And, Place::FlagC, at(Place::FlagC), at(Place::T3), Width::Byte);
      }
      break;
    default:
      throw std::logic_error("applyRead: not a read");
  }
}

// The register a store writes.
Operand storeValue(std::uint8_t opcode) {
  switch (opcode) {
    case 0xD8: case 0xD9: case 0xC9: return at(Place::X);
    case 0xCB: case 0xDB: case 0xCC: return at(Place::Y);
    default: return at(Place::A);
  }
}

// What a read-modify-write computes from the byte in T1 (and, for a two-operand
// form, the source in `source`), leaving the byte to write back in T1.
void applyModify(Builder& b, std::uint8_t opcode, Operand source, std::uint32_t bit) {
  const Operand t1 = at(Place::T1);
  switch (opcode) {
    case 0xAB: case 0xBB: case 0xAC: b.unary(Op::Inc, Place::T1, t1, Width::Byte); break;
    case 0x8B: case 0x9B: case 0x8C: b.unary(Op::Dec, Place::T1, t1, Width::Byte); break;
    case 0x0B: case 0x1B: case 0x0C: b.unary(Op::Asl, Place::T1, t1, Width::Byte); break;
    case 0x4B: case 0x5B: case 0x4C: b.unary(Op::Lsr, Place::T1, t1, Width::Byte); break;
    case 0x2B: case 0x3B: case 0x2C: b.unary(Op::Rol, Place::T1, t1, Width::Byte); break;
    case 0x6B: case 0x7B: case 0x6C: b.unary(Op::Ror, Place::T1, t1, Width::Byte); break;

    case 0x99: case 0x89: case 0x98: b.alu(Op::Adc, Place::T1, t1, source, Width::Byte); break;
    case 0xB9: case 0xA9: case 0xB8: b.alu(Op::Sbc, Place::T1, t1, source, Width::Byte); break;
    case 0x39: case 0x29: case 0x38:
      b.alu(Op::And, Place::T1, t1, source, Width::Byte);
      b.setNZ(Place::T1, t1, Width::Byte);
      break;
    case 0x19: case 0x09: case 0x18:
      b.alu(Op::Or, Place::T1, t1, source, Width::Byte);
      b.setNZ(Place::T1, t1, Width::Byte);
      break;
    case 0x59: case 0x49: case 0x58:
      b.alu(Op::Xor, Place::T1, t1, source, Width::Byte);
      b.setNZ(Place::T1, t1, Width::Byte);
      break;

    // SET1 / CLR1 dp.b: the bit index is the opcode's top three bits
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2:
      b.alu(Op::Or, Place::T1, t1, imm(1u << (opcode >> 5)), Width::Byte);
      break;
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
      b.alu(Op::And, Place::T1, t1, imm(0xFFu & ~(1u << (opcode >> 5))), Width::Byte);
      break;

    // TSET1 / TCLR1: the flags of A - memory, then the bits A holds turned on or off
    case 0x0E:
      b.alu(Op::Sub, Place::T3, at(Place::A), t1, Width::Byte);
      b.setNZ(Place::T3, at(Place::T3), Width::Byte);
      b.alu(Op::Or, Place::T1, t1, at(Place::A), Width::Byte);
      break;
    case 0x4E:
      b.alu(Op::Sub, Place::T3, at(Place::A), t1, Width::Byte);
      b.setNZ(Place::T3, at(Place::T3), Width::Byte);
      b.alu(Op::Xor, Place::T3, at(Place::A), imm(0xFF), Width::Byte);
      b.alu(Op::And, Place::T1, t1, at(Place::T3), Width::Byte);
      break;

    // NOT1 m.b, and MOV1 m.b,C
    case 0xEA: b.alu(Op::Xor, Place::T1, t1, imm(1u << bit), Width::Byte); break;
    case 0xCA:
      b.alu(Op::And, Place::T1, t1, imm(0xFFu & ~(1u << bit)), Width::Byte);
      b.alu(Op::Shl, Place::T3, at(Place::FlagC), imm(bit), Width::Byte);
      b.alu(Op::Or, Place::T1, t1, at(Place::T3), Width::Byte);
      break;

    default:
      throw std::logic_error("applyModify: not a read-modify-write");
  }
}

// What an instruction that settles on the byte it read does, writing nothing.
void applyInternal(Builder& b, std::uint8_t opcode, Operand source, std::uint32_t bit) {
  const Operand t1 = at(Place::T1);
  switch (opcode) {
    case 0x69: case 0x78: case 0x79:
      b.alu(Op::Cmp, Place::None, t1, source, Width::Byte);
      break;
    // OR1 C,m.b / OR1 C,/m.b / EOR1 C,m.b
    case 0x0A: case 0x2A: case 0x8A:
      b.alu(Op::Shr, Place::T3, t1, imm(bit), Width::Byte);
      b.alu(Op::And, Place::T3, at(Place::T3), imm(1), Width::Byte);
      if (opcode == 0x2A) b.alu(Op::Xor, Place::T3, at(Place::T3), imm(1), Width::Byte);
      b.alu(opcode == 0x8A ? Op::Xor : Op::Or, Place::FlagC, at(Place::FlagC), at(Place::T3),
            Width::Byte);
      break;
    default:
      throw std::logic_error("applyInternal: not an internal result");
  }
}

// An implied instruction, once the byte after the opcode has been read and
// thrown away.
void applyImplied(Builder& b, std::uint8_t opcode) {
  switch (opcode) {
    case 0x7D: b.setNZ(Place::A, at(Place::X), Width::Byte); break;  // MOV A,X
    case 0xDD: b.setNZ(Place::A, at(Place::Y), Width::Byte); break;  // MOV A,Y
    case 0x5D: b.setNZ(Place::X, at(Place::A), Width::Byte); break;  // MOV X,A
    case 0xFD: b.setNZ(Place::Y, at(Place::A), Width::Byte); break;  // MOV Y,A
    case 0x9D: b.setNZ(Place::X, at(Place::S), Width::Byte); break;  // MOV X,SP
    case 0xBD: b.set(Place::S, at(Place::X), Width::Byte); break;    // MOV SP,X

    case 0xBC: b.unary(Op::Inc, Place::A, at(Place::A), Width::Byte); break;
    case 0x3D: b.unary(Op::Inc, Place::X, at(Place::X), Width::Byte); break;
    case 0xFC: b.unary(Op::Inc, Place::Y, at(Place::Y), Width::Byte); break;
    case 0x9C: b.unary(Op::Dec, Place::A, at(Place::A), Width::Byte); break;
    case 0x1D: b.unary(Op::Dec, Place::X, at(Place::X), Width::Byte); break;
    case 0xDC: b.unary(Op::Dec, Place::Y, at(Place::Y), Width::Byte); break;
    case 0x1C: b.unary(Op::Asl, Place::A, at(Place::A), Width::Byte); break;
    case 0x5C: b.unary(Op::Lsr, Place::A, at(Place::A), Width::Byte); break;
    case 0x3C: b.unary(Op::Rol, Place::A, at(Place::A), Width::Byte); break;
    case 0x7C: b.unary(Op::Ror, Place::A, at(Place::A), Width::Byte); break;
    case 0x9F:  // XCN A: the nibbles exchanged
      b.alu(Op::Shr, Place::T1, at(Place::A), imm(4), Width::Byte);
      b.alu(Op::Shl, Place::T2, at(Place::A), imm(4), Width::Byte);
      b.alu(Op::Or, Place::T3, at(Place::T1), at(Place::T2), Width::Byte);
      b.setNZ(Place::A, at(Place::T3), Width::Byte);
      break;

    case 0xCF: b.alu(Op::Mul, Place::YA, at(Place::Y), at(Place::A), Width::Word); break;
    case 0x9E: b.alu(Op::Div, Place::YA, at(Place::YA), at(Place::X), Width::Word); break;
    case 0xDF: b.unary(Op::Daa, Place::A, at(Place::A), Width::Byte); break;
    case 0xBE: b.unary(Op::Das, Place::A, at(Place::A), Width::Byte); break;

    case 0x60: b.flag(Place::FlagC, false); break;  // CLRC
    case 0x80: b.flag(Place::FlagC, true); break;   // SETC
    case 0xED: b.alu(Op::Xor, Place::FlagC, at(Place::FlagC), imm(1), Width::Byte); break;  // NOTC
    case 0x20: b.flag(Place::FlagP, false); break;  // CLRP
    case 0x40: b.flag(Place::FlagP, true); break;   // SETP
    case 0xE0:                                       // CLRV clears the half carry too
      b.flag(Place::FlagV, false);
      b.flag(Place::FlagH, false);
      break;
    case 0xA0: b.flag(Place::FlagI, true); break;   // EI
    case 0xC0: b.flag(Place::FlagI, false); break;  // DI
    case 0x00: break;                                // NOP
    default:
      throw std::logic_error("applyImplied: not an implied instruction");
  }
}

// ---- the families ------------------------------------------------------------------
// The operand bytes of an instruction, as the chip reads them.
struct Bytes {
  std::uint8_t first = 0;
  std::uint8_t second = 0;
  [[nodiscard]] std::uint16_t word() const {
    return static_cast<std::uint16_t>(first | (second << 8));
  }
};

// A memory instruction: the address settled in T0 with every access the chip
// makes on the way, then the read, the store, the modify or the comparison.
void liftMemory(Builder& b, std::uint8_t opcode, const MemForm& form, const Bytes& bytes,
                std::uint32_t next) {
  const Place t0 = Place::T0;
  const Place t1 = Place::T1;
  const Place t2 = Place::T2;
  Operand source = imm(0);  // a two-operand form's source, once read
  std::uint32_t bit = 0;
  switch (form.mode) {
    case AddrMode::Implied:
      b.discard(next);
      applyImplied(b, opcode);
      return;
    case AddrMode::Immediate:
      applyRead(b, opcode, imm(bytes.first), 0);
      return;
    case AddrMode::Dp: b.page(t0, imm(bytes.first)); break;
    case AddrMode::DpX: b.page(t0, imm(bytes.first), Place::X); break;
    case AddrMode::DpY: b.page(t0, imm(bytes.first), Place::Y); break;
    case AddrMode::Abs: b.set(t0, imm(bytes.word()), Width::Word); break;
    case AddrMode::AbsX: b.alu(Op::Add, t0, imm(bytes.word()), at(Place::X), Width::Word); break;
    case AddrMode::AbsY: b.alu(Op::Add, t0, imm(bytes.word()), at(Place::Y), Width::Word); break;
    case AddrMode::AbsBit:
      b.set(t0, imm(bytes.word() & 0x1FFFu), Width::Word);
      bit = bytes.word() >> 13;
      break;
    case AddrMode::Indirect:
    case AddrMode::IndirectIncrement:
      b.discard(next);
      b.page(t0, imm(0), Place::X);
      break;
    case AddrMode::IndexedIndirect:
      b.page(t1, imm(bytes.first), Place::X);
      b.load(t0, at(t1), Width::Word, Step::Page);
      break;
    case AddrMode::IndirectIndexed:
      b.page(t1, imm(bytes.first));
      b.load(t2, at(t1), Width::Word, Step::Page);
      b.alu(Op::Add, t0, at(t2), at(Place::Y), Width::Word);
      break;
    case AddrMode::IndirectToIndirect:
      b.discard(next);
      b.page(t0, imm(0), Place::X);
      b.page(t1, imm(0), Place::Y);
      b.load(t2, at(t1), Width::Byte);
      source = at(t2);
      break;
    case AddrMode::DpToDp:
      b.page(t1, imm(bytes.first));
      b.load(t2, at(t1), Width::Byte);
      b.page(t0, imm(bytes.second));
      source = at(t2);
      break;
    case AddrMode::ImmediateToDp:
      b.page(t0, imm(bytes.second));
      source = imm(bytes.first);
      break;
  }

  switch (form.access) {
    case MemAccess::Read:
      b.load(t1, at(t0), Width::Byte);
      applyRead(b, opcode, at(t1), bit);
      if (form.mode == AddrMode::IndirectIncrement) {
        b.alu(Op::Add, Place::X, at(Place::X), imm(1), Width::Byte);
      }
      break;
    case MemAccess::Write:
      if (form.destination == Destination::Read) b.load(Place::T3, at(t0), Width::Byte);
      b.store(at(t0), form.mode == AddrMode::DpToDp || form.mode == AddrMode::ImmediateToDp
                          ? source
                          : storeValue(opcode),
              Width::Byte);
      if (form.mode == AddrMode::IndirectIncrement) {
        b.alu(Op::Add, Place::X, at(Place::X), imm(1), Width::Byte);
      }
      break;
    case MemAccess::Modify:
      b.load(t1, at(t0), Width::Byte);
      if (form.destination == Destination::ReadThenRead) b.load(Place::T3, at(t0), Width::Byte);
      applyModify(b, opcode, source, bit);
      b.store(at(t0), at(t1), Width::Byte);
      break;
    case MemAccess::Internal:
      b.load(t1, at(t0), Width::Byte);
      applyInternal(b, opcode, source, bit);
      break;
  }
}

// A direct-page word instruction: the low byte's address in T0, the high byte's
// — one past it, inside the page — in T2.
void liftWord(Builder& b, std::uint8_t opcode, WordForm form, const Bytes& bytes) {
  const Place t0 = Place::T0;
  const Place t1 = Place::T1;
  const Place t2 = Place::T2;
  const Place t3 = Place::T3;
  b.page(t0, imm(bytes.first));
  b.page(t2, imm(static_cast<std::uint8_t>(bytes.first + 1u)));
  switch (form) {
    case WordForm::Read:
      b.load(t1, at(t0), Width::Word, Step::Page);
      if (opcode == 0xBA) {  // MOVW YA,dp
        b.setNZ(Place::YA, at(t1), Width::Word);
      } else if (opcode == 0x7A) {  // ADDW YA,dp: no carry in
        b.flag(Place::FlagC, false);
        b.alu(Op::Adc, Place::YA, at(Place::YA), at(t1), Width::Word);
      } else {  // SUBW YA,dp: no borrow in
        b.flag(Place::FlagC, true);
        b.alu(Op::Sbc, Place::YA, at(Place::YA), at(t1), Width::Word);
      }
      break;
    case WordForm::Compare:
      b.load(t1, at(t0), Width::Word, Step::Page);
      b.alu(Op::Cmp, Place::None, at(Place::YA), at(t1), Width::Word);
      break;
    case WordForm::Modify: {
      // Each byte goes back before the next is read; the high byte moves only
      // when the low one wrapped, and the flags are the whole word's.
      const bool increment = opcode == 0x3A;
      b.load(t1, at(t0), Width::Byte);
      b.alu(increment ? Op::Add : Op::Sub, t1, at(t1), imm(1), Width::Byte);
      b.store(at(t0), at(t1), Width::Byte);
      b.load(t3, at(t2), Width::Byte);
      b.alu(increment ? Op::Add : Op::Sub, t3, at(t3), imm(1), Width::Byte,
            placeTest(When::PlaceIs, t1, increment ? 0x00u : 0xFFu));
      b.store(at(t2), at(t3), Width::Byte);
      b.alu(Op::Shl, t3, at(t3), imm(8), Width::Word);
      b.alu(Op::Or, t3, at(t3), at(t1), Width::Word);
      b.setNZ(t3, at(t3), Width::Word);
      break;
    }
    case WordForm::Store:
      b.load(t3, at(t0), Width::Byte);
      b.store(at(t0), at(Place::A), Width::Byte);
      b.store(at(t2), at(Place::Y), Width::Byte);
      break;
    case WordForm::None:
      throw std::logic_error("liftWord: not a word instruction");
  }
}

// The flag a relative branch tests, and the state that takes it.
struct BranchTest {
  Place flag = Place::None;  // None: always taken
  bool whenSet = false;
};

BranchTest branchTest(std::uint8_t opcode) {
  switch (opcode) {
    case 0xF0: return {Place::FlagZ, true};   // BEQ
    case 0xD0: return {Place::FlagZ, false};  // BNE
    case 0xB0: return {Place::FlagC, true};   // BCS
    case 0x90: return {Place::FlagC, false};  // BCC
    case 0x70: return {Place::FlagV, true};   // BVS
    case 0x50: return {Place::FlagV, false};  // BVC
    case 0x30: return {Place::FlagN, true};   // BMI
    case 0x10: return {Place::FlagN, false};  // BPL
    default: return {};                        // BRA
  }
}

// A branch taken under `cond`: the program counter to the target and the
// cycles the taken branch costs beyond the base.
void takeBranch(Builder& b, Address target, std::uint32_t extra, Cond cond) {
  b.set(Place::PC, imm(target), Width::Word, cond);
  b.cycles(extra, cond);
}

// A control-flow instruction. `extra` is the cycles a taken branch costs beyond
// the measured base; `next` is the address after the opcode, which the one-byte
// forms read and throw away.
void liftControl(Builder& b, std::uint8_t opcode, Control form, const Bytes& bytes,
                 const disasm::Instruction& instruction, std::uint32_t extra, std::uint32_t next) {
  const Place t0 = Place::T0;
  const Place t1 = Place::T1;
  const Address target = instruction.target.value_or(0);
  switch (form) {
    case Control::Branch: {
      const BranchTest test = branchTest(opcode);
      if (test.flag == Place::None) {
        b.set(Place::PC, imm(target), Width::Word);
      } else {
        takeBranch(b, target, extra,
                   flagTest(test.whenSet ? When::FlagSet : When::FlagClear, test.flag));
      }
      break;
    }
    case Control::BitBranch:
    case Control::CompareIndexed:
      if (form == Control::CompareIndexed) {
        b.page(t0, imm(bytes.first), Place::X);
      } else {
        b.page(t0, imm(bytes.first));
      }
      b.load(t1, at(t0), Width::Byte);
      if (opcode == 0x2E || opcode == 0xDE) {  // CBNE: branch when A differs
        b.alu(Op::Sub, t1, at(t1), at(Place::A), Width::Byte);
        takeBranch(b, target, extra, placeTest(When::PlaceIsNot, t1, 0));
      } else {
        // BBS / BBC: the bit is the opcode's top three bits, and an even high
        // nibble branches on it set.
        const bool onSet = ((opcode >> 4) & 1u) == 0u;
        b.alu(Op::And, t1, at(t1), imm(1u << (opcode >> 5)), Width::Byte);
        takeBranch(b, target, extra, placeTest(onSet ? When::PlaceIsNot : When::PlaceIs, t1, 0));
      }
      break;
    case Control::DecrementDp:
      b.page(t0, imm(bytes.first));
      b.load(t1, at(t0), Width::Byte);
      b.alu(Op::Sub, t1, at(t1), imm(1), Width::Byte);
      b.store(at(t0), at(t1), Width::Byte);
      takeBranch(b, target, extra, placeTest(When::PlaceIsNot, t1, 0));
      break;
    case Control::DecrementY:
      b.discard(next);
      b.alu(Op::Sub, Place::Y, at(Place::Y), imm(1), Width::Byte);
      takeBranch(b, target, extra, placeTest(When::PlaceIsNot, Place::Y, 0));
      break;
    case Control::Jump:
      b.set(Place::PC, imm(bytes.word()), Width::Word);
      break;
    case Control::JumpIndexed:
      b.alu(Op::Add, t0, imm(bytes.word()), at(Place::X), Width::Word);
      b.load(t1, at(t0), Width::Word);
      b.set(Place::PC, at(t1), Width::Word);
      break;
    case Control::Call:
      b.push(at(Place::PC), Width::Word);
      b.set(Place::PC, imm(bytes.word()), Width::Word);
      break;
    case Control::CallPage:
      b.push(at(Place::PC), Width::Word);
      b.set(Place::PC, imm(0xFF00u | bytes.first), Width::Word);
      break;
    case Control::CallVector:
      b.discard(next);
      b.push(at(Place::PC), Width::Word);
      b.load(Place::PC, imm(0xFFDEu - 2u * (opcode >> 4)), Width::Word);
      break;
    case Control::Break:
      b.discard(next);
      b.push(at(Place::PC), Width::Word);
      b.push(at(Place::P), Width::Byte);
      b.load(Place::PC, imm(0xFFDEu), Width::Word);
      b.flag(Place::FlagB, true);
      b.flag(Place::FlagI, false);
      break;
    case Control::Return:
      b.discard(next);
      b.pull(Place::PC, Width::Word);
      break;
    case Control::ReturnInterrupt:
      b.discard(next);
      b.pull(Place::P, Width::Byte);
      b.pull(Place::PC, Width::Word);
      break;
    case Control::Push: {
      b.discard(next);
      const Place source = opcode == 0x2D ? Place::A : opcode == 0x4D ? Place::X
                         : opcode == 0x6D ? Place::Y : Place::P;
      b.push(at(source), Width::Byte);
      break;
    }
    case Control::Pop: {
      b.discard(next);
      const Place dst = opcode == 0xAE ? Place::A : opcode == 0xCE ? Place::X
                      : opcode == 0xEE ? Place::Y : Place::P;
      b.pull(dst, Width::Byte);
      break;
    }
    case Control::Halt: {
      for (int i = 0; i < 3; ++i) b.discard(next);
      Effect& e = b.emit(Op::Halt);
      e.a = imm(opcode == 0xEF ? 0u : 1u);
      e.width = Width::Byte;
      break;
    }
    case Control::None:
      throw std::logic_error("liftControl: not a control-flow instruction");
  }
}

// The instruction layer's operands, by the shape of the operand bytes: the first
// operand's value as the dialect writes it, and the second byte where a form has
// one.
void operandsOf(const disasm::Instruction& instruction, Spc700Operands shape, const Bytes& bytes,
                Instruction& out) {
  switch (shape) {
    case Spc700Operands::None: break;
    case Spc700Operands::Imm:
    case Spc700Operands::Dp:
    case Spc700Operands::Upage:
      out.operand = bytes.first;
      break;
    case Spc700Operands::Abs:
      out.operand = bytes.word();
      break;
    case Spc700Operands::AbsBit:
      out.operand = bytes.word() & 0x1FFFu;
      out.operand2 = static_cast<std::uint8_t>(bytes.word() >> 13);
      break;
    case Spc700Operands::Rel:
      out.operand = instruction.target.value_or(0);
      break;
    case Spc700Operands::DpRel:
      out.operand = bytes.first;
      break;
    case Spc700Operands::DpDp:
    case Spc700Operands::ImmDp:
      out.operand = bytes.first;
      out.operand2 = bytes.second;
      break;
  }
}

}  // namespace

Node liftSpc700Instruction(const disasm::Instruction& instruction, bool patched) {
  const std::uint8_t opcode = instruction.opcode;
  const disasm::Spc700Opcode& row = disasm::spc700Opcodes()[opcode];
  Bytes bytes;
  if (instruction.bytes.size() > 1) bytes.first = instruction.bytes[1];
  if (instruction.bytes.size() > 2) bytes.second = instruction.bytes[2];

  Node node;
  Instruction& i = node.instruction;
  i.address = instruction.address & 0xFFFFu;
  i.length = instruction.length;
  i.mnemonic = disasm::spc700Mnemonic(opcode);
  i.form = disasm::spc700Form(opcode);
  i.flow = flowOf(instruction.flow);
  i.target = instruction.target;
  operandsOf(instruction, row.operands, bytes, i);
  node.cost.base = {instruction.cycles.base, instruction.cycles.base, instruction.cycles.base,
                    instruction.cycles.base};
  if (instruction.operandAddress) {
    node.registerName = disasm::spc700Backend().registerName(*instruction.operandAddress);
  }
  node.patched = patched;

  const std::uint32_t next = (i.address + 1u) & 0xFFFFu;
  const std::uint32_t extra =
      instruction.cycles.taken > instruction.cycles.base
          ? static_cast<std::uint32_t>(instruction.cycles.taken - instruction.cycles.base)
          : 0u;

  Builder b;
  b.set(Place::PC, imm((i.address + i.length) & 0xFFFFu), Width::Word);
  if (const WordForm word = wordForm(opcode); word != WordForm::None) {
    liftWord(b, opcode, word, bytes);
  } else if (MemForm form; memoryForm(opcode, form)) {
    liftMemory(b, opcode, form, bytes, next);
  } else {
    liftControl(b, opcode, controlForm(opcode), bytes, instruction, extra, next);
  }
  node.effects = std::move(b.effects);
  return node;
}

std::vector<Node> liftSpc700(const disasm::Listing& listing) {
  std::vector<Node> nodes;
  for (const disasm::Line& line : listing.lines) {
    if (!line.isCode) continue;
    const bool patched = line.instruction.note.find("PATCHED at run time") != std::string::npos;
    nodes.push_back(liftSpc700Instruction(line.instruction, patched));
  }
  return nodes;
}

}  // namespace snaggletooth::ir
