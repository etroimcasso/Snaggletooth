// The run with the host's thread copying and the watching on threads of its
// own, held to the run on the host's thread: the same observation and the same
// notes on every example, however the host runs the machine and however many
// threads check it; a reset and an observation midway as `ObservedRun` takes
// them; the refusals; a buffer that reaches its bound, one that goes through
// files, and a keyframe the kept work RAM differs from.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "run_test_support.h"
#include "rom/rom_buffered_run.h"
#include "rom/rom_observe.h"
#include "rom/rom_observe_internal.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/snes/video_frame.h"

namespace snaggletooth::disasm {
namespace {

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

// A directory of this case's own, empty at the start of it.
class RomBufferedRun : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* const which = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("snaggletooth-buffered-" + std::string(which == nullptr ? "case" : which->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::filesystem::path dir_;
};

struct Hosted {
  RunObservation observation;
  std::vector<std::string> notes;
  std::uint64_t master = 0;
  BufferedRun::Progress progress;
};

BufferedRunOptions withThreads(unsigned threads) {
  return BufferedRunOptions{.memoryBytes = 1'000'000'000, .diskBytes = 64'000'000'000, .threads = threads};
}

// The host `observeRun` is: the machine stepped to the budget, each frame's pads
// presented after the step that wrapped the beam, and here a keyframe there too.
Hosted bufferedSteps(std::span<const std::uint8_t> rom, std::uint64_t cycles, const InputScript& input,
                     const std::filesystem::path& dir, BufferedRunOptions options) {
  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir, options};
  std::uint32_t frame = 0;
  detail::presentPads(machine, input, frame);
  std::uint64_t spent = 0;
  while (spent < cycles) {
    spent += machine.step();
    if (run.frame() != frame) {
      frame = run.frame();
      detail::presentPads(machine, input, frame);
      run.keyframe();
    }
  }
  Hosted out;
  out.master = machine.state().master;
  out.observation = run.finish(out.notes);
  out.progress = run.progress();
  return out;
}

SnesConfig threadedConfig(std::span<const std::uint8_t> rom, bool audioThread, bool pictureThread) {
  SnesConfig config;
  config.rom = rom;
  config.apuThread = audioThread;
  config.ppuThread = pictureThread;
  return config;
}

// A player's pads: the next frame's presented at every frame the PPU finishes.
struct FramePads final : FrameObserver {
  Snes& machine;
  const InputScript& input;
  std::uint32_t count = 0;
  FramePads(Snes& m, const InputScript& i) : machine(m), input(i) { detail::presentPads(machine, input, 0); }
  void frame(const VideoFrame&) override { detail::presentPads(machine, input, ++count); }
};

// A host that runs the machine a quarter frame at a time past `cycles`, then
// finishes the instruction the last call stopped inside with one step(): with
// `options`, a BufferedRun, a keyframe after every call that saw the beam wrap;
// without, an ObservedRun.
Hosted runHost(std::span<const std::uint8_t> rom, std::uint64_t cycles, const InputScript& input, bool audioThread,
               bool pictureThread, const std::filesystem::path* dir, BufferedRunOptions options) {
  Snes machine{threadedConfig(rom, audioThread, pictureThread)};
  FramePads pads{machine, input};
  machine.setFrameObserver(&pads);
  Hosted out;
  if (dir != nullptr) {
    BufferedRun run{machine, rom, *dir, options};
    std::uint32_t frame = 0;
    while (machine.state().master < cycles) {
      machine.run(kFrame / 4u);
      if (run.frame() != frame) {
        frame = run.frame();
        run.keyframe();
      }
    }
    machine.step();
    out.master = machine.state().master;
    out.observation = run.finish(out.notes);
    out.progress = run.progress();
  } else {
    ObservedRun run{machine, rom};
    while (machine.state().master < cycles) machine.run(kFrame / 4u);
    machine.step();
    out.master = machine.state().master;
    out.observation = run.finish(out.notes);
  }
  machine.setFrameObserver(nullptr);
  return out;
}

bool anyNoteHolds(const std::vector<std::string>& notes, std::string_view text) {
  for (const std::string& note : notes) {
    if (note.find(text) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_F(RomBufferedRun, TheObservationIsObserveRunsOnEveryExample) {
  const InputScript script = exampleScript();
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    std::vector<std::string> ownNotes;
    const RunObservation own = observeRun(rom, 12u * kFrame, script, ownNotes);
    const Hosted reference = runHost(rom, 12u * kFrame, script, false, false, nullptr, {});
    for (const unsigned threads : {1u, 3u}) {
      const std::string what = std::string(example.name) + ", " + std::to_string(threads) + " threads";
      const Hosted stepped = bufferedSteps(rom, 12u * kFrame, script, dir_, withThreads(threads));
      expectSameObservation(own, stepped.observation, what + ", step()");
      EXPECT_EQ(stepped.notes, ownNotes) << what;
      EXPECT_EQ(stepped.progress.keyframesDiffered, 0u) << what;
      const Hosted ran = runHost(rom, 12u * kFrame, script, false, false, &dir_, withThreads(threads));
      ASSERT_EQ(ran.master, reference.master) << what;
      expectSameObservation(reference.observation, ran.observation, what + ", run()");
      EXPECT_EQ(ran.notes, reference.notes) << what;
      EXPECT_EQ(ran.progress.keyframesDiffered, 0u) << what;
      EXPECT_EQ(ran.progress.playedMaster, ran.progress.hostMaster) << what << ": every boundary played";
      EXPECT_EQ(ran.progress.held, 0u) << what;
    }
  }
}

TEST_F(RomBufferedRun, TheObservationIsTheSameUnderEitherThreadAndBoth) {
  // The drawing cartridge runs transfers and HDMA every frame and keys two
  // voices on, so both streams carry every kind of record.
  const std::vector<std::uint8_t> rom = examples::drawingImage();
  const InputScript script = exampleScript();
  const Hosted reference = runHost(rom, 12u * kFrame, script, false, false, nullptr, {});
  ASSERT_GT(reference.observation.spc700Instructions, 0u);
  ASSERT_FALSE(reference.observation.samples.empty()) << "a key-on is in the run";
  for (const bool audioThread : {false, true}) {
    for (const bool pictureThread : {false, true}) {
      const std::string what = std::string("audio thread ") + (audioThread ? "on" : "off") + ", picture thread " +
                               (pictureThread ? "on" : "off");
      const Hosted ran = runHost(rom, 12u * kFrame, script, audioThread, pictureThread, &dir_, withThreads(2));
      ASSERT_EQ(ran.master, reference.master) << what;
      expectSameObservation(reference.observation, ran.observation, what);
      EXPECT_EQ(ran.notes, reference.notes) << what;
      EXPECT_EQ(ran.progress.keyframesDiffered, 0u) << what;
    }
  }
}

TEST_F(RomBufferedRun, AResetInTheMiddleIsTakenAsObservedRunTakesIt) {
  // The resetting cartridge has a fill open, a table walked and a helper that
  // never returns at the reset. An observation right after it ends the
  // buffer's blocks on the reset's records.
  const std::vector<std::uint8_t> rom = resettingImage();
  const auto host = [&](auto& run, Snes& machine, RunObservation& atReset, std::vector<std::string>& atResetNotes) {
    std::uint64_t spent = 0;
    while (machine.state().cpu.pc != 0x808Du && spent < 2u * kFrame) spent += machine.step();
    spent = 0;
    while (spent < kFrame + kFrame / 2u) spent += machine.step();
    run.reset();
    atReset = run.observation(atResetNotes);
    spent = 0;
    while (spent < 3u * kFrame) spent += machine.step();
  };
  Snes plainMachine{SnesConfig{.rom = rom}};
  ObservedRun plain{plainMachine, rom};
  RunObservation plainAtReset;
  std::vector<std::string> plainAtResetNotes;
  host(plain, plainMachine, plainAtReset, plainAtResetNotes);
  std::vector<std::string> plainNotes;
  const RunObservation expected = plain.finish(plainNotes);
  ASSERT_EQ(plainMachine.state().cpu.pc, 0x806Fu) << "the pass after the reset spins";
  EXPECT_EQ(expected.divergences, 0u);
  EXPECT_EQ(expected.spc700Divergences, 0u);

  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir_, withThreads(2)};
  RunObservation atReset;
  std::vector<std::string> atResetNotes;
  host(run, machine, atReset, atResetNotes);
  EXPECT_EQ(run.frame(), plain.frame()) << "the reset is no wrap of the beam on the host's count either";
  std::vector<std::string> notes;
  const RunObservation seen = run.finish(notes);
  expectSameObservation(plainAtReset, atReset, "at the reset");
  EXPECT_EQ(atResetNotes, plainAtResetNotes);
  expectSameObservation(expected, seen, "a reset in the helper's spin");
  EXPECT_EQ(notes, plainNotes);
}

TEST_F(RomBufferedRun, TheResetLineGoesThroughTheSoundCpuAsObservedRunTakesIt) {
  // The sound boot program of the test's own: after the reset it writes $04
  // through $F3 to whatever register the reset left the DSP's address at, and
  // keys voice 0 on without setting DIR. Four points a CPU step apart.
  std::vector<std::uint8_t> rom = examples::loRomImage(1);
  examples::put(rom, 0x0000u, {0x80u, 0xFEu});  // $8000 BRA $8000
  SnesConfig config;
  config.rom = rom;
  config.bootRom = resettingBootImage();
  for (const unsigned offset : {0u, 5u, 10u, 15u}) {
    const std::string what = "reset after " + std::to_string(offset) + " more steps";
    const auto host = [&](auto& run, Snes& machine) {
      std::uint64_t spent = 0;
      while (spent < kFrame) spent += machine.step();
      for (unsigned k = 0; k < offset; ++k) static_cast<void>(machine.step());
      run.reset();
      spent = 0;
      while (spent < kFrame) spent += machine.step();
    };
    Snes plainMachine{config};
    ObservedRun plain{plainMachine, rom};
    host(plain, plainMachine);
    std::vector<std::string> plainNotes;
    const RunObservation expected = plain.finish(plainNotes);
    ASSERT_TRUE(!expected.samples.empty() || !plainNotes.empty()) << what << ": the key-on after the reset is seen";

    Snes machine{config};
    BufferedRun run{machine, rom, dir_, withThreads(2)};
    host(run, machine);
    std::vector<std::string> notes;
    const RunObservation seen = run.finish(notes);
    expectSameObservation(expected, seen, what);
    EXPECT_EQ(notes, plainNotes) << what;
  }
}

TEST_F(RomBufferedRun, AnObservationOnTheHostsThreadHoldsTheSoundCpusSamplesAndNotes) {
  // The drawing cartridge keys two voices on; with its directory's entry 0
  // pointed at $FF01, one of them makes a note on the sound CPU.
  std::vector<std::uint8_t> noting = examples::drawingImage();
  noting[0x2780u] = 0x01u;  // entry 0: start $FF01
  noting[0x2781u] = 0xFFu;
  for (const std::vector<std::uint8_t>& rom : {examples::drawingImage(), noting}) {
    Snes plainMachine{SnesConfig{.rom = rom}};
    ObservedRun plain{plainMachine, rom};
    Snes machine{SnesConfig{.rom = rom}};
    BufferedRun run{machine, rom, dir_, withThreads(2)};
    std::uint64_t spent = 0;
    while (spent < 12u * kFrame) spent += machine.step();
    std::uint64_t plainSpent = 0;
    while (plainSpent < 12u * kFrame) plainSpent += plainMachine.step();
    std::vector<std::string> plainNotes;
    const RunObservation expected = plain.observation(plainNotes);
    std::vector<std::string> notes;
    const RunObservation seen = run.observation(notes);
    ASSERT_TRUE(!expected.samples.empty() || !plainNotes.empty());
    expectSameObservation(expected, seen, "midway");
    EXPECT_EQ(notes, plainNotes);
    std::vector<std::string> ignored;
    static_cast<void>(plain.finish(ignored));
    static_cast<void>(run.finish(ignored));
  }
}

TEST_F(RomBufferedRun, AnObservationOnTheHostsThreadIsObservedRunsAtTheSameBoundary) {
  // The moving cartridge walks two HDMA tables and sends a sprite table every
  // frame, so halfway down a frame a walk is open.
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const std::uint64_t middle = 2u * kFrame + kFrame / 2u;
  Snes plainMachine{SnesConfig{.rom = rom}};
  ObservedRun plain{plainMachine, rom};
  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir_, withThreads(2)};
  std::uint64_t spent = 0;
  while (spent < middle) spent += machine.step();
  std::uint64_t plainSpent = 0;
  while (plainSpent < middle) plainSpent += plainMachine.step();
  ASSERT_EQ(machine.state().master, plainMachine.state().master);

  std::vector<std::string> plainMidNotes;
  const RunObservation plainMid = plain.observation(plainMidNotes);
  std::vector<std::string> midNotes;
  const RunObservation mid = run.observation(midNotes);
  expectSameObservation(plainMid, mid, "midway");
  EXPECT_EQ(midNotes, plainMidNotes);
  EXPECT_GT(mid.instructions, 0u);
  EXPECT_EQ(run.progress().playedMaster, run.progress().hostMaster) << "the threads reached the host's boundary";

  while (spent < 4u * kFrame) spent += machine.step();
  while (plainSpent < 4u * kFrame) plainSpent += plainMachine.step();
  std::vector<std::string> plainNotes;
  const RunObservation whole = plain.finish(plainNotes);
  std::vector<std::string> notes;
  const RunObservation seen = run.finish(notes);
  expectSameObservation(whole, seen, "with an observation midway");
  EXPECT_EQ(notes, plainNotes);
}

TEST_F(RomBufferedRun, AnObservationFromAnotherThreadIsOfWhatTheBufferHandedOver) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir_, withThreads(2)};
  std::uint64_t spent = 0;
  while (spent < 3u * kFrame) spent += machine.step();
  RunObservation elsewhere;
  std::vector<std::string> elsewhereNotes;
  std::thread asker([&] { elsewhere = run.observation(elsewhereNotes); });
  asker.join();
  std::vector<std::string> hereNotes;
  const RunObservation here = run.observation(hereNotes);
  EXPECT_GT(elsewhere.instructions, 0u) << "three frames fill blocks the buffer hands over";
  EXPECT_LE(elsewhere.instructions, here.instructions) << "the block being written is not handed over from there";
  std::vector<std::string> notes;
  const RunObservation seen = run.finish(notes);
  EXPECT_EQ(seen.instructions, here.instructions);
  EXPECT_EQ(seen.divergences, 0u);
}

TEST_F(RomBufferedRun, AMachinePastPowerOnOrWithAnObserverOrADirectoryThatCannotBeMadeIsRefused) {
  const std::vector<std::uint8_t> rom = examples::threeBankImage();
  {
    Snes machine{SnesConfig{.rom = rom}};
    static_cast<void>(machine.step());
    EXPECT_THROW(BufferedRun(machine, rom, dir_, {}), std::logic_error) << "the machine has run";
    EXPECT_EQ(machine.observer(), nullptr);
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    ObservedRun other{machine, rom};
    EXPECT_THROW(BufferedRun(machine, rom, dir_, {}), std::logic_error) << "observers set";
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    const std::vector<std::uint8_t> tooSmall(16, 0u);
    EXPECT_THROW(BufferedRun(machine, tooSmall, dir_, {}), std::logic_error) << "no cartridge header";
    EXPECT_EQ(machine.instructionObserver(), nullptr);
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    std::filesystem::create_directories(dir_);
    { std::ofstream(dir_ / "file") << "a file, not a directory"; }
    EXPECT_THROW(BufferedRun(machine, rom, dir_ / "file" / "buffer", {}), std::runtime_error);
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.apuObserver(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), nullptr);
  }
}

TEST_F(RomBufferedRun, TheObserversAreTheRunsUntilItFinishesOrGoes) {
  const std::vector<std::uint8_t> rom = examples::threeBankImage();
  {
    Snes machine{SnesConfig{.rom = rom}};
    {
      BufferedRun run{machine, rom, dir_, withThreads(1)};
      EXPECT_NE(machine.observer(), nullptr);
      EXPECT_NE(machine.apuObserver(), nullptr);
      EXPECT_NE(machine.instructionObserver(), nullptr);
      for (int i = 0; i < 1000; ++i) static_cast<void>(machine.step());
      // Let go without finish(): every thread is joined, the observers cleared.
    }
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.apuObserver(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), nullptr);
    EXPECT_FALSE(std::filesystem::exists(dir_)) << "the buffer's directory goes with the run";
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    BufferedRun run{machine, rom, dir_, withThreads(1)};
    std::vector<std::string> notes;
    static_cast<void>(run.finish(notes));
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), nullptr);
    EXPECT_THROW(static_cast<void>(run.finish(notes)), std::logic_error) << "finished once";
    EXPECT_THROW(run.keyframe(), std::logic_error);
    EXPECT_THROW(static_cast<void>(run.observation(notes)), std::logic_error);
  }
}

TEST_F(RomBufferedRun, ABufferThatGoesThroughFilesPlaysEveryRecord) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const InputScript script = exampleScript();
  std::vector<std::string> ownNotes;
  const RunObservation own = observeRun(rom, 12u * kFrame, script, ownNotes);
  const Hosted throughFiles = bufferedSteps(rom, 12u * kFrame, script, dir_,
                                        BufferedRunOptions{.memoryBytes = 1'000'000, .diskBytes = 256'000'000, .threads = 1});
  expectSameObservation(own, throughFiles.observation, "1 MB of RAM");
  EXPECT_EQ(throughFiles.notes, ownNotes);
  EXPECT_GT(throughFiles.progress.diskWritten, 0u) << "the run went through files";
  EXPECT_EQ(throughFiles.progress.diskUsed, 0u) << "and every file was read back";
  EXPECT_EQ(throughFiles.progress.memoryBytes, 1'000'000u);
  EXPECT_EQ(throughFiles.progress.diskBytes, 256'000'000u);
  EXPECT_FALSE(std::filesystem::exists(dir_)) << "and they are gone";
}

TEST_F(RomBufferedRun, ABufferThatFillsLeavesTheObservationOfWhatCameBefore) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const InputScript script = exampleScript();
  std::vector<std::string> ownNotes;
  const RunObservation own = observeRun(rom, 12u * kFrame, script, ownNotes);
  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir_, BufferedRunOptions{.memoryBytes = 1'000'000, .diskBytes = 3'000'000, .threads = 2}};
  std::uint64_t spent = 0;
  std::uint32_t frame = 0;
  std::uint32_t wraps = 0;  // the beam's wraps, counted here
  detail::presentPads(machine, script, frame);
  while (spent < 12u * kFrame) {
    const std::uint16_t line = machine.state().vpos;
    spent += machine.step();
    if (machine.state().vpos < line) ++wraps;
    if (run.frame() != frame) {
      frame = run.frame();
      detail::presentPads(machine, script, frame);
    }
  }
  EXPECT_TRUE(run.full());
  EXPECT_GT(wraps, 0u);
  EXPECT_EQ(run.frame(), wraps) << "the frames go on being counted";
  EXPECT_LE(run.progress().diskUsed, 3'000'000u) << "the files stay within their limit";
  std::vector<std::string> notes;
  const RunObservation seen = run.finish(notes);
  EXPECT_GT(seen.instructions, 0u);
  EXPECT_LT(seen.instructions, own.instructions) << "the run stopped watching at the bound";
  EXPECT_EQ(seen.divergences, 0u);
  EXPECT_FALSE(anyNoteHolds(notes, "disagreed"));
}

TEST_F(RomBufferedRun, AKeyframeTheKeptWorkRamDiffersFromIsCountedAndNotedOnce) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir_, withThreads(2)};
  std::uint64_t spent = 0;
  while (spent < 2u * kFrame) spent += machine.step();
  run.keyframe();
  // A byte of work RAM changed behind every observer's back.
  SnesState state = machine.state();
  state.wram[0x1F00u] = static_cast<std::uint8_t>(state.wram[0x1F00u] ^ 0xFFu);
  ASSERT_TRUE(machine.restore(state));
  run.keyframe();
  run.keyframe();
  std::vector<std::string> notes;
  static_cast<void>(run.finish(notes));
  EXPECT_EQ(run.progress().keyframesDiffered, 2u) << "the first matched, the two after the change did not";
  std::size_t said = 0;
  for (const std::string& note : notes) {
    if (note == "run: the kept work RAM differs from the machine's at frame " + std::to_string(run.frame())) ++said;
  }
  EXPECT_EQ(said, 1u);
}


TEST_F(RomBufferedRun, AFailureKeepsWhatWasPlayedBeforeIt) {
  // The CPU stream's directory is made a file once the run is bound, and the
  // buffer holds no RAM of its own, so the first block it moves to a file
  // fails. The threads play nothing more; what they played is answered, and
  // said to be all there is.
  const std::vector<std::uint8_t> rom = examples::movingImage();
  Snes machine{SnesConfig{.rom = rom}};
  BufferedRun run{machine, rom, dir_, BufferedRunOptions{.memoryBytes = 0, .diskBytes = 256'000'000, .threads = 1}};
  std::filesystem::remove_all(dir_ / "cpu");
  { std::ofstream(dir_ / "cpu") << "a file, not a directory"; }
  std::uint64_t spent = 0;
  while (spent < 60u * kFrame && run.failure().empty()) spent += machine.step();
  ASSERT_FALSE(run.failure().empty()) << "a block went to a file";
  EXPECT_NE(run.failure().find("could not write"), std::string::npos) << run.failure();
  std::vector<std::string> midNotes;
  const RunObservation mid = run.observation(midNotes);
  ASSERT_FALSE(midNotes.empty());
  EXPECT_EQ(midNotes.back(), "run: " + run.failure() + "; the observation holds what was played before it");
  const std::uint64_t failedAt = spent;
  while (spent < failedAt + 2u * kFrame) spent += machine.step();
  std::vector<std::string> notes;
  const RunObservation seen = run.finish(notes);
  ASSERT_FALSE(notes.empty());
  EXPECT_EQ(notes.back(), "run: " + run.failure() + "; the observation holds what was played before it");
  EXPECT_EQ(seen.instructions, mid.instructions) << "nothing played after the failure";
  EXPECT_EQ(seen.divergences, 0u);
  std::vector<std::string> ownNotes;
  const RunObservation own = observeRun(rom, spent, InputScript{}, ownNotes);
  EXPECT_LT(seen.instructions, own.instructions);
}

}  // namespace snaggletooth::disasm
