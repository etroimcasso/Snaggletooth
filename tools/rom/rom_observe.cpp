#include "rom/rom_observe.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "ir/cpu65816_lift.h"
#include "ir/ir_lockstep.h"
#include "ir/ir_text.h"
#include "snaggletooth/cpu/cpu65816.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {
namespace {

// The address the tree places the bytes the CPU holds at `address`: the one
// home of the image bytes it reads, so a bank that mirrors the image names the
// bank the image is written for. An address outside the image is its own.
Address placed(CartridgeMap map, std::size_t imageBytes, Address address) {
  const std::optional<std::size_t> offset = romOffset(map, address, imageBytes);
  if (!offset) return address;
  const std::optional<std::uint32_t> home = romAddress(map, *offset);
  return home ? *home : address;
}

bool inImage(CartridgeMap map, std::size_t imageBytes, Address address) {
  return romOffset(map, address, imageBytes).has_value();
}

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
// transfer engines move and groups them into ranges, and hands every CPU
// access and cycle to the step observer the lockstep reads. A range is open
// while the next byte lands where the channel's step says; it closes at a
// break in the step, at a new trigger for its channel, at the start of a frame
// for the HDMA engine's — a new walk of the table — and at the end of the run.
// A closed range that was seen before is counted, not repeated.
//
// The loop tells the recorder the instruction about to run before every step,
// so a CPU write to `MDMAEN` or `HDMAEN` — which the observer sees like any
// other access — names the site every byte the channel then moves belongs to.
struct Recorder final : BusObserver {
  const Snes& machine;
  Address site = 0;  // the instruction about to run
  ir::StepObserver cpu;  // what the CPU did this step: its fetches, its data accesses, its cycles

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
      cpu.access(a);
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

  void internal(std::uint32_t address, std::optional<CycleKind> kind) override {
    cpu.internal(address, kind);
  }
};

// The four forms whose destination the bytes do not name and the run reads
// from the pointer instead: their landing is a reached target, never a
// landing the run has to record from where the CPU went.
bool indirectForm(const ir::Instruction& instruction) {
  const bool indirect = instruction.addressing == ir::Addressing::AbsoluteIndirect ||
                        instruction.addressing == ir::Addressing::AbsoluteIndirectLong ||
                        instruction.addressing == ir::Addressing::AbsoluteIndexedIndirect;
  return indirect && (instruction.flow == ir::Flow::Jump || instruction.flow == ir::Flow::Call);
}

// The interpreter run beside the machine over the whole run: every executed
// instruction lifted from the bytes the CPU fetched, checked, and read for
// where the CPU went next and what its registers held.
struct Lockstep {
  CartridgeMap map;
  std::size_t imageBytes;
  const Cpu65816Backend& backend = cpu65816Backend();
  ir::Interpreter interpreter;
  const std::vector<ir::Effect> nmi = ir::interruptSequence(ir::Interrupt::Nmi);
  const std::vector<ir::Effect> irq = ir::interruptSequence(ir::Interrupt::Irq);

  // The nodes lifted so far, one per address, mode and bytes — so bytes the
  // program rewrote at an address are a second node there, and a mirror bank
  // shares the node of the bank the tree places the bytes in.
  using NodeKey = std::tuple<Address, std::uint32_t, std::vector<std::uint8_t>>;
  std::map<NodeKey, ir::Node> nodes;

  // Where the run's own flow says execution may arrive without an instruction
  // naming it: the address after every call taken, for its return, and every
  // instruction a hardware interrupt interrupted, for the handler's `RTI`. All
  // as the tree places them.
  std::set<Address> expectedReturns;

  // What the run recorded.
  std::set<std::tuple<Address, Address, std::uint32_t>> landings;  // site, target, mode
  std::vector<Landing> ran;
  struct Values {
    std::set<std::uint16_t> d;
    std::set<std::uint8_t> dbr;
  };
  std::map<Address, Values> seen;
  std::set<Address> notedSites;  // a site already named in the notes
  std::vector<ir::Divergence> divergences;
  std::uint64_t instructions = 0;
  std::uint64_t interrupts = 0;
  std::uint64_t diverged = 0;
  std::uint64_t steps = 0;

  Lockstep(CartridgeMap m, std::size_t bytes, const Cpu65816State& start)
      : map(m), imageBytes(bytes) {
    interpreter.registers = ir::registersOf(start);
  }

  std::string modeText(const Cpu65816Mode& mode) const {
    return backend.describe(contextOf(mode));
  }

  // One step the machine took, from the state before it to the state after.
  void step(const Cpu65816State& before, const Cpu65816State& after,
            const ir::StepObserver& observed, std::vector<std::string>& notes) {
    const std::uint64_t ordinal = steps++;
    if (before.run != CpuRunState::Running) {
      // A halted cycle: nothing ran. If a line ended the wait, the interpreter's
      // wait ends with it.
      if (after.run == CpuRunState::Running) interpreter.release();
      return;
    }
    // A transfer engine held the bus for the whole step; the instruction is
    // still ahead of the CPU and is seen on the step that runs it.
    if (!observed.cpuRan) return;

    const Address rawSite = (static_cast<Address>(before.pbr) << 16) | before.pc;
    const Address site = placed(map, imageBytes, rawSite);
    ir::Divergence prototype;
    prototype.instruction = ordinal;
    prototype.site = site;
    const std::size_t divergencesBefore = divergences.size();

    // What the machine took: a hardware request due at the boundary, or the
    // instruction at the program counter.
    const bool nmiTaken = before.nmiPending;
    const bool irqTaken = !nmiTaken && before.irqLine && (before.p & kCpuFlagI) == 0;
    if (nmiTaken || irqTaken) {
      prototype.name = nmiTaken ? "NMI" : "IRQ";
      ir::checkInterrupt(interpreter, nmiTaken ? nmi : irq, observed, after, prototype, divergences);
      ++interrupts;
      expectedReturns.insert(site);
      if (divergences.size() != divergencesBefore) noteDivergence(prototype.name, site, before, notes);
      return;
    }

    // The instruction, from the bytes the CPU fetched, decoded under the mode
    // the CPU was in and placed where the tree places it.
    std::vector<std::uint8_t> bytes;
    for (const BusAccess& fetch : observed.fetches) bytes.push_back(fetch.value);
    const Cpu65816Mode mode = modeOf(before);
    const NodeKey key{site, contextOf(mode).bits, bytes};
    auto found = nodes.find(key);
    if (found == nodes.end()) {
      const std::optional<Decoded> decoded = backend.decode(bytes, site, site, contextOf(mode));
      if (!decoded || decoded->instruction.length != bytes.size()) {
        // The chip fetched bytes the decoder does not read as one instruction of
        // that length. Nothing is known to run, so nothing is checked; said once.
        if (notedSites.insert(site).second) {
          notes.push_back("run: the bytes the CPU fetched at " + formatAddress(site, 24) +
                          " do not decode as one instruction under " + modeText(mode) +
                          "; the step is not checked");
        }
        interpreter.registers = ir::registersOf(after);
        return;
      }
      found = nodes.emplace(key, ir::liftInstruction(decoded->instruction, mode)).first;
    }
    const ir::Node& node = found->second;

    // What the run saw at the site, before the instruction ran.
    if (inImage(map, imageBytes, rawSite)) {
      Values& values = seen[site];
      values.d.insert(before.d);
      values.dbr.insert(before.dbr);
    }

    ir::checkNode(interpreter, node, observed, after, prototype, divergences);
    ++instructions;
    if (divergences.size() != divergencesBefore) {
      noteDivergence(describeNode(node), site, before, notes);
    }

    // Where the CPU went, against what the instruction names: the address
    // after it, for a form that falls through; its constant target; the
    // pointer's target, for the four forms the run reads ahead; the vector, for
    // a software interrupt; and for a return, an address the run's own calls
    // and interrupts said to expect one at. A call's return is expected after
    // it, whichever instruction returns there.
    const ir::Instruction& instruction = node.instruction;
    const Address following = backend.following(site, instruction.length);
    if (instruction.flow == ir::Flow::Call) expectedReturns.insert(following);
    if (after.run != CpuRunState::Running) return;  // a wait or a stop: no landing
    const Address rawLanded = (static_cast<Address>(after.pbr) << 16) | after.pc;
    const Address landed = placed(map, imageBytes, rawLanded);
    const bool fallsThrough = instruction.flow == ir::Flow::Continue ||
                              instruction.flow == ir::Flow::Branch ||
                              instruction.flow == ir::Flow::Call;
    const bool named =
        rawLanded == rawSite ||  // a block move with bytes left, run again
        (fallsThrough && landed == following) ||
        (instruction.target && landed == placed(map, imageBytes, *instruction.target)) ||
        indirectForm(instruction) ||  // a reached target
        instruction.mnemonic == std::string_view("BRK") ||  // the vector the header names
        instruction.mnemonic == std::string_view("COP") ||
        (instruction.flow == ir::Flow::Return && expectedReturns.count(landed) != 0);
    if (named) return;
    if (!inImage(map, imageBytes, rawLanded)) {
      if (notedSites.insert(site).second) {
        notes.push_back("run: the CPU arrived at " + formatAddress(rawLanded, 24) + " from " +
                        formatAddress(site, 24) + ", which the tree does not hold; not recorded");
      }
      return;
    }
    const Cpu65816Mode arrived = modeOf(after);
    if (!landings.insert({site, landed, contextOf(arrived).bits}).second) return;
    ran.push_back(Landing{.target = landed, .mode = arrived, .site = site, .name = {}});
  }

  // The mode the CPU is in, as the trace carries one: the widths are known —
  // the CPU has them — and the carry is not remembered, since nothing here
  // follows an `XCE`.
  static Cpu65816Mode modeOf(const Cpu65816State& cpu) {
    return {.emulation = cpu.e,
            .accumulator8 = (cpu.p & kCpuFlagM) != 0,
            .index8 = (cpu.p & kCpuFlagX) != 0,
            .accumulatorKnown = true,
            .indexKnown = true,
            .carryKnown = false,
            .carry = false};
  }

  static std::string describeNode(const ir::Node& node) {
    return "`" + std::string(node.instruction.mnemonic) + " " +
           std::string(ir::addressingName(node.instruction.addressing)) + "`";
  }

  // A disagreement, said once per site with what disagreed first.
  void noteDivergence(const std::string& what, Address site, const Cpu65816State& before,
                      std::vector<std::string>& notes) {
    ++diverged;
    if (!notedSites.insert(site).second) return;
    const ir::Divergence& d = divergences.back();
    char values[64];
    std::snprintf(values, sizeof values, "machine $%X, the lift $%X", d.expected, d.actual);
    notes.push_back("run: the lift of " + what + " at " + formatAddress(site, 24) + " under " +
                    modeText(modeOf(before)) + " disagreed with the machine (" + d.what + ": " +
                    values + "); the interpreter was realigned");
  }
};

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
  Lockstep lockstep{map, rom.size(), machine.state().cpu};
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
    const Cpu65816State cpuBefore = before.cpu;
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
    recorder.cpu.clear();
    spent += machine.step();

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
    lockstep.step(cpuBefore, after.cpu, recorder.cpu, notes);

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
  return observation;
}

}  // namespace snaggletooth::disasm
