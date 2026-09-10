#pragma once

// SNES assembly from the instruction layer.
//
// A node's mnemonic and addressing mode name one opcode, and its operand with
// its length names the operand bytes, so everything an assembler needs is in the
// instruction layer: this file writes the source line from it, and can write the
// bytes back too, without a byte ever having been held. The register widths a
// region's source carries from line to line are followed through the same
// function the assembler and the disassembler follow them through, so a
// directive lands exactly where the assembler needs one.
//
// A line may be written with names in place of addresses — a label for the
// target of a branch, jump or call; a hardware register's name for an absolute
// operand — and the names are the caller's: what a label is, and which register
// an address reaches, are facts attached to the program, not part of it.
//
// The sound program's lines are written the same way from a sound-CPU node: its
// mnemonic and form name the SPC700 table's row, whose text is filled with the
// operands as the SPC700 disassembler prints them, and the bytes come back from
// the same row.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816_disasm.h"
#include "ir/ir.h"
#include "spc700_disasm.h"

namespace snaggletooth::ir {

// The opcode the mnemonic and the addressing mode name together, from the
// backend's table.
[[nodiscard]] std::uint8_t opcodeOf(const Instruction& instruction);

// The bytes the instruction assembles to: the opcode, then the operand bytes its
// length says it has, in the order the chip reads them.
[[nodiscard]] std::vector<std::uint8_t> encode(const Instruction& instruction);

// The names a line is written with in place of addresses. Each is empty where
// the address is written.
struct SourceNames {
  std::string_view target;      // the label of the target, behind the marker its form needs
  std::string_view operand;     // the hardware register an absolute data operand addresses
  std::string_view annotation;  // text the trailing comment carries after the cost
};

// The instruction as source: the mnemonic and the operand as the dialect writes
// them. With no names it is the text the disassembler's listing carries.
[[nodiscard]] std::string renderInstruction(const Instruction& instruction,
                                            const SourceNames& names = {});

// The cost as a listing prints it: the measured base under the node's mode, with
// `/taken` for a conditional branch, or `?` where a width the trace did not know
// decides it.
[[nodiscard]] std::string renderCost(const Node& node);

// One line of source: the instruction under the indent, then the comment with
// the address, the bytes padded to `bytesWidth`, the cost, and the annotation,
// the register a long operand names, and `PATCHED at run time` where the node was
// lifted from patched bytes.
[[nodiscard]] std::string renderLine(const Node& node, const SourceNames& names,
                                     std::size_t bytesWidth);

// The mode a region of source carries from one instruction to the next, and the
// directives each instruction needs before it. A region begins with nothing
// carried — the assembler's own starting state — and `directives` answers for
// one instruction and then carries what it leaves, through `cpu65816ModeAfter`,
// to the next.
class SourceMode {
 public:
  // Back to the start of a region: at the start of a file, at an `ORG`, and
  // after a run of data.
  void reset() { left_.reset(); }

  // The lines the instruction needs before it, judged against what the line
  // above left; then what this one leaves is carried.
  [[nodiscard]] std::vector<std::string> directives(const Node& node);

 private:
  std::optional<disasm::Cpu65816Mode> left_;
};

// ---- the sound program ----------------------------------------------------------

// The opcode a sound-CPU node's mnemonic and form name together, from the
// SPC700 table; `std::logic_error` where they name none.
[[nodiscard]] std::uint8_t opcodeOfSpc700(const Instruction& instruction);

// The bytes a sound-CPU instruction assembles to: the opcode, then the operand
// bytes its form has, in the order the chip reads them — a relative form's
// displacement from the instruction after it, a bit form's address and bit
// index packed into one word.
[[nodiscard]] std::vector<std::uint8_t> encodeSpc700(const Instruction& instruction);

// A sound-CPU instruction as source: the table's text with the operands as the
// SPC700 disassembler prints them. With `targetLabel`, a branch, an absolute
// call or an absolute jump is written with the label in place of its target
// address, as the listing writes one; a `PCALL` keeps its byte.
[[nodiscard]] std::string renderSpc700Instruction(const Instruction& instruction,
                                                  std::string_view targetLabel = {});

// A sound-CPU node's cost as the listing prints it: the measured base, with
// `/taken` where a `Cycles` effect fires under a condition.
[[nodiscard]] std::string renderSpc700Cost(const Node& node);

// One line of the sound program's source: the instruction under the indent,
// then the comment with the 16-bit address, the bytes padded to `bytesWidth`,
// the cost, and the note — the register the operand names, the page-`$FF`
// address a `PCALL` reaches, and `PATCHED at run time` where the node was lifted
// from patched bytes.
[[nodiscard]] std::string renderSpc700Line(const Node& node, std::string_view targetLabel,
                                           std::size_t bytesWidth);

}  // namespace snaggletooth::ir
