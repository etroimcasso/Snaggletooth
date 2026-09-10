#pragma once

// The SPC700 lift — a sound program's instructions as nodes of the intermediate
// representation.
//
// The second place in the toolkit where bytes become meaning. It reads each
// decoded instruction's opcode and operand bytes, names the instruction in the
// instruction layer by the SPC700 table's mnemonic and operand form, and writes
// out the effects the chip performs for it: the direct-page arithmetic under the
// P flag with every wrap the chip has, the pointer reads inside the page, the
// loads and stores in the order the chip makes them — the read a store makes of
// its destination before writing it, and the read of the byte after the opcode
// that a one-byte instruction makes and throws away — the register writes, the
// flags each operation moves, and the cycles a taken branch costs beyond its
// measured base. Every rule is the core's, and the core over a flat bus is what
// the lift is proven against.
//
// A sound-CPU node carries no mode: the chip's instructions always read the
// same way, and the page a direct operand lives in is the P flag's choice at
// run time, which the effects read.

#include <cstdint>
#include <vector>

#include "disasm/disasm.h"
#include "ir/ir.h"
#include "spc700_disasm.h"

namespace snaggletooth::ir {

// One instruction as a node. `patched` marks bytes that differ from the image
// the code started as.
[[nodiscard]] Node liftSpc700Instruction(const disasm::Instruction& instruction,
                                         bool patched = false);

// A whole SPC700 listing as the sound program's nodes: one per code line, in
// address order.
[[nodiscard]] std::vector<Node> liftSpc700(const disasm::Listing& listing);

}  // namespace snaggletooth::ir
