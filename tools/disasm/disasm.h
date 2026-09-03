#pragma once

// The disassembly framework — the part of a tracing disassembler that does not
// depend on which chip the bytes are for.
//
// A disassembler here is a backend over this framework. The backend knows the
// instruction set: it decodes one instruction at an address and says how
// execution leaves it. The framework knows everything else: it follows control
// flow from the entry points it is given, so a byte is code only when execution
// can reach it, and it renders the result as assemblable source with the address,
// the raw bytes, the cycle cost and any annotation riding in a trailing comment.
//
// Addresses are 24-bit throughout — a bank in the top byte, an offset below it —
// so a backend for a chip with a 16-bit address space simply never sets the bank.
//
// A trace carries a context beside every address it visits. The framework does
// not interpret it; a backend uses it for whatever state decides how the bytes at
// an address read. The 65816's operand widths depend on two flags that change
// along the path that reaches an instruction, so its backend carries those flags
// here, and where two paths reach one address with different contexts the
// framework reports the conflict rather than choosing. A backend whose
// instructions always read the same way passes the context through untouched.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snaggletooth::disasm {

// A 24-bit address: the bank in bits 16-23, the offset within it below.
using Address = std::uint32_t;

// What an instruction costs in chip cycles. Most costs are fixed and `taken` is
// zero. A conditional instruction costs `base` when its condition fails and
// `taken` when it holds.
struct CycleCost {
  std::uint8_t base = 0;
  std::uint8_t taken = 0;
};

// How an instruction reaches the rest of the program. The tracer needs to know
// whether execution falls through it, leaves it, or stops at it.
enum class Flow : std::uint8_t {
  Continue,  // falls through to the next instruction
  Branch,    // falls through, and may also reach `target`
  Jump,      // never falls through; reaches `target` when that is known statically
  Call,      // reaches `target`, and execution resumes after the call
  Return,    // never falls through
  Halt,      // never falls through and never resumes
};

// The state a trace carries beside an address. Its meaning belongs to the
// backend; the framework only compares it and asks the backend to describe it.
struct Context {
  std::uint32_t bits = 0;
  friend bool operator==(const Context&, const Context&) = default;
};

// One decoded instruction.
struct Instruction {
  Address address = 0;
  std::uint8_t opcode = 0;
  std::uint8_t length = 1;
  Flow flow = Flow::Continue;
  CycleCost cycles;
  std::vector<std::uint8_t> bytes;         // the instruction's own bytes, opcode first
  std::string text;                        // rendered mnemonic and operands
  std::optional<Address> target;           // the destination, when it is a constant
  std::optional<Address> operandAddress;   // the memory address the operand names, when the backend reports one
  std::string note;                        // an annotation: a named register, a changed byte
};

// What a backend returns for one instruction: the instruction, and the context
// execution carries out of it — to the instruction after it and to its target
// alike.
struct Decoded {
  Instruction instruction;
  Context next;
};

// One line of a listing: an instruction, or a run of bytes execution never reached.
struct Line {
  bool isCode = false;
  Address address = 0;
  Instruction instruction;         // when isCode
  std::vector<std::uint8_t> data;  // when !isCode
};

// A finished listing: its lines in address order, the labels the trace found, and
// the width its addresses print at.
struct Listing {
  std::vector<Line> lines;
  std::map<Address, std::string> labels;
  std::vector<std::string> warnings;
  unsigned addressBits = 16;  // 16 prints `$XXXX`; 24 prints `$BB:XXXX`
};

// What to disassemble. `image` is the bytes; `base` is the address `image[0]`
// occupies. Tracing starts at every address in `entries`, each with `context`;
// an empty `entries` traces from `base` alone.
//
// `priorImage`, when it is the same length as `image`, is the same region before
// the code ran. Every byte that differs is called out on the line that carries it,
// which is how a self-modifying program's patched slots become visible instead of
// being read as if they had always held those bytes.
struct Request {
  std::span<const std::uint8_t> image;
  Address base = 0;
  std::vector<Address> entries;
  std::span<const std::uint8_t> priorImage;
  bool annotateRegisters = true;
  std::map<Address, std::string> symbols;
  Context context;
};

// An instruction set, as the framework sees one.
class Backend {
 public:
  virtual ~Backend() = default;

  // The chip's name, as a listing reports it.
  [[nodiscard]] virtual std::string_view name() const = 0;

  // How wide the chip's addresses are: 16 or 24.
  [[nodiscard]] virtual unsigned addressBits() const = 0;

  // Decodes the instruction at `at` in an image whose first byte occupies `base`,
  // under `context`. Returns nothing when the address lies outside the image, or
  // when the instruction's operand bytes would run past its end.
  [[nodiscard]] virtual std::optional<Decoded> decode(std::span<const std::uint8_t> image,
                                                      Address base, Address at,
                                                      Context context) const = 0;

  // The name of the hardware register at `address`, or an empty view when the
  // address is ordinary memory.
  [[nodiscard]] virtual std::string_view registerName(Address address) const = 0;

  // The address of the instruction following one of `length` bytes at `at`. The
  // program counter wraps within its bank on every chip built so far, so that is
  // the default.
  [[nodiscard]] virtual Address following(Address at, std::uint8_t length) const {
    return (at & 0xFF0000u) | ((at + length) & 0xFFFFu);
  }

  // A context, as a warning names it. The default prints its bits.
  [[nodiscard]] virtual std::string describe(Context context) const;
};

// Traces the image from its entry points with the backend and returns the listing.
[[nodiscard]] Listing trace(const Backend& backend, const Request& request);

// Renders a listing as text: address, raw bytes, mnemonic, cycle cost, notes.
[[nodiscard]] std::string render(const Listing& listing);

// An address as a listing prints it: `$XXXX` at 16 bits, `$BB:XXXX` at 24.
[[nodiscard]] std::string formatAddress(Address address, unsigned addressBits);

}  // namespace snaggletooth::disasm
