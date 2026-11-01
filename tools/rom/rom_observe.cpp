#include "rom/rom_observe.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

#include "ir/spc700_lift.h"
#include "rom/rom_observe_internal.h"
#include "snaggletooth/apu/dsp.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {
namespace {

using namespace detail;

std::optional<std::uint16_t> readWord(const CartridgeBoard& board, std::span<const std::uint8_t> rom,
                                      const SnesState& state, Address address) {
  const std::optional<std::uint8_t> low = readByte(board, rom, state, address);
  const std::optional<std::uint8_t> high = readByte(board, rom, state, address + 1u);
  if (!low || !high) return std::nullopt;
  return static_cast<std::uint16_t>(*low | (*high << 8));
}

// Where the instruction at the CPU's position is about to go, when it is one of
// the four forms whose pointer the bytes do not name — read the way the CPU is
// about to read it. Nothing for any other instruction; nothing, with `unreadable`
// set, when the pointer lies where the run cannot see.
struct Pending {
  Address target = 0;
  bool call = false;
};

std::optional<Pending> pendingTarget(const CartridgeBoard& board, std::span<const std::uint8_t> rom,
                                     const SnesState& state, bool& unreadable) {
  const Cpu65816State& cpu = state.cpu;
  const Address site = (static_cast<Address>(cpu.pbr) << 16) | cpu.pc;
  const std::optional<std::uint8_t> opcode = readByte(board, rom, state, site);
  if (!opcode) return std::nullopt;
  const Cpu65816Opcode& info = cpu65816Opcodes()[*opcode];
  const bool indirect = info.mode == Cpu65816Addressing::AbsoluteIndirect ||
                        info.mode == Cpu65816Addressing::AbsoluteIndirectLong ||
                        info.mode == Cpu65816Addressing::AbsoluteIndexedIndirect;
  if (!indirect || (info.flow != Flow::Jump && info.flow != Flow::Call)) return std::nullopt;

  // The operand follows the opcode within the program bank.
  const Address operandAt = (site & 0xFF0000u) | ((cpu.pc + 1u) & 0xFFFFu);
  const std::optional<std::uint16_t> operand = readWord(board, rom, state, operandAt);
  if (!operand) {
    unreadable = true;
    return std::nullopt;
  }

  const Address programBank = site & 0xFF0000u;
  std::optional<Address> target;
  switch (info.mode) {
    // `(!abs)`: a two-byte pointer in bank zero; the program bank is unchanged.
    case Cpu65816Addressing::AbsoluteIndirect:
      if (const std::optional<std::uint16_t> ptr = readWord(board, rom, state, *operand)) {
        target = programBank | *ptr;
      }
      break;
    // `[!abs]`: a three-byte pointer in bank zero; the third byte is the bank.
    case Cpu65816Addressing::AbsoluteIndirectLong: {
      const std::optional<std::uint16_t> low = readWord(board, rom, state, *operand);
      const std::optional<std::uint8_t> bank = readByte(board, rom, state, *operand + 2u);
      if (low && bank) target = (static_cast<Address>(*bank) << 16) | *low;
      break;
    }
    // `(!abs,X)`: the operand plus X addresses a two-byte pointer in the program
    // bank, wrapping within it; the program bank is unchanged.
    case Cpu65816Addressing::AbsoluteIndexedIndirect: {
      const Address pointerAt = programBank | ((*operand + cpu.x) & 0xFFFFu);
      if (const std::optional<std::uint16_t> ptr = readWord(board, rom, state, pointerAt)) {
        target = programBank | *ptr;
      }
      break;
    }
    default:
      break;
  }
  if (!target) {
    unreadable = true;
    return std::nullopt;
  }
  return Pending{.target = *target, .call = info.flow == Flow::Call};
}

// The cap on an origin's intervals, above which it is widened to its hull:
// set by exactness — raised until no staged range on a real cartridge is
// approximate — and never by cost.
constexpr std::size_t kOriginCap = 64;

// The sound interpreter run beside the audio machine over the whole run. At
// every boundary the machine reports, the instruction about to run is decoded
// from the bytes at the program counter as the machine holds them then — the
// image mapped over the boot-ROM window included — lifted, and held until the
// boundary after says what the machine did: the accesses between, with the
// instruction's own fetches set apart, the registers after and the cycles.
// The fetched bytes are held to the decoded ones too, so an instruction whose
// bytes changed between the boundary and its fetch is noted rather than
// checked against the wrong node.
//
// The same accesses keep a copy of the DSP's register file: a write to `$F2`
// selects a register and a write to `$F3` sets it, and a write to `KON` with a
// bit set is a key-on of each voice named. At each key-on the sample the DSP is
// about to play is read from the audio memory as the machine holds it then —
// the directory entry `DIR` and the voice's `SRCN` select, through the
// machine's own reading of it, and the blocks from its start to the first
// carrying the end flag, stopping at the memory's end — and kept once per
// distinct sample with a count.
struct AudioLockstep final : ApuObserver {
  // The DSP's address register and its register file as the CPU last wrote
  // them; a write to `$F3` while the address is $80 or above sets nothing,
  // since those are the file's read-only mirrors.
  static constexpr std::uint8_t kDspRegisters = 0x80u;
  static constexpr std::uint16_t kDspAddressPort = 0x00F2u;
  static constexpr std::uint16_t kDspDataPort = 0x00F3u;
  static constexpr std::uint8_t kDspKon = 0x4Cu;
  static constexpr std::uint8_t kDspDir = 0x5Du;
  static constexpr std::uint8_t kDspSrcn = 0x04u;  // voice v's source number is at v * $10 + 4
  static constexpr std::size_t kAudioMemory = 65536;
  static constexpr std::size_t kBrrBlock = 9;

  const Snes& machine;
  std::vector<std::string>& notes;
  ir::Spc700Interpreter interpreter;
  std::vector<ir::Spc700Access> accesses;  // since the last boundary, in order
  std::uint8_t dspAddress = 0;
  std::array<std::uint8_t, kDspRegisters> dsp{};
  std::vector<KeyedSample> samples;
  std::set<std::uint16_t> unendedStarts;  // a start already noted as having no end flag

  // The nodes lifted so far, one per address and bytes — so bytes the program
  // rewrote at an address are a second node there.
  using NodeKey = std::pair<std::uint16_t, std::vector<std::uint8_t>>;
  std::map<NodeKey, ir::Node> nodes;

  // The instruction about to run, decoded at the last boundary; nothing when
  // the core is halted or the bytes there do not decode.
  struct Pending {
    std::uint16_t pc = 0;
    std::vector<std::uint8_t> bytes;
    const ir::Node* node = nullptr;
  };
  std::optional<Pending> pending;

  std::set<std::uint16_t> notedSites;  // a site already named in the notes
  std::vector<ir::Divergence> divergences;
  std::uint64_t instructions = 0;
  std::uint64_t diverged = 0;
  std::uint64_t steps = 0;

  AudioLockstep(const Snes& m, std::vector<std::string>& n) : machine(m), notes(n) {
    interpreter.registers = ir::registersOf(m.state().apu.cpu);
    prepare(m.state().apu.cpu);
  }

  void access(std::uint16_t address, std::uint8_t value, bool write) override {
    accesses.push_back(ir::Spc700Access{.address = address, .value = value, .write = write});
    if (!write) return;
    if (address == kDspAddressPort) {
      dspAddress = value;
    } else if (address == kDspDataPort && dspAddress < kDspRegisters) {
      dsp[dspAddress] = value;
      if (dspAddress == kDspKon && value != 0u) keyOn(value);
    }
  }

  // Each voice `mask` names keyed on: its sample read from the audio memory as
  // the machine holds it now — the memory itself, which is what the DSP reads,
  // not the CPU's view with the boot ROM over its window — and kept once with
  // its count.
  void keyOn(std::uint8_t mask) {
    const std::span<const std::uint8_t, kAudioMemory> ram(machine.state().apu.ram);
    for (unsigned voice = 0; voice < 8; ++voice) {
      if ((mask & (1u << voice)) == 0u) continue;
      const std::uint8_t srcn = dsp[(voice << 4) | kDspSrcn];
      const BrrSource source = readBrrSource(ram, dsp[kDspDir], srcn);
      KeyedSample sample{.start = source.start, .loop = source.loop, .bytes = {}, .times = 1};
      // The blocks from the start to the first carrying the end flag; a walk
      // that reaches the end of the audio memory without one names nothing.
      bool ended = false;
      std::size_t at = source.start;
      while (at + kBrrBlock <= kAudioMemory) {
        const std::uint8_t header = ram[at];
        for (std::size_t k = 0; k < kBrrBlock; ++k) sample.bytes.push_back(ram[at + k]);
        at += kBrrBlock;
        if ((header & 0x01u) != 0u) {
          ended = true;
          break;
        }
      }
      if (!ended) {
        if (unendedStarts.insert(source.start).second) {
          notes.push_back("run: the sample at " + formatAddress(source.start, 16) + " voice " +
                          std::to_string(voice) +
                          " keyed on reaches the end of the audio memory without an end flag; not recorded");
        }
        continue;
      }
      const auto known = std::find_if(samples.begin(), samples.end(),
                                      [&](const KeyedSample& s) { return sameSample(s, sample); });
      if (known == samples.end()) {
        samples.push_back(std::move(sample));
      } else {
        ++known->times;
      }
    }
  }

  void instruction(const Spc700State& before, const Spc700State& after,
                   std::uint32_t cycles) override {
    const std::uint64_t ordinal = steps++;
    if (before.run == RunState::Running) {
      step(ordinal, before, after, cycles);
    } else if (after.run == RunState::Running) {
      // Nothing ends a halt but a reset; the interpreter follows the machine.
      interpreter.registers = ir::registersOf(after);
    }
    accesses.clear();
    prepare(after);
  }

  // Decodes and lifts the instruction at `state`'s program counter from the
  // bytes the machine holds there now, ahead of the step that runs it.
  void prepare(const Spc700State& state) {
    pending.reset();
    if (state.run != RunState::Running) return;
    std::array<std::uint8_t, 3> window{};
    for (std::size_t i = 0; i < window.size(); ++i) {
      window[i] = machine.peekApu(static_cast<std::uint16_t>(state.pc + i));
    }
    const std::optional<Instruction> decoded = decodeAt(window, state.pc, state.pc);
    if (!decoded) return;
    Pending next;
    next.pc = state.pc;
    next.bytes = decoded->bytes;
    const NodeKey key{state.pc, next.bytes};
    auto found = nodes.find(key);
    if (found == nodes.end()) found = nodes.emplace(key, ir::liftSpc700Instruction(*decoded)).first;
    next.node = &found->second;
    pending = std::move(next);
  }

  // One instruction the machine ran, from the boundary before it to the one after.
  void step(std::uint64_t ordinal, const Spc700State& before, const Spc700State& after,
            std::uint32_t cycles) {
    const std::uint16_t site = before.pc;
    // A reload moved the CPU since the last boundary: the bytes now at the
    // program counter are the best reading there is.
    if (!pending || pending->pc != site) prepare(before);
    if (!pending) {
      note(site, "run: the bytes at " + formatAddress(site, 16) +
                     " on the sound CPU do not decode as one instruction; the step is not checked");
      interpreter.registers = ir::registersOf(after);
      return;
    }
    const std::optional<ir::Spc700StepAccesses> split =
        ir::splitSpc700Step(accesses, site, pending->bytes.size());
    bool fetchedAsDecoded = split.has_value();
    if (split) {
      for (std::size_t i = 0; i < split->fetches.size(); ++i) {
        if (split->fetches[i].value != pending->bytes[i]) fetchedAsDecoded = false;
      }
    }
    if (!fetchedAsDecoded) {
      note(site, "run: the sound CPU's step at " + formatAddress(site, 16) +
                     " did not fetch the instruction decoded there; the step is not checked");
      interpreter.registers = ir::registersOf(after);
      return;
    }

    ir::Divergence prototype;
    prototype.instruction = ordinal;
    prototype.site = site;
    const std::size_t divergencesBefore = divergences.size();
    ir::checkSpc700Node(interpreter, *pending->node, split->data, cycles, after, prototype,
                        divergences);
    ++instructions;
    if (divergences.size() != divergencesBefore) {
      ++diverged;
      const ir::Divergence& d = divergences.back();
      char values[64];
      std::snprintf(values, sizeof values, "machine $%X, the lift $%X", d.expected, d.actual);
      note(site, "run: the lift of `" + std::string(pending->node->instruction.mnemonic) + " " +
                     std::string(pending->node->instruction.form) + "` at " +
                     formatAddress(site, 16) + " on the sound CPU disagreed with the machine (" +
                     d.what + ": " + values + "); the interpreter was realigned");
    }
  }

  // A note about a site, said once.
  void note(std::uint16_t site, std::string text) {
    if (notedSites.insert(site).second) notes.push_back(std::move(text));
  }
};

// The scratch a routine's kept values wrote, as a note names it: the maximal
// runs of set indices in address order, `$BB:XXXX` for one byte and
// `$BB:XXXX-$BB:XXXX` for more, joined by `, `.
std::string scratchRanges(const std::vector<bool>& scratch) {
  std::string out;
  for (std::size_t i = 0; i < scratch.size();) {
    if (!scratch[i]) {
      ++i;
      continue;
    }
    std::size_t end = i;
    while (end < scratch.size() && scratch[end]) ++end;
    if (!out.empty()) out += ", ";
    out += text::address24(ir::Provenance::workRamAddress(i));
    if (end - i > 1u) out += "-" + text::address24(ir::Provenance::workRamAddress(end - 1u));
    i = end;
  }
  return out;
}

// The second run's end. The run is held to the first's counts, and nothing
// runs when they differ. Otherwise every planned routine the run entered runs
// once for each value of each of its inputs, on one second machine restored
// from the routine's snapshot each time; a routine the run never entered is
// noted, its values counted as planned. A value whose extents equal, field for
// field, those of an outcome of the same routine, register, width and call
// joins that outcome. After a routine's values, the scratch its kept values
// wrote is noted once.
void enumerate(RunObservation& observation, const Enumerating& enumerating, std::span<const std::uint16_t> recordedClasses,
               const Lockstep& lockstep, std::span<const std::uint8_t> rom, const CartridgeBoard& board,
               const InputScript& input, std::vector<std::string>& notes, const ProgressSink& progress,
               std::uint64_t firstInstructions, std::uint64_t firstDivergences) {
  if (lockstep.instructions != firstInstructions || lockstep.diverged != firstDivergences) {
    notes.push_back("run: the second run executed " + std::to_string(lockstep.instructions) + " instructions and " +
                    std::to_string(lockstep.diverged) + " divergences where the first executed " +
                    std::to_string(firstInstructions) + " and " + std::to_string(firstDivergences) +
                    "; nothing enumerated");
    return;
  }
  Snes sandbox{SnesConfig{.rom = rom}};
  for (const Planned& planned : enumerating.planned) {
    const PlannedRoutine& routine = *planned.routine;
    if (!planned.snapshot) {
      notes.push_back("enumerate " + routine.label + ": the second run did not enter it; nothing run");
      for (const EnumerationInput& in : routine.inputs) observation.variantsPlanned += in.values.size();
      continue;
    }
    const Snapshot& snapshot = *planned.snapshot;
    std::vector<bool> scratch(0x20000u, false);
    std::uint64_t wroteScratch = 0;
    for (const EnumerationInput& in : routine.inputs) {
      for (const std::uint32_t value : in.values) {
        ++observation.variantsPlanned;
        const VariantSetting setting{.reg = in.reg, .bits = in.bits, .value = value};
        SnesState state = snapshot.state;
        setInput(state, setting);
        if (!sandbox.restore(state)) throw std::logic_error("a snapshot's save is not the cartridge's size");
        sandbox.setApuObserver(nullptr);
        ir::Provenance copy = snapshot.shadow;
        Recorder rec{sandbox, copy};
        copy.carries = &rec;
        sandbox.setObserver(&rec);
        Lockstep step{board, rom.size(), state.cpu, copy};
        step.expectedReturns.insert(snapshot.returnTo);
        step.expectedNext = (static_cast<Address>(state.cpu.pbr) << 16) | state.cpu.pc;
        ++observation.variantsRun;
        bool wrote = false;
        std::optional<EnumeratedRun> run =
            runVariant(snapshot, planned, recordedClasses, scratch, wrote, setting, sandbox, step, rec, copy, rom, board,
                       input, routine.label, notes, observation.enumerationCycles);
        sandbox.setObserver(nullptr);
        if (progress) progress(Progress{.stage = "enumerating", .spent = 0, .budget = 0});
        if (!run) continue;
        ++observation.variantsReturned;
        ++observation.variantsConfined;
        if (wrote) ++wroteScratch;
        if (run->extents.empty()) continue;
        const auto same = std::find_if(observation.enumerated.begin(), observation.enumerated.end(),
                                       [&](const EnumeratedRun& outcome) {
                                         return outcome.routine == run->routine && outcome.reg == run->reg &&
                                                outcome.bits == run->bits && outcome.enteredFrom == run->enteredFrom &&
                                                outcome.extents == run->extents;
                                       });
        if (same != observation.enumerated.end()) {
          same->values.push_back(value);
        } else {
          observation.enumerated.push_back(std::move(*run));
        }
      }
    }
    if (wroteScratch > 0) {
      notes.push_back("enumerate " + routine.label + ": " + std::to_string(wroteScratch) +
                      (wroteScratch == 1u ? " value" : " values") + " wrote " + scratchRanges(scratch) +
                      ", extents the run never carried; not lifted");
    }
  }
}

}  // namespace

bool sameLanding(const Landing& a, const Landing& b) {
  return a.site == b.site && a.target == b.target &&
         contextOf(a.mode).bits == contextOf(b.mode).bits;
}

std::string_view movedKindName(MovedKind kind) {
  switch (kind) {
    case MovedKind::Dma: return "dma";
    case MovedKind::Table: return "table";
    case MovedKind::Indirect: return "indirect";
    case MovedKind::Stream: return "stream";
    case MovedKind::Staged: return "staged";
    case MovedKind::Proven: return "proven";
    case MovedKind::Enumerated: return "enumerated";
  }
  return "dma";
}

std::string_view inputRegisterName(InputRegister reg) {
  switch (reg) {
    case InputRegister::A: return "A";
    case InputRegister::X: return "X";
    case InputRegister::Y: return "Y";
  }
  return "A";
}

std::string inputValuesText(unsigned bits, std::span<const std::uint32_t> values) {
  const int digits = bits == 8u ? 2 : 4;
  std::string out;
  for (std::size_t i = 0; i < values.size();) {
    if (!out.empty()) out += ",";
    out += "$" + text::hex(values[i], digits);
    // Three or more a step apart are a run of that step; two consecutive
    // values are a run of one.
    std::size_t last = i;
    if (i + 2u < values.size() && values[i + 1u] > values[i] &&
        values[i + 2u] - values[i + 1u] == values[i + 1u] - values[i]) {
      const std::uint32_t step = values[i + 1u] - values[i];
      last = i + 2u;
      while (last + 1u < values.size() && values[last + 1u] - values[last] == step) ++last;
      out += "-$" + text::hex(values[last], digits);
      if (step != 1u) out += "/" + std::to_string(step);
    } else if (i + 1u < values.size() && values[i + 1u] == values[i] + 1u) {
      last = i + 1u;
      out += "-$" + text::hex(values[last], digits);
    }
    i = last + 1u;
  }
  return out;
}

std::string_view movedStepName(MovedStep step) {
  switch (step) {
    case MovedStep::Increment: return "increment";
    case MovedStep::Decrement: return "decrement";
    case MovedStep::Fixed: return "fixed";
  }
  return "increment";
}

bool sameRange(const MovedRange& a, const MovedRange& b) { return keyOf(a) == keyOf(b); }

Address extentStart(const MovedRange& range) {
  if (range.step != MovedStep::Decrement) return range.memory;
  const Address bank = range.memory & 0xFF0000u;
  return bank | static_cast<std::uint16_t>((range.memory & 0xFFFFu) - (range.bytes - 1u));
}

bool sameExtent(const StagedRange& a, const StagedRange& b) {
  return a.memory == b.memory && a.bytes == b.bytes;
}

bool sameStream(const StreamedRange& a, const StreamedRange& b) {
  return a.site == b.site && a.registerAddress == b.registerAddress && a.memory == b.memory &&
         a.romOffset == b.romOffset && a.bytes == b.bytes && a.landing.has_value() == b.landing.has_value() &&
         (!a.landing || (a.landing->memory == b.landing->memory && a.landing->lowest == b.landing->lowest &&
                         a.landing->highest == b.landing->highest && a.landing->shown == b.landing->shown &&
                         a.landing->areas == b.landing->areas && a.landing->depths == b.landing->depths));
}

bool sameContent(const CarriedContent& a, const CarriedContent& b) {
  return a.bytes == b.bytes && a.cls == b.cls && a.kind == b.kind && a.unit == b.unit && a.indirect == b.indirect;
}

bool sameSample(const KeyedSample& a, const KeyedSample& b) { return a.start == b.start && a.bytes == b.bytes; }

unsigned hdmaUnitOf(std::uint8_t pattern) {
  static constexpr unsigned kUnits[8] = {1, 2, 2, 4, 4, 4, 2, 4};
  return kUnits[pattern & 7u];
}

bool walkedBefore(const WalkedRange& a, const WalkedRange& b) {
  if (a.site != b.site) return a.site < b.site;
  if (a.channel != b.channel) return a.channel < b.channel;
  if (a.memory != b.memory) return a.memory < b.memory;
  if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  if (a.bytes != b.bytes) return a.bytes > b.bytes;
  if (a.unit != b.unit) return a.unit < b.unit;
  return !a.indirect && b.indirect;
}

std::optional<unsigned> landingDepth(const PortLanding& landing) {
  switch (landing.depths) {
    case kDepth2: return 2u;
    case kDepth4: return 4u;
    case kDepth8: return 8u;
    default: return std::nullopt;
  }
}

std::string depthText(const PortLanding& landing) {
  const std::optional<unsigned> depth = landingDepth(landing);
  return depth ? std::to_string(*depth) : std::string("none");
}

bool rangeBefore(const MovedRange& a, const MovedRange& b) {
  if (a.site != b.site) return a.site < b.site;
  if (a.channel != b.channel) return a.channel < b.channel;
  if (a.memory != b.memory) return a.memory < b.memory;
  if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  return a.bytes > b.bytes;  // the whole walk, then what the run's end cut
}

bool landedBefore(const LandedRange& a, const LandedRange& b) {
  if (a.site != b.site) return a.site < b.site;
  if (a.channel != b.channel) return a.channel < b.channel;
  if (a.memory != b.memory) return a.memory < b.memory;
  if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  if (a.bytes != b.bytes) return a.bytes > b.bytes;
  if (a.landing.memory != b.landing.memory) return static_cast<int>(a.landing.memory) < static_cast<int>(b.landing.memory);
  if (a.landing.lowest != b.landing.lowest) return a.landing.lowest < b.landing.lowest;
  if (a.landing.highest != b.landing.highest) return a.landing.highest < b.landing.highest;
  if (a.landing.shown != b.landing.shown) return a.landing.shown;
  if (a.landing.areas != b.landing.areas) return a.landing.areas < b.landing.areas;
  return a.landing.depths < b.landing.depths;
}

std::string areaText(const PortLanding& landing) {
  if (!landing.shown) return "unshown";
  static constexpr std::pair<std::uint16_t, const char*> kNames[] = {
      {kAreaTilemap1, "tilemap1"}, {kAreaTilemap2, "tilemap2"}, {kAreaTilemap3, "tilemap3"},
      {kAreaTilemap4, "tilemap4"}, {kAreaTiles1, "tiles1"},     {kAreaTiles2, "tiles2"},
      {kAreaTiles3, "tiles3"},     {kAreaTiles4, "tiles4"},     {kAreaSprites, "sprites"},
      {kAreaMode7, "mode7"},       {kAreaPalette, "palette"},   {kAreaOam, "oam"}};
  std::string out;
  for (const auto& [bit, name] : kNames) {
    if ((landing.areas & bit) == 0u) continue;
    if (!out.empty()) out += '+';
    out += name;
  }
  return out.empty() ? "none" : out;
}

std::string portAddressText(PortMemory memory, std::uint16_t address) {
  char text[8];
  switch (memory) {
    case PortMemory::Vram: std::snprintf(text, sizeof text, "$%04X", address); break;
    case PortMemory::Cgram: std::snprintf(text, sizeof text, "$%02X", address); break;
    case PortMemory::Oam: std::snprintf(text, sizeof text, "$%03X", address); break;
  }
  return text;
}

bool sameSighting(const ReachedTarget& a, const ReachedTarget& b) {
  return a.site == b.site && a.target == b.target &&
         contextOf(a.mode).bits == contextOf(b.mode).bits;
}

RunObservation observeRun(std::span<const std::uint8_t> rom, std::uint64_t masterCycles,
                          const InputScript& input, std::vector<std::string>& notes,
                          const ProgressSink& progress) {
  return observeRun(rom, masterCycles, input, notes, progress, EnumerationPlan{}, 0, 0);
}

RunObservation observeRun(std::span<const std::uint8_t> rom, std::uint64_t masterCycles, const InputScript& input,
                          std::vector<std::string>& notes, const ProgressSink& progress, const EnumerationPlan& plan,
                          std::uint64_t firstInstructions, std::uint64_t firstDivergences) {
  RunObservation observation;
  // A second run's own findings are the first run's again, so its notes are
  // not repeated; only what it enumerates is said.
  const bool second = !plan.routines.empty();
  std::vector<std::string> repeated;
  std::vector<std::string>& runNotes = second ? repeated : notes;
  const std::string_view kStage = second ? "running the cartridge again" : "running the cartridge";
  const auto report = [&](std::uint64_t at) {
    if (progress) progress(Progress{.stage = kStage, .spent = at, .budget = masterCycles});
  };
  std::vector<ReachedTarget>& out = observation.reached;
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  if (!header) {
    runNotes.push_back("no run: the image is too small to hold a cartridge header at any site");
    return observation;
  }
  const CartridgeBoard board = cartridgeBoard(rom);

  Snes machine{SnesConfig{.rom = rom}};
  ir::Provenance shadow{board, rom.size(), kOriginCap};
  Recorder recorder{machine, shadow};
  shadow.carries = &recorder;
  machine.setObserver(&recorder);
  Lockstep lockstep{board, rom.size(), machine.state().cpu, shadow};
  AudioLockstep audio{machine, runNotes};
  machine.setApuObserver(&audio);
  std::optional<Enumerating> enumerating;
  if (second) {
    enumerating.emplace(plan);
    lockstep.enumerating = &*enumerating;
  }
  std::set<std::tuple<Address, Address, std::uint32_t>> seen;
  std::set<Address> unreadableSites;
  std::set<Address> unconfirmedSites;

  // Frames are counted from power-on, the first being 0, and a frame begins when
  // the beam wraps to line 0. The pads for a frame are presented as it begins,
  // ahead of the vertical blank in which the auto-read latches them.
  std::uint32_t frame = 0;
  presentPads(machine, input, frame);

  std::uint64_t spent = 0;
  std::uint64_t reported = 0;  // the tick the last report was made at
  report(0);
  while (spent < masterCycles) {
    if (spent / kProgressTick != reported) {
      reported = spent / kProgressTick;
      report(spent);
    }
    // `state()` is the live machine: everything read from it before the step is
    // copied out here, since the step rewrites it.
    const SnesState& before = machine.state();
    const Cpu65816State cpuBefore = before.cpu;
    const Address site = (static_cast<Address>(before.cpu.pbr) << 16) | before.cpu.pc;
    const std::uint16_t stackBefore = before.cpu.s;
    const std::uint16_t lineBefore = before.vpos;
    bool unreadable = false;
    const std::optional<Pending> pending = pendingTarget(board, rom, before, unreadable);
    const Cpu65816Mode mode = modeOf(before.cpu);
    if (unreadable && unreadableSites.insert(site).second) {
      runNotes.push_back("run: the jump at " + formatAddress(site, 24) +
                      " reads its pointer from memory the run cannot see; not recorded");
    }

    recorder.stepBegan(site);
    spent += machine.step();
    if (enumerating) enumerating->spent = spent;

    // A step runs one instruction, or one cycle of a transfer, never a whole
    // frame, so the beam wrapping to line 0 is a frame boundary seen exactly once.
    if (machine.state().vpos < lineBefore) {
      ++frame;
      presentPads(machine, input, frame);
      recorder.frameBegan();
    }

    // The interpreter beside the machine: the step's instruction lifted from
    // its fetches and checked, the landing read, the registers recorded.
    const SnesState& after = machine.state();
    lockstep.step(cpuBefore, after.cpu, recorder.cpu, runNotes);

    // The second run keeps what the planned routines' frames wrote, and the
    // machine and the shadow at the first instruction of each one's first
    // invocation.
    if (enumerating) {
      enumerating->wrote(recorder);
      if (lockstep.snapshotDue) {
        Planned& due = enumerating->planned[*lockstep.snapshotDue];
        due.snapshot.emplace(Snapshot{.state = after,
                                      .shadow = shadow,
                                      .returnTo = enumerating->dueReturn,
                                      .enteredFrom = enumerating->dueSite,
                                      .entryS = after.cpu.s,
                                      .frame = frame,
                                      .spent = spent});
        due.snapshot->shadow.carries = nullptr;
        lockstep.snapshotDue.reset();
      }
    }

    if (!pending) continue;
    // The landing confirms the pointer. A step that serviced an interrupt instead
    // lands in the handler and the instruction has not run yet; it is seen when it
    // does. A landing elsewhere means the reading of the form is wrong, and that is
    // said rather than recorded.
    const Address landed = (static_cast<Address>(after.cpu.pbr) << 16) | after.cpu.pc;
    // A step that left the program counter where it was ran no instruction: the
    // CPU was held off the bus — a DMA transfer, an HDMA event — and the jump is
    // still ahead of it. It is seen on the step that runs it.
    if (landed == site) continue;
    if (landed != pending->target) {
      // An interrupt sequence lands in bank zero having pushed the return address
      // and the status byte: four bytes in native mode, three in emulation. Nothing
      // the four forms do moves the stack that way.
      const std::uint16_t pushed = static_cast<std::uint16_t>(stackBefore - after.cpu.s);
      const bool interrupted = after.cpu.pbr == 0 && (pushed == 4u || pushed == 3u);
      if (!interrupted && unconfirmedSites.insert(site).second) {
        runNotes.push_back("run: the jump at " + formatAddress(site, 24) + " read a pointer to " +
                        formatAddress(pending->target, 24) + " but the CPU went to " +
                        formatAddress(landed, 24) + "; not recorded");
      }
      continue;
    }
    if (!seen.insert({site, pending->target, contextOf(mode).bits}).second) continue;
    out.push_back(ReachedTarget{.target = pending->target,
                                .mode = mode,
                                .site = site,
                                .call = pending->call,
                                .name = {}});
  }

  std::sort(out.begin(), out.end(), [](const ReachedTarget& a, const ReachedTarget& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return contextOf(a.mode).bits < contextOf(b.mode).bits;
  });

  if (enumerating) {
    report(spent);
    enumerate(observation, *enumerating, plan.recordedClasses, lockstep, rom, board, input, notes, progress,
              firstInstructions, firstDivergences);
  }

  // The shadow closes the stream still open — the recorder folds the last
  // carry with it and takes its landing — and then the recorder closes every
  // range, so what the run's end cut short is counted, and a landing no frame
  // drew is unshown.
  shadow.finish();
  recorder.finish();
  shadow.carries = nullptr;
  machine.setObserver(nullptr);
  machine.setApuObserver(nullptr);
  observation.moved = std::move(recorder.out);
  std::sort(observation.moved.begin(), observation.moved.end(), rangeBefore);
  observation.landed = std::move(recorder.landed);
  std::sort(observation.landed.begin(), observation.landed.end(), landedBefore);
  observation.walked = std::move(recorder.walked);
  std::sort(observation.walked.begin(), observation.walked.end(), walkedBefore);
  observation.palettes = std::move(recorder.palettes);

  observation.ran = std::move(lockstep.ran);
  std::sort(observation.ran.begin(), observation.ran.end(), [](const Landing& a, const Landing& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return contextOf(a.mode).bits < contextOf(b.mode).bits;
  });
  for (const auto& [address, values] : lockstep.seen) {
    observation.seen.push_back(SeenState{.address = address,
                                         .d = {values.d.begin(), values.d.end()},
                                         .dbr = {values.dbr.begin(), values.dbr.end()}});
  }
  observation.instructions = lockstep.instructions;
  observation.interrupts = lockstep.interrupts;
  observation.nodes = lockstep.nodes.size();
  observation.divergences = lockstep.diverged;
  observation.spc700Instructions = audio.instructions;
  observation.spc700Nodes = audio.nodes.size();
  observation.spc700Divergences = audio.diverged;
  // The samples the key-ons named: by start, the shorter first, then by bytes.
  observation.samples = std::move(audio.samples);
  std::sort(observation.samples.begin(), observation.samples.end(),
            [](const KeyedSample& a, const KeyedSample& b) {
              if (a.start != b.start) return a.start < b.start;
              if (a.bytes.size() != b.bytes.size()) return a.bytes.size() < b.bytes.size();
              return a.bytes < b.bytes;
            });

  // What was staged: every extent, in address order then by count, its
  // writers most bytes first.
  for (auto& [extent, range] : recorder.staged) {
    std::sort(range.writers.begin(), range.writers.end(),
              [](const StagedWriter& a, const StagedWriter& b) {
                if (a.bytes != b.bytes) return a.bytes > b.bytes;
                if (a.unwritten != b.unwritten) return !a.unwritten;
                return a.writer < b.writer;
              });
    observation.staged.push_back(std::move(range));
  }
  // What the CPU streamed: each distinct stream and landing once with its
  // count, as the recorder took them from the shadow's closings; the source
  // is the shadow's, which stands as the widest run it found.
  observation.streamed = std::move(recorder.streams);
  for (StreamedRange& range : observation.streamed) {
    for (const ir::Stream& stream : shadow.streams()) {
      if (stream.site == range.site && stream.registerAddress == range.registerAddress &&
          stream.memory == range.memory && stream.first == range.romOffset && stream.bytes == range.bytes) {
        range.source = stream.source;
        break;
      }
    }
  }
  std::sort(observation.streamed.begin(), observation.streamed.end(),
            [](const StreamedRange& a, const StreamedRange& b) {
              if (a.site != b.site) return a.site < b.site;
              if (a.registerAddress != b.registerAddress) return a.registerAddress < b.registerAddress;
              if (a.memory != b.memory) return a.memory < b.memory;
              if (a.romOffset != b.romOffset) return a.romOffset < b.romOffset;
              if (a.landing.has_value() != b.landing.has_value()) return !a.landing.has_value();
              if (!a.landing) return false;
              if (a.landing->lowest != b.landing->lowest) return a.landing->lowest < b.landing->lowest;
              if (a.landing->highest != b.landing->highest) return a.landing->highest < b.landing->highest;
              if (a.landing->shown != b.landing->shown) return a.landing->shown;
              if (a.landing->areas != b.landing->areas) return a.landing->areas < b.landing->areas;
              return a.landing->depths < b.landing->depths;
            });
  observation.originSets = shadow.origins().interned();
  if (!enumerating) report(spent);
  observation.originCap = shadow.origins().cap();
  return observation;
}

}  // namespace snaggletooth::disasm
