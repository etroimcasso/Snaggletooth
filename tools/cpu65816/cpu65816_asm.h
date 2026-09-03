#pragma once

// The 65816 assembler — the 65816 dialect over the assembler
// (`assembler/assembler.h`), and the call that assembles 65816 source without
// naming the dialect.
//
// The dialect is the inverse of the 65816 disassembler's rendering, built from
// the same instruction table, and it follows the register widths the same way:
// through `cpu65816ModeAfter`, the one function both tools move the mode with.
// An immediate is as wide as the register it loads, so the dialect carries the
// mode along the file — `REP` and `SEP` move it, `XCE` after `CLC` or `SEC`
// changes it, `PLP` and `RTI` make the widths unknown — and the directives `A8`,
// `A16`, `X8`, `X16`, `EMULATION` and `NATIVE` say what the instructions cannot.
// A region begins in native mode with both widths unknown, and an immediate
// assembled under an unknown width is an error, never a guess.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "assembler/assembler.h"
#include "cpu65816_disasm.h"

namespace snaggletooth::assembler {

// The 65816 dialect: 24-bit addresses, the instruction table's forms, and the
// mode carried along the file.
class Cpu65816Dialect final : public Dialect {
 public:
  Cpu65816Dialect();

  [[nodiscard]] std::string_view name() const override { return "65816"; }
  [[nodiscard]] unsigned addressBits() const override { return 24; }
  [[nodiscard]] bool reserved(std::string_view upperName) const override;
  void beginRegion() override;
  bool directive(std::string_view upperName, std::string_view operands,
                 const Evaluator& evaluator, std::string& error) override;
  [[nodiscard]] Encoded encode(std::string_view mnemonic, std::string_view operands, Address at,
                               const Evaluator& evaluator) override;

  // The mode the next instruction assembles under.
  [[nodiscard]] const disasm::Cpu65816Mode& mode() const { return mode_; }

 private:
  std::map<std::string, std::vector<const disasm::Cpu65816Opcode*>> forms_;
  disasm::Cpu65816Mode mode_ = disasm::Cpu65816Mode::nativeUnknown();
};

// Assembles 65816 source. `file` names it in diagnostics.
[[nodiscard]] Assembly assembleCpu65816(std::string_view source, std::string_view file = "");

}  // namespace snaggletooth::assembler
