#include "rom/rom_observe.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "snaggletooth/cpu/cpu65816.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {
namespace {

// The byte the CPU would read at a bus address, from what the run can see: the
// image, and work RAM — banks $7E-$7F whole, and the first 8 KB of every bank that
// mirrors them. A register, the save window, or open bus is nothing: the core reads
// it, the toolkit does not pretend to.
std::optional<std::uint8_t> readByte(CartridgeMap map, std::span<const std::uint8_t> rom,
                                     const SnesState& state, Address address) {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  const std::uint32_t offset = address & 0xFFFFu;
  if (bank == 0x7Eu || bank == 0x7Fu) return state.wram[((bank - 0x7Eu) << 16) | offset];
  const bool mirrors = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
  if (mirrors && offset < 0x2000u) return state.wram[offset];
  if (const std::optional<std::size_t> at = romOffset(map, address, rom.size())) return rom[*at];
  return std::nullopt;
}

std::optional<std::uint16_t> readWord(CartridgeMap map, std::span<const std::uint8_t> rom,
                                      const SnesState& state, Address address) {
  const std::optional<std::uint8_t> low = readByte(map, rom, state, address);
  const std::optional<std::uint8_t> high = readByte(map, rom, state, address + 1u);
  if (!low || !high) return std::nullopt;
  return static_cast<std::uint16_t>(*low | (*high << 8));
}

// The mode the CPU is in, as the trace carries one. The widths are known — the
// CPU has them — and the carry is not remembered, since nothing here follows an
// `XCE`.
Cpu65816Mode modeOf(const Cpu65816State& cpu) {
  return {.emulation = cpu.e,
          .accumulator8 = (cpu.p & kCpuFlagM) != 0,
          .index8 = (cpu.p & kCpuFlagX) != 0,
          .accumulatorKnown = true,
          .indexKnown = true,
          .carryKnown = false,
          .carry = false};
}

// Where the instruction at the CPU's position is about to go, when it is one of
// the four forms whose pointer the bytes do not name — read the way the CPU is
// about to read it. Nothing for any other instruction; nothing, with `unreadable`
// set, when the pointer lies where the run cannot see.
struct Pending {
  Address target = 0;
  bool call = false;
};

std::optional<Pending> pendingTarget(CartridgeMap map, std::span<const std::uint8_t> rom,
                                     const SnesState& state, bool& unreadable) {
  const Cpu65816State& cpu = state.cpu;
  const Address site = (static_cast<Address>(cpu.pbr) << 16) | cpu.pc;
  const std::optional<std::uint8_t> opcode = readByte(map, rom, state, site);
  if (!opcode) return std::nullopt;
  const Cpu65816Opcode& info = cpu65816Opcodes()[*opcode];
  const bool indirect = info.mode == Cpu65816Addressing::AbsoluteIndirect ||
                        info.mode == Cpu65816Addressing::AbsoluteIndirectLong ||
                        info.mode == Cpu65816Addressing::AbsoluteIndexedIndirect;
  if (!indirect || (info.flow != Flow::Jump && info.flow != Flow::Call)) return std::nullopt;

  // The operand follows the opcode within the program bank.
  const Address operandAt = (site & 0xFF0000u) | ((cpu.pc + 1u) & 0xFFFFu);
  const std::optional<std::uint16_t> operand = readWord(map, rom, state, operandAt);
  if (!operand) {
    unreadable = true;
    return std::nullopt;
  }

  const Address programBank = site & 0xFF0000u;
  std::optional<Address> target;
  switch (info.mode) {
    // `(!abs)`: a two-byte pointer in bank zero; the program bank is unchanged.
    case Cpu65816Addressing::AbsoluteIndirect:
      if (const std::optional<std::uint16_t> ptr = readWord(map, rom, state, *operand)) {
        target = programBank | *ptr;
      }
      break;
    // `[!abs]`: a three-byte pointer in bank zero; the third byte is the bank.
    case Cpu65816Addressing::AbsoluteIndirectLong: {
      const std::optional<std::uint16_t> low = readWord(map, rom, state, *operand);
      const std::optional<std::uint8_t> bank = readByte(map, rom, state, *operand + 2u);
      if (low && bank) target = (static_cast<Address>(*bank) << 16) | *low;
      break;
    }
    // `(!abs,X)`: the operand plus X addresses a two-byte pointer in the program
    // bank, wrapping within it; the program bank is unchanged.
    case Cpu65816Addressing::AbsoluteIndexedIndirect: {
      const Address pointerAt = programBank | ((*operand + cpu.x) & 0xFFFFu);
      if (const std::optional<std::uint16_t> ptr = readWord(map, rom, state, pointerAt)) {
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

// Gives each port what the script holds for it at `frame`.
void presentPads(Snes& machine, const InputScript& input, std::uint32_t frame) {
  machine.setJoypad(JoypadPort::One, input.padAt(JoypadPort::One, frame));
  machine.setJoypad(JoypadPort::Two, input.padAt(JoypadPort::Two, frame));
}

// Whether a CPU access reaches the registers: offsets `$2100`-`$43FF` of the
// system banks. The two start registers live there, and nowhere else.
bool inSystemBank(std::uint32_t address) {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  return bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
}

constexpr std::uint16_t kMdmaen = 0x420Bu;
constexpr std::uint16_t kHdmaen = 0x420Cu;

// The address the step says follows `address`.
Address stepped(Address address, MovedStep step) {
  const Address bank = address & 0xFF0000u;
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  switch (step) {
    case MovedStep::Increment: return bank | static_cast<std::uint16_t>(offset + 1u);
    case MovedStep::Decrement: return bank | static_cast<std::uint16_t>(offset - 1u);
    case MovedStep::Fixed: return address;
  }
  return address;
}

// Every field of a range but its count, as a key.
using RangeKey = std::tuple<Address, std::uint8_t, bool, Address, Address, std::uint8_t,
                            std::uint32_t, std::uint8_t>;
RangeKey keyOf(const MovedRange& r) {
  return {r.site,   r.channel, r.toRegister,                    r.registerAddress,
          r.memory, static_cast<std::uint8_t>(r.step), r.bytes, static_cast<std::uint8_t>(r.kind)};
}

// The observer the run sets on the machine: it follows every byte the two
// transfer engines move and groups them into ranges. A range is open while the
// next byte lands where the channel's step says; it closes at a break in the
// step, at a new trigger for its channel, at the start of a frame for the HDMA
// engine's — a new walk of the table — and at the end of the run. A closed
// range that was seen before is counted, not repeated.
//
// The loop tells the recorder the instruction about to run before every step,
// so a CPU write to `MDMAEN` or `HDMAEN` — which the observer sees like any
// other access — names the site every byte the channel then moves belongs to.
struct Recorder final : BusObserver {
  const Snes& machine;
  Address site = 0;  // the instruction about to run

  // An open range and where its next byte is expected.
  struct Open {
    MovedRange range;
    Address next = 0;
  };
  std::array<std::optional<Open>, 8> dma;       // a general-purpose transfer per channel
  std::array<std::optional<Open>, 8> table;     // an HDMA table per channel
  std::array<std::optional<Open>, 8> indirect;  // an indirect block per channel
  std::array<Address, 8> dmaSite{};             // the last `MDMAEN` write naming each channel
  std::array<Address, 8> hdmaSite{};            // the `HDMAEN` write that enabled each channel
  std::uint8_t hdmaEnabled = 0;                 // the last `HDMAEN` value

  std::map<RangeKey, std::size_t> index;  // a closed range's place in `out`
  std::vector<MovedRange> out;

  explicit Recorder(const Snes& m) : machine(m) {}

  void close(std::optional<Open>& open) {
    if (!open) return;
    const RangeKey key = keyOf(open->range);
    const auto found = index.find(key);
    if (found == index.end()) {
      index.emplace(key, out.size());
      out.push_back(open->range);
    } else {
      ++out[found->second].times;
    }
    open.reset();
  }

  void closeChannel(std::uint8_t channel) {
    close(dma[channel]);
    close(table[channel]);
    close(indirect[channel]);
  }

  // A new frame's HDMA walks every table from its start again.
  void frameBegan() {
    for (std::uint8_t c = 0; c < 8; ++c) {
      close(table[c]);
      close(indirect[c]);
    }
  }

  void finish() {
    for (std::uint8_t c = 0; c < 8; ++c) closeChannel(c);
  }

  // A byte of `kind` the channel moved at `address` on the A bus.
  void moved(std::uint8_t channel, MovedKind kind, Address address) {
    std::optional<Open>& open =
        kind == MovedKind::Dma ? dma[channel] : kind == MovedKind::Table ? table[channel] : indirect[channel];
    const DmaChannel& ch = machine.state().dma[channel];
    const Address trigger = kind == MovedKind::Dma ? dmaSite[channel] : hdmaSite[channel];
    const bool toRegister = (ch.dmap & 0x80u) == 0u;
    const Address registerAddress = 0x2100u | ch.bbad;
    // The engine walks a table and an indirect block upward whatever the step
    // bits say; only a general-purpose transfer follows them.
    MovedStep step = MovedStep::Increment;
    if (kind == MovedKind::Dma) {
      const std::uint8_t adjust = (ch.dmap >> 3) & 3u;
      step = adjust == 0u ? MovedStep::Increment : adjust == 2u ? MovedStep::Decrement : MovedStep::Fixed;
    }
    if (open && open->next == address && open->range.site == trigger &&
        open->range.toRegister == toRegister && open->range.registerAddress == registerAddress &&
        open->range.step == step) {
      ++open->range.bytes;
      open->next = stepped(address, step);
      return;
    }
    close(open);
    MovedRange range{.site = trigger,
                     .channel = channel,
                     .toRegister = toRegister,
                     .registerAddress = registerAddress,
                     .registerName = {},
                     .registerClass = std::nullopt,
                     .memory = address,
                     .step = step,
                     .bytes = 1,
                     .kind = kind,
                     .times = 1};
    if (const std::optional<Cpu65816Register> reg = cpu65816Register(registerAddress)) {
      range.registerName = reg->name;
      range.registerClass = reg->cls;
    }
    open = Open{.range = range, .next = stepped(address, step)};
  }

  void access(const BusAccess& a) override {
    if (a.source == AccessSource::Cpu) {
      // Only the two start registers matter here: a write to `MDMAEN` names the
      // site of every byte the channels it selects then move, and closes what
      // those channels had open; a write to `HDMAEN` does the same for the
      // channels it newly enables.
      if (!a.write) return;
      const std::uint16_t offset = static_cast<std::uint16_t>(a.address & 0xFFFFu);
      if ((offset != kMdmaen && offset != kHdmaen) || !inSystemBank(a.address)) return;
      for (std::uint8_t c = 0; c < 8; ++c) {
        const bool named = ((a.value >> c) & 1u) != 0u;
        if (offset == kMdmaen) {
          if (!named) continue;
          close(dma[c]);
          dmaSite[c] = site;
        } else {
          const bool was = ((hdmaEnabled >> c) & 1u) != 0u;
          if (named && !was) {
            close(table[c]);
            close(indirect[c]);
            hdmaSite[c] = site;
          }
        }
      }
      if (offset == kHdmaen) hdmaEnabled = a.value;
      return;
    }
    if (a.source != AccessSource::Dma && a.source != AccessSource::Hdma) return;
    // The A-bus side of a byte is the read when the byte goes to the register
    // and the write when it comes back from one; the other side is the register.
    const bool toRegister = (machine.state().dma[a.channel].dmap & 0x80u) == 0u;
    if (a.write == toRegister) return;
    const MovedKind kind = a.source == AccessSource::Dma ? MovedKind::Dma
                           : a.table                     ? MovedKind::Table
                                                         : MovedKind::Indirect;
    moved(a.channel, kind, a.address);
  }

  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};

}  // namespace

std::string_view movedKindName(MovedKind kind) {
  switch (kind) {
    case MovedKind::Dma: return "dma";
    case MovedKind::Table: return "table";
    case MovedKind::Indirect: return "indirect";
  }
  return "dma";
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

bool rangeBefore(const MovedRange& a, const MovedRange& b) {
  if (a.site != b.site) return a.site < b.site;
  if (a.channel != b.channel) return a.channel < b.channel;
  if (a.memory != b.memory) return a.memory < b.memory;
  if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  return a.bytes > b.bytes;  // the whole walk, then what the run's end cut
}

bool sameSighting(const ReachedTarget& a, const ReachedTarget& b) {
  return a.site == b.site && a.target == b.target &&
         contextOf(a.mode).bits == contextOf(b.mode).bits;
}

RunObservation observeRun(std::span<const std::uint8_t> rom, std::uint64_t masterCycles,
                          const InputScript& input, std::vector<std::string>& notes) {
  RunObservation observation;
  std::vector<ReachedTarget>& out = observation.reached;
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  if (!header) {
    notes.push_back("no run: the image is too small to hold a cartridge header at any site");
    return observation;
  }
  const CartridgeMap map = header->map;

  Snes machine{SnesConfig{.rom = rom}};
  Recorder recorder{machine};
  machine.setObserver(&recorder);
  std::set<std::tuple<Address, Address, std::uint32_t>> seen;
  std::set<Address> unreadableSites;
  std::set<Address> unconfirmedSites;

  // Frames are counted from power-on, the first being 0, and a frame begins when
  // the beam wraps to line 0. The pads for a frame are presented as it begins,
  // ahead of the vertical blank in which the auto-read latches them.
  std::uint32_t frame = 0;
  presentPads(machine, input, frame);

  std::uint64_t spent = 0;
  while (spent < masterCycles) {
    // `state()` is the live machine: everything read from it before the step is
    // copied out here, since the step rewrites it.
    const SnesState& before = machine.state();
    const Address site = (static_cast<Address>(before.cpu.pbr) << 16) | before.cpu.pc;
    const std::uint16_t stackBefore = before.cpu.s;
    const std::uint16_t lineBefore = before.vpos;
    bool unreadable = false;
    const std::optional<Pending> pending = pendingTarget(map, rom, before, unreadable);
    const Cpu65816Mode mode = modeOf(before.cpu);
    if (unreadable && unreadableSites.insert(site).second) {
      notes.push_back("run: the jump at " + formatAddress(site, 24) +
                      " reads its pointer from memory the run cannot see; not recorded");
    }

    recorder.site = site;
    spent += machine.step();

    // A step runs one instruction, or one cycle of a transfer, never a whole
    // frame, so the beam wrapping to line 0 is a frame boundary seen exactly once.
    if (machine.state().vpos < lineBefore) {
      ++frame;
      presentPads(machine, input, frame);
      recorder.frameBegan();
    }

    if (!pending) continue;
    // The landing confirms the pointer. A step that serviced an interrupt instead
    // lands in the handler and the instruction has not run yet; it is seen when it
    // does. A landing elsewhere means the reading of the form is wrong, and that is
    // said rather than recorded.
    const SnesState& after = machine.state();
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
        notes.push_back("run: the jump at " + formatAddress(site, 24) + " read a pointer to " +
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

  recorder.finish();
  machine.setObserver(nullptr);
  observation.moved = std::move(recorder.out);
  std::sort(observation.moved.begin(), observation.moved.end(), rangeBefore);
  return observation;
}

}  // namespace snaggletooth::disasm
