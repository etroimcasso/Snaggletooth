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
//
// The same run watches the transfer engines. Every byte a general-purpose DMA
// or an HDMA channel moves crosses the bus in the engine's name, and the run
// records where each range of them came from, where it went, how many there
// were and which instruction started it — the transfers a cartridge sets up from
// pointers, which the bytes alone never name a source for.
//
// The same run lifts every instruction the CPU executes from the bytes it
// fetched — wherever they lay: the image through any mirror, work RAM, a byte
// the program rewrote — and holds that node to the machine through the
// differential's own check (`ir/ir_lockstep.h`), so every fact the run computes
// from a node is a fact the machine agreed with. From those nodes the run sees
// two more things. Every place the CPU arrived that the instruction before did
// not name — a return to an address the code itself put on the stack, an `RTI`
// into flow the bytes do not carry — is a landing, recorded with the mode the
// CPU arrived in, and the trace starts from it exactly as it starts from a
// reached target. And at every site in the image the run executed, the values
// the direct register and the data bank held are recorded, which is what the
// run saw against what every path proves.
//
// Beside the interpreter runs its shadow (`ir/ir_provenance.h`): every value
// carries the image offsets it was computed from, work RAM keeps the origin
// and the last writer of every byte, and the engines' and the port's moves
// keep the shadow current. So a range an engine carries out of work RAM — the
// tiles a routine decompressed, the sprite table a frame assembled — names the
// image bytes it was built from and the routine that built it, and a sequence
// of stores the CPU made to a data register from consecutive image bytes is
// recorded as the stream it is.
//
// A run sees what it exercised. Left alone, a cartridge reaches its title and
// its attract mode; with a recorded run replayed into its controller ports it
// reaches what a player does, and the trace follows.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/ir_provenance.h"
#include "rom/input_script.h"

namespace snaggletooth::disasm {

// One destination a run saw an indirect jump or call take: where it landed, in
// the bank the CPU arrived in — the disassembler places it to trace from it and
// to name it — the mode it arrived in, and the instruction that took it, as the
// tree places it. `call` says which of the four forms it was — a call's target
// is a routine, a jump's a location — which is what names the label.
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

// What a range of bytes was to the engine that moved it: a general-purpose
// transfer; an HDMA channel's table, read as the frame walked it — the line
// counts, a direct table's inline values, an indirect table's pointers; or the
// block an indirect entry pointed at. The last two are not an engine's: they
// name a lifted file whose bytes the CPU carried to a data register itself,
// one store at a time (`Stream`), or the image source of a range a routine
// built in work RAM before an engine carried it (`Staged`); no range an engine
// moved is ever of either kind.
enum class MovedKind : std::uint8_t { Dma, Table, Indirect, Stream, Staged };

// A kind as a manifest names it: `dma`, `table`, `indirect`, `stream`, `staged`.
[[nodiscard]] std::string_view movedKindName(MovedKind kind);

// How the memory address moved from one byte to the next, as the channel's
// `DMAP` said: up, down, or not at all — a fill from one byte, which is not a
// range of anything. A table and an indirect block always step up.
enum class MovedStep : std::uint8_t { Increment, Decrement, Fixed };

// A step as a manifest names it: `increment`, `decrement`, `fixed`.
[[nodiscard]] std::string_view movedStepName(MovedStep step);

// One contiguous range of bytes one channel moved under one trigger, as the
// engine performed it. `site` is the instruction that started it: the write to
// `MDMAEN` for a general-purpose transfer, the write to `HDMAEN` that enabled
// the channel for a table and its blocks. `registerAddress` is the B-bus
// register the channel's `BBAD` named, with its name and class where the
// address has one; a pattern that reaches a second register is implied by the
// count. `memory` is the A-bus address the range began at and `bytes` how many
// followed it under `step`. `times` is how many sightings of exactly this range
// the run made — a sprite table sent every frame is one range, seen once a frame.
struct MovedRange {
  Address site = 0;
  std::uint8_t channel = 0;
  bool toRegister = true;  // memory to the register; false for a read back into memory
  Address registerAddress = 0;
  std::string_view registerName;  // empty when no register has the address
  std::optional<RegisterClass> registerClass;
  Address memory = 0;
  MovedStep step = MovedStep::Increment;
  std::uint32_t bytes = 0;
  MovedKind kind = MovedKind::Dma;
  std::uint32_t times = 1;
};

// Two sightings are the same range when every field but the count agrees.
[[nodiscard]] bool sameRange(const MovedRange& a, const MovedRange& b);

// The lowest address a range covered: its memory address, or for a range read
// downward the address its last byte came from.
[[nodiscard]] Address extentStart(const MovedRange& range);

// The order ranges are reported and written in: by site, then channel, then
// memory address, then kind, then the longer first — so a table's blocks follow
// the table, and a walk the run's end cut short follows the whole one.
[[nodiscard]] bool rangeBefore(const MovedRange& a, const MovedRange& b);

// One place the CPU arrived that the instruction before it did not name: a
// return to an address the code itself put on the stack, an `RTI` into flow the
// bytes do not carry — any successor the node does not name that the four
// indirect forms do not cover. `target` is where it arrived, in the bank the CPU
// arrived in — the disassembler places it to trace from it — and `site` the
// instruction that took it, as the tree places it; `mode` is the mode the CPU
// arrived in. A landing outside the image is a note, not a landing. The
// name is what a reached target's is: `loc_` and the address, unless a person's
// entry names the same target under the same mode. Empty as the run reports it;
// the disassembler fills it in.
struct Landing {
  Address target = 0;
  Cpu65816Mode mode;
  Address site = 0;
  std::string name;
};

// Two landings are the same when the same site arrived at the same target under
// the same mode.
[[nodiscard]] bool sameLanding(const Landing& a, const Landing& b);

// The values the run saw at one site in the image, before the instruction
// there ran: every direct register and every data bank, each set in ascending
// order. A site the run executed under one direct register has one value.
struct SeenState {
  Address address = 0;
  std::vector<std::uint16_t> d;
  std::vector<std::uint8_t> dbr;
};

// One writer of a staged range's bytes — an instruction, as the tree places it,
// or the trigger of an engine — with how many of the range's bytes it wrote,
// over every sighting, the origin of those bytes together, and their sources:
// the runs of image bytes the writer's invocations read that hold the origin
// (`ir/ir_provenance.h`), ascending, no two touching. Bytes nothing wrote since
// power-on are counted under a writer that is `unwritten`.
struct StagedWriter {
  ir::Writer writer;
  bool unwritten = false;
  std::uint64_t bytes = 0;
  ir::OriginSet origin;
  std::vector<ir::OriginInterval> sources;
};

// One extent of work RAM an engine carried to a register, and where its bytes
// came from: the lowest address and the count, the origin of every byte
// together over every sighting of every range with that extent, and the
// writers, most bytes first. An extent whose origin is empty was built from
// constants alone.
struct StagedRange {
  Address memory = 0;
  std::uint32_t bytes = 0;
  ir::OriginSet origin;
  std::vector<StagedWriter> writers;
};

// Two staged ranges are the same extent when they begin at the same address
// and run for the same count.
[[nodiscard]] bool sameExtent(const StagedRange& a, const StagedRange& b);

// A stream the CPU carried a byte at a time: consecutive stores to one data
// register — `VMDATAL`/`VMDATAH` as one, `CGDATA`, `OAMDATA`, the audio ports
// in pairs — from values whose origins are consecutive image bytes, made at one
// site or by instructions one after another. `site` is the first store's, as
// the tree places it; `registerAddress` is the register the stream names, with
// its name and class; `romOffset` is the image offset of the first byte and
// `bytes` how many consecutive ones followed; `times` is how many sightings of
// exactly this stream the run made.
struct StreamedRange {
  Address site = 0;
  Address registerAddress = 0;
  std::string_view registerName;
  std::optional<RegisterClass> registerClass;
  std::size_t romOffset = 0;
  std::uint32_t bytes = 0;
  std::uint32_t times = 1;
};

// Two streams are the same when every field but the count agrees.
[[nodiscard]] bool sameStream(const StreamedRange& a, const StreamedRange& b);

// Everything one run recorded: the targets the indirect jumps took, in site
// order, then target order, each site/target/mode once; the ranges the engines
// moved, in `rangeBefore` order, each distinct range once with its count; the
// landings, in site order, then target order, each site/target/mode once; the
// values seen, in address order; the staged extents, in address order, then
// by count; the streams, in site order, then register, then offset; and what
// the run beside the interpreter checked. `divergences` counts the steps on
// which the node lifted from the fetches disagreed with the machine — each site
// once in the notes, the interpreter realigned after — and is zero on every
// cartridge the lift is right for. `originSets` is how many distinct origins
// the run interned, and `originCap` the cap above which one is widened.
struct RunObservation {
  std::vector<ReachedTarget> reached;
  std::vector<MovedRange> moved;
  std::vector<Landing> ran;
  std::vector<SeenState> seen;
  std::vector<StagedRange> staged;
  std::vector<StreamedRange> streamed;
  std::uint64_t instructions = 0;  // steps the interpreter ran a node for and checked
  std::uint64_t interrupts = 0;    // hardware sequences run and checked
  std::size_t nodes = 0;           // distinct nodes lifted from fetches: an address, a mode, the bytes
  std::uint64_t divergences = 0;
  std::size_t originSets = 0;
  std::size_t originCap = 0;
};

// Boots `rom` on the machine and steps it for `masterCycles` of the master clock,
// recording every distinct target the four indirect forms took, every range
// the transfer engines moved, every landing the instructions did not name, the
// direct register and data bank at every site executed in the image, where
// every range carried out of work RAM came from, and every stream the CPU
// carried — with every executed instruction lifted from its fetches and checked
// against the machine, its shadow beside it. A site whose pointer lies where the run cannot read it — anything
// but the image and work RAM — is named once in `notes` and produces nothing;
// so is a site whose pointer did not match where the CPU then went, which the
// design does not expect and reports rather than hides. A range is recorded
// wherever its memory address lies: the engine addressed it, and nothing here
// needs to read it. A landing outside the image is named once in `notes` and
// not recorded: the tree has nothing to trace there. A step on which the lifted
// node disagreed with the machine is named once per site in `notes`.
//
// `input` is replayed into the controller ports as the run goes: at the start
// of every frame, counted from power-on, each port is given what the script
// holds for it there. An empty script leaves both ports empty.
[[nodiscard]] RunObservation observeRun(std::span<const std::uint8_t> rom,
                                        std::uint64_t masterCycles, const InputScript& input,
                                        std::vector<std::string>& notes);

}  // namespace snaggletooth::disasm
