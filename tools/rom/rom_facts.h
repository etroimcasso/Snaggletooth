#pragma once

// What the code reaches: the hardware a disassembled cartridge drives, attached
// to the addresses that drive it, and the routines those addresses belong to.
//
// The trace already knows which instruction names which register — the listing
// writes the name in the comment beside it. What it does not say is what the
// register is for, whether the instruction reads or writes it, and with what
// value. Those three turn a listing into something a person can study: a routine
// that writes VMADDL and then VMDATAL is loading graphics, and one that writes a
// channel's B-bus address with $18 is about to send a transfer to the same place.
// The routines say which lines belong together, which routines call which, and so
// what each is for — the part of the machine it drives itself, and the part it
// drives through what it calls.
//
// Everything here is read off the bytes and nothing is inferred. A value is a
// value every path into the instruction proves — the instruction before loaded
// it as an immediate, or a load twenty instructions and a call earlier did and
// nothing since has touched the register — and no further; a transfer whose
// set-up came from a table has no value, and says so by carrying none; a
// routine's boundary is where execution stops or is named, and no further. What
// every path proves comes from the dataflow over the lifted program
// (`ir/ir_dataflow.h`), run here over every region at once.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/ir_dataflow.h"

namespace snaggletooth::disasm {

struct CartridgeDisassembly;  // rom_disasm.h

// The program every region lifts to, and what every path proves over it. The
// dataflow refers to the program, so the two travel together.
struct ProvenProgram {
  std::unique_ptr<ir::Program> program;
  std::unique_ptr<ir::Dataflow> flow;
  ir::ImageReader image;
  ir::StackReach stack;
};

// Lifts every 65816 region of the disassembly into one program and runs the
// dataflow over it from every entry — the reset vector with the direct register
// and the data bank zero, every other entry with nothing proven — along the
// program's own flow and every destination a run reached or the bytes derived.
[[nodiscard]] ProvenProgram proveProgram(const CartridgeDisassembly& disassembly,
                                         std::span<const std::uint8_t> rom);

// A destination the bytes prove a jump or a call through a pointer takes: where
// it leads, the mode the jump carries in, the site, the address the pointer was
// read from, and whether the site calls or jumps. `name` is the label the target
// carries in the tree: a person's entry naming the same target under the same
// mode wins; otherwise `sub_` or `loc_` with the address. Empty as the analysis
// reports it; the disassembler fills it in.
struct DerivedTarget {
  Address target = 0;
  Cpu65816Mode mode;
  Address site = 0;
  Address pointer = 0;
  bool call = false;
  std::string name;
};

// Two derivations are the same when the same site derives the same target under
// the same mode through the same pointer.
[[nodiscard]] bool sameDerivation(const DerivedTarget& a, const DerivedTarget& b);

// Every destination the dataflow derived, in site order then target order, each
// under the mode its site carries.
[[nodiscard]] std::vector<DerivedTarget> derivedTargets(const CartridgeDisassembly& disassembly,
                                                        const ProvenProgram& proven);

// What every path proves about three registers at a label, before the
// instruction there runs: each is the values the register can hold — one is a
// fact, several are what different paths prove, none is not known.
struct StateFact {
  Address address = 0;
  std::vector<std::uint32_t> d;
  std::vector<std::uint32_t> dbr;
  std::vector<std::uint32_t> s;
};

// One fact per label at which at least one of the three is known, in address
// order. The sound program is another chip's and has none.
[[nodiscard]] std::vector<StateFact> stateFacts(const CartridgeDisassembly& disassembly,
                                                const ProvenProgram& proven);

// How an instruction touches the register its operand names. The kind is in the
// instruction set: a load, a compare or a bit test reads, a store writes, and the
// read-modify-write forms do both.
enum class AccessKind : std::uint8_t {
  Read,
  Write,
  ReadWrite,
};

// A kind as a manifest names it: `read`, `write`, `read-write`.
[[nodiscard]] std::string_view accessKindName(AccessKind kind);

// One instruction reaching one hardware register.
//
// `value` is the byte the instruction wrote, when the bytes say it: the
// instruction immediately before loaded the store's register with an immediate,
// with no label between them; the instruction is `STZ` and writes a zero it
// carries itself; or every path into the store proves the register's value. A
// read never has one, and neither does a write whose value came from anywhere
// else.
//
// `run` numbers the straight-line stretch of code the site sits in. A run begins
// at a label, after a run of data, and after an instruction execution does not
// fall through — so two sites in one run were reached one after the other, by the
// only path there is. It is what makes a transfer's set-up attributable: the
// pieces of a channel written across several instructions belong together only
// when nothing could have entered between them.
struct HardwareAccess {
  Address site = 0;
  Address registerAddress = 0;
  std::string_view name;
  RegisterClass cls = RegisterClass::Display;
  AccessKind kind = AccessKind::Read;
  std::optional<std::uint8_t> value;
  std::uint32_t run = 0;
};

// Which way a channel moves bytes, as the direction bit of `DMAP` says — or
// unknown, where no value for `DMAP` was found.
enum class DmaDirection : std::uint8_t {
  ToBBus,   // the CPU's memory to the register: a transfer that loads hardware
  ToABus,   // the register to the CPU's memory: a transfer that reads it back
  Unknown,
};

// A direction as a manifest names it.
[[nodiscard]] std::string_view dmaDirectionName(DmaDirection direction);

// A transfer a channel was set up for. The destination is the register the
// channel's `BBAD` names — that value, not the `BBAD` register itself, is what
// the transfer reaches — so its class is what the transfer is: `Vram` is a
// tileset or a tilemap, `Cgram` a palette, `Oam` sprite tables, `Apu` a driver
// or its samples.
//
// A field is absent where the bytes did not say it. `destination` is absent when
// no value for `BBAD` was found, `source` when the three address registers were
// not all written with values in the same run, and `startMask` when no write to
// `MDMAEN` or `HDMAEN` with a value follows in that run. None of them is guessed.
struct DmaTransfer {
  Address site = 0;  // where the channel's `BBAD`, or its `DMAP` alone, was written
  std::uint8_t channel = 0;
  DmaDirection direction = DmaDirection::Unknown;
  std::optional<Address> destination;
  std::string_view destinationName;
  std::optional<RegisterClass> destinationClass;
  std::optional<Address> source;
  std::optional<std::uint8_t> startMask;
  bool hdma = false;  // the start came from `HDMAEN` rather than `MDMAEN`
  std::uint32_t run = 0;
};

// Every hardware register the cartridge's code reaches, in address order.
//
// Only the 65816 regions are read: the sound program is another chip's, with
// registers of its own. Only an instruction the trace decoded produces a fact —
// a run of data produces none — and only where the operand names an address the
// listing itself annotates, which is a long operand in its own bank and an
// absolute operand in bank zero, or, given what every path proves, a direct-page
// operand under a direct register the paths settle, which then names the
// register it lands on in bank zero. Without `proven`, a direct-page operand
// produces nothing and a value is the instruction before's alone.
//
// An instruction whose register is 16 bits wide reaches two registers and
// produces a fact for each, which is how `STX $4300` under a 16-bit index sets a
// channel's direction and its destination at once.
[[nodiscard]] std::vector<HardwareAccess> hardwareAccesses(const CartridgeDisassembly& disassembly,
                                                           const ProvenProgram* proven = nullptr);

// The transfers those accesses set up, in address order — one per site that wrote
// a channel's `BBAD` or, where none did, its `DMAP`.
[[nodiscard]] std::vector<DmaTransfer> dmaTransfers(const std::vector<HardwareAccess>& accesses);

// A routine: the lines execution reaches from a label by falling through, by
// branching and by jumping, without passing a return or a halt, and without
// entering the routine a call names — the call is an edge, and execution resumes
// after it. A jump or a call whose target the bytes do not name ends the walk
// there, as it ends the trace.
//
// Every label a call names is a routine, as is every entry and every target a run
// reached; a label only branches and jumps reach belongs to the routines that
// reach it. Nothing else draws a boundary, so a routine that falls through into
// the next label's code, or jumps into it, holds those lines too, and a line two
// routines reach is in both — that is what the bytes say, and a person names the
// boundary when they name the routine.
//
// `reaches` is the routine's role: the classes of the registers its own lines
// reach, and of the transfers its own lines set up. `through` is what its calls
// reach, followed through every routine they call in turn.
struct Routine {
  Address address = 0;  // the label the routine begins at
  std::string label;
  std::vector<Address> lines;  // every line it holds, in address order
  std::size_t bytes = 0;
  std::vector<Address> calls;  // the routines its calls name, in address order, each once
  std::vector<RegisterClass> reaches;
  std::vector<RegisterClass> through;
};

// Every routine in the 65816 regions, in address order. The sound program is
// another chip's and has none.
[[nodiscard]] std::vector<Routine> routines(const CartridgeDisassembly& disassembly);

}  // namespace snaggletooth::disasm
