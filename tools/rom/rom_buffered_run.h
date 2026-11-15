#pragma once

// The run over a machine a host runs, with the host's thread copying each
// boundary's record into a buffer and the watching on threads of the run's own.
//
// `ObservedRun` (`rom/rom_observe.h`) does its work inside the machine's reports,
// on the thread that runs the machine. A `BufferedRun` sets the same three
// observers, and each of them copies what it is told into a buffer
// (`rom/rom_buffer.h`) and does nothing else: no decode, no compare, no file
// call, no lock. The run works through the buffer on threads of its own. The
// two CPUs' instruction checks run on a pool, a block of the buffer to a thread
// at a time, each from the register file at the block's first boundary; the
// shadow and the recorder run on one thread, which takes each block's checks in
// order and plays the shadow's calls the checks kept; the sound CPU's register
// copy and its samples run on another. The buffer holds what the threads have
// not reached, in memory to the size the host names and then in files under the
// directory it names, so a host on a slow CPU runs at its own pace and the
// threads catch up; the host names both sizes and the run imposes no floor or
// ceiling on either. Every thread the run starts runs at the lowest scheduling
// class the platform gives a thread (`lowerThisThread`).
//
// The observation is the one `observeRun` makes of the same play, field for
// field, with the same notes. Once a frame the host puts the whole machine into
// the buffer with `keyframe()`, and the run checks the work RAM it keeps against
// it.
//
//   Snes machine{SnesConfig{.rom = rom}};
//   disasm::BufferedRun run{machine, rom, directory, {}};  // 1 GB of RAM, 64 GB of files
//   for (int frame = 0; frame < 3600; ++frame) {
//     machine.run(357'366u);  // the host's own loop
//     run.keyframe();
//   }
//   std::vector<std::string> notes;
//   const disasm::RunObservation seen = run.finish(notes);

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rom/rom_observe.h"

namespace snaggletooth::disasm {

struct BufferedRunOptions {
  std::uint64_t memoryBytes = 1'000'000'000;  // the most RAM the buffer holds, both streams together
  std::uint64_t diskBytes = 64'000'000'000;   // the most it holds in files
  unsigned threads = 0;  // the pool's threads; 0: two fewer than the host's cores, at least 1
};

// Bound like ObservedRun — to a machine at power-on, from the image it was built
// from, with no bus, audio or instruction observer set — it sets the three
// observers; the host runs the machine as it likes with `step()` or `run()`,
// presents its own pads, and calls `keyframe()` between two of its calls once a
// frame. Every member but `progress()`, `full()` and `observation()` is called
// on the host's thread between two of its calls into the machine, and none from
// inside a report.
class BufferedRun {
 public:
  // Throws `std::logic_error` as `ObservedRun`'s constructor does: a machine that
  // has run a master cycle, one with an observer set, an image with no cartridge
  // header. Throws `std::runtime_error` when `directory` cannot be created. The
  // buffer's files go under `directory/cpu` and `directory/sound`. The host makes
  // no call into the guest and restores no state while the run is bound.
  BufferedRun(Snes& machine, std::span<const std::uint8_t> rom, std::filesystem::path directory,
              BufferedRunOptions options);
  // `finish()` when it was not called: the observers are cleared and every
  // thread is joined. The buffer's files are removed, and `directory` when that
  // leaves it empty.
  ~BufferedRun();
  BufferedRun(const BufferedRun&) = delete;
  BufferedRun& operator=(const BufferedRun&) = delete;

  // The whole machine, once a frame, between two of the host's calls.
  void keyframe();

  // Pulls the console's reset line (`Snes::reset()`) and takes the run through
  // it, as `ObservedRun::reset` does.
  void reset();

  // How many times the beam has wrapped to line 0 since power-on, counted on
  // the host's thread: the frame a host presents an input script's pads for.
  [[nodiscard]] std::uint32_t frame() const noexcept;

  // Whether the buffer filled: what it holds would pass the RAM and the disk
  // together. The observers copy nothing more, and the observation is of
  // everything before. A host that stops calling into the machine before the
  // buffer fills, by `progress()`, loses nothing.
  [[nodiscard]] bool full() const noexcept;

  // What the threads have reached: the master cycles of the last boundary they
  // played, against the host's; the bytes the buffer holds, the RAM its blocks
  // take and the bytes in its files now, against the two limits; the bytes
  // written to files so far; and how many keyframes the work RAM the run keeps
  // differed from, which is 0 for a record that holds everything the run reads.
  struct Progress {
    std::uint64_t playedMaster = 0;
    std::uint64_t hostMaster = 0;
    std::uint64_t held = 0;
    std::uint64_t memoryUsed = 0;
    std::uint64_t memoryBytes = 0;
    std::uint64_t diskUsed = 0;
    std::uint64_t diskBytes = 0;
    std::uint64_t diskWritten = 0;
    std::uint64_t keyframesDiffered = 0;
  };
  [[nodiscard]] Progress progress() const noexcept;

  // The observation of everything the threads have played, with the run left
  // going, as `ObservedRun::observation` gives one. Asked on the host's thread,
  // the blocks being written are handed over first, so it is the observation at
  // the host's boundary; asked on another thread, it is of every block the
  // buffer had handed over when it was asked. It waits for the threads to reach
  // that block. `notes` gains the notes so far, the main CPU's and then the sound
  // CPU's. Throws `std::logic_error` once the run is finished.
  [[nodiscard]] RunObservation observation(std::vector<std::string>& notes);

  // Ends the run: the observers are cleared, the buffer ends, the threads play
  // everything buffered, every thread is joined, and the whole observation is
  // answered, as `ObservedRun::finish` answers it. Called once; a second call
  // throws `std::logic_error`.
  [[nodiscard]] RunObservation finish(std::vector<std::string>& notes);

  // The first failure a thread of the run met — a buffer file that could not
  // be written or read, a block that did not play — or nothing. From a failure
  // on, the threads play nothing more; `observation()` and `finish()` answer
  // everything played before it, and their notes end with
  // `run: <failure>; the observation holds what was played before it`.
  [[nodiscard]] std::string failure() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace snaggletooth::disasm
