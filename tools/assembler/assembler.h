#pragma once

// The assembler — the part of an assembler that does not depend on which chip the
// code is for. It is the common layer of the assembly language
// (`docs/assembly-lexicon.md`): the line form, comments, numbers, character and
// string literals, labels, `EQU`, expressions, `ORG` and the data directives, the
// second pass that resolves forward references, and the diagnostics.
//
// A chip's assembler is a dialect over this: it owns the mnemonics, the operand
// syntax of its addressing modes, and any directive its encoding needs, and it
// turns one instruction into bytes. The 65816 dialect also carries the register
// widths along the file, because an immediate's length depends on them.
//
// Assembly is absolute. Every byte's address is known while it is assembled, so
// the result is a set of address ranges with bytes, not a relocatable object.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snaggletooth::assembler {

// An address: 16 bits wide in one dialect, 24 in the other.
using Address = std::uint32_t;

// One error: the file, the line it is on (counted from 1), and what was expected.
struct Diagnostic {
  std::string file;
  unsigned line = 0;
  std::string message;
};

// A run of bytes the assembly emitted, starting at `start`.
struct Range {
  Address start = 0;
  std::vector<std::uint8_t> bytes;
};

// What assembling a file produces. `ranges` are in address order and never
// overlap; two runs the source emitted back to back are one range. `symbols` holds
// every label and `EQU` with its value. A file with any error emits no ranges.
struct Assembly {
  std::vector<Range> ranges;
  std::map<std::string, std::uint32_t> symbols;
  std::vector<Diagnostic> errors;

  [[nodiscard]] bool ok() const { return errors.empty(); }
};

// An expression's value. `resolved` is false only on the first pass, for an
// expression naming a symbol the file defines later; the value is then zero and
// nothing may be checked against it.
struct Value {
  std::uint32_t value = 0;
  bool resolved = true;
};

// How a dialect evaluates the expressions inside the operand syntax it owns.
class Evaluator {
 public:
  virtual ~Evaluator() = default;

  // Evaluates `text` as an expression. Returns nothing and sets `error` when the
  // text is not an expression or names a symbol nothing defines.
  [[nodiscard]] virtual std::optional<Value> evaluate(std::string_view text,
                                                      std::string& error) const = 0;

  // Whether this is the first pass, where a forward reference is unresolved.
  [[nodiscard]] virtual bool firstPass() const = 0;
};

// What a dialect returns for one instruction: its bytes, or why it has none.
struct Encoded {
  std::vector<std::uint8_t> bytes;
  std::string error;
};

// An instruction set, as the assembler sees one.
class Dialect {
 public:
  virtual ~Dialect() = default;

  // The chip's name, as a diagnostic reports it.
  [[nodiscard]] virtual std::string_view name() const = 0;

  // How wide the dialect's addresses are: 16 or 24.
  [[nodiscard]] virtual unsigned addressBits() const = 0;

  // Whether an upper-cased name is a mnemonic, a register or a directive of the
  // dialect, and so may not be a label.
  [[nodiscard]] virtual bool reserved(std::string_view upperName) const = 0;

  // Called at the start of assembly, at every `ORG`, and after every data
  // directive: a region begins, and whatever the dialect carries along a region
  // starts over.
  virtual void beginRegion() = 0;

  // Handles a directive of the dialect's own. Returns false when `upperName` is
  // not one; true when it is, with `error` set if its operands were wrong.
  virtual bool directive(std::string_view upperName, std::string_view operands,
                         const Evaluator& evaluator, std::string& error) = 0;

  // Assembles one instruction placed at `at`. `mnemonic` is upper-cased;
  // `operands` is the rest of the line with the comment removed and the ends
  // trimmed, in the case it was written. On the first pass an operand may
  // evaluate unresolved; the bytes must still be the right length, and nothing is
  // checked against the value.
  [[nodiscard]] virtual Encoded encode(std::string_view mnemonic, std::string_view operands,
                                       Address at, const Evaluator& evaluator) = 0;
};

// Assembles `source` under `dialect`. `file` names the source in diagnostics.
[[nodiscard]] Assembly assemble(Dialect& dialect, std::string_view source,
                                std::string_view file = "");

// The assembly's ranges laid into one image `size` bytes long whose first byte
// occupies `base`, the gaps holding `fill`. Returns nothing when a range lies
// outside the image.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> image(const Assembly& assembly,
                                                             Address base, std::size_t size,
                                                             std::uint8_t fill = 0);

// ---- helpers for dialects ------------------------------------------------------

// `text` upper-cased, ASCII only.
[[nodiscard]] std::string upper(std::string_view text);

// `text` with every space and tab outside quotes removed, so operand syntax can
// be matched character by character.
[[nodiscard]] std::string compact(std::string_view text);

// The end of the expression that starts at `from` in `text`: the index of the
// first character that is not part of it. Returns `from` when no expression
// starts there. An expression ends before `+X` or `+Y` where the register
// letter is not the start of a longer name, which is how an index suffix written
// with `+` is told from a term.
[[nodiscard]] std::size_t expressionEnd(std::string_view text, std::size_t from);

// Whether `value` fits in `bits` bits.
[[nodiscard]] constexpr bool fits(std::uint32_t value, unsigned bits) noexcept {
  return bits >= 32 || value < (1u << bits);
}

// A value as a diagnostic prints it: `$` and hexadecimal digits.
[[nodiscard]] std::string hex(std::uint32_t value, unsigned digits);

}  // namespace snaggletooth::assembler
