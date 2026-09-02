#pragma once

// The SPC700 disassembler — turns a block of memory into a readable listing.
//
// Two properties separate it from a plain byte-to-mnemonic printer.
//
// It traces rather than sweeps. A linear walk that starts at the first byte and
// decodes forward treats jump tables, text and packed data as instructions, and
// produces confident nonsense from all of them. This disassembler follows control
// flow from the entry points it is given: a byte is code only when execution can
// reach it. Everything else is emitted as data, labelled as such.
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
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snaggletooth::disasm {

// What an instruction costs in chip cycles. Most costs are fixed and `taken` is
// zero. The conditional instructions — the relative branches, the compare- and
// decrement-and-branch forms — cost `base` when the condition fails and `taken`
// when it holds.
struct CycleCost {
  std::uint8_t base = 0;
  std::uint8_t taken = 0;
};

// How an instruction reaches the rest of the program. A tracer needs to know
// whether execution falls through it, leaves it, or stops at it.
enum class Flow : std::uint8_t {
  Continue,  // falls through to the next instruction
  Branch,    // falls through, and may also reach `target`
  Jump,      // never falls through; reaches `target` when that is known statically
  Call,      // reaches `target`, and execution resumes after the call
  Return,    // never falls through
  Halt,      // never falls through and never resumes
};

// One decoded instruction.
struct Instruction {
  std::uint16_t address = 0;
  std::uint8_t opcode = 0;
  std::uint8_t length = 1;
  Flow flow = Flow::Continue;
  CycleCost cycles;
  std::vector<std::uint8_t> bytes;      // the instruction's own bytes, opcode first
  std::string text;                     // rendered mnemonic and operands
  std::optional<std::uint16_t> target;  // the destination, when it is a constant
  std::string note;                     // an annotation: a named register, a changed byte
};

// One line of a listing: an instruction, or a run of bytes execution never reached.
struct Line {
  bool isCode = false;
  std::uint16_t address = 0;
  Instruction instruction;      // when isCode
  std::vector<std::uint8_t> data;  // when !isCode
};

// A finished listing: its lines in address order, and the labels the trace found.
struct Listing {
  std::vector<Line> lines;
  std::map<std::uint16_t, std::string> labels;
  std::vector<std::string> warnings;
};

// What to disassemble. `image` is the bytes; `base` is the address `image[0]`
// occupies. Tracing starts at every address in `entries`; an empty `entries`
// traces from `base` alone.
//
// `priorImage`, when it is the same length as `image`, is the same region before
// the code ran. Every byte that differs is called out on the line that carries it,
// which is how a self-modifying driver's patched slots become visible instead of
// being read as if they had always held those bytes.
struct DisasmRequest {
  std::span<const std::uint8_t> image;
  std::uint16_t base = 0;
  std::vector<std::uint16_t> entries;
  std::span<const std::uint8_t> priorImage;
  bool annotateRegisters = true;
  std::map<std::uint16_t, std::string> symbols;
};

// Decodes the single instruction at `address`. Returns nothing when the address
// lies outside the image, or when the instruction's operand bytes would run past
// its end.
[[nodiscard]] std::optional<Instruction> decodeAt(std::span<const std::uint8_t> image,
                                                  std::uint16_t base,
                                                  std::uint16_t address);

// Traces the image from its entry points and returns the listing.
[[nodiscard]] Listing trace(const DisasmRequest& request);

// Renders a listing as text: address, raw bytes, mnemonic, cycle cost, notes.
[[nodiscard]] std::string render(const Listing& listing);

// The measured cost of every opcode, indexed by opcode. Measured once, by running
// the interpreter — see the file comment.
[[nodiscard]] const std::array<CycleCost, 256>& cycleTable();

// The name of an SPC700 hardware register, or an empty view when the address is
// ordinary memory. The chip's registers occupy $00F0-$00FF.
[[nodiscard]] std::string_view registerName(std::uint16_t address);

}  // namespace snaggletooth::disasm
