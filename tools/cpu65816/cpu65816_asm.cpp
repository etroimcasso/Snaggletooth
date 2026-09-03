#include "cpu65816_asm.h"

#include <array>
#include <optional>
#include <utility>

namespace snaggletooth::assembler {
namespace {

using disasm::Cpu65816Addressing;
using disasm::Cpu65816Mode;
using disasm::Cpu65816Opcode;

// The register names the dialect's syntax uses, and the dialect's directives;
// a label may spell neither.
constexpr std::array<std::string_view, 4> kRegisters = {"A", "X", "Y", "S"};
constexpr std::array<std::string_view, 6> kDirectives = {"A8", "A16", "X8", "X16",
                                                          "EMULATION", "NATIVE"};

// How an operand is written, before the mnemonic says which addressing mode
// that is. `first` and `second` are the expressions inside the syntax.
enum class Shape : std::uint8_t {
  Implied,
  Accumulator,
  Immediate,          // #e
  Bare,               // e
  IndexX,             // e,X
  IndexY,             // e,Y
  IndexS,             // e,S
  Absolute,           // !e
  AbsoluteX,          // !e,X
  AbsoluteY,          // !e,Y
  Indirect,           // (e)
  IndirectX,          // (e,X)
  IndirectY,          // (e),Y
  StackIndirectY,     // (e,S),Y
  AbsoluteIndirect,   // (!e)
  AbsoluteIndexedIndirect,  // (!e,X)
  IndirectLong,       // [e]
  IndirectLongY,      // [e],Y
  AbsoluteIndirectLong,     // [!e]
  BlockMove,          // e,e
};

struct Operand {
  Shape shape = Shape::Implied;
  std::string first;
  std::string second;
  bool longMarked = false;  // written with a bank separator or behind `>`
  std::string error;
};

// Whether an expression's text carries a bank separator outside a literal.
bool hasBankSeparator(std::string_view text) {
  bool quoted = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && quoted) {
      ++i;
      continue;
    }
    if (text[i] == '\'') quoted = !quoted;
    else if (text[i] == ':' && !quoted) return true;
  }
  return false;
}

// Takes an operand's compacted text apart.
Operand parseOperand(std::string_view text) {
  Operand out;
  if (text.empty()) return out;
  if (upper(text) == "A") {
    out.shape = Shape::Accumulator;
    return out;
  }

  // An expression from `from`, then what follows it.
  auto expression = [&](std::size_t from, std::string& into) -> std::string_view {
    const std::size_t end = expressionEnd(text, from);
    if (end == from) {
      out.error = "expected an operand";
      return {};
    }
    into = std::string(text.substr(from, end - from));
    return text.substr(end);
  };

  if (text.front() == '#') {
    out.shape = Shape::Immediate;
    const std::string_view rest = expression(1, out.first);
    if (!out.error.empty()) return out;
    if (!rest.empty()) out.error = "unexpected `" + std::string(rest) + "` after the immediate";
    return out;
  }

  if (text.front() == '(' || text.front() == '[') {
    const bool bracket = text.front() == '[';
    std::size_t from = 1;
    const bool absolute = from < text.size() && text[from] == '!';
    if (absolute) ++from;
    const std::string rest = upper(expression(from, out.first));
    if (!out.error.empty()) return out;
    if (absolute && hasBankSeparator(out.first)) {
      out.error = "`!` with a bank separator: the bank already says the operand is long";
      return out;
    }
    if (bracket) {
      if (rest == "]") out.shape = absolute ? Shape::AbsoluteIndirectLong : Shape::IndirectLong;
      else if (rest == "],Y" && !absolute) out.shape = Shape::IndirectLongY;
      else out.error = "expected `]` or `],Y` after the operand";
      return out;
    }
    if (rest == ")") out.shape = absolute ? Shape::AbsoluteIndirect : Shape::Indirect;
    else if (rest == ",X)") out.shape = absolute ? Shape::AbsoluteIndexedIndirect : Shape::IndirectX;
    else if (rest == "),Y" && !absolute) out.shape = Shape::IndirectY;
    else if (rest == ",S),Y" && !absolute) out.shape = Shape::StackIndirectY;
    else out.error = "expected `)`, `,X)`, `),Y` or `,S),Y` after the operand";
    return out;
  }

  if (text.front() == '!') {
    const std::string rest = upper(expression(1, out.first));
    if (!out.error.empty()) return out;
    if (hasBankSeparator(out.first)) {
      out.error = "`!` with a bank separator: the bank already says the operand is long";
      return out;
    }
    if (rest.empty()) out.shape = Shape::Absolute;
    else if (rest == ",X") out.shape = Shape::AbsoluteX;
    else if (rest == ",Y") out.shape = Shape::AbsoluteY;
    else out.error = "unexpected `" + rest + "` after the absolute operand";
    return out;
  }

  std::size_t from = 0;
  if (text.front() == '>') {
    out.longMarked = true;
    from = 1;
  }
  const std::string_view rest = expression(from, out.first);
  if (!out.error.empty()) return out;
  if (hasBankSeparator(out.first)) out.longMarked = true;
  const std::string suffix = upper(rest);
  if (suffix.empty()) {
    out.shape = Shape::Bare;
  } else if (suffix == ",X") {
    out.shape = Shape::IndexX;
  } else if (suffix == ",Y") {
    out.shape = Shape::IndexY;
  } else if (suffix == ",S") {
    out.shape = Shape::IndexS;
  } else if (rest.front() == ',' && !out.longMarked) {
    const std::size_t end = expressionEnd(rest, 1);
    if (end != rest.size() || end == 1) {
      out.error = "unexpected `" + std::string(rest) + "` after the operand";
      return out;
    }
    out.second = std::string(rest.substr(1));
    out.shape = Shape::BlockMove;
  } else {
    out.error = "unexpected `" + std::string(rest) + "` after the operand";
  }
  return out;
}

// The syntax of an addressing mode, as a diagnostic names it.
std::string_view syntaxOf(Cpu65816Addressing mode) {
  switch (mode) {
    case Cpu65816Addressing::Implied: return "implied";
    case Cpu65816Addressing::Accumulator: return "A";
    case Cpu65816Addressing::ImmediateM:
    case Cpu65816Addressing::ImmediateX:
    case Cpu65816Addressing::ImmediateByte: return "#imm";
    case Cpu65816Addressing::Direct: return "dp";
    case Cpu65816Addressing::DirectX: return "dp,X";
    case Cpu65816Addressing::DirectY: return "dp,Y";
    case Cpu65816Addressing::DirectIndirect: return "(dp)";
    case Cpu65816Addressing::DirectIndirectX: return "(dp,X)";
    case Cpu65816Addressing::DirectIndirectY: return "(dp),Y";
    case Cpu65816Addressing::DirectIndirectLong: return "[dp]";
    case Cpu65816Addressing::DirectIndirectLongY: return "[dp],Y";
    case Cpu65816Addressing::StackRelative: return "sr,S";
    case Cpu65816Addressing::StackRelativeY: return "(sr,S),Y";
    case Cpu65816Addressing::Absolute: return "!abs";
    case Cpu65816Addressing::AbsoluteX: return "!abs,X";
    case Cpu65816Addressing::AbsoluteY: return "!abs,Y";
    case Cpu65816Addressing::AbsoluteLong: return "long";
    case Cpu65816Addressing::AbsoluteLongX: return "long,X";
    case Cpu65816Addressing::AbsoluteIndirect: return "(!abs)";
    case Cpu65816Addressing::AbsoluteIndirectLong: return "[!abs]";
    case Cpu65816Addressing::AbsoluteIndexedIndirect: return "(!abs,X)";
    case Cpu65816Addressing::Relative:
    case Cpu65816Addressing::RelativeLong: return "rel";
    case Cpu65816Addressing::BlockMove: return "bank,bank";
    case Cpu65816Addressing::PushAbsolute: return "abs";
    case Cpu65816Addressing::PushRelative: return "rel";
  }
  return "";
}

}  // namespace

Cpu65816Dialect::Cpu65816Dialect() {
  for (const Cpu65816Opcode& row : disasm::cpu65816Opcodes()) {
    forms_[row.mnemonic].push_back(&row);
  }
}

bool Cpu65816Dialect::reserved(std::string_view upperName) const {
  if (forms_.count(std::string(upperName)) != 0) return true;
  for (std::string_view reg : kRegisters) {
    if (reg == upperName) return true;
  }
  for (std::string_view directive : kDirectives) {
    if (directive == upperName) return true;
  }
  return false;
}

void Cpu65816Dialect::beginRegion() { mode_ = Cpu65816Mode::nativeUnknown(); }

bool Cpu65816Dialect::directive(std::string_view upperName, std::string_view operands,
                                const Evaluator&, std::string& error) {
  bool known = false;
  for (std::string_view directive : kDirectives) {
    if (directive == upperName) known = true;
  }
  if (!known) return false;
  if (!compact(operands).empty()) {
    error = std::string(upperName) + " takes no operand";
    return true;
  }
  if (upperName == "EMULATION") {
    mode_.emulation = true;
    mode_.accumulator8 = true;
    mode_.index8 = true;
    mode_.accumulatorKnown = true;
    mode_.indexKnown = true;
    return true;
  }
  if (upperName == "NATIVE") {
    mode_.emulation = false;
    return true;
  }
  const bool accumulator = upperName.front() == 'A';
  const bool eight = upperName.back() == '8';
  if (mode_.emulation && !eight) {
    error = std::string(upperName) + " in emulation mode: both widths are eight until XCE leaves it";
    return true;
  }
  if (accumulator) {
    mode_.accumulator8 = eight;
    mode_.accumulatorKnown = true;
  } else {
    mode_.index8 = eight;
    mode_.indexKnown = true;
  }
  return true;
}

Encoded Cpu65816Dialect::encode(std::string_view mnemonic, std::string_view operands, Address at,
                                const Evaluator& evaluator) {
  const auto found = forms_.find(std::string(mnemonic));
  if (found == forms_.end()) {
    return Encoded{.bytes = {}, .error = "`" + std::string(mnemonic) + "` is not a 65816 instruction"};
  }
  const Operand operand = parseOperand(compact(operands));
  if (!operand.error.empty()) return Encoded{.bytes = {}, .error = operand.error};

  // Which addressing mode the written shape is, for this mnemonic. A bare
  // expression is the target of a branch or a `PER`, the value of a `PEA`, a
  // long operand when it names a bank, and a direct-page offset otherwise.
  const std::vector<const Cpu65816Opcode*>& rows = found->second;
  auto has = [&](Cpu65816Addressing mode) -> const Cpu65816Opcode* {
    for (const Cpu65816Opcode* row : rows) {
      if (row->mode == mode) return row;
    }
    return nullptr;
  };
  auto firstOf = [&](std::initializer_list<Cpu65816Addressing> modes) -> Cpu65816Addressing {
    for (Cpu65816Addressing mode : modes) {
      if (has(mode)) return mode;
    }
    return *modes.begin();
  };
  Cpu65816Addressing mode = Cpu65816Addressing::Implied;
  switch (operand.shape) {
    case Shape::Implied: mode = Cpu65816Addressing::Implied; break;
    case Shape::Accumulator: mode = Cpu65816Addressing::Accumulator; break;
    case Shape::Immediate:
      mode = firstOf({Cpu65816Addressing::ImmediateM, Cpu65816Addressing::ImmediateX,
                      Cpu65816Addressing::ImmediateByte});
      break;
    case Shape::Bare:
      if (has(Cpu65816Addressing::Relative) || has(Cpu65816Addressing::RelativeLong) ||
          has(Cpu65816Addressing::PushRelative) || has(Cpu65816Addressing::PushAbsolute)) {
        mode = firstOf({Cpu65816Addressing::Relative, Cpu65816Addressing::RelativeLong,
                        Cpu65816Addressing::PushRelative, Cpu65816Addressing::PushAbsolute});
      } else {
        mode = operand.longMarked ? Cpu65816Addressing::AbsoluteLong : Cpu65816Addressing::Direct;
      }
      break;
    case Shape::IndexX:
      mode = operand.longMarked ? Cpu65816Addressing::AbsoluteLongX : Cpu65816Addressing::DirectX;
      break;
    case Shape::IndexY: mode = Cpu65816Addressing::DirectY; break;
    case Shape::IndexS: mode = Cpu65816Addressing::StackRelative; break;
    case Shape::Absolute: mode = Cpu65816Addressing::Absolute; break;
    case Shape::AbsoluteX: mode = Cpu65816Addressing::AbsoluteX; break;
    case Shape::AbsoluteY: mode = Cpu65816Addressing::AbsoluteY; break;
    case Shape::Indirect: mode = Cpu65816Addressing::DirectIndirect; break;
    case Shape::IndirectX: mode = Cpu65816Addressing::DirectIndirectX; break;
    case Shape::IndirectY: mode = Cpu65816Addressing::DirectIndirectY; break;
    case Shape::StackIndirectY: mode = Cpu65816Addressing::StackRelativeY; break;
    case Shape::AbsoluteIndirect: mode = Cpu65816Addressing::AbsoluteIndirect; break;
    case Shape::AbsoluteIndexedIndirect: mode = Cpu65816Addressing::AbsoluteIndexedIndirect; break;
    case Shape::IndirectLong: mode = Cpu65816Addressing::DirectIndirectLong; break;
    case Shape::IndirectLongY: mode = Cpu65816Addressing::DirectIndirectLongY; break;
    case Shape::AbsoluteIndirectLong: mode = Cpu65816Addressing::AbsoluteIndirectLong; break;
    case Shape::BlockMove: mode = Cpu65816Addressing::BlockMove; break;
  }
  const Cpu65816Opcode* row = has(mode);
  if (!row) {
    return Encoded{.bytes = {},
                   .error = std::string(mnemonic) + " has no `" + std::string(syntaxOf(mode)) +
                            "` form" + (operand.longMarked && mode == Cpu65816Addressing::AbsoluteLong
                                            ? "; it does not take a long operand"
                                            : "")};
  }

  // The operand's value. Unresolved on the first pass means zero, unchecked.
  Encoded out;
  out.bytes.push_back(row->opcode);
  bool unresolved = false;
  auto value = [&](const std::string& text) -> std::optional<std::uint32_t> {
    std::string error;
    const std::optional<Value> v = evaluator.evaluate(text, error);
    if (!v) {
      out.error = error;
      return std::nullopt;
    }
    if (!v->resolved) unresolved = true;
    return v->value;
  };
  auto emitBytes = [&](std::uint32_t v, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out.bytes.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  };
  auto emitFitting = [&](const std::string& text, unsigned bits, const char* what) {
    const std::optional<std::uint32_t> v = value(text);
    if (!v) return;
    if (!unresolved && !fits(*v, bits)) {
      out.error = std::string(what) + " " + hex(*v, 6) + " does not fit in " +
                  (bits == 8 ? "a byte" : bits == 16 ? "16 bits" : "24 bits");
      return;
    }
    emitBytes(*v, bits / 8);
  };
  // A displacement from the end of the instruction to a target in the same bank.
  auto emitDisplacement = [&](const std::string& text, unsigned length, bool eightBit) {
    const std::optional<std::uint32_t> target = value(text);
    if (!target) return;
    if (unresolved) {
      emitBytes(0, eightBit ? 1 : 2);
      return;
    }
    const Address bank = at & 0xFF0000u;
    if ((*target & 0xFF0000u) != bank) {
      out.error = "the target " + disasm::formatAddress(*target, 24) +
                  " is in another bank; a branch stays within its own";
      return;
    }
    const Address after = bank | ((at + length) & 0xFFFFu);
    int delta = static_cast<int>((*target - after) & 0xFFFFu);
    if (delta > 0x7FFF) delta -= 0x10000;
    if (eightBit && (delta < -128 || delta > 127)) {
      out.error = "the target " + disasm::formatAddress(*target, 24) + " is " +
                  std::to_string(delta) +
                  " bytes from the end of the instruction; a branch reaches -128 to +127";
      return;
    }
    emitBytes(static_cast<std::uint32_t>(delta), eightBit ? 1 : 2);
  };

  const bool emulation = mode_.emulation;
  const bool accumulator8 = emulation || mode_.accumulator8;
  const bool index8 = emulation || mode_.index8;
  const std::uint8_t length =
      static_cast<std::uint8_t>(1 + disasm::cpu65816OperandBytes(mode, accumulator8, index8));

  switch (mode) {
    case Cpu65816Addressing::Implied:
    case Cpu65816Addressing::Accumulator:
      break;
    case Cpu65816Addressing::ImmediateM:
      if (!emulation && !mode_.accumulatorKnown) {
        out.error = std::string(mnemonic) + " # under an accumulator width nothing has said; "
                    "A8 or A16 says it, or a REP or SEP above";
        break;
      }
      emitFitting(operand.first, accumulator8 ? 8 : 16, "the immediate");
      break;
    case Cpu65816Addressing::ImmediateX:
      if (!emulation && !mode_.indexKnown) {
        out.error = std::string(mnemonic) + " # under an index width nothing has said; "
                    "X8 or X16 says it, or a REP or SEP above";
        break;
      }
      emitFitting(operand.first, index8 ? 8 : 16, "the immediate");
      break;
    case Cpu65816Addressing::ImmediateByte:
      emitFitting(operand.first, 8, "the immediate");
      break;
    case Cpu65816Addressing::Direct:
    case Cpu65816Addressing::DirectX:
    case Cpu65816Addressing::DirectY:
    case Cpu65816Addressing::DirectIndirect:
    case Cpu65816Addressing::DirectIndirectX:
    case Cpu65816Addressing::DirectIndirectY:
    case Cpu65816Addressing::DirectIndirectLong:
    case Cpu65816Addressing::DirectIndirectLongY:
      emitFitting(operand.first, 8, "the direct-page offset");
      break;
    case Cpu65816Addressing::StackRelative:
    case Cpu65816Addressing::StackRelativeY:
      emitFitting(operand.first, 8, "the stack offset");
      break;
    case Cpu65816Addressing::Absolute:
    case Cpu65816Addressing::AbsoluteX:
    case Cpu65816Addressing::AbsoluteY:
    case Cpu65816Addressing::AbsoluteIndirect:
    case Cpu65816Addressing::AbsoluteIndirectLong:
    case Cpu65816Addressing::AbsoluteIndexedIndirect: {
      // An absolute operand is an offset in a bank, so a 24-bit value in the
      // instruction's own bank — a label in the same file — names its offset.
      // A value in any other bank does not fit and is reported.
      const std::optional<std::uint32_t> v = value(operand.first);
      if (!v) break;
      const bool sameBank = (*v >> 16) == (at >> 16);
      if (!unresolved && !fits(*v, 16) && !sameBank) {
        out.error = "the address " + hex(*v, 6) + " does not fit in 16 bits and is not in bank " +
                    hex(at >> 16, 2);
        break;
      }
      emitBytes(*v & 0xFFFFu, 2);
      break;
    }
    case Cpu65816Addressing::PushAbsolute:
      emitFitting(operand.first, 16, "the address");
      break;
    case Cpu65816Addressing::AbsoluteLong:
    case Cpu65816Addressing::AbsoluteLongX:
      emitFitting(operand.first, 24, "the address");
      break;
    case Cpu65816Addressing::Relative:
      emitDisplacement(operand.first, length, true);
      break;
    case Cpu65816Addressing::RelativeLong:
    case Cpu65816Addressing::PushRelative:
      emitDisplacement(operand.first, length, false);
      break;
    case Cpu65816Addressing::BlockMove: {
      // Written source first; stored destination first.
      const std::optional<std::uint32_t> source = value(operand.first);
      const std::optional<std::uint32_t> destination = value(operand.second);
      if (!source || !destination) break;
      if (!unresolved && (!fits(*source, 8) || !fits(*destination, 8))) {
        out.error = "a block move names two banks, each one byte";
        break;
      }
      out.bytes.push_back(static_cast<std::uint8_t>(*destination & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>(*source & 0xFFu));
      break;
    }
  }
  if (!out.error.empty()) {
    out.bytes.resize(length);
    return out;
  }

  // The mode the next instruction assembles under. REP and SEP move it by their
  // mask, which therefore has to be known on the first pass.
  if ((row->opcode == 0xC2 || row->opcode == 0xE2) && unresolved) {
    out.error = "the mask of " + std::string(mnemonic) + " must be known when it is read; "
                "it decides the width of what follows";
    return out;
  }
  std::string note;
  const std::uint8_t first = out.bytes.size() > 1 ? out.bytes[1] : std::uint8_t{0};
  mode_ = disasm::cpu65816ModeAfter(row->opcode, first, mode_, note);
  return out;
}

Assembly assembleCpu65816(std::string_view source, std::string_view file) {
  Cpu65816Dialect dialect;
  return assemble(dialect, source, file);
}

}  // namespace snaggletooth::assembler
