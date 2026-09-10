#include "ir/ir_render.h"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace snaggletooth::ir {
namespace {

using disasm::Cpu65816Addressing;
using disasm::Cpu65816Mode;

std::string hex(std::uint32_t value, int digits) {
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "%0*X", digits, static_cast<unsigned>(value));
  return buffer;
}

std::string byte(std::uint32_t value) { return "$" + hex(value & 0xFFu, 2); }
std::string word(std::uint32_t value) { return "$" + hex(value & 0xFFFFu, 4); }
std::string longAddress(Address address) { return disasm::formatAddress(address, 24); }

// The backend's addressing mode for the representation's.
Cpu65816Addressing backendMode(Addressing addressing) {
  using M = Cpu65816Addressing;
  switch (addressing) {
    case Addressing::Implied: return M::Implied;
    case Addressing::Accumulator: return M::Accumulator;
    case Addressing::ImmediateM: return M::ImmediateM;
    case Addressing::ImmediateX: return M::ImmediateX;
    case Addressing::ImmediateByte: return M::ImmediateByte;
    case Addressing::Direct: return M::Direct;
    case Addressing::DirectX: return M::DirectX;
    case Addressing::DirectY: return M::DirectY;
    case Addressing::DirectIndirect: return M::DirectIndirect;
    case Addressing::DirectIndirectX: return M::DirectIndirectX;
    case Addressing::DirectIndirectY: return M::DirectIndirectY;
    case Addressing::DirectIndirectLong: return M::DirectIndirectLong;
    case Addressing::DirectIndirectLongY: return M::DirectIndirectLongY;
    case Addressing::StackRelative: return M::StackRelative;
    case Addressing::StackRelativeY: return M::StackRelativeY;
    case Addressing::Absolute: return M::Absolute;
    case Addressing::AbsoluteX: return M::AbsoluteX;
    case Addressing::AbsoluteY: return M::AbsoluteY;
    case Addressing::AbsoluteLong: return M::AbsoluteLong;
    case Addressing::AbsoluteLongX: return M::AbsoluteLongX;
    case Addressing::AbsoluteIndirect: return M::AbsoluteIndirect;
    case Addressing::AbsoluteIndirectLong: return M::AbsoluteIndirectLong;
    case Addressing::AbsoluteIndexedIndirect: return M::AbsoluteIndexedIndirect;
    case Addressing::Relative: return M::Relative;
    case Addressing::RelativeLong: return M::RelativeLong;
    case Addressing::BlockMove: return M::BlockMove;
    case Addressing::PushAbsolute: return M::PushAbsolute;
    case Addressing::PushRelative: return M::PushRelative;
  }
  return M::Implied;
}

// The table's opcode for each (mnemonic, mode) pair, built once from the table
// itself so the two cannot disagree.
std::uint8_t lookup(std::string_view mnemonic, Cpu65816Addressing mode) {
  const std::array<disasm::Cpu65816Opcode, 256>& table = disasm::cpu65816Opcodes();
  for (const disasm::Cpu65816Opcode& row : table) {
    if (row.mode == mode && mnemonic == row.mnemonic) return row.opcode;
  }
  throw std::logic_error("no opcode is " + std::string(mnemonic) + " under that addressing mode");
}

// Where the instruction after this one begins: the program counter wraps within
// its bank.
Address following(const Instruction& instruction) {
  return (instruction.address & 0xFF0000u) |
         ((instruction.address + instruction.length) & 0xFFFFu);
}

// Whether the instruction leaves for its target rather than reading memory there.
bool leaves(const Instruction& instruction) {
  return instruction.flow == Flow::Jump || instruction.flow == Flow::Call;
}

// What precedes a target written as a symbol: the absolute forms' `!`, the long
// forms' `>`, nothing before a branch's.
std::string_view symbolMarker(const Instruction& instruction) {
  switch (instruction.addressing) {
    case Addressing::Absolute: return "!";
    case Addressing::AbsoluteLong: return ">";
    default: return "";
  }
}

void padTo(std::string& text, std::size_t column) {
  if (text.size() < column) {
    text.append(column - text.size(), ' ');
  } else {
    text += "  ";
  }
}

}  // namespace

std::uint8_t opcodeOf(const Instruction& instruction) {
  return lookup(instruction.mnemonic, backendMode(instruction.addressing));
}

std::vector<std::uint8_t> encode(const Instruction& instruction) {
  std::vector<std::uint8_t> bytes;
  bytes.push_back(opcodeOf(instruction));
  const std::uint32_t operand = instruction.operand;
  auto push = [&](std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  };
  switch (instruction.addressing) {
    case Addressing::Implied:
    case Addressing::Accumulator:
      break;
    case Addressing::ImmediateM:
    case Addressing::ImmediateX:
      push(operand, instruction.length - 1u);
      break;
    case Addressing::ImmediateByte:
    case Addressing::Direct:
    case Addressing::DirectX:
    case Addressing::DirectY:
    case Addressing::DirectIndirect:
    case Addressing::DirectIndirectX:
    case Addressing::DirectIndirectY:
    case Addressing::DirectIndirectLong:
    case Addressing::DirectIndirectLongY:
    case Addressing::StackRelative:
    case Addressing::StackRelativeY:
      push(operand, 1);
      break;
    case Addressing::Absolute:
    case Addressing::AbsoluteX:
    case Addressing::AbsoluteY:
    case Addressing::AbsoluteIndirect:
    case Addressing::AbsoluteIndirectLong:
    case Addressing::AbsoluteIndexedIndirect:
    case Addressing::PushAbsolute:
      push(operand, 2);
      break;
    case Addressing::AbsoluteLong:
    case Addressing::AbsoluteLongX:
      push(operand, 3);
      break;
    // A relative form carries the address it reaches; the byte is the
    // displacement from the instruction after it, within the bank.
    case Addressing::Relative:
      push((operand - following(instruction)) & 0xFFFFu, 1);
      break;
    case Addressing::RelativeLong:
    case Addressing::PushRelative:
      push((operand - following(instruction)) & 0xFFFFu, 2);
      break;
    // The chip reads the destination bank first and the source bank second.
    case Addressing::BlockMove:
      push(instruction.operand2, 1);
      push(operand, 1);
      break;
  }
  return bytes;
}

std::string renderInstruction(const Instruction& instruction, const SourceNames& names) {
  const std::uint32_t operand = instruction.operand;
  std::string text;
  // A target with a label is written as the label behind the marker its form
  // needs, whatever the form; the operand text below is for the address.
  if (instruction.target && !names.target.empty()) {
    return std::string(instruction.mnemonic) + " " + std::string(symbolMarker(instruction)) +
           std::string(names.target);
  }
  // A register's name stands for an absolute data operand's address.
  const bool named = !names.operand.empty() && !leaves(instruction);
  const std::string absolute = named ? std::string(names.operand) : word(operand);
  switch (instruction.addressing) {
    case Addressing::Implied: break;
    case Addressing::Accumulator: text = "A"; break;
    case Addressing::ImmediateM:
    case Addressing::ImmediateX:
      text = instruction.length == 2 ? "#" + byte(operand) : "#" + word(operand);
      break;
    case Addressing::ImmediateByte: text = "#" + byte(operand); break;
    case Addressing::Direct: text = byte(operand); break;
    case Addressing::DirectX: text = byte(operand) + ",X"; break;
    case Addressing::DirectY: text = byte(operand) + ",Y"; break;
    case Addressing::DirectIndirect: text = "(" + byte(operand) + ")"; break;
    case Addressing::DirectIndirectX: text = "(" + byte(operand) + ",X)"; break;
    case Addressing::DirectIndirectY: text = "(" + byte(operand) + "),Y"; break;
    case Addressing::DirectIndirectLong: text = "[" + byte(operand) + "]"; break;
    case Addressing::DirectIndirectLongY: text = "[" + byte(operand) + "],Y"; break;
    case Addressing::StackRelative: text = byte(operand) + ",S"; break;
    case Addressing::StackRelativeY: text = "(" + byte(operand) + ",S),Y"; break;
    case Addressing::Absolute: text = "!" + absolute; break;
    case Addressing::AbsoluteX: text = "!" + absolute + ",X"; break;
    case Addressing::AbsoluteY: text = "!" + absolute + ",Y"; break;
    case Addressing::AbsoluteLong: text = longAddress(operand); break;
    case Addressing::AbsoluteLongX: text = longAddress(operand) + ",X"; break;
    case Addressing::AbsoluteIndirect: text = "(!" + word(operand) + ")"; break;
    case Addressing::AbsoluteIndirectLong: text = "[!" + word(operand) + "]"; break;
    case Addressing::AbsoluteIndexedIndirect: text = "(!" + word(operand) + ",X)"; break;
    case Addressing::Relative:
    case Addressing::RelativeLong:
    case Addressing::PushRelative:
      text = longAddress(operand);
      break;
    case Addressing::PushAbsolute: text = word(operand); break;
    case Addressing::BlockMove: text = byte(operand) + "," + byte(instruction.operand2); break;
  }
  return text.empty() ? std::string(instruction.mnemonic)
                      : std::string(instruction.mnemonic) + " " + text;
}

std::string renderCost(const Node& node) {
  const Mode& mode = node.mode;
  // The base under every setting of the widths the mode allows; a cost is known
  // only when they agree. Emulation mode fixes both widths, and the lift measured
  // all four entries under it alike.
  std::optional<std::uint8_t> base;
  bool agree = true;
  for (int a = 0; a < 2 && agree; ++a) {
    const bool accumulator8 = mode.emulation || (mode.accumulatorKnown ? mode.accumulator8 : a == 0);
    if ((mode.emulation || mode.accumulatorKnown) && a == 1) break;
    for (int x = 0; x < 2; ++x) {
      const bool index8 = mode.emulation || (mode.indexKnown ? mode.index8 : x == 0);
      if ((mode.emulation || mode.indexKnown) && x == 1) break;
      const std::uint8_t measured = node.cost.base[costIndex(accumulator8, index8)];
      if (!base) {
        base = measured;
      } else if (*base != measured) {
        agree = false;
        break;
      }
    }
  }
  if (!agree || !base) return "?";
  // A conditional branch costs its base plus the cycles its condition adds when
  // it holds; the effect that adds them is conditioned on the flag the branch
  // tests.
  std::uint32_t taken = 0;
  for (const Effect& e : node.effects) {
    if (e.op != Op::Cycles) continue;
    if ((e.when.when == When::FlagSet || e.when.when == When::FlagClear) &&
        !e.when.andEmulation) {
      taken += e.a.value;
    }
  }
  std::string text = std::to_string(*base);
  if (taken != 0) text += "/" + std::to_string(*base + taken);
  return text;
}

std::string renderLine(const Node& node, const SourceNames& names, std::size_t bytesWidth) {
  // Where the trailing comment starts: the same column the listing uses.
  constexpr std::size_t kCommentColumn = 40;
  std::string row = "        " + renderInstruction(node.instruction, names);
  padTo(row, kCommentColumn);

  std::string bytes;
  for (const std::uint8_t b : encode(node.instruction)) bytes += hex(b, 2) + " ";
  if (bytes.size() < bytesWidth) bytes.append(bytesWidth - bytes.size(), ' ');

  row += "; " + longAddress(node.instruction.address) + "  " + bytes + " " + renderCost(node);
  std::string note(names.annotation);
  if (!node.registerName.empty() && names.operand.empty()) {
    if (!note.empty()) note += "; ";
    note += node.registerName;
  }
  if (node.patched) {
    if (!note.empty()) note += "; ";
    note += "PATCHED at run time";
  }
  if (!note.empty()) row += "  " + note;
  return row + "\n";
}

// ---- the sound program ----------------------------------------------------------

namespace {

using disasm::Spc700Operands;

// A table row's text with `%1` and `%2` replaced by the operands rendered for
// it, as the SPC700 disassembler fills it.
std::string fillSlots(std::string_view text, const std::string& first, const std::string& second) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 1 < text.size() && (text[i + 1] == '1' || text[i + 1] == '2')) {
      out += text[i + 1] == '1' ? first : second;
      ++i;
      continue;
    }
    out += text[i];
  }
  return out;
}

// The operand slots as the SPC700 disassembler prints them: a byte or a word, a
// relative form's target, a bit form's address and bit index.
void spc700Slots(const Instruction& instruction, Spc700Operands shape, std::string& first,
                 std::string& second) {
  switch (shape) {
    case Spc700Operands::None: break;
    case Spc700Operands::Imm:
    case Spc700Operands::Dp:
    case Spc700Operands::Upage:
      first = hex(instruction.operand & 0xFFu, 2);
      break;
    case Spc700Operands::Abs:
      first = hex(instruction.operand & 0xFFFFu, 4);
      break;
    case Spc700Operands::AbsBit:
      first = hex(instruction.operand & 0x1FFFu, 4);
      second = std::to_string(instruction.operand2);
      break;
    case Spc700Operands::Rel:
      first = hex(instruction.target.value_or(instruction.operand) & 0xFFFFu, 4);
      break;
    case Spc700Operands::DpRel:
      first = hex(instruction.operand & 0xFFu, 2);
      second = hex(instruction.target.value_or(0) & 0xFFFFu, 4);
      break;
    case Spc700Operands::DpDp:
    case Spc700Operands::ImmDp:
      first = hex(instruction.operand & 0xFFu, 2);
      second = hex(instruction.operand2, 2);
      break;
  }
}

// The displacement a relative form's byte carries: from the instruction after
// it to the target, within the audio unit's space.
std::uint8_t spc700Displacement(const Instruction& instruction) {
  const std::uint32_t after = (instruction.address + instruction.length) & 0xFFFFu;
  return static_cast<std::uint8_t>((instruction.target.value_or(instruction.operand) - after) & 0xFFu);
}

}  // namespace

std::uint8_t opcodeOfSpc700(const Instruction& instruction) {
  const std::optional<std::uint8_t> opcode =
      disasm::spc700OpcodeOf(instruction.mnemonic, instruction.form);
  if (!opcode) {
    throw std::logic_error("no opcode of the sound CPU is " + std::string(instruction.mnemonic) +
                           " with " + std::string(instruction.form));
  }
  return *opcode;
}

std::vector<std::uint8_t> encodeSpc700(const Instruction& instruction) {
  const std::uint8_t opcode = opcodeOfSpc700(instruction);
  std::vector<std::uint8_t> bytes{opcode};
  auto byte = [](std::uint32_t value) { return static_cast<std::uint8_t>(value & 0xFFu); };
  switch (disasm::spc700Opcodes()[opcode].operands) {
    case Spc700Operands::None: break;
    case Spc700Operands::Imm:
    case Spc700Operands::Dp:
    case Spc700Operands::Upage:
      bytes.push_back(byte(instruction.operand));
      break;
    case Spc700Operands::Abs:
      bytes.push_back(byte(instruction.operand));
      bytes.push_back(byte(instruction.operand >> 8));
      break;
    case Spc700Operands::AbsBit: {
      const std::uint32_t word = (instruction.operand & 0x1FFFu) | (static_cast<std::uint32_t>(instruction.operand2) << 13);
      bytes.push_back(byte(word));
      bytes.push_back(byte(word >> 8));
      break;
    }
    case Spc700Operands::Rel:
      bytes.push_back(spc700Displacement(instruction));
      break;
    case Spc700Operands::DpRel:
      bytes.push_back(byte(instruction.operand));
      bytes.push_back(spc700Displacement(instruction));
      break;
    case Spc700Operands::DpDp:
    case Spc700Operands::ImmDp:
      bytes.push_back(byte(instruction.operand));
      bytes.push_back(instruction.operand2);
      break;
  }
  return bytes;
}

std::string renderSpc700Instruction(const Instruction& instruction, std::string_view targetLabel) {
  const disasm::Spc700Opcode& row = disasm::spc700Opcodes()[opcodeOfSpc700(instruction)];
  std::string first;
  std::string second;
  spc700Slots(instruction, row.operands, first, second);
  const std::string_view text(row.text);
  // The forms whose target the dialect writes as a symbol: an absolute call or
  // jump, and the branches, where the target is the slot printed as `$%1` or
  // `$%2`. The symbol replaces the `$` and the digits together.
  const bool symbolic = (row.operands == Spc700Operands::Abs && instruction.target) ||
                        row.operands == Spc700Operands::Rel || row.operands == Spc700Operands::DpRel;
  if (symbolic && !targetLabel.empty()) {
    const std::string_view slot = row.operands == Spc700Operands::DpRel ? "$%2" : "$%1";
    const std::size_t at = text.find(slot);
    return fillSlots(text.substr(0, at), first, second) + std::string(targetLabel) +
           fillSlots(text.substr(at + slot.size()), first, second);
  }
  return fillSlots(text, first, second);
}

std::string renderSpc700Cost(const Node& node) {
  // One measured cost; a taken branch adds the cycles of the `Cycles` effect
  // that fires under its condition.
  std::uint32_t taken = 0;
  for (const Effect& e : node.effects) {
    if (e.op == Op::Cycles && e.when.when != When::Always) taken += e.a.value;
  }
  std::string text = std::to_string(node.cost.base[0]);
  if (taken != 0) text += "/" + std::to_string(node.cost.base[0] + taken);
  return text;
}

std::string renderSpc700Line(const Node& node, std::string_view targetLabel, std::size_t bytesWidth) {
  constexpr std::size_t kCommentColumn = 40;
  std::string row = "        " + renderSpc700Instruction(node.instruction, targetLabel);
  padTo(row, kCommentColumn);

  std::string bytes;
  for (const std::uint8_t b : encodeSpc700(node.instruction)) bytes += hex(b, 2) + " ";
  if (bytes.size() < bytesWidth) bytes.append(bytesWidth - bytes.size(), ' ');

  row += "; " + disasm::formatAddress(node.instruction.address & 0xFFFFu, 16) + "  " + bytes + " " +
         renderSpc700Cost(node);
  // The note: the register the operand names, or the address a `PCALL` reaches,
  // whose operand is an offset into page $FF.
  std::string note(node.registerName);
  if (note.empty() && node.instruction.form == "upage" && node.instruction.target) {
    note = "$" + hex(*node.instruction.target & 0xFFFFu, 4);
  }
  if (node.patched) {
    if (!note.empty()) note += "; ";
    note += "PATCHED at run time";
  }
  if (!note.empty()) row += "  " + note;
  return row + "\n";
}

std::vector<std::string> SourceMode::directives(const Node& node) {
  // The node's mode, as the backend carries one: what it reads under, with the
  // carry memory the line above left, which is what `XCE` exchanges.
  Cpu65816Mode now;
  now.emulation = node.mode.emulation;
  now.accumulator8 = node.mode.accumulator8;
  now.index8 = node.mode.index8;
  now.accumulatorKnown = node.mode.accumulatorKnown;
  now.indexKnown = node.mode.indexKnown;
  now.carryKnown = left_ ? left_->carryKnown : false;
  now.carry = left_ ? left_->carry : false;

  std::optional<disasm::Context> before;
  if (left_) before = disasm::contextOf(*left_);
  std::vector<std::string> out =
      disasm::cpu65816Backend().directives(before, disasm::contextOf(now));

  std::string note;
  left_ = disasm::cpu65816ModeAfter(opcodeOf(node.instruction),
                                    static_cast<std::uint8_t>(node.instruction.operand & 0xFFu),
                                    now, note);
  return out;
}

}  // namespace snaggletooth::ir
