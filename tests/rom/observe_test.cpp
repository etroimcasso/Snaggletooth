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
// site, and that no example cartridge diverges — on the sound CPU too, whose
// every instruction, the upload stub's and the uploaded program's, is lifted
// from the bytes at its program counter and held to the audio machine.

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
using examples::landingImage;
using examples::liftingImage;
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
using examples::uploadingImage;

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

// The manifest without the blocks a run writes fresh and a disassembly without
// one does not write at all — `origin`, `staged`, `streamed`, `landed`,
// `walked`, `preview` and `seen` — and without the blank line that stood
// before each, so what the manifest keeps from one to the next is compared
// without them.
std::string withoutRunLines(const std::string& manifest) {
  std::string out;
  std::size_t position = 0;
  while (position < manifest.size()) {
    const std::size_t end = manifest.find('\n', position);
    const std::string line =
        manifest.substr(position, end == std::string::npos ? std::string::npos : end - position + 1);
    position = end == std::string::npos ? manifest.size() : end + 1;
    const bool fresh = line.rfind("seen ", 0) == 0 || line.rfind("origin ", 0) == 0 ||
                       line.rfind("staged ", 0) == 0 || line.rfind("streamed ", 0) == 0 ||
                       line.rfind("landed ", 0) == 0 || line.rfind("walked ", 0) == 0 ||
                       line.rfind("preview ", 0) == 0;
    if (fresh) continue;
    if (line == "\n" && out.size() >= 1 && out.back() == '\n' && out.size() >= 2 &&
        out[out.size() - 2] == '\n') {
      continue;  // a second blank line in a row
    }
    out += line;
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

// The landings recorded under one trigger for one channel, in the order recorded.
std::vector<const LandedRange*> landingsAt(const std::vector<LandedRange>& seen, Address site,
                                           std::uint8_t channel) {
  std::vector<const LandedRange*> out;
  for (const LandedRange& l : seen) {
    if (l.site == site && l.channel == channel) out.push_back(&l);
  }
  return out;
}

// The one landing recorded under a trigger for channel 0.
const LandedRange* landingAt(const std::vector<LandedRange>& seen, Address site) {
  const std::vector<const LandedRange*> all = landingsAt(seen, site, 0);
  return all.size() == 1 ? all.front() : nullptr;
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
  EXPECT_EQ(withoutRunLines(renderManifest(replayed)), withoutRunLines(renderManifest(ran)));
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
  for (const AssetFile& asset : d.assets) tree[asset.file] = std::string(asset.written.begin(), asset.written.end());
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
  // The tables are written in their form again from the files on disk: the
  // tree the run wrote serves them.
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
  again.assets = input->assets;
  again.readFile = [&](const std::string& file) -> std::optional<std::string> {
    for (const AssetFile& asset : ran.assets) {
      if (asset.file == file) return std::string(asset.written.begin(), asset.written.end());
    }
    return std::nullopt;
  };
  const CartridgeDisassembly kept = disassembleCartridge(again);
  ASSERT_EQ(kept.moved.size(), ran.moved.size());
  EXPECT_EQ(withoutRunLines(renderManifest(kept)), withoutRunLines(renderManifest(ran)));
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
  for (const AssetFile& asset : d.assets) tree[asset.file] = std::string(asset.written.begin(), asset.written.end());
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
    EXPECT_EQ(o.spc700Divergences, 0u) << example.name;
    EXPECT_GT(o.spc700Instructions, 0u) << example.name << ": the upload stub runs on every cartridge";
    for (const std::string& note : notes) {
      EXPECT_EQ(note.find("disagreed"), std::string::npos) << example.name << ": " << note;
      EXPECT_EQ(note.find("not checked"), std::string::npos) << example.name << ": " << note;
    }
  }
}

// ---- the sound CPU ---------------------------------------------------------------
//
// The uploading cartridge sends a twenty-instruction program and starts it; the
// stub that receives it runs first. Every instruction of both is decoded from
// the bytes at the program counter, lifted, and held to the audio machine.

TEST(RomLockstep, EverySoundInstructionIsLiftedFromItsBytesAndCheckedAgainstTheMachine) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  std::vector<std::string> notes;
  const RunObservation o = observe(rom, 5u * kFrame, &notes);
  EXPECT_EQ(o.spc700Divergences, 0u);
  EXPECT_TRUE(notes.empty()) << notes.front();
  // The stub's instructions and the twenty uploaded ones are distinct nodes;
  // the stub loops, so it runs many more times than it has nodes.
  EXPECT_GE(o.spc700Nodes, 20u);
  EXPECT_GT(o.spc700Instructions, o.spc700Nodes);

  // The same run replayed on the tree's sound program checks the uploaded
  // nodes and counts the stub's instructions, which the tree has no node for:
  // together they are every instruction the observed run checked.
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = true;
  request.observeRun = false;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_EQ(d.program.spc700.size(), 20u);
  ir::Replay replay;
  replay.rom = rom;
  replay.masterCycles = 5u * kFrame;
  const ir::DifferentialReport report = ir::differential(d.program, replay);
  EXPECT_TRUE(report.divergences.empty());
  EXPECT_EQ(report.spc700Instructions, 20u);
  EXPECT_EQ(report.spc700Patched, 0u);
  EXPECT_EQ(o.spc700Instructions, report.spc700Instructions + report.spc700Unlifted);
}

TEST(RomLockstep, TwoRunsCheckTheSameSoundInstructions) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  const RunObservation first = observe(rom, 3u * kFrame);
  const RunObservation second = observe(rom, 3u * kFrame);
  EXPECT_EQ(first.spc700Instructions, second.spc700Instructions);
  EXPECT_EQ(first.spc700Nodes, second.spc700Nodes);
  EXPECT_EQ(first.spc700Divergences, 0u);
}

// ---- where every byte came from ------------------------------------------------
//
// The staging cartridge builds eight ranges in work RAM, every way the shadow
// has a rule for, and sends each — by the engines, and one by the CPU itself
// a word at a time; a run of four frames sees all of it. The cases pin what
// the run says of each extent — its origin, its source, its writer — and the
// streams the CPU carried, from the image and from a buffer.

namespace {

using examples::stagingImage;

const StagedRange* stagedAt(const std::vector<StagedRange>& staged, Address memory,
                            std::uint32_t bytes) {
  for (const StagedRange& s : staged) {
    if (s.memory == memory && s.bytes == bytes) return &s;
  }
  return nullptr;
}

std::vector<ir::OriginInterval> intervals(std::initializer_list<ir::OriginInterval> list) {
  return list;
}

}  // namespace

TEST(RomStaged, EveryExtentCarriedOutOfWorkRamIsRecordedOnce) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  ASSERT_EQ(o.staged.size(), 8u);
  // In address order, then by count.
  EXPECT_EQ(o.staged[0].memory, 0x7E0400u);
  EXPECT_EQ(o.staged[1].memory, 0x7E0500u);
  EXPECT_EQ(o.staged[2].memory, 0x7F0000u);
  EXPECT_EQ(o.staged[3].memory, 0x7F0100u);
  EXPECT_EQ(o.staged[4].memory, 0x7F0300u);
  EXPECT_EQ(o.staged[5].memory, 0x7F0600u);
  EXPECT_EQ(o.staged[6].memory, 0x7F0700u);
  EXPECT_EQ(o.staged[7].memory, 0x7F0800u);
  // The table HDMA walks every frame is one extent however many frames walked it.
  EXPECT_EQ(o.staged[5].bytes, 3u);
  // The range two transfers sent two places is one extent.
  EXPECT_EQ(o.staged[6].bytes, 8u);
  // The transfer into work RAM through the port is not an extent: its memory
  // is the image, and nothing was carried out of work RAM by it.
  EXPECT_EQ(stagedAt(o.staged, 0x009300u, 32), nullptr);
}

TEST(RomStaged, ADecodedRangesOriginIsTheValueBytesAndItsSourceIsTheStreamWhole) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* tiles = stagedAt(o.staged, 0x7F0000u, 32);
  ASSERT_NE(tiles, nullptr);
  // Five runs, each from one value byte: a comb of five, the counts not in it.
  EXPECT_EQ(tiles->origin.image,
            intervals({{0x1001u, 0x1001u}, {0x1003u, 0x1003u}, {0x1005u, 0x1005u}, {0x1007u, 0x1007u}, {0x1009u, 0x1009u}}));
  EXPECT_FALSE(tiles->origin.approximate);
  EXPECT_TRUE(tiles->origin.registers.empty());
  // One writer: the decoder's store, thirty-two bytes, every one.
  ASSERT_EQ(tiles->writers.size(), 1u);
  const StagedWriter& writer = tiles->writers.front();
  EXPECT_EQ(writer.writer.site, 0x008116u);
  EXPECT_FALSE(writer.writer.engine);
  EXPECT_FALSE(writer.unwritten);
  EXPECT_EQ(writer.bytes, 32u);
  // Its source is the stream the decoder read, whole: the counts, the values
  // and the zero that ended it.
  EXPECT_EQ(writer.sources, intervals({{0x1000u, 0x100Au}}));
}

TEST(RomStaged, ACopiedRangesOriginIsExactAndItsSourceIsItself) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* copy = stagedAt(o.staged, 0x7F0100u, 32);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->origin.image, intervals({{0x1100u, 0x111Fu}}));
  ASSERT_EQ(copy->writers.size(), 1u);
  EXPECT_EQ(copy->writers.front().writer.site, 0x008149u);
  EXPECT_EQ(copy->writers.front().sources, intervals({{0x1100u, 0x111Fu}}));
}

TEST(RomStaged, ATableAnEngineWalksOutOfWorkRamIsStagedLikeATransfer) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* table = stagedAt(o.staged, 0x7F0600u, 3);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->origin.image, intervals({{0x1400u, 0x1402u}}));
  ASSERT_EQ(table->writers.size(), 1u);
  EXPECT_EQ(table->writers.front().writer.site, 0x008389u);
  EXPECT_EQ(table->writers.front().sources, intervals({{0x1400u, 0x1402u}}));
}

TEST(RomStaged, ARangeBuiltFromConstantsHasNoOrigin) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* fill = stagedAt(o.staged, 0x7F0300u, 16);
  ASSERT_NE(fill, nullptr);
  EXPECT_TRUE(fill->origin.empty());
  ASSERT_EQ(fill->writers.size(), 1u);
  EXPECT_EQ(fill->writers.front().writer.site, 0x0081A7u);
  EXPECT_TRUE(fill->writers.front().sources.empty());
}

TEST(RomStaged, ARangeAnEngineWroteThroughThePortIsTheEnginesAndExact) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* port = stagedAt(o.staged, 0x7E0400u, 32);
  ASSERT_NE(port, nullptr);
  EXPECT_EQ(port->origin.image, intervals({{0x1300u, 0x131Fu}}));
  ASSERT_EQ(port->writers.size(), 1u);
  EXPECT_TRUE(port->writers.front().writer.engine);
  EXPECT_EQ(port->writers.front().writer.site, 0x0081FCu);  // the write to MDMAEN
  EXPECT_EQ(port->writers.front().sources, intervals({{0x1300u, 0x131Fu}}));
}

TEST(RomStaged, BytesTheCpuWroteThroughThePortAreTheStoresOwn) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* two = stagedAt(o.staged, 0x7E0500u, 2);
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(two->origin.image, intervals({{0x1100u, 0x1101u}}));
  ASSERT_EQ(two->writers.size(), 2u);
  EXPECT_EQ(two->writers[0].writer.site, 0x008316u);
  EXPECT_EQ(two->writers[1].writer.site, 0x00831Eu);
  // Each byte's origin is its own; its source is the run holding it among
  // the routine's own reads and the caller's they joined — and the caller's
  // run over `$9100` is the whole block the copy read, which is the wider.
  EXPECT_EQ(two->writers[0].origin.image, intervals({{0x1100u, 0x1100u}}));
  EXPECT_EQ(two->writers[1].origin.image, intervals({{0x1101u, 0x1101u}}));
  EXPECT_EQ(two->writers[0].sources, intervals({{0x1100u, 0x111Fu}}));
  EXPECT_EQ(two->writers[1].sources, intervals({{0x1100u, 0x111Fu}}));
}

TEST(RomStaged, BytesNothingWroteAreUnwritten) {
  // The moving cartridge sends a sprite table it never fills whole.
  const RunObservation o = observe(movingImage(), 3u * kFrame);
  const StagedRange* table = stagedAt(o.staged, 0x7E0200u, 544);
  ASSERT_NE(table, nullptr);
  const auto unwritten = std::find_if(table->writers.begin(), table->writers.end(),
                                      [](const StagedWriter& w) { return w.unwritten; });
  ASSERT_NE(unwritten, table->writers.end());
  EXPECT_TRUE(unwritten->origin.empty());
  EXPECT_GT(unwritten->bytes, 0u);
}

TEST(RomStaged, TwoRunsSeeTheSameExtents) {
  const RunObservation first = observe(stagingImage(), 4u * kFrame);
  const RunObservation second = observe(stagingImage(), 4u * kFrame);
  ASSERT_EQ(first.staged.size(), second.staged.size());
  for (std::size_t i = 0; i < first.staged.size(); ++i) {
    EXPECT_TRUE(sameExtent(first.staged[i], second.staged[i]));
    EXPECT_EQ(first.staged[i].origin, second.staged[i].origin);
    ASSERT_EQ(first.staged[i].writers.size(), second.staged[i].writers.size());
    for (std::size_t w = 0; w < first.staged[i].writers.size(); ++w) {
      EXPECT_EQ(first.staged[i].writers[w].sources, second.staged[i].writers[w].sources);
    }
  }
  EXPECT_EQ(first.originSets, second.originSets);
  EXPECT_GT(first.originSets, 0u);
  EXPECT_EQ(first.originCap, 64u);
}

TEST(RomStreamed, ALoopOfStoresFromConsecutiveBytesIsOneStream) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  ASSERT_EQ(o.streamed.size(), 2u);
  const StreamedRange& stream = o.streamed.front();
  EXPECT_EQ(stream.site, 0x00818Fu);
  EXPECT_EQ(stream.registerAddress, 0x002122u);
  EXPECT_EQ(stream.registerName, "CGDATA");
  ASSERT_TRUE(stream.registerClass.has_value());
  EXPECT_EQ(*stream.registerClass, RegisterClass::Cgram);
  EXPECT_EQ(stream.romOffset, 0x1200u);
  EXPECT_EQ(stream.bytes, 16u);
  EXPECT_EQ(stream.times, 1u);
  EXPECT_FALSE(stream.memory.has_value());
  // The loop read the end mark after the palette: its source is the seventeen.
  EXPECT_EQ(stream.source, (ir::OriginInterval{0x1200u, 0x1210u}));
  EXPECT_TRUE(sameStream(stream, stream));
}

TEST(RomStreamed, ABufferTheCpuCarriesOutIsAStreamFromWorkRamAndAStagedExtent) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  ASSERT_EQ(o.streamed.size(), 2u);
  const StreamedRange& carry = o.streamed.back();
  EXPECT_EQ(carry.site, 0x0084C8u);
  EXPECT_EQ(carry.registerAddress, 0x002118u);
  EXPECT_EQ(carry.registerName, "VMDATAL");
  ASSERT_TRUE(carry.memory.has_value());
  EXPECT_EQ(*carry.memory, 0x7F0800u);
  EXPECT_EQ(carry.bytes, 16u);
  EXPECT_EQ(carry.times, 1u);
  // The buffer is an extent the run carried out, exactly as an engine's: its
  // writer is the copy, its source the sixteen bytes the copy read.
  const StagedRange* buffer = stagedAt(o.staged, 0x7F0800u, 16);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->origin.image, intervals({{0x1600u, 0x160Fu}}));
  ASSERT_EQ(buffer->writers.size(), 1u);
  EXPECT_EQ(buffer->writers.front().writer.site, 0x008489u);
  EXPECT_EQ(buffer->writers.front().bytes, 16u);
  EXPECT_EQ(buffer->writers.front().sources, intervals({{0x1600u, 0x160Fu}}));
}

TEST(RomStaged, ARangeSentTwoPlacesIsOneExtentWithOneSource) {
  const RunObservation o = observe(stagingImage(), 4u * kFrame);
  const StagedRange* both = stagedAt(o.staged, 0x7F0700u, 8);
  ASSERT_NE(both, nullptr);
  EXPECT_EQ(both->origin.image, intervals({{0x1500u, 0x1507u}}));
  ASSERT_EQ(both->writers.size(), 1u);
  EXPECT_EQ(both->writers.front().writer.site, 0x0083C9u);
  // Two sightings, one per transfer: the writer's count is the bytes over both.
  EXPECT_EQ(both->writers.front().bytes, 16u);
}

TEST(RomStreamed, TheLiftingCartridgeStreamsNothing) {
  // Everything it sends goes through the engines.
  const RunObservation o = observe(examples::liftingImage(), 3u * kFrame);
  EXPECT_TRUE(o.streamed.empty());
}

TEST(RomStaged, TheManifestCarriesTheLinesAndTheNextReadsPastThem) {
  const std::vector<std::uint8_t> rom = stagingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("origin   $7F:0000 bytes 32 from $00:9000 bytes 11 using 5 by sub_008100 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("origin   $7F:0100 bytes 32 from $00:9100 bytes 32 using 32 by sub_008140 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("origin   $7F:0300 bytes 16 computed by sub_0081A0\n"), std::string::npos);
  EXPECT_NE(manifest.find("origin   $7E:0400 bytes 32 from $00:9300 bytes 32 using 32 by none exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("origin   $7E:0500 bytes 2 from $00:9100 bytes 32 using 2 by sub_008300 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   vram/00_9000.bin at $7F:0000 bytes 32 to Vram by sub_008100 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("origin   $7F:0600 bytes 3 from $00:9400 bytes 3 using 3 by sub_008380 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   hdma/00_9400.bin at $7F:0600 bytes 3 to Display by sub_008380 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("streamed $00:818F $00:2122 CGDATA Cgram from $00:9200 bytes 16 times 1 at $00-$07 in palette depth none\n"),
            std::string::npos);
  // A source sent two places: one file, a `staged` line per class it fed.
  EXPECT_NE(manifest.find("origin   $7F:0700 bytes 8 from $00:9500 bytes 8 using 8 by sub_0083C0 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   staged/00_9500.bin at $7F:0700 bytes 8 to Vram by sub_0083C0 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   staged/00_9500.bin at $7F:0700 bytes 8 to Cgram by sub_0083C0 exact\n"),
            std::string::npos);
  // A buffer the CPU carried out: the stream names the buffer, the buffer's
  // source is the file.
  EXPECT_NE(manifest.find("streamed $00:84C8 $00:2118 VMDATAL Vram from $7F:0800 bytes 16 times 1 at $0035-$003D in unshown depth none\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("origin   $7F:0800 bytes 16 from $00:9600 bytes 16 using 16 by sub_008480 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   vram/00_9600.bin at $7F:0800 bytes 16 to Vram by sub_008480 exact\n"),
            std::string::npos);
  // The lines are read past, never read back: the next run sees them again.
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
  EXPECT_FALSE(parseManifest("origin $7F:0000 bytes 32 nonsense\n", error).has_value() == false &&
               error.empty());
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

// ---- where a transfer landed -------------------------------------------------------

// The landing cartridge's sites: the writes to `MDMAEN`.
constexpr Address kLandTiles = 0x008034u;       // a tileset to word $3000
constexpr Address kLandMap = 0x008066u;         // a map to $0000
constexpr Address kLandAcross = 0x008098u;      // sixty-four bytes from $0FF0
constexpr Address kLandNowhere = 0x0080CAu;     // thirty-two to $5000
constexpr Address kLandSprites = 0x0080FCu;     // a sprite sheet to $6000
constexpr Address kLandPalette = 0x008129u;     // a palette at entry sixteen
constexpr Address kLandOam = 0x00815Bu;         // the sprite table from reset
constexpr Address kLandRotated = 0x008192u;     // under the 8-bit translation
constexpr Address kLandFill = 0x0081C9u;        // a fill to $5C00
constexpr Address kLandFlipped = 0x008349u;     // the second frame's map to $7800
constexpr Address kLandWrapped = 0x00837Bu;     // the second frame's map to $0200, under the screen's wrap
constexpr Address kLandMode7 = 0x0083BEu;       // the third frame's upload under Mode 7
constexpr Address kLandUnshown = 0x008401u;     // the fourth frame's upload in forced blank
constexpr Address kLandOamAgain = 0x008430u;    // the sprite table from the handler

// The landing cartridge run for eight frames: every frame the handler does
// something on has passed, and the fourth's upload has no frame after it.
const std::vector<LandedRange>& landings() {
  static const std::vector<LandedRange> seen = [] {
    std::vector<std::string> notes;
    return observeRun(landingImage(), 8u * kFrame, InputScript{}, notes).landed;
  }();
  return seen;
}

TEST(RomLanded, ARangeToADataPortHasALandingWithItsExtent) {
  const LandedRange* tiles = landingAt(landings(), kLandTiles);
  ASSERT_NE(tiles, nullptr);
  EXPECT_EQ(tiles->channel, 0u);
  EXPECT_EQ(tiles->memory, 0x009000u);
  EXPECT_EQ(tiles->bytes, 64u);
  EXPECT_EQ(tiles->kind, MovedKind::Dma);
  EXPECT_EQ(tiles->landing.memory, PortMemory::Vram);
  EXPECT_EQ(tiles->landing.lowest, 0x3000u);
  EXPECT_EQ(tiles->landing.highest, 0x301Fu) << "sixty-four bytes are thirty-two words";
  EXPECT_EQ(tiles->times, 1u);
}

TEST(RomLanded, TheAreaIsReadAtTheFirstDrawnFrameAgainstTheBasesThenInForce) {
  // Every base was written after the upload; at the first drawn frame BG1
  // and BG2 share the name base at $1000.
  const LandedRange* tiles = landingAt(landings(), kLandTiles);
  ASSERT_NE(tiles, nullptr);
  EXPECT_TRUE(tiles->landing.shown);
  EXPECT_EQ(tiles->landing.areas, kAreaTiles1 | kAreaTiles2);
  EXPECT_EQ(areaText(tiles->landing), "tiles1+tiles2");
}

TEST(RomLanded, AMapLandsInALayersScreen) {
  const LandedRange* map = landingAt(landings(), kLandMap);
  ASSERT_NE(map, nullptr);
  EXPECT_EQ(map->landing.lowest, 0x0000u);
  EXPECT_EQ(map->landing.highest, 0x001Fu);
  EXPECT_EQ(map->landing.areas, kAreaTilemap1);
}

TEST(RomLanded, ARangeAcrossAreasNamesEveryOne) {
  // $0FF0-$100F: the last words of BG3's screen and the first of the three
  // name bases at $1000.
  const LandedRange* across = landingAt(landings(), kLandAcross);
  ASSERT_NE(across, nullptr);
  EXPECT_EQ(across->landing.areas, kAreaTilemap3 | kAreaTiles1 | kAreaTiles2 | kAreaTiles3);
  EXPECT_EQ(areaText(across->landing), "tilemap3+tiles1+tiles2+tiles3");
}

TEST(RomLanded, ARangeNoBaseReachesIsShownInNoArea) {
  const LandedRange* nowhere = landingAt(landings(), kLandNowhere);
  ASSERT_NE(nowhere, nullptr);
  EXPECT_TRUE(nowhere->landing.shown);
  EXPECT_EQ(nowhere->landing.areas, 0u);
  EXPECT_EQ(areaText(nowhere->landing), "none");
}

TEST(RomLanded, ASpriteSheetLandsInTheSpriteTiles) {
  const LandedRange* sheet = landingAt(landings(), kLandSprites);
  ASSERT_NE(sheet, nullptr);
  EXPECT_EQ(sheet->landing.lowest, 0x6000u);
  EXPECT_EQ(sheet->landing.areas, kAreaSprites);
}

TEST(RomLanded, APaletteLandsAtItsEntryAndIsReadAtOnce) {
  const LandedRange* palette = landingAt(landings(), kLandPalette);
  ASSERT_NE(palette, nullptr);
  EXPECT_EQ(palette->landing.memory, PortMemory::Cgram);
  EXPECT_EQ(palette->landing.lowest, 0x10u);
  EXPECT_EQ(palette->landing.highest, 0x1Fu) << "thirty-two bytes are sixteen entries";
  EXPECT_EQ(palette->landing.areas, kAreaPalette);
  EXPECT_EQ(areaText(palette->landing), "palette");
  EXPECT_EQ(portAddressText(palette->landing.memory, palette->landing.lowest), "$10");
}

TEST(RomLanded, APaletteLandsWhetherOrNotAFrameIsDrawn) {
  // The lifting cartridge never leaves forced blank: its VRAM landings are
  // unshown, and its palette landings are still the palette.
  std::vector<std::string> notes;
  const RunObservation run = observeRun(liftingImage(), 3u * kFrame, InputScript{}, notes);
  std::size_t palettes = 0;
  for (const LandedRange& l : run.landed) {
    if (l.landing.memory == PortMemory::Cgram) {
      ++palettes;
      EXPECT_TRUE(l.landing.shown);
      EXPECT_EQ(l.landing.areas, kAreaPalette);
    } else if (l.landing.memory == PortMemory::Vram) {
      EXPECT_FALSE(l.landing.shown);
    }
  }
  EXPECT_GT(palettes, 0u);
}

TEST(RomLanded, ASpriteTableLandsAtByteZero) {
  const LandedRange* table = landingAt(landings(), kLandOam);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->landing.memory, PortMemory::Oam);
  EXPECT_EQ(table->landing.lowest, 0x000u);
  EXPECT_EQ(table->landing.highest, 0x21Fu);
  EXPECT_EQ(table->landing.areas, kAreaOam);
  EXPECT_EQ(portAddressText(table->landing.memory, table->landing.highest), "$21F");
}

TEST(RomLanded, TheSameRangeLandingInTwoPlacesIsTwoLandings) {
  // The handler sends the sprite table on three frames without writing the
  // OAM address: on two it lands at $000, reloaded at the start of vblank,
  // and on the third the handler had moved the address to $010 first.
  const std::vector<const LandedRange*> sends = landingsAt(landings(), kLandOamAgain, 0);
  ASSERT_EQ(sends.size(), 2u);
  EXPECT_EQ(sends[0]->landing.lowest, 0x000u);
  EXPECT_EQ(sends[0]->landing.highest, 0x21Fu);
  EXPECT_EQ(sends[0]->times, 2u);
  EXPECT_EQ(sends[1]->landing.lowest, 0x010u);
  EXPECT_EQ(sends[1]->landing.highest, 0x21Fu) << "the bytes past $21F land in its mirrors";
  EXPECT_EQ(sends[1]->times, 1u);
}

TEST(RomLanded, ATranslatedRangeLandsOnTheRotatedWords) {
  // Sixteen words from $2100 under the 8-bit rotation: $2100, $2108 … $2178.
  const LandedRange* rotated = landingAt(landings(), kLandRotated);
  ASSERT_NE(rotated, nullptr);
  EXPECT_EQ(rotated->landing.lowest, 0x2100u);
  EXPECT_EQ(rotated->landing.highest, 0x2178u);
  EXPECT_EQ(rotated->landing.areas, kAreaTiles1 | kAreaTiles2 | kAreaTiles3 | kAreaSprites)
      << "the sprite base's second half wraps round the end of VRAM to $2000";
}

TEST(RomLanded, AFillLands) {
  const LandedRange* fill = landingAt(landings(), kLandFill);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->landing.lowest, 0x5C00u);
  EXPECT_EQ(fill->landing.highest, 0x5C1Fu);
  EXPECT_EQ(areaText(fill->landing), "none");
}

TEST(RomLanded, AMapUploadedBehindABaseIsReadAsTheMapItBecame) {
  // Uploaded to $7800 while BG2's screen was at $0400, then BG2 pointed at a
  // four-screen map from $7000; the first drawn frame after the upload has
  // the flip.
  const LandedRange* flipped = landingAt(landings(), kLandFlipped);
  ASSERT_NE(flipped, nullptr);
  EXPECT_EQ(flipped->landing.lowest, 0x7800u);
  EXPECT_EQ(flipped->landing.areas, kAreaTilemap2);
}

TEST(RomLanded, AnAreaThatWrapsPastTheEndOfVramReachesItsStart) {
  // Four screens from $7400 run to $83FF, and the address has fifteen bits:
  // the words from $0000 to $03FF are the screen's last, and the map uploaded
  // to $0200 lies in it — and in BG1's screen, which was there all along.
  const LandedRange* wrapped = landingAt(landings(), kLandWrapped);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(wrapped->landing.lowest, 0x0200u);
  EXPECT_EQ(wrapped->landing.areas, kAreaTilemap1 | kAreaTilemap2);
}

TEST(RomLanded, AnUploadUnderMode7IsMode7) {
  const LandedRange* mode7 = landingAt(landings(), kLandMode7);
  ASSERT_NE(mode7, nullptr);
  EXPECT_EQ(mode7->landing.areas, kAreaMode7);
  EXPECT_EQ(areaText(mode7->landing), "mode7");
}

TEST(RomLanded, AnUploadNoFrameDrewIsUnshown) {
  const LandedRange* unshown = landingAt(landings(), kLandUnshown);
  ASSERT_NE(unshown, nullptr);
  EXPECT_EQ(unshown->landing.lowest, 0x0100u);
  EXPECT_FALSE(unshown->landing.shown);
  EXPECT_EQ(unshown->landing.areas, 0u) << "a landing no frame drew lies in no area";
  EXPECT_EQ(areaText(unshown->landing), "unshown");
}

TEST(RomLanded, ARangeToARegisterThatIsNoDataPortHasNoLanding) {
  // The moving cartridge's channel 4 walks a table to the brightness
  // register; its channel 5 sends bytes through `CGADD` and `CGDATA` in
  // pairs, and those land.
  std::vector<std::string> notes;
  const RunObservation run = observeRun(movingImage(), 3u * kFrame, InputScript{}, notes);
  EXPECT_TRUE(landingsAt(run.landed, kTables, 4).empty());
  const std::vector<const LandedRange*> palette = landingsAt(run.landed, kTables, 5);
  ASSERT_FALSE(palette.empty());
  for (const LandedRange* l : palette) EXPECT_EQ(l->landing.memory, PortMemory::Cgram);
}

TEST(RomLanded, LandingsAreWrittenInTheirRangesOrder) {
  const std::vector<LandedRange>& seen = landings();
  for (std::size_t i = 1; i < seen.size(); ++i) EXPECT_FALSE(landedBefore(seen[i], seen[i - 1]));
}

TEST(RomLanded, AStreamCarriesItsLandingAndAStreamToTheAudioPortsNone) {
  // The staging cartridge's loops: one carries a palette to `CGDATA` from
  // address zero, and the run never turns the screen on.
  std::vector<std::string> notes;
  const RunObservation run = observeRun(stagingImage(), 2u * kFrame, InputScript{}, notes);
  const StreamedRange* palette = nullptr;
  const StreamedRange* tiles = nullptr;
  for (const StreamedRange& stream : run.streamed) {
    if (stream.registerAddress == 0x002122u) palette = &stream;
    if (stream.registerAddress == 0x002118u) tiles = &stream;
  }
  ASSERT_NE(palette, nullptr);
  ASSERT_TRUE(palette->landing.has_value());
  EXPECT_EQ(palette->landing->memory, PortMemory::Cgram);
  EXPECT_EQ(palette->landing->lowest, 0x00u);
  EXPECT_EQ(palette->landing->highest, 0x07u) << "sixteen bytes are eight entries";
  EXPECT_EQ(areaText(*palette->landing), "palette");
  ASSERT_NE(tiles, nullptr);
  ASSERT_TRUE(tiles->landing.has_value());
  EXPECT_EQ(tiles->landing->memory, PortMemory::Vram);
  EXPECT_FALSE(tiles->landing->shown);
}

// ---- what a lifted file's form needs ----------------------------------------------
//
// The drawing cartridge sends one of everything an editable form has a grammar
// for; the cases pin what the run keeps beside a landing for the file's form —
// the depth of the tile areas, the palette RAM at the frame that read it, the
// unit a table was walked under — and the contents an extent of work RAM was
// carried out with.

namespace {

using examples::drawingImage;

const LandedRange* drawingLanding(const RunObservation& run, Address site) {
  return landingAt(run.landed, site);
}

}  // namespace

TEST(RomLanded, ALandingCarriesTheDepthOfItsTileAreas) {
  const RunObservation run = observe(drawingImage(), 4u * kFrame);
  const LandedRange* fourBit = drawingLanding(run, 0x008046u);
  const LandedRange* twoBit = drawingLanding(run, 0x008070u);
  const LandedRange* sprites = drawingLanding(run, 0x00809Au);
  const LandedRange* palette = drawingLanding(run, 0x0080CAu);
  const LandedRange* map = drawingLanding(run, 0x008100u);
  const LandedRange* mode7 = drawingLanding(run, 0x0083EEu);
  ASSERT_NE(fourBit, nullptr);
  ASSERT_NE(twoBit, nullptr);
  ASSERT_NE(sprites, nullptr);
  ASSERT_NE(palette, nullptr);
  ASSERT_NE(map, nullptr);
  ASSERT_NE(mode7, nullptr);
  EXPECT_EQ(fourBit->landing.depths, kDepth4) << "BG1's name base under Mode 1";
  EXPECT_EQ(landingDepth(fourBit->landing), 4u);
  EXPECT_EQ(twoBit->landing.depths, kDepth2) << "BG3's name base under Mode 1";
  EXPECT_EQ(depthText(twoBit->landing), "2");
  EXPECT_EQ(sprites->landing.depths, kDepth4) << "the sprite tiles are always four";
  EXPECT_EQ(palette->landing.depths, 0u);
  EXPECT_EQ(depthText(palette->landing), "none");
  EXPECT_EQ(map->landing.depths, 0u) << "a screen has no depth";
  EXPECT_EQ(mode7->landing.depths, kDepth8);
  EXPECT_EQ(depthText(mode7->landing), "8");
}

TEST(RomLanded, ALandingReadInTwoDepthsSaysNone) {
  // The landing cartridge's rotated words reach BG1's and BG2's name bases,
  // BG3's and the sprite tiles: four bits, four, two and four.
  const RunObservation run = observe(landingImage(), 8u * kFrame);
  const LandedRange* rotated = landingAt(run.landed, 0x008192u);
  ASSERT_NE(rotated, nullptr);
  EXPECT_EQ(rotated->landing.depths, kDepth2 | kDepth4);
  EXPECT_FALSE(landingDepth(rotated->landing).has_value());
  EXPECT_EQ(depthText(rotated->landing), "none");
}

TEST(RomLanded, AVramLandingNamesThePaletteAsItStoodAtItsFrame) {
  const RunObservation run = observe(drawingImage(), 4u * kFrame);
  const LandedRange* fourBit = drawingLanding(run, 0x008046u);
  const LandedRange* palette = drawingLanding(run, 0x0080CAu);
  const LandedRange* fromBuffer = drawingLanding(run, 0x008367u);
  ASSERT_NE(fourBit, nullptr);
  ASSERT_NE(palette, nullptr);
  ASSERT_NE(fromBuffer, nullptr);
  // The tileset was read at the first drawn frame, when the palette RAM held
  // the sixteen words the reset code sent to entry zero and nothing else —
  // the fifteen bits of each the PPU keeps; the top bit is the image's alone.
  ASSERT_TRUE(fourBit->landing.palette.has_value());
  ASSERT_LT(*fourBit->landing.palette, run.palettes.size());
  const std::vector<std::uint8_t>& first = run.palettes[*fourBit->landing.palette];
  ASSERT_EQ(first.size(), 512u);
  for (std::size_t i = 0; i < 16; ++i) {
    const std::uint16_t word = static_cast<std::uint16_t>(i * 0x0421u);
    EXPECT_EQ(first[2u * i], word & 0xFFu) << i;
    EXPECT_EQ(first[2u * i + 1u], word >> 8) << i;
  }
  EXPECT_EQ(first[32], 0u) << "entry sixteen was not written yet";
  // A palette landing names none: no palette colours the palette.
  EXPECT_FALSE(palette->landing.palette.has_value());
  // The tiles sent from the buffer on the second frame were read at the third,
  // after the handler had sent the second blob to entry sixteen.
  ASSERT_TRUE(fromBuffer->landing.palette.has_value());
  const std::vector<std::uint8_t>& later = run.palettes[*fromBuffer->landing.palette];
  EXPECT_NE(*fromBuffer->landing.palette, *fourBit->landing.palette);
  const std::uint8_t decoded[] = {0x1F, 0x1F, 0x1F, 0x1F, 0x7C, 0x7C, 0x7C, 0x7C, 0xE0, 0xE0, 0xE0, 0xE0,
                                  0x03, 0x03, 0x03, 0x03, 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55};
  for (std::size_t i = 0; i < 32; ++i) {
    const std::uint8_t kept = i % 2u == 1u ? static_cast<std::uint8_t>(decoded[i] & 0x7Fu) : decoded[i];
    EXPECT_EQ(later[32u + i], kept) << i;
  }
  // Each distinct palette is kept once.
  EXPECT_EQ(run.palettes.size(), 2u);
}

TEST(RomLanded, AnUnshownLandingNamesNoPalette) {
  const RunObservation run = observe(landingImage(), 8u * kFrame);
  const LandedRange* unshown = landingAt(run.landed, 0x008401u);
  ASSERT_NE(unshown, nullptr);
  EXPECT_FALSE(unshown->landing.shown);
  EXPECT_FALSE(unshown->landing.palette.has_value());
}

TEST(RomLanded, AWalkKeepsTheUnitAndTheForm) {
  const RunObservation run = observe(drawingImage(), 4u * kFrame);
  // The walks seen every frame, whole; a run's end cuts the frame's walk
  // short, and that walk is its own range with its own line, seen once.
  std::vector<const WalkedRange*> whole;
  for (const WalkedRange& walk : run.walked) {
    if (walk.times > 1u) whole.push_back(&walk);
  }
  ASSERT_EQ(whole.size(), 3u) << run.walked.size();
  // In their ranges' order: channel 1's direct table, channel 2's indirect
  // table, then the block channel 2's entries point at.
  EXPECT_EQ(whole[0]->site, 0x0081A8u);
  EXPECT_EQ(whole[0]->channel, 1u);
  EXPECT_EQ(whole[0]->memory, 0x00A400u);
  EXPECT_EQ(whole[0]->bytes, 11u);
  EXPECT_EQ(whole[0]->kind, MovedKind::Table);
  EXPECT_EQ(whole[0]->unit, 2u) << "pattern 1: two registers, a byte each";
  EXPECT_FALSE(whole[0]->indirect);
  EXPECT_EQ(whole[1]->channel, 2u);
  EXPECT_EQ(whole[1]->memory, 0x00A410u);
  EXPECT_EQ(whole[1]->kind, MovedKind::Table);
  EXPECT_EQ(whole[1]->unit, 1u);
  EXPECT_TRUE(whole[1]->indirect);
  EXPECT_EQ(whole[2]->memory, 0x00A420u);
  EXPECT_EQ(whole[2]->kind, MovedKind::Indirect);
  EXPECT_EQ(whole[2]->unit, 1u);
  EXPECT_TRUE(whole[2]->indirect);
  EXPECT_EQ(whole[0]->times, whole[1]->times);
  // The walk joins its range as a landing does.
  const MovedRange* table = rangeAt(run.moved, 0x0081A8u, 1, 0x00A400u);
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->bytes, whole[0]->bytes);
  EXPECT_EQ(table->times, whole[0]->times);
  // Every walk is in its range's order, whole before cut.
  for (std::size_t i = 1; i < run.walked.size(); ++i) EXPECT_TRUE(walkedBefore(run.walked[i - 1], run.walked[i]));
}

TEST(RomLanded, TheUnitFollowsTheChannelsTransferPattern) {
  // fullsnes: patterns 0 → 1 byte, 1 and 2 → 2, 3 through 5 → 4, 6 → 2, 7 → 4.
  EXPECT_EQ(hdmaUnitOf(0), 1u);
  EXPECT_EQ(hdmaUnitOf(1), 2u);
  EXPECT_EQ(hdmaUnitOf(2), 2u);
  EXPECT_EQ(hdmaUnitOf(3), 4u);
  EXPECT_EQ(hdmaUnitOf(4), 4u);
  EXPECT_EQ(hdmaUnitOf(5), 4u);
  EXPECT_EQ(hdmaUnitOf(6), 2u);
  EXPECT_EQ(hdmaUnitOf(7), 4u);
}

TEST(RomLanded, AGeneralPurposeTransferHasNoWalk) {
  const RunObservation run = observe(landingImage(), 4u * kFrame);
  EXPECT_TRUE(run.walked.empty());
}

TEST(RomStaged, AnExtentKeepsEachContentItWasCarriedOutWith) {
  const RunObservation run = observe(drawingImage(), 4u * kFrame);
  const StagedRange* buffer = stagedAt(run.staged, 0x7E1000u, 32);
  ASSERT_NE(buffer, nullptr);
  // Ten contents, in the order the run carried them: the two parts of the
  // two-part blob, the twenty-four-and-eight, the sixteen-and-sixteen, the two
  // parts of the blob sent at two depths, the twenty-four the routine cleared
  // with eight from a blob, the sums of two blobs, then the blob sent to the
  // tiles and the blob sent to the palette.
  ASSERT_EQ(buffer->contents.size(), 10u);
  for (std::size_t i = 0; i < 10; ++i) EXPECT_EQ(buffer->contents[i].order, i) << "carried in this order";
  EXPECT_EQ(buffer->contents[0].bytes[0], 0x12u);
  EXPECT_EQ(buffer->contents[1].bytes[0], 0x9Au);
  EXPECT_EQ(buffer->contents[2].bytes[0], 0x21u);
  EXPECT_EQ(buffer->contents[2].bytes[24], 0x87u);
  EXPECT_EQ(buffer->contents[3].bytes[0], 0xA9u);
  EXPECT_EQ(buffer->contents[3].bytes[16], 0xEDu);
  EXPECT_EQ(buffer->contents[4].bytes[0], 0x0Au);
  EXPECT_EQ(landingDepth(*buffer->contents[4].landing), 2u) << "BG3's name base";
  EXPECT_EQ(buffer->contents[5].bytes[0], 0x1Eu);
  EXPECT_EQ(landingDepth(*buffer->contents[5].landing), 4u) << "BG1's";
  // The twenty-four-and-eight was built from two blobs: one value byte of the
  // blob at $A560 and three of the blob at $A580, a comb of four — and byte
  // by byte, three runs of eight from the one blob then a run of eight from
  // the other.
  ASSERT_EQ(buffer->contents[2].origin.image.size(), 4u);
  EXPECT_EQ(buffer->contents[2].origin.image[0].first, 0x2561u);
  EXPECT_EQ(buffer->contents[2].origin.image[1].first, 0x2581u);
  EXPECT_EQ(buffer->contents[2].origin.image[3].first, 0x2585u);
  ASSERT_EQ(buffer->contents[2].byteOrigins.size(), 4u);
  EXPECT_EQ(buffer->contents[2].byteOrigins[0].bytes, 8u);
  ASSERT_EQ(buffer->contents[2].byteOrigins[0].origin.image.size(), 1u);
  EXPECT_EQ(buffer->contents[2].byteOrigins[0].origin.image[0].first, 0x2581u);
  EXPECT_EQ(buffer->contents[2].byteOrigins[2].origin.image[0].first, 0x2585u);
  EXPECT_EQ(buffer->contents[2].byteOrigins[3].bytes, 8u);
  EXPECT_EQ(buffer->contents[2].byteOrigins[3].origin.image[0].first, 0x2561u);
  // The twenty-four the routine cleared have no origin; the eight after them
  // are the blob at $A5E0's.
  const CarriedContent& cleared = buffer->contents[6];
  EXPECT_EQ(cleared.bytes[0], 0u);
  EXPECT_EQ(cleared.bytes[24], 0xC3u);
  ASSERT_EQ(cleared.byteOrigins.size(), 2u);
  EXPECT_EQ(cleared.byteOrigins[0].bytes, 24u);
  EXPECT_TRUE(cleared.byteOrigins[0].origin.empty());
  EXPECT_EQ(cleared.byteOrigins[1].bytes, 8u);
  ASSERT_EQ(cleared.byteOrigins[1].origin.image.size(), 1u);
  EXPECT_EQ(cleared.byteOrigins[1].origin.image[0].first, 0x25E1u);
  // Every summed byte adds two bytes of the blob at $A5E8 to one of the blob
  // at $A5F4: its own origin is three image bytes, and no two bytes share one.
  const CarriedContent& summed = buffer->contents[7];
  EXPECT_EQ(summed.bytes[0], 0x03u);
  EXPECT_EQ(summed.bytes[7], 0x88u);
  ASSERT_EQ(summed.byteOrigins.size(), 32u);
  EXPECT_EQ(summed.byteOrigins[0].bytes, 1u);
  EXPECT_EQ(summed.byteOrigins[0].origin.imageBytes(), 3u);
  EXPECT_EQ(summed.byteOrigins[0].origin.image.front().first, 0x25E8u);
  EXPECT_EQ(summed.byteOrigins[0].origin.image.back().first, 0x25F4u);
  // The tiles: four runs of eight, sent to VRAM, landing in BG1's name base
  // at four bits; built from the blob at $A500.
  const CarriedContent& tiles = buffer->contents[8];
  std::vector<std::uint8_t> expected;
  for (const std::uint8_t value : {0x11u, 0x22u, 0x33u, 0x44u}) {
    for (int i = 0; i < 8; ++i) expected.push_back(value);
  }
  EXPECT_EQ(tiles.bytes, expected);
  EXPECT_EQ(tiles.cls, RegisterClass::Vram);
  EXPECT_EQ(tiles.kind, MovedKind::Dma);
  ASSERT_TRUE(tiles.landing.has_value());
  EXPECT_TRUE(tiles.landing->shown);
  EXPECT_EQ(tiles.landing->areas, kAreaTiles1 | kAreaTiles2);
  EXPECT_EQ(landingDepth(*tiles.landing), 4u);
  EXPECT_TRUE(tiles.landing->palette.has_value());
  // Built from the first blob: its four value bytes, a comb.
  ASSERT_EQ(tiles.origin.image.size(), 4u);
  EXPECT_EQ(tiles.origin.image.front().first, 0x2501u);
  EXPECT_EQ(tiles.origin.image.back().first, 0x2507u);
  // Then the palette, built from the blob at $A520 into the same bytes.
  const CarriedContent& palette = buffer->contents[9];
  EXPECT_EQ(palette.bytes.size(), 32u);
  EXPECT_EQ(palette.bytes[0], 0x1Fu);
  EXPECT_EQ(palette.bytes[31], 0x55u);
  EXPECT_EQ(palette.cls, RegisterClass::Cgram);
  ASSERT_TRUE(palette.landing.has_value());
  EXPECT_EQ(palette.landing->areas, kAreaPalette);
  ASSERT_FALSE(palette.origin.image.empty());
  EXPECT_EQ(palette.origin.image.front().first, 0x2521u);
  EXPECT_FALSE(sameContent(tiles, palette));
}

TEST(RomStaged, ABufferTheCpuCarriesOutKeepsItsContent) {
  const RunObservation run = observe(stagingImage(), 4u * kFrame);
  const StagedRange* buffer = stagedAt(run.staged, 0x7F0800u, 16);
  ASSERT_NE(buffer, nullptr);
  ASSERT_EQ(buffer->contents.size(), 1u);
  const CarriedContent& content = buffer->contents.front();
  EXPECT_EQ(content.kind, MovedKind::Stream);
  EXPECT_EQ(content.cls, RegisterClass::Vram);
  ASSERT_EQ(content.bytes.size(), 16u);
  for (std::size_t i = 0; i < 16; ++i) EXPECT_EQ(content.bytes[i], 0xB0u + i);
  ASSERT_TRUE(content.landing.has_value());
  EXPECT_FALSE(content.landing->shown) << "the run never turns the screen on";
}

TEST(RomStaged, ATableWalkedEveryFrameFromOneBufferIsOneContent) {
  const RunObservation run = observe(stagingImage(), 4u * kFrame);
  const StagedRange* table = stagedAt(run.staged, 0x7F0600u, 3);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->contents.size(), 1u);
  const CarriedContent& content = table->contents.front();
  EXPECT_EQ(content.kind, MovedKind::Table);
  EXPECT_EQ(content.bytes, (std::vector<std::uint8_t>{0x01u, 0x0Fu, 0x00u}));
  EXPECT_EQ(content.unit, 1u);
  EXPECT_FALSE(content.indirect);
  EXPECT_FALSE(content.landing.has_value()) << "the brightness register is no data port";
}

TEST(RomStaged, TwoRunsSeeTheSameContentsAndWalks) {
  const RunObservation first = observe(drawingImage(), 4u * kFrame);
  const RunObservation second = observe(drawingImage(), 4u * kFrame);
  ASSERT_EQ(first.staged.size(), second.staged.size());
  for (std::size_t i = 0; i < first.staged.size(); ++i) {
    ASSERT_EQ(first.staged[i].contents.size(), second.staged[i].contents.size());
    for (std::size_t c = 0; c < first.staged[i].contents.size(); ++c) {
      EXPECT_TRUE(sameContent(first.staged[i].contents[c], second.staged[i].contents[c]));
    }
  }
  ASSERT_EQ(first.walked.size(), second.walked.size());
  for (std::size_t i = 0; i < first.walked.size(); ++i) {
    EXPECT_EQ(first.walked[i].unit, second.walked[i].unit);
    EXPECT_EQ(first.walked[i].times, second.walked[i].times);
  }
  EXPECT_EQ(first.palettes, second.palettes);
}

namespace {

// A cartridge that copies sixteen bytes into work RAM through the port and
// sends them to the palette read downward, from the highest byte first.
std::vector<std::uint8_t> downwardImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0xA9u, 0x00u, 0x8Du, 0x81u, 0x21u,       // $8000 WMADDL
      0xA9u, 0x01u, 0x8Du, 0x82u, 0x21u,       // $8005 WMADDM: the port at $7E:0100
      0xA9u, 0x00u, 0x8Du, 0x83u, 0x21u,       // $800A WMADDH
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,       // $800F DMAP0 = $00
      0xA9u, 0x80u, 0x8Du, 0x01u, 0x43u,       // $8014 BBAD0 = $80: WMDATA
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,       // $8019 A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,       // $801E A1T0 high: $9000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,       // $8023 A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8028 DAS0 low: 16
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $802D DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8032 MDMAEN = $01 (the write at $8034)
      0xA9u, 0x00u, 0x8Du, 0x21u, 0x21u,       // $8037 CGADD = 0
      0xA9u, 0x10u, 0x8Du, 0x00u, 0x43u,       // $803C DMAP0 = $10: A->B, decrement, pattern 0
      0xA9u, 0x22u, 0x8Du, 0x01u, 0x43u,       // $8041 BBAD0 = $22: CGDATA
      0xA9u, 0x0Fu, 0x8Du, 0x02u, 0x43u,       // $8046 A1T0 low
      0xA9u, 0x01u, 0x8Du, 0x03u, 0x43u,       // $804B A1T0 high: $010F, the highest byte
      0xA9u, 0x7Eu, 0x8Du, 0x04u, 0x43u,       // $8050 A1B0 = $7E: work RAM
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,       // $8055 DAS0 low: 16
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $805A DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $805F MDMAEN = $01 (the write at $8061)
      0x80u, 0xFEu,                            // $8064 BRA $8064
  });
  for (std::size_t i = 0; i < 16; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x40u + i);
  return rom;
}

}  // namespace

TEST(RomStaged, ADecrementingCarryKeepsItsContentInAddressOrder) {
  const RunObservation run = observe(downwardImage(), 2u * kFrame);
  const StagedRange* buffer = stagedAt(run.staged, 0x7E0100u, 16);
  ASSERT_NE(buffer, nullptr);
  ASSERT_EQ(buffer->contents.size(), 1u);
  const CarriedContent& content = buffer->contents.front();
  EXPECT_EQ(content.cls, RegisterClass::Cgram);
  ASSERT_EQ(content.bytes.size(), 16u);
  // Read from $010F down to $0100, kept as the bytes lie in memory.
  for (std::size_t i = 0; i < 16; ++i) EXPECT_EQ(content.bytes[i], 0x40u + i) << i;
  ASSERT_TRUE(content.landing.has_value());
  EXPECT_EQ(content.landing->areas, kAreaPalette);
}

}  // namespace snaggletooth::disasm
