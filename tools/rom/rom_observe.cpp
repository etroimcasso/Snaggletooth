#include "rom/rom_observe.h"

#include <algorithm>
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

}  // namespace

bool sameSighting(const ReachedTarget& a, const ReachedTarget& b) {
  return a.site == b.site && a.target == b.target &&
         contextOf(a.mode).bits == contextOf(b.mode).bits;
}

std::vector<ReachedTarget> observeRun(std::span<const std::uint8_t> rom, std::uint64_t masterCycles,
                                      std::vector<std::string>& notes) {
  std::vector<ReachedTarget> out;
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  if (!header) {
    notes.push_back("no run: the image is too small to hold a cartridge header at any site");
    return out;
  }
  const CartridgeMap map = header->map;

  Snes machine{SnesConfig{.rom = rom}};
  std::set<std::tuple<Address, Address, std::uint32_t>> seen;
  std::set<Address> unreadableSites;
  std::set<Address> unconfirmedSites;

  std::uint64_t spent = 0;
  while (spent < masterCycles) {
    // `state()` is the live machine: everything read from it before the step is
    // copied out here, since the step rewrites it.
    const SnesState& before = machine.state();
    const Address site = (static_cast<Address>(before.cpu.pbr) << 16) | before.cpu.pc;
    const std::uint16_t stackBefore = before.cpu.s;
    bool unreadable = false;
    const std::optional<Pending> pending = pendingTarget(map, rom, before, unreadable);
    const Cpu65816Mode mode = modeOf(before.cpu);
    if (unreadable && unreadableSites.insert(site).second) {
      notes.push_back("run: the jump at " + formatAddress(site, 24) +
                      " reads its pointer from memory the run cannot see; not recorded");
    }

    spent += machine.step();

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
  return out;
}

}  // namespace snaggletooth::disasm
