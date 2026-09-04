#include "snaggletooth/snes/snes.h"

#include <cstddef>
#include <utility>

#include "snes_ipl_stub.h"

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

// The scanline structure. A line is 1364 master cycles (341 dots) except NTSC's line
// 240 on an odd frame, which is four cycles short to keep the colour signal in step;
// a frame is 262 lines on NTSC and 312 on PAL. Vblank begins at line 225 (overscan,
// which would move it to 240, is out of scope with the rest of the real PPU) and runs
// to the last line; line 0 is the end of vblank. The active picture spans master
// cycles 88..1112 within a line, which bounds the horizontal-blank flag.
constexpr std::uint16_t kLineMaster = 1364u;
constexpr std::uint16_t kShortLineMaster = 1360u;
constexpr std::uint16_t kShortLineV = 240u;
constexpr std::uint16_t kVblankStartLine = 225u;
constexpr std::uint16_t kNtscLines = 262u;
constexpr std::uint16_t kPalLines = 312u;
constexpr std::uint16_t kActiveStart = 88u;
constexpr std::uint16_t kActiveEnd = 1112u;

// The multiply/divide unit is clocked by the CPU, so its documented latencies are
// counted in CPU cycles — the same wait no matter the memory speed. Multiply is
// ready after 8, divide after 16.
constexpr std::uint8_t kMultiplyClocks = 8u;
constexpr std::uint8_t kDivideClocks = 16u;

// The auto-joypad read holds the busy flag for a fixed window each frame.
constexpr std::uint16_t kAutoJoyClocks = 4224u;

// The largest save a cartridge can address through any window.
constexpr std::size_t kMaxSaveRamBytes = 128u * 1024u;

}  // namespace

Snes::Snes(SnesConfig config)
    : rom_(config.rom.begin(), config.rom.end()), region_(config.region) {
  map_ = config.map.value_or(detectCartridgeMap(rom_));
  const std::size_t save = config.saveRamBytes.value_or(declaredSaveRamBytes(rom_));
  state_.sram.assign(save > kMaxSaveRamBytes ? kMaxSaveRamBytes : save, 0u);
  const ApuRatio ratio = region_ == Region::Pal ? kPalApu : kNtscApu;
  apuNum_ = ratio.num;
  apuDen_ = ratio.den;
  state_.apu = apu_.state();  // the APU's seeded post-boot ready state
  if (config.iplStub) {
    // Seed the upload stub over the ready state: the audio CPU runs the handshake
    // and posts its own ready bytes, the way it does when the console powers on.
    // Left off, the machine keeps the ready state, which is how a program loaded
    // straight into audio RAM skips the handshake.
    // A supplied boot ROM takes the stub's place, and the audio unit runs it.
    const std::span<const std::uint8_t, kIplWindowBytes> image =
        config.bootRom.has_value() ? std::span<const std::uint8_t, kIplWindowBytes>(*config.bootRom)
                                   : std::span<const std::uint8_t, kIplWindowBytes>(iplStubImage());
    seedIplStub(state_.apu, image);
    // Map the same image over the $FFC0 window. The upload shell scratch-writes
    // that range in RAM and re-enters it expecting the boot code to read back
    // unchanged; the mapping serves the image to the CPU while CONTROL bit 7 is set,
    // so those reads survive the driver's writes to the RAM beneath.
    apu_.mapIplRom(image);
  }
  state_.cpu = powerOnCpu();  // emulation mode, the program counter at the reset vector
  load();
}

void Snes::restore(const SnesState& state) {
  state_ = state;
  load();
}

void Snes::load() {
  cpu_.restore(state_.cpu);
  apu_.restore(state_.apu);
  // The NMI pin's remembered level is not part of the snapshot, so re-derive it from
  // the flags and enables and sync it WITHOUT minting an edge: the pending latch, if
  // one was in flight, rides the snapshot on its own. Driving it as a fresh assertion
  // would double a request the restore already carries. The IRQ line is level-only, so
  // setting it plainly is correct.
  cpu_.syncNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
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
  videoAdvanced_ = false;

  // The bus has a priority order: HDMA outranks a general-purpose DMA, which
  // outranks the CPU. HDMA runs a whole event in this one cycle; a general-purpose
  // DMA runs a single byte or overhead cycle; otherwise the CPU steps, and a DMA
  // armed by a recent $420B write engages after this one more CPU cycle.
  if (state_.hdmaRunPending) {
    hdmaCycle();
    state_.hdmaRunPending = false;
  } else if (state_.dmaRunning) {
    dmaCycle();
  } else {
    const bool armedAtStart = state_.dmaArm != 0u;
    Bus bus{*this};
    cpu_.stepCycle(bus);
    if (armedAtStart && --state_.dmaArm == 0u && state_.mdmaen != 0u) {
      state_.dmaRunning = true;
      state_.dmaOpened = false;
      state_.dmaChannelOpened = false;
      state_.dmaUnit = 0u;
      state_.dmaPauseMaster = state_.master + lastCost_;  // the pause is the end of this cycle
    }
  }

  // Every access ticks the machine's events before its value resolves; a halted
  // cycle makes no access, so tick it here — time still passes while the CPU sits.
  // A halted cycle can also be the first one after a transfer, so it too owes the
  // resume-rounding pad the bus callbacks apply on a live cycle.
  if (!videoAdvanced_) {
    if (state_.dmaResumePad) {
      lastCost_ += resumePad(lastCost_);
      state_.dmaResumePad = false;
    }
    tickVideo(lastCost_);
  }

  // Drive the interrupt lines from the flags and enables as they now stand, after any
  // register write this cycle, so the level is settled for the next fetch to sample.
  driveLines();

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

namespace {

[[nodiscard]] constexpr std::uint32_t busAddress(std::uint8_t bank, std::uint16_t offset) noexcept {
  return (static_cast<std::uint32_t>(bank) << 16) | offset;
}

}  // namespace

std::uint8_t Snes::romByte(std::uint8_t bank, std::uint16_t offset) const noexcept {
  const std::optional<std::size_t> index = romOffset(map_, busAddress(bank, offset), rom_.size());
  return index.has_value() ? rom_[*index] : std::uint8_t{0};
}

bool Snes::addressIsRom(std::uint8_t bank, std::uint16_t offset) const noexcept {
  return cartridgeRegion(map_, busAddress(bank, offset)) == CartridgeRegion::Rom;
}

std::optional<std::size_t> Snes::saveRamIndex(std::uint8_t bank,
                                              std::uint16_t offset) const noexcept {
  if (state_.sram.empty()) return std::nullopt;
  // The offset is reduced to the declared size, so a small save repeats across
  // its window.
  const std::optional<std::size_t> linear = saveRamOffset(map_, busAddress(bank, offset));
  if (!linear.has_value()) return std::nullopt;
  return *linear % state_.sram.size();
}

std::uint8_t Snes::busRead(std::uint32_t address) {
  const std::uint32_t cost = accessCost(address);
  lastCost_ = cost;
  if (state_.dmaResumePad) {  // the first cycle after a transfer pays the resume-rounding pad
    lastCost_ += resumePad(cost);
    state_.dmaResumePad = false;
  }
  tickVideo(lastCost_);  // tick-first: the read sees the event it shares the cycle with
  videoAdvanced_ = true;
  return routeRead(address);
}

void Snes::busWrite(std::uint32_t address, std::uint8_t value) {
  const std::uint32_t cost = accessCost(address);
  lastCost_ = cost;
  if (state_.dmaResumePad) {
    lastCost_ += resumePad(cost);
    state_.dmaResumePad = false;
  }
  tickVideo(lastCost_);  // tick-first, so a write lands after the event it shares the cycle with
  videoAdvanced_ = true;
  routeWrite(address, value);
}

std::uint8_t Snes::routeRead(std::uint32_t address) {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);

  if (bank >= 0x7E && bank <= 0x7F) {
    return latch(state_.wram[(static_cast<std::size_t>(bank - 0x7E) << 16) | offset]);
  }
  const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
  if (systemBank) {
    if (offset <= 0x1FFF) return latch(state_.wram[offset]);
    if (offset >= 0x2100 && offset <= 0x213F) return readPpuReg(offset);
    if (offset >= 0x2140 && offset <= 0x217F) {
      return latch(apu_.readPort(static_cast<std::uint8_t>(offset & 3u)));
    }
    if (offset >= 0x2180 && offset <= 0x2183) return readWramPort(offset);
    if (offset == 0x4016 || offset == 0x4017) return readJoypadPort(offset);
    if (offset >= 0x4200 && offset <= 0x421F) return readCpuReg(offset);
    if (offset >= 0x4300 && offset <= 0x437F) return readDmaReg(offset);
  }
  if (const std::optional<std::size_t> save = saveRamIndex(bank, offset)) {
    return latch(state_.sram[*save]);
  }
  if (addressIsRom(bank, offset)) return latch(romByte(bank, offset));
  return state_.mdr;  // an unmapped read returns the last value the data bus carried
}

void Snes::routeWrite(std::uint32_t address, std::uint8_t value) {
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
    if (offset >= 0x2100 && offset <= 0x213F) {
      writePpuReg(offset, value);
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
    if (offset == 0x4016) {
      writeJoypadStrobe(value);
      return;
    }
    if (offset >= 0x4200 && offset <= 0x421F) {
      writeCpuReg(offset, value);
      return;
    }
    if (offset >= 0x4300 && offset <= 0x437F) {
      writeDmaReg(offset, value);
      return;
    }
  }
  if (const std::optional<std::size_t> save = saveRamIndex(bank, offset)) {
    state_.sram[*save] = value;
    return;
  }
  // A write to ROM or to an unmapped address changes nothing beyond the data bus.
}

std::uint8_t Snes::readWramPort(std::uint16_t offset) {
  if (offset == 0x2180) {
    const std::uint32_t at = state_.wmadd & 0x1FFFFu;
    const std::uint8_t v = state_.wram[at];
    state_.wmadd = (at + 1u) & 0x1FFFFu;
    // The port's own read of work RAM, at the bank-$7E address it reached; the
    // access to $2180 that asked for it is reported by whoever made it.
    observe(0x7E0000u | at, v, false, CycleKind::DataRead, AccessSource::WramPort);
    return latch(v);
  }
  return state_.mdr;  // $2181-$2183 are write-only
}

void Snes::writeWramPort(std::uint16_t offset, std::uint8_t value) {
  switch (offset) {
    case 0x2180: {
      const std::uint32_t at = state_.wmadd & 0x1FFFFu;
      state_.wram[at] = value;
      state_.wmadd = (at + 1u) & 0x1FFFFu;
      observe(0x7E0000u | at, value, true, CycleKind::DataWrite, AccessSource::WramPort);
      break;
    }
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

std::uint16_t Snes::lineLength() const noexcept {
  // NTSC drops one dot (four master cycles) from line 240 on odd frames.
  if (region_ == Region::Ntsc && state_.field == 1u && state_.vpos == kShortLineV) {
    return kShortLineMaster;
  }
  return kLineMaster;
}

bool Snes::irqConditionMet() const noexcept {
  const std::uint8_t mode = static_cast<std::uint8_t>((state_.nmitimen >> 4) & 3u);
  if (mode == 0u) return false;
  const std::uint16_t hdot = static_cast<std::uint16_t>(state_.hpos >> 2);  // four master cycles per dot
  const bool hReached = hdot >= state_.htime;   // the H compare, met once the beam passes the dot
  const bool vAt = state_.vpos == state_.vtime;  // the V compare, met across the whole line
  switch (mode) {
    case 1: return hReached;          // H=H at any line
    case 2: return vAt;               // V=V at H=0 (the line start)
    default: return hReached && vAt;  // H=H and V=V
  }
}

void Snes::advanceLine() noexcept {
  const std::uint16_t total = region_ == Region::Ntsc ? kNtscLines : kPalLines;
  state_.vpos = static_cast<std::uint16_t>(state_.vpos + 1u);
  if (state_.vpos >= total) {
    state_.vpos = 0u;
    state_.field ^= 1u;  // the next frame carries the other parity
  }
  state_.hdmaLineFired = false;  // each scanline may trigger its own HDMA delivery
  if (state_.vpos == kVblankStartLine) {
    state_.vblankNmi = true;  // the NMI flag is set at the start of vblank, whether or not NMIs are enabled
    state_.hdmaActive = 0u;   // and every HDMA channel deactivates for the rest of the frame
    if ((state_.nmitimen & 1u) != 0u) {
      // The auto-joypad read runs each frame it is enabled: it strobes the pads
      // now, clocks their bits out over the window, and lands them as it ends.
      latchJoypads();
      state_.autoJoyClocks = kAutoJoyClocks;
    }
  }
  if (state_.vpos == 0u) {
    state_.vblankNmi = false;    // and clears at the end of vblank
    state_.hdmaInited = false;   // the new frame re-initialises HDMA at line 0
  }
}

void Snes::tickVideo(std::uint32_t cost) {
  // The arithmetic unit steps once per CPU cycle regardless of the cycle's master
  // cost; its result lands when the countdown reaches zero.
  if (state_.mathClocks != 0u) {
    --state_.mathClocks;
    if (state_.mathClocks == 0u) commitMath();
  }
  // The auto-joypad busy window is counted in master cycles; the result registers
  // take the bits it read as it closes.
  if (state_.autoJoyClocks != 0u) {
    state_.autoJoyClocks = state_.autoJoyClocks > cost
        ? static_cast<std::uint16_t>(state_.autoJoyClocks - cost)
        : std::uint16_t{0u};
    if (state_.autoJoyClocks == 0u) finishAutoJoypadRead();
  }

  // Advance the beam by the cycle's master cost, wrapping scanlines as it goes. The
  // H/V-timer flag latches on the rising edge of its condition, so a compare the step
  // moves across raises it exactly once.
  const bool metBefore = irqConditionMet();
  state_.hpos = static_cast<std::uint16_t>(state_.hpos + cost);
  while (state_.hpos >= lineLength()) {
    state_.hpos = static_cast<std::uint16_t>(state_.hpos - lineLength());
    advanceLine();
  }
  if (irqConditionMet() && !metBefore) state_.timeup = true;

  // HDMA triggers, each latched so it fires once: the frame's initialisation as the
  // beam passes dot 6 of line 0, and a delivery as it passes dot 278 of every
  // visible line. Triggering only marks the event pending; it runs on the next
  // machine cycle, so it preempts a general-purpose DMA at a whole byte.
  if (!state_.hdmaInited && state_.vpos == 0u && state_.hpos >= 24u &&
      state_.hdmaen != 0u) {
    state_.hdmaInited = true;
    state_.hdmaRunPending = true;
    state_.hdmaIniting = true;
  }
  if (!state_.hdmaLineFired && state_.vpos <= 224u && state_.hpos >= kActiveEnd &&
      state_.hdmaActive != 0u) {
    state_.hdmaLineFired = true;
    state_.hdmaRunPending = true;
    state_.hdmaIniting = false;
  }
}

void Snes::driveLines() {
  // The NMI line is asserted while the flag is set and NMIs are enabled; enabling
  // while the flag is high raises the line, which is the documented mid-vblank
  // trigger. The IRQ line follows the timer flag directly.
  cpu_.setNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
}

void Snes::commitMath() noexcept {
  switch (state_.mathOp) {
    case MathOp::Multiply:
      // The quotient register already took the multiplier when the multiply started;
      // now the product lands. (WRMPYA * WRMPYB fits sixteen bits.)
      state_.rdmpy = static_cast<std::uint16_t>(state_.wrmpya * state_.wrmpyb);
      break;
    case MathOp::Divide:
      if (state_.wrdivb == 0u) {
        state_.rddiv = 0xFFFFu;        // dividing by zero yields an all-ones quotient
        state_.rdmpy = state_.wrdiv;   // and the dividend as the remainder
      } else {
        state_.rddiv = static_cast<std::uint16_t>(state_.wrdiv / state_.wrdivb);
        state_.rdmpy = static_cast<std::uint16_t>(state_.wrdiv % state_.wrdivb);
      }
      break;
    case MathOp::None:
      break;
  }
  state_.mathOp = MathOp::None;
}

std::uint16_t Snes::vramWordAddress() const noexcept {
  // The address translation left-rotates the low 8, 9 or 10 bits of the word address
  // by three, so a bitmap laid out by increasing tile number reads back as rows.
  const std::uint16_t addr = state_.vmadd;
  switch ((state_.vmain >> 2) & 3u) {
    case 1: return static_cast<std::uint16_t>((addr & 0xFF00u) | ((addr << 3) & 0x00F8u) | ((addr >> 5) & 0x0007u));
    case 2: return static_cast<std::uint16_t>((addr & 0xFE00u) | ((addr << 3) & 0x01F8u) | ((addr >> 6) & 0x0007u));
    case 3: return static_cast<std::uint16_t>((addr & 0xFC00u) | ((addr << 3) & 0x03F8u) | ((addr >> 7) & 0x0007u));
    default: return addr;
  }
}

std::uint16_t Snes::readVramWord() const noexcept {
  const std::uint16_t word = vramWordAddress();
  const std::size_t byte = static_cast<std::size_t>(word) << 1;
  return static_cast<std::uint16_t>(state_.vram[byte & 0xFFFFu] |
                                    (state_.vram[(byte + 1u) & 0xFFFFu] << 8));
}

void Snes::stepVramAddress(bool highByte) noexcept {
  // The increment happens after the low or the high byte, whichever $2115 bit 7
  // selects — so an access to the other byte leaves the address alone.
  const bool incrementOnHigh = (state_.vmain & 0x80u) != 0u;
  if (highByte != incrementOnHigh) return;
  static constexpr std::uint16_t kStep[4] = {1u, 32u, 128u, 128u};
  state_.vmadd = static_cast<std::uint16_t>(state_.vmadd + kStep[state_.vmain & 3u]);
}

std::uint8_t Snes::readPpuReg(std::uint16_t offset) {
  switch (offset) {
    case 0x2139: {  // RDVRAML: the low byte of the prefetch register
      const std::uint8_t v = static_cast<std::uint8_t>(state_.vramLatch & 0xFFu);
      if ((state_.vmain & 0x80u) == 0u) {
        state_.vramLatch = readVramWord();  // prefetch from the OLD address, THEN increment (the documented glitch)
        stepVramAddress(/*highByte=*/false);
      }
      return latch(v);
    }
    case 0x213A: {  // RDVRAMH: the high byte of the prefetch register
      const std::uint8_t v = static_cast<std::uint8_t>(state_.vramLatch >> 8);
      if ((state_.vmain & 0x80u) != 0u) {
        state_.vramLatch = readVramWord();
        stepVramAddress(/*highByte=*/true);
      }
      return latch(v);
    }
    case 0x213B: {  // RDCGRAM: two reads make a word; the high byte's top bit is open bus
      const std::uint16_t byte = static_cast<std::uint16_t>(state_.cgadd) << 1;
      std::uint8_t v;
      if (!state_.cgLatchHigh) {
        v = state_.cgram[byte & 0x1FFu];
        state_.cgLatchHigh = true;
      } else {
        v = static_cast<std::uint8_t>((state_.cgram[(byte + 1u) & 0x1FFu] & 0x7Fu) | (state_.mdr & 0x80u));
        state_.cgadd = static_cast<std::uint8_t>(state_.cgadd + 1u);
        state_.cgLatchHigh = false;
      }
      return latch(v);
    }
    default:
      break;
  }
  return state_.mdr;  // a read of an unmodelled PPU register is open bus
}

void Snes::writePpuReg(std::uint16_t offset, std::uint8_t value) {
  switch (offset) {
    case 0x2100: state_.inidisp = value; return;  // forced blank and brightness
    case 0x2107: state_.bg1sc = value; return;
    case 0x2108: state_.bg2sc = value; return;
    case 0x2109: state_.bg3sc = value; return;
    case 0x210A: state_.bg4sc = value; return;
    case 0x210B: state_.bg12nba = value; return;
    case 0x210C: state_.bg34nba = value; return;
    case 0x2115: state_.vmain = value; return;  // increment mode and address translation
    case 0x2116:
      state_.vmadd = static_cast<std::uint16_t>((state_.vmadd & 0xFF00u) | value);
      state_.vramLatch = readVramWord();  // changing the address prefetches the new word
      return;
    case 0x2117:
      state_.vmadd = static_cast<std::uint16_t>((state_.vmadd & 0x00FFu) | (value << 8));
      state_.vramLatch = readVramWord();
      return;
    case 0x2118: {  // VMDATAL: the low byte of the addressed word
      const std::size_t byte = static_cast<std::size_t>(vramWordAddress()) << 1;
      state_.vram[byte & 0xFFFFu] = value;
      stepVramAddress(/*highByte=*/false);  // a write never prefetches
      return;
    }
    case 0x2119: {  // VMDATAH: the high byte of the addressed word
      const std::size_t byte = static_cast<std::size_t>(vramWordAddress()) << 1;
      state_.vram[(byte + 1u) & 0xFFFFu] = value;
      stepVramAddress(/*highByte=*/true);
      return;
    }
    case 0x2121:  // CGADD: setting the address resets the low/high flip-flop
      state_.cgadd = value;
      state_.cgLatchHigh = false;
      return;
    case 0x2122:  // CGDATA: the low byte is held, the high byte commits the word
      if (!state_.cgLatchHigh) {
        state_.cgLatch = value;
        state_.cgLatchHigh = true;
      } else {
        const std::uint16_t byte = static_cast<std::uint16_t>(state_.cgadd) << 1;
        state_.cgram[byte & 0x1FFu] = state_.cgLatch;
        state_.cgram[(byte + 1u) & 0x1FFu] = static_cast<std::uint8_t>(value & 0x7Fu);
        state_.cgadd = static_cast<std::uint8_t>(state_.cgadd + 1u);
        state_.cgLatchHigh = false;
      }
      return;
    case 0x212C: state_.tm = value; return;
    default: return;  // the stub does not model the other PPU registers
  }
}

std::uint8_t Snes::readCpuReg(std::uint16_t offset) {
  switch (offset) {
    case 0x4210: {  // RDNMI: the vblank flag (bit 7), CPU version 2 (bits 3-0), open bus between
      const std::uint8_t v = static_cast<std::uint8_t>(
          (state_.vblankNmi ? 0x80u : 0x00u) | (state_.mdr & 0x70u) | 0x02u);
      state_.vblankNmi = false;  // reading acknowledges the flag
      return latch(v);
    }
    case 0x4211: {  // TIMEUP: the H/V-timer IRQ flag (bit 7), open bus below
      const std::uint8_t v = static_cast<std::uint8_t>(
          (state_.timeup ? 0x80u : 0x00u) | (state_.mdr & 0x7Fu));
      state_.timeup = false;  // reading acknowledges the flag
      return latch(v);
    }
    case 0x4212: {  // HVBJOY: vblank (bit 7), hblank (bit 6), auto-joypad busy (bit 0), open bus between
      const bool vblank = state_.vpos >= kVblankStartLine;
      const bool hblank = state_.hpos < kActiveStart || state_.hpos >= kActiveEnd;
      const std::uint8_t v = static_cast<std::uint8_t>(
          (vblank ? 0x80u : 0x00u) | (hblank ? 0x40u : 0x00u) | (state_.mdr & 0x3Eu) |
          (state_.autoJoyClocks != 0u ? 0x01u : 0x00u));
      return latch(v);
    }
    case 0x4214: return latch(static_cast<std::uint8_t>(state_.rddiv & 0xFFu));
    case 0x4215: return latch(static_cast<std::uint8_t>(state_.rddiv >> 8));
    case 0x4216: return latch(static_cast<std::uint8_t>(state_.rdmpy & 0xFFu));
    case 0x4217: return latch(static_cast<std::uint8_t>(state_.rdmpy >> 8));
    default:
      break;
  }
  if (offset >= 0x4218 && offset <= 0x421F) {
    return latch(state_.joy[static_cast<std::size_t>(offset - 0x4218)]);  // the auto-read result, as of the last window's end
  }
  return state_.mdr;  // other CPU-register reads are open bus
}

void Snes::writeCpuReg(std::uint16_t offset, std::uint8_t value) {
  switch (offset) {
    case 0x4200:  // NMITIMEN: NMI enable, H/V IRQ mode, auto-joypad enable
      if (((value >> 4) & 3u) == 0u) state_.timeup = false;  // disabling the IRQ acknowledges it
      state_.nmitimen = value;
      return;
    case 0x4202: state_.wrmpya = value; return;
    case 0x4203:  // WRMPYB: its write starts the multiply
      state_.wrmpyb = value;
      state_.rddiv = value;  // the shared unit immediately loads the quotient register with the multiplier
      state_.mathOp = MathOp::Multiply;
      state_.mathClocks = kMultiplyClocks;
      return;
    case 0x4204:
      state_.wrdiv = static_cast<std::uint16_t>((state_.wrdiv & 0xFF00u) | value);
      return;
    case 0x4205:
      state_.wrdiv = static_cast<std::uint16_t>((state_.wrdiv & 0x00FFu) | (value << 8));
      return;
    case 0x4206:  // WRDIVB: its write starts the divide
      state_.wrdivb = value;
      state_.mathOp = MathOp::Divide;
      state_.mathClocks = kDivideClocks;
      return;
    case 0x4207: state_.htime = static_cast<std::uint16_t>((state_.htime & 0x0100u) | value); return;
    case 0x4208: state_.htime = static_cast<std::uint16_t>((state_.htime & 0x00FFu) | ((value & 1u) << 8)); return;
    case 0x4209: state_.vtime = static_cast<std::uint16_t>((state_.vtime & 0x0100u) | value); return;
    case 0x420A: state_.vtime = static_cast<std::uint16_t>((state_.vtime & 0x00FFu) | ((value & 1u) << 8)); return;
    case 0x420B: triggerDma(value); return;             // start a general-purpose DMA on each selected channel
    case 0x420C: state_.hdmaen = value; return;         // enable HDMA on the selected channels
    case 0x420D: state_.memsel = static_cast<std::uint8_t>(value & 1u); return;
    default: return;  // $4201 WRIO and the read-only ports ignore writes
  }
}

// ---- the controller ports -----------------------------------------------------

std::uint16_t Joypad::bits() const noexcept {
  // The wire order, first bit highest: B Y Select Start Up Down Left Right A X L R,
  // then the four identity bits, zero for a standard pad.
  std::uint16_t word = 0;
  const bool order[12] = {b, y, select, start, up, down, left, right, a, x, l, r};
  for (std::size_t i = 0; i < 12; ++i) {
    if (order[i]) word = static_cast<std::uint16_t>(word | (0x8000u >> i));
  }
  return word;
}

void Snes::setJoypad(JoypadPort port, std::optional<Joypad> pad) noexcept {
  state_.pads[static_cast<std::size_t>(port)] = pad;
}

const std::optional<Joypad>& Snes::joypad(JoypadPort port) const noexcept {
  return state_.pads[static_cast<std::size_t>(port)];
}

void Snes::latchJoypads() noexcept {
  // The strobe pulse: every port's pad loads its sixteen bits and the clock count
  // starts over. A port with nothing in it latches nothing.
  for (std::size_t p = 0; p < 2; ++p) {
    state_.joyLatch[p] = state_.pads[p] ? state_.pads[p]->bits() : std::uint16_t{0u};
    state_.joyClocks[p] = 0u;
  }
}

std::uint8_t Snes::clockJoypad(std::size_t port) noexcept {
  const std::optional<Joypad>& pad = state_.pads[port];
  if (!pad) return 0u;  // an empty port's data line stays high, which reads as zero
  // While the strobe is held high the pad keeps reloading its register, so every
  // clock returns the first bit — B — and the count never advances.
  if (state_.joyStrobe) return static_cast<std::uint8_t>((pad->bits() >> 15) & 1u);
  if (state_.joyClocks[port] >= 16u) return 1u;  // past the sixteenth bit a pad returns its padding, low
  const std::uint8_t bit =
      static_cast<std::uint8_t>((state_.joyLatch[port] >> (15u - state_.joyClocks[port])) & 1u);
  ++state_.joyClocks[port];
  return bit;
}

void Snes::finishAutoJoypadRead() noexcept {
  // Sixteen clocks per port, the first bit landing highest, into the port's pair
  // of registers: the low byte at $4218/$421A, the high at $4219/$421B. The
  // second data line of each port — a multitap's — carries nothing here, so
  // $421C-$421F stay zero.
  for (std::size_t p = 0; p < 2; ++p) {
    std::uint16_t word = 0;
    for (std::size_t i = 0; i < 16; ++i) {
      word = static_cast<std::uint16_t>((word << 1) | clockJoypad(p));
    }
    state_.joy[p * 2u] = static_cast<std::uint8_t>(word & 0xFFu);
    state_.joy[p * 2u + 1u] = static_cast<std::uint8_t>(word >> 8);
  }
}

std::uint8_t Snes::readJoypadPort(std::uint16_t offset) {
  if (offset == 0x4016) {
    // JOYSER0: bit 0 is port 1's data line, bit 1 its second line (nothing is on
    // it); the rest is open bus.
    return latch(static_cast<std::uint8_t>((state_.mdr & 0xFCu) | clockJoypad(0)));
  }
  // JOYSER1: bit 0 is port 2's data line, bit 1 its second line; bits 4-2 are
  // wired low and read as ones; the rest is open bus.
  return latch(static_cast<std::uint8_t>((state_.mdr & 0xE0u) | 0x1Cu | clockJoypad(1)));
}

void Snes::writeJoypadStrobe(std::uint8_t value) noexcept {
  // Bit 0 is the strobe line to both ports. Its fall latches the pads; while it
  // is high the pads reload continuously, which clockJoypad reads as B every time.
  const bool high = (value & 1u) != 0u;
  if (state_.joyStrobe && !high) latchJoypads();
  state_.joyStrobe = high;
}

}  // namespace snaggletooth
