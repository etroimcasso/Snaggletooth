#include "snaggletooth/snes/snes.h"

#include <algorithm>
#include <array>
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

// The scanline structure. A line is 1364 master cycles and carries 340 dots: every
// dot is four cycles except 323 and 327, which are six. Two lines a frame are not
// 1364 — NTSC's line 240 on an odd field is four cycles short, and PAL's line 311 on
// an odd interlaced field four long — and an interlaced frame of even parity runs one
// line past the 262 NTSC and 312 PAL a frame otherwise has, which is where dot 340
// exists at all.
constexpr std::uint16_t kLineMaster = 1364u;
constexpr std::uint16_t kShortLineMaster = 1360u;
constexpr std::uint16_t kLongLineMaster = 1368u;
constexpr std::uint16_t kShortLineV = 240u;
constexpr std::uint16_t kLongLineV = 311u;
constexpr std::uint16_t kNtscLines = 262u;
constexpr std::uint16_t kPalLines = 312u;

// Where the dot map's two long dots begin and end, in master cycles into the line.
constexpr std::uint16_t kFirstLongDot = 1292u;
constexpr std::uint16_t kAfterFirstLongDot = 1298u;
constexpr std::uint16_t kSecondLongDot = 1310u;
constexpr std::uint16_t kAfterSecondLongDot = 1316u;

// The per-line events, as master cycles into the line they belong to. The blank flag
// is raised at H = 274 and lowered again at H = 1 of the line that follows, on every
// line of the frame; the frame's parity toggles at H = 1 of its first line; vertical
// blank's own line raises the NMI flag at H = 0.5 and hands the sprite table back at
// H = 10; and each visible line delivers its HDMA at dot 278.
constexpr std::uint16_t kHblankClear = 4u;
constexpr std::uint16_t kHblankSet = 1096u;
constexpr std::uint16_t kFieldToggle = 4u;
constexpr std::uint16_t kNmiFlagOffset = 2u;
constexpr std::uint16_t kOamReloadOffset = 40u;
constexpr std::uint16_t kHdmaDeliver = 1112u;

// The picture the chip draws: dots 22 to 277 of every line the frame's own vertical
// blank leaves below it, the first line of a frame drawing nothing. Every one of
// those dots is four master cycles wide, the two long ones lying well past them.
constexpr std::uint16_t kFirstPictureDot = 22u;
constexpr std::uint16_t kLastPictureDot = 277u;
static_assert(kPictureWidth == kLastPictureDot - kFirstPictureDot + 1u,
              "the picture is as wide as the dots it is drawn from");
constexpr std::uint16_t kTallestPicture = kOverscanVblankStartLine - 1u;
constexpr std::size_t kPixelBytes = 4u;
constexpr std::size_t kRasterBytes = kHiresWidth * kPixelBytes * kTallestPicture;

// The H/V timer's trigger points. With an H position to compare against, the flag is
// raised 14 master cycles past four times it; with none, 1374 master cycles after the
// previous line began — which is ten into a line following a normal one, fourteen
// after the short one and six after the long one.
constexpr std::uint16_t kTimerHOrigin = 14u;
constexpr std::uint16_t kTimerLineSpan = 1374u;
// The dot the timer raises nothing on, on the short line and on a frame's last line:
// anomie-timing.txt 121-123 measures both, and documents no mechanism for either.
constexpr std::uint16_t kTimerQuietDot = 153u;

// The memory refresh: the CPU is held off the bus for forty master cycles once a
// line, at the point on an eight-cycle grid nearest 536 into the line, spent a fast
// cycle at a time so a run can stop inside one.
constexpr std::uint16_t kRefreshMaster = 40u;
constexpr std::uint16_t kRefreshTarget = 536u;
constexpr std::uint32_t kRefreshStep = 6u;

// The multiply/divide unit is clocked by the CPU, so its documented latencies are
// counted in CPU cycles — the same wait no matter the memory speed. Multiply is
// ready after 8, divide after 16.
constexpr std::uint8_t kMultiplyClocks = 8u;
constexpr std::uint8_t kDivideClocks = 16u;

// The auto-joypad read. It begins on vertical blank's first line: at H = 74.5 the
// first time the machine performs one, and thereafter at the first point on a
// 256-cycle grid carried from the previous read's start that lies at or past
// H = 32.5. The strobe pulse takes 128 cycles, then each of the sixteen bits a
// further 256, so the busy flag holds for 4224 cycles and the sixteenth bit lands
// as it clears.
constexpr std::uint16_t kAutoJoyFirstStart = 298u;   // H = 74.5, in master cycles into the line
constexpr std::uint16_t kAutoJoyEarliest = 130u;     // H = 32.5
constexpr std::uint16_t kAutoJoyGrid = 256u;
constexpr std::uint16_t kAutoJoyStrobe = 128u;
constexpr std::uint16_t kAutoJoyBit = 256u;
constexpr std::uint8_t kAutoJoyBits = 16u;
static_assert(kAutoJoyStrobe + kAutoJoyBits * kAutoJoyBit == 4224u,
              "the auto-read's busy window is the documented 4224 master cycles");

// The largest save a cartridge can address through any window.
constexpr std::size_t kMaxSaveRamBytes = 128u * 1024u;

}  // namespace

Snes::Snes(SnesConfig config)
    : apu_(&state_.apu),  // the audio machine runs in the snapshot's own storage, seeded at power-on
      rom_(config.rom.begin(), config.rom.end()),
      region_(config.region) {
  map_ = config.map.value_or(detectCartridgeMap(rom_));
  const std::size_t save = config.saveRamBytes.value_or(declaredSaveRamBytes(rom_));
  state_.sram.assign(save > kMaxSaveRamBytes ? kMaxSaveRamBytes : save, 0u);
  const ApuRatio ratio = region_ == Region::Pal ? kPalApu : kNtscApu;
  apuNum_ = ratio.num;
  apuDen_ = ratio.den;
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

Snes::Snes(Snes&& moved) noexcept
    : cpu_(std::move(moved.cpu_)),
      state_(std::move(moved.state_)),
      apu_(std::move(moved.apu_), &state_.apu),  // the audio machine follows its state here
      rom_(std::move(moved.rom_)),
      region_(moved.region_),
      map_(moved.map_),
      apuNum_(moved.apuNum_),
      apuDen_(moved.apuDen_),
      lastCost_(moved.lastCost_),
      videoAdvanced_(moved.videoAdvanced_),
      timerHPoint_(moved.timerHPoint_),
      timerHPointOnVLine_(moved.timerHPointOnVLine_),
      timerZeroOnVLine_(moved.timerZeroOnVLine_),
      observer_(moved.observer_),
      portLanding_(moved.portLanding_),
      frameObserver_(moved.frameObserver_),
      raster_(std::move(moved.raster_)),
      derived_(std::move(moved.derived_)),
      frameFinished_(moved.frameFinished_),
      frameWide_(moved.frameWide_),
      framePictureLines_(moved.framePictureLines_),
      frameWidth_(moved.frameWidth_),
      saveObserver_(moved.saveObserver_),
      saveChanged_(moved.saveChanged_),
      saveFinished_(moved.saveFinished_) {
  moved.observer_ = nullptr;
  moved.frameObserver_ = nullptr;
  moved.saveObserver_ = nullptr;
}

void Snes::setFrameObserver(FrameObserver* observer) noexcept {
  frameObserver_ = observer;
}

void Snes::restore(const SnesState& state) {
  state_ = state;
  // The caller replaced the save window along with everything else, which changes
  // it as surely as a store does — and a save left disagreeing with the machine is
  // worse than one written again.
  saveChanged_ = true;
  load();
}

void Snes::load() {
  // Every register the picture path works its answers out from has just been
  // replaced, so none of those answers stands: the first dot drawn after this works
  // them out from what the caller supplied.
  derived_.dropAll();
  cpu_.restore(state_.cpu);
  apu_.reload();  // its state is state_.apu, written in place by the restore or the seeding above
  // The NMI pin's remembered level is not part of the snapshot, so re-derive it from
  // the flags and enables and sync it WITHOUT minting an edge: the pending latch, if
  // one was in flight, rides the snapshot on its own. Driving it as a fresh assertion
  // would double a request the restore already carries. The IRQ line is level-only, so
  // setting it plainly is correct.
  cpu_.syncNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
}

void Snes::sync() {
  // The audio machine's state is already in state_.apu; only the live CPU's
  // register set has a copy to write back.
  state_.cpu = cpu_.state();
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

void Snes::closeCycle() {
  // What every cycle ends with, whatever it was for: the timer's crossing settles
  // under the mode the cycle leaves behind, the interrupt lines take their levels from
  // the flags and enables as they now stand — so a register write this cycle is
  // settled for the next fetch to sample — and the master counter advances, paying the
  // audio machine the share of it that its own crystal owes.
  settleTimer();
  cpu_.setNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
  state_.master += lastCost_;
  state_.apuPhase += lastCost_ * apuNum_;
  const std::uint64_t apuCycles = state_.apuPhase / apuDen_;
  state_.apuPhase %= apuDen_;
  if (apuCycles != 0) apu_.run(apuCycles);
}

void Snes::refreshCycle() {
  // The memory refresh, with the CPU held off the bus. Time passes as it does in any
  // cycle — the beam moves and the audio machine is paced — but no access is made and
  // the observer is told nothing: a pause is not a cycle the CPU spent, any more than
  // a transfer's overhead is. It is spent a fast cycle at a time, so a run may stop
  // inside one and carry the rest of it in the state.
  lastCost_ = state_.refreshLeft < kRefreshStep ? state_.refreshLeft : kRefreshStep;
  state_.refreshLeft = static_cast<std::uint8_t>(state_.refreshLeft - lastCost_);
  tickVideo(lastCost_);
  closeCycle();
}

void Snes::machineCycle() {
  if (state_.refreshLeft != 0u) {
    refreshCycle();
    return;
  }

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

  closeCycle();
}

std::uint32_t Snes::step() {
  const std::uint64_t before = state_.master;
  // A refresh the last call left part-way through is spent first: it is not an
  // instruction, and the instruction this call owes is the one after it.
  while (state_.refreshLeft != 0u) machineCycle();
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
  const std::uint8_t busBefore = state_.mdr;  // the byte the bus held before this write
  tickVideo(lastCost_);  // tick-first, so a write lands after the event it shares the cycle with
  videoAdvanced_ = true;
  routeWrite(address, value);
  redrawInidispEarly(address, busBefore);
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
    if (offset >= 0x2100 && offset <= 0x213F) {
      // A byte the PPU drove goes onto the data bus; a register with nothing to
      // say leaves the bus as it was, which is what the read returns.
      const std::optional<std::uint8_t> v = Ppu{state_.ppu, derived_}.read(offset, ppuInputs());
      return v.has_value() ? latch(*v) : state_.mdr;
    }
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
      portLanding_ = Ppu{state_.ppu, derived_}.write(offset, value, ppuInputs());
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
    // Every store into the save window arrives here, which is what lets the machine
    // say a frame changed it without comparing anything (SaveObserver, `snes.h`).
    saveChanged_ = true;
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
  // One line a frame is not 1364: NTSC drops a dot from line 240 on an odd field
  // unless the frame is interlaced, and PAL adds one to line 311 on an odd field when
  // it is. Each is decided by the state as its own line runs.
  const bool oddField = state_.field == 1u;
  if (region_ == Region::Ntsc) {
    if (oddField && state_.vpos == kShortLineV && !state_.ppu.interlace()) {
      return kShortLineMaster;
    }
    return kLineMaster;
  }
  if (oddField && state_.vpos == kLongLineV && state_.ppu.interlace()) return kLongLineMaster;
  return kLineMaster;
}

std::uint16_t Snes::frameLines() const noexcept {
  // An interlaced frame of even parity carries one line more than the frame otherwise
  // has, and that line is vertical blank's like the ones before it.
  const std::uint16_t base = region_ == Region::Ntsc ? kNtscLines : kPalLines;
  return state_.ppu.interlace() && state_.field == 0u
             ? static_cast<std::uint16_t>(base + 1u)
             : base;
}

namespace {

// Whether a position in the line falls in its dot's second half: the last two
// master cycles of a four-cycle dot, the last three of one of the two six-cycle
// dots. The short line keeps 340 even dots and has no six-cycle ones.
[[nodiscard]] bool lateHalfOf(std::uint16_t hpos, bool shortLine) noexcept {
  if (!shortLine) {
    if (hpos >= kFirstLongDot && hpos < kAfterFirstLongDot) {
      return static_cast<unsigned>(hpos - kFirstLongDot) >= 3u;
    }
    if (hpos >= kSecondLongDot && hpos < kAfterSecondLongDot) {
      return static_cast<unsigned>(hpos - kSecondLongDot) >= 3u;
    }
    if (hpos >= kAfterFirstLongDot) {
      // Past a long dot the four-cycle grid is offset by the two cycles it added.
      const unsigned offset = hpos >= kAfterSecondLongDot ? 4u : 2u;
      return ((static_cast<unsigned>(hpos) - offset) & 3u) >= 2u;
    }
  }
  return (hpos & 3u) >= 2u;
}

}  // namespace

std::uint16_t Snes::hdot() const noexcept { return hdotAt(state_.hpos); }

std::uint16_t Snes::hdotAt(std::uint16_t h) const noexcept {
  // The dot the position is on. Dots 323 and 327 are six master cycles wide and every
  // other dot is four, so past dot 322 the count falls behind a plain quarter of the
  // position and dot 340 is reached only on a line that runs 1368. The short line
  // keeps 340 even dots and none of this applies to it.
  if (lineLength() == kShortLineMaster || h < kFirstLongDot) {
    return static_cast<std::uint16_t>(h >> 2);
  }
  if (h < kAfterFirstLongDot) return 323u;
  if (h < kSecondLongDot) return static_cast<std::uint16_t>(324u + (h - kAfterFirstLongDot) / 4u);
  if (h < kAfterSecondLongDot) return 327u;
  if (h < kLineMaster) return static_cast<std::uint16_t>(328u + (h - kAfterSecondLongDot) / 4u);
  return 340u;
}

bool Snes::inHblank() const noexcept {
  // The flag stands from H = 274 to H = 1 of the next line, so the line's first four
  // master cycles are still the previous line's blank.
  return state_.hpos < kHblankClear || state_.hpos >= kHblankSet;
}

std::uint64_t Snes::nextRefresh(std::uint64_t lineStart) const noexcept {
  // Each pause is the point on the previous pause's eight-cycle grid that lies
  // nearest the middle of its own line, so consecutive normal lines come up 538 and
  // 534 cycles in and a line of another length re-phases the pair.
  const std::uint64_t target = lineStart + kRefreshTarget;
  const std::uint64_t ahead = (target - state_.refreshAt) % 8u;
  return ahead <= 4u ? target - ahead : target + (8u - ahead);
}

bool Snes::timerCrossed() const noexcept {
  switch (static_cast<std::uint8_t>((state_.nmitimen >> 4) & 3u)) {
    case 1: return timerHPoint_;         // H = H on every line
    case 2: return timerZeroOnVLine_;    // V = V, with no H to compare
    case 3: return timerHPointOnVLine_;  // H = H and V = V
    default: return false;               // the timer is off
  }
}

void Snes::settleTimer() noexcept {
  // A crossing is noted as the cycle ticks and the flag is raised at the cycle's end
  // under the mode the cycle leaves behind, so a write that arms the timer in the very
  // cycle its point is crossed is in time and one that disarms it is too.
  if (timerCrossed()) state_.timeup = true;
}

void Snes::crossLine(std::uint64_t lineStart, std::uint64_t from, std::uint64_t to) {
  // The events inside a line, each at its own master offset, for the span the cycle
  // just covered. A point is passed when the span reaches it, and a span never starts
  // before its own line — so an event at offset 0 can never be passed here and belongs
  // to advanceLine, which runs as the line begins. Everything below lies past it.
  const auto passed = [from, to](std::uint64_t point) noexcept {
    return from < point && point <= to;
  };

  if (state_.vpos == 0u && passed(lineStart + kFieldToggle)) {
    state_.field ^= 1u;  // the frame takes its parity as its first line begins
  }

  // Vertical blank's line raises the NMI flag two cycles after the signal itself, and
  // hands the sprite table's address back at dot 10. Neither re-fires: they are dated
  // from the line the blank began on, which comes once a frame.
  if (state_.inVblank && state_.vpos == state_.vblankBeginLine) {
    if (passed(lineStart + kNmiFlagOffset)) {
      // The flag's rise is the edge the CPU latches, here and now while NMIs are
      // enabled: a read of $4210 in this same cycle sees the flag and clears it,
      // and the NMI is taken all the same, at the end of the instruction.
      state_.vblankNmi = true;
      if ((state_.nmitimen & 0x80u) != 0u) cpu_.setNmiLine(true);
    }
    if (passed(lineStart + kOamReloadOffset)) Ppu{state_.ppu, derived_}.beginVblank();
    // The auto-read begins at its own point on this line, once, while $4200 asks
    // for it: the strobe pulse latches every pad and the clocks follow.
    if ((state_.nmitimen & 1u) != 0u && state_.autoJoyStart < lineStart &&
        passed(autoJoypadStart(lineStart))) {
      latchJoypads();
      state_.autoJoyStart = autoJoypadStart(lineStart);
      state_.autoJoyClocked = 0u;
    }
  }

  const std::uint64_t zeroPoint = lineStart + kTimerLineSpan - state_.previousLineMaster;
  const std::uint64_t hPoint = state_.htime != 0u
      ? lineStart + kTimerHOrigin + 4ull * state_.htime
      : zeroPoint;
  const bool onTimerLine = state_.vpos == state_.vtime;
  // Dot 153 raises nothing on the short scanline, nor on the last scanline of a frame.
  // The V-only point is a line's own and keeps its place: the exception is a dot.
  const bool quietDot = state_.htime == kTimerQuietDot &&
                        (lineLength() == kShortLineMaster ||
                         state_.vpos == static_cast<std::uint16_t>(frameLines() - 1u));
  if (!quietDot && passed(hPoint)) {
    timerHPoint_ = true;
    if (onTimerLine) timerHPointOnVLine_ = true;
  }
  if (onTimerLine && passed(zeroPoint)) timerZeroOnVLine_ = true;

  // The refresh: the cycle whose tick reaches the point finishes on its own time, and
  // the pause runs before the next one. The pause holds the core whatever it is doing,
  // so a core waiting on WAI samples the interrupt line it wakes on at the first idle
  // cycle past the pause rather than inside it.
  if (passed(state_.refreshAt)) {
    state_.refreshLeft = static_cast<std::uint8_t>(kRefreshMaster);
  }

  // A latch owed by a fall of the I/O port's top bit lands as the beam reaches its
  // point, capturing the dot and line there rather than at the write. The line is
  // this span's, and the dot is the point's own offset into it.
  if (state_.counterLatchAt != 0u && passed(state_.counterLatchAt)) {
    PpuInputs in = ppuInputs();
    in.hdot = hdotAt(static_cast<std::uint16_t>(state_.counterLatchAt - lineStart));
    Ppu{state_.ppu, derived_}.latchCounters(in);
    state_.counterLatchAt = 0u;
  }
}

void Snes::rangeSpan(std::uint64_t lineStart, std::uint64_t to) noexcept {
  // The pass belongs to a line the chip renders, and it is gathering for the line
  // after it — so the frame's first line, which draws nothing, is the one that
  // finds the picture's first line its sprites.
  if (state_.inVblank) return;

  // The sprites whose dots the span passed: sprite N is examined at the picture's
  // first dot plus twice its place in the walk, so the 128 of them fill the 256
  // dots of the visible span.
  const std::uint64_t last = (to - lineStart) / 4u;
  if (last < kFirstPictureDot) return;
  const std::uint64_t reached = (last - kFirstPictureDot) / 2u + 1u;

  Ppu ppu{state_.ppu, derived_};
  const std::uint16_t line = static_cast<std::uint16_t>(state_.vpos + 1u);
  while (static_cast<std::uint64_t>(state_.ppu.sprites.scanned) < reached &&
         static_cast<unsigned>(state_.ppu.sprites.scanned) < kSprites) {
    const std::uint8_t before = state_.ppu.sprites.scanned;
    ppu.rangeSprite(line);
    if (state_.ppu.sprites.scanned == before) return;  // forced blank walks nowhere
  }
}

void Snes::drawSpan(std::uint64_t lineStart, std::uint64_t from, std::uint64_t to) {
  // The first line of a frame draws nothing, and the lines from this frame's own
  // vertical blank on are past the picture.
  if (state_.vpos == 0u || state_.inVblank) return;

  // The dots the span passed, by the same reckoning the line's events use: a dot is
  // reached when the span covers the master cycle it begins on.
  const std::uint64_t first = (from - lineStart) / 4u + 1u;
  const std::uint64_t last = (to - lineStart) / 4u;
  const std::uint64_t dot = first < kFirstPictureDot ? kFirstPictureDot : first;
  const std::uint64_t stop = last < kLastPictureDot ? last : kLastPictureDot;
  Ppu ppu{state_.ppu, derived_};
  const PpuInputs in = ppuInputs();

  // What the chip carries from one position to the next is decided whether or not
  // anyone is watching, since it is part of the state a snapshot holds.
  if (frameObserver_ == nullptr) {
    for (std::uint64_t at = dot; at <= stop; ++at) {
      ppu.decide(static_cast<std::uint16_t>(at - kFirstPictureDot), in);
    }
    return;
  }

  if (raster_.empty()) {
    raster_.assign(kRasterBytes, 0u);  // black, and opaque: a line nobody drew is black
    for (std::size_t alpha = 3u; alpha < kRasterBytes; alpha += kPixelBytes) raster_[alpha] = 255u;
  }

  const std::size_t line = state_.vpos - 1u;
  for (std::uint64_t at = dot; at <= stop; ++at) {
    const std::uint16_t x = static_cast<std::uint16_t>(at - kFirstPictureDot);
    const Ppu::Dot out = ppu.dot(x, in);
    if (out.hires && !frameWide_) widenFrame(line, x);

    // A frame drawn in half-pixels anywhere is 512 wide throughout, and a
    // position drawn whole fills both of its halves.
    if (frameWide_) {
      std::uint8_t* const pixel =
          raster_.data() + (line * kHiresWidth + static_cast<std::size_t>(x) * 2u) * kPixelBytes;
      std::copy(out.left.begin(), out.left.end(), pixel);
      std::copy(out.right.begin(), out.right.end(), pixel + kPixelBytes);
    } else {
      std::uint8_t* const pixel =
          raster_.data() + (line * kPictureWidth + static_cast<std::size_t>(x)) * kPixelBytes;
      std::copy(out.right.begin(), out.right.end(), pixel);
    }
  }
}

void Snes::widenFrame(std::size_t line, std::uint16_t x) noexcept {
  // Every pixel already drawn this frame — the rows above `line` and the first `x`
  // of `line` itself — moves to twice its column in a row twice as wide, written
  // into both halves. Each row lands at or beyond where it stood, so working from
  // the last pixel back never overwrites one not yet moved.
  const std::uint8_t* const from = raster_.data();
  std::uint8_t* const to = raster_.data();
  for (std::size_t row = line + 1u; row-- > 0u;) {
    const std::size_t count = row == line ? x : kPictureWidth;
    for (std::size_t column = count; column-- > 0u;) {
      const std::size_t source = (row * kPictureWidth + column) * kPixelBytes;
      const std::size_t target = (row * kHiresWidth + column * 2u) * kPixelBytes;
      std::array<std::uint8_t, kPixelBytes> pixel{};
      std::copy(from + source, from + source + kPixelBytes, pixel.begin());
      std::copy(pixel.begin(), pixel.end(), to + target);
      std::copy(pixel.begin(), pixel.end(), to + target + kPixelBytes);
    }
  }
  frameWide_ = true;
}

void Snes::redrawInidispEarly(std::uint32_t address, std::uint8_t busBefore) {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
  if (!systemBank || offset != 0x2100u || state_.inVblank) return;
  // The cycle drew its dots under the register as it stood; redraw only the last of
  // them, under the byte the bus held before the write. Master is still the cycle's
  // start here — closeCycle advances it — so the end is start + this cycle's cost.
  const std::uint16_t endHpos = state_.hpos;
  if (endHpos < lastCost_) return;  // the cycle crossed a line; its last dot is off the picture
  const std::uint64_t beamEnd = state_.master + lastCost_;
  const std::uint64_t lineStart = beamEnd - endHpos;
  const std::uint8_t written = state_.ppu.inidisp;  // the value the write applied, restored after
  // The dot resolves under the same beam position the cycle drew it from, which is
  // the span's start, so the register is the only thing that differs.
  state_.hpos = static_cast<std::uint16_t>(endHpos - lastCost_);
  state_.ppu.inidisp = busBefore;
  drawSpan(lineStart, beamEnd - 4u, beamEnd);
  state_.ppu.inidisp = written;
  state_.hpos = endHpos;
}

void Snes::deliverFrame() {
  if (frameObserver_ == nullptr || raster_.empty()) return;
  const std::size_t bytes =
      static_cast<std::size_t>(framePictureLines_) * frameWidth_ * kPixelBytes;
  frameObserver_->frame(VideoFrame{
      .pixels = std::span<const std::uint8_t>(raster_.data(), bytes),
      .width = frameWidth_,
      .height = framePictureLines_,
      .field = frameField_,
  });
}

void Snes::deliverSave() {
  if (saveObserver_ == nullptr) return;
  saveObserver_->changed(std::span<const std::uint8_t>(state_.sram.data(), state_.sram.size()));
}

void Snes::advanceLine(std::uint64_t lineStart) noexcept {
  state_.vpos = static_cast<std::uint16_t>(state_.vpos + 1u);
  if (state_.vpos >= frameLines()) state_.vpos = 0u;
  state_.hdmaLineFired = false;  // each scanline may trigger its own HDMA delivery
  state_.refreshAt = nextRefresh(lineStart);

  // The horizontal blank the line just ended is where Time draws the sprites the
  // Range pass across that line found — for the line beginning now, which holds
  // H = 0 of it, hblank being lowered a dot later. Range then starts again, on the
  // line after this one, and the mosaic's vertical counter takes the new line.
  Ppu ppu{state_.ppu, derived_};
  ppu.timeSprites(state_.vpos, state_.field);
  ppu.beginRange(static_cast<std::uint16_t>(state_.vpos + 1u));
  ppu.beginLine(state_.vpos);

  if (state_.vpos == 0u) {
    // The picture the beam has just finished is as tall as its own vertical blank
    // left it, and carries the parity it ran under — both read here, before the
    // frame beginning now changes either.
    if (frameObserver_ != nullptr) {
      frameFinished_ = true;
      frameField_ = state_.field;
      frameWidth_ = frameWide_ ? kHiresWidth : kPictureWidth;
      frameWide_ = false;
      framePictureLines_ = state_.vblankBeginLine > 1u
          ? static_cast<std::uint16_t>(state_.vblankBeginLine - 1u)
          : static_cast<std::uint16_t>(state_.ppu.vblankStartLine() - 1u);
    }
    // A frame that changed the save window is owed a report, handed over where the
    // picture is rather than from here: this line advances inside a cycle that
    // cannot throw, and what a host does with a save — writing a file, most
    // plainly — can. The beam reaches this line whether or not anyone is watching
    // the picture, so a save is reported to a host that asked for nothing else.
    if (saveObserver_ != nullptr && saveChanged_) {
      saveChanged_ = false;
      saveFinished_ = true;
    }
    state_.inVblank = false;     // the frame begins in the picture
    state_.vblankNmi = false;    // and the NMI flag clears with it
    // The new frame re-initialises HDMA at line 0, and everything the last frame's
    // channels stood on is the last frame's. The clearing happens here rather than in
    // the initialisation itself because a frame that begins with $420C empty holds no
    // initialisation at all, and a channel the program brings in later must still find
    // a clean slate.
    state_.hdmaInited = false;
    state_.hdmaActive = 0u;
    state_.hdmaEnded = 0u;
    state_.hdmaDoWrite = 0u;
    Ppu{state_.ppu, derived_}.beginFrame();  // the overflow flags belong to the picture just drawn
    return;
  }
  if (state_.inVblank) return;  // begun is begun; a later SETINI change re-fires nothing

  // Vertical blank begins at the line SETINI asks for, and the machine asks again at
  // the start of every line until it has: the bit cleared between the two start lines
  // begins the blank at the next line, and the bit set after it has begun changes
  // nothing.
  if (state_.vpos < kVblankStartLine) return;
  if (state_.vpos < kOverscanVblankStartLine && state_.ppu.overscan()) return;
  state_.inVblank = true;
  state_.vblankBeginLine = state_.vpos;
  state_.hdmaActive = 0u;  // every HDMA channel deactivates for the rest of the frame
}

void Snes::tickVideo(std::uint32_t cost) {
  // The arithmetic unit steps once per CPU cycle regardless of the cycle's master
  // cost; its result lands when the countdown reaches zero.
  if (state_.mathClocks != 0u) {
    --state_.mathClocks;
    if (state_.mathClocks == 0u) commitMath();
  }
  // Advance the beam by the cycle's master cost, one line's share at a time, so every
  // event the span passes is placed at its own master offset. The line's events come
  // first, then the line's end hands the beam to the next one.
  timerHPoint_ = false;
  timerHPointOnVLine_ = false;
  timerZeroOnVLine_ = false;

  std::uint64_t at = state_.master;
  const std::uint64_t end = at + cost;
  std::uint64_t lineStart = at - state_.hpos;
  while (at < end) {
    const std::uint64_t lineEnd = lineStart + lineLength();
    const std::uint64_t stop = end < lineEnd ? end : lineEnd;
    crossLine(lineStart, at, stop);
    // Finding the next line's sprites is the chip's own work and it does it for
    // nobody's benefit, because a program can read what the pass found.
    rangeSpan(lineStart, stop);
    // The picture is resolved a dot at a time, from the registers and the memories
    // as they stand at each one. A machine nobody is watching resolves only what the
    // chip carries from one position to the next.
    drawSpan(lineStart, at, stop);
    at = stop;
    state_.hpos = static_cast<std::uint16_t>(at - lineStart);
    if (at == lineEnd) {
      state_.previousLineMaster = static_cast<std::uint16_t>(lineEnd - lineStart);
      lineStart = lineEnd;
      state_.hpos = 0u;
      advanceLine(lineStart);
    }
  }

  // The auto-read's clocks that fell inside this span land their bits, a read
  // begun inside it included.
  clockAutoJoypad(end);

  // HDMA triggers, each latched so it fires once: the frame's initialisation as the
  // beam passes dot 6 of line 0, and a delivery as it passes dot 278 of every line the
  // picture still owns. Triggering only marks the event pending; it runs on the next
  // machine cycle, so it preempts a general-purpose DMA at a whole byte.
  if (!state_.hdmaInited && state_.vpos == 0u && state_.hpos >= 24u &&
      state_.hdmaen != 0u) {
    state_.hdmaInited = true;
    state_.hdmaRunPending = true;
    state_.hdmaIniting = true;
  }
  if (!state_.hdmaLineFired && !state_.inVblank && state_.hpos >= kHdmaDeliver &&
      (state_.hdmaActive & state_.hdmaen) != 0u) {
    state_.hdmaLineFired = true;
    state_.hdmaRunPending = true;
    state_.hdmaIniting = false;
  }

  // A picture the beam finished this cycle goes to whoever is watching, with
  // everything the cycle owed the machine already done.
  if (frameFinished_) {
    frameFinished_ = false;
    deliverFrame();
  }
  // And the save window that frame changed, in the same place and for the same
  // reason: the machine is between cycles here, so a host may do what it likes
  // with the bytes, including throwing.
  if (saveFinished_) {
    saveFinished_ = false;
    deliverSave();
  }
}

void Snes::commitMath() noexcept {
  // The operands are the pair the job started with, not the registers as they
  // stand now: a program may load the next dividend while this division runs and
  // still read this division's quotient.
  switch (state_.mathOp) {
    case MathOp::Multiply:
      // The quotient register already took the multiplier when the multiply started;
      // now the product lands. (WRMPYA * WRMPYB fits sixteen bits.)
      state_.rdmpy = static_cast<std::uint16_t>(state_.mathLeft * state_.mathRight);
      break;
    case MathOp::Divide:
      if (state_.mathRight == 0u) {
        state_.rddiv = 0xFFFFu;          // dividing by zero yields an all-ones quotient
        state_.rdmpy = state_.mathLeft;  // and the dividend as the remainder
      } else {
        state_.rddiv = static_cast<std::uint16_t>(state_.mathLeft / state_.mathRight);
        state_.rdmpy = static_cast<std::uint16_t>(state_.mathLeft % state_.mathRight);
      }
      break;
    case MathOp::None:
      break;
  }
  state_.mathOp = MathOp::None;
}

PpuInputs Snes::ppuInputs() const noexcept {
  return PpuInputs{
      .hdot = hdot(),
      .vpos = state_.vpos,
      .field = state_.field,
      .vblank = state_.inVblank,
      .hblank = inHblank(),
      .pal = region_ == Region::Pal,
      .extLatch = (state_.wrio & 0x80u) != 0u,
      .lateHalf = lateHalfOf(state_.hpos, lineLength() == kShortLineMaster),
  };
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
      // A read in the very cycle the flag rises receives it set and acknowledges
      // nothing (fullsnes.txt 1781-1784): the cycle has ticked, so the crossing is
      // known here, and the cycle's end raises the flag over this read's clear.
      const std::uint8_t v = static_cast<std::uint8_t>(
          ((state_.timeup || timerCrossed()) ? 0x80u : 0x00u) | (state_.mdr & 0x7Fu));
      state_.timeup = false;  // reading acknowledges the flag
      return latch(v);
    }
    case 0x4212: {  // HVBJOY: vblank (bit 7), hblank (bit 6), auto-joypad busy (bit 0), open bus between
      const std::uint8_t v = static_cast<std::uint8_t>(
          (state_.inVblank ? 0x80u : 0x00u) | (inHblank() ? 0x40u : 0x00u) |
          (state_.mdr & 0x3Eu) | (autoJoypadBusy() ? 0x01u : 0x00u));
      return latch(v);
    }
    case 0x4213: return latch(state_.wrio);  // RDIO: the port's lines, which nothing on the console drives, so as written
    case 0x4214: return latch(static_cast<std::uint8_t>(state_.rddiv & 0xFFu));
    case 0x4215: return latch(static_cast<std::uint8_t>(state_.rddiv >> 8));
    case 0x4216: return latch(static_cast<std::uint8_t>(state_.rdmpy & 0xFFu));
    case 0x4217: return latch(static_cast<std::uint8_t>(state_.rdmpy >> 8));
    default:
      break;
  }
  if (offset >= 0x4218 && offset <= 0x421F) {
    return latch(state_.joy[static_cast<std::size_t>(offset - 0x4218)]);  // the auto-read's registers as they stand, part-shifted while it is busy
  }
  return state_.mdr;  // other CPU-register reads are open bus
}

void Snes::writeCpuReg(std::uint16_t offset, std::uint8_t value) {
  switch (offset) {
    case 0x4200:  // NMITIMEN: NMI enable, H/V IRQ mode, auto-joypad enable
      if (((value >> 4) & 3u) == 0u) state_.timeup = false;  // disabling the IRQ acknowledges it
      state_.nmitimen = value;
      return;
    case 0x4201: {  // WRIO: the I/O port; its top bit falling latches the PPU's counters
      const bool fell = (state_.wrio & 0x80u) != 0u && (value & 0x80u) == 0u;
      state_.wrio = value;
      // The latch through this line lands one dot after the point a $2137 read of
      // the same cycle would (fullsnes.txt 27073-27075). It is owed and
      // captured as the beam passes that point, so it lands from the next cycle's
      // tick, and a snapshot taken between the write and the landing carries it.
      if (fell) state_.counterLatchAt = state_.master + lastCost_ + 4u;
      return;
    }
    case 0x4202: state_.wrmpya = value; return;
    case 0x4203:  // WRMPYB: its write starts the multiply, on the operands as they stand
      state_.wrmpyb = value;
      state_.rddiv = value;  // the shared unit immediately loads the quotient register with the multiplier
      state_.mathLeft = state_.wrmpya;
      state_.mathRight = value;
      state_.mathOp = MathOp::Multiply;
      state_.mathClocks = kMultiplyClocks;
      return;
    case 0x4204:
      state_.wrdiv = static_cast<std::uint16_t>((state_.wrdiv & 0xFF00u) | value);
      return;
    case 0x4205:
      state_.wrdiv = static_cast<std::uint16_t>((state_.wrdiv & 0x00FFu) | (value << 8));
      return;
    case 0x4206:  // WRDIVB: its write starts the divide, on the operands as they stand
      state_.wrdivb = value;
      state_.mathLeft = state_.wrdiv;
      state_.mathRight = value;
      state_.mathOp = MathOp::Divide;
      state_.mathClocks = kDivideClocks;
      return;
    case 0x4207: state_.htime = static_cast<std::uint16_t>((state_.htime & 0x0100u) | value); return;
    case 0x4208: state_.htime = static_cast<std::uint16_t>((state_.htime & 0x00FFu) | ((value & 1u) << 8)); return;
    case 0x4209: state_.vtime = static_cast<std::uint16_t>((state_.vtime & 0x0100u) | value); return;
    case 0x420A: state_.vtime = static_cast<std::uint16_t>((state_.vtime & 0x00FFu) | ((value & 1u) << 8)); return;
    case 0x420B: triggerDma(value); return;             // start a general-purpose DMA on each selected channel
    case 0x420C: enableHdma(value); return;             // enable HDMA on the selected channels
    case 0x420D: state_.memsel = static_cast<std::uint8_t>(value & 1u); return;
    case 0x4211: state_.timeup = false; return;  // TIMEUP: a write acknowledges the flag as a read does
    default: return;  // the other read-only ports ignore writes
  }
}

// ---- the controller ports -----------------------------------------------------

namespace {

// The twelve, in the order the pad shifts them out, which is the order the Button
// values themselves run in — so a button indexes this table directly.
constexpr std::array<Button, kButtonCount> kButtonOrder{
    Button::B,  Button::Y,    Button::Select, Button::Start,
    Button::Up, Button::Down, Button::Left,   Button::Right,
    Button::A,  Button::X,    Button::L,      Button::R,
};

constexpr std::array<std::string_view, kButtonCount> kButtonNames{
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r",
};

constexpr char lowered(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::span<const Button> buttons() noexcept { return kButtonOrder; }

std::string_view buttonName(Button button) noexcept {
  return kButtonNames[static_cast<std::size_t>(button)];
}

std::optional<Button> buttonFromName(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kButtonCount; ++i) {
    const std::string_view candidate = kButtonNames[i];
    if (candidate.size() != name.size()) continue;
    bool same = true;
    for (std::size_t c = 0; c < name.size(); ++c) {
      if (lowered(name[c]) != candidate[c]) {
        same = false;
        break;
      }
    }
    if (same) return kButtonOrder[i];
  }
  return std::nullopt;
}

bool Joypad::holds(Button button) const noexcept {
  switch (button) {
    case Button::B: return b;
    case Button::Y: return y;
    case Button::Select: return select;
    case Button::Start: return start;
    case Button::Up: return up;
    case Button::Down: return down;
    case Button::Left: return left;
    case Button::Right: return right;
    case Button::A: return a;
    case Button::X: return x;
    case Button::L: return l;
    case Button::R: return r;
  }
  return false;
}

void Joypad::hold(Button button, bool pressed) noexcept {
  switch (button) {
    case Button::B: b = pressed; return;
    case Button::Y: y = pressed; return;
    case Button::Select: select = pressed; return;
    case Button::Start: start = pressed; return;
    case Button::Up: up = pressed; return;
    case Button::Down: down = pressed; return;
    case Button::Left: left = pressed; return;
    case Button::Right: right = pressed; return;
    case Button::A: a = pressed; return;
    case Button::X: x = pressed; return;
    case Button::L: l = pressed; return;
    case Button::R: r = pressed; return;
  }
}

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

std::uint64_t Snes::autoJoypadStart(std::uint64_t lineStart) const noexcept {
  // The machine's first read begins at H = 74.5. Every later one begins at the
  // first point on the 256-cycle grid carried from the previous read's start that
  // lies at or past H = 32.5 of the line.
  if (state_.autoJoyStart == 0u) return lineStart + kAutoJoyFirstStart;
  const std::uint64_t earliest = lineStart + kAutoJoyEarliest;
  const std::uint64_t ahead = (earliest - state_.autoJoyStart) % kAutoJoyGrid;
  return ahead == 0u ? earliest : earliest + (kAutoJoyGrid - ahead);
}

bool Snes::autoJoypadBusy() const noexcept {
  return state_.autoJoyStart != 0u && state_.autoJoyClocked < kAutoJoyBits;
}

void Snes::clockAutoJoypad(std::uint64_t now) noexcept {
  if (!autoJoypadBusy()) return;
  // The bits whose clocks have completed by `now`: none through the strobe
  // pulse, then one every 256 cycles.
  const std::uint64_t elapsed = now > state_.autoJoyStart ? now - state_.autoJoyStart : 0u;
  const std::uint64_t landed =
      elapsed <= kAutoJoyStrobe ? 0u : (elapsed - kAutoJoyStrobe) / kAutoJoyBit;
  const auto reached = static_cast<std::uint8_t>(landed < kAutoJoyBits ? landed : kAutoJoyBits);
  // Each clock shifts every port's register up one and puts the bit it read at the
  // bottom, so the first bit read — B — is at bit 15 once all sixteen are in, and a
  // register read before then holds the previous read's bits shifted part-way out
  // above this one's shifted part-way in. The low byte is at $4218/$421A and the
  // high at $4219/$421B; the ports' second data lines — a multitap's — carry
  // nothing here, so $421C-$421F stay zero.
  while (state_.autoJoyClocked < reached) {
    for (std::size_t p = 0; p < 2; ++p) {
      const auto word = static_cast<std::uint16_t>(
          state_.joy[p * 2u] | (static_cast<std::uint16_t>(state_.joy[p * 2u + 1u]) << 8));
      const auto shifted = static_cast<std::uint16_t>((word << 1) | clockJoypad(p));
      state_.joy[p * 2u] = static_cast<std::uint8_t>(shifted & 0xFFu);
      state_.joy[p * 2u + 1u] = static_cast<std::uint8_t>(shifted >> 8);
    }
    ++state_.autoJoyClocked;
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
