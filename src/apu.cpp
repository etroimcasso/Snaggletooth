#include "snaggletooth/apu/apu.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace snaggletooth {

namespace {

// CONTROL ($F1) bit 7 maps the boot-ROM image over the $FFC0 window for SPC700
// reads (see Apu::mapIplRom); writes to the window always reach the RAM beneath.
constexpr std::uint8_t kControlIplRom = 0x80;

// ENDX ($7C) is the one DSP register whose write clears the register rather than
// storing the value: any write acknowledges (clears) all voice end flags.
constexpr std::uint8_t kDspEndx = 0x7C;

// FLG ($6C) resets to $E0: soft reset set, amplifier muted, echo writes disabled,
// noise stopped. A driver clears it once it has set up the DSP.
constexpr std::uint8_t kDspFlg = 0x6C;

// KON ($4C) takes effect on the write: the value written becomes the internal
// key-on the next poll acts on, beside the register byte the write also stores.
constexpr std::uint8_t kDspKon = 0x4C;

// The seeded post-IPL power-on state: what the machine looks like once the boot
// ROM has cleared zero page, posted the ready bytes, and handed control over.
ApuState powerOnState() {
  ApuState s{};
  s.cpu.sp = 0xEF;  // the stack lives in page $01; the first push lands at $01EF
  s.test = 0x0A;
  s.control = 0xB0;               // bit 7 maps the $FFC0 window when an image is mapped
  s.outputPorts = {0xAA, 0xBB, 0x00, 0x00};  // the ready bytes a host polls for
  s.dsp[kDspFlg] = 0xE0;          // FLG's documented reset value
  for (TimerState& t : s.timers) {
    t.target = 0x00;
    t.stage2 = 0x00;
    t.stage3 = 0x0F;  // TnOUT reads $F on power on
  }
  return s;
}

}  // namespace

Apu::Apu() : state_(powerOnState()) { syncCpuAndSlot(); }

Apu::Apu(ApuState state) : state_(std::move(state)) { syncCpuAndSlot(); }

void Apu::restore(ApuState state) {
  state_ = std::move(state);
  syncCpuAndSlot();
  frames_.clear();  // pending output belongs to the machine that produced it
}

void Apu::syncCpuAndSlot() {
  cpu_.restore(state_.cpu);
  // The DSP's sample slot rides the master counter: after a cycle that leaves the
  // counter at D, the DSP has run slot (D-1) and its cursor sits at D mod 32. A
  // restored or seeded state re-establishes that lockstep so the machine drives
  // the schedule without passing the phase every cycle.
  state_.dsp.slotCursor = static_cast<std::uint8_t>(state_.divider & 31u);
}

void Apu::writePort(std::uint8_t index, std::uint8_t value) {
  if (index < 4) state_.inputPorts[index] = value;
}

std::uint8_t Apu::readPort(std::uint8_t index) const noexcept {
  return index < 4 ? state_.outputPorts[index] : 0;
}

void Apu::loadRam(std::uint16_t address, std::span<const std::uint8_t> bytes) noexcept {
  for (std::uint8_t b : bytes) {
    state_.ram[address] = b;
    address = static_cast<std::uint16_t>(address + 1);
  }
}

void Apu::setPc(std::uint16_t pc) {
  state_.cpu.pc = pc;
  cpu_.restore(state_.cpu);
}

void Apu::mapIplRom(std::span<const std::uint8_t, kIplWindowBytes> image) {
  std::array<std::uint8_t, kIplWindowBytes> copy{};
  std::copy(image.begin(), image.end(), copy.begin());
  iplImage_ = copy;
}

void Apu::machineCycle() {
  // The counter wraps at 65536, a multiple of 128, 32 and 16, so every phase
  // below survives the wrap unbroken.
  ++state_.divider;
  const std::uint16_t phase = state_.divider;

  // The clocked events, on their documented slots of the DSP's 32-cycle sample
  // frame: T0 and T1 tick on the first slot of every fourth frame, T2 on the
  // first slot of every half-frame, and the sample lands on the frame boundary.
  if (phase % 128u == 1u) {
    tickTimer(0);
    tickTimer(1);
  }
  if (phase % 16u == 1u) tickTimer(2);
  sampleFrame();

  // The CPU's access closes the cycle. A halted core reaches nothing here, and
  // the cycle still passes for everything above.
  Bus bus{*this};
  cpu_.stepCycle(bus);
}

void Apu::tickTimer(std::size_t index) {
  const std::uint8_t enable = static_cast<std::uint8_t>(1u << index);
  if (!(state_.control & enable)) return;  // stage 2 only counts while enabled
  TimerState& t = state_.timers[index];
  t.stage2 = static_cast<std::uint8_t>(t.stage2 + 1);  // 0-255 wraparound
  // Post-increment comparator: a target of 0 matches only after 256 counts.
  if (t.stage2 == t.target) {
    t.stage3 = static_cast<std::uint8_t>((t.stage3 + 1) & 0x0Fu);  // 4-bit output
    t.stage2 = 0;
  }
}

void Apu::sampleFrame() {
  // The DSP runs one of a sample's 32 slots this cycle; the wrap slot delivers the
  // finished stereo frame, which joins the queue. The span is writable so the echo
  // unit can write its feedback straight into APU RAM (its own bus access — no
  // overlay), the delay line the echo depends on.
  const std::span<std::uint8_t, 65536> ram{state_.ram};
  const SlotResult sample = stepDspCycle(state_.dsp, ram);
  if (sample.delivered) frames_.push_back(sample.frame);
}

std::uint32_t Apu::step() {
  std::uint32_t cycles = 0;
  if (cpu_.state().run != RunState::Running) {
    // A halted core prices two idle cycles, the same two the core itself charges
    // for a step it cannot take.
    machineCycle();
    machineCycle();
    cycles = 2;
  } else {
    do {
      machineCycle();
      ++cycles;
    } while (!cpu_.atInstructionBoundary());
  }
  state_.cpu = cpu_.state();  // the snapshot is coherent wherever the machine stops
  return cycles;
}

std::uint64_t Apu::run(std::uint64_t budget) {
  for (std::uint64_t n = 0; n < budget; ++n) machineCycle();
  state_.cpu = cpu_.state();
  return budget;
}

std::vector<StereoFrame> Apu::takeFrames() {
  std::vector<StereoFrame> drained = std::move(frames_);
  frames_.clear();
  return drained;
}

void Apu::writeDspRegister(std::uint8_t reg, std::uint8_t value) {
  if (reg == kDspEndx) {
    state_.dsp[kDspEndx] = 0;  // ENDX: any write acknowledges all end flags
    return;
  }
  state_.dsp[reg] = value;
  // A KON write also arms the internal key-on the poll consumes. The value
  // replaces whatever was pending, so of two writes between polls only the
  // second one keys anything on.
  if (reg == kDspKon) state_.dsp.internalKon = value;
}

void Apu::reset() {
  ApuState fresh = powerOnState();
  // The master counter cannot be reset — the timer phase and the sample phase
  // both ride it — the targets are retained, and RAM above zero page keeps its
  // contents; the timer outputs go to 0 rather than the power-on $F. Everything
  // else returns to its documented reset value via powerOnState.
  fresh.divider = state_.divider;
  for (std::size_t i = 0; i < fresh.timers.size(); ++i) {
    fresh.timers[i].target = state_.timers[i].target;
    fresh.timers[i].stage3 = 0x00;
  }
  for (std::size_t addr = 0x0100; addr < fresh.ram.size(); ++addr)
    fresh.ram[addr] = state_.ram[addr];
  state_ = std::move(fresh);
  syncCpuAndSlot();
  frames_.clear();  // a reset abandons any un-drained output
}

std::uint8_t Apu::busRead(std::uint16_t address) {
  if (address >= 0x00F0u && address <= 0x00FFu)
    return readRegister(static_cast<std::uint8_t>(address));
  // The boot-ROM window: with an image mapped and CONTROL bit 7 set, an SPC700 read
  // in $FFC0-$FFFF returns the image, not the RAM beneath. Writes are unconditional
  // (busWrite always hits RAM), so a driver can scratch the window and still read the
  // mapped bytes back. With no image mapped or the bit clear, the address is RAM.
  if (iplImage_ && address >= kIplWindowBase && (state_.control & kControlIplRom) != 0u)
    return (*iplImage_)[address - kIplWindowBase];
  return state_.ram[address];
}

void Apu::busWrite(std::uint16_t address, std::uint8_t value) {
  if (address >= 0x00F0u && address <= 0x00FFu) {
    writeRegister(static_cast<std::uint8_t>(address), value);
    return;
  }
  state_.ram[address] = value;
}

std::uint8_t Apu::readRegister(std::uint8_t reg) {
  switch (reg) {
    case 0xF2: return state_.dspAddr;                       // DSPADDR reads back the latched address
    case 0xF3: return state_.dsp[state_.dspAddr & 0x7Fu];   // DSPDATA masks the address with $7F
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:             // input ports: what the host last wrote
      return state_.inputPorts[reg - 0xF4u];
    case 0xF8: case 0xF9:                                   // scratch registers behave as RAM
      return state_.ram[reg];
    case 0xFD: case 0xFE: case 0xFF: {                      // TnOUT: return the 4-bit stage-3 counter, then clear it
      TimerState& t = state_.timers[reg - 0xFDu];
      const std::uint8_t out = static_cast<std::uint8_t>(t.stage3 & 0x0Fu);
      t.stage3 = 0;
      return out;
    }
    // TEST, CONTROL and TnTARGET are write-only and read back 0.
    default:
      return 0;
  }
}

void Apu::writeRegister(std::uint8_t reg, std::uint8_t value) {
  // Every overlay write also lands in the underlying RAM byte (the register is an
  // overlay on RAM), then applies the register's own effect. TEST while the P
  // flag is set is the one write that has no effect at all.
  switch (reg) {
    case 0xF0:  // TEST
      if (cpu_.state().psw & kFlagP) return;
      state_.ram[reg] = value;
      state_.test = value;
      return;
    case 0xF1: {  // CONTROL
      const std::uint8_t previous = state_.control;
      state_.ram[reg] = value;
      state_.control = value;
      // Bit 4 clears input ports 0/1, bit 5 clears input ports 2/3 — a one-time
      // zeroing on every write with the bit set, never a 0->1 transition.
      if (value & 0x10u) { state_.inputPorts[0] = 0; state_.inputPorts[1] = 0; }
      if (value & 0x20u) { state_.inputPorts[2] = 0; state_.inputPorts[3] = 0; }
      // A timer enable going 0->1 resets that timer's stage-2 and stage-3
      // counters; the shared stage-1 divider is left running.
      for (std::size_t i = 0; i < state_.timers.size(); ++i) {
        const std::uint8_t enable = static_cast<std::uint8_t>(1u << i);
        if ((value & enable) && !(previous & enable)) {
          state_.timers[i].stage2 = 0;
          state_.timers[i].stage3 = 0;
        }
      }
      return;
    }
    case 0xF2:  // DSPADDR
      state_.ram[reg] = value;
      state_.dspAddr = value;
      return;
    case 0xF3:  // DSPDATA: writes beyond address $7F are ignored
      state_.ram[reg] = value;
      if (state_.dspAddr <= 0x7Fu) writeDspRegister(state_.dspAddr, value);
      return;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:  // output ports: SPC700 -> host
      state_.ram[reg] = value;
      state_.outputPorts[reg - 0xF4u] = value;
      return;
    case 0xFA: case 0xFB: case 0xFC:  // TnTARGET
      state_.ram[reg] = value;
      state_.timers[reg - 0xFAu].target = value;
      return;
    // $F8/$F9 are plain RAM, and a write to a TnOUT register ($FD-$FF) lands in
    // RAM but drives nothing. Both need only the RAM write above.
    default:
      state_.ram[reg] = value;
      return;
  }
}

}  // namespace snaggletooth
