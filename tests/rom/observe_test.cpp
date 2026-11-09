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
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_differential.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"
#include "rom/rom_observe_internal.h"
#include "rom/rom_verify.h"
#include "snaggletooth/snes/video_frame.h"

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
// one does not write at all — `origin`, `staged`, `enumerated`, `streamed`,
// `landed`, `walked`, `preview` and `seen`, and the notes the toolkit's run of
// the routines and its reading of a transfer from work RAM write — and without
// the blank line that stood before each, so what the manifest keeps from one
// to the next is compared without them.
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
                       line.rfind("preview ", 0) == 0 || line.rfind("enumerated ", 0) == 0 ||
                       line.rfind("note     enumerate ", 0) == 0 ||
                       (line.rfind("note     dma ", 0) == 0 &&
                        (line.find("; the run wrote the extent and never started this transfer;") != std::string::npos ||
                         line.find("; nothing wrote the extent in the run; nothing to run from") != std::string::npos));
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

// A pointer in a LoROM save window with no save behind it is image bytes the
// run can see, read as the CPU reads them; on a coprocessor's board the same
// half is the chip's, which the run cannot see, and the jump is noted, not
// guessed.
TEST(RomObserve, APointerInABareWindowIsReadAndOneInAChipsHalfIsNot) {
  std::vector<std::uint8_t> rom = loRomImage(128);  // 4 MB, no save: bank $70 is its own bytes
  put(rom, 0x0000u, {0x5Cu, 0x00u, 0x80u, 0x70u});  // JML $70:8000
  put(rom, 0x380000u, {0xA2u, 0x00u,                // $70:8000: LDX #$00
                       0x7Cu, 0x00u, 0x10u,         // $70:8002: JMP (!$1000,X), the pointer at $70:1000
                       0xDBu});                     // $70:8005: STP
  put(rom, 0x381000u, {0x05u, 0x80u});              // $70:9000, which $70:1000 repeats: $8005
  std::vector<std::string> notes;
  const std::vector<ReachedTarget> seen = run(rom, kFrame, &notes);
  const ReachedTarget* jump = reachedFrom(seen, 0x708002u);
  ASSERT_NE(jump, nullptr);
  EXPECT_EQ(jump->target, 0x708005u);
  for (const std::string& note : notes) EXPECT_EQ(note.find("cannot see"), std::string::npos) << note;

  rom[0x7FC0u + 0x16u] = 0x03u;  // the chipset byte: a DSP, whose board keeps the half
  notes.clear();
  const std::vector<ReachedTarget> chip = run(rom, kFrame, &notes);
  EXPECT_EQ(reachedFrom(chip, 0x708002u), nullptr);
  const bool noted = std::any_of(notes.begin(), notes.end(), [](const std::string& note) {
    return note.find("$70:8002") != std::string::npos && note.find("cannot see") != std::string::npos;
  });
  EXPECT_TRUE(noted);
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
constexpr Address kTables = 0x008115u;    // HDMA channels 4 and 5
constexpr Address kRamTable = 0x00815Au;  // HDMA channel 6, its table in work RAM
constexpr Address kSprites = 0x008326u;   // channel 7 from the handler, every frame
constexpr Address kChunks = 0x0081C2u;    // channel 0 twice from one instruction, the chunks adjacent
constexpr Address kMirror = 0x0081B5u;    // channel 0 once more, MDMAEN written through bank $80

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
  // frame leaves the walk unfinished, and the bytes it did read are a range. The
  // channel is brought in part-way down the first frame, and a channel brought in
  // while others are going delivers before it fetches, so that frame's walk begins a
  // byte below the table and is one byte longer than the frames that begin at the top.
  const std::vector<std::uint8_t> rom = movingImage();
  const std::vector<MovedRange> seen = moved(rom, 2u * kFrame + 60u * 1364u);
  const std::vector<const MovedRange*> walks = rangesAt(seen, kRamTable, 6);
  ASSERT_EQ(walks.size(), 3u) << "the frame it joined, one whole walk, one the budget cut";
  EXPECT_EQ(walks[0]->bytes, 130u) << "the cursor's byte, the entry, 127 values and the terminator";
  EXPECT_EQ(walks[1]->bytes, 129u);
  EXPECT_LT(walks[2]->bytes, 129u);
  EXPECT_GT(walks[2]->bytes, 1u);
  EXPECT_EQ(walks[2]->times, 1u);
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
  EXPECT_NE(manifest.find("moved    $00:8115 channel 5 to-register $00:2121 CGADD Cgram memory $00:9520 "
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
  EXPECT_EQ(both->writers.front().writer.site, 0x0083E9u);
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
  EXPECT_NE(manifest.find("origin   $7F:0700 bytes 8 from $00:9500 bytes 8 using 8 by sub_0083E0 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   staged/00_9500.bin at $7F:0700 bytes 8 to Vram by sub_0083E0 exact\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("staged   staged/00_9500.bin at $7F:0700 bytes 8 to Cgram by sub_0083E0 exact\n"),
            std::string::npos);
  // A buffer the CPU carried out: the stream names the buffer, the buffer's
  // source is the file. Its eight words follow the 53 the transfers sent.
  EXPECT_NE(manifest.find("streamed $00:84C8 $00:2118 VMDATAL Vram from $7F:0800 bytes 16 times 1 at $0035-$003C in unshown depth none\n"),
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
  // A palette landing names none: no palette colors the palette.
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
  EXPECT_EQ(whole[0]->site, 0x0081CCu);
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
  const MovedRange* table = rangeAt(run.moved, 0x0081CCu, 1, 0x00A400u);
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
  const std::uint8_t runs[4] = {0x11u, 0x22u, 0x33u, 0x44u};
  for (const std::uint8_t value : runs) {
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

// ---- the samples the key-ons named ---------------------------------------------
//
// The drawing cartridge's sound program keys two voices on at once: voice 0 on
// the two-block sample uploaded with the directory, voice 1 on two headers the
// program wrote over cleared memory. The cases pin what the run reads at the
// key-on — the directory entry, the blocks to the end flag — how a sample
// keyed on again is counted, that other bytes at the same address are another
// sample, and that a walk without an end flag names nothing.

namespace {

// The uploaded sample as the image holds it, two blocks of shift 11.
const std::vector<std::uint8_t> kUploadedSample = {0xB0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
                                                   0xB3, 0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21};

// The sample the program built: a first header, eight zero bytes the boot
// left, a last header with the end and loop flags, eight more.
const std::vector<std::uint8_t> kBuiltSample = {0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3, 0, 0, 0, 0, 0, 0, 0, 0};

const KeyedSample* sampleAt(const std::vector<KeyedSample>& samples, std::uint16_t start) {
  for (const KeyedSample& s : samples) {
    if (s.start == start) return &s;
  }
  return nullptr;
}

}  // namespace

TEST(RomSamples, AKeyOnNamesEachVoicesSampleThroughTheDirectory) {
  const RunObservation run = observe(drawingImage(), 4u * kFrame);
  ASSERT_EQ(run.samples.size(), 2u);
  const KeyedSample* uploaded = sampleAt(run.samples, 0x0308u);
  const KeyedSample* built = sampleAt(run.samples, 0x0330u);
  ASSERT_NE(uploaded, nullptr);
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(uploaded->loop, 0x0311u) << "entry 0's loop address";
  EXPECT_EQ(uploaded->bytes, kUploadedSample);
  EXPECT_EQ(uploaded->times, 1u);
  EXPECT_EQ(built->loop, 0x0330u);
  EXPECT_EQ(built->bytes, kBuiltSample);
  EXPECT_EQ(built->times, 1u);
  EXPECT_EQ(&run.samples.front(), uploaded) << "in start order";
}

TEST(RomSamples, ASampleKeyedOnAgainCountsOnceAndOtherBytesThereAreAnother) {
  // The program goes on after the key-on: it rewrites the built sample's
  // second byte and keys both voices on again. The image holds the program's
  // extra bytes after its ninetieth, so the upload is lengthened to carry them.
  std::vector<std::uint8_t> rom = drawingImage();
  put(rom, 0x2758u, {
      0xE8u, 0x11u, 0xC5u, 0x31u, 0x03u,  // $0258 MOV A,#$11 / MOV !$0331,A
      0x8Fu, 0x03u, 0xF3u,                // $025D KON = $03 again, the address register still at KON
      0x2Fu, 0xFEu,                       // $0260 BRA $0260
  });
  rom[0x024Bu] = 0x62u;  // CPX #$62 at $824A: the program is 98 bytes now
  rom[0x0261u] = 0x63u;  // LDA #$63 at $8260: the next block's index is two past its last
  const RunObservation run = observe(rom, 4u * kFrame);
  ASSERT_EQ(run.samples.size(), 3u);
  const KeyedSample* uploaded = sampleAt(run.samples, 0x0308u);
  ASSERT_NE(uploaded, nullptr);
  EXPECT_EQ(uploaded->times, 2u) << "the same bytes at the same address, keyed on twice";
  EXPECT_EQ(run.samples[1].start, 0x0330u);
  EXPECT_EQ(run.samples[2].start, 0x0330u);
  EXPECT_EQ(run.samples[1].bytes, kBuiltSample) << "the shorter first, then by bytes";
  std::vector<std::uint8_t> rewritten = kBuiltSample;
  rewritten[1] = 0x11u;
  EXPECT_EQ(run.samples[2].bytes, rewritten);
  EXPECT_EQ(run.samples[1].times, 1u);
  EXPECT_EQ(run.samples[2].times, 1u);
  EXPECT_FALSE(sameSample(run.samples[1], run.samples[2]));
}

TEST(RomSamples, AWriteToARegisterMirrorSetsNothing) {
  // The program goes on after the key-on: it writes zero through the
  // directory register's read-only mirror at $DD and keys both voices on
  // again. The DSP keeps its directory, so the same two samples are named,
  // the uploaded one twice.
  std::vector<std::uint8_t> rom = drawingImage();
  put(rom, 0x2758u, {
      0x8Fu, 0xDDu, 0xF2u, 0x8Fu, 0x00u, 0xF3u,  // $0258 the mirror of DIR written with zero
      0x8Fu, 0x4Cu, 0xF2u, 0x8Fu, 0x03u, 0xF3u,  // $025E KON = $03 again
      0x2Fu, 0xFEu,                              // $0264 BRA $0264
  });
  rom[0x024Bu] = 0x66u;  // CPX #$66 at $824A: the program is 102 bytes now
  rom[0x0261u] = 0x67u;  // LDA #$67 at $8260: the next block's index is two past its last
  const RunObservation run = observe(rom, 4u * kFrame);
  ASSERT_EQ(run.samples.size(), 2u);
  const KeyedSample* uploaded = sampleAt(run.samples, 0x0308u);
  ASSERT_NE(uploaded, nullptr);
  EXPECT_EQ(uploaded->times, 2u);
  EXPECT_EQ(uploaded->bytes, kUploadedSample);
  EXPECT_NE(sampleAt(run.samples, 0x0330u), nullptr);
}

TEST(RomSamples, AKeyOnNamingNoVoiceNamesNothing) {
  // The program goes on after the key-on and writes KON again with no bit
  // set: nothing is keyed on, and the counts stay at one.
  std::vector<std::uint8_t> rom = drawingImage();
  put(rom, 0x2758u, {
      0x8Fu, 0x00u, 0xF3u,  // $0258 KON = $00, the address register still at KON
      0x2Fu, 0xFEu,         // $025B BRA $025B
  });
  rom[0x024Bu] = 0x5Du;  // CPX #$5D at $824A: the program is 93 bytes now
  rom[0x0261u] = 0x5Eu;  // LDA #$5E at $8260: the next block's index is two past its last
  const RunObservation run = observe(rom, 4u * kFrame);
  ASSERT_EQ(run.samples.size(), 2u);
  EXPECT_EQ(run.samples[0].times, 1u);
  EXPECT_EQ(run.samples[1].times, 1u);
}

TEST(RomSamples, TheWalkPassesALoopFlagAndStopsAtTheEndFlag) {
  // The uploaded sample's first block carries the loop flag alone and its
  // last the end flag alone: the walk reads both blocks, since only the end
  // flag stops it.
  std::vector<std::uint8_t> rom = drawingImage();
  rom[0x2780u + 8u] = 0xB2u;       // the first block: shift 11, loop, no end
  rom[0x2780u + 8u + 9u] = 0xB1u;  // the last block: shift 11, end, no loop
  const RunObservation run = observe(rom, 4u * kFrame);
  const KeyedSample* uploaded = sampleAt(run.samples, 0x0308u);
  ASSERT_NE(uploaded, nullptr);
  ASSERT_EQ(uploaded->bytes.size(), 18u);
  EXPECT_EQ(uploaded->bytes[0], 0xB2u);
  EXPECT_EQ(uploaded->bytes[9], 0xB1u);
}

TEST(RomSamples, AWalkThatReachesTheEndOfMemoryWithoutAnEndFlagIsANoteAndNoSample) {
  // Entry 0 is pointed at $FF01: from there the walk reads cleared memory,
  // then the seven headers it meets in the upload stub's bytes at $FFC0 —
  // every one even — and reaches the memory's end after twenty-eight blocks
  // without a flag. Voice 1's built sample is named as before.
  std::vector<std::uint8_t> rom = drawingImage();
  rom[0x2780u] = 0x01u;  // entry 0: start $FF01
  rom[0x2781u] = 0xFFu;
  std::vector<std::string> notes;
  const RunObservation run = observe(rom, 4u * kFrame, &notes);
  ASSERT_EQ(run.samples.size(), 1u);
  EXPECT_EQ(run.samples.front().start, 0x0330u);
  const auto noted = [&](std::string_view text) {
    return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(text) != std::string::npos; });
  };
  EXPECT_TRUE(noted("run: the sample at $FF01 voice 0 keyed on reaches the end of the audio memory without an end flag; "
                    "not recorded"))
      << (notes.empty() ? std::string("no notes") : notes.front());
  EXPECT_EQ(std::count_if(notes.begin(), notes.end(),
                          [](const std::string& n) { return n.find("without an end flag") != std::string::npos; }),
            1)
      << "once per start address";
}

// Every byte an engine writes on the A bus is named in the step that wrote it:
// a transfer from a register back into work RAM, and one into work RAM through
// the port.
TEST(RomObserve, TheRecorderNamesEveryEngineWrite) {
  const std::vector<std::pair<std::vector<std::uint8_t>, Address>> cases = {
      {movingImage(), 0x7E0300u},           // channel 1 reads VRAM back into $7E:0300
      {examples::transferImage(), 0x7E1000u},  // through the port to $7E:1000
  };
  for (const auto& [rom, first] : cases) {
    const CartridgeBoard board = cartridgeBoard(rom);
    Snes machine{SnesConfig{.rom = rom}};
    ir::Provenance shadow{board, rom.size(), 64};
    detail::Recorder recorder{machine, shadow};
    shadow.carries = &recorder;
    machine.setObserver(&recorder);
    std::set<Address> named;
    std::uint64_t spent = 0;
    while (spent < 3u * kFrame) {
      const Cpu65816State& cpu = machine.state().cpu;
      recorder.stepBegan((static_cast<Address>(cpu.pbr) << 16) | cpu.pc);
      spent += machine.step();
      named.insert(recorder.engineWrites.begin(), recorder.engineWrites.end());
    }
    machine.setObserver(nullptr);
    shadow.carries = nullptr;
    for (Address at = first; at < first + 16u; ++at) {
      // The same byte of work RAM, through whichever mirror the engine named it.
      const bool found = std::any_of(named.begin(), named.end(), [&](Address a) {
        return ir::Provenance::workRamIndex(a) == ir::Provenance::workRamIndex(at);
      });
      EXPECT_TRUE(found) << formatAddress(at, 24);
    }
  }
}

// ---- the run on a machine a host runs -------------------------------------------
//
// An ObservedRun rides the boundaries the machine reports, so a host that steps
// the machine the way `observeRun` does gets `observeRun`'s observation field for
// field, and a host that runs it a quarter frame at a time, with either thread or
// both, gets the same again. The cases pin that, the refusals, an observation
// taken mid-run, the order of the notes, and the reset line.

namespace {

void expectSameLanding(const PortLanding& a, const PortLanding& b, const std::string& what) {
  EXPECT_EQ(a.memory, b.memory) << what;
  EXPECT_EQ(a.lowest, b.lowest) << what;
  EXPECT_EQ(a.highest, b.highest) << what;
  EXPECT_EQ(a.shown, b.shown) << what;
  EXPECT_EQ(a.areas, b.areas) << what;
  EXPECT_EQ(a.depths, b.depths) << what;
  EXPECT_EQ(a.palette, b.palette) << what;
}

// Every field of two observations, in the order `RunObservation` declares them.
void expectSameObservation(const RunObservation& a, const RunObservation& b, const std::string& what) {
  ASSERT_EQ(a.reached.size(), b.reached.size()) << what;
  for (std::size_t i = 0; i < a.reached.size(); ++i) {
    EXPECT_TRUE(sameSighting(a.reached[i], b.reached[i])) << what << " reached " << i;
    EXPECT_EQ(a.reached[i].call, b.reached[i].call) << what;
    EXPECT_EQ(a.reached[i].name, b.reached[i].name) << what;
  }
  ASSERT_EQ(a.moved.size(), b.moved.size()) << what;
  for (std::size_t i = 0; i < a.moved.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.moved[i]), detail::keyOf(b.moved[i])) << what << " moved " << i;
    EXPECT_EQ(a.moved[i].times, b.moved[i].times) << what << " moved " << i;
  }
  ASSERT_EQ(a.landed.size(), b.landed.size()) << what;
  for (std::size_t i = 0; i < a.landed.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.landed[i]), detail::keyOf(b.landed[i])) << what << " landed " << i;
    EXPECT_EQ(a.landed[i].landing.palette, b.landed[i].landing.palette) << what << " landed " << i;
    EXPECT_EQ(a.landed[i].times, b.landed[i].times) << what << " landed " << i;
  }
  ASSERT_EQ(a.walked.size(), b.walked.size()) << what;
  for (std::size_t i = 0; i < a.walked.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.walked[i]), detail::keyOf(b.walked[i])) << what << " walked " << i;
    EXPECT_EQ(a.walked[i].times, b.walked[i].times) << what << " walked " << i;
  }
  EXPECT_EQ(a.palettes, b.palettes) << what;
  ASSERT_EQ(a.ran.size(), b.ran.size()) << what;
  for (std::size_t i = 0; i < a.ran.size(); ++i) {
    EXPECT_TRUE(sameLanding(a.ran[i], b.ran[i])) << what << " ran " << i;
    EXPECT_EQ(a.ran[i].name, b.ran[i].name) << what;
  }
  ASSERT_EQ(a.seen.size(), b.seen.size()) << what;
  for (std::size_t i = 0; i < a.seen.size(); ++i) {
    EXPECT_EQ(a.seen[i].address, b.seen[i].address) << what;
    EXPECT_EQ(a.seen[i].d, b.seen[i].d) << what;
    EXPECT_EQ(a.seen[i].dbr, b.seen[i].dbr) << what;
  }
  ASSERT_EQ(a.staged.size(), b.staged.size()) << what;
  for (std::size_t i = 0; i < a.staged.size(); ++i) {
    const StagedRange& x = a.staged[i];
    const StagedRange& y = b.staged[i];
    EXPECT_EQ(x.memory, y.memory) << what << " staged " << i;
    EXPECT_EQ(x.bytes, y.bytes) << what << " staged " << i;
    EXPECT_EQ(x.origin, y.origin) << what << " staged " << i;
    ASSERT_EQ(x.writers.size(), y.writers.size()) << what << " staged " << i;
    for (std::size_t w = 0; w < x.writers.size(); ++w) {
      EXPECT_EQ(x.writers[w].writer, y.writers[w].writer) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].unwritten, y.writers[w].unwritten) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].bytes, y.writers[w].bytes) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].origin, y.writers[w].origin) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].sources, y.writers[w].sources) << what << " staged " << i;
    }
    ASSERT_EQ(x.contents.size(), y.contents.size()) << what << " staged " << i;
    for (std::size_t c = 0; c < x.contents.size(); ++c) {
      const CarriedContent& p = x.contents[c];
      const CarriedContent& q = y.contents[c];
      EXPECT_TRUE(sameContent(p, q)) << what << " staged " << i << " content " << c;
      EXPECT_EQ(p.origin, q.origin) << what << " staged " << i << " content " << c;
      ASSERT_EQ(p.landing.has_value(), q.landing.has_value()) << what;
      if (p.landing) expectSameLanding(*p.landing, *q.landing, what);
      ASSERT_EQ(p.byteOrigins.size(), q.byteOrigins.size()) << what;
      for (std::size_t k = 0; k < p.byteOrigins.size(); ++k) {
        EXPECT_EQ(p.byteOrigins[k].bytes, q.byteOrigins[k].bytes) << what;
        EXPECT_EQ(p.byteOrigins[k].origin, q.byteOrigins[k].origin) << what;
      }
      EXPECT_EQ(p.order, q.order) << what << " staged " << i << " content " << c;
    }
  }
  ASSERT_EQ(a.streamed.size(), b.streamed.size()) << what;
  for (std::size_t i = 0; i < a.streamed.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.streamed[i]), detail::keyOf(b.streamed[i])) << what << " streamed " << i;
    EXPECT_EQ(a.streamed[i].times, b.streamed[i].times) << what << " streamed " << i;
    EXPECT_EQ(a.streamed[i].source, b.streamed[i].source) << what << " streamed " << i;
  }
  ASSERT_EQ(a.samples.size(), b.samples.size()) << what;
  for (std::size_t i = 0; i < a.samples.size(); ++i) {
    EXPECT_EQ(a.samples[i].start, b.samples[i].start) << what;
    EXPECT_EQ(a.samples[i].loop, b.samples[i].loop) << what;
    EXPECT_EQ(a.samples[i].bytes, b.samples[i].bytes) << what;
    EXPECT_EQ(a.samples[i].times, b.samples[i].times) << what;
  }
  EXPECT_EQ(a.instructions, b.instructions) << what;
  EXPECT_EQ(a.interrupts, b.interrupts) << what;
  EXPECT_EQ(a.nodes, b.nodes) << what;
  EXPECT_EQ(a.divergences, b.divergences) << what;
  EXPECT_EQ(a.spc700Instructions, b.spc700Instructions) << what;
  EXPECT_EQ(a.spc700Nodes, b.spc700Nodes) << what;
  EXPECT_EQ(a.spc700Divergences, b.spc700Divergences) << what;
  EXPECT_EQ(a.originSets, b.originSets) << what;
  EXPECT_EQ(a.originCap, b.originCap) << what;
  // A host's run makes no plan, so neither side holds an outcome to compare.
  EXPECT_EQ(a.enumerated.size(), b.enumerated.size()) << what;
  EXPECT_EQ(a.variantsPlanned, b.variantsPlanned) << what;
  EXPECT_EQ(a.variantsRun, b.variantsRun) << what;
  EXPECT_EQ(a.variantsReturned, b.variantsReturned) << what;
  EXPECT_EQ(a.variantsConfined, b.variantsConfined) << what;
  EXPECT_EQ(a.enumerationCycles, b.enumerationCycles) << what;
}

// One script for every example: buttons pressed and let go over the first
// frames, so a cartridge that reads the pads takes its paths. No example has a
// script of its own.
InputScript exampleScript() {
  std::string error;
  const std::optional<InputScript> parsed =
      parseInputScript("frame 2 1 b\nframe 4 1 none\nframe 6 1 start\nframe 8 1 start b\nframe 10 1 none\n", error);
  EXPECT_TRUE(parsed.has_value()) << error;
  return parsed.value_or(InputScript{});
}

// The host `observeRun` is, over an ObservedRun: the machine stepped to the
// budget, each frame's pads presented after the step that wrapped the beam.
RunObservation steppedHost(std::span<const std::uint8_t> rom, std::uint64_t cycles, const InputScript& input,
                           std::vector<std::string>& notes) {
  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun run{machine, rom};
  std::uint32_t frame = 0;
  detail::presentPads(machine, input, frame);
  std::uint64_t spent = 0;
  while (spent < cycles) {
    spent += machine.step();
    if (run.frame() != frame) {
      frame = run.frame();
      detail::presentPads(machine, input, frame);
    }
  }
  return run.finish(notes);
}

// A player's pads: the next frame's presented at every frame the PPU finishes.
struct FramePads final : FrameObserver {
  Snes& machine;
  const InputScript& input;
  std::uint32_t count = 0;
  FramePads(Snes& m, const InputScript& i) : machine(m), input(i) { detail::presentPads(machine, input, 0); }
  void frame(const VideoFrame&) override { detail::presentPads(machine, input, ++count); }
};

struct Hosted {
  RunObservation observation;
  std::vector<std::string> notes;
  std::uint64_t master = 0;
};

SnesConfig threadedConfig(std::span<const std::uint8_t> rom, bool audioThread, bool pictureThread) {
  SnesConfig config;
  config.rom = rom;
  config.apuThread = audioThread;
  config.ppuThread = pictureThread;
  return config;
}

// A host that runs the machine a quarter frame at a time past `cycles`, then
// finishes the instruction the last call stopped inside with one step().
Hosted runHost(std::span<const std::uint8_t> rom, std::uint64_t cycles, const InputScript& input, bool audioThread,
               bool pictureThread) {
  Snes machine{threadedConfig(rom, audioThread, pictureThread)};
  ObservedRun run{machine, rom};
  FramePads pads{machine, input};
  machine.setFrameObserver(&pads);
  while (machine.state().master < cycles) machine.run(kFrame / 4u);
  machine.step();
  Hosted out;
  out.master = machine.state().master;
  out.observation = run.finish(out.notes);
  machine.setFrameObserver(nullptr);
  return out;
}

// A host that steps the machine to the master count `master`.
Hosted stepHost(std::span<const std::uint8_t> rom, std::uint64_t master, const InputScript& input, bool audioThread,
                bool pictureThread) {
  Snes machine{threadedConfig(rom, audioThread, pictureThread)};
  ObservedRun run{machine, rom};
  FramePads pads{machine, input};
  machine.setFrameObserver(&pads);
  while (machine.state().master < master) machine.step();
  Hosted out;
  out.master = machine.state().master;
  out.observation = run.finish(out.notes);
  machine.setFrameObserver(nullptr);
  return out;
}

struct QuietBus final : BusObserver {
  void access(const BusAccess&) override {}
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};
struct QuietAudio final : ApuObserver {
  void access(std::uint16_t, std::uint8_t, bool) override {}
  void instruction(const Spc700State&, const Spc700State&, std::uint32_t) override {}
};
struct QuietBoundaries final : InstructionObserver {
  void instruction(const Cpu65816State&, const Cpu65816State&, std::uint32_t) override {}
};

bool hasNote(const std::vector<std::string>& notes, std::string_view text) {
  return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(text) != std::string::npos; });
}

// The first note holding `text`, or nothing: an assertion that it is empty
// names the note when one is there.
std::string noteWith(const std::vector<std::string>& notes, std::string_view text) {
  const auto found =
      std::find_if(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(text) != std::string::npos; });
  return found == notes.end() ? std::string() : *found;
}

}  // namespace

TEST(RomObservedRun, AHostsRunIsObserveRunsObservationOnEveryExample) {
  const InputScript script = exampleScript();
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    std::vector<std::string> ownNotes;
    const RunObservation own = observeRun(rom, 12u * kFrame, script, ownNotes);
    std::vector<std::string> hostNotes;
    const RunObservation hosted = steppedHost(rom, 12u * kFrame, script, hostNotes);
    expectSameObservation(own, hosted, std::string(example.name));
    EXPECT_EQ(ownNotes, hostNotes) << example.name;
  }
}

TEST(RomObservedRun, TheRunIsTheSameHoweverTheHostRunsTheMachine) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  const InputScript script = exampleScript();
  const Hosted reference = runHost(rom, 5u * kFrame, script, false, false);
  ASSERT_GT(reference.observation.spc700Instructions, 0u);
  for (const bool audioThread : {false, true}) {
    for (const bool pictureThread : {false, true}) {
      const std::string what = std::string("audio thread ") + (audioThread ? "on" : "off") + ", picture thread " +
                               (pictureThread ? "on" : "off");
      const Hosted byRun = runHost(rom, 5u * kFrame, script, audioThread, pictureThread);
      const Hosted byStep = stepHost(rom, byRun.master, script, audioThread, pictureThread);
      ASSERT_EQ(byRun.master, reference.master) << what;
      ASSERT_EQ(byStep.master, byRun.master) << what << ": the two hosts stand at one boundary";
      expectSameObservation(reference.observation, byRun.observation, what + ", run()");
      expectSameObservation(reference.observation, byStep.observation, what + ", step()");
      EXPECT_EQ(byRun.notes, reference.notes) << what;
      EXPECT_EQ(byStep.notes, reference.notes) << what;
    }
  }
}

TEST(RomObservedRun, AMachinePastPowerOnOrWithAnObserverIsRefused) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  {
    Snes machine{SnesConfig{.rom = rom}};
    static_cast<void>(machine.step());
    EXPECT_THROW(static_cast<void>(ObservedRun(machine, rom)), std::logic_error) << "the machine has run";
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.apuObserver(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), nullptr);
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    QuietBus bus;
    machine.setObserver(&bus);
    EXPECT_THROW(static_cast<void>(ObservedRun(machine, rom)), std::logic_error) << "a bus observer";
    EXPECT_EQ(machine.observer(), &bus);
    EXPECT_EQ(machine.apuObserver(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), nullptr);
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    QuietAudio audio;
    machine.setApuObserver(&audio);
    EXPECT_THROW(static_cast<void>(ObservedRun(machine, rom)), std::logic_error) << "an audio observer";
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.apuObserver(), &audio);
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    QuietBoundaries boundaries;
    machine.setInstructionObserver(&boundaries);
    EXPECT_THROW(static_cast<void>(ObservedRun(machine, rom)), std::logic_error) << "an instruction observer";
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), &boundaries);
    machine.setInstructionObserver(nullptr);
  }
  {
    Snes machine{SnesConfig{.rom = rom}};
    const std::vector<std::uint8_t> tooSmall(16, 0u);
    EXPECT_THROW(static_cast<void>(ObservedRun(machine, tooSmall)), std::logic_error) << "no cartridge header";
    EXPECT_EQ(machine.observer(), nullptr);
  }
  {
    // Bound and let go without finish(): the observers are cleared, and a
    // second run binds to the same machine at power-on.
    Snes machine{SnesConfig{.rom = rom}};
    {
      ObservedRun run{machine, rom};
      EXPECT_NE(machine.observer(), nullptr);
      EXPECT_NE(machine.apuObserver(), nullptr);
      EXPECT_NE(machine.instructionObserver(), nullptr);
    }
    EXPECT_EQ(machine.observer(), nullptr);
    EXPECT_EQ(machine.apuObserver(), nullptr);
    EXPECT_EQ(machine.instructionObserver(), nullptr);
    EXPECT_NO_THROW(static_cast<void>(ObservedRun(machine, rom)));
  }
  {
    // Finished: the observers are cleared, and the host's own set afterwards
    // stay when the run goes.
    Snes machine{SnesConfig{.rom = rom}};
    QuietBoundaries boundaries;
    {
      ObservedRun run{machine, rom};
      static_cast<void>(machine.step());
      std::vector<std::string> notes;
      static_cast<void>(run.finish(notes));
      EXPECT_EQ(machine.observer(), nullptr);
      EXPECT_EQ(machine.apuObserver(), nullptr);
      EXPECT_EQ(machine.instructionObserver(), nullptr);
      machine.setInstructionObserver(&boundaries);
    }
    EXPECT_EQ(machine.instructionObserver(), &boundaries);
    machine.setInstructionObserver(nullptr);
  }
}

TEST(RomObservedRun, AnObservationMidRunLeavesTheRunAsItWasAndHoldsNothingOpen) {
  // The moving cartridge walks two HDMA tables and sends a sprite table every
  // frame, so halfway down a frame a walk is open.
  const std::vector<std::uint8_t> rom = movingImage();
  const std::uint64_t middle = 2u * kFrame + kFrame / 2u;
  std::vector<std::string> plainNotes;
  const RunObservation plain = steppedHost(rom, 4u * kFrame, InputScript{}, plainNotes);

  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun run{machine, rom};
  std::uint64_t spent = 0;
  while (spent < middle) spent += machine.step();
  std::vector<std::string> midNotes;
  const RunObservation mid = run.observation(midNotes);
  while (spent < 4u * kFrame) spent += machine.step();
  std::vector<std::string> notes;
  const RunObservation whole = run.finish(notes);
  expectSameObservation(plain, whole, "with an observation midway");
  EXPECT_EQ(notes, plainNotes);

  // A run finished at the same midpoint closes what was open there; the
  // observation taken at it holds none of that.
  Snes cut{SnesConfig{.rom = rom}};
  ObservedRun cutRun{cut, rom};
  std::uint64_t cutSpent = 0;
  while (cutSpent < middle) cutSpent += cut.step();
  std::vector<std::string> cutNotes;
  const RunObservation finished = cutRun.finish(cutNotes);
  std::uint64_t midWalks = 0;
  std::uint64_t finishedWalks = 0;
  for (const MovedRange& range : mid.moved) {
    if (range.kind == MovedKind::Table) midWalks += range.times;
    const auto same = std::find_if(finished.moved.begin(), finished.moved.end(),
                                   [&](const MovedRange& r) { return sameRange(r, range); });
    ASSERT_NE(same, finished.moved.end()) << "a range the observation holds closed by the midpoint";
    EXPECT_LE(range.times, same->times);
  }
  for (const MovedRange& range : finished.moved) {
    if (range.kind == MovedKind::Table) finishedWalks += range.times;
  }
  EXPECT_GT(midWalks, 0u);
  EXPECT_LT(midWalks, finishedWalks) << "the walks open at the midpoint are counted only once they close";
}

TEST(RomObservedRun, TheMainCpusNotesComeBeforeTheSoundCpus) {
  // The drawing cartridge with its directory's entry 0 pointed at $FF01, so
  // the key-on makes a note on the sound CPU, and its idle loop replaced by a
  // count of about seven frames, then a return to a stretch of work RAM the
  // tree does not hold, which makes a note on the main CPU after it.
  std::vector<std::uint8_t> rom = examples::drawingImage();
  rom[0x2780u] = 0x01u;  // entry 0: start $FF01
  rom[0x2781u] = 0xFFu;
  put(rom, 0x02AAu, {
      0xA2u, 0x00u,                    // $82AA LDX #$00
      0xA0u, 0x00u,                    // $82AC LDY #$00
      0x88u,                           // $82AE DEY
      0xD0u, 0xFDu,                    // $82AF BNE $82AE
      0xCAu,                           // $82B1 DEX
      0xD0u, 0xFAu,                    // $82B2 BNE $82AE
      0xA9u, 0x80u,                    // $82B4 LDA #$80
      0x8Fu, 0x00u, 0x10u, 0x7Eu,      // $82B6 STA $7E:1000
      0xA9u, 0xFEu,                    // $82BA LDA #$FE
      0x8Fu, 0x01u, 0x10u, 0x7Eu,      // $82BC STA $7E:1001: BRA $1000 in work RAM
      0xF4u, 0xFFu, 0x0Fu,             // $82C0 PEA $0FFF
      0x60u,                           // $82C3 RTS: to $00:1000
  });
  constexpr std::string_view kSound = "without an end flag";
  constexpr std::string_view kMain = "which the tree does not hold";

  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun run{machine, rom};
  std::uint64_t spent = 0;
  std::vector<std::string> early;
  while (spent < 12u * kFrame && !hasNote(early, kSound)) {
    const std::uint64_t until = spent + kFrame / 4u;
    while (spent < until) spent += machine.step();
    early.clear();
    static_cast<void>(run.observation(early));
  }
  ASSERT_TRUE(hasNote(early, kSound)) << "the key-on is noted";
  ASSERT_FALSE(hasNote(early, kMain)) << "before the main CPU's note is made";
  while (spent < 12u * kFrame) spent += machine.step();
  std::vector<std::string> notes;
  const RunObservation o = run.finish(notes);
  EXPECT_EQ(o.divergences, 0u);
  const auto at = [&](std::string_view text) {
    return std::find_if(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(text) != std::string::npos; });
  };
  ASSERT_NE(at(kMain), notes.end());
  ASSERT_NE(at(kSound), notes.end());
  EXPECT_LT(at(kMain), at(kSound)) << "the main CPU's first, though the sound CPU's was made first";
}

namespace {

// A cartridge for the reset line, in emulation mode throughout. Every pass
// starts a fill of sixty-four bytes from $9100 to VMDATAL on channel 0 and
// sets channel 1 up on a one-line HDMA table at $9200. A flag in work RAM,
// which a reset keeps, says which pass it is. The first enables the table from
// $8049, sets the flag and calls a helper that loads Y from $9007, reads
// $9000-$900F and spins; the pass after the reset enables the table from $8057,
// stores Y to VMDATAL, then $9008-$900A one at a time, a stream, and spins.
std::vector<std::uint8_t> resettingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0xA9u, 0x08u, 0x8Du, 0x00u, 0x43u,  // $8000 DMAP0 = $08: A->B, fixed source, pattern 0
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,  // $8005 BBAD0 = $18: VMDATAL
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,  // $800A A1T0 low
      0xA9u, 0x91u, 0x8Du, 0x03u, 0x43u,  // $800F A1T0 high: $9100
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,  // $8014 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,  // $8019 DAS0 low: 64
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,  // $801E DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,  // $8023 MDMAEN = $01 (the write at $8025)
      0xA9u, 0x00u, 0x8Du, 0x10u, 0x43u,  // $8028 DMAP1 = $00: direct, pattern 0
      0xA9u, 0x32u, 0x8Du, 0x11u, 0x43u,  // $802D BBAD1 = $32: COLDATA
      0xA9u, 0x00u, 0x8Du, 0x12u, 0x43u,  // $8032 A1T1 low
      0xA9u, 0x92u, 0x8Du, 0x13u, 0x43u,  // $8037 A1T1 high: $9200
      0xA9u, 0x00u, 0x8Du, 0x14u, 0x43u,  // $803C A1B1 = $00
      0xAFu, 0x00u, 0x00u, 0x7Eu,         // $8041 LDA $7E:0000: the flag
      0xD0u, 0x0Eu,                       // $8045 BNE $8055
      0xA9u, 0x02u, 0x8Du, 0x0Cu, 0x42u,  // $8047 HDMAEN = $02 (the write at $8049)
      0xA9u, 0x01u,                       // $804C LDA #$01
      0x8Fu, 0x00u, 0x00u, 0x7Eu,         // $804E STA $7E:0000
      0x20u, 0x80u, 0x80u,                // $8052 JSR $8080
      0xA9u, 0x02u, 0x8Du, 0x0Cu, 0x42u,  // $8055 HDMAEN = $02 (the write at $8057)
      0x8Cu, 0x18u, 0x21u,                // $805A STY $2118: Y as the reset left it
      0xADu, 0x08u, 0x90u,                // $805D LDA $9008
      0x8Du, 0x18u, 0x21u,                // $8060 STA $2118
      0xADu, 0x09u, 0x90u,                // $8063 LDA $9009
      0x8Du, 0x18u, 0x21u,                // $8066 STA $2118
      0xADu, 0x0Au, 0x90u,                // $8069 LDA $900A
      0x8Du, 0x18u, 0x21u,                // $806C STA $2118
      0x80u, 0xFEu,                       // $806F BRA $806F
  });
  put(rom, 0x0080u, {
      0xACu, 0x07u, 0x90u, // $8080 LDY $9007
      0xA2u, 0x00u,        // $8083 LDX #$00
      0xBDu, 0x00u, 0x90u, // $8085 LDA $9000,X
      0xE8u,               // $8088 INX
      0xE0u, 0x10u,        // $8089 CPX #$10
      0xD0u, 0xF8u,        // $808B BNE $8085
      0x80u, 0xFEu,        // $808D BRA $808D
  });
  for (std::size_t i = 0; i < 16; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x20u + i);
  rom[0x1100u] = 0x5Au;                   // the fill's byte
  put(rom, 0x1200u, {0x01u, 0x1Fu, 0x00u});  // one line of $1F, then the table's end
  return rom;
}

}  // namespace

TEST(RomObservedRun, TheResetLineIsTakenThroughTheRun) {
  const std::vector<std::uint8_t> rom = resettingImage();
  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun run{machine, rom};
  // Into the helper's spin, and a frame on, so the table is walked.
  std::uint64_t spent = 0;
  while (machine.state().cpu.pc != 0x808Du && spent < 2u * kFrame) spent += machine.step();
  ASSERT_EQ(machine.state().cpu.pc, 0x808Du) << "the helper spins";
  spent = 0;
  while (spent < kFrame + kFrame / 2u) spent += machine.step();
  const std::uint32_t framesBefore = run.frame();
  run.reset();
  EXPECT_EQ(run.frame(), framesBefore) << "a reset is not a wrap of the beam";

  // The fill was still open at the reset: closed there, it is counted.
  std::vector<std::string> atResetNotes;
  const RunObservation atReset = run.observation(atResetNotes);
  const std::vector<const MovedRange*> fillAtReset = rangesAt(atReset.moved, 0x008025u, 0);
  ASSERT_EQ(fillAtReset.size(), 1u) << "the range open at the reset is closed and counted";
  EXPECT_EQ(fillAtReset.front()->times, 1u);

  // The first step after the reset begins no frame: the beam stands at line 0.
  spent = machine.step();
  EXPECT_EQ(run.frame(), framesBefore) << "the step after the reset starts from the reset's own boundary";
  while (spent < 3u * kFrame) spent += machine.step();
  std::vector<std::string> notes;
  const RunObservation o = run.finish(notes);
  EXPECT_EQ(o.divergences, 0u);
  EXPECT_EQ(o.spc700Divergences, 0u);
  EXPECT_GT(o.spc700Instructions, 0u);
  EXPECT_EQ(noteWith(notes, "disagreed"), "");
  EXPECT_EQ(noteWith(notes, "not checked"), "");
  const std::vector<const MovedRange*> fill = rangesAt(o.moved, 0x008025u, 0);
  ASSERT_EQ(fill.size(), 1u);
  EXPECT_EQ(fill.front()->bytes, 64u);
  EXPECT_EQ(fill.front()->times, 2u) << "once before the reset and once after";
  // The table enabled after the reset is walked under that pass's write.
  EXPECT_FALSE(rangesAt(o.moved, 0x008049u, 1).empty());
  EXPECT_FALSE(rangesAt(o.moved, 0x008057u, 1).empty()) << "the table the second pass enabled is that pass's";

  // The helper never returned: the reset ended its invocation, so the stream
  // the second pass carried is lifted from the bytes the root read, not from
  // the helper's sixteen; and Y's value, loaded from $9007 before the reset,
  // carries no origin past it, so its store opens no stream there.
  ASSERT_EQ(o.streamed.size(), 1u);
  EXPECT_EQ(o.streamed.front().romOffset, 0x1008u);
  EXPECT_EQ(o.streamed.front().bytes, 3u);
  EXPECT_EQ(o.streamed.front().source, (ir::OriginInterval{0x1008u, 0x100Au}));
}

namespace {

// A cartridge whose reset vector names $8040, the address after the store that
// ends the first pass's stream, so the first pass flows on into it with no jump
// between. The first pass sets a flag in work RAM and stores $9000 and $9001
// to VMDATAL; the pass after the reset falls through the flag's test and
// stores $9002, the next byte, from another site.
std::vector<std::uint8_t> fallThroughResetImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  rom[0x7FC0u + 0x3Cu] = 0x40u;  // reset -> $8040
  put(rom, 0x0034u, {
      0xADu, 0x00u, 0x90u,        // $8034 LDA $9000
      0x8Du, 0x18u, 0x21u,        // $8037 STA $2118
      0xADu, 0x01u, 0x90u,        // $803A LDA $9001
      0x8Du, 0x18u, 0x21u,        // $803D STA $2118
      0xAFu, 0x00u, 0x00u, 0x7Eu, // $8040 LDA $7E:0000: the flag
      0xF0u, 0x0Au,               // $8044 BEQ $8050
      0xADu, 0x02u, 0x90u,        // $8046 LDA $9002
      0x8Du, 0x18u, 0x21u,        // $8049 STA $2118
      0x80u, 0xFEu,               // $804C BRA $804C
      0x00u, 0x00u,               // $804E
      0xA9u, 0x01u,               // $8050 LDA #$01
      0x8Fu, 0x00u, 0x00u, 0x7Eu, // $8052 STA $7E:0000
      0x4Cu, 0x34u, 0x80u,        // $8056 JMP $8034
  });
  for (std::size_t i = 0; i < 4; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x60u + i);
  return rom;
}

}  // namespace

TEST(RomObservedRun, AStreamOpenAtTheResetEndsThere) {
  // The reset comes with the program counter where falling through the last
  // store leads, so only the reset itself breaks the straight run: the
  // stream the first pass carried is two bytes, and the store after the reset
  // starts nothing of it.
  const std::vector<std::uint8_t> rom = fallThroughResetImage();
  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun run{machine, rom};
  std::uint64_t spent = 0;
  while (!(machine.state().cpu.pc == 0x8040u && machine.state().wram[0] == 1u) && spent < kFrame) {
    spent += machine.step();
  }
  ASSERT_EQ(machine.state().cpu.pc, 0x8040u);
  run.reset();
  spent = 0;
  while (spent < kFrame) spent += machine.step();
  std::vector<std::string> notes;
  const RunObservation o = run.finish(notes);
  EXPECT_EQ(o.divergences, 0u);
  ASSERT_EQ(o.streamed.size(), 1u);
  EXPECT_EQ(o.streamed.front().romOffset, 0x1000u);
  EXPECT_EQ(o.streamed.front().bytes, 2u) << "the store after the reset continues nothing";
}

TEST(RomObservedRun, AResetTakenWhileTheCpuWaitsLeavesItRunning) {
  // A program that waits for an interrupt it never enables: the reset ends the
  // wait, and the CPU runs from its reset vector into the same wait, checked.
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0xCBu, 0x80u, 0xFDu});  // $8000 WAI / $8001 BRA $8000
  Snes machine{SnesConfig{.rom = rom}};
  ObservedRun run{machine, rom};
  std::uint64_t spent = 0;
  while (spent < kFrame) spent += machine.step();
  ASSERT_EQ(machine.state().cpu.run, CpuRunState::Waiting);
  run.reset();
  EXPECT_EQ(machine.state().cpu.run, CpuRunState::Running);
  spent = 0;
  while (spent < 2u * kFrame) spent += machine.step();
  std::vector<std::string> notes;
  const RunObservation o = run.finish(notes);
  EXPECT_EQ(o.divergences, 0u);
  EXPECT_EQ(o.instructions, 2u) << "the WAI before the reset, and the one after it";
  EXPECT_EQ(noteWith(notes, "not checked"), "");
}

namespace {

// A sound boot program of the test's own, in place of the stub. It reads X
// before anything writes it. Before the reset, with the flag at $0300 clear,
// it writes a directory entry at $0400 naming a one-block sample at $0500,
// sets DIR to $04 and leaves the DSP's address at DIR, sets the flag, writes
// `BRA $FFC0` into the RAM under the window, sets X to $55, unmaps the window
// and spins there. After the reset, the window mapped again, it writes $04
// through $F3 to whatever register the address names, and keys voice 0 on
// without setting DIR.
std::array<std::uint8_t, kIplWindowBytes> resettingBootImage() {
  std::array<std::uint8_t, kIplWindowBytes> image{};
  const std::vector<std::uint8_t> code = {
      0x7Du,                    // $FFC0 MOV A,X
      0xE5u, 0x00u, 0x03u,      // $FFC1 MOV A,!$0300: the flag
      0xD0u, 0x28u,             // $FFC4 BNE $FFEE
      0xE8u, 0x05u,             // $FFC6 MOV A,#$05
      0xC5u, 0x01u, 0x04u,      // $FFC8 MOV !$0401,A: entry 0 starts at $0500
      0xC5u, 0x03u, 0x04u,      // $FFCB MOV !$0403,A: and loops there
      0xE8u, 0x01u,             // $FFCE MOV A,#$01
      0xC5u, 0x00u, 0x05u,      // $FFD0 MOV !$0500,A: a block with the end flag
      0xC5u, 0x00u, 0x03u,      // $FFD3 MOV !$0300,A: the flag
      0x8Fu, 0x5Du, 0xF2u,      // $FFD6 MOV $F2,#$5D: DIR
      0x8Fu, 0x04u, 0xF3u,      // $FFD9 MOV $F3,#$04: the directory at $0400
      0xE8u, 0x2Fu,             // $FFDC MOV A,#$2F
      0xC5u, 0xC0u, 0xFFu,      // $FFDE MOV !$FFC0,A
      0xE8u, 0xFEu,             // $FFE1 MOV A,#$FE
      0xC5u, 0xC1u, 0xFFu,      // $FFE3 MOV !$FFC1,A: BRA $FFC0 in the RAM beneath
      0xCDu, 0x55u,             // $FFE6 MOV X,#$55
      0x8Fu, 0x00u, 0xF1u,      // $FFE8 MOV $F1,#$00: the window unmapped
      0x5Fu, 0xC0u, 0xFFu,      // $FFEB JMP !$FFC0
      0x8Fu, 0x04u, 0xF3u,      // $FFEE MOV $F3,#$04
      0x8Fu, 0x4Cu, 0xF2u,      // $FFF1 MOV $F2,#$4C: KON
      0x8Fu, 0x01u, 0xF3u,      // $FFF4 MOV $F3,#$01: voice 0
      0x2Fu, 0xFEu,             // $FFF7 BRA $FFF7
  };
  std::copy(code.begin(), code.end(), image.begin());
  return image;
}

}  // namespace

TEST(RomObservedRun, TheResetLineGoesThroughTheSoundCpu) {
  // At sixteen points a CPU step apart, the reset finds the sound CPU in its
  // spin in RAM with X at $55 and the DSP's address at DIR. After it, every
  // sound instruction is checked from the registers the reset left, the
  // instruction at $FFC0 is the window's again, and the key-on reads the
  // directory the DSP reads, at $0000, not the $0400 the program set before.
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0x80u, 0xFEu});  // $8000 BRA $8000
  SnesConfig config;
  config.rom = rom;
  config.bootRom = resettingBootImage();
  for (unsigned offset = 0; offset < 16u; ++offset) {
    const std::string what = "reset after " + std::to_string(offset) + " more steps";
    Snes machine{config};
    ObservedRun run{machine, rom};
    std::uint64_t spent = 0;
    while (spent < kFrame) spent += machine.step();
    for (unsigned k = 0; k < offset; ++k) static_cast<void>(machine.step());
    ASSERT_EQ(machine.state().apu.cpu.x, 0x55u) << what;
    ASSERT_TRUE(machine.state().apu.cpu.pc >= 0xFFC0u && machine.state().apu.cpu.pc <= 0xFFC2u) << what << ": the spin in RAM";
    ASSERT_EQ(machine.state().apu.dspAddr, 0x5Du) << what;
    run.reset();
    spent = 0;
    while (spent < kFrame) spent += machine.step();
    std::vector<std::string> notes;
    const RunObservation o = run.finish(notes);
    EXPECT_EQ(o.spc700Divergences, 0u) << what;
    EXPECT_EQ(o.divergences, 0u) << what;
    EXPECT_EQ(noteWith(notes, "not checked"), "") << what;
    EXPECT_EQ(noteWith(notes, "disagreed"), "") << what;
    for (const KeyedSample& sample : o.samples) EXPECT_NE(sample.start, 0x0500u) << what << ": the directory before the reset";
    const bool keyed = std::any_of(o.samples.begin(), o.samples.end(), [](const KeyedSample& k) { return k.start == 0u; }) ||
                       hasNote(notes, "the sample at $0000 voice 0");
    EXPECT_TRUE(keyed) << what << ": the key-on after the reset names entry 0 of the directory at $0000";
  }
}

TEST(RomObservedRun, AnInterruptAtAJumpInEmulationModeIsNoWrongPointer) {
  // In emulation mode an interrupt pushes three bytes. The loop takes a jump
  // through a pointer in work RAM to a branch back to it, under a
  // vertical-blank interrupt every frame, so some interrupts are taken at the jump.
  std::vector<std::uint8_t> rom = examples::imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x0Eu, 0x85u, 0x10u,  // $8000 LDA #$0E / STA $10
      0xA9u, 0x80u, 0x85u, 0x11u,  // $8004 LDA #$80 / STA $11: the pointer at $0010 names $800E
      0x8Du, 0x00u, 0x42u,         // $8008 STA $4200: NMITIMEN = $80
      0x6Cu, 0x10u, 0x00u,         // $800B JMP (!$0010)
      0x80u, 0xFBu,                // $800E BRA $800B
  });
  std::vector<std::string> notes;
  const RunObservation o = observe(rom, 10u * kFrame, &notes);
  EXPECT_GT(o.interrupts, 5u);
  ASSERT_NE(reachedFrom(o.reached, 0x00800Bu), nullptr);
  EXPECT_EQ(reachedFrom(o.reached, 0x00800Bu)->target, 0x00800Eu);
  EXPECT_EQ(noteWith(notes, "but the CPU went to"), "");
}

TEST(RomObservedRun, AnHdmaEventAtAJumpIsNoWrongPointer) {
  // An HDMA table transferring on every line, and a loop through a jump: the
  // machine reports an HDMA event with the CPU standing at the jump, the
  // program counter where it was, and the jump is still ahead of it.
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,  // $8000 DMAP0 = $00: direct, pattern 0
      0xA9u, 0x32u, 0x8Du, 0x01u, 0x43u,  // $8005 BBAD0 = $32: COLDATA
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,  // $800A A1T0 low
      0xA9u, 0x90u, 0x8Du, 0x03u, 0x43u,  // $800F A1T0 high: $9000
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,  // $8014 A1B0 = $00
      0xA9u, 0x01u, 0x8Du, 0x0Cu, 0x42u,  // $8019 HDMAEN = $01
      0xA9u, 0x29u, 0x85u, 0x10u,         // $801E LDA #$29 / STA $10
      0xA9u, 0x80u, 0x85u, 0x11u,         // $8022 LDA #$80 / STA $11: the pointer names $8029
      0x6Cu, 0x10u, 0x00u,                // $8026 JMP (!$0010)
      0x80u, 0xFBu,                       // $8029 BRA $8026
  });
  rom[0x1000u] = 0xFFu;  // repeat, 127 lines
  rom[0x1080u] = 0xFFu;  // and 127 more; the bytes between are the values, then the end
  std::vector<std::string> notes;
  const RunObservation o = observe(rom, 3u * kFrame, &notes);
  ASSERT_NE(reachedFrom(o.reached, 0x008026u), nullptr);
  EXPECT_EQ(reachedFrom(o.reached, 0x008026u)->target, 0x008029u);
  EXPECT_EQ(noteWith(notes, "but the CPU went to"), "");
}

TEST(RomObservedRun, AStreamSeenAgainCarriesTheWidestSourceItsCarrierRead) {
  // The program carries $9000-$9001 to VMDATAL, ends the stream with a
  // constant, reads $9002-$9003, and carries the same two bytes again from the
  // same sites: the second closing finds the run grown to four bytes, and every
  // sighting of the stream is lifted as the four.
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0xA0u, 0x02u,          // $8000 LDY #$02: two passes
      0xADu, 0x00u, 0x90u,   // $8002 LDA $9000
      0x8Du, 0x18u, 0x21u,   // $8005 STA $2118
      0xADu, 0x01u, 0x90u,   // $8008 LDA $9001
      0x8Du, 0x18u, 0x21u,   // $800B STA $2118
      0xA9u, 0x00u,          // $800E LDA #$00
      0x8Du, 0x18u, 0x21u,   // $8010 STA $2118: the stream ends
      0xADu, 0x02u, 0x90u,   // $8013 LDA $9002
      0xADu, 0x03u, 0x90u,   // $8016 LDA $9003: the run grows
      0x88u,                 // $8019 DEY
      0xD0u, 0xE6u,          // $801A BNE $8002
      0x80u, 0xFEu,          // $801C BRA $801C
  });
  for (std::size_t i = 0; i < 4; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x70u + i);
  const RunObservation o = observe(rom, kFrame);
  ASSERT_FALSE(o.streamed.empty());
  std::uint32_t sightings = 0;
  for (const StreamedRange& stream : o.streamed) {
    EXPECT_EQ(stream.romOffset, 0x1000u);
    EXPECT_EQ(stream.source, (ir::OriginInterval{0x1000u, 0x1003u}));
    sightings += stream.times;
  }
  EXPECT_EQ(sightings, 2u);
}

TEST(RomObservedRun, AStagedRangesWritersAreMostBytesFirst) {
  const InputScript script = exampleScript();
  std::size_t mixed = 0;
  for (const examples::Example& example : examples::examples()) {
    std::vector<std::string> notes;
    const RunObservation o = observeRun(example.build(), 12u * kFrame, script, notes);
    for (const StagedRange& range : o.staged) {
      for (std::size_t w = 1; w < range.writers.size(); ++w) {
        const StagedWriter& a = range.writers[w - 1];
        const StagedWriter& b = range.writers[w];
        EXPECT_GE(a.bytes, b.bytes) << example.name;
        if (a.bytes != b.bytes) ++mixed;
      }
    }
  }
  EXPECT_GT(mixed, 0u) << "an extent two writers wrote unequal shares of";
}

// The recorder holds the port's own access only until the access that caused
// it, in the same cycle; at every boundary step() returns at it holds none.
TEST(RomObservedRun, AtEveryBoundaryTheRecorderHoldsNoPortAccess) {
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    const CartridgeBoard board = cartridgeBoard(rom);
    Snes machine{SnesConfig{.rom = rom}};
    ir::Provenance shadow{board, rom.size(), 64};
    detail::Recorder recorder{machine, shadow};
    shadow.carries = &recorder;
    machine.setObserver(&recorder);
    std::uint64_t spent = 0;
    std::uint64_t held = 0;
    while (spent < 6u * kFrame) {
      const Cpu65816State& cpu = machine.state().cpu;
      recorder.stepBegan((static_cast<Address>(cpu.pbr) << 16) | cpu.pc);
      spent += machine.step();
      if (recorder.port) ++held;
    }
    machine.setObserver(nullptr);
    shadow.carries = nullptr;
    EXPECT_EQ(held, 0u) << example.name;
  }
}

// The origin an engine carries between its read and its write is set by the
// read of the byte it then writes, so one left over at a boundary — a table's
// header read last — reaches no byte: the engines' writes land the same
// origins with it overwritten at every boundary as without.
TEST(RomObservedRun, AnOriginLeftCarriedAtABoundaryReachesNothing) {
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    const CartridgeBoard board = cartridgeBoard(rom);
    const auto runOnce = [&](bool stale) {
      Snes machine{SnesConfig{.rom = rom}};
      ir::Provenance shadow{board, rom.size(), 64};
      const ir::Origin bogus = shadow.origins().hardwareRegister(0x21FFu);
      detail::Recorder recorder{machine, shadow};
      shadow.carries = &recorder;
      machine.setObserver(&recorder);
      std::uint64_t spent = 0;
      while (spent < 6u * kFrame) {
        const Cpu65816State& cpu = machine.state().cpu;
        recorder.stepBegan((static_cast<Address>(cpu.pbr) << 16) | cpu.pc);
        if (stale) recorder.carried = bogus;
        spent += machine.step();
      }
      machine.setObserver(nullptr);
      shadow.carries = nullptr;
      std::vector<ir::OriginSet> origins;
      for (std::size_t i = 0; i < 0x20000u; ++i) {
        origins.push_back(shadow.origins().of(shadow.originOf(ir::Provenance::workRamAddress(i)).value_or(ir::kNoOrigin)));
      }
      return origins;
    };
    EXPECT_TRUE(runOnce(false) == runOnce(true)) << example.name;
  }
}

}  // namespace snaggletooth::disasm
