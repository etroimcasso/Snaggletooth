#include "rom/rom_facts.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "rom/cartridge_entries.h"
#include "rom/rom_disasm.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {
namespace {

// The node lifted for the code line at `site` — the first reading where the
// address was read two ways — with what every path proves before it, or
// nothing where no path reached it.
struct SiteProof {
  const ir::Node* node = nullptr;
  ir::Evaluation evaluation;
};

std::optional<SiteProof> proofAt(const ir::Program& program, const ProvenProgram& proven, Address site) {
  const std::vector<ir::Node>& nodes = program.nodes;
  auto it = std::lower_bound(nodes.begin(), nodes.end(), site, [](const ir::Node& n, Address a) {
    return n.instruction.address < a;
  });
  for (; it != nodes.end() && it->instruction.address == site; ++it) {
    const std::size_t index = static_cast<std::size_t>(it - nodes.begin());
    const ir::State* before = proven.flow->before(index);
    if (!before) continue;
    return SiteProof{.node = &*it, .evaluation = ir::evaluate(*it, *before, proven.image, proven.stack)};
  }
  return std::nullopt;
}

// The one access an instruction makes to the memory its operand names: the
// store, or the load, or a read-modify-write's write-back.
const ir::ProvenAccess* operandAccess(const SiteProof& proof) {
  const ir::ProvenAccess* load = nullptr;
  for (const ir::ProvenAccess& access : proof.evaluation.accesses) {
    if (access.op == ir::Op::Store || access.op == ir::Op::StoreRmw) return &access;
    if (access.op == ir::Op::Load && load == nullptr) load = &access;
  }
  return load;
}

bool directForm(const ir::Node& node) {
  switch (node.instruction.addressing) {
    case ir::Addressing::Direct:
    case ir::Addressing::DirectX:
    case ir::Addressing::DirectY:
      return true;
    default:
      return false;
  }
}

// The register an instruction's memory operand passes through, which is the
// register whose width decides how many bytes the operand is. The index forms
// name themselves; everything else — the loads and stores of the accumulator,
// the compares against it, and every read-modify-write form, which works on
// memory at the accumulator's width — follows the accumulator.
bool indexWidth(std::string_view mnemonic) {
  return mnemonic == "LDX" || mnemonic == "STX" || mnemonic == "CPX" || mnemonic == "LDY" ||
         mnemonic == "STY" || mnemonic == "CPY";
}

// Whether the operand at `mnemonic` is two bytes wide under `mode` — and so
// reaches two consecutive registers. A width the trace does not know is not a
// width: the operand is read as one register, which is what the listing already
// says by refusing to guess.
bool operandIsWide(const Cpu65816Mode& mode, std::string_view mnemonic) {
  return indexWidth(mnemonic) ? (mode.indexKnown && !mode.index8)
                              : (mode.accumulatorKnown && !mode.accumulator8);
}

// What an instruction does to the memory its operand names. The kind is in the
// instruction set and nowhere else.
std::optional<AccessKind> accessKind(std::string_view mnemonic) {
  static constexpr std::array<std::string_view, 12> kReads = {
      "LDA", "LDX", "LDY", "CMP", "CPX", "CPY", "BIT", "ADC", "SBC", "AND", "ORA", "EOR"};
  static constexpr std::array<std::string_view, 4> kWrites = {"STA", "STX", "STY", "STZ"};
  static constexpr std::array<std::string_view, 8> kReadWrites = {"INC", "DEC", "ASL", "LSR",
                                                                  "ROL", "ROR", "TSB", "TRB"};
  if (std::find(kReads.begin(), kReads.end(), mnemonic) != kReads.end()) return AccessKind::Read;
  if (std::find(kWrites.begin(), kWrites.end(), mnemonic) != kWrites.end()) return AccessKind::Write;
  if (std::find(kReadWrites.begin(), kReadWrites.end(), mnemonic) != kReadWrites.end()) {
    return AccessKind::ReadWrite;
  }
  return std::nullopt;
}

// Whether a load fills exactly the register a store empties. `LDA` before `STX`
// says nothing about what `STX` writes, so the pair has to match.
bool fills(std::string_view load, std::string_view store) {
  return (load == "LDA" && store == "STA") || (load == "LDX" && store == "STX") ||
         (load == "LDY" && store == "STY");
}

// The immediate the instruction before loaded into the register this store is
// about to write, or nothing. This is the whole of what the bytes say about a
// written value: one instruction back, the same register, no label between —
// which the caller has already settled by handing over the previous line only
// while both sit in one run.
std::optional<std::uint16_t> immediateBefore(const Line* previous, std::string_view store) {
  if (previous == nullptr) return std::nullopt;
  const Cpu65816Opcode& info = cpu65816Opcodes()[previous->instruction.opcode];
  const bool isImmediate = info.mode == Cpu65816Addressing::ImmediateM ||
                           info.mode == Cpu65816Addressing::ImmediateX;
  if (!isImmediate || !fills(info.mnemonic, store)) return std::nullopt;
  const std::vector<std::uint8_t>& bytes = previous->instruction.bytes;
  if (bytes.size() == 2) return bytes[1];
  if (bytes.size() == 3) return static_cast<std::uint16_t>(bytes[1] | (bytes[2] << 8));
  return std::nullopt;
}

// The channels' registers: eight channels sixteen bytes apart from $4300, the
// eight slots that describe a transfer first in each.
constexpr Address kDmaBase = 0x4300u;

}  // namespace

std::string_view accessKindName(AccessKind kind) {
  switch (kind) {
    case AccessKind::Read: return "read";
    case AccessKind::Write: return "write";
    case AccessKind::ReadWrite: return "read-write";
  }
  return {};
}

std::string_view dmaDirectionName(DmaDirection direction) {
  switch (direction) {
    case DmaDirection::ToBBus: return "to-register";
    case DmaDirection::ToABus: return "from-register";
    case DmaDirection::Unknown: return "direction-unknown";
  }
  return {};
}

std::vector<HardwareAccess> hardwareAccesses(const CartridgeDisassembly& disassembly,
                                             const ProvenProgram* proven) {
  std::vector<HardwareAccess> out;
  std::uint32_t run = 0;

  for (const RegionListing& region : disassembly.regions) {
    const Listing& listing = region.listing;
    const Line* previous = nullptr;
    ++run;
    for (const Line& line : listing.lines) {
      // A run of data ends the run: nothing before it reached anything after it
      // by falling through.
      if (!line.isCode) {
        previous = nullptr;
        ++run;
        continue;
      }
      // A label is somewhere another path arrives, so what the instruction above
      // left is not what every arrival carries.
      if (listing.labels.find(line.address) != listing.labels.end()) {
        previous = nullptr;
        ++run;
      }

      const Instruction& instruction = line.instruction;
      const Cpu65816Opcode& info = cpu65816Opcodes()[instruction.opcode];
      // `accessKind` is the whole of the rule: it answers for the forms that
      // touch the memory their operand names, and for nothing else. A jump or a
      // call through a pointer held at a register address reaches the pointer
      // rather than the hardware, and is named by none of those forms.
      const std::optional<AccessKind> kind = accessKind(info.mnemonic);

      // What every path proves at this instruction, when the program was proven:
      // the address its operand reaches and the value it moves.
      std::optional<SiteProof> proof;
      const ir::ProvenAccess* access = nullptr;
      if (proven && kind) {
        proof = proofAt(disassembly.program, *proven, instruction.address);
        if (proof) access = operandAccess(*proof);
      }

      // The address the fact is about: the one the listing annotates — a long
      // operand in its own bank, an absolute operand in bank zero — or, for a
      // direct-page operand, the one every path proves it lands on.
      std::optional<Address> first = kind ? instruction.operandAddress : std::nullopt;
      if (!first && proof && access && directForm(*proof->node)) {
        if (const std::optional<std::uint32_t> address = access->address.single()) first = *address;
      }

      if (first) {
        // Only an operand the listing itself annotates, or the paths settle,
        // produces a fact, so the report says no more than is proven.
        if (const std::optional<Cpu65816Register> named = cpu65816Register(*first)) {
          const Cpu65816Mode mode = modeOf(line.context);
          const bool wide = operandIsWide(mode, info.mnemonic);
          const bool writes = *kind == AccessKind::Write;

          // `STZ` carries its own value; anything else needs the instruction
          // before to have loaded one, or every path to prove one.
          std::optional<std::uint16_t> written;
          if (writes && info.mnemonic == std::string_view("STZ")) {
            written = std::uint16_t{0};
          } else if (writes) {
            written = immediateBefore(previous, info.mnemonic);
            if (!written && access) {
              if (const std::optional<std::uint32_t> value = access->value.single()) {
                written = static_cast<std::uint16_t>(*value & 0xFFFFu);
              }
            }
          }

          const unsigned reached = wide ? 2u : 1u;
          for (unsigned i = 0; i < reached; ++i) {
            const std::optional<Cpu65816Register> reg = cpu65816Register(*first + i);
            if (!reg) continue;
            HardwareAccess fact{.site = instruction.address,
                                .registerAddress = *first + i,
                                .name = reg->name,
                                .cls = reg->cls,
                                .kind = *kind,
                                .value = std::nullopt,
                                .run = run};
            if (written) {
              fact.value = static_cast<std::uint8_t>(i == 0 ? (*written & 0xFFu) : (*written >> 8));
            }
            out.push_back(fact);
          }
        }
      }

      // A label is the only boundary there is to draw. Where execution does not
      // fall through an instruction, whatever the trace decoded next was reached
      // by a jump or a branch — so it carries a label, and the line above already
      // ended the run there.
      previous = &line;
    }
  }

  std::sort(out.begin(), out.end(), [](const HardwareAccess& a, const HardwareAccess& b) {
    if (a.site != b.site) return a.site < b.site;
    return a.registerAddress < b.registerAddress;
  });
  return out;
}

namespace {

// One channel's registers as a run of straight-line code leaves them, and the
// set-up in progress since the channel last started: which registers were
// written and where. A slot holds the value the last write proved, or nothing
// when the last write proved none — a register written from a variable is
// unknown from then on, whatever an earlier write left.
struct ChannelState {
  std::array<std::optional<std::uint8_t>, 8> slots;  // DMAP, BBAD, A1TL, A1TH, A1B, DASL, DASH, DASB
  bool written = false;    // any register, since the run began
  bool described = false;  // the direction or the destination, with a value, since the last start
  bool started = false;    // a start since the last register write: the sites are the last set-up's
  std::optional<Address> dmapSite;   // the write that proved `DMAP`
  std::optional<Address> bbadSite;   // the write that proved `BBAD`
  std::optional<Address> firstSite;  // the first register written for this set-up
};

// The transfer a channel's registers describe, as they stand.
DmaTransfer transferFrom(const ChannelState& state, std::uint8_t channel, std::uint32_t run) {
  DmaTransfer transfer{.site = state.bbadSite ? *state.bbadSite
                               : state.dmapSite ? *state.dmapSite
                                                : state.firstSite.value_or(0),
                       .channel = channel,
                       .direction = DmaDirection::Unknown,
                       .destination = std::nullopt,
                       .destinationName = {},
                       .destinationClass = std::nullopt,
                       .source = std::nullopt,
                       .step = std::nullopt,
                       .bytes = std::nullopt,
                       .startMask = std::nullopt,
                       .startSite = std::nullopt,
                       .hdma = false,
                       .run = run};
  if (const std::optional<std::uint8_t> dmap = state.slots[0]) {
    transfer.direction = (*dmap & 0x80u) ? DmaDirection::ToABus : DmaDirection::ToBBus;
    // The A-bus step: bit 3 holds the address still, bit 4 walks it down.
    transfer.step = (*dmap & 0x08u)   ? MovedStep::Fixed
                    : (*dmap & 0x10u) ? MovedStep::Decrement
                                      : MovedStep::Increment;
  }
  // The destination is the value in `BBAD`, not `BBAD` itself: the channel
  // moves bytes to the B-bus register that value selects.
  if (const std::optional<std::uint8_t> bbad = state.slots[1]) {
    const Address destination = 0x2100u | *bbad;
    transfer.destination = destination;
    if (const std::optional<Cpu65816Register> reg = cpu65816Register(destination)) {
      transfer.destinationName = reg->name;
      transfer.destinationClass = reg->cls;
    }
  }
  if (state.slots[2] && state.slots[3] && state.slots[4]) {
    transfer.source = (static_cast<Address>(*state.slots[4]) << 16) |
                      (static_cast<Address>(*state.slots[3]) << 8) | static_cast<Address>(*state.slots[2]);
  }
  // The count is sixteen bits, and the engine moves the whole bank's worth on
  // a zero: it counts down before it tests.
  if (state.slots[5] && state.slots[6]) {
    const std::uint32_t count = (static_cast<std::uint32_t>(*state.slots[6]) << 8) | *state.slots[5];
    transfer.bytes = count == 0 ? 0x10000u : count;
  }
  return transfer;
}

}  // namespace

std::vector<DmaTransfer> dmaTransfers(const std::vector<HardwareAccess>& accesses) {
  // The accesses are in site order, and a run's sites are contiguous in it, so
  // each run is walked in the order its instructions execute.
  std::vector<DmaTransfer> out;
  std::map<std::uint32_t, std::vector<const HardwareAccess*>> runs;
  for (const HardwareAccess& access : accesses) {
    if (access.kind == AccessKind::Read) continue;
    runs[access.run].push_back(&access);
  }

  for (const auto& [number, writes] : runs) {
    std::array<ChannelState, 8> channels{};
    for (const HardwareAccess* write : writes) {
      const Address reg = write->registerAddress;
      if (reg >= kDmaBase && reg < kDmaBase + 0x80u) {
        const std::uint8_t channel = static_cast<std::uint8_t>((reg - kDmaBase) / 0x10u);
        const std::uint8_t slot = static_cast<std::uint8_t>((reg - kDmaBase) % 0x10u);
        if (slot >= 8) continue;  // the engine's own current-address registers
        ChannelState& state = channels[channel];
        // The first write after a start begins a new set-up: the sites are
        // its own from here, and a start with no write since is the last
        // set-up started again, under its sites.
        if (state.started) {
          state.started = false;
          state.dmapSite.reset();
          state.bbadSite.reset();
          state.firstSite.reset();
        }
        state.slots[slot] = write->value;
        state.written = true;
        if (!state.firstSite) state.firstSite = write->site;
        // A direction or a destination the bytes say describes a transfer, and
        // the write that said it is the site; a write whose value the bytes do
        // not say — a store from a variable, a read-modify-write of `DMAP` —
        // describes nothing, though it leaves the register unknown.
        if (slot == 0 && write->value && !state.dmapSite) state.dmapSite = write->site;
        if (slot == 1 && write->value && !state.bbadSite) state.bbadSite = write->site;
        if (slot <= 1 && write->value) state.described = true;
        continue;
      }
      if (reg != 0x420Bu && reg != 0x420Cu) continue;
      // A start whose mask the bytes do not say starts nothing the analysis can
      // name; one that names channels emits a transfer for each channel this
      // run has given a direction or a destination, with its registers as they
      // stand — a channel whose every register came from values the bytes do
      // not say is a transfer of nothing the line could state.
      if (!write->value) continue;
      const bool hdma = reg == 0x420Cu;
      for (std::uint8_t channel = 0; channel < 8; ++channel) {
        ChannelState& state = channels[channel];
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << channel);
        if (!(*write->value & bit) || !state.written) continue;
        if (!state.slots[0] && !state.slots[1]) continue;
        DmaTransfer transfer = transferFrom(state, channel, number);
        transfer.startMask = *write->value;
        transfer.startSite = write->site;
        transfer.hdma = hdma;
        out.push_back(transfer);
        state.described = false;
        state.started = true;
      }
    }
    // What the run set up and did not start: a channel whose direction or
    // destination it wrote. One that had only its source or its count rewritten
    // reaches whatever an earlier stretch settled, and is that stretch's.
    for (std::uint8_t channel = 0; channel < 8; ++channel) {
      if (!channels[channel].described) continue;
      out.push_back(transferFrom(channels[channel], channel, number));
    }
  }

  std::stable_sort(out.begin(), out.end(), [](const DmaTransfer& a, const DmaTransfer& b) {
    if (a.site != b.site) return a.site < b.site;
    return a.channel < b.channel;
  });
  return out;
}

namespace {

// The lines a walk from `root` holds: what falling through, branching and jumping
// reach, across every region, stopping at a return, a halt, a target the bytes do
// not name, and an address that is not a code line. A call adds nothing but the
// line after it.
std::set<Address> walkFrom(Address root, const std::map<Address, const Line*>& code) {
  std::set<Address> held;
  std::vector<Address> pending = {root};
  const Cpu65816Backend& backend = cpu65816Backend();
  while (!pending.empty()) {
    const Address at = pending.back();
    pending.pop_back();
    const auto found = code.find(at);
    if (found == code.end() || !held.insert(at).second) continue;
    const Instruction& instruction = found->second->instruction;
    const Address next = backend.following(at, instruction.length);
    switch (instruction.flow) {
      case Flow::Continue:
      case Flow::Call:
        pending.push_back(next);
        break;
      case Flow::Branch:
        pending.push_back(next);
        if (instruction.target) pending.push_back(*instruction.target);
        break;
      case Flow::Jump:
        if (instruction.target) pending.push_back(*instruction.target);
        break;
      case Flow::Return:
      case Flow::Halt:
        break;
    }
  }
  return held;
}

}  // namespace

std::vector<Routine> routines(const CartridgeDisassembly& disassembly) {
  // Every code line of the 65816 regions by address, and every label.
  std::map<Address, const Line*> code;
  std::map<Address, std::string> labels;
  for (const RegionListing& region : disassembly.regions) {
    for (const Line& line : region.listing.lines) {
      if (line.isCode) code[line.address] = &line;
    }
    for (const auto& [address, name] : region.listing.labels) labels[address] = name;
  }

  // The roots: every entry, every target a run reached, and every label a call
  // names — each where the tree places it, since an entry a person gave and a
  // target a run saw name the bank the CPU runs in, which may be a mirror. Every
  // code line was reached by the trace from an entry through these same flows,
  // entering calls where this walk does not — so every line is in the routine of
  // some root, and no other label starts one.
  const CartridgeMap map = disassembly.header.map;
  std::set<Address> roots;
  auto placed = [&](Address address) {
    const std::optional<std::size_t> offset = romOffset(map, address, disassembly.imageBytes);
    if (!offset) return;
    if (const std::optional<std::uint32_t> home = romAddress(map, *offset)) roots.insert(*home);
  };
  for (const TraceEntry& entry : disassembly.entries) placed(entry.address);
  for (const ReachedTarget& seen : disassembly.reached) placed(seen.target);
  for (const auto& [address, line] : code) {
    const Instruction& instruction = line->instruction;
    if (instruction.flow == Flow::Call && instruction.target &&
        code.find(*instruction.target) != code.end()) {
      roots.insert(*instruction.target);
    }
  }

  std::vector<Routine> out;
  for (const Address root : roots) {
    const auto label = labels.find(root);
    if (code.find(root) == code.end() || label == labels.end()) continue;
    Routine routine;
    routine.address = root;
    routine.label = label->second;
    const std::set<Address> held = walkFrom(root, code);
    std::set<Address> calls;
    for (const Address at : held) {
      const Instruction& instruction = code.at(at)->instruction;
      routine.lines.push_back(at);
      routine.bytes += instruction.length;
      if (instruction.flow == Flow::Call && instruction.target &&
          code.find(*instruction.target) != code.end()) {
        calls.insert(*instruction.target);
      }
    }
    routine.calls.assign(calls.begin(), calls.end());
    out.push_back(std::move(routine));
  }

  // The role: what the routine's own lines reach — the registers the accesses
  // name and the destinations the transfers name — and, through its calls, what
  // every routine it can reach by calling reaches itself.
  std::map<Address, std::set<RegisterClass>> direct;
  for (Routine& routine : out) {
    std::set<RegisterClass>& classes = direct[routine.address];
    const std::set<Address> held(routine.lines.begin(), routine.lines.end());
    for (const HardwareAccess& access : disassembly.accesses) {
      if (held.count(access.site)) classes.insert(access.cls);
    }
    for (const DmaTransfer& dma : disassembly.dmas) {
      if (held.count(dma.site) && dma.destinationClass) classes.insert(*dma.destinationClass);
    }
    routine.reaches.assign(classes.begin(), classes.end());
  }
  std::map<Address, const Routine*> byAddress;
  for (const Routine& routine : out) byAddress[routine.address] = &routine;
  for (Routine& routine : out) {
    std::set<RegisterClass> classes;
    std::set<Address> visited;
    std::vector<Address> pending(routine.calls.begin(), routine.calls.end());
    while (!pending.empty()) {
      const Address callee = pending.back();
      pending.pop_back();
      if (!visited.insert(callee).second) continue;
      const auto found = byAddress.find(callee);
      if (found == byAddress.end()) continue;
      const std::set<RegisterClass>& reached = direct[callee];
      classes.insert(reached.begin(), reached.end());
      pending.insert(pending.end(), found->second->calls.begin(), found->second->calls.end());
    }
    routine.through.assign(classes.begin(), classes.end());
  }
  return out;
}

// ---- what every path proves ----------------------------------------------------

namespace {

Cpu65816Mode cpuModeOf(const ir::Mode& mode) {
  return Cpu65816Mode{.emulation = mode.emulation,
                      .accumulator8 = mode.accumulator8,
                      .index8 = mode.index8,
                      .accumulatorKnown = mode.accumulatorKnown,
                      .indexKnown = mode.indexKnown,
                      .carryKnown = false,
                      .carry = false};
}

// The address every byte of the image is placed at, or nothing off the image.
std::optional<Address> homeOf(CartridgeMap map, std::size_t imageBytes, Address address) {
  const std::optional<std::size_t> offset = romOffset(map, address, imageBytes);
  if (!offset) return std::nullopt;
  return romAddress(map, *offset);
}

std::vector<std::uint32_t> valuesOf(const ir::Values& values) {
  return values.known ? values.values : std::vector<std::uint32_t>{};
}

}  // namespace

ProvenProgram proveProgram(const CartridgeDisassembly& disassembly, std::span<const std::uint8_t> rom) {
  const CartridgeMap map = disassembly.header.map;
  const std::size_t imageBytes = rom.size();

  // Every vector begins in bank zero, which the chip clears to take it — reset
  // with the direct register and the data bank cleared too, the others knowing
  // nothing else. An entry a person added begins in the bank of the address they
  // gave, and knows nothing else.
  const std::optional<Address> reset = homeOf(map, imageBytes, disassembly.header.emulation.reset);
  struct Vector {
    Address home = 0;
    Cpu65816Mode mode;
  };
  std::vector<Vector> vectors;
  for (const VectorEntry& vector : vectorEntries(disassembly.header)) {
    const std::optional<Address> home = homeOf(map, imageBytes, vector.address);
    if (!home) continue;
    const bool native = vector.name.ends_with("_native");
    vectors.push_back(Vector{.home = *home, .mode = native ? Cpu65816Mode::nativeUnknown() : Cpu65816Mode::reset()});
  }
  std::vector<ir::FlowEntry> entries;
  for (const TraceEntry& entry : disassembly.entries) {
    const bool isReset = reset && entry.address == *reset && entry.mode == Cpu65816Mode::reset();
    const bool isVector = std::any_of(vectors.begin(), vectors.end(), [&](const Vector& v) {
      return v.home == entry.address && v.mode == entry.mode;
    });
    entries.push_back(ir::FlowEntry{.address = entry.address,
                                    .state = isReset    ? ir::resetState()
                                             : isVector ? ir::handlerState()
                                                        : ir::entryState(entry.address)});
  }
  std::vector<ir::Sighting> sightings;
  // Where a run landed without an instruction naming it is a sighting like a
  // reached target: the instruction that took the CPU there is the site, and
  // what the paths prove at it flows to the landing, in the bank the CPU
  // arrived in.
  for (const Landing& landing : disassembly.ran) {
    sightings.push_back(ir::Sighting{.site = landing.site, .target = landing.target});
  }
  for (const ReachedTarget& seen : disassembly.reached) {
    sightings.push_back(ir::Sighting{.site = seen.site, .target = seen.target});
  }
  for (const DerivedTarget& derived : disassembly.derived) {
    sightings.push_back(ir::Sighting{.site = derived.site, .target = derived.target});
  }

  ir::ImageReader image = [map, imageBytes, rom](ir::Address address) -> std::optional<std::uint8_t> {
    const std::optional<std::size_t> offset = romOffset(map, address, imageBytes);
    if (!offset) return std::nullopt;
    return rom[*offset];
  };
  ir::Canonical canonical = [map, imageBytes](ir::Address address) { return homeOf(map, imageBytes, address); };
  // The stack pointer is sixteen bits in bank zero, and bank zero's low eight
  // kilobytes are work RAM, mirrored in every bank that shows the registers and
  // in bank $7E itself; nowhere else in bank zero can a stack be written.
  ir::StackReach stack = [](ir::Address address) {
    const std::uint32_t bank = address >> 16;
    const bool mirrored = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu) || bank == 0x7Eu;
    return mirrored && (address & 0xFFFFu) < 0x2000u;
  };

  ProvenProgram proven;
  proven.image = image;
  proven.stack = stack;
  proven.flow = std::make_unique<ir::Dataflow>(disassembly.program, entries, sightings, image, canonical, stack);
  return proven;
}

bool sameDerivation(const DerivedTarget& a, const DerivedTarget& b) {
  return a.site == b.site && a.target == b.target && a.pointer == b.pointer &&
         contextOf(a.mode).bits == contextOf(b.mode).bits;
}

std::vector<DerivedTarget> derivedTargets(const CartridgeDisassembly& disassembly,
                                          const ProvenProgram& proven) {
  std::vector<DerivedTarget> out;
  const std::vector<ir::Node>& nodes = disassembly.program.nodes;
  for (const ir::DerivedTarget& derived : proven.flow->derived()) {
    // The mode a jump through a pointer carries in is the mode it runs under:
    // none of the four forms moves a flag.
    auto site = std::lower_bound(nodes.begin(), nodes.end(), derived.site,
                                 [](const ir::Node& n, Address a) { return n.instruction.address < a; });
    if (site == nodes.end() || site->instruction.address != derived.site) continue;
    out.push_back(DerivedTarget{.target = derived.target,
                                .mode = cpuModeOf(site->mode),
                                .site = derived.site,
                                .pointer = derived.pointer,
                                .call = derived.call,
                                .name = {}});
  }
  return out;
}

std::vector<StateFact> stateFacts(const CartridgeDisassembly& disassembly, const ProvenProgram& proven) {
  std::vector<StateFact> out;
  const std::vector<ir::Node>& nodes = disassembly.program.nodes;
  for (const RegionListing& region : disassembly.regions) {
    for (const auto& [address, label] : region.listing.labels) {
      const ir::State* state = proven.flow->before(address);
      if (!state) continue;
      auto node = std::lower_bound(nodes.begin(), nodes.end(), address,
                                   [](const ir::Node& n, Address a) { return n.instruction.address < a; });
      if (node == nodes.end() || node->instruction.address != address) continue;
      StateFact fact{.address = address,
                     .d = valuesOf(state->registers.d),
                     .dbr = valuesOf(state->registers.dbr),
                     .s = valuesOf(state->registers.s)};
      // Under emulation the stack pointer's high byte is one, whatever a path
      // left in it.
      if (node->mode.emulation) {
        for (std::uint32_t& s : fact.s) s = 0x0100u | (s & 0xFFu);
        std::sort(fact.s.begin(), fact.s.end());
        fact.s.erase(std::unique(fact.s.begin(), fact.s.end()), fact.s.end());
      }
      if (fact.d.empty() && fact.dbr.empty() && fact.s.empty()) continue;
      out.push_back(std::move(fact));
    }
  }
  std::sort(out.begin(), out.end(), [](const StateFact& a, const StateFact& b) { return a.address < b.address; });
  return out;
}

}  // namespace snaggletooth::disasm
