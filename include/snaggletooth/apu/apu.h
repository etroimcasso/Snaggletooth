#pragma once

// The APU machine — the SPC700 wrapped in its 64KB RAM and the $F0-$FF register
// overlay.
//
// The machine owns the CPU, the RAM, and the hardware registers the CPU reaches
// through the top sixteen bytes of zero page. Its internal bus satisfies the
// ApuBus concept: reads and writes in $00F0-$00FF hit the register overlay,
// everything else is plain RAM. The host drives the comm ports from the other
// side and loads programs directly into RAM (there is no IPL ROM — the machine
// boots into the seeded post-IPL ready state).
//
// The three timers are seeded to their power-on values and their target and
// output registers obey the overlay, but they do not yet advance: the stage
// counters, the enable-transition resets, and the cycle-budget run surface land
// in the next unit. Stepping here runs one CPU instruction over the overlay.

#include <array>
#include <cstdint>
#include <span>

#include "snaggletooth/apu/spc700.h"

namespace snaggletooth {

// One timer's mutable stage counters. The enable bit lives in the CONTROL
// register (ApuState::control); the shared stage-1 divider is ApuState::divider.
struct TimerState {
  std::uint8_t stage2 = 0;  // 0-255 counter compared against the target
  std::uint8_t stage3 = 0;  // 4-bit output counter; an overlay read of TnOUT clears it
  std::uint8_t target = 0;  // TnTARGET; a target of 0 divides by 256
};

// The whole machine as a value: snapshot by copy, restore by assignment.
struct ApuState {
  Spc700State cpu{};
  std::array<std::uint8_t, 65536> ram{};
  std::uint8_t test = 0;
  std::uint8_t control = 0;
  std::uint8_t dspAddr = 0;
  std::array<std::uint8_t, 128> dsp{};
  std::array<std::uint8_t, 4> inputPorts{};   // host -> SPC700 (the CPU reads these at $F4-$F7)
  std::array<std::uint8_t, 4> outputPorts{};  // SPC700 -> host (the CPU writes these at $F4-$F7)
  std::uint16_t divider = 0;                  // shared free-running stage-1 cycle counter
  std::array<TimerState, 3> timers{};
};

class Apu {
 public:
  // The seeded post-IPL power-on machine: zeroed RAM, SP at $01EF, TEST $0A,
  // CONTROL $B0, the $AA/$BB ready bytes posted to output ports 0 and 1, and the
  // timers at their power-on values (TnTARGET $00, TnOUT $F). There is no IPL
  // ROM; a host points the CPU at a loaded image with setPc.
  Apu();
  explicit Apu(ApuState state);

  // The whole machine as a value. state() is coherent between steps; restore()
  // replaces every field.
  [[nodiscard]] const ApuState& state() const noexcept { return state_; }
  void restore(ApuState state);

  // The host face of the comm ports (index 0-3). writePort sets an input latch
  // the CPU reads; readPort returns an output latch the CPU wrote. Writing one
  // side never disturbs the other.
  void writePort(std::uint8_t index, std::uint8_t value);
  [[nodiscard]] std::uint8_t readPort(std::uint8_t index) const noexcept;

  // Host RAM access for loading images and inspecting memory. These bypass the
  // overlay — RAM is RAM from the host side, and a read of $00F0-$00FF returns
  // the underlying byte, not the register the CPU would see.
  [[nodiscard]] std::uint8_t readRam(std::uint16_t address) const noexcept {
    return state_.ram[address];
  }
  void writeRam(std::uint16_t address, std::uint8_t value) noexcept {
    state_.ram[address] = value;
  }
  void loadRam(std::uint16_t address, std::span<const std::uint8_t> bytes) noexcept;

  // Points the CPU at a loaded image (the machine has no IPL ROM to set an entry
  // for). A convenience over restoring a whole state with a changed PC.
  void setPc(std::uint16_t pc);

  // Runs one CPU instruction over the overlay and returns its cycle count.
  std::uint32_t step();

 private:
  // The internal bus: $00F0-$00FF route to the register overlay, everything else
  // is RAM. Both the CPU and its dummy reads pass through here.
  struct Bus {
    Apu& apu;
    std::uint8_t read(std::uint16_t address) { return apu.busRead(address); }
    void write(std::uint16_t address, std::uint8_t value) {
      apu.busWrite(address, value);
    }
  };

  std::uint8_t busRead(std::uint16_t address);
  void busWrite(std::uint16_t address, std::uint8_t value);
  std::uint8_t readRegister(std::uint8_t reg);
  void writeRegister(std::uint8_t reg, std::uint8_t value);

  Spc700 cpu_;      // the live CPU state during a step
  ApuState state_;  // RAM, overlay and timers are authoritative here; cpu is synced at each step
};

}  // namespace snaggletooth
