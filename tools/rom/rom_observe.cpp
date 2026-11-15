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
        Recorder rec{MachineView{sandbox}, copy};
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

ObservedRun::ObservedRun(Snes& machine, std::span<const std::uint8_t> rom) {
  if (machine.state().master != 0) {
    throw std::logic_error("a run binds to a machine at power-on, and this one has run");
  }
  if (machine.observer() != nullptr || machine.apuObserver() != nullptr || machine.instructionObserver() != nullptr) {
    throw std::logic_error("a run sets the machine's bus, audio and instruction observers, and one is already set");
  }
  if (!parseCartridgeHeader(rom)) {
    throw std::logic_error("the image is too small to hold a cartridge header at any site");
  }
  impl_ = std::make_unique<Impl>(machine, rom);
}

ObservedRun::~ObservedRun() {
  if (impl_->host->instructionObserver() != impl_.get()) return;
  impl_->host->setObserver(nullptr);
  impl_->host->setApuObserver(nullptr);
  impl_->host->setInstructionObserver(nullptr);
}

void ObservedRun::reset() { impl_->reset(); }

std::uint32_t ObservedRun::frame() const noexcept { return impl_->frame; }

RunObservation ObservedRun::observation(std::vector<std::string>& notes) const {
  RunObservation observation;
  impl_->assemble(observation);
  impl_->notesInto(notes);
  return observation;
}

RunObservation ObservedRun::finish(std::vector<std::string>& notes) {
  RunObservation observation;
  impl_->finish(observation);
  impl_->notesInto(notes);
  return observation;
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
  if (!parseCartridgeHeader(rom)) {
    runNotes.push_back("no run: the image is too small to hold a cartridge header at any site");
    return observation;
  }

  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun::Impl run{machine, rom};
  if (second) {
    run.enumerating.emplace(plan);
    run.lockstep.enumerating = &*run.enumerating;
  }

  // The pads for a frame are presented as it begins, ahead of the vertical
  // blank in which the auto-read latches them: after the step that wrapped the
  // beam to line 0.
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
    spent += machine.step();
    if (run.frame != frame) {
      frame = run.frame;
      presentPads(machine, input, frame);
    }

    // The machine and the shadow at the first instruction of each planned
    // routine's first invocation.
    if (run.enumerating && run.lockstep.snapshotDue) {
      const SnesState& after = machine.state();
      Planned& due = run.enumerating->planned[*run.lockstep.snapshotDue];
      due.snapshot.emplace(Snapshot{.state = after,
                                    .shadow = run.shadow,
                                    .returnTo = run.enumerating->dueReturn,
                                    .enteredFrom = run.enumerating->dueSite,
                                    .entryS = after.cpu.s,
                                    .frame = frame,
                                    .spent = spent});
      due.snapshot->shadow.carries = nullptr;
      run.lockstep.snapshotDue.reset();
    }
  }

  if (run.enumerating) {
    report(spent);
    enumerate(observation, *run.enumerating, plan.recordedClasses, run.lockstep, rom, run.board, input, notes,
              progress, firstInstructions, firstDivergences);
  }
  run.finish(observation);
  run.notesInto(runNotes);
  if (!run.enumerating) report(spent);
  return observation;
}

}  // namespace snaggletooth::disasm
