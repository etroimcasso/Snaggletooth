// The save observer: the machine saying that the cartridge's battery-backed save
// window changed during the frame it just finished, and handing over the window as
// it stands. It is what a host persists a save from — the core says WHEN and what
// the bytes are, and never learns where they go. The cases hold it to reporting
// once a frame however many stores landed, to saying nothing in a frame where none
// did, and to leaving a machine nobody is watching exactly as it was; and they hold
// it to the terms the machine's other three observers already keep — the host's
// object, not part of the state.

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Every report a run produced, each the whole window as it stood when the frame
// that changed it finished.
struct Saves final : SaveObserver {
  std::vector<std::vector<std::uint8_t>> reports;

  void changed(std::span<const std::uint8_t> save) override {
    reports.emplace_back(save.begin(), save.end());
  }
};

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kStaLong = 0x8Fu;  // STA addr,long — the save window is not in bank 0
constexpr std::uint8_t kIncA = 0x1Au;
constexpr std::uint8_t kBra = 0x80u;
constexpr std::uint8_t kStp = 0xDBu;

// A one-bank LoROM cartridge running `program` from $8000.
std::vector<std::uint8_t> cartridge(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// LoROM maps the save window at $70-$7D:0000-7FFF, so a store into it is a long
// one: $700000 is the window's first byte, $700001 its second.

// Two bytes into the window, then stop: the whole of what a frame changed is two
// known values at two known offsets.
std::vector<std::uint8_t> storesTwoBytes() {
  return cartridge({kLdaImm, 0x5Au, kStaLong, 0x00u, 0x00u, 0x70u,
                    kLdaImm, 0xA3u, kStaLong, 0x01u, 0x00u, 0x70u, kStp});
}

// Stores into the window forever, a different value each time round, so every
// frame of a run is a frame that changed it.
std::vector<std::uint8_t> storesForever() {
  return cartridge({kLdaImm, 0x01u,                      // $8000
                    kStaLong, 0x00u, 0x00u, 0x70u,       // $8002
                    kIncA,                               // $8006
                    kBra, 0xF9u});                       // $8007 -> $8002
}

// Everything but the save window: the program stops at once, so the beam runs and
// nothing stores.
std::vector<std::uint8_t> stopsAtOnce() { return cartridge({kStp}); }

// A frame is 262 lines of 1364 master cycles less the four its short line drops;
// past that is where a finished frame is handed over.
constexpr std::uint64_t kFrameMaster = 357364u;
constexpr std::uint64_t kOneFrame = kFrameMaster + 20000u;
constexpr std::uint64_t kTwoFrames = 2u * kFrameMaster + 20000u;

constexpr std::size_t kSaveBytes = 2048u;

Snes machineWithSave(const std::vector<std::uint8_t>& rom, std::size_t saveBytes = kSaveBytes) {
  return Snes(SnesConfig{.rom = rom, .saveRamBytes = saveBytes});
}

// ---- the machine nobody is watching ---------------------------------------------

TEST(SaveObserver, NoneIsSetAtPowerOn) {
  const std::vector<std::uint8_t> rom = storesTwoBytes();
  Snes machine = machineWithSave(rom);
  EXPECT_EQ(machine.saveObserver(), nullptr);
}

TEST(SaveObserver, AMachineWithNoObserverRunsExactlyAsBefore) {
  const std::vector<std::uint8_t> rom = storesForever();

  Snes watched = machineWithSave(rom);
  Saves saves;
  watched.setSaveObserver(&saves);
  watched.run(kTwoFrames);

  Snes unwatched = machineWithSave(rom);
  unwatched.run(kTwoFrames);

  const SnesState& a = watched.state();
  const SnesState& b = unwatched.state();
  EXPECT_EQ(a.sram, b.sram) << "the save window ran the same either way";
  EXPECT_EQ(a.wram, b.wram);
  EXPECT_EQ(a.cpu.pc, b.cpu.pc);
  EXPECT_EQ(a.cpu.a, b.cpu.a);
  EXPECT_EQ(a.cpu.p, b.cpu.p);
  EXPECT_FALSE(saves.reports.empty()) << "the watched run did report, so the comparison means something";
}

// ---- what it is told, and when --------------------------------------------------

TEST(SaveObserver, TheObserverIsToldOnceAfterAFrameThatStoredIntoTheWindow) {
  const std::vector<std::uint8_t> rom = storesTwoBytes();
  Snes machine = machineWithSave(rom);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kOneFrame);

  ASSERT_EQ(saves.reports.size(), 1u) << "two stores in one frame are one report";
  EXPECT_EQ(saves.reports.front()[0], 0x5Au);
  EXPECT_EQ(saves.reports.front()[1], 0xA3u) << "the window as it finally stood, not as the first store left it";
}

TEST(SaveObserver, ManyStoresInOneFrameAreStillOneReport) {
  const std::vector<std::uint8_t> rom = storesForever();
  Snes machine = machineWithSave(rom);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kOneFrame);

  EXPECT_EQ(saves.reports.size(), 1u);
}

// The report clears what raised it. Without this the machine would go on reporting
// a change that has already been told, and a host would write the same file every
// frame for the rest of the run — which the single-frame cases above cannot see.
TEST(SaveObserver, AFrameAFTERTheOneThatChangedItTellsItNothingMore) {
  const std::vector<std::uint8_t> rom = storesTwoBytes();  // stores twice, then stops
  Snes machine = machineWithSave(rom);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kTwoFrames);

  EXPECT_EQ(saves.reports.size(), 1u) << "the second frame stored nothing; the first was already told";
}

TEST(SaveObserver, AFrameWithNoStoreTellsItNothing) {
  const std::vector<std::uint8_t> rom = stopsAtOnce();
  Snes machine = machineWithSave(rom);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kTwoFrames);

  EXPECT_TRUE(saves.reports.empty()) << "the beam ran; the save window did not change";
}

TEST(SaveObserver, TheBytesAreTheWholeWindowAtTheDeclaredSize) {
  const std::vector<std::uint8_t> rom = storesTwoBytes();
  Snes machine = machineWithSave(rom, kSaveBytes);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kOneFrame);

  ASSERT_EQ(saves.reports.size(), 1u);
  EXPECT_EQ(saves.reports.front().size(), kSaveBytes) << "the whole window, not the bytes that changed";
  EXPECT_EQ(saves.reports.front(), machine.state().sram) << "and it is the window itself, byte for byte";
}

TEST(SaveObserver, ACartridgeWithoutSaveRamNeverTellsIt) {
  const std::vector<std::uint8_t> rom = storesForever();
  Snes machine = machineWithSave(rom, 0u);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kTwoFrames);

  EXPECT_TRUE(saves.reports.empty()) << "there is no window for the stores to land in";
}

// ---- the terms the other three observers keep -----------------------------------

TEST(SaveObserver, RestoreMarksItChanged) {
  const std::vector<std::uint8_t> rom = stopsAtOnce();
  Snes machine = machineWithSave(rom);
  Saves saves;
  machine.setSaveObserver(&saves);
  machine.run(kOneFrame);
  ASSERT_TRUE(saves.reports.empty()) << "a halted cartridge changed nothing";

  SnesState state = machine.state();
  state.sram.assign(kSaveBytes, 0x7Eu);
  machine.restore(state);
  machine.run(kOneFrame);

  ASSERT_EQ(saves.reports.size(), 1u) << "the caller replaced the window wholesale";
  EXPECT_EQ(saves.reports.front()[0], 0x7Eu);
}

TEST(SaveObserver, TheObserverIsNotPartOfTheState) {
  const std::vector<std::uint8_t> rom = storesForever();
  Snes watched = machineWithSave(rom);
  Saves saves;
  watched.setSaveObserver(&saves);
  watched.run(kOneFrame);

  // A state taken while one was set, restored into a machine that has none, does
  // not carry it — and restoring leaves whatever the machine had in place.
  Snes bare = machineWithSave(rom);
  bare.restore(watched.state());
  bare.run(kOneFrame);
  EXPECT_EQ(bare.saveObserver(), nullptr);

  EXPECT_EQ(watched.saveObserver(), &saves) << "restore leaves the machine's own in place";
  const std::size_t before = saves.reports.size();
  watched.restore(watched.state());
  watched.run(kOneFrame);
  EXPECT_GT(saves.reports.size(), before);
}

TEST(SaveObserver, ClearingItStopsTheCalls) {
  const std::vector<std::uint8_t> rom = storesForever();
  Snes machine = machineWithSave(rom);
  Saves saves;
  machine.setSaveObserver(&saves);

  machine.run(kOneFrame);
  ASSERT_EQ(saves.reports.size(), 1u);

  machine.setSaveObserver(nullptr);
  machine.run(kOneFrame);

  EXPECT_EQ(saves.reports.size(), 1u) << "nothing is told after it is cleared";
  EXPECT_EQ(machine.saveObserver(), nullptr);
  EXPECT_NE(machine.state().sram[0], 0x00u) << "and the machine went on storing into the window";
}

}  // namespace
}  // namespace snaggletooth
