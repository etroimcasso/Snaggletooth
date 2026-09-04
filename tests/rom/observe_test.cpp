// The run as oracle: the machine stepped through a cartridge whose dispatch runs
// through pointers, and the targets it took recorded as entries the trace then
// starts from. The cartridge is built by hand so every target is reachable only
// through one of the four indirect forms, and the cases pin what is recorded,
// what is not, and what the manifest and the trace do with it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"
#include "rom/rom_verify.h"

namespace snaggletooth::disasm {
namespace {

using examples::dispatchingImage;
using examples::loRomImage;
using examples::put;
using examples::stalledJumpImage;
using examples::unreadablePointerImage;

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

const ReachedTarget* reachedFrom(const std::vector<ReachedTarget>& seen, Address site) {
  for (const ReachedTarget& r : seen) {
    if (r.site == site) return &r;
  }
  return nullptr;
}

std::vector<ReachedTarget> run(std::span<const std::uint8_t> rom, std::uint64_t cycles,
                               std::vector<std::string>* notes = nullptr) {
  std::vector<std::string> local;
  return observeRun(rom, cycles, InputScript{}, notes ? *notes : local);
}

}  // namespace

// ---- what a run records ------------------------------------------------------

TEST(RomObserve, EachIndirectFormRecordsTheTargetItTook) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> seen = run(rom, 4u * kFrame);

  const ReachedTarget* table = reachedFrom(seen, 0x008232u);  // JMP (!$8100,X), X = 2
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->target, 0x008210u);
  EXPECT_FALSE(table->call);

  const ReachedTarget* call = reachedFrom(seen, 0x008212u);   // JSR (!$8100,X), X = 0
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->target, 0x008200u);
  EXPECT_TRUE(call->call);

  const ReachedTarget* plain = reachedFrom(seen, 0x008215u);  // JMP (!$8104)
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(plain->target, 0x008220u);

  const ReachedTarget* longJump = reachedFrom(seen, 0x008220u);  // JML [!$8106]
  ASSERT_NE(longJump, nullptr);
  EXPECT_EQ(longJump->target, 0x018000u) << "the third byte of the pointer is the bank";
}

TEST(RomObserve, TheModeRecordedIsTheOneTheJumpCarriesIn) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> seen = run(rom, 4u * kFrame);

  const ReachedTarget* narrow = reachedFrom(seen, 0x008215u);
  ASSERT_NE(narrow, nullptr);
  EXPECT_FALSE(narrow->mode.emulation);
  EXPECT_TRUE(narrow->mode.accumulator8);
  EXPECT_TRUE(narrow->mode.index8);

  const ReachedTarget* wide = reachedFrom(seen, 0x018025u);  // after REP #$10
  ASSERT_NE(wide, nullptr);
  EXPECT_TRUE(wide->mode.accumulator8);
  EXPECT_FALSE(wide->mode.index8) << "REP #$10 widened the index registers before the jump";
  EXPECT_TRUE(wide->mode.accumulatorKnown);
  EXPECT_TRUE(wide->mode.indexKnown);
}

// `JMP (!abs,X)` reads its pointer in the program bank; the datasheet's prose
// says bank zero and its diagram says the program bank, and the core reads the
// program bank. Bank $00 holds a table at the same offset naming another target.
TEST(RomObserve, AnIndexedIndirectPointerIsReadInTheProgramBank) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> seen = run(rom, 4u * kFrame);
  const ReachedTarget* banked = reachedFrom(seen, 0x018002u);
  ASSERT_NE(banked, nullptr);
  EXPECT_EQ(banked->target, 0x018020u) << "bank $00's table at $8100 would have said $01:8200";
  for (const ReachedTarget& r : seen) EXPECT_NE(r.target, 0x018200u);
}

TEST(RomObserve, AJumpTakenManyTimesIsOneSighting) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> seen = run(rom, 30u * kFrame);
  const std::size_t sightings = static_cast<std::size_t>(
      std::count_if(seen.begin(), seen.end(),
                    [](const ReachedTarget& r) { return r.site == 0x018025u; }));
  EXPECT_EQ(sightings, 1u) << "sixty-five thousand takes of one jump to one target";
  ASSERT_NE(reachedFrom(seen, 0x018025u), nullptr);
  EXPECT_EQ(reachedFrom(seen, 0x018025u)->target, 0x018030u);
}

// An NMI lands on the jump itself during the long loop. The step that services it
// goes to the handler, not the pointer's target, and records nothing; the jump runs
// after the handler returns and is seen then.
TEST(RomObserve, AnInterruptServicedInsteadOfTheJumpRecordsNothing) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  std::vector<std::string> notes;
  const std::vector<ReachedTarget> seen = run(rom, 30u * kFrame, &notes);
  for (const ReachedTarget& r : seen) {
    EXPECT_NE(r.target, 0x008300u) << "the NMI handler is a vector, never a reached target";
  }
  for (const std::string& note : notes) {
    EXPECT_EQ(note.find("but the CPU went to"), std::string::npos) << note;
  }
}

// A step on which the CPU is held off the bus runs no instruction and leaves the
// program counter where it was. That is not a wrong pointer; the jump is still
// ahead, and is recorded on the step that runs it.
TEST(RomObserve, AJumpBehindADmaTransferIsRecordedWhenItRuns) {
  const std::vector<std::uint8_t> rom = stalledJumpImage();
  std::vector<std::string> notes;
  const std::vector<ReachedTarget> seen = run(rom, kFrame, &notes);
  const ReachedTarget* stalled = reachedFrom(seen, 0x008026u);
  ASSERT_NE(stalled, nullptr);
  EXPECT_EQ(stalled->target, 0x008220u);
  EXPECT_TRUE(notes.empty()) << notes.front();
}

TEST(RomObserve, ASiteInWorkRamIsDecodedFromWorkRam) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> seen = run(rom, 4u * kFrame);
  const ReachedTarget* copied = reachedFrom(seen, 0x7E2000u);  // the JML [!$8109] copied into RAM
  ASSERT_NE(copied, nullptr);
  EXPECT_EQ(copied->target, 0x008230u) << "the pointer's third byte is the bank it lands in";
}

TEST(RomObserve, APointerTheRunCannotReadIsNotedNotGuessed) {
  const std::vector<std::uint8_t> rom = unreadablePointerImage();
  std::vector<std::string> notes;
  const std::vector<ReachedTarget> seen = run(rom, kFrame, &notes);
  EXPECT_EQ(reachedFrom(seen, 0x008000u), nullptr);
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_NE(notes[0].find("$00:8000"), std::string::npos) << notes[0];
  EXPECT_NE(notes[0].find("cannot see"), std::string::npos) << notes[0];
}

TEST(RomObserve, TheBudgetBoundsTheRun) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  // A budget that ends before the copy into work RAM has been made: only what
  // ran within it is seen.
  const std::vector<ReachedTarget> seen = run(rom, 64u);
  EXPECT_TRUE(seen.empty());
}

TEST(RomObserve, TwoRunsSeeTheSameThing) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> first = run(rom, 30u * kFrame);
  const std::vector<ReachedTarget> second = run(rom, 30u * kFrame);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_TRUE(sameSighting(first[i], second[i])) << i;
  }
  EXPECT_FALSE(first.empty());
}

TEST(RomObserve, SightingsAreOrderedBySiteThenTarget) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const std::vector<ReachedTarget> seen = run(rom, 4u * kFrame);
  for (std::size_t i = 1; i < seen.size(); ++i) {
    const bool ordered = seen[i - 1].site < seen[i].site ||
                         (seen[i - 1].site == seen[i].site && seen[i - 1].target <= seen[i].target);
    EXPECT_TRUE(ordered) << i;
  }
}

// ---- the disassembly, the manifest, and the trace ------------------------------

TEST(RomObserve, TheTraceStartsFromWhatTheRunReached) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = false;
  const CartridgeDisassembly without = disassembleCartridge(request);
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly with = disassembleCartridge(request);

  const auto instructions = [](const CartridgeDisassembly& d, const std::string& file) {
    std::size_t n = 0;
    for (const RegionListing& region : d.regions) {
      if (region.region.file != file) continue;
      for (const Line& line : region.listing.lines) n += line.isCode ? 1u : 0u;
    }
    return n;
  };
  EXPECT_EQ(instructions(without, "bank_01.asm"), 0u) << "the trace alone cannot enter bank $01";
  EXPECT_GT(instructions(with, "bank_01.asm"), 0u) << "the run's JML target leads the trace in";
  EXPECT_GT(instructions(with, "bank_00.asm"), instructions(without, "bank_00.asm"));
  EXPECT_FALSE(with.reached.empty());
}

TEST(RomObserve, AReachedTargetIsLabelledByTheFormThatTookIt) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  std::map<Address, std::string> names;
  for (const ReachedTarget& r : d.reached) names[r.target] = r.name;
  EXPECT_EQ(names[0x008200u], "sub_008200") << "a call's target is a routine";
  EXPECT_EQ(names[0x008220u], "loc_008220") << "a jump's target is a location";
  EXPECT_EQ(names[0x018000u], "loc_018000");
}

TEST(RomObserve, APersonsEntryKeepsItsNameOverTheRuns) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  request.entries = {TraceEntry{.address = 0x008220u,
                                .mode = Cpu65816Mode::native(true, true),
                                .name = "dispatch"}};
  const CartridgeDisassembly d = disassembleCartridge(request);
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("entry    $00:8220 dispatch e=0 m=8 x=8\n"), std::string::npos) << manifest;
  EXPECT_NE(manifest.find("reached  $00:8220 dispatch e=0 m=8 x=8 from $00:8215\n"),
            std::string::npos)
      << "the run still saw it, under the person's name";
  EXPECT_EQ(manifest.find("loc_008220"), std::string::npos);
}

TEST(RomObserve, TheManifestCarriesTheSightingsAndReadsThemBack) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("reached  $00:8200 sub_008200 e=0 m=8 x=8 from $00:8212\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("reached  $01:8000 loc_018000 e=0 m=8 x=8 from $00:8220\n"),
            std::string::npos)
      << manifest;

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->reached.size(), d.reached.size());
  for (std::size_t i = 0; i < d.reached.size(); ++i) {
    EXPECT_TRUE(sameSighting(input->reached[i], d.reached[i])) << i;
    EXPECT_EQ(input->reached[i].name, d.reached[i].name);
    EXPECT_EQ(input->reached[i].call, d.reached[i].call);
  }

  EXPECT_FALSE(parseManifest("reached $00:8200 sub_008200 e=0 m=8 x=8 at $00:8212\n", error)
                   .has_value());
  EXPECT_NE(error.find("`from`"), std::string::npos);
  EXPECT_FALSE(parseManifest("reached $00:8200 sub_008200 e=0 m=8 from $00:8212\n", error)
                   .has_value());
}

// What an earlier run saw is traced from again without running: the manifest is
// the run's memory.
TEST(RomObserve, AnEarlierRunsSightingsAreTracedFromWithoutRunning) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest first;
  first.rom = rom;
  first.captureSound = false;
  first.observeRun = true;
  first.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly ran = disassembleCartridge(first);

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  CartridgeRequest again;
  again.rom = rom;
  again.captureSound = false;
  again.observeRun = false;
  again.reached = input->reached;
  const CartridgeDisassembly replayed = disassembleCartridge(again);
  ASSERT_EQ(replayed.reached.size(), ran.reached.size());
  EXPECT_EQ(renderManifest(replayed), renderManifest(ran));
}

// A manifest's sightings and a new run's are one set: what both saw is written
// once.
TEST(RomObserve, AnEarlierRunsSightingsMergeWithThisOnes) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest first;
  first.rom = rom;
  first.captureSound = false;
  first.observeRun = true;
  first.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly ran = disassembleCartridge(first);

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  CartridgeRequest again = first;
  again.reached = input->reached;
  const CartridgeDisassembly merged = disassembleCartridge(again);
  EXPECT_EQ(merged.reached.size(), ran.reached.size());
  EXPECT_EQ(renderManifest(merged), renderManifest(ran));
}

TEST(RomObserve, ATargetOutsideTheImageIsNotedNotTraced) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = false;
  request.reached = {ReachedTarget{.target = 0x7E2000u,
                                   .mode = Cpu65816Mode::native(true, true),
                                   .site = 0x00801Bu,
                                   .call = false,
                                   .name = {}}};
  const CartridgeDisassembly d = disassembleCartridge(request);
  EXPECT_TRUE(d.reached.empty());
  bool noted = false;
  for (const std::string& note : d.notes) noted = noted || note.find("$7E:2000") != std::string::npos;
  EXPECT_TRUE(noted);
}

TEST(RomObserve, TheTreeStillAssemblesToItsImage) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);

  std::map<std::string, std::string> tree;
  for (const RegionListing& region : d.regions) tree[region.region.file] = renderRegion(region, d);
  std::string error;
  const std::optional<ManifestInput> manifest = parseManifest(renderManifest(d), error);
  ASSERT_TRUE(manifest.has_value()) << error;
  const VerifyReport report = verifyProject(*manifest, rom, [&tree](const std::string& file) {
    const auto found = tree.find(file);
    if (found == tree.end()) return std::optional<std::string>{};
    return std::optional<std::string>{found->second};
  });
  EXPECT_TRUE(report.error.empty()) << report.error;
  EXPECT_TRUE(report.identical()) << renderReport(report);
}

}  // namespace snaggletooth::disasm
