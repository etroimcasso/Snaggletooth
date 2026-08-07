#include "snaggletooth/apu/apu.h"

#include <utility>

namespace snaggletooth {

namespace {

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
  return cycles;
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
    // TnOUT ($FD-$FF) reads resolve to 0 until the stage-3 counters go live in
    // the next unit; TEST, CONTROL and TnTARGET are write-only and read back 0.
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
    case 0xF1:  // CONTROL
      state_.ram[reg] = value;
      state_.control = value;
      // Bit 4 clears input ports 0/1, bit 5 clears input ports 2/3 — a one-time
      // zeroing on every write with the bit set, never a 0->1 transition. The
      // enable bits and their stage resets go live with the timers next unit.
      if (value & 0x10u) { state_.inputPorts[0] = 0; state_.inputPorts[1] = 0; }
      if (value & 0x20u) { state_.inputPorts[2] = 0; state_.inputPorts[3] = 0; }
      return;
    case 0xF2:  // DSPADDR
      state_.ram[reg] = value;
      state_.dspAddr = value;
      return;
    case 0xF3:  // DSPDATA: writes beyond address $7F are ignored
      state_.ram[reg] = value;
      if (state_.dspAddr <= 0x7Fu) state_.dsp[state_.dspAddr] = value;
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
