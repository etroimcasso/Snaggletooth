#include "snaggletooth/snes/snes.h"

#include <cstddef>
#include <utility>

namespace snaggletooth {
namespace {

// The APU advances by an exact rational share of the master cycles that elapse.
// The APU runs off a 24.576 MHz crystal divided by 24 — a 1,024,000 Hz cycle rate
// — in both regions; only the master clock differs. The NTSC master clock is
// 236,250,000 / 11 Hz and the PAL master clock is 21,281,370 Hz, so the ratio of
// APU cycles to master cycles reduces to lowest terms as:
//   NTSC: 1,024,000 / (236,250,000 / 11) = 5632 / 118125
//   PAL:  1,024,000 / 21,281,370         = 102400 / 2128137
// The machine accumulates the numerator per master cycle and takes an APU cycle
// for every denominator, so the count run after N master cycles is exactly
// floor(N * num / den) — no floating point, no rounding drift.
struct ApuRatio {
  std::uint32_t num;
  std::uint32_t den;
};
constexpr ApuRatio kNtscApu{.num = 5632u, .den = 118125u};
constexpr ApuRatio kPalApu{.num = 102400u, .den = 2128137u};

}  // namespace

Snes::Snes(SnesConfig config)
    : rom_(config.rom.begin(), config.rom.end()), region_(config.region) {
  const ApuRatio ratio = region_ == Region::Pal ? kPalApu : kNtscApu;
  apuNum_ = ratio.num;
  apuDen_ = ratio.den;
  state_.apu = apu_.state();  // the APU's seeded post-boot ready state
  state_.cpu = powerOnCpu();  // emulation mode, the program counter at the reset vector
  load();
}

void Snes::restore(SnesState state) {
  state_ = std::move(state);
  load();
}

void Snes::load() {
  cpu_.restore(state_.cpu);
  apu_.restore(state_.apu);
}

void Snes::sync() {
  state_.cpu = cpu_.state();
  state_.apu = apu_.state();
}

Cpu65816State Snes::powerOnCpu() const {
  const std::uint16_t reset = static_cast<std::uint16_t>(
      romByte(0x00, 0xFFFC) |
      (static_cast<std::uint16_t>(romByte(0x00, 0xFFFD)) << 8));
  return Cpu65816State{
      .pc = reset,
      .s = 0x01FF,
      .p = static_cast<std::uint8_t>(kCpuFlagM | kCpuFlagX | kCpuFlagI),
      .e = true,
      .run = CpuRunState::Running,
  };
}

void Snes::machineCycle() {
  // A bus access overwrites this with its region's cost; a halted cycle makes no
  // access and keeps the fast rate, the same rate an internal cycle charges.
  lastCost_ = 6;
  Bus bus{*this};
  cpu_.stepCycle(bus);

  state_.master += lastCost_;
  state_.apuPhase += lastCost_ * apuNum_;
  const std::uint64_t apuCycles = state_.apuPhase / apuDen_;
  state_.apuPhase %= apuDen_;
  if (apuCycles != 0) apu_.run(apuCycles);
}

std::uint32_t Snes::step() {
  const std::uint64_t before = state_.master;
  if (cpu_.state().run != CpuRunState::Running) {
    machineCycle();  // one idle cycle, which may be the one that ends a wait
  } else {
    do {
      machineCycle();
    } while (!cpu_.atInstructionBoundary());
  }
  state_.consumed = state_.master;  // an instruction lands on a cycle boundary, carrying nothing
  sync();
  return static_cast<std::uint32_t>(state_.master - before);
}

std::uint64_t Snes::run(std::uint64_t budget) {
  state_.consumed += budget;
  while (state_.master < state_.consumed) machineCycle();
  sync();
  return budget;
}

std::vector<StereoFrame> Snes::takeFrames() { return apu_.takeFrames(); }

std::uint32_t Snes::accessCost(std::uint32_t address) const noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  const bool fast2 = (state_.memsel & 1u) != 0u;  // the second waitstate region under MEMSEL

  // Work RAM and the first waitstate region span whole banks and always run slow.
  if (bank >= 0x7E && bank <= 0x7F) return 8u;   // work RAM
  if (bank >= 0x40 && bank <= 0x7D) return 8u;   // the first LoROM region
  if (bank >= 0xC0) return fast2 ? 6u : 8u;      // the second LoROM region, under MEMSEL

  // System banks $00-$3F and $80-$BF share one layout below $8000, then LoROM.
  if (offset <= 0x1FFF) return 8u;               // work-RAM mirror
  if (offset <= 0x3FFF) return 6u;               // registers and unused pages, fast
  if (offset <= 0x41FF) return 12u;              // the manual joypad ports, extra slow
  if (offset <= 0x5FFF) return 6u;               // more registers, fast
  if (offset <= 0x7FFF) return 8u;               // the expansion region, slow
  return (bank >= 0x80) ? (fast2 ? 6u : 8u)      // $80-$BF LoROM follows MEMSEL
                        : 8u;                     // $00-$3F LoROM is always slow
}

std::uint8_t Snes::romByte(std::uint8_t bank, std::uint16_t offset) const noexcept {
  if (rom_.empty()) return 0;
  // LoROM lays each bank's upper half end to end; a bank's high bit only selects
  // the waitstate region, so it is masked off here. The image mirrors to fill the
  // address space.
  const std::size_t index =
      ((static_cast<std::size_t>(bank) & 0x7Fu) << 15) |
      (static_cast<std::size_t>(offset) & 0x7FFFu);
  return rom_[index % rom_.size()];
}

std::uint8_t Snes::busRead(std::uint32_t address) {
  lastCost_ = accessCost(address);
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);

  if (bank >= 0x7E && bank <= 0x7F) {
    return latch(state_.wram[(static_cast<std::size_t>(bank - 0x7E) << 16) | offset]);
  }
  const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
  if (systemBank) {
    if (offset <= 0x1FFF) return latch(state_.wram[offset]);
    if (offset >= 0x2140 && offset <= 0x217F) {
      return latch(apu_.readPort(static_cast<std::uint8_t>(offset & 3u)));
    }
    if (offset >= 0x2180 && offset <= 0x2183) return readWramPort(offset);
  }
  if (offset >= 0x8000) return latch(romByte(bank, offset));
  return state_.mdr;  // an unmapped read returns the last value the data bus carried
}

void Snes::busWrite(std::uint32_t address, std::uint8_t value) {
  lastCost_ = accessCost(address);
  state_.mdr = value;  // a write drives the data bus
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);

  if (bank >= 0x7E && bank <= 0x7F) {
    state_.wram[(static_cast<std::size_t>(bank - 0x7E) << 16) | offset] = value;
    return;
  }
  const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
  if (systemBank) {
    if (offset <= 0x1FFF) {
      state_.wram[offset] = value;
      return;
    }
    if (offset >= 0x2140 && offset <= 0x217F) {
      apu_.writePort(static_cast<std::uint8_t>(offset & 3u), value);
      return;
    }
    if (offset >= 0x2180 && offset <= 0x2183) {
      writeWramPort(offset, value);
      return;
    }
    if (offset == 0x420D) {
      state_.memsel = static_cast<std::uint8_t>(value & 1u);
      return;
    }
  }
  // A write to ROM or to an unmapped address changes nothing beyond the data bus.
}

std::uint8_t Snes::readWramPort(std::uint16_t offset) {
  if (offset == 0x2180) {
    const std::uint8_t v = state_.wram[state_.wmadd & 0x1FFFFu];
    state_.wmadd = (state_.wmadd + 1u) & 0x1FFFFu;
    return latch(v);
  }
  return state_.mdr;  // $2181-$2183 are write-only
}

void Snes::writeWramPort(std::uint16_t offset, std::uint8_t value) {
  switch (offset) {
    case 0x2180:
      state_.wram[state_.wmadd & 0x1FFFFu] = value;
      state_.wmadd = (state_.wmadd + 1u) & 0x1FFFFu;
      break;
    case 0x2181:
      state_.wmadd = (state_.wmadd & 0x1FF00u) | value;
      break;
    case 0x2182:
      state_.wmadd = (state_.wmadd & 0x100FFu) |
                     (static_cast<std::uint32_t>(value) << 8);
      break;
    case 0x2183:
      state_.wmadd = (state_.wmadd & 0x0FFFFu) |
                     (static_cast<std::uint32_t>(value & 1u) << 16);
      break;
    default:
      break;
  }
}

}  // namespace snaggletooth
