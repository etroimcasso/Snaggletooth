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

#include "cartridge_fixtures.h"
#include "gtest/gtest.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"
#include "rom/rom_verify.h"

namespace snaggletooth::disasm {
namespace {

using fixtures::loRomImage;
using fixtures::put;

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

// A two-bank cartridge that dispatches through every indirect form, each to a
// target nothing else names. Every site the cases name is commented with its
// address.
//
// Bank $00 (file $0000): reset switches to native mode, enables the vblank NMI,
// copies a `JML [!$8109]` into work RAM and runs it from there, then dispatches
// through a table with X, calls through the same table, jumps through a plain
// pointer, and jumps long through a three-byte pointer into bank $01. Bank $01
// takes one indirect jump sixty-five thousand times — long enough for NMIs to
// land on the jump itself — then stops.
std::vector<std::uint8_t> dispatchingImage() {
  std::vector<std::uint8_t> rom = loRomImage(2);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x2A] = 0x00u;  // native NMI -> $8300
  rom[site + 0x2B] = 0x83u;

  // The pointers, at $00:8100.
  put(rom, 0x0100u, {0x00u, 0x82u,          // $8100: -> $8200 (the routine)
                     0x10u, 0x82u,          // $8102: -> $8210 (the table's second target)
                     0x20u, 0x82u,          // $8104: -> $8220
                     0x00u, 0x80u, 0x01u,   // $8106: -> $01:8000 (three bytes)
                     0x30u, 0x82u, 0x00u,   // $8109: -> $00:8230 (three bytes; the copy in RAM jumps here)
                     0x30u, 0x80u});        // $810C: -> $8030 (bank $01's loop, read in bank zero)
  put(rom, 0x0000u, {
                        0x18u, 0xFBu,                // $8000 CLC / XCE      -> native
                        0xE2u, 0x30u,                // $8002 SEP #$30       -> A8, X8
                        0xA9u, 0x80u,                // $8004 LDA #$80
                        0x8Du, 0x00u, 0x42u,         // $8006 STA !$4200     NMI on
                        0xA9u, 0xDCu,                // $8009 LDA #$DC       `JML [!$8109]` -> $7E:2000
                        0x8Fu, 0x00u, 0x20u, 0x7Eu,  // $800B STA $7E:2000
                        0xA9u, 0x09u,                // $800F LDA #$09
                        0x8Fu, 0x01u, 0x20u, 0x7Eu,  // $8011 STA $7E:2001
                        0xA9u, 0x81u,                // $8015 LDA #$81
                        0x8Fu, 0x02u, 0x20u, 0x7Eu,  // $8017 STA $7E:2002
                        0x5Cu, 0x00u, 0x20u, 0x7Eu,  // $801B JML $7E:2000   run the copy
                    });
  put(rom, 0x0230u, {                                // $8230, reached from the copy in RAM
                        0xA2u, 0x02u,                // $8230 LDX #$02
                        0x7Cu, 0x00u, 0x81u,         // $8232 JMP (!$8100,X) -> $8210
                    });
  put(rom, 0x0210u, {                                // $8210
                        0xA2u, 0x00u,                // $8210 LDX #$00
                        0xFCu, 0x00u, 0x81u,         // $8212 JSR (!$8100,X) -> $8200, returns
                        0x6Cu, 0x04u, 0x81u,         // $8215 JMP (!$8104)   -> $8220
                    });
  put(rom, 0x0200u, {0xEAu, 0x60u});                 // $8200 NOP / RTS
  put(rom, 0x0220u, {                                // $8220
                        0xDCu, 0x06u, 0x81u,         // $8220 JML [!$8106]   -> $01:8000
                    });
  put(rom, 0x0300u, {0x40u});                        // $8300 RTI   (the NMI handler)
  // Bank $01 (file $8000). Its first jump is `(!abs,X)` through a table at
  // $01:8100 — the same offset as bank $00's table, which names a different
  // target, so a pointer read in the wrong bank lands in the wrong place.
  put(rom, 0x8000u, {
                        0xA2u, 0x00u,                // $01:8000 LDX #$00
                        0x7Cu, 0x00u, 0x81u,         // $01:8002 JMP (!$8100,X) -> $01:8020
                    });
  put(rom, 0x8100u, {0x20u, 0x80u});                 // $01:8100: -> $8020 (bank $00's $8100 -> $8200)
  put(rom, 0x8020u, {
                        0xC2u, 0x10u,                // $01:8020 REP #$10    -> X16
                        0xA0u, 0xFFu, 0xFFu,         // $01:8022 LDY #$FFFF
                        0x6Cu, 0x0Cu, 0x81u,         // $01:8025 JMP (!$810C) -> $01:8030, every time
                    });
  put(rom, 0x8030u, {
                        0x88u,                       // $01:8030 DEY
                        0xD0u, 0xF2u,                // $01:8031 BNE $01:8025  (-14 from $8033)
                        0xDBu,                       // $01:8033 STP
                    });
  return rom;
}

// A cartridge that starts a DMA transfer and takes an indirect jump on the very
// next instruction: the CPU is held off the bus while the transfer runs, so the
// step that meets the jump may run nothing at all before the jump does.
std::vector<std::uint8_t> stalledJumpImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0100u, {0x20u, 0x82u});          // $8100: -> $8220
  put(rom, 0x0000u, {
                        0x18u, 0xFBu,                // $8000 CLC / XCE
                        0xE2u, 0x30u,                // $8002 SEP #$30
                        0xA9u, 0x00u,                // $8004 LDA #$00
                        0x8Du, 0x00u, 0x43u,         // $8006 STA !$4300   DMAP0: A to B, one register
                        0xA9u, 0x80u,                // $8009 LDA #$80
                        0x8Du, 0x01u, 0x43u,         // $800B STA !$4301   BBAD0: WMDATA
                        0x9Cu, 0x02u, 0x43u,         // $800E STZ !$4302   A1T0L
                        0xA9u, 0x80u,                // $8011 LDA #$80
                        0x8Du, 0x03u, 0x43u,         // $8013 STA !$4303   A1T0H: source $00:8000
                        0x9Cu, 0x04u, 0x43u,         // $8016 STZ !$4304   A1B0
                        0x9Cu, 0x05u, 0x43u,         // $8019 STZ !$4305   DAS0L
                        0xA9u, 0x10u,                // $801C LDA #$10
                        0x8Du, 0x06u, 0x43u,         // $801E STA !$4306   DAS0H: 4096 bytes
                        0xA9u, 0x01u,                // $8021 LDA #$01
                        0x8Du, 0x0Bu, 0x42u,         // $8023 STA !$420B   MDMAEN: go
                        0x6Cu, 0x00u, 0x81u,         // $8026 JMP (!$8100) -> $8220, the CPU stalled
                    });
  put(rom, 0x0220u, {0xDBu});                        // $8220 STP
  return rom;
}

// A cartridge whose one indirect jump reads its pointer from a register.
std::vector<std::uint8_t> unreadablePointerImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0x6Cu, 0x40u, 0x21u,   // $8000 JMP (!$2140)  the APU port is not memory
                     0xDBu});               // $8003 STP
  return rom;
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
  return observeRun(rom, cycles, notes ? *notes : local);
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
