// The run as oracle: the machine stepped through a cartridge whose dispatch runs
// through pointers, and the targets it took recorded as entries the trace then
// starts from. The cartridge is built by hand so every target is reachable only
// through one of the four indirect forms, and the cases pin what is recorded,
// what is not, and what the manifest and the trace do with it.
//
// The same run watches the transfer engines. A second cartridge moves bytes
// every way they can, and the cases pin each field of what is recorded, how a
// range is closed and counted, and what the manifest does with it.
//
// The same run lifts every instruction the CPU executes from its fetches and
// holds it to the machine. A third cartridge rewrites a routine in work RAM and
// returns to addresses the bytes do not name, and the cases pin what is
// checked, which landings are recorded and which are not, the values seen at a
// site, and that no example cartridge diverges.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_differential.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"
#include "rom/rom_verify.h"

namespace snaggletooth::disasm {
namespace {

using examples::copVectorImage;
using examples::dispatchingImage;
using examples::loRomImage;
using examples::mirroredImage;
using examples::mixedImage;
using examples::movingImage;
using examples::provingImage;
using examples::put;
using examples::ramCodeImage;
using examples::runningBankImage;
using examples::stalledJumpImage;
using examples::threeBankImage;
using examples::unreadablePointerImage;

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

// The manifest without its `seen` block — the last block, written fresh by a
// run and not at all by a disassembly without one — so what the manifest keeps
// from one to the next is compared without it.
std::string withoutSeen(const std::string& manifest) {
  std::string out;
  std::size_t position = 0;
  while (position < manifest.size()) {
    const std::size_t end = manifest.find('\n', position);
    const std::string line =
        manifest.substr(position, end == std::string::npos ? std::string::npos : end - position + 1);
    position = end == std::string::npos ? manifest.size() : end + 1;
    if (line.rfind("seen ", 0) != 0) out += line;
  }
  while (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0) out.pop_back();
  return out;
}

const ReachedTarget* reachedFrom(const std::vector<ReachedTarget>& seen, Address site) {
  for (const ReachedTarget& r : seen) {
    if (r.site == site) return &r;
  }
  return nullptr;
}

std::vector<ReachedTarget> run(std::span<const std::uint8_t> rom, std::uint64_t cycles,
                               std::vector<std::string>* notes = nullptr) {
  std::vector<std::string> local;
  return observeRun(rom, cycles, InputScript{}, notes ? *notes : local).reached;
}

std::vector<MovedRange> moved(std::span<const std::uint8_t> rom, std::uint64_t cycles,
                              std::vector<std::string>* notes = nullptr) {
  std::vector<std::string> local;
  return observeRun(rom, cycles, InputScript{}, notes ? *notes : local).moved;
}

// The ranges recorded under one trigger for one channel, in the order recorded.
std::vector<const MovedRange*> rangesAt(const std::vector<MovedRange>& seen, Address site,
                                        std::uint8_t channel) {
  std::vector<const MovedRange*> out;
  for (const MovedRange& r : seen) {
    if (r.site == site && r.channel == channel) out.push_back(&r);
  }
  return out;
}

// The one range recorded under a trigger for a channel at a memory address.
const MovedRange* rangeAt(const std::vector<MovedRange>& seen, Address site, std::uint8_t channel,
                          Address memory) {
  for (const MovedRange& r : seen) {
    if (r.site == site && r.channel == channel && r.memory == memory) return &r;
  }
  return nullptr;
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
  EXPECT_EQ(withoutSeen(renderManifest(replayed)), withoutSeen(renderManifest(ran)));
  EXPECT_TRUE(replayed.seen.empty()) << "what a run saw is the run's, not read back";
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
  for (const AssetFile& asset : d.assets) tree[asset.file] = std::string(asset.bytes.begin(), asset.bytes.end());
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

// ---- what a run moved ----------------------------------------------------------

// The moving cartridge's sites: the writes to `MDMAEN` and `HDMAEN`.
constexpr Address kTileset = 0x008025u;   // channel 0, 32 bytes from $9000 to VMDATAL
constexpr Address kFill = 0x008043u;      // channel 0, 64 bytes from the one byte at $9100
constexpr Address kReadBack = 0x00806Bu;  // channel 1, 16 bytes of VRAM into $7E:0300
constexpr Address kPair = 0x0080B6u;      // channels 2 and 3 under one mask
constexpr Address kZeroMask = 0x0080BBu;  // a write of zero
constexpr Address kTables = 0x0080F7u;    // HDMA channels 4 and 5
constexpr Address kRamTable = 0x008128u;  // HDMA channel 6, its table in work RAM
constexpr Address kSprites = 0x008326u;   // channel 7 from the handler, every frame
constexpr Address kChunks = 0x008190u;    // channel 0 twice from one instruction, the chunks adjacent
constexpr Address kMirror = 0x008183u;    // channel 0 once more, MDMAEN written through bank $80

TEST(RomMoved, ATransferIsRecordedFromTheInstructionThatStartedIt) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const MovedRange* tiles = rangeAt(seen, kTileset, 0, 0x009000u);
  ASSERT_NE(tiles, nullptr);
  EXPECT_TRUE(tiles->toRegister);
  EXPECT_EQ(tiles->registerAddress, 0x002118u);
  EXPECT_EQ(tiles->registerName, "VMDATAL");
  ASSERT_TRUE(tiles->registerClass.has_value());
  EXPECT_EQ(*tiles->registerClass, RegisterClass::Vram);
  EXPECT_EQ(tiles->step, MovedStep::Increment);
  EXPECT_EQ(tiles->bytes, 32u);
  EXPECT_EQ(tiles->kind, MovedKind::Dma);
  EXPECT_EQ(tiles->times, 1u);
}

TEST(RomMoved, AFixedSourceIsAFillFromOneByteNotARange) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const std::vector<const MovedRange*> fill = rangesAt(seen, kFill, 0);
  ASSERT_EQ(fill.size(), 1u);
  EXPECT_EQ(fill[0]->memory, 0x009100u);
  EXPECT_EQ(fill[0]->step, MovedStep::Fixed);
  EXPECT_EQ(fill[0]->bytes, 64u) << "sixty-four bytes moved, all from one address";
  EXPECT_EQ(fill[0]->registerName, "VMDATAL");
}

TEST(RomMoved, AReadBackFromARegisterIsFromRegisterAndLandsInMemory) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const MovedRange* read = rangeAt(seen, kReadBack, 1, 0x7E0300u);
  ASSERT_NE(read, nullptr);
  EXPECT_FALSE(read->toRegister);
  EXPECT_EQ(read->registerAddress, 0x002139u);
  EXPECT_EQ(read->registerName, "VMDATALREAD");
  EXPECT_EQ(read->bytes, 16u);
  EXPECT_EQ(read->step, MovedStep::Increment);
}

TEST(RomMoved, ADecrementingTransferBeginsAtItsHighestByte) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const MovedRange* palette = rangeAt(seen, kPair, 2, 0x00920Fu);
  ASSERT_NE(palette, nullptr);
  EXPECT_EQ(palette->step, MovedStep::Decrement);
  EXPECT_EQ(palette->bytes, 16u);
  EXPECT_EQ(palette->registerName, "CGDATA");
  ASSERT_TRUE(palette->registerClass.has_value());
  EXPECT_EQ(*palette->registerClass, RegisterClass::Cgram);
}

TEST(RomMoved, ABBusAddressNoRegisterHasIsRecordedWithoutAName) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const MovedRange* nameless = rangeAt(seen, kPair, 3, 0x009400u);
  ASSERT_NE(nameless, nullptr);
  EXPECT_EQ(nameless->registerAddress, 0x002150u);
  EXPECT_TRUE(nameless->registerName.empty());
  EXPECT_FALSE(nameless->registerClass.has_value());
  EXPECT_EQ(nameless->bytes, 8u);
}

TEST(RomMoved, TwoChannelsUnderOneMaskAreTwoRangesAtOneSite) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  std::size_t atSite = 0;
  for (const MovedRange& r : seen) atSite += r.site == kPair ? 1u : 0u;
  EXPECT_EQ(atSite, 2u);
  EXPECT_EQ(rangesAt(seen, kPair, 2).size(), 1u);
  EXPECT_EQ(rangesAt(seen, kPair, 3).size(), 1u);
}

TEST(RomMoved, AZeroMaskStartsNothing) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  for (const MovedRange& r : seen) EXPECT_NE(r.site, kZeroMask);
}

TEST(RomMoved, AnHdmaTableIsRecordedAsTheFrameWalkedIt) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 3u * kFrame);
  const MovedRange* table = rangeAt(seen, kTables, 4, 0x009500u);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->kind, MovedKind::Table);
  EXPECT_EQ(table->bytes, 5u) << "two entries, two values and the terminator";
  EXPECT_EQ(table->step, MovedStep::Increment);
  EXPECT_TRUE(table->toRegister);
  EXPECT_EQ(table->registerName, "INIDISP");
  EXPECT_GE(table->times, 1u);
}

TEST(RomMoved, AnIndirectTableAndTheBlocksItPointsAtAreSeparateRanges) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 3u * kFrame);
  const MovedRange* table = rangeAt(seen, kTables, 5, 0x009510u);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->kind, MovedKind::Table);
  EXPECT_EQ(table->bytes, 7u) << "two entries with their pointers, and the terminator";
  EXPECT_EQ(table->registerName, "CGADD");
  // The first entry's block is at $9522 and the second's at $9520, so a frame's
  // last block ends exactly where the next frame's first begins; a new frame is
  // a new walk, and the two stay two.
  const MovedRange* first = rangeAt(seen, kTables, 5, 0x009522u);
  const MovedRange* second = rangeAt(seen, kTables, 5, 0x009520u);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->kind, MovedKind::Indirect);
  EXPECT_EQ(first->bytes, 2u);
  EXPECT_EQ(second->kind, MovedKind::Indirect);
  EXPECT_EQ(second->bytes, 2u) << "not joined to the next frame's first block";
  EXPECT_EQ(first->registerName, "CGADD") << "a block is named by the register its channel reaches";
  for (const MovedRange* r : rangesAt(seen, kTables, 5)) {
    if (r->kind == MovedKind::Indirect) {
      EXPECT_EQ(r->bytes, 2u) << "no block joined to another frame's";
    }
  }
}

TEST(RomMoved, TheSameInstructionStartingTheChannelAgainBeginsANewRange) {
  // Two sixteen-byte chunks, the second from where the first ended, both
  // started by the one `STA !MDMAEN` in a subroutine: two ranges, not one.
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const std::vector<const MovedRange*> chunks = rangesAt(seen, kChunks, 0);
  ASSERT_EQ(chunks.size(), 2u);
  EXPECT_EQ(chunks[0]->memory, 0x009600u);
  EXPECT_EQ(chunks[0]->bytes, 16u);
  EXPECT_EQ(chunks[0]->times, 1u);
  EXPECT_EQ(chunks[1]->memory, 0x009610u);
  EXPECT_EQ(chunks[1]->bytes, 16u);
  EXPECT_EQ(chunks[1]->times, 1u);
}

TEST(RomMoved, AWriteToTheStartRegisterThroughAMirrorBankIsATrigger) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame);
  const MovedRange* third = rangeAt(seen, kMirror, 0, 0x009620u);
  ASSERT_NE(third, nullptr) << "the store to $80:420B started it";
  EXPECT_EQ(third->bytes, 16u);
  EXPECT_EQ(third->registerName, "VMDATAL");
}

TEST(RomMoved, ATableInWorkRamIsRecordedWithNoNote) {
  const std::vector<std::uint8_t> rom = movingImage();
  std::vector<std::string> notes;
  const std::vector<MovedRange> seen = moved(rom, 3u * kFrame, &notes);
  EXPECT_TRUE(notes.empty()) << notes.front();
  const MovedRange* table = rangeAt(seen, kRamTable, 6, 0x7E0400u);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->kind, MovedKind::Table);
  EXPECT_EQ(table->bytes, 129u) << "the entry, 127 values and the terminator";
}

TEST(RomMoved, TheHdmaSiteIsTheWriteThatEnabledTheChannel) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 3u * kFrame);
  // The second write to HDMAEN names channels 4 and 5 again; they keep the
  // write that enabled them, and channel 6 takes the second.
  EXPECT_NE(rangeAt(seen, kTables, 4, 0x009500u), nullptr);
  EXPECT_NE(rangeAt(seen, kTables, 5, 0x009510u), nullptr);
  EXPECT_NE(rangeAt(seen, kRamTable, 6, 0x7E0400u), nullptr);
  EXPECT_TRUE(rangesAt(seen, kRamTable, 4).empty());
  EXPECT_TRUE(rangesAt(seen, kRamTable, 5).empty());
}

TEST(RomMoved, ARangeSeenEveryFrameIsCountedNotRepeated) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 5u * kFrame);
  const std::vector<const MovedRange*> sprites = rangesAt(seen, kSprites, 7);
  ASSERT_EQ(sprites.size(), 1u);
  EXPECT_EQ(sprites[0]->memory, 0x7E0200u);
  EXPECT_EQ(sprites[0]->bytes, 544u);
  EXPECT_EQ(sprites[0]->registerName, "OAMDATA");
  EXPECT_EQ(sprites[0]->kind, MovedKind::Dma);
  EXPECT_GE(sprites[0]->times, 3u) << "once per vertical blank over five frames";
}

TEST(RomMoved, ARunTheBudgetCutsRecordsWhatMoved) {
  // Channel 6's table takes 127 lines to walk. A run that ends part-way down a
  // frame leaves the walk unfinished, and the bytes it did read are a range.
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame + 60u * 1364u);
  const std::vector<const MovedRange*> walks = rangesAt(seen, kRamTable, 6);
  ASSERT_EQ(walks.size(), 2u) << "one whole walk, one the budget cut";
  EXPECT_EQ(walks[0]->bytes, 129u);
  EXPECT_LT(walks[1]->bytes, 129u);
  EXPECT_GT(walks[1]->bytes, 1u);
  EXPECT_EQ(walks[1]->times, 1u);
}

TEST(RomMoved, TwoRunsSeeTheSameRanges) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> first = moved(rom, 4u * kFrame);
  const std::vector<MovedRange> second = moved(rom, 4u * kFrame);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_TRUE(sameRange(first[i], second[i])) << i;
    EXPECT_EQ(first[i].times, second[i].times) << i;
  }
}

TEST(RomMoved, RangesAreOrderedBySiteChannelMemoryKindThenBytes) {
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 3u * kFrame);
  ASSERT_GE(seen.size(), 10u);
  for (std::size_t i = 1; i < seen.size(); ++i) {
    EXPECT_TRUE(rangeBefore(seen[i - 1], seen[i])) << i;
    EXPECT_FALSE(rangeBefore(seen[i], seen[i - 1])) << i;
  }
  // The two blocks follow their table by memory address: the table at $9510,
  // then $9520, then $9524 — a walk the run's end cut short sits beside the
  // whole one, so the addresses are read as a sequence with repeats.
  std::vector<Address> addresses;
  for (const MovedRange* r : rangesAt(seen, kTables, 5)) {
    if (addresses.empty() || addresses.back() != r->memory) addresses.push_back(r->memory);
  }
  EXPECT_EQ(addresses, (std::vector<Address>{0x009510u, 0x009520u, 0x009522u}));
}

TEST(RomMoved, TheManifestCarriesTheRangesAndReadsThemBack) {
  const std::vector<std::uint8_t> rom = movingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 3u * kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("moved    $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                          "increment bytes 32 as dma times 1\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("moved    $00:8043 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9100 "
                          "fixed bytes 64 as dma times 1\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("moved    $00:806B channel 1 from-register $00:2139 VMDATALREAD Vram memory "
                          "$7E:0300 increment bytes 16 as dma times 1\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("moved    $00:80B6 channel 2 to-register $00:2122 CGDATA Cgram memory $00:920F "
                          "decrement bytes 16 as dma times 1\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("moved    $00:80B6 channel 3 to-register $00:2150 none none memory $00:9400 "
                          "increment bytes 8 as dma times 1\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("moved    $00:80F7 channel 5 to-register $00:2121 CGADD Cgram memory $00:9520 "
                          "increment bytes 2 as indirect times "),
            std::string::npos)
      << manifest;

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->moved.size(), d.moved.size());
  for (std::size_t i = 0; i < d.moved.size(); ++i) {
    EXPECT_TRUE(sameRange(input->moved[i], d.moved[i])) << i;
    EXPECT_EQ(input->moved[i].times, d.moved[i].times) << i;
    EXPECT_EQ(input->moved[i].registerName, d.moved[i].registerName) << i;
    EXPECT_EQ(input->moved[i].registerClass, d.moved[i].registerClass) << i;
  }

  const std::string good =
      "moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 increment bytes 32 "
      "as dma times 1\n";
  EXPECT_TRUE(parseManifest(good, error).has_value()) << error;
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment bytes 32 dma times 1\n",
                             error)
                   .has_value());
  EXPECT_NE(error.find("`as`"), std::string::npos);
  // Every keyword is checked in its place, not only counted.
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment bytes 32 at dma times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram source $00:9000 "
                             "increment bytes 32 as dma times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment byte 32 as dma times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment bytes 32 as dma time 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 chan 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment bytes 32 as dma times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 9 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment bytes 32 as dma times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "sideways bytes 32 as dma times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moved $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 "
                             "increment bytes 32 as block times 1\n",
                             error)
                   .has_value());
  EXPECT_FALSE(parseManifest("moves $00:8025 channel 0\n", error).has_value());
}

// What an earlier run saw move is kept without running: the manifest is the
// run's memory, and the asset pass reads it from a tree that was not re-run.
TEST(RomMoved, AnEarlierRunsRangesAreKeptWithoutRunning) {
  const std::vector<std::uint8_t> rom = movingImage();
  CartridgeRequest first;
  first.rom = rom;
  first.captureSound = false;
  first.observeRun = true;
  first.runMasterCycles = 3u * kFrame;
  const CartridgeDisassembly ran = disassembleCartridge(first);
  ASSERT_FALSE(ran.moved.empty());

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  CartridgeRequest again;
  again.rom = rom;
  again.captureSound = false;
  again.observeRun = false;
  again.moved = input->moved;
  const CartridgeDisassembly kept = disassembleCartridge(again);
  ASSERT_EQ(kept.moved.size(), ran.moved.size());
  EXPECT_EQ(withoutSeen(renderManifest(kept)), withoutSeen(renderManifest(ran)));
}

// A manifest's ranges and a new run's are one set: a range this run saw again
// carries this run's count, and one it did not see is kept as it was.
TEST(RomMoved, ThisRunsCountReplacesAnEarlierOnesAndAnUnseenRangeIsKept) {
  const std::vector<std::uint8_t> rom = movingImage();
  CartridgeRequest first;
  first.rom = rom;
  first.captureSound = false;
  first.observeRun = true;
  first.runMasterCycles = 3u * kFrame;
  const CartridgeDisassembly ran = disassembleCartridge(first);

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  CartridgeRequest again = first;
  again.moved = input->moved;
  for (MovedRange& r : again.moved) r.times += 100u;  // an earlier run's counts
  again.moved.push_back(MovedRange{.site = 0x00FFF0u,
                                   .channel = 7,
                                   .toRegister = true,
                                   .registerAddress = 0x002104u,
                                   .registerName = "OAMDATA",
                                   .registerClass = RegisterClass::Oam,
                                   .memory = 0x7E1000u,
                                   .step = MovedStep::Increment,
                                   .bytes = 8,
                                   .kind = MovedKind::Dma,
                                   .times = 2});
  const CartridgeDisassembly merged = disassembleCartridge(again);
  ASSERT_EQ(merged.moved.size(), ran.moved.size() + 1u);
  for (const MovedRange& r : ran.moved) {
    const auto kept = std::find_if(merged.moved.begin(), merged.moved.end(),
                                   [&](const MovedRange& m) { return sameRange(m, r); });
    ASSERT_NE(kept, merged.moved.end());
    EXPECT_EQ(kept->times, r.times) << "this run's count, not the earlier one's";
  }
  const MovedRange* unseen = rangeAt(merged.moved, 0x00FFF0u, 7, 0x7E1000u);
  ASSERT_NE(unseen, nullptr);
  EXPECT_EQ(unseen->times, 2u);
}

TEST(RomMoved, TheTreeStillAssemblesToItsImage) {
  const std::vector<std::uint8_t> rom = movingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 3u * kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_FALSE(d.moved.empty());

  std::map<std::string, std::string> tree;
  for (const RegionListing& region : d.regions) tree[region.region.file] = renderRegion(region, d);
  for (const AssetFile& asset : d.assets) tree[asset.file] = std::string(asset.bytes.begin(), asset.bytes.end());
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

// ---- the run beside the interpreter --------------------------------------------

namespace {

RunObservation observe(std::span<const std::uint8_t> rom, std::uint64_t cycles,
                       std::vector<std::string>* notes = nullptr) {
  std::vector<std::string> local;
  return observeRun(rom, cycles, InputScript{}, notes ? *notes : local);
}

const Landing* landingFrom(const std::vector<Landing>& ran, Address site) {
  for (const Landing& l : ran) {
    if (l.site == site) return &l;
  }
  return nullptr;
}

const SeenState* seenAt(const std::vector<SeenState>& seen, Address address) {
  for (const SeenState& s : seen) {
    if (s.address == address) return &s;
  }
  return nullptr;
}

}  // namespace

// The run's nodes are checked by the machine exactly as the tree's are by the
// differential: the same steps, the same interrupts, no disagreement in either.
TEST(RomLockstep, EveryInstructionIsLiftedFromItsFetchesAndCheckedAgainstTheMachine) {
  const std::vector<std::uint8_t> rom = mixedImage();
  std::vector<std::string> notes;
  const RunObservation o = observe(rom, 5u * kFrame, &notes);
  EXPECT_EQ(o.divergences, 0u);
  EXPECT_TRUE(notes.empty()) << notes.front();
  EXPECT_EQ(o.interrupts, 3u);
  // Thirty-three instructions in the image; the emulation handler's RTI never runs.
  EXPECT_EQ(o.nodes, 33u);

  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = false;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ir::Program program;
  for (const RegionListing& region : d.regions) {
    std::vector<std::uint8_t> image;
    for (const Line& line : region.listing.lines) {
      const std::vector<std::uint8_t>& bytes = line.isCode ? line.instruction.bytes : line.data;
      image.insert(image.end(), bytes.begin(), bytes.end());
    }
    program = ir::lift65816(region.listing, image, region.region.first);
  }
  ir::Replay replay;
  replay.rom = rom;
  replay.masterCycles = 5u * kFrame;
  const ir::DifferentialReport report = ir::differential(program, replay);
  EXPECT_TRUE(report.divergences.empty());
  EXPECT_EQ(o.instructions, report.instructions) << "every instruction the tree's replay checked";
  EXPECT_EQ(o.interrupts, report.interrupts);
}

TEST(RomLockstep, BytesTheProgramRewroteAtAnAddressAreASecondNode) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  const RunObservation o = observe(rom, kFrame);
  EXPECT_EQ(o.divergences, 0u);
  // Thirty-nine instructions in the image; in work RAM `INC A`, then `DEC A` at
  // the same address, and the `RTL` after both.
  EXPECT_EQ(o.nodes, 42u);
  // The image's thirty-nine, the loop's eleven once more, and four in work RAM.
  EXPECT_EQ(o.instructions, 54u);
}

TEST(RomLockstep, AReturnToAnAddressTheCodePutOnTheStackIsALanding) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  const RunObservation o = observe(rom, kFrame);
  ASSERT_EQ(o.ran.size(), 3u);
  const Landing* fromRam = landingFrom(o.ran, 0x7E2001u);  // the routine's RTL
  ASSERT_NE(fromRam, nullptr);
  EXPECT_EQ(fromRam->target, 0x00802Cu) << "the frame the program built, not the JSL's";
  const Landing* pushed = landingFrom(o.ran, 0x008039u);  // PEA $8039 ; RTS, on both passes
  ASSERT_NE(pushed, nullptr);
  EXPECT_EQ(pushed->target, 0x00803Au);
  const Landing* pulled = landingFrom(o.ran, 0x00804Cu);  // RTI into a frame built by hand
  ASSERT_NE(pulled, nullptr);
  EXPECT_EQ(pulled->target, 0x00804Du);
  EXPECT_TRUE(o.ran[0].site < o.ran[1].site && o.ran[1].site < o.ran[2].site) << "in site order";
}

TEST(RomLockstep, ALandingTakenTwiceIsRecordedOnce) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  const RunObservation o = observe(rom, kFrame);
  std::size_t fromTheLoop = 0;
  for (const Landing& l : o.ran) fromTheLoop += l.site == 0x008039u ? 1u : 0u;
  EXPECT_EQ(fromTheLoop, 1u) << "the RTS ran on both passes of the loop";
}

TEST(RomLockstep, ALandingInWorkRamIsANoteNotALanding) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  std::vector<std::string> notes;
  const RunObservation o = observe(rom, kFrame, &notes);
  for (const Landing& l : o.ran) EXPECT_NE(l.target, 0x7E2000u);
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_NE(notes[0].find("$7E:2000"), std::string::npos) << notes[0];
  EXPECT_NE(notes[0].find("$00:802B"), std::string::npos) << notes[0];
  EXPECT_NE(notes[0].find("does not hold"), std::string::npos) << notes[0];
}

TEST(RomLockstep, AnIndirectJumpIsReachedAndNotALanding) {
  const std::vector<std::uint8_t> rom = dispatchingImage();
  const RunObservation o = observe(rom, 4u * kFrame);
  EXPECT_FALSE(o.reached.empty());
  EXPECT_TRUE(o.ran.empty()) << "the pointer was read ahead; the landing confirmed it";
  EXPECT_EQ(o.divergences, 0u);
}

// A fall-through, a branch, a call and its return, a hardware interrupt and its
// RTI, a software interrupt to a vector in the image and its RTI: every one
// lands where the run expects.
TEST(RomLockstep, FlowTheInstructionsNameLandsNowhereNew) {
  const RunObservation mixed = observe(mixedImage(), 5u * kFrame);
  EXPECT_EQ(mixed.interrupts, 3u);
  EXPECT_TRUE(mixed.ran.empty());
  const RunObservation threeBank = observe(threeBankImage(), 2u * kFrame);
  EXPECT_TRUE(threeBank.ran.empty());
  EXPECT_EQ(threeBank.divergences, 0u);
  // A COP in emulation mode and a BRK in native mode, each to a handler the
  // header names in the image, each returning after its signature byte.
  const RunObservation vectors = observe(copVectorImage(), kFrame);
  EXPECT_TRUE(vectors.ran.empty()) << "a software interrupt reaches the vector the header names";
  EXPECT_EQ(vectors.instructions, 7u) << "COP, RTI, CLC, XCE, BRK, RTI, STP";
  EXPECT_EQ(vectors.interrupts, 0u) << "a software interrupt is an instruction, not a hardware sequence";
  EXPECT_EQ(vectors.divergences, 0u);
  const RunObservation proving = observe(provingImage(), 2u * kFrame);
  EXPECT_TRUE(proving.ran.empty());
  EXPECT_EQ(proving.divergences, 0u);
}

TEST(RomLockstep, TheLandingCarriesTheModeTheCpuArrivedIn) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  const RunObservation o = observe(rom, kFrame);
  const Landing* pushed = landingFrom(o.ran, 0x008039u);
  ASSERT_NE(pushed, nullptr);
  EXPECT_EQ(pushed->mode, Cpu65816Mode::reset()) << "emulation mode, both widths eight and known";
  // The RTI ran with both widths eight and pulled a status byte with both
  // sixteen: the landing carries what the CPU arrived with.
  const Landing* pulled = landingFrom(o.ran, 0x00804Cu);
  ASSERT_NE(pulled, nullptr);
  EXPECT_EQ(pulled->mode, Cpu65816Mode::native(false, false));
}

TEST(RomLockstep, TheValuesSeenAtASiteAreEveryDirectRegisterAndDataBankTheRunHadThere) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  const RunObservation o = observe(rom, kFrame);
  const SeenState* twice = seenAt(o.seen, 0x00802Cu);
  ASSERT_NE(twice, nullptr);
  EXPECT_EQ(twice->d, (std::vector<std::uint16_t>{0x4300u, 0x4310u}));
  EXPECT_EQ(twice->dbr, (std::vector<std::uint8_t>{0x00u}));
  const SeenState* once = seenAt(o.seen, 0x00803Fu);
  ASSERT_NE(once, nullptr);
  EXPECT_EQ(once->d, (std::vector<std::uint16_t>{0x4320u}));
  // The value before the instruction: the PLD at $801C sees the zero it replaces.
  const SeenState* pull = seenAt(o.seen, 0x00801Cu);
  ASSERT_NE(pull, nullptr);
  EXPECT_EQ(pull->d, (std::vector<std::uint16_t>{0x0000u}));
  EXPECT_EQ(seenAt(o.seen, 0x7E2000u), nullptr) << "a site outside the image is not seen";
  EXPECT_EQ(o.seen.size(), 39u) << "every instruction in the image, once";
  for (std::size_t i = 1; i < o.seen.size(); ++i) EXPECT_LT(o.seen[i - 1].address, o.seen[i].address);
}

TEST(RomLockstep, ASiteRunThroughAMirrorBankIsSeenWhereTheTreePlacesIt) {
  const std::vector<std::uint8_t> rom = mirroredImage();
  const RunObservation o = observe(rom, kFrame);
  EXPECT_EQ(o.divergences, 0u);
  EXPECT_EQ(o.instructions, 7u);
  EXPECT_EQ(o.nodes, 7u);
  EXPECT_TRUE(o.ran.empty());
  EXPECT_NE(seenAt(o.seen, 0x008010u), nullptr);
  EXPECT_EQ(seenAt(o.seen, 0x808010u), nullptr);
}

// A cartridge that runs through a mirror: the run records where the CPU went in
// the bank it went there in, and sees each site where the tree places it.
TEST(RomLockstep, AReachedTargetAndALandingAreRecordedInTheBankTheCpuArrivedIn) {
  const std::vector<std::uint8_t> rom = runningBankImage();
  const RunObservation o = observe(rom, kFrame);
  EXPECT_EQ(o.divergences, 0u);
  ASSERT_EQ(o.reached.size(), 4u) << "the pointer jump, the table jump, and the shared table jump run in two banks";
  EXPECT_EQ(o.reached[0].target, 0x008400u);
  EXPECT_EQ(o.reached[0].site, 0x00802Fu) << "a reached target's site is the address the CPU ran it at";
  EXPECT_EQ(o.reached[1].target, 0x008900u);
  EXPECT_EQ(o.reached[1].site, 0x008412u);
  EXPECT_EQ(o.reached[2].target, 0x008A10u);
  EXPECT_EQ(o.reached[2].site, 0x008A04u);
  EXPECT_EQ(o.reached[3].target, 0xC08A10u);
  EXPECT_EQ(o.reached[3].site, 0xC08A04u);
  ASSERT_EQ(o.ran.size(), 1u);
  EXPECT_EQ(o.ran[0].target, 0x008500u) << "the return through the frame PEA built";
  EXPECT_EQ(o.ran[0].site, 0xC08905u);
  EXPECT_EQ(o.ran[0].mode, Cpu65816Mode::native(true, true));
  const SeenState* arrived = seenAt(o.seen, 0xC08400u);
  ASSERT_NE(arrived, nullptr);
  EXPECT_EQ(arrived->dbr, (std::vector<std::uint8_t>{0x00u}));
  EXPECT_EQ(seenAt(o.seen, 0x008400u), nullptr);
  const SeenState* routine = seenAt(o.seen, 0xC08202u);
  ASSERT_NE(routine, nullptr);
  EXPECT_EQ(routine->dbr, (std::vector<std::uint8_t>{0xC0u})) << "the long call's routine runs in $C0";
}

TEST(RomLockstep, TwoRunsSeeTheSameLandingsAndValues) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  const RunObservation first = observe(rom, kFrame);
  const RunObservation second = observe(rom, kFrame);
  ASSERT_EQ(first.ran.size(), second.ran.size());
  for (std::size_t i = 0; i < first.ran.size(); ++i) EXPECT_TRUE(sameLanding(first.ran[i], second.ran[i]));
  ASSERT_EQ(first.seen.size(), second.seen.size());
  for (std::size_t i = 0; i < first.seen.size(); ++i) {
    EXPECT_EQ(first.seen[i].address, second.seen[i].address);
    EXPECT_EQ(first.seen[i].d, second.seen[i].d);
    EXPECT_EQ(first.seen[i].dbr, second.seen[i].dbr);
  }
  EXPECT_EQ(first.instructions, second.instructions);
}

// The lift is held to the machine on every cartridge that is ours: a
// disagreement anywhere is a finding about the lift, and this is where it
// would first be seen.
TEST(RomLockstep, NoExampleCartridgeDivergesFromTheMachine) {
  for (const examples::Example& example : examples::examples()) {
    std::vector<std::string> notes;
    const RunObservation o = observe(example.build(), 4u * kFrame, &notes);
    EXPECT_EQ(o.divergences, 0u) << example.name;
    for (const std::string& note : notes) {
      EXPECT_EQ(note.find("disagreed"), std::string::npos) << example.name << ": " << note;
    }
  }
}

TEST(RomLockstep, TheTreeStillAssemblesToItsImage) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_EQ(d.ran.size(), 3u);

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
