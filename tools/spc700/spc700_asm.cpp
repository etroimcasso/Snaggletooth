#include "spc700_asm.h"

#include <optional>
#include <utility>

namespace snaggletooth::assembler {
namespace {

using disasm::Spc700Operands;

constexpr char toUpper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// The register names the dialect's syntax uses, which a label may not spell.
constexpr std::array<std::string_view, 7> kRegisters = {"A", "X", "Y", "SP", "PSW", "YA", "C"};

// A displacement from `after` to `target`, as the byte holds it, or nothing when
// the target is out of reach. Addresses wrap at 16 bits.
std::optional<std::uint8_t> displacement(std::uint32_t after, std::uint32_t target) {
  int delta = static_cast<int>((target - after) & 0xFFFFu);
  if (delta > 0x7FFF) delta -= 0x10000;
  if (delta < -128 || delta > 127) return std::nullopt;
  return static_cast<std::uint8_t>(delta & 0xFF);
}

}  // namespace

Spc700Dialect::Spc700Dialect() {
  // Every row of the table is one form of its mnemonic: the text before the
  // first space names the mnemonic, the rest is the operand syntax with `$%1`
  // and `$%2` (or bare `%2`) where an expression goes. The `$` before a slot is
  // part of how a number prints, not of the syntax, so it is dropped.
  for (const disasm::Spc700Opcode& row : disasm::spc700Opcodes()) {
    const std::string_view text = row.text;
    const std::size_t space = text.find(' ');
    const std::string mnemonic = upper(text.substr(0, space));
    Form form;
    form.row = &row;
    if (space != std::string_view::npos) {
      const std::string_view syntax = text.substr(space + 1);
      Segment literal;
      auto flush = [&]() {
        if (!literal.literal.empty()) form.segments.push_back(literal);
        literal = Segment{};
      };
      for (std::size_t i = 0; i < syntax.size(); ++i) {
        const char c = syntax[i];
        if (c == '$' && i + 1 < syntax.size() && syntax[i + 1] == '%') continue;
        if (c == '%' && i + 1 < syntax.size() && (syntax[i + 1] == '1' || syntax[i + 1] == '2')) {
          flush();
          form.segments.push_back(Segment{.slot = true, .literal = {},
                                          .index = static_cast<unsigned>(syntax[i + 1] - '0')});
          ++i;
          continue;
        }
        literal.literal += toUpper(c);
      }
      flush();
    }
    forms_[mnemonic].push_back(std::move(form));
  }
}

bool Spc700Dialect::reserved(std::string_view upperName) const {
  if (forms_.count(std::string(upperName)) != 0) return true;
  for (std::string_view reg : kRegisters) {
    if (reg == upperName) return true;
  }
  return false;
}

bool Spc700Dialect::directive(std::string_view, std::string_view, const Evaluator&,
                              std::string&) {
  return false;
}

Encoded Spc700Dialect::encode(std::string_view mnemonic, std::string_view operands, Address at,
                              const Evaluator& evaluator) {
  const auto found = forms_.find(std::string(mnemonic));
  if (found == forms_.end()) {
    return Encoded{.bytes = {}, .error = "`" + std::string(mnemonic) + "` is not an SPC700 instruction"};
  }
  const std::string text = compact(operands);

  // Matches the operand against one form: each literal piece character by
  // character, case aside; each slot as an expression, which ends where the
  // expression grammar ends — or, before a `.bit` piece, at the last `.` before
  // the next comma, since `.` is also a character a name may contain.
  auto matchForm = [&](const Form& form) -> std::optional<std::array<std::string, 2>> {
    std::array<std::string, 2> slots;
    std::size_t pos = 0;
    for (std::size_t s = 0; s < form.segments.size(); ++s) {
      const Segment& segment = form.segments[s];
      if (!segment.slot) {
        if (text.size() - pos < segment.literal.size()) return std::nullopt;
        for (std::size_t i = 0; i < segment.literal.size(); ++i) {
          if (toUpper(text[pos + i]) != segment.literal[i]) return std::nullopt;
        }
        pos += segment.literal.size();
        continue;
      }
      std::size_t end = pos;
      const bool bitFollows = s + 1 < form.segments.size() && !form.segments[s + 1].slot &&
                              !form.segments[s + 1].literal.empty() &&
                              form.segments[s + 1].literal.front() == '.';
      if (bitFollows) {
        std::size_t stop = text.find(',', pos);
        if (stop == std::string::npos) stop = text.size();
        const std::size_t dot = text.rfind('.', stop == 0 ? 0 : stop - 1);
        if (dot == std::string::npos || dot <= pos) return std::nullopt;
        end = dot;
      } else {
        end = expressionEnd(text, pos);
        if (end == pos) return std::nullopt;
      }
      // A register name is syntax, never a term, so `MOV A,$10` is not the
      // two-operand form with `A` as an offset.
      const std::string slot = text.substr(pos, end - pos);
      const std::string upperSlot = upper(slot);
      for (std::string_view reg : kRegisters) {
        if (reg == upperSlot) return std::nullopt;
      }
      slots[segment.index - 1] = slot;
      pos = end;
    }
    if (pos != text.size()) return std::nullopt;
    return slots;
  };

  const Form* matched = nullptr;
  std::array<std::string, 2> slots;
  std::string alsoMatched;
  for (const Form& form : found->second) {
    std::optional<std::array<std::string, 2>> match = matchForm(form);
    if (!match) continue;
    if (matched) {
      alsoMatched = form.row->text;
      break;
    }
    matched = &form;
    slots = *match;
  }
  if (!matched) {
    return Encoded{.bytes = {},
                   .error = "`" + std::string(mnemonic) + " " + std::string(operands) +
                            "` is not a form of " + std::string(mnemonic)};
  }
  if (!alsoMatched.empty()) {
    return Encoded{.bytes = {},
                   .error = "`" + std::string(mnemonic) + " " + std::string(operands) +
                            "` could be `" + matched->row->text + "` or `" + alsoMatched + "`"};
  }

  // The slots' values. A value unresolved on the first pass is zero and is not
  // checked; the bytes are still the right length.
  const disasm::Spc700Opcode& row = *matched->row;
  Encoded out;
  out.bytes.push_back(row.opcode);
  bool unresolved = false;
  auto value = [&](unsigned index) -> std::optional<std::uint32_t> {
    std::string error;
    const std::optional<Value> v = evaluator.evaluate(slots[index - 1], error);
    if (!v) {
      out.error = error;
      return std::nullopt;
    }
    if (!v->resolved) unresolved = true;
    return v->value;
  };
  auto byte = [&](unsigned index, const char* what) -> std::optional<std::uint8_t> {
    const std::optional<std::uint32_t> v = value(index);
    if (!v) return std::nullopt;
    if (!unresolved && !fits(*v, 8)) {
      out.error = std::string(what) + " " + hex(*v, 4) + " does not fit in a byte";
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(*v & 0xFFu);
  };
  auto word = [&](unsigned index) -> std::optional<std::uint16_t> {
    const std::optional<std::uint32_t> v = value(index);
    if (!v) return std::nullopt;
    return static_cast<std::uint16_t>(*v & 0xFFFFu);
  };
  auto reach = [&](unsigned index, std::uint32_t after) -> std::optional<std::uint8_t> {
    const std::optional<std::uint32_t> target = value(index);
    if (!target) return std::nullopt;
    if (unresolved) return std::uint8_t{0};
    const std::optional<std::uint8_t> disp = displacement(after, *target);
    if (!disp) {
      int delta = static_cast<int>((*target - after) & 0xFFFFu);
      if (delta > 0x7FFF) delta -= 0x10000;
      out.error = "the target " + hex(*target, 4) + " is " + std::to_string(delta) +
                  " bytes from the end of the instruction; a branch reaches -128 to +127";
      return std::nullopt;
    }
    return *disp;
  };

  const std::uint8_t length = static_cast<std::uint8_t>(1 + disasm::spc700OperandBytes(row.operands));
  switch (row.operands) {
    case Spc700Operands::None:
      break;
    case Spc700Operands::Imm:
    case Spc700Operands::Dp:
    case Spc700Operands::Upage: {
      const std::optional<std::uint8_t> b = byte(1, row.operands == Spc700Operands::Imm ? "the immediate" : "the offset");
      if (!b) break;
      out.bytes.push_back(*b);
      break;
    }
    case Spc700Operands::Abs: {
      const std::optional<std::uint16_t> w = word(1);
      if (!w) break;
      out.bytes.push_back(static_cast<std::uint8_t>(*w & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>(*w >> 8));
      break;
    }
    case Spc700Operands::AbsBit: {
      const std::optional<std::uint32_t> address = value(1);
      const std::optional<std::uint32_t> bit = value(2);
      if (!address || !bit) break;
      if (!unresolved && address.value() > 0x1FFFu) {
        out.error = "the address of a bit operand must fit in 13 bits, $0000-$1FFF; " +
                    hex(*address, 4) + " does not";
        break;
      }
      if (!unresolved && bit.value() > 7u) {
        out.error = "the bit index is 0 to 7; " + std::to_string(*bit) + " is not";
        break;
      }
      const std::uint16_t w = static_cast<std::uint16_t>((*address & 0x1FFFu) | ((*bit & 7u) << 13));
      out.bytes.push_back(static_cast<std::uint8_t>(w & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>(w >> 8));
      break;
    }
    case Spc700Operands::Rel: {
      const std::optional<std::uint8_t> d = reach(1, (at + length) & 0xFFFFu);
      if (!d) break;
      out.bytes.push_back(*d);
      break;
    }
    case Spc700Operands::DpRel: {
      const std::optional<std::uint8_t> dp = byte(1, "the offset");
      if (!dp) break;
      const std::optional<std::uint8_t> d = reach(2, (at + length) & 0xFFFFu);
      if (!d) break;
      out.bytes.push_back(*dp);
      out.bytes.push_back(*d);
      break;
    }
    case Spc700Operands::DpDp:
    case Spc700Operands::ImmDp: {
      const std::optional<std::uint8_t> first =
          byte(1, row.operands == Spc700Operands::ImmDp ? "the immediate" : "the source offset");
      if (!first) break;
      const std::optional<std::uint8_t> second = byte(2, "the destination offset");
      if (!second) break;
      out.bytes.push_back(*first);
      out.bytes.push_back(*second);
      break;
    }
  }
  if (!out.error.empty()) out.bytes.resize(length);
  return out;
}

Assembly assembleSpc700(std::string_view source, std::string_view file) {
  Spc700Dialect dialect;
  return assemble(dialect, source, file);
}

}  // namespace snaggletooth::assembler
