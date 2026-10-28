#include "rom/rom_enumerate.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "ir/ir_dataflow.h"
#include "ir/ir_provenance.h"
#include "rom/rom_text.h"

namespace snaggletooth::disasm {
namespace {

using text::address24;

// The address the tree places the bytes at `address` at: the one home of the
// image bytes it reads. Nothing for an address outside the image.
std::optional<Address> placedInImage(const CartridgeBoard& board, std::size_t imageBytes, Address address) {
  const std::optional<std::size_t> offset = romOffset(board, address, imageBytes);
  if (!offset) return std::nullopt;
  const std::optional<std::uint32_t> home = romAddress(board.map, *offset);
  if (!home) return std::nullopt;
  return *home;
}

// Every address a call lands on: the program's call nodes with a target, and
// the calls a run reached or the bytes derived, each where the tree places it.
std::set<Address> calledAddresses(const CartridgeDisassembly& disassembly) {
  const CartridgeBoard& board = disassembly.board;
  const std::size_t imageBytes = disassembly.imageBytes;
  std::set<Address> out;
  const auto add = [&](Address target) {
    if (const std::optional<Address> home = placedInImage(board, imageBytes, target)) out.insert(*home);
  };
  for (const ir::Node& node : disassembly.program.nodes) {
    if (node.instruction.flow == ir::Flow::Call && node.instruction.target) add(*node.instruction.target);
  }
  for (const ReachedTarget& reached : disassembly.reached) {
    if (reached.call) add(reached.target);
  }
  for (const DerivedTarget& derived : disassembly.derived) {
    if (derived.call) add(derived.target);
  }
  return out;
}

// One register as an input: at 16 bits from the whole register, at 8 from its
// low byte, and nothing where the mode does not settle the width or every path
// proves fewer than two values.
std::optional<EnumerationInput> inputOf(InputRegister reg, bool wide, bool settled, const ir::Values& whole,
                                        const ir::Values& low) {
  if (!settled) return std::nullopt;
  const ir::Values& values = wide ? whole : low;
  if (!values.known || values.values.size() < 2) return std::nullopt;
  return EnumerationInput{.reg = reg, .bits = wide ? 16u : 8u, .values = values.values};
}

// The bytes of A, X and Y as six bits: A's low byte 1 and high 2, X's 4 and 8, Y's 16 and 32.
using LiveBytes = std::uint8_t;
constexpr LiveBytes kEveryByte = 0x3Fu;

// What one node does to those bytes: the bytes it reads before any write of its own, and the bytes it leaves written.
struct NodeUse {
  LiveBytes reads = 0;
  LiveBytes writes = 0;
};

// The two bytes of A, X or Y; nothing for any other place.
LiveBytes bytesOf(ir::Place place) {
  switch (place) {
    case ir::Place::A: return 0x03u;
    case ir::Place::X: return 0x0Cu;
    case ir::Place::Y: return 0x30u;
    default: return 0;
  }
}

// Whether an effect of `width` under the node's mode works on the low byte
// alone: `Byte`, or `ByM`/`ByX` where the mode settles eight bits.
bool lowByteOnly(const ir::Node& node, ir::Width width) {
  const ir::Mode& mode = node.mode;
  switch (width) {
    case ir::Width::Byte: return true;
    case ir::Width::ByM: return mode.emulation || (mode.accumulatorKnown && mode.accumulator8);
    case ir::Width::ByX: return mode.emulation || (mode.indexKnown && mode.index8);
    default: return false;
  }
}

// Whether an effect of `width` under the node's mode works on both bytes as
// far as the mode settles it: `Word`, `Long`, or `ByM`/`ByX` at a known sixteen.
bool bothBytesSettled(const ir::Node& node, ir::Width width) {
  const ir::Mode& mode = node.mode;
  switch (width) {
    case ir::Width::Word:
    case ir::Width::Long: return true;
    case ir::Width::ByM: return !mode.emulation && mode.accumulatorKnown && !mode.accumulator8;
    case ir::Width::ByX: return !mode.emulation && mode.indexKnown && !mode.index8;
    default: return false;
  }
}

// The bytes a read of `place` at `width` takes: the low byte where the width
// is eight bits, both where it is sixteen or the mode does not settle it.
LiveBytes readBytes(const ir::Node& node, ir::Place place, ir::Width width) {
  const LiveBytes both = bytesOf(place);
  return lowByteOnly(node, width) ? static_cast<LiveBytes>(both & 0x15u) : both;
}

// The bytes a write to `place` at `width` leaves written. An index register
// written at any width loses both bytes, as the chip clears the high byte
// under an eight-bit index; the accumulator loses its high byte only where the
// width is settled at sixteen bits.
LiveBytes writtenBytes(const ir::Node& node, ir::Place place, ir::Width width) {
  const LiveBytes both = bytesOf(place);
  if (place != ir::Place::A) return both;
  return bothBytesSettled(node, width) ? both : static_cast<LiveBytes>(both & 0x01u);
}

[[nodiscard]] NodeUse nodeUse(const ir::Node& node) {
  NodeUse use;
  LiveBytes written = 0;
  const auto read = [&](LiveBytes bytes) { use.reads |= static_cast<LiveBytes>(bytes & ~written); };
  for (const ir::Effect& effect : node.effects) {
    read(readBytes(node, effect.a.place, effect.width));
    read(readBytes(node, effect.b.place, effect.width));
    if (effect.when.when == ir::When::PlaceIs || effect.when.when == ir::When::PlaceIsNot) {
      read(readBytes(node, effect.when.place, effect.width));
    }
    if (effect.op == ir::Op::Xba) read(bytesOf(ir::Place::A));
    if (effect.when.when == ir::When::Always) written |= writtenBytes(node, effect.dst.place, effect.width);
  }
  use.writes = written;
  return use;
}

// The routine whose label is at `address`, or nothing.
const Routine* routineAtAddress(const CartridgeDisassembly& disassembly, Address address) {
  const std::vector<Routine>& routines = disassembly.routines;
  const auto found = std::lower_bound(routines.begin(), routines.end(), address,
                                      [](const Routine& routine, Address at) { return routine.address < at; });
  return found != routines.end() && found->address == address ? &*found : nullptr;
}

// The bytes some path from the routine's entry reads before writing, over the routine's own nodes and its callees' entries.
[[nodiscard]] LiveBytes liveAtEntry(const CartridgeDisassembly& disassembly, const Routine& routine,
                                    std::map<Address, LiveBytes>& known, std::set<Address>& visiting) {
  if (const auto memo = known.find(routine.address); memo != known.end()) return memo->second;
  if (visiting.count(routine.address) != 0) return kEveryByte;
  visiting.insert(routine.address);

  const CartridgeBoard& board = disassembly.board;
  const std::size_t imageBytes = disassembly.imageBytes;
  const std::vector<ir::Node>& nodes = disassembly.program.nodes;
  const std::set<Address> lines(routine.lines.begin(), routine.lines.end());

  // The routine's nodes, every mode at each of its lines, by index into `nodes`.
  std::vector<std::size_t> own;
  std::map<std::size_t, std::size_t> slotOf;
  const auto nodesAt = [&](Address address) {
    std::vector<std::size_t> out;
    auto at = std::lower_bound(nodes.begin(), nodes.end(), address,
                               [](const ir::Node& node, Address wanted) { return node.instruction.address < wanted; });
    for (; at != nodes.end() && at->instruction.address == address; ++at) {
      out.push_back(static_cast<std::size_t>(at - nodes.begin()));
    }
    return out;
  };
  for (const Address line : lines) {
    for (const std::size_t index : nodesAt(line)) {
      slotOf.emplace(index, own.size());
      own.push_back(index);
    }
  }

  // Each node's successors inside the routine, and the bytes every path
  // leaving it otherwise reads.
  struct Edges {
    std::vector<std::size_t> next;
    LiveBytes beyond = 0;
  };
  std::vector<Edges> edges(own.size());
  std::vector<NodeUse> uses(own.size());
  for (std::size_t slot = 0; slot < own.size(); ++slot) {
    const ir::Node& node = nodes[own[slot]];
    const ir::Instruction& instruction = node.instruction;
    Edges& out = edges[slot];
    uses[slot] = nodeUse(node);
    const auto to = [&](std::optional<Address> address) {
      if (!address || lines.count(*address) == 0) {
        out.beyond = kEveryByte;
        return;
      }
      const std::vector<std::size_t> at = nodesAt(*address);
      if (at.empty()) out.beyond = kEveryByte;
      for (const std::size_t index : at) out.next.push_back(slotOf.at(index));
    };
    const auto placed = [&](Address address) { return placedInImage(board, imageBytes, address); };
    const Address after = instruction.address + instruction.length;
    switch (instruction.flow) {
      case ir::Flow::Continue: to(after); break;
      case ir::Flow::Branch:
        to(after);
        to(instruction.target ? placed(*instruction.target) : std::nullopt);
        break;
      case ir::Flow::Jump: {
        if (instruction.target) {
          to(placed(*instruction.target));
          break;
        }
        bool any = false;
        const auto fromHere = [&](Address site, Address target, bool call) {
          if (call || placed(site) != std::optional<Address>{instruction.address}) return;
          any = true;
          to(placed(target));
        };
        for (const ReachedTarget& reached : disassembly.reached) fromHere(reached.site, reached.target, reached.call);
        for (const DerivedTarget& derived : disassembly.derived) fromHere(derived.site, derived.target, derived.call);
        if (!any) out.beyond = kEveryByte;
        break;
      }
      case ir::Flow::Call: {
        to(after);
        if (instruction.mnemonic == "BRK" || instruction.mnemonic == "COP") {
          out.beyond = kEveryByte;
          break;
        }
        std::vector<std::optional<Address>> targets;
        if (instruction.target) {
          targets.push_back(placed(*instruction.target));
        } else {
          const auto fromHere = [&](Address site, Address target, bool call) {
            if (call && placed(site) == std::optional<Address>{instruction.address}) targets.push_back(placed(target));
          };
          for (const ReachedTarget& reached : disassembly.reached) fromHere(reached.site, reached.target, reached.call);
          for (const DerivedTarget& derived : disassembly.derived) fromHere(derived.site, derived.target, derived.call);
        }
        if (targets.empty()) out.beyond = kEveryByte;
        for (const std::optional<Address>& target : targets) {
          const Routine* callee = target ? routineAtAddress(disassembly, *target) : nullptr;
          out.beyond |= callee != nullptr ? liveAtEntry(disassembly, *callee, known, visiting) : kEveryByte;
        }
        break;
      }
      case ir::Flow::Return:
      case ir::Flow::Halt: break;
    }
  }

  // The backward fixed point: what is live into a node is what it reads, and
  // what is live out of it that it does not write.
  std::vector<LiveBytes> liveIn(own.size(), 0);
  for (bool changed = true; changed;) {
    changed = false;
    for (std::size_t slot = own.size(); slot-- > 0;) {
      LiveBytes liveOut = edges[slot].beyond;
      for (const std::size_t next : edges[slot].next) liveOut |= liveIn[next];
      const LiveBytes in = static_cast<LiveBytes>(uses[slot].reads | (liveOut & ~uses[slot].writes));
      if (in != liveIn[slot]) {
        liveIn[slot] = in;
        changed = true;
      }
    }
  }

  LiveBytes answer = 0;
  const std::vector<std::size_t> entry = nodesAt(routine.address);
  if (entry.empty()) answer = kEveryByte;
  for (const std::size_t index : entry) {
    const auto slot = slotOf.find(index);
    answer |= slot != slotOf.end() ? liveIn[slot->second] : kEveryByte;
  }
  visiting.erase(routine.address);
  known.emplace(routine.address, answer);
  return answer;
}

// Whether a live mask holds a byte of an input at its width.
bool readsInput(LiveBytes live, const EnumerationInput& input) {
  const LiveBytes both = bytesOf(input.reg == InputRegister::A   ? ir::Place::A
                                 : input.reg == InputRegister::X ? ir::Place::X
                                                                 : ir::Place::Y);
  const LiveBytes width = input.bits == 16u ? both : static_cast<LiveBytes>(both & 0x15u);
  return (live & width) != 0;
}

// The lowest address a transfer the code proves reads, and how an address
// compares with it: as a work-RAM index where it lies in work RAM, so a mirror
// and its home compare equal, and as itself elsewhere.
Address lowestRead(const DmaTransfer& dma) {
  return *dma.step == MovedStep::Decrement
             ? (*dma.source & 0xFF0000u) | static_cast<std::uint16_t>((*dma.source & 0xFFFFu) - (*dma.bytes - 1u))
             : *dma.source;
}
std::uint64_t extentKey(Address address) {
  const std::optional<std::size_t> index = ir::Provenance::workRamIndex(address);
  return index ? *index : (std::uint64_t{1} << 32) | address;
}

// Whether the run started the transfer the code proves: a range it moved to a
// register on the transfer's channel from its start site, whose lowest
// address, count and step are the transfer's.
bool confirmedByTheRun(const CartridgeDisassembly& disassembly, const DmaTransfer& dma) {
  const std::optional<Address> start =
      placedInImage(disassembly.board, disassembly.imageBytes, dma.startSite.value_or(dma.site));
  if (!start) return false;
  const std::uint64_t low = extentKey(lowestRead(dma));
  return std::any_of(disassembly.moved.begin(), disassembly.moved.end(), [&](const MovedRange& range) {
    return range.channel == dma.channel && range.toRegister &&
           placedInImage(disassembly.board, disassembly.imageBytes, range.site) == start &&
           extentKey(extentStart(range)) == low && range.bytes == *dma.bytes && range.step == *dma.step;
  });
}

}  // namespace

EnumerationPlan enumerationPlan(const CartridgeDisassembly& disassembly, const ProvenProgram& proven,
                                std::vector<std::string>& notes) {
  const std::map<Address, std::string> routineOf = routineByLine(disassembly);
  std::map<std::string, Address> addressOf;
  for (const Routine& routine : disassembly.routines) addressOf.emplace(routine.label, routine.address);

  // The writers: the routine holding each site that wrote a staged extent's
  // bytes, and the sites no routine holds.
  std::set<Address> members;
  std::set<Address> bareSites;
  for (const StagedRange& range : disassembly.staged) {
    for (const StagedWriter& writer : range.writers) {
      if (writer.writer.engine || writer.unwritten) continue;
      const auto held = routineOf.find(writer.writer.site);
      if (held == routineOf.end()) {
        bareSites.insert(writer.writer.site);
        continue;
      }
      members.insert(addressOf.at(held->second));
    }
  }

  // The closure: every routine whose calls reach a member.
  for (bool added = true; added;) {
    added = false;
    for (const Routine& routine : disassembly.routines) {
      if (members.count(routine.address) != 0) continue;
      const bool reaches = std::any_of(routine.calls.begin(), routine.calls.end(),
                                       [&](Address callee) { return members.count(callee) != 0; });
      if (reaches) {
        members.insert(routine.address);
        added = true;
      }
    }
  }

  for (const Address site : bareSites) {
    notes.push_back("enumerate " + address24(site) + ": no routine holds the site; not run");
  }

  const std::set<Address> called = calledAddresses(disassembly);
  std::map<Address, LiveBytes> known;
  std::set<Address> visiting;
  EnumerationPlan plan;
  plan.recordedClasses = stagedClassBits(disassembly);
  for (const Routine& routine : disassembly.routines) {
    if (members.count(routine.address) == 0) continue;
    const std::string head = "enumerate " + routine.label + ": ";
    if (called.count(routine.address) == 0) {
      notes.push_back(head + "entered by a jump, no return to end at; not run");
      continue;
    }
    const ir::State* state = proven.flow->before(routine.address);
    if (state == nullptr) {
      notes.push_back(head + "no path the analysis follows reaches its entry; not run");
      continue;
    }
    const std::vector<ir::Node>& nodes = disassembly.program.nodes;
    const auto entry = std::lower_bound(nodes.begin(), nodes.end(), routine.address,
                                        [](const ir::Node& node, Address at) { return node.instruction.address < at; });
    const ir::Mode mode = entry != nodes.end() && entry->instruction.address == routine.address ? entry->mode
                                                                                               : ir::Mode{};
    const ir::RegisterState& r = state->registers;
    PlannedRoutine planned{.address = routine.address, .label = routine.label, .inputs = {}};
    const bool accumulatorWide = mode.accumulatorKnown && !mode.accumulator8 && !mode.emulation;
    const bool accumulatorSettled = mode.accumulatorKnown || mode.emulation;
    const bool indexWide = mode.indexKnown && !mode.index8 && !mode.emulation;
    const bool indexSettled = mode.indexKnown || mode.emulation;
    std::vector<EnumerationInput> bounded;
    for (const std::optional<EnumerationInput>& input :
         {inputOf(InputRegister::A, accumulatorWide, accumulatorSettled, r.a(), r.aLow),
          inputOf(InputRegister::X, indexWide, indexSettled, r.x(), r.xLow),
          inputOf(InputRegister::Y, indexWide, indexSettled, r.y(), r.yLow)}) {
      if (input) bounded.push_back(*input);
    }
    if (bounded.empty()) {
      notes.push_back(head + "no input every path bounds; not run");
      continue;
    }
    const LiveBytes live = liveAtEntry(disassembly, routine, known, visiting);
    for (EnumerationInput& input : bounded) {
      if (readsInput(live, input)) {
        planned.inputs.push_back(std::move(input));
      } else {
        notes.push_back(head + std::string(inputRegisterName(input.reg)) +
                        " is bounded on every path and read on none before it is written; not an input");
      }
    }
    if (planned.inputs.empty()) continue;
    plan.routines.push_back(std::move(planned));
  }
  return plan;
}

void noteProvenFromWorkRam(CartridgeDisassembly& disassembly) {
  const CartridgeBoard& board = disassembly.board;
  for (const DmaTransfer& dma : disassembly.dmas) {
    if (dma.direction != DmaDirection::ToBBus || !dma.startMask || !dma.startSite || dma.hdma) continue;
    if (!dma.source || !dma.bytes || !dma.step || *dma.step == MovedStep::Fixed) continue;
    if (romOffset(board, *dma.source, disassembly.imageBytes)) continue;
    if (confirmedByTheRun(disassembly, dma)) continue;
    // The extent the transfer reads, lowest byte first.
    const std::uint64_t first = extentKey(lowestRead(dma));
    const std::uint64_t end = first + *dma.bytes;
    const StagedRange* written = nullptr;
    for (const StagedRange& range : disassembly.staged) {
      const std::uint64_t from = extentKey(range.memory);
      if (from < end && first < from + range.bytes) {
        written = &range;
        break;
      }
    }
    const std::string head = "dma " + address24(dma.site) + " channel " + std::to_string(dma.channel) +
                             ": the code proves " + address24(*dma.source) + " " +
                             std::string(movedStepName(*dma.step)) + " bytes " + std::to_string(*dma.bytes) + "; ";
    disassembly.notes.push_back(written != nullptr
                                    ? head + "the run wrote the extent and never started this transfer; its sources "
                                             "are the staged lines at " +
                                          address24(written->memory)
                                    : head + "nothing wrote the extent in the run; nothing to run from");
  }
}

}  // namespace snaggletooth::disasm
