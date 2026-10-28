#pragma once

// Which routines a run runs once for every value an index can take, and what
// the code proves about a transfer from work RAM the run never started.
//
// A cartridge keeps most of what it sends to the hardware behind a table: a
// loader takes an index, reads a pointer, and builds the entry it points at in
// work RAM. A run sees the entries the game selected. The plan names every
// routine the shadow saw build a carried range — and every routine that calls
// one, since the bound on the index is usually the caller's — and, for each
// one a call enters, the registers every path bounds at its entry: the index
// a caller masked or compared before the call. The second run
// (`rom/rom_observe.h`) runs each of those routines once for every such value.

#include <string>
#include <vector>

#include "rom/rom_disasm.h"

namespace snaggletooth::disasm {

// The routines the second run enumerates: every routine the shadow named as a
// writer of a staged extent, and every routine from which one is reachable
// through the routines' calls; for each that a call enters, the registers every
// path bounds at its entry. A routine the plan refuses — no path reaches it, no
// input is bounded, a jump enters it, no routine holds the site — is named once
// in `notes` and is not in the plan.
//
// The rules, in order. The writers are the routines holding the site of every
// staged extent's writer that is the CPU's and wrote; a site no routine holds
// is noted `enumerate $BB:XXXX: no routine holds the site; not run`. The
// closure adds every routine whose calls reach a member, until nothing is
// added. A routine is entered by a call when a call node of the program, or a
// call the run reached or the bytes derived, lands on its address; otherwise
// `enumerate <label>: entered by a jump, no return to end at; not run`. The
// state every path proves before its first node is read; none is
// `enumerate <label>: no path the analysis follows reaches its entry; not run`.
// Each of A, X and Y is read at the width the entry's mode gives it — the low
// byte alone at 8 bits, nothing where the mode does not settle the width — and
// is bounded when every path proves two values or more for it. None is
// `enumerate <label>: no input every path bounds; not run`. A bounded register
// is an input when some path from the entry reads a byte of it at that width
// before writing it: an operand, a push, or a call whose callee reads it at
// its own entry. The paths are followed over the routine's own nodes, and
// every byte counts as read where the walk cannot follow: a successor outside
// the routine, a jump or a call to an unknown target or to an address no
// routine begins at, `BRK`, `COP`, and a call back into a routine being
// walked. A write under a condition kills nothing; a write to the accumulator
// at a width the node does not settle kills its low byte alone; a write to an
// index register kills both of its bytes. A bounded register no path reads is
// `enumerate <label>: <R> is bounded on every path and read on none before it
// is written; not an input`, in the order A, X, Y. The direct register, the
// data bank, the stack pointer, the program bank and the flags are never
// inputs. Notes are written for the sites first, in address order, then for
// the routines, in address order.
[[nodiscard]] EnumerationPlan enumerationPlan(const CartridgeDisassembly& disassembly, const ProvenProgram& proven,
                                              std::vector<std::string>& notes);

// The notes for every transfer the code proves whole from work RAM that the run
// never started: whether the run wrote the extent under another site. A
// general-purpose transfer to the B bus with a proven start, source, count and
// a step that moves, whose source is not the image, is confirmed by a range the
// run moved to a register on the same channel from the same start site with
// the same lowest address, count and step; one no range confirms is noted
// `dma $BB:XXXX channel <n>: the code proves $BB:XXXX <step> bytes <n>; the run
// wrote the extent and never started this transfer; its sources are the staged
// lines at $BB:XXXX` when a staged extent overlaps it, and `...; nothing wrote
// the extent in the run; nothing to run from` otherwise. Written in the
// transfers' order.
void noteProvenFromWorkRam(CartridgeDisassembly& disassembly);

}  // namespace snaggletooth::disasm
