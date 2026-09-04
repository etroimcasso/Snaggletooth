#pragma once

// The 65816 lift — a listing's instructions as nodes of the intermediate
// representation.
//
// This is the one place in the toolkit where bytes become meaning. It reads each
// decoded instruction's opcode and operand bytes, names the instruction in the
// instruction layer, and writes out the effects the chip performs for it: the
// address arithmetic with every wrap the chip has, the loads and stores in the
// order the chip makes them, the register writes at their widths, the flags each
// operation moves, and the cycles it costs beyond its measured base. Every rule
// is the core's, and the core over a flat bus is what the lift is proven against.
//
// The mode a node reads under is the trace's. A width the trace settled becomes a
// type; a width it did not — after `PLP` or `RTI` in native mode — becomes a
// selection by the live flag, so the node is right whichever way the flag falls.

#include <cstdint>
#include <span>
#include <vector>

#include "cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/ir.h"

namespace snaggletooth::ir {

// One instruction as a node. `mode` is what the instruction was decoded under;
// `patched` marks bytes that differ from the image the code started as.
[[nodiscard]] Node liftInstruction(const disasm::Instruction& instruction,
                                   const disasm::Cpu65816Mode& mode, bool patched = false);

// A whole 65816 listing as a program: one node per code line, in address order.
//
// An address the trace reached under two modes that read its bytes two ways is
// two nodes. The listing carries the first reading; the second is decoded again
// from `image` — the bytes the listing was traced from, with `image[0]` at
// `base` — and follows the first. With no image, an address reads one way only.
[[nodiscard]] Program lift65816(const disasm::Listing& listing,
                                std::span<const std::uint8_t> image = {}, Address base = 0);

// Which hardware interrupt sequence.
enum class Interrupt : std::uint8_t { Nmi, Irq };

// The effects of a hardware interrupt taken between two instructions: the read
// of the instruction it interrupts, the program bank saved in native mode, the
// program counter and the status byte pushed, the vector read, and the handler
// entered in bank zero with further maskable requests disabled. `lift65816`
// attaches both to the program it returns.
[[nodiscard]] std::vector<Effect> interruptSequence(Interrupt interrupt);

}  // namespace snaggletooth::ir
