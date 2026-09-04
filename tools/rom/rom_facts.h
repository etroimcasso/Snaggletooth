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
// value the instruction before loaded as an immediate, and no further; a transfer
// whose set-up came from a table has no value, and says so by carrying none; a
// routine's boundary is where execution stops or is named, and no further.
// Reaching further — carrying values across calls, resolving a table — takes
// dataflow over a lifted form, which this does not do.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"

namespace snaggletooth::disasm {

struct CartridgeDisassembly;  // rom_disasm.h

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
// with no label between them, or the instruction is `STZ` and writes a zero it
// carries itself. A read never has one, and neither does a write whose value came
// from anywhere else.
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
// absolute operand in bank zero. A direct-page operand names no address the image
// can settle, so it produces nothing.
//
// An instruction whose register is 16 bits wide reaches two registers and
// produces a fact for each, which is how `STX $4300` under a 16-bit index sets a
// channel's direction and its destination at once.
[[nodiscard]] std::vector<HardwareAccess> hardwareAccesses(const CartridgeDisassembly& disassembly);

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
