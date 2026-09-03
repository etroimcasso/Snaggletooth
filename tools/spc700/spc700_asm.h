#pragma once

// The SPC700 assembler — the SPC700 dialect over the assembler
// (`assembler/assembler.h`), and the call that assembles SPC700 source without
// naming the dialect.
//
// The dialect is the inverse of the SPC700 disassembler's rendering, built from
// the same instruction table: each row's text, with its operand slots, is the
// form the assembler matches a line against. Every instruction has a fixed
// length, so the dialect carries nothing along a file and has no directives of
// its own.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "assembler/assembler.h"
#include "spc700_disasm.h"

namespace snaggletooth::assembler {

// The SPC700 dialect: 16-bit addresses, the instruction table's forms.
class Spc700Dialect final : public Dialect {
 public:
  Spc700Dialect();

  [[nodiscard]] std::string_view name() const override { return "SPC700"; }
  [[nodiscard]] unsigned addressBits() const override { return 16; }
  [[nodiscard]] bool reserved(std::string_view upperName) const override;
  void beginRegion() override {}
  bool directive(std::string_view upperName, std::string_view operands,
                 const Evaluator& evaluator, std::string& error) override;
  [[nodiscard]] Encoded encode(std::string_view mnemonic, std::string_view operands, Address at,
                               const Evaluator& evaluator) override;

 private:
  // One piece of a form's operand syntax: text to match, or a slot where an
  // expression goes, numbered as the table's `%1` and `%2`.
  struct Segment {
    bool slot = false;
    std::string literal;  // upper-cased, when not a slot
    unsigned index = 0;   // 1 or 2, when a slot
  };
  // One form of a mnemonic: the row it encodes and its operand syntax in pieces.
  struct Form {
    const disasm::Spc700Opcode* row = nullptr;
    std::vector<Segment> segments;
  };

  std::map<std::string, std::vector<Form>> forms_;
};

// Assembles SPC700 source. `file` names it in diagnostics.
[[nodiscard]] Assembly assembleSpc700(std::string_view source, std::string_view file = "");

}  // namespace snaggletooth::assembler
