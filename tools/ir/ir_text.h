#pragma once

// The intermediate representation as text: a name for every value of the
// vocabulary, and a node written out with its effects, one per line, the way a
// person reads what the lift wrote. The text is for reading; nothing parses it
// back.

#include <string>
#include <string_view>

#include "ir/ir.h"

namespace snaggletooth::ir {

[[nodiscard]] std::string_view opName(Op op) noexcept;
[[nodiscard]] std::string_view placeName(Place place) noexcept;
[[nodiscard]] std::string_view widthName(Width width) noexcept;
[[nodiscard]] std::string_view stepName(Step step) noexcept;
[[nodiscard]] std::string_view accessName(Access access) noexcept;
[[nodiscard]] std::string_view whenName(When when) noexcept;

// An addressing mode as source spells its operand — `abs,X`, `(dp),Y`, `#imm(M)`.
[[nodiscard]] std::string_view addressingName(Addressing addressing) noexcept;

// A mode as the text names it: `e=1`, or `e=0 m=8 x=?` with `?` for a width the
// trace did not know.
[[nodiscard]] std::string modeName(const Mode& mode);

// One effect on one line: the operation, its destination and operands, the
// width, the step or the pin where the operation has one, and the condition.
[[nodiscard]] std::string renderEffect(const Effect& effect);

// One node: a header line with its address, mnemonic, operand, length, mode,
// measured costs and register name, then each effect indented under it.
[[nodiscard]] std::string renderNode(const Node& node);

}  // namespace snaggletooth::ir
