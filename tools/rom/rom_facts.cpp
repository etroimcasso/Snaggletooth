#include "rom/rom_facts.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>

#include "rom/rom_disasm.h"

namespace snaggletooth::disasm {
namespace {

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

// A channel's register addresses. The eight channels sit sixteen bytes apart
// from $4300, and the five that describe a transfer are the first five slots.
constexpr Address kDmaBase = 0x4300u;
constexpr Address channelRegister(std::uint8_t channel, std::uint8_t slot) {
  return kDmaBase + static_cast<Address>(channel) * 0x10u + slot;
}

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

std::vector<HardwareAccess> hardwareAccesses(const CartridgeDisassembly& disassembly) {
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

      if (instruction.operandAddress && kind) {
        const Address first = *instruction.operandAddress;
        // Only an operand the listing itself annotates produces a fact, so the
        // report says no more than the comment beside the instruction does.
        if (const std::optional<Cpu65816Register> named = cpu65816Register(first)) {
          const Cpu65816Mode mode = modeOf(line.context);
          const bool wide = operandIsWide(mode, info.mnemonic);
          const bool writes = *kind == AccessKind::Write;

          // `STZ` carries its own value; anything else needs the instruction
          // before to have loaded one.
          std::optional<std::uint16_t> written;
          if (writes && info.mnemonic == std::string_view("STZ")) {
            written = std::uint16_t{0};
          } else if (writes) {
            written = immediateBefore(previous, info.mnemonic);
          }

          const unsigned reached = wide ? 2u : 1u;
          for (unsigned i = 0; i < reached; ++i) {
            const std::optional<Cpu65816Register> reg = cpu65816Register(first + i);
            if (!reg) continue;
            HardwareAccess fact{.site = instruction.address,
                                .registerAddress = first + i,
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

std::vector<DmaTransfer> dmaTransfers(const std::vector<HardwareAccess>& accesses) {
  // What one straight-line run wrote: the value of each register it set, and the
  // site each channel's `BBAD` and `DMAP` were written at.
  struct RunState {
    std::map<Address, std::uint8_t> values;
    std::array<std::optional<Address>, 8> bbadSite;
    std::array<std::optional<Address>, 8> dmapSite;
    std::optional<std::uint8_t> mdmaen;
    std::optional<std::uint8_t> hdmaen;
    std::uint32_t run = 0;
  };

  std::vector<DmaTransfer> out;
  std::map<std::uint32_t, RunState> runs;
  for (const HardwareAccess& access : accesses) {
    if (access.kind == AccessKind::Read || !access.value) continue;
    RunState& state = runs[access.run];
    state.run = access.run;
    state.values[access.registerAddress] = *access.value;
    if (access.registerAddress == 0x420Bu) state.mdmaen = *access.value;
    if (access.registerAddress == 0x420Cu) state.hdmaen = *access.value;
    for (std::uint8_t channel = 0; channel < 8; ++channel) {
      if (access.registerAddress == channelRegister(channel, 0) && !state.dmapSite[channel]) {
        state.dmapSite[channel] = access.site;
      }
      if (access.registerAddress == channelRegister(channel, 1) && !state.bbadSite[channel]) {
        state.bbadSite[channel] = access.site;
      }
    }
  }

  for (const auto& [number, state] : runs) {
    for (std::uint8_t channel = 0; channel < 8; ++channel) {
      // A transfer is a destination the bytes named. Where only the direction and
      // pattern were written, the site is that, and the destination is absent.
      const std::optional<Address> site =
          state.bbadSite[channel] ? state.bbadSite[channel] : state.dmapSite[channel];
      if (!site) continue;

      auto valueOf = [&state](Address address) -> std::optional<std::uint8_t> {
        const auto found = state.values.find(address);
        if (found == state.values.end()) return std::nullopt;
        return found->second;
      };

      DmaTransfer transfer{.site = *site,
                           .channel = channel,
                           .direction = DmaDirection::Unknown,
                           .destination = std::nullopt,
                           .destinationName = {},
                           .destinationClass = std::nullopt,
                           .source = std::nullopt,
                           .startMask = std::nullopt,
                           .hdma = false,
                           .run = number};

      if (const std::optional<std::uint8_t> dmap = valueOf(channelRegister(channel, 0))) {
        transfer.direction = (*dmap & 0x80u) ? DmaDirection::ToABus : DmaDirection::ToBBus;
      }
      // The destination is the value in `BBAD`, not `BBAD` itself: the channel
      // moves bytes to the B-bus register that value selects.
      if (const std::optional<std::uint8_t> bbad = valueOf(channelRegister(channel, 1))) {
        const Address destination = 0x2100u | *bbad;
        transfer.destination = destination;
        if (const std::optional<Cpu65816Register> reg = cpu65816Register(destination)) {
          transfer.destinationName = reg->name;
          transfer.destinationClass = reg->cls;
        }
      }
      const std::optional<std::uint8_t> low = valueOf(channelRegister(channel, 2));
      const std::optional<std::uint8_t> high = valueOf(channelRegister(channel, 3));
      const std::optional<std::uint8_t> bank = valueOf(channelRegister(channel, 4));
      if (low && high && bank) {
        transfer.source = (static_cast<Address>(*bank) << 16) |
                          (static_cast<Address>(*high) << 8) | static_cast<Address>(*low);
      }
      const std::uint8_t bit = static_cast<std::uint8_t>(1u << channel);
      if (state.mdmaen && (*state.mdmaen & bit)) {
        transfer.startMask = *state.mdmaen;
      } else if (state.hdmaen && (*state.hdmaen & bit)) {
        transfer.startMask = *state.hdmaen;
        transfer.hdma = true;
      }
      out.push_back(transfer);
    }
  }

  std::sort(out.begin(), out.end(), [](const DmaTransfer& a, const DmaTransfer& b) {
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
  // names. Every code line was reached by the trace from an entry through these
  // same flows, entering calls where this walk does not — so every line is in
  // the routine of some root, and no other label starts one.
  std::set<Address> roots;
  for (const TraceEntry& entry : disassembly.entries) roots.insert(entry.address);
  for (const ReachedTarget& seen : disassembly.reached) roots.insert(seen.target);
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

}  // namespace snaggletooth::disasm
