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
// The three timers advance on the cycles each instruction delivers. A single
// free-running stage-1 divider counts machine cycles for all three (T0 and T1
// tick every 128, T2 every 16); each timer's stage-2 counter increments on its
// stage-1 ticks while enabled and, on reaching its target, advances a 4-bit
// stage-3 counter that an overlay read of TnOUT returns and clears. step() runs
// one instruction and then advances the timers by the cycles it took; run()
// steps whole instructions against a cycle budget.

#include <array>
#include <cstdint>
#include <span>

#include "snaggletooth/apu/dsp.h"
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
  DspState dsp{};
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

  // Re-seeds the post-IPL state with the documented reset differences: the timer
  // outputs clear to 0 (the power-on value is $F), but the targets and the shared
  // stage-1 divider are retained (the divider cannot be reset), CONTROL and TEST
  // return to their reset values, the ready bytes are re-posted, zero page is
  // cleared, and the rest of RAM is left as it was.
  void reset();

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

  // Runs one CPU instruction over the overlay, advances the timers by the cycles
  // it took, and returns that count. A halted core still delivers 2 cycles, so
  // the timers keep ticking while the CPU sits idle.
  std::uint32_t step();

  // Steps whole instructions until at least `budget` cycles have run, returning
  // the cycles actually run — the last instruction may overshoot, and the caller
  // carries the remainder. run(0) runs nothing and returns 0. Every instruction
  // costs at least 2 cycles, so the budget is always reached.
  std::uint64_t run(std::uint64_t budget);

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

  // Writes a DSP register through DSPDATA. Most registers are a RAM-like byte;
  // ENDX ($7C) is a real register whose bits any write acknowledges (clears).
  void writeDspRegister(std::uint8_t reg, std::uint8_t value);

  // Advances the shared stage-1 divider by `cycles` and passes the resulting
  // stage-1 ticks to each enabled timer's stage-2/stage-3 counters.
  void advanceTimers(std::uint32_t cycles);

  // Advances the DSP's 32-cycle sample divider by `cycles`. It free-runs; the
  // voice pipeline that consumes its sample ticks arrives with the next unit.
  void advanceDsp(std::uint32_t cycles);

  Spc700 cpu_;      // the live CPU state during a step
  ApuState state_;  // RAM, overlay and timers are authoritative here; cpu is synced at each step
};

}  // namespace snaggletooth
