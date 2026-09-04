#pragma once

// The intermediate representation — a program's instructions as their meaning,
// with no bytes in it.
//
// A node is one instruction at one address under one mode, and it carries two
// layers. The instruction layer names the instruction the way source does:
// address, mnemonic, addressing mode, operand value, and the mode it reads under.
// Mnemonic and addressing mode together name one opcode, and the operand with its
// width names the operand bytes, so a renderer reproduces the bytes without ever
// holding them. The effect layer says what the instruction does: a sequence of
// typed operations over the CPU's named state, a few temporaries, and a bus. An
// interpreter runs the effect layer and nothing else; a renderer reads the
// instruction layer and nothing else. Neither sees a byte, and nothing in this
// directory holds one — the lift from a listing is the one place bytes enter, and
// it is where they stop.
//
// The effects model the CPU alone. A memory access is a load or a store at an
// address the effects before it computed; whether that address is a hardware
// register is the memory map's answer, and the memory map belongs to whatever runs
// the program. A register name is attached to a node only where the instruction's
// own bytes name the bank.
//
// A width is a type where the trace that produced the node settled it, and a
// selection by the live flag where it did not — after `PLP` or `RTI` in native mode
// the flags are whatever came off the stack, and the node says so rather than
// guessing. Every rule an effect follows is stated beside it here and, in full, in
// `docs/ir.md`.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace snaggletooth::ir {

// A 24-bit address: the bank in bits 16-23, the offset within it below.
using Address = std::uint32_t;

// The mode a node reads under: the emulation flag, and each register width with
// whether it is known. Emulation mode forces both widths to eight and known.
struct Mode {
  bool emulation = true;
  bool accumulator8 = true;
  bool index8 = true;
  bool accumulatorKnown = true;
  bool indexKnown = true;
  friend bool operator==(const Mode&, const Mode&) = default;
};

// How an instruction's operand is laid out. With the mnemonic it names one
// opcode; with the operand value it names the operand bytes.
enum class Addressing : std::uint8_t {
  Implied,
  Accumulator,
  ImmediateM,           // an immediate as wide as the accumulator
  ImmediateX,           // an immediate as wide as the index registers
  ImmediateByte,        // one byte: the masks of REP and SEP, WDM, the BRK and COP signature
  Direct,
  DirectX,
  DirectY,
  DirectIndirect,       // (dp)
  DirectIndirectX,      // (dp,X)
  DirectIndirectY,      // (dp),Y
  DirectIndirectLong,   // [dp]
  DirectIndirectLongY,  // [dp],Y
  StackRelative,        // sr,S
  StackRelativeY,       // (sr,S),Y
  Absolute,
  AbsoluteX,
  AbsoluteY,
  AbsoluteLong,
  AbsoluteLongX,
  AbsoluteIndirect,         // (abs), a pointer in bank zero
  AbsoluteIndirectLong,     // [abs], a three-byte pointer in bank zero
  AbsoluteIndexedIndirect,  // (abs,X), a pointer in the program bank
  Relative,                 // an 8-bit displacement, carried as the address it reaches
  RelativeLong,             // a 16-bit displacement, carried the same way
  BlockMove,                // a source bank and a destination bank
  PushAbsolute,             // PEA: a 16-bit value
  PushRelative,             // PER: a 16-bit displacement, carried as the address it names
};

// How execution leaves an instruction.
enum class Flow : std::uint8_t {
  Continue,  // falls through
  Branch,    // falls through, and may also reach the target
  Jump,      // never falls through
  Call,      // reaches the target, and execution resumes after it
  Return,    // never falls through
  Halt,      // never falls through and never resumes
};

// The instruction layer: what source says about an instruction, and nothing that
// source does not. `operand` is the operand's value as the dialect writes it — an
// immediate, a direct-page offset, an absolute or long address, the target
// address of a relative form, the value PEA pushes, the address PER names, or a
// block move's source bank, whose destination bank is `operand2`. `target` is the
// constant successor of a branch, a jump or a call, when the instruction names
// one.
struct Instruction {
  Address address = 0;
  std::uint8_t length = 1;
  std::string_view mnemonic;
  Addressing addressing = Addressing::Implied;
  Flow flow = Flow::Continue;
  std::uint32_t operand = 0;
  std::uint8_t operand2 = 0;
  std::optional<Address> target;
};

// How wide an effect works. `ByM` and `ByX` are eight bits when the live flag says
// so — the emulation flag, or the accumulator or index width bit — and sixteen
// otherwise; the other three are fixed.
enum class Width : std::uint8_t { Byte, Word, Long, ByM, ByX };

// The state an effect names. The registers are the CPU's own; the four
// temporaries belong to the node and start at zero when it runs; a flag is one bit
// of the status register, and `E` the emulation flag beside it. `Imm` is a
// constant, carried in the operand's value.
enum class Place : std::uint8_t {
  None,
  Imm,
  A,    // the 16-bit accumulator: A below, B above
  X,
  Y,
  S,
  D,
  PC,
  PBR,
  DBR,
  P,
  E,
  T0,
  T1,
  T2,
  T3,
  FlagN,
  FlagV,
  FlagM,
  FlagX,
  FlagD,
  FlagI,
  FlagZ,
  FlagC,
};

struct Operand {
  Place place = Place::None;
  std::uint32_t value = 0;  // when `place` is `Imm`
};

// How the second and third bytes of a multi-byte access find their addresses.
enum class Step : std::uint8_t {
  Flat,           // the next 24-bit address
  Bank0,          // the next address within bank zero
  Bank,           // the next address within the first byte's bank
  Direct,         // within bank zero — or within the page, in emulation mode with the direct register's low byte zero
  DirectPointer,  // within bank zero — or within the page, in emulation mode with the direct register zero
};

// What a bus access is for. A run compares addresses, values and order alike
// whatever the kind; the kind says which pins the chip drives.
enum class Access : std::uint8_t {
  Data,
  Rmw,            // a read-modify-write's read, or its write-back
  RmwUnmodified,  // the write of the byte just read that emulation mode drives before the write-back
  Vector,         // an interrupt vector pull
};

// The condition an effect runs under.
enum class When : std::uint8_t {
  Always,
  Emulation,      // the emulation flag is set
  Native,         // it is clear
  FlagSet,        // the flag `place` names is set
  FlagClear,      // it is clear
  PlaceIs,        // the register `place` names holds `value`
  PlaceIsNot,     // it does not
  DirectLowByte,  // the direct register's low byte is non-zero
  IndexCrossed,   // the index registers are eight bits wide and the last bank-relative address's low-byte addition carried
};

struct Cond {
  When when = When::Always;
  Place place = Place::None;
  std::uint32_t value = 0;
  bool andEmulation = false;  // and the emulation flag is set as well
};

// The operations. Each is written `dst ← f(a, b)` where it has a destination;
// `width` sizes the values, and a register written at eight bits follows the
// register's own rule — the accumulator keeps its high byte, an index register
// clears its own.
enum class Op : std::uint8_t {
  Set,     // dst ← a
  SetNZ,   // dst ← a, and N and Z from the value at the width
  Add,     // dst ← a + b, masked to the width, no flags
  Sub,     // dst ← a - b, masked, no flags
  And,     // dst ← a & b, no flags
  Or,      // dst ← a | b, no flags
  Xor,     // dst ← a ^ b, no flags
  Shr,     // dst ← a >> b, masked to the width, no flags

  DirectAddress,   // dst ← the bank-zero address D + a + b, page-wrapped in emulation mode with D's low byte zero
  BankAddress,     // dst ← DBR:a + b as a 24-bit sum, recording whether the low-byte addition carried
  LongAddress,     // dst ← a + b as a 24-bit sum
  ProgramAddress,  // dst ← PBR:a
  StackAddress,    // dst ← (S + a) in bank zero

  Load,      // dst ← the value at address a, bytes stepping by `step`, low byte first
  Store,     // the value b to address a, low byte first
  StoreRmw,  // the same, high byte first — a read-modify-write's write-back
  Push,      // the value a onto the stack, high byte first; `pinned` keeps S in page one under emulation
  Pull,      // dst ← the value pulled, low byte first; `pinned` likewise
  SettleStack,  // in emulation mode, S back into page one

  Adc,     // dst ← a + b + C at the width, decimal when D is set; N V Z C
  Sbc,     // dst ← a - b - !C at the width, decimal when D is set; N V Z C
  Cmp,     // N Z C from a - b at the width; nothing written
  Bit,     // Z from a & b; N and V from b's two top bits at the width
  BitImm,  // Z from a & b alone
  Asl,     // dst ← a << 1; C from the bit shifted out; N Z
  Lsr,     // dst ← a >> 1; C from the bit shifted out; N Z
  Rol,     // dst ← (a << 1) | C; C from the bit shifted out; N Z
  Ror,     // dst ← (a >> 1) | (C at the top); C from the bit shifted out; N Z
  Inc,     // dst ← a + 1 at the width; N Z
  Dec,     // dst ← a - 1 at the width; N Z
  Tsb,     // dst ← a | b; Z from a & b
  Trb,     // dst ← a & ~b; Z from a & b
  WriteP,  // P ← a, with M and X forced set under emulation, the index high bytes cleared when X narrows, and S pinned under emulation
  Xba,     // exchange the accumulator's halves; N Z from the new low byte
  Xce,     // exchange C and E; entering emulation forces both widths, clears the index high bytes and pins S
  Halt,    // stop running: a is 0 for a wait an interrupt ends, 1 for a stop only a reset ends
  Cycles,  // the instruction costs a more cycles
};

struct Effect {
  Op op = Op::Set;
  Operand dst;
  Operand a;
  Operand b;
  Width width = Width::Byte;
  Step step = Step::Flat;
  Access access = Access::Data;
  bool pinned = false;
  Cond when;
};

// What an instruction costs: the measured base under each setting of the widths
// the node may run with — indexed by `costIndex` — plus whatever `Cycles` effects
// fire when it runs.
struct Cost {
  std::array<std::uint8_t, 4> base{};
};

[[nodiscard]] constexpr std::size_t costIndex(bool accumulator8, bool index8) noexcept {
  return (accumulator8 ? 0u : 2u) | (index8 ? 0u : 1u);
}

// One instruction at one address under one mode.
//
// `registerName` is the hardware register the operand names, attached only where
// the bytes name the bank — a long operand — and empty everywhere else. `patched`
// marks a node lifted from bytes that differ from the image the code started as,
// so it is a different reading of its address from the one the prior image gives.
struct Node {
  Instruction instruction;
  Mode mode;
  std::vector<Effect> effects;
  Cost cost;
  std::string_view registerName;
  bool patched = false;
};

// A lifted program: its nodes in address order, with one node per address and
// mode — an address two paths read two ways is two nodes, the first reading
// first — and the two hardware interrupt sequences, which are not the program's
// but the chip's, run by an interpreter between two nodes when its host asks.
struct Program {
  std::vector<Node> nodes;
  std::vector<Effect> nmi;
  std::vector<Effect> irq;

  // The node at an address for the live flags, or nothing. A node whose width is
  // a live-flag selection matches either setting of that width.
  [[nodiscard]] const Node* find(Address address, bool emulation, bool accumulator8,
                                 bool index8) const noexcept {
    auto it = std::lower_bound(nodes.begin(), nodes.end(), address,
                               [](const Node& node, Address wanted) {
                                 return node.instruction.address < wanted;
                               });
    for (; it != nodes.end() && it->instruction.address == address; ++it) {
      const Mode& mode = it->mode;
      if (mode.emulation != emulation) continue;
      if (mode.accumulatorKnown && mode.accumulator8 != accumulator8) continue;
      if (mode.indexKnown && mode.index8 != index8) continue;
      return &*it;
    }
    return nullptr;
  }
};

}  // namespace snaggletooth::ir
