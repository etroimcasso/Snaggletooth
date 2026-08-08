#include "snaggletooth/apu/apu.h"

#include <cstddef>
#include <utility>

namespace snaggletooth {

namespace {

// ENDX ($7C) is the one DSP register whose write clears the register rather than
// storing the value: any write acknowledges (clears) all voice end flags.
constexpr std::uint8_t kDspEndx = 0x7C;

// The seeded post-IPL power-on state: what the machine looks like once the boot
// ROM has cleared zero page, posted the ready bytes, and handed control over.
ApuState powerOnState() {
  ApuState s{};
  s.cpu.sp = 0xEF;  // the stack lives in page $01; the first push lands at $01EF
  s.test = 0x0A;
  s.control = 0xB0;               // bit 7 (IPL mapping) is carried but gates nothing
  s.outputPorts = {0xAA, 0xBB, 0x00, 0x00};  // the ready bytes a host polls for
  for (TimerState& t : s.timers) {
    t.target = 0x00;
    t.stage2 = 0x00;
    t.stage3 = 0x0F;  // TnOUT reads $F on power on
  }
  return s;
}

}  // namespace

Apu::Apu() : state_(powerOnState()) { cpu_.restore(state_.cpu); }

Apu::Apu(ApuState state) : state_(std::move(state)) { cpu_.restore(state_.cpu); }

void Apu::restore(ApuState state) {
  state_ = std::move(state);
  cpu_.restore(state_.cpu);
  frames_.clear();  // pending output belongs to the machine that produced it
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

std::uint32_t Apu::step() {
  Bus bus{*this};
  const std::uint32_t cycles = cpu_.step(bus);
  state_.cpu = cpu_.state();  // keep the snapshot coherent with the live CPU
  // The timers and the DSP sample divider advance after the instruction, so a
  // read the CPU issues mid-instruction observes their pre-step state
  // (instruction-granular).
  advanceTimers(cycles);
  advanceDsp(cycles);
  return cycles;
}

std::uint64_t Apu::run(std::uint64_t budget) {
  std::uint64_t consumed = 0;
  while (consumed < budget) consumed += step();  // every step delivers >= 2 cycles
  return consumed;
}

void Apu::advanceTimers(std::uint32_t cycles) {
  // The divider is a free-running machine-cycle counter. Its wrap at 65536 is a
  // multiple of both 128 and 16, so the tick phase is preserved across the wrap;
  // a single step advances it by far less than a full period.
  const std::uint32_t before = state_.divider;
  const std::uint32_t after = before + cycles;
  state_.divider = static_cast<std::uint16_t>(after);

  // A stage-1 tick is a crossing of the timer's base period. T0 and T1 tick on
  // the same 128-cycle boundaries; T2 ticks on every 16.
  const std::uint32_t ticks128 = (after / 128u) - (before / 128u);
  const std::uint32_t ticks16 = (after / 16u) - (before / 16u);

  auto tick = [this](std::size_t index, std::uint32_t stage1Ticks) {
    const std::uint8_t enable = static_cast<std::uint8_t>(1u << index);
    if (!(state_.control & enable)) return;  // stage 2 only counts while enabled
    TimerState& t = state_.timers[index];
    for (std::uint32_t n = 0; n < stage1Ticks; ++n) {
      t.stage2 = static_cast<std::uint8_t>(t.stage2 + 1);  // 0-255 wraparound
      // Post-increment comparator: a target of 0 matches only after 256 counts.
      if (t.stage2 == t.target) {
        t.stage3 = static_cast<std::uint8_t>((t.stage3 + 1) & 0x0Fu);  // 4-bit output
        t.stage2 = 0;
      }
    }
  };
  tick(0, ticks128);
  tick(1, ticks128);
  tick(2, ticks16);
}

void Apu::advanceDsp(std::uint32_t cycles) {
  // One DSP sample every 32 machine cycles (32 kHz). The divider free-runs and
  // wraps at 65536 — a multiple of 32, so the sample phase survives the wrap. It
  // is retained across reset().
  const std::uint32_t before = state_.dsp.sampleDivider;
  const std::uint32_t after = before + cycles;
  state_.dsp.sampleDivider = static_cast<std::uint16_t>(after);

  // Generate a stereo frame for each 32-cycle boundary the divider crossed.
  const std::uint32_t samples = (after / 32u) - (before / 32u);
  const std::span<const std::uint8_t, 65536> ram{state_.ram};
  for (std::uint32_t n = 0; n < samples; ++n)
    frames_.push_back(stepDspSample(state_.dsp, ram));
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
}

void Apu::reset() {
  ApuState fresh = powerOnState();
  // The divider cannot be reset, the targets are retained, and RAM above zero
  // page keeps its contents; the timer outputs go to 0 rather than the power-on
  // $F. Everything else returns to its documented reset value via powerOnState.
  fresh.divider = state_.divider;
  fresh.dsp.sampleDivider = state_.dsp.sampleDivider;  // the sample clock free-runs too
  for (std::size_t i = 0; i < fresh.timers.size(); ++i) {
    fresh.timers[i].target = state_.timers[i].target;
    fresh.timers[i].stage3 = 0x00;
  }
  for (std::size_t addr = 0x0100; addr < fresh.ram.size(); ++addr)
    fresh.ram[addr] = state_.ram[addr];
  state_ = std::move(fresh);
  cpu_.restore(state_.cpu);
  frames_.clear();  // a reset abandons any un-drained output
}

std::uint8_t Apu::busRead(std::uint16_t address) {
  if (address >= 0x00F0u && address <= 0x00FFu)
    return readRegister(static_cast<std::uint8_t>(address));
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
