#pragma once

// The APU machine — the SPC700 wrapped in its 64KB RAM and the $F0-$FF register
// overlay.
//
// The machine owns the CPU, the RAM, and the hardware registers the CPU reaches
// through the top sixteen bytes of zero page. Its internal bus satisfies the
// ApuBus concept: reads and writes in $00F0-$00FF hit the register overlay; an
// SPC700 read in $FFC0-$FFFF returns the mapped boot-ROM image when one is set and
// CONTROL bit 7 is on (see mapIplRom); everything else is plain RAM. The host
// drives the comm ports from the other side and loads programs directly into RAM.
// The machine holds no console boot ROM — it comes up in the seeded post-IPL ready
// state, and any image a host maps over the window is an original program written
// to the documented upload protocol.
//
// The machine runs one cycle at a time. Within a cycle the clocked events go
// first — the master counter advances, the stage-1 timer ticks land, the DSP
// takes its slot — and the CPU's single bus access lands last. So a read of a
// timer output on the cycle that timer ticks returns the incremented count, and
// a DSP write lands on its true cycle relative to the 32-cycle sample boundary.
//
// One counter drives all of it: the SPC700 and the DSP share a clock, so the
// timer phase and the sample phase are the same free-running phase. T0 and T1
// tick every 128 cycles and T2 every 16, on the counter's documented slots; the
// DSP takes a sample every 32. Each timer's stage-2 counter increments on its
// stage-1 ticks while enabled and, on reaching its target, advances a 4-bit
// stage-3 counter that an overlay read of TnOUT returns and clears.
//
// step() runs one instruction; run() runs an exact number of cycles and may
// stop mid-instruction, which is a legal resting place — instruction progress
// is part of the state value.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "snaggletooth/apu/dsp.h"
#include "snaggletooth/apu/spc700.h"

namespace snaggletooth {

// The audio unit's boot-ROM window: 64 bytes at $FFC0-$FFFF. When CONTROL bit 7 is
// set and an image is mapped (Apu::mapIplRom), an SPC700 read in this range returns
// the image byte; the RAM beneath stays writable and reappears when the bit clears.
inline constexpr std::uint16_t kIplWindowBase = 0xFFC0u;
inline constexpr std::size_t kIplWindowBytes = 64u;

// One timer's mutable stage counters. The enable bit lives in the CONTROL
// register (ApuState::control); the stage-1 ticks come off the machine's master
// counter, ApuState::divider.
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
  // The auxiliary port bytes the CPU reads at $F8/$F9 (the S-SMP's unconnected
  // P4/P5 pins). Storage of their own, not a view of RAM: a CPU write lands here
  // as well as in the RAM beneath, a CPU read returns this value, and the S-DSP's
  // echo buffer writes — which reach only the RAM — never alter it. The
  // unconnected pins read back as written; power-on leaves them high ($FF).
  std::array<std::uint8_t, 2> auxPorts{0xFF, 0xFF};
  std::uint16_t divider = 0;                  // the master cycle counter: timer ticks and sample boundaries
  std::array<TimerState, 3> timers{};
};

class Apu {
 public:
  // The seeded post-IPL power-on machine: zeroed RAM, SP at $01EF, TEST $0A,
  // CONTROL $B0, the $AA/$BB ready bytes posted to output ports 0 and 1, and the
  // timers at their power-on values (TnTARGET $00, TnOUT $F). No image is mapped
  // over the $FFC0 window; a host loads a program into RAM and points the CPU at it
  // with setPc, or maps a boot-ROM image with mapIplRom to run the upload protocol.
  Apu();
  explicit Apu(ApuState state);

  // The whole machine as a value. state() is coherent at any cycle the machine
  // has stopped on, mid-instruction included; restore() replaces every field and
  // resumes exactly there.
  [[nodiscard]] const ApuState& state() const noexcept { return state_; }
  void restore(ApuState state);

  // Re-seeds the post-IPL state with the documented reset differences: the timer
  // outputs clear to 0 (the power-on value is $F), but the targets and the master
  // counter are retained (a free-running counter cannot be reset), CONTROL and
  // TEST return to their reset values, the ready bytes are re-posted, zero page
  // is cleared, and the rest of RAM is left as it was.
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

  // Maps a 64-byte boot-ROM image over the $FFC0-$FFFF window. While CONTROL bit 7
  // is set, an SPC700 read in that range returns the image; every write still lands
  // in the RAM beneath, and clearing the bit exposes that RAM again. A machine with
  // no image mapped — the default — reads plain RAM there regardless of the bit.
  // The image is fixed configuration, not machine state: restore() and reset() keep
  // it, the way the console keeps its boot ROM. There is no console ROM here; the
  // image is an original program written to the documented upload protocol.
  void mapIplRom(std::span<const std::uint8_t, kIplWindowBytes> image);

  // Runs machine cycles to the end of one CPU instruction and returns how many it
  // took. Called after run() stopped mid-instruction, it finishes the instruction
  // in progress rather than starting one. A halted core runs 2 cycles and returns
  // 2, so the timers and the DSP keep going while the CPU sits idle.
  std::uint32_t step();

  // Runs exactly `budget` machine cycles and returns that count. The stop lands
  // wherever the budget falls, mid-instruction included — run(a) then run(b) is
  // bitwise run(a+b). run(0) runs nothing.
  std::uint64_t run(std::uint64_t budget);

  // Drains the 32 kHz stereo frames the DSP has produced since the last drain,
  // clearing the internal queue. One frame lands per DSP sample — every 32
  // machine cycles — so a caller drains periodically to bound the queue. Frames are output, not machine state: they are not part of a
  // snapshot, and restore() and reset() discard any that are pending.
  [[nodiscard]] std::vector<StereoFrame> takeFrames();

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

  // Reloads the live CPU from state_ and re-locks the DSP sample slot to the master
  // counter after a construct, restore, or reset.
  void syncCpuAndSlot();

  std::uint8_t busRead(std::uint16_t address);
  void busWrite(std::uint16_t address, std::uint8_t value);
  std::uint8_t readRegister(std::uint8_t reg);
  void writeRegister(std::uint8_t reg, std::uint8_t value);

  // Writes a DSP register through DSPDATA: cpuWriteDspRegister, which owns the
  // write's semantics (ENDX's acknowledge, KON's arming, the stamp a
  // DSP-written register carries).
  void writeDspRegister(std::uint8_t reg, std::uint8_t value);

  // One machine cycle. The master counter advances, the timer ticks and the DSP
  // sample boundary that land on the new count are taken, and then the CPU makes
  // its one bus access — the order the chips share their multiplexed bus in, and
  // the reason a read sees the tick that shares its cycle.
  void machineCycle();

  // One stage-1 tick for timer `index`: its stage-2 counter increments while the
  // timer is enabled and, on reaching the target, advances stage 3 and zeroes.
  void tickTimer(std::size_t index);

  // Runs one of the DSP sample's 32 slots this cycle and, on the wrap slot, queues
  // the stereo frame the sample finished. The RAM span is writable so the echo unit
  // can reach its delay line in APU RAM directly.
  void sampleFrame();

  Spc700 cpu_;      // the live CPU state while the machine runs
  ApuState state_;  // RAM, overlay and timers are authoritative here; cpu is synced before every return
  std::vector<StereoFrame> frames_;  // DSP output awaiting the host's drain; not part of the snapshot
  std::optional<std::array<std::uint8_t, kIplWindowBytes>> iplImage_;  // the $FFC0 window image; config, absent by default, kept across restore()/reset()
};

}  // namespace snaggletooth
