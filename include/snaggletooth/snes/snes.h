#pragma once

// The SNES machine — the 5A22 (its 65816 core) wired to the console's memory map,
// the work RAM, and the APU across the communication ports.
//
// The machine owns the CPU, the 128 KB of work RAM, and the audio machine. Its
// bus maps a 24-bit address the way the console does: work RAM in banks $7E-$7F
// and mirrored into the low pages of every system bank, the cartridge ROM in the
// LoROM window, the APU communication ports at $2140-$2143, and the work-RAM data
// port at $2180-$2183. A read of an unmapped address returns the last value the
// data bus carried.
//
// Every access is priced by the region it reaches. The console runs three memory
// speeds — six, eight, or twelve master cycles per access — and the machine
// charges each cycle its region's cost as the CPU makes it. The APU keeps its own
// slower clock: the machine advances it by the exact rational share of the master
// cycles that have elapsed, computed in integer arithmetic so a run is
// reproducible to the byte.
//
// step() runs one CPU instruction and returns the master cycles it took. run()
// spends an exact master-cycle budget and may stop part-way through an
// instruction, which is a legal resting place — instruction progress is part of
// the state value, so run(a) followed by run(b) advances the machine exactly as
// run(a + b) would.
//
// A machine runs at one of the two console clock rates, chosen at build. The
// region-speed map is counted in master cycles either way, so it does not change;
// only the master clock does, and with it the exact share of master cycles the
// APU's own crystal is paced against.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/cpu/cpu65816.h"

namespace snaggletooth {

// The console clock rate. It sets the master clock, and so the ratio the APU is
// paced against; the memory-region speeds, counted in master cycles, are the same
// in both.
enum class Region : std::uint8_t { Ntsc, Pal };

// How a machine is built: the cartridge image, the clock rate, and whether to
// seed the APU upload stub. The ROM is copied in, so the span need not outlive
// the call.
struct SnesConfig {
  std::span<const std::uint8_t> rom;   // a LoROM cartridge image
  Region region = Region::Ntsc;        // the console clock rate
  bool iplStub = true;                 // seed the APU with its upload stub (arrives with the loader)
};

// The whole machine as a value: snapshot by copy, restore by assignment. The ROM
// is fixed configuration rather than state and lives in the machine, not here, so
// restore() replaces the mutable machine and keeps the cartridge in place.
struct SnesState {
  Cpu65816State cpu{};
  std::array<std::uint8_t, 131072> wram{};  // 128 KB work RAM (banks $7E-$7F)
  ApuState apu{};
  std::uint32_t wmadd = 0;      // the work-RAM port address ($2181-$2183); $2180 auto-increments it
  std::uint8_t memsel = 0;      // $420D bit 0: the second waitstate region runs fast (1) or slow (0)
  std::uint8_t mdr = 0;         // the last value on the data bus, returned by a read of open bus
  std::uint64_t master = 0;     // the free-running master-cycle counter
  std::uint64_t consumed = 0;   // master cycles reported through run(); master - consumed is the budget carried between calls
  std::uint32_t apuPhase = 0;   // the APU-clock accumulator for the exact master-to-APU cycle ratio
};

class Snes {
 public:
  // Builds the machine from a cartridge and seeds the power-on state: work RAM
  // cleared, the APU in its post-boot ready state, and the CPU in emulation mode
  // with its program counter at the cartridge's reset vector.
  explicit Snes(SnesConfig config);

  // The whole machine as a value. state() is coherent at any cycle the machine has
  // stopped on, mid-instruction included; restore() replaces the mutable machine
  // and resumes exactly there, keeping the cartridge and clock rate in place.
  [[nodiscard]] const SnesState& state() const noexcept { return state_; }
  void restore(SnesState state);

  // The clock rate the machine was built at. Fixed for its life, like the
  // cartridge.
  [[nodiscard]] Region region() const noexcept { return region_; }

  // Runs master cycles to the end of one CPU instruction and returns how many it
  // took. Called after run() stopped mid-instruction, it finishes the instruction
  // in progress. A halted core runs one idle cycle and returns its cost, so the
  // APU keeps going while the CPU sits.
  std::uint32_t step();

  // Runs exactly `budget` master cycles and returns that count. A cycle is priced
  // by its region, so the machine may pass the budget part-way through a cycle; the
  // overshoot is carried into the next call, making run(a) then run(b) advance the
  // machine exactly as run(a + b). run(0) runs nothing.
  std::uint64_t run(std::uint64_t budget);

  // Drains the 32 kHz stereo frames the APU has produced since the last drain. The
  // machine paces the APU, so frames accumulate as it runs; a caller drains
  // periodically to bound the queue. Frames are output, not state.
  [[nodiscard]] std::vector<StereoFrame> takeFrames();

 private:
  // The mapped bus the CPU runs over. Each access records its region's master cost
  // on the machine and routes to work RAM, the cartridge, or a register; an
  // internal cycle drives an address without an access and costs the fast rate.
  struct Bus {
    Snes& m;
    std::uint8_t read(std::uint32_t address, CycleKind) { return m.busRead(address); }
    void write(std::uint32_t address, std::uint8_t value, CycleKind) {
      m.busWrite(address, value);
    }
    void internal(std::uint32_t) { m.busInternal(); }
    void internal(std::uint32_t, CycleKind) { m.busInternal(); }
  };

  // One master-cycle group: the CPU makes its single access (which prices the
  // cycle), the master counter advances by that cost, and the APU is paced forward
  // by the master cycles it now owes.
  void machineCycle();

  // Reloads the live CPU and APU from state_ after a construct or restore.
  void load();
  // Copies the live CPU and APU back into state_ before a public return.
  void sync();

  std::uint8_t busRead(std::uint32_t address);
  void busWrite(std::uint32_t address, std::uint8_t value);
  void busInternal() noexcept { lastCost_ = 6; }

  // The master-cycle cost of reaching `address`, by the documented region map. The
  // second waitstate region ($80-$BF:$8000-$FFFF and $C0-$FF) follows MEMSEL.
  [[nodiscard]] std::uint32_t accessCost(std::uint32_t address) const noexcept;

  // The cartridge byte a LoROM address reaches, mirrored across the image. Pure —
  // it neither prices the cycle nor touches the data bus.
  [[nodiscard]] std::uint8_t romByte(std::uint8_t bank, std::uint16_t offset) const noexcept;

  // The work-RAM data port: $2180 reads or writes work RAM at the port address and
  // steps it; $2181-$2183 set the address and read back as open bus.
  std::uint8_t readWramPort(std::uint16_t offset);
  void writeWramPort(std::uint16_t offset, std::uint8_t value);

  // Records `value` as the data bus's last byte and returns it, so an unmapped read
  // that follows sees it.
  std::uint8_t latch(std::uint8_t value) noexcept {
    state_.mdr = value;
    return value;
  }

  // The CPU's power-on state: emulation mode, the interrupt disable set, and the
  // program counter at the cartridge's reset vector.
  [[nodiscard]] Cpu65816State powerOnCpu() const;

  Cpu65816 cpu_;                     // the live CPU while the machine runs
  Apu apu_;                          // the live audio machine, paced by the interleave
  SnesState state_;                  // work RAM, registers and counters are authoritative here
  std::vector<std::uint8_t> rom_;    // the cartridge image, fixed for the machine's life
  Region region_ = Region::Ntsc;     // the clock rate, fixed for the machine's life
  std::uint32_t apuNum_ = 5632u;     // the APU-to-master cycle ratio for this region (numerator)
  std::uint32_t apuDen_ = 118125u;   // and its denominator
  std::uint32_t lastCost_ = 6;       // the master cost of the cycle in progress
};

}  // namespace snaggletooth
