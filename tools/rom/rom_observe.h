#pragma once

// The run as oracle: the cartridge booted on the machine and watched, one
// instruction at a time, for the jumps the bytes alone cannot follow.
//
// A trace names a target only when the instruction names it. Four forms do not:
// `JMP (!abs)`, `JMP (!abs,X)`, `JML [!abs]` and `JSR (!abs,X)` take their
// destination from a pointer in memory, and a cartridge whose dispatch runs
// through one of them is a wall to the trace — everything past it is data until
// a person supplies the destinations. A run knows them. Every time the machine is
// about to execute one of those four, the pointer it is about to read is read
// first, the way the CPU reads it, and the target is recorded with the mode the
// instruction carries in. Those targets are entries: the disassembler traces from
// them exactly as it traces from a vector or from an entry a person added.
//
// Nothing is inferred from where the CPU landed. The pointer is read before the
// step, and the landing only confirms it — a step that services an interrupt
// instead lands in the handler, and records nothing; the instruction runs later,
// and is seen then.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"

namespace snaggletooth::disasm {

// One destination a run saw an indirect jump or call take: where it landed, the
// mode it arrived in, and the instruction that took it. `call` says which of the
// four forms it was — a call's target is a routine, a jump's a location — which
// is what names the label.
struct ReachedTarget {
  Address target = 0;
  Cpu65816Mode mode;
  Address site = 0;
  bool call = false;
  // The label the target carries in the tree: `sub_`/`loc_` and its address unless
  // a person's entry names the same target under the same mode, whose name wins.
  // Empty as the run reports it; the disassembler fills it in.
  std::string name;
};

// Two sightings are the same when the same site reached the same target under
// the same mode; what it was called is not part of what was seen.
[[nodiscard]] bool sameSighting(const ReachedTarget& a, const ReachedTarget& b);

// Boots `rom` on the machine and steps it for `masterCycles` of the master clock,
// recording every distinct target the four indirect forms took. Returned in site
// order, then target order, each site/target/mode once. A site whose pointer lies
// where the run cannot read it — anything but the image and work RAM — is named
// once in `notes` and produces nothing; so is a site whose pointer did not match
// where the CPU then went, which the design does not expect and reports rather
// than hides.
[[nodiscard]] std::vector<ReachedTarget> observeRun(std::span<const std::uint8_t> rom,
                                                    std::uint64_t masterCycles,
                                                    std::vector<std::string>& notes);

}  // namespace snaggletooth::disasm
