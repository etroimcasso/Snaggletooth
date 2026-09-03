#pragma once

// The SPC700 disassembler — the SPC700 backend over the disassembly framework
// (`disasm/disasm.h`), and the convenience calls that disassemble a block of
// SPC700 memory without naming the backend.
//
// Its cycle counts come from the interpreter, not from a table beside it. Each
// opcode's cost is measured by running the core over a synthetic bus, so the
// listing and the emulator cannot disagree about what an instruction costs. The
// instructions whose cost depends on a condition — a branch that is taken, a
// compare that differs — are measured both ways and print as `base/taken`.
//
// It reads a raw image with a load address, so it serves a RAM dump, a driver
// blob carved out of a ROM, and the RAM half of an .spc file equally.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "disasm/disasm.h"

namespace snaggletooth::disasm {

// What operand bytes follow an opcode, and how each one reads. The instruction's
// text carries the surrounding syntax — a register name, an index suffix, the
// brackets of an indirect form — and leaves a numbered slot where each operand
// lands. So this enumerates the operand bytes alone, and every instruction
// sharing a byte shape shares a value here however differently it prints.
enum class Spc700Operands : std::uint8_t {
  None,    // no operand bytes
  Imm,     // one byte, an immediate value
  Dp,      // one byte, a direct-page offset
  Abs,     // two bytes, an address, low byte first
  AbsBit,  // two bytes: an address in the low 13 bits, a bit index in the top 3
  Rel,     // one byte, a displacement from the end of the instruction
  DpRel,   // a direct-page offset, then a displacement
  DpDp,    // a source offset, then a destination offset
  ImmDp,   // an immediate byte, then a destination offset
  Upage,   // one byte, an offset into page $FF
};

// How many bytes an operand shape adds to the opcode.
[[nodiscard]] constexpr std::uint8_t spc700OperandBytes(Spc700Operands operands) noexcept {
  switch (operands) {
    case Spc700Operands::None: return 0;
    case Spc700Operands::Imm:
    case Spc700Operands::Dp:
    case Spc700Operands::Rel:
    case Spc700Operands::Upage: return 1;
    case Spc700Operands::Abs:
    case Spc700Operands::AbsBit:
    case Spc700Operands::DpRel:
    case Spc700Operands::DpDp:
    case Spc700Operands::ImmDp: return 2;
  }
  return 0;
}

// One row of the instruction table: the opcode, its text with `%1` and `%2`
// where the operands go — numbered in the order the bytes appear, so a form
// whose source byte comes first but prints second names its slots out of order —
// the operand shape, and how execution leaves the instruction. The same table
// decodes an instruction and encodes one, so the two cannot disagree.
struct Spc700Opcode {
  std::uint8_t opcode = 0;
  const char* text = "";
  Spc700Operands operands = Spc700Operands::None;
  Flow flow = Flow::Continue;
};

// The instruction table, indexed by opcode.
[[nodiscard]] const std::array<Spc700Opcode, 256>& spc700Opcodes();

// The SPC700 has a 16-bit address space and its instructions always read the same
// way, so the backend never sets a bank and passes the context through untouched.
class Spc700Backend final : public Backend {
 public:
  [[nodiscard]] std::string_view name() const override { return "SPC700"; }
  [[nodiscard]] unsigned addressBits() const override { return 16; }
  [[nodiscard]] std::optional<Decoded> decode(std::span<const std::uint8_t> image, Address base,
                                              Address at, Context context) const override;
  [[nodiscard]] std::string_view registerName(Address address) const override;
};

// The one SPC700 backend; it holds no state.
[[nodiscard]] const Spc700Backend& spc700Backend();

// A request to the SPC700 disassembler is a framework request.
using DisasmRequest = Request;

// Decodes the single instruction at `address`. Returns nothing when the address
// lies outside the image, or when the instruction's operand bytes would run past
// its end.
[[nodiscard]] std::optional<Instruction> decodeAt(std::span<const std::uint8_t> image,
                                                  std::uint16_t base,
                                                  std::uint16_t address);

// Traces the image from its entry points with the SPC700 backend and returns the
// listing.
[[nodiscard]] Listing trace(const DisasmRequest& request);

// The measured cost of every opcode, indexed by opcode. Measured once, by running
// the interpreter — see the file comment.
[[nodiscard]] const std::array<CycleCost, 256>& cycleTable();

// The name of an SPC700 hardware register, or an empty view when the address is
// ordinary memory. The chip's registers occupy $00F0-$00FF.
[[nodiscard]] std::string_view registerName(std::uint16_t address);

}  // namespace snaggletooth::disasm
