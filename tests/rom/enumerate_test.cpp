// The toolkit runs a loader once for every entry its index can select. The
// `enumerating` cartridge calls four loaders for only some of their entries;
// the cases pin which routines are planned and why the others are not, which
// registers are inputs and at what width, the second run's snapshot,
// footprint and self-check, every way a value is kept or refused, and what
// the manifest and the files then say. A few cases build an image of their
// own where the cartridge has no instance of what they pin.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/ir_dataflow.h"
#include "rom/rom_disasm.h"
#include "rom/rom_enumerate.h"
#include "rom/rom_observe.h"
#include "rom/rom_observe_internal.h"
#include "rom/rom_text.h"

namespace snaggletooth::disasm {
namespace {

using examples::enumeratingImage;
using examples::loRomImage;
using examples::put;

constexpr std::uint64_t kSecond = 21'477'272u;  // one second of the master clock

// The cartridge disassembled with a run of one second.
CartridgeDisassembly disassembled(const std::vector<std::uint8_t>& rom) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = kSecond;
  return disassembleCartridge(request);
}

// The `enumerating` cartridge, disassembled once for every case that reads it.
const std::vector<std::uint8_t>& enumeratingRom() {
  static const std::vector<std::uint8_t> rom = enumeratingImage();
  return rom;
}
const CartridgeDisassembly& enumerating() {
  static const CartridgeDisassembly out = disassembled(enumeratingRom());
  return out;
}

struct PlanAndNotes {
  EnumerationPlan plan;
  std::vector<std::string> notes;
};

// The plan over a disassembly, with the notes it wrote.
PlanAndNotes planOf(const CartridgeDisassembly& disassembly, std::span<const std::uint8_t> rom) {
  const ProvenProgram proven = proveProgram(disassembly, rom);
  PlanAndNotes out;
  out.plan = enumerationPlan(disassembly, proven, out.notes);
  return out;
}

const PlannedRoutine* routineAt(const EnumerationPlan& plan, Address address) {
  const auto found = std::find_if(plan.routines.begin(), plan.routines.end(),
                                  [&](const PlannedRoutine& r) { return r.address == address; });
  return found == plan.routines.end() ? nullptr : &*found;
}

bool hasNote(const std::vector<std::string>& notes, const std::string& note) {
  return std::find(notes.begin(), notes.end(), note) != notes.end();
}

std::vector<std::uint32_t> evens(std::uint32_t count) {
  std::vector<std::uint32_t> out;
  for (std::uint32_t i = 0; i < count; ++i) out.push_back(2u * i);
  return out;
}

// ---- the plan -------------------------------------------------------------------

// The writers of every staged extent and the routines that call them: the
// four loaders whose index a caller bounds are planned; the writer whose index
// it loads itself, the reset routine, the routine reset jumps to, the fifth
// loader's X and the vertical-blank handler are each named once, in address
// order.
TEST(RomEnumerate, TheWritersAndTheirCallersArePlanned) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  std::vector<std::string> labels;
  for (const PlannedRoutine& routine : planned.plan.routines) labels.push_back(routine.label);
  EXPECT_EQ(labels, (std::vector<std::string>{"sub_008100", "sub_008800", "sub_008B00", "sub_008D00"}));
  EXPECT_EQ(planned.notes,
            (std::vector<std::string>{
                "enumerate reset: entered by a jump, no return to end at; not run",
                "enumerate sub_008500: no input every path bounds; not run",
                "enumerate loc_008700: entered by a jump, no return to end at; not run",
                "enumerate sub_008D00: X is bounded on every path and read on none before it is written; not an input",
                "enumerate nmi_native: entered by a jump, no return to end at; not run",
            }));
}

// A routine only a jump enters has no return the snapshot can name to end a
// value's run at.
TEST(RomEnumerate, ARoutineNoCallEntersIsNotRun) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  EXPECT_EQ(routineAt(planned.plan, 0x008700u), nullptr);
  EXPECT_TRUE(hasNote(planned.notes, "enumerate loc_008700: entered by a jump, no return to end at; not run"));
}

// A site no routine holds — code the CPU ran in work RAM — has no routine to run.
TEST(RomEnumerate, ABareSiteIsNotRun) {
  CartridgeDisassembly disassembly = enumerating();
  ASSERT_FALSE(disassembly.staged.empty());
  disassembly.staged.front().writers.push_back(StagedWriter{.writer = ir::Writer{.site = 0x7E1000u, .engine = false},
                                                            .unwritten = false,
                                                            .bytes = 1,
                                                            .origin = {},
                                                            .sources = {}});
  const PlanAndNotes planned = planOf(disassembly, enumeratingRom());
  ASSERT_FALSE(planned.notes.empty());
  EXPECT_EQ(planned.notes.front(), "enumerate $7E:1000: no routine holds the site; not run");
  EXPECT_EQ(planned.plan.routines.size(), 4u) << "the routines are planned as before";
}

// A routine the analysis reaches by no path has no state at its entry.
TEST(RomEnumerate, ARoutineNoPathReachesIsNotRun) {
  const CartridgeDisassembly& disassembly = enumerating();
  const std::vector<std::uint8_t>& rom = enumeratingRom();
  const CartridgeBoard board = disassembly.board;
  ProvenProgram proven;
  proven.image = [board, &rom](ir::Address address) -> std::optional<std::uint8_t> {
    const std::optional<std::size_t> offset = romOffset(board, address, rom.size());
    if (!offset) return std::nullopt;
    return rom[*offset];
  };
  const ir::Canonical canonical = [board, &rom](ir::Address address) -> std::optional<ir::Address> {
    const std::optional<std::size_t> offset = romOffset(board, address, rom.size());
    if (!offset) return std::nullopt;
    return romAddress(board.map, *offset);
  };
  proven.flow = std::make_unique<ir::Dataflow>(disassembly.program, std::vector<ir::FlowEntry>{},
                                               std::vector<ir::Sighting>{}, proven.image, canonical);
  std::vector<std::string> notes;
  const EnumerationPlan plan = enumerationPlan(disassembly, proven, notes);
  EXPECT_TRUE(plan.routines.empty());
  for (const char* label : {"sub_008100", "sub_008500", "sub_008800", "sub_008B00"}) {
    EXPECT_TRUE(hasNote(notes, std::string("enumerate ") + label +
                                   ": no path the analysis follows reaches its entry; not run"))
        << label;
  }
}

// An index the caller masks and doubles before the call: eight values at the
// sixteen bits the entry's mode gives X.
TEST(RomEnumerate, AnIndexMaskedInTheCallerIsAnInput) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  const PlannedRoutine* loader = routineAt(planned.plan, 0x008100u);
  ASSERT_NE(loader, nullptr);
  ASSERT_EQ(loader->inputs.size(), 1u);
  EXPECT_EQ(loader->inputs[0].reg, InputRegister::X);
  EXPECT_EQ(loader->inputs[0].bits, 16u);
  EXPECT_EQ(loader->inputs[0].values, evens(8));
}

// An index the caller compares against four and branches past on the carry:
// the four values below it.
TEST(RomEnumerate, AnIndexComparedInTheCallerIsAnInput) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  const PlannedRoutine* loader = routineAt(planned.plan, 0x008800u);
  ASSERT_NE(loader, nullptr);
  ASSERT_EQ(loader->inputs.size(), 1u);
  EXPECT_EQ(loader->inputs[0].reg, InputRegister::A);
  EXPECT_EQ(loader->inputs[0].values, (std::vector<std::uint32_t>{0, 1, 2, 3}));
}

// An index the routine loads from work RAM itself bounds nothing at its entry.
TEST(RomEnumerate, AnIndexLoadedFromWorkRamIsNoInput) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  EXPECT_EQ(routineAt(planned.plan, 0x008500u), nullptr);
  EXPECT_TRUE(hasNote(planned.notes, "enumerate sub_008500: no input every path bounds; not run"));
}

// The width is the entry's: A at eight bits for the loader entered with an
// eight-bit accumulator, at sixteen for the one entered with a sixteen-bit one.
TEST(RomEnumerate, TheWidthIsTheEntryModes) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  const PlannedRoutine* narrow = routineAt(planned.plan, 0x008B00u);
  const PlannedRoutine* wide = routineAt(planned.plan, 0x008800u);
  ASSERT_NE(narrow, nullptr);
  ASSERT_NE(wide, nullptr);
  ASSERT_EQ(narrow->inputs.size(), 1u);
  EXPECT_EQ(narrow->inputs[0].reg, InputRegister::A);
  EXPECT_EQ(narrow->inputs[0].bits, 8u);
  EXPECT_EQ(narrow->inputs[0].values, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  ASSERT_EQ(wide->inputs.size(), 1u);
  EXPECT_EQ(wide->inputs[0].bits, 16u);
}

// A writer entered with the direct register and the data bank each bounded to
// several values, and A, X and Y each one value: no input.
std::vector<std::uint8_t> boundedDirectImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE       -> native
      0xC2u, 0x30u,                // $8002 REP #$30
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $8004 LDA $7E:0010
      0x29u, 0x03u, 0x00u,         // $8008 AND #$0003
      0x5Bu,                       // $800B TCD             D in {0, 1, 2, 3}
      0xE2u, 0x20u,                // $800C SEP #$20
      0xAFu, 0x12u, 0x00u, 0x7Eu,  // $800E LDA $7E:0012
      0x29u, 0x01u,                // $8012 AND #$01
      0x48u,                       // $8014 PHA
      0xABu,                       // $8015 PLB             DBR in {0, 1}
      0xC2u, 0x30u,                // $8016 REP #$30
      0xA9u, 0x00u, 0x00u,         // $8018 LDA #$0000
      0xA2u, 0x00u, 0x00u,         // $801B LDX #$0000
      0xA0u, 0x00u, 0x00u,         // $801E LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8021 JSR !$8100
      0x80u, 0xFEu,                // $8024 BRA *
  });
  put(rom, 0x0100u, {
      0xE2u, 0x20u,                // $8100 SEP #$20        sub_008100
      0xAFu, 0x00u, 0x90u, 0x00u,  // $8102 LDA $00:9000
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8106 STA $7F:0000
      0xA9u, 0x00u,                // $810A LDA #$00
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $810C STA $00:4300    DMAP0
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8110 STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $8114 STA $00:4303    A1T0 high
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $8118 STA $00:4306    DAS0 high
      0xA9u, 0x22u,                // $811C LDA #$22
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $811E STA $00:4301    BBAD0 = CGDATA
      0xA9u, 0x7Fu,                // $8122 LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $8124 STA $00:4304    A1B0
      0xA9u, 0x01u,                // $8128 LDA #$01
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $812A STA $00:4305    DAS0 low: 1
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $812E STA $00:420B    MDMAEN
      0x60u,                       // $8132 RTS
  });
  rom[0x1000u] = 0x5Au;  // $9000
  return rom;
}

TEST(RomEnumerate, DAndDbrAreNeverInputs) {
  const std::vector<std::uint8_t> rom = boundedDirectImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const ProvenProgram proven = proveProgram(disassembly, rom);
  const ir::State* entry = proven.flow->before(Address{0x008100u});
  ASSERT_NE(entry, nullptr);
  ASSERT_TRUE(entry->registers.d.known);
  EXPECT_EQ(entry->registers.d.values.size(), 4u) << "every path bounds the direct register";
  ASSERT_TRUE(entry->registers.dbr.known);
  EXPECT_EQ(entry->registers.dbr.values.size(), 2u) << "and the data bank";
  std::vector<std::string> notes;
  const EnumerationPlan plan = enumerationPlan(disassembly, proven, notes);
  EXPECT_TRUE(plan.routines.empty());
  EXPECT_TRUE(hasNote(notes, "enumerate sub_008100: no input every path bounds; not run"));
}

// The fifth loader is entered with X bounded to eight values by the handler's
// mask, and writes X with `TAX` before anything reads it: X is noted and not
// set, and A, which `ASL A` reads first, is its one input.
TEST(RomEnumerate, ARegisterNoPathReadsIsNoInput) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  EXPECT_TRUE(hasNote(planned.notes, "enumerate sub_008D00: X is bounded on every path and read on none before it "
                                     "is written; not an input"));
  const PlannedRoutine* loader = routineAt(planned.plan, 0x008D00u);
  ASSERT_NE(loader, nullptr);
  ASSERT_EQ(loader->inputs.size(), 1u);
  EXPECT_EQ(loader->inputs[0].reg, InputRegister::A);
  EXPECT_EQ(loader->inputs[0].bits, 16u);
  EXPECT_EQ(loader->inputs[0].values, (std::vector<std::uint32_t>{0, 1, 2, 3, 4}));
}

// A register an instruction reads before any writes it is an input: the
// fifth loader's A, read by `ASL A`, and the first loader's X, read by the
// table load's index.
TEST(RomEnumerate, ARegisterReadBeforeItIsWrittenIsAnInput) {
  const PlanAndNotes planned = planOf(enumerating(), enumeratingRom());
  const PlannedRoutine* fifth = routineAt(planned.plan, 0x008D00u);
  const PlannedRoutine* first = routineAt(planned.plan, 0x008100u);
  ASSERT_NE(fifth, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(fifth->inputs.size(), 1u);
  EXPECT_EQ(fifth->inputs[0].reg, InputRegister::A);
  ASSERT_EQ(first->inputs.size(), 1u);
  EXPECT_EQ(first->inputs[0].reg, InputRegister::X);
  for (const std::string& note : planned.notes) {
    EXPECT_EQ(note.find("sub_008100: X is bounded"), std::string::npos) << note;
    EXPECT_EQ(note.find("sub_008D00: A is bounded"), std::string::npos) << note;
  }
}

// The tail every loader built below ends with: `bytes` bytes from `$7F:0000`
// to the palette on channel 0, then the accumulator back at sixteen bits;
// forty-four bytes of code.
void putPaletteSend(std::vector<std::uint8_t>& rom, std::size_t at, std::uint8_t bytes = 1) {
  put(rom, at, {
      0xA9u, 0x00u,                // LDA #$00
      0x8Fu, 0x00u, 0x43u, 0x00u,  // STA $00:4300    DMAP0
      0x8Fu, 0x02u, 0x43u, 0x00u,  // STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // STA $00:4303    A1T0 high
      0x8Fu, 0x06u, 0x43u, 0x00u,  // STA $00:4306    DAS0 high
      0xA9u, 0x22u,                // LDA #$22
      0x8Fu, 0x01u, 0x43u, 0x00u,  // STA $00:4301    BBAD0 = CGDATA
      0xA9u, 0x7Fu,                // LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // STA $00:4304    A1B0
      0xA9u, bytes,                // LDA #bytes
      0x8Fu, 0x05u, 0x43u, 0x00u,  // STA $00:4305    DAS0 low
      0xA9u, 0x01u,                // LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // STA $00:420B    MDMAEN
      0xC2u, 0x20u,                // REP #$20
  });
}

// Reset bounding X to four values with a mask and calling `$8100`, A16 X16.
void putXBoundingReset(std::vector<std::uint8_t>& rom) {
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $800C LDA $7E:0010
      0x29u, 0x03u, 0x00u,         // $8010 AND #$0003
      0xAAu,                       // $8013 TAX             X in {0, 1, 2, 3}
      0xA9u, 0x00u, 0x00u,         // $8014 LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $8017 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $801A JSR !$8100
      0x80u, 0xFEu,                // $801D BRA *
  });
}

// A loader bounded in X that saves X on the stack, builds its byte from a
// constant address and restores X, never indexing by it: the push reads X.
std::vector<std::uint8_t> pushingLoaderImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  putXBoundingReset(rom);
  put(rom, 0x0100u, {
      0xDAu,                       // $8100 PHX             sub_008100
      0xE2u, 0x20u,                // $8101 SEP #$20
      0xAFu, 0x00u, 0x90u, 0x00u,  // $8103 LDA $00:9000
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8107 STA $7F:0000
  });
  putPaletteSend(rom, 0x010Bu);   // $810B
  put(rom, 0x0137u, {
      0xFAu,                       // $8137 PLX
      0x60u,                       // $8138 RTS
  });
  rom[0x1000u] = 0x5Au;  // $9000
  return rom;
}

TEST(RomEnumerate, APushedRegisterIsRead) {
  const std::vector<std::uint8_t> rom = pushingLoaderImage();
  const PlanAndNotes planned = planOf(disassembled(rom), rom);
  const PlannedRoutine* loader = routineAt(planned.plan, 0x008100u);
  ASSERT_NE(loader, nullptr) << (planned.notes.empty() ? "" : planned.notes.front());
  ASSERT_EQ(loader->inputs.size(), 1u);
  EXPECT_EQ(loader->inputs[0].reg, InputRegister::X);
  EXPECT_EQ(loader->inputs[0].values, (std::vector<std::uint32_t>{0, 1, 2, 3}));
}

// A loader bounded in X whose first instruction calls a routine that compares
// X: what the callee reads at its entry, the call reads.
std::vector<std::uint8_t> callingLoaderImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  putXBoundingReset(rom);
  put(rom, 0x0100u, {
      0x20u, 0x00u, 0x82u,         // $8100 JSR !$8200      sub_008100
      0xE2u, 0x20u,                // $8103 SEP #$20
      0xAFu, 0x00u, 0x90u, 0x00u,  // $8105 LDA $00:9000
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8109 STA $7F:0000
  });
  putPaletteSend(rom, 0x010Du);   // $810D
  put(rom, 0x0139u, {0x60u});      // $8139 RTS
  put(rom, 0x0200u, {
      0xE0u, 0x02u, 0x00u,         // $8200 CPX #$0002      sub_008200: reads X
      0x60u,                       // $8203 RTS
  });
  rom[0x1000u] = 0x5Au;  // $9000
  return rom;
}

TEST(RomEnumerate, ACalleesReadsAreTheCallers) {
  const std::vector<std::uint8_t> rom = callingLoaderImage();
  const PlanAndNotes planned = planOf(disassembled(rom), rom);
  const PlannedRoutine* loader = routineAt(planned.plan, 0x008100u);
  ASSERT_NE(loader, nullptr) << (planned.notes.empty() ? "" : planned.notes.front());
  ASSERT_EQ(loader->inputs.size(), 1u);
  EXPECT_EQ(loader->inputs[0].reg, InputRegister::X);
}

// A loader bounded in A at sixteen bits that writes the accumulator's low
// byte at eight, then swaps the halves to take its index from the high byte:
// the eight-bit write leaves the high byte as it came in, so A is an input.
std::vector<std::uint8_t> highByteLoaderImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $800C LDA $7E:0010
      0x29u, 0x00u, 0x03u,         // $8010 AND #$0300      A in {$0000, $0100, $0200, $0300}
      0xA2u, 0x00u, 0x00u,         // $8013 LDX #$0000
      0xA0u, 0x00u, 0x00u,         // $8016 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8019 JSR !$8100
      0x80u, 0xFEu,                // $801C BRA *
  });
  put(rom, 0x0100u, {
      0xE2u, 0x20u,                // $8100 SEP #$20        sub_008100
      0xA9u, 0x00u,                // $8102 LDA #$00        the low byte alone
      0xEBu,                       // $8104 XBA             the index into the low byte
      0xC2u, 0x20u,                // $8105 REP #$20
      0x29u, 0xFFu, 0x00u,         // $8107 AND #$00FF
      0x0Au,                       // $810A ASL A
      0xAAu,                       // $810B TAX
      0xE2u, 0x20u,                // $810C SEP #$20
      0xBFu, 0x00u, 0x90u, 0x00u,  // $810E LDA $00:9000,X
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8112 STA $7F:0000
  });
  putPaletteSend(rom, 0x0116u);   // $8116
  put(rom, 0x0142u, {0x60u});      // $8142 RTS
  for (std::size_t i = 0; i < 8; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x50u + i);  // $9000
  return rom;
}

TEST(RomEnumerate, AnEightBitWriteToTheAccumulatorLeavesItsHighByteLive) {
  const std::vector<std::uint8_t> rom = highByteLoaderImage();
  const PlanAndNotes planned = planOf(disassembled(rom), rom);
  const PlannedRoutine* loader = routineAt(planned.plan, 0x008100u);
  ASSERT_NE(loader, nullptr) << (planned.notes.empty() ? "" : planned.notes.front());
  ASSERT_EQ(loader->inputs.size(), 1u);
  EXPECT_EQ(loader->inputs[0].reg, InputRegister::A);
  EXPECT_EQ(loader->inputs[0].bits, 16u);
  EXPECT_EQ(loader->inputs[0].values, (std::vector<std::uint32_t>{0x0000, 0x0100, 0x0200, 0x0300}));
}

// A cartridge that copies sixteen bytes from `$9000` to `$7E:2000`, sets
// channel 1 up to send all sixteen to the palette, waits for the vertical
// blank, and starts it; the handler sets the channel's count to eight before
// the start, so the run moves eight bytes from that start where the code
// proves sixteen.
std::vector<std::uint8_t> shortenedTransferImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  rom[0x7FC0u + 0x2Au] = 0x00u;  // native NMI -> $8E00
  rom[0x7FC0u + 0x2Bu] = 0x8Eu;
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xE2u, 0x30u,                // $8002 SEP #$30
      0xA2u, 0x00u,                // $8004 LDX #$00
      0xBFu, 0x00u, 0x90u, 0x00u,  // $8006 LDA $00:9000,X
      0x9Fu, 0x00u, 0x20u, 0x7Eu,  // $800A STA $7E:2000,X
      0xE8u,                       // $800E INX
      0xE0u, 0x10u,                // $800F CPX #$10
      0xD0u, 0xF3u,                // $8011 BNE $8006
      0xA9u, 0x80u,                // $8013 LDA #$80
      0x8Fu, 0x00u, 0x42u, 0x00u,  // $8015 STA $00:4200    NMITIMEN
      0xA9u, 0x00u,                // $8019 LDA #$00
      0x8Fu, 0x10u, 0x43u, 0x00u,  // $801B STA $00:4310    DMAP1
      0xA9u, 0x22u,                // $801F LDA #$22
      0x8Fu, 0x11u, 0x43u, 0x00u,  // $8021 STA $00:4311    BBAD1 = CGDATA
      0xA9u, 0x00u,                // $8025 LDA #$00
      0x8Fu, 0x12u, 0x43u, 0x00u,  // $8027 STA $00:4312    A1T1 low
      0xA9u, 0x20u,                // $802B LDA #$20
      0x8Fu, 0x13u, 0x43u, 0x00u,  // $802D STA $00:4313    A1T1 high: $2000
      0xA9u, 0x7Eu,                // $8031 LDA #$7E
      0x8Fu, 0x14u, 0x43u, 0x00u,  // $8033 STA $00:4314    A1B1
      0xA9u, 0x10u,                // $8037 LDA #$10
      0x8Fu, 0x15u, 0x43u, 0x00u,  // $8039 STA $00:4315    DAS1 low: 16
      0xA9u, 0x00u,                // $803D LDA #$00
      0x8Fu, 0x16u, 0x43u, 0x00u,  // $803F STA $00:4316    DAS1 high
      0xCBu,                       // $8043 WAI
      0xA9u, 0x02u,                // $8044 LDA #$02
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8046 STA $00:420B    MDMAEN: channel 1
      0x80u, 0xFEu,                // $804A BRA *
  });
  put(rom, 0x0E00u, {
      0x48u,                       // $8E00 PHA             the vertical-blank handler
      0xAFu, 0x10u, 0x42u, 0x00u,  // $8E01 LDA $00:4210    RDNMI
      0xA9u, 0x08u,                // $8E05 LDA #$08
      0x8Fu, 0x15u, 0x43u, 0x00u,  // $8E07 STA $00:4315    DAS1 low: 8
      0x68u,                       // $8E0B PLA
      0x40u,                       // $8E0C RTI
  });
  for (std::size_t i = 0; i < 16; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x30u + i);
  return rom;
}

TEST(RomEnumerate, ATransferStartedWithAnotherRangeIsNotConfirmed) {
  const CartridgeDisassembly d = disassembled(shortenedTransferImage());
  const auto moved = std::find_if(d.moved.begin(), d.moved.end(), [](const MovedRange& m) { return m.channel == 1; });
  ASSERT_NE(moved, d.moved.end()) << renderManifest(d);
  EXPECT_EQ(moved->bytes, 8u) << "the run started the site with the handler's count";
  const auto proven = std::find_if(d.dmas.begin(), d.dmas.end(), [](const DmaTransfer& t) { return t.channel == 1; });
  ASSERT_NE(proven, d.dmas.end());
  ASSERT_TRUE(proven->bytes.has_value());
  EXPECT_EQ(*proven->bytes, 16u) << "the code proves sixteen";
  EXPECT_TRUE(hasNote(d.notes, "dma " + text::address24(proven->site) +
                                   " channel 1: the code proves $7E:2000 increment bytes 16; the run wrote the extent "
                                   "and never started this transfer; its sources are the staged lines at $7E:2000"))
      << renderManifest(d);
}

// ---- the second run -------------------------------------------------------------

// The first run, the plan over the disassembly, and the second run carrying it
// out, with the notes each wrote.
struct Enumerated {
  RunObservation first;
  RunObservation second;
  std::vector<std::string> notes;
};

Enumerated enumerateRun(const std::vector<std::uint8_t>& rom, const CartridgeDisassembly& disassembly,
                        std::uint64_t instructionsOff = 0) {
  Enumerated out;
  std::vector<std::string> firstNotes;
  out.first = observeRun(rom, kSecond, InputScript{}, firstNotes);
  std::vector<std::string> planNotes;
  const EnumerationPlan plan = enumerationPlan(disassembly, proveProgram(disassembly, rom), planNotes);
  out.second = observeRun(rom, kSecond, InputScript{}, out.notes, {}, plan, out.first.instructions + instructionsOff,
                          out.first.divergences);
  return out;
}

const Enumerated& enumeratingRuns() {
  static const Enumerated runs = enumerateRun(enumeratingRom(), enumerating());
  return runs;
}

// The outcome of a routine that holds a value, or nothing.
const EnumeratedRun* runOf(const RunObservation& run, Address routine, std::uint32_t value) {
  const auto found = std::find_if(run.enumerated.begin(), run.enumerated.end(), [&](const EnumeratedRun& r) {
    return r.routine == routine && std::find(r.values.begin(), r.values.end(), value) != r.values.end();
  });
  return found == run.enumerated.end() ? nullptr : &*found;
}

const EnumeratedExtent* extentAt(const EnumeratedRun& run, Address memory) {
  const auto found = std::find_if(run.extents.begin(), run.extents.end(),
                                  [&](const EnumeratedExtent& e) { return e.memory == memory; });
  return found == run.extents.end() ? nullptr : &*found;
}

// The snapshot is the first invocation's: every value is entered from the
// first call, whose index was entry 2, not the second's.
TEST(RomEnumerate, TheSnapshotIsTheFirstInvocations) {
  const Enumerated& runs = enumeratingRuns();
  for (const std::uint32_t value : {0u, 2u, 4u, 8u, 10u}) {
    const EnumeratedRun* run = runOf(runs.second, 0x008100u, value);
    ASSERT_NE(run, nullptr) << value;
    EXPECT_EQ(run->enteredFrom, 0x00804Au) << "the first call, not the second at $00:8065";
  }
}

// The second run executes exactly what the first did, and enumerates; told
// another count, it says so and runs nothing.
TEST(RomEnumerate, TheSecondRunMatchesTheFirst) {
  const Enumerated& runs = enumeratingRuns();
  EXPECT_EQ(runs.second.instructions, runs.first.instructions);
  EXPECT_EQ(runs.second.divergences, runs.first.divergences);
  EXPECT_EQ(runs.second.variantsRun, 21u);
  const Enumerated off = enumerateRun(enumeratingRom(), enumerating(), 1);
  EXPECT_TRUE(off.second.enumerated.empty());
  EXPECT_EQ(off.second.variantsPlanned, 0u);
  EXPECT_EQ(off.second.variantsRun, 0u);
  const std::string expected = "run: the second run executed " + std::to_string(off.first.instructions) +
                               " instructions and 0 divergences where the first executed " +
                               std::to_string(off.first.instructions + 1u) + " and 0; nothing enumerated";
  EXPECT_EQ(off.notes, (std::vector<std::string>{expected}));
}

// The footprint is everything either recorded invocation wrote: the raw
// copy's pointer and length in the direct page and its thirty-two bytes, and
// the run-length copy's counter at `$06`, which only the second invocation
// wrote — so the run-length entry the run never selected stays inside it. Its
// one extent is the thirty-two bytes the recorded extent holds; the direct
// page is scratch, noted once for the routine with the counter among it.
TEST(RomEnumerate, TheFootprintIsTheUnionOverEveryInvocation) {
  const Enumerated& runs = enumeratingRuns();
  const EnumeratedRun* run = runOf(runs.second, 0x008100u, 2u);
  ASSERT_NE(run, nullptr) << "entry 1, run-length, is confined";
  ASSERT_EQ(run->extents.size(), 1u);
  EXPECT_EQ(run->extents[0].memory, 0x7F0000u);
  EXPECT_EQ(run->extents[0].bytes, 32u);
  EXPECT_TRUE(hasNote(runs.notes, "enumerate sub_008100: 5 values wrote $7E:0000-$7E:0001, $7E:0004-$7E:0006, "
                                  "extents the run never carried; not lifted"));
}

// A loader that waits twice for the vertical blank for entry 0, writes the
// frame count's byte for entry 1, never returns for entry 2, and writes
// nothing for entry 3; the run selects entry 0, and the handler counts frames
// in `$7E:0020`.
std::vector<std::uint8_t> waitingLoaderImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  rom[0x7FC0u + 0x2Au] = 0x00u;  // native NMI -> $8E00
  rom[0x7FC0u + 0x2Bu] = 0x8Eu;
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE       -> native
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0x8Fu, 0x10u, 0x00u, 0x7Eu,  // $800C STA $7E:0010    the index, 0
      0xE2u, 0x20u,                // $8010 SEP #$20
      0xA9u, 0x80u,                // $8012 LDA #$80
      0x8Fu, 0x00u, 0x42u, 0x00u,  // $8014 STA $00:4200    NMITIMEN
      0xC2u, 0x30u,                // $8018 REP #$30
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $801A LDA $7E:0010
      0x29u, 0x03u, 0x00u,         // $801E AND #$0003
      0x0Au,                       // $8021 ASL A
      0xAAu,                       // $8022 TAX
      0xA9u, 0x00u, 0x00u,         // $8023 LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $8026 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8029 JSR !$8100
      0x80u, 0xFEu,                // $802C BRA *
  });
  put(rom, 0x0100u, {
      0xE2u, 0x20u,                // $8100 SEP #$20        sub_008100
      0xE0u, 0x00u, 0x00u,         // $8102 CPX #$0000
      0xF0u, 0x0Bu,                // $8105 BEQ $8112
      0xE0u, 0x02u, 0x00u,         // $8107 CPX #$0002
      0xF0u, 0x3Bu,                // $810A BEQ $8147
      0xE0u, 0x04u, 0x00u,         // $810C CPX #$0004
      0xF0u, 0x3Du,                // $810F BEQ $814E
      0x60u,                       // $8111 RTS             entry 3: nothing written
      0xA9u, 0x11u,                // $8112 LDA #$11        entry 0
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8114 STA $7F:0000
      0xCBu,                       // $8118 WAI
      0xCBu,                       // $8119 WAI
      0xA9u, 0x00u,                // $811A LDA #$00
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $811C STA $00:4300    DMAP0
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8120 STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $8124 STA $00:4303    A1T0 high
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $8128 STA $00:4306    DAS0 high
      0x8Fu, 0x21u, 0x21u, 0x00u,  // $812C STA $00:2121    CGADD
      0xA9u, 0x22u,                // $8130 LDA #$22
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $8132 STA $00:4301    BBAD0 = CGDATA
      0xA9u, 0x7Fu,                // $8136 LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $8138 STA $00:4304    A1B0
      0xA9u, 0x01u,                // $813C LDA #$01
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $813E STA $00:4305    DAS0 low: 1
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8142 STA $00:420B    MDMAEN
      0x60u,                       // $8146 RTS
      0xA9u, 0x22u,                // $8147 LDA #$22        entry 1
      0x8Fu, 0x20u, 0x00u, 0x7Eu,  // $8149 STA $7E:0020    the handler's byte
      0x60u,                       // $814D RTS
      0x80u, 0xFEu,                // $814E BRA *           entry 2
  });
  put(rom, 0x0E00u, {
      0x08u,                       // $8E00 PHP
      0xE2u, 0x20u,                // $8E01 SEP #$20
      0x48u,                       // $8E03 PHA
      0xAFu, 0x10u, 0x42u, 0x00u,  // $8E04 LDA $00:4210    RDNMI
      0xAFu, 0x20u, 0x00u, 0x7Eu,  // $8E08 LDA $7E:0020
      0x1Au,                       // $8E0C INC A
      0x8Fu, 0x20u, 0x00u, 0x7Eu,  // $8E0D STA $7E:0020
      0x68u,                       // $8E11 PLA
      0x28u,                       // $8E12 PLP
      0x40u,                       // $8E13 RTI
  });
  return rom;
}

const Enumerated& waitingRuns() {
  static const std::vector<std::uint8_t> rom = waitingLoaderImage();
  static const CartridgeDisassembly disassembly = disassembled(rom);
  static const Enumerated runs = enumerateRun(rom, disassembly);
  return runs;
}

// What the handler writes while the loader waits is the handler's: it is no
// part of the loader's footprint, so the entry that writes the same byte
// itself is refused.
TEST(RomEnumerate, AnInterruptsWritesAreNotTheRoutines) {
  EXPECT_TRUE(hasNote(waitingRuns().notes,
                      "enumerate sub_008100 X=$0002: wrote $7E:0020, outside what the run saw the routine write; "
                      "not lifted"));
}

// The budget is 64 times the longest invocation when that is more than a
// second: the loader's waits make its invocation two frames long.
TEST(RomEnumerate, TheLongestInvocationSetsTheBudget) {
  const std::vector<std::string>& notes = waitingRuns().notes;
  const std::string head = "enumerate sub_008100 X=$0004: did not return within ";
  const auto found = std::find_if(notes.begin(), notes.end(),
                                  [&](const std::string& note) { return note.rfind(head, 0) == 0; });
  ASSERT_NE(found, notes.end());
  const std::uint64_t budget = std::stoull(found->substr(head.size()));
  EXPECT_GT(budget, kSecond) << "the invocation, not the floor";
  EXPECT_EQ(budget % 64u, 0u);
  EXPECT_EQ(found->substr(found->size() - std::string(" cycles; not lifted").size()), " cycles; not lifted");
}

// ---- the values -----------------------------------------------------------------

// Every value of every input runs: eight for the first loader's X, four each
// for the third and fourth loaders' A, five for the fifth's; the first
// loader's five confined values are kept and its three others refused, and
// every other value is kept.
TEST(RomEnumerate, EveryAdmittedValueRuns) {
  const RunObservation& run = enumeratingRuns().second;
  EXPECT_EQ(run.variantsPlanned, 21u);
  EXPECT_EQ(run.variantsRun, 21u);
  EXPECT_EQ(run.variantsReturned, 18u);
  EXPECT_EQ(run.variantsConfined, 18u);
  for (std::uint32_t value = 0; value < 5; ++value) {
    const EnumeratedRun* r = runOf(run, 0x008D00u, value);
    ASSERT_NE(r, nullptr) << value;
    EXPECT_EQ(r->reg, InputRegister::A);
    EXPECT_EQ(r->bits, 16u);
  }
  EXPECT_GT(run.enumerationCycles, kSecond) << "the endless entry ran out its second";
  std::vector<std::uint32_t> kept;
  for (const EnumeratedRun& r : run.enumerated) {
    if (r.routine == 0x008100u) kept.insert(kept.end(), r.values.begin(), r.values.end());
  }
  EXPECT_EQ(kept, (std::vector<std::uint32_t>{0, 2, 4, 8, 10}));
  for (const Address routine : {0x008800u, 0x008B00u}) {
    for (std::uint32_t value = 0; value < 4; ++value) {
      const EnumeratedRun* r = runOf(run, routine, value);
      ASSERT_NE(r, nullptr) << routine << " " << value;
      EXPECT_EQ(r->reg, InputRegister::A);
      EXPECT_EQ(r->bits, routine == 0x008800u ? 16u : 8u);
    }
  }
}

// The value the run itself selected runs like every other, and builds what
// the run built.
TEST(RomEnumerate, TheRecordedValueHasItsOwnLine) {
  const EnumeratedRun* run = runOf(enumeratingRuns().second, 0x008100u, 4u);
  ASSERT_NE(run, nullptr);
  EXPECT_EQ(run->values, (std::vector<std::uint32_t>{4})) << "no other entry builds the same bytes";
  const EnumeratedExtent* built = extentAt(*run, 0x7F0000u);
  ASSERT_NE(built, nullptr);
  ASSERT_EQ(built->values.size(), 32u);
  for (std::size_t i = 0; i < 32; ++i) EXPECT_EQ(built->values[i], 0x20u + i);
  EXPECT_EQ(built->sources, (std::vector<ir::OriginInterval>{{0x0380u, 0x03A1u}}));
}

// A value's sources are the runs its own invocation read: the whole
// run-length stream of entry 1, counts and header included.
TEST(RomEnumerate, AVariantsSourcesAreItsInvocationsRuns) {
  const EnumeratedRun* run = runOf(enumeratingRuns().second, 0x008100u, 2u);
  ASSERT_NE(run, nullptr);
  const EnumeratedExtent* built = extentAt(*run, 0x7F0000u);
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(built->sources, (std::vector<ir::OriginInterval>{{0x0340u, 0x0351u}}));
  ASSERT_EQ(built->values.size(), 32u);
  for (std::size_t i = 0; i < 32; ++i) EXPECT_EQ(built->values[i], 0xA1u + i / 4u);
  EXPECT_FALSE(built->origin.approximate);
  EXPECT_EQ(built->origin.image.size(), 8u) << "the eight value bytes, every other one";
}

TEST(RomEnumerate, AVariantThatDoesNotReturnIsRefused) {
  EXPECT_TRUE(hasNote(enumeratingRuns().notes,
                      "enumerate sub_008100 X=$000C: did not return within 21477272 cycles; not lifted"));
}

TEST(RomEnumerate, AVariantThatWritesPastTheFootprintIsRefused) {
  EXPECT_TRUE(hasNote(enumeratingRuns().notes,
                      "enumerate sub_008100 X=$000E: wrote $7F:0020, outside what the run saw the routine write; "
                      "not lifted"));
}

TEST(RomEnumerate, AVariantThatWritesANewRegisterIsRefused) {
  EXPECT_TRUE(hasNote(enumeratingRuns().notes,
                      "enumerate sub_008100 X=$0006: wrote INIDISP, a register the run never saw the routine write; "
                      "not lifted"));
}

TEST(RomEnumerate, AVariantWritingNothingIsNoted) {
  EXPECT_TRUE(hasNote(waitingRuns().notes, "enumerate sub_008100 X=$0006: wrote no byte of work RAM; nothing to lift"));
  EXPECT_EQ(runOf(waitingRuns().second, 0x008100u, 6u), nullptr) << "a value that built nothing is kept as no run";
}

// A value whose lifted instruction disagrees with the machine is refused at
// the first step: the case seeds the interpreter with a node for the loader's
// first instruction whose read lands one byte off.
TEST(RomEnumerate, ADivergingVariantIsRefused) {
  const std::vector<std::uint8_t>& rom = enumeratingRom();
  const CartridgeBoard board = cartridgeBoard(rom);
  Snes machine{SnesConfig{.rom = rom}};
  while ((static_cast<Address>(machine.state().cpu.pbr) << 16 | machine.state().cpu.pc) != 0x008100u) {
    (void)machine.step();
  }
  const PlannedRoutine routine{.address = 0x008100u, .label = "sub_008100", .inputs = {}};
  detail::Planned planned;
  planned.routine = &routine;
  const detail::Snapshot snapshot{.state = machine.state(),
                                  .shadow = ir::Provenance{board, rom.size(), 64},
                                  .returnTo = 0x00804Du,
                                  .enteredFrom = 0x00804Au,
                                  .entryS = machine.state().cpu.s,
                                  .frame = 0,
                                  .spent = 0};
  const detail::VariantSetting setting{.reg = InputRegister::X, .bits = 16, .value = 4};
  SnesState state = snapshot.state;
  detail::setInput(state, setting);
  Snes sandbox{SnesConfig{.rom = rom}};
  ASSERT_TRUE(sandbox.restore(state));
  ir::Provenance copy = snapshot.shadow;
  detail::Recorder rec{sandbox, copy};
  copy.carries = &rec;
  sandbox.setObserver(&rec);
  detail::Lockstep step{board, rom.size(), state.cpu, copy};
  step.expectedReturns.insert(snapshot.returnTo);
  step.expectedNext = 0x008100u;
  // The node for `LDA $00:8200,X` under A16, X16, its read one byte off.
  const std::vector<std::uint8_t> bytes = {0xBFu, 0x00u, 0x82u, 0x00u};
  const Cpu65816Mode mode = detail::modeOf(state.cpu);
  const std::optional<Decoded> decoded = cpu65816Backend().decode(bytes, 0x008100u, 0x008100u, contextOf(mode));
  ASSERT_TRUE(decoded.has_value());
  ir::Node node = ir::liftInstruction(decoded->instruction, mode);
  const auto address = std::find_if(node.effects.begin(), node.effects.end(),
                                    [](const ir::Effect& e) { return e.op == ir::Op::LongAddress; });
  ASSERT_NE(address, node.effects.end());
  address->a.value += 1u;
  step.nodes.emplace(detail::Lockstep::NodeKey{0x008100u, contextOf(mode).bits, bytes}, node);
  std::vector<std::string> notes;
  std::uint64_t cycles = 0;
  std::vector<bool> scratch(0x20000u, false);
  bool wroteScratch = false;
  const std::optional<EnumeratedRun> run =
      detail::runVariant(snapshot, planned, {}, scratch, wroteScratch, setting, sandbox, step, rec, copy, rom, board,
                         InputScript{}, routine.label, notes, cycles);
  sandbox.setObserver(nullptr);
  EXPECT_FALSE(run.has_value());
  EXPECT_EQ(notes, (std::vector<std::string>{
                       "enumerate sub_008100 X=$0004: the lift disagreed with the machine at $00:8100; not lifted"}));
  EXPECT_GT(cycles, 0u);
}

// The values run once the run is over, against everything the run saw: entry
// 1 writes the counter only the second invocation wrote, and is kept.
TEST(RomEnumerate, TheVariantsRunAtTheRunsEnd) {
  const Enumerated& runs = enumeratingRuns();
  ASSERT_NE(runOf(runs.second, 0x008100u, 2u), nullptr);
  for (const std::string& note : runs.notes) {
    EXPECT_NE(note.rfind("enumerate sub_008100 X=$0002:", 0), 0u) << note;
  }
}

// ---- what the manifest says -----------------------------------------------------

std::vector<std::string> enumeratedLinesBy(const std::string& manifest, const std::string& label) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while ((at = manifest.find("enumerated ", at)) != std::string::npos) {
    const std::size_t end = manifest.find('\n', at);
    const std::string line = manifest.substr(at, end - at);
    if (line.find(" by " + label + " ") != std::string::npos) out.push_back(line);
    at = end;
  }
  return out;
}

// The line of an outcome holding `value` by `label`, or nothing.
std::optional<std::string> lineOf(const std::vector<std::string>& lines, const std::string& value) {
  for (const std::string& line : lines) {
    if (line.find(" " + value + " entered from ") != std::string::npos) return line;
  }
  return std::nullopt;
}

// A value whose routine carries what it builds itself takes the class of
// that carry, the recorded extent's: the fourth loader's sprite tables.
TEST(RomEnumerate, AVariantsOwnCarryNamesItsClass) {
  const std::vector<std::string> lines = enumeratedLinesBy(renderManifest(enumerating()), "sub_008B00");
  const std::optional<std::string> line = lineOf(lines, "A=$00");
  ASSERT_TRUE(line.has_value()) << lines.size();
  EXPECT_NE(line->find(" to Oam by "), std::string::npos) << *line;
}

// A value whose routine carries nothing takes the class the run's carry of
// the recorded extent gave it: reset sends the third loader's buffer to the
// palette.
TEST(RomEnumerate, ARecordedCarryNamesTheClassOfAVariantThatCarriesNothing) {
  const std::vector<std::string> lines = enumeratedLinesBy(renderManifest(enumerating()), "sub_008800");
  const std::optional<std::string> line = lineOf(lines, "A=$0000");
  ASSERT_TRUE(line.has_value()) << lines.size();
  EXPECT_NE(line->find(" to Cgram by "), std::string::npos) << *line;
}

// The bytes a routine writes that no recorded extent holds and no transfer
// of its own carried are scratch: noted once for the routine, with how many
// of its kept values wrote them, and lifted by nothing.
TEST(RomEnumerate, AnExtentTheRunNeverCarriedIsNoted) {
  EXPECT_TRUE(hasNote(enumerating().notes, "enumerate sub_008100: 5 values wrote $7E:0000-$7E:0001, $7E:0004-$7E:0006, "
                                           "extents the run never carried; not lifted"));
  EXPECT_TRUE(hasNote(enumerating().notes, "enumerate sub_008D00: 5 values wrote $7E:0000-$7E:0001, $7E:0004-$7E:0007, "
                                           "$7F:0420-$7F:042F, extents the run never carried; not lifted"));
}

// One note a routine for its scratch, after its values' notes: the first,
// third, fourth and fifth loaders' each once, and no note names one value's.
TEST(RomEnumerate, TheScratchIsNotedOnceARoutine) {
  const std::vector<std::string>& notes = enumerating().notes;
  for (const char* note : {
           "enumerate sub_008100: 5 values wrote $7E:0000-$7E:0001, $7E:0004-$7E:0006, extents the run never carried; "
           "not lifted",
           "enumerate sub_008800: 4 values wrote $7E:0000-$7E:0001, extents the run never carried; not lifted",
           "enumerate sub_008B00: 4 values wrote $7E:0000-$7E:0001, extents the run never carried; not lifted",
           "enumerate sub_008D00: 5 values wrote $7E:0000-$7E:0001, $7E:0004-$7E:0007, $7F:0420-$7F:042F, extents "
           "the run never carried; not lifted",
       }) {
    EXPECT_EQ(std::count(notes.begin(), notes.end(), std::string(note)), 1) << note;
  }
  for (const std::string& note : notes) {
    const bool perValue = note.find("=$") != std::string::npos && note.find("never carried") != std::string::npos;
    EXPECT_FALSE(perValue) << note;
  }
  const auto refused = std::find(notes.begin(), notes.end(),
                                 "enumerate sub_008100 X=$000E: wrote $7F:0020, outside what the run saw the routine "
                                 "write; not lifted");
  const auto scratch = std::find(notes.begin(), notes.end(),
                                 "enumerate sub_008100: 5 values wrote $7E:0000-$7E:0001, $7E:0004-$7E:0006, extents "
                                 "the run never carried; not lifted");
  ASSERT_NE(refused, notes.end());
  ASSERT_NE(scratch, notes.end());
  EXPECT_LT(refused, scratch) << "after the routine's value notes";
}

// Entry 2 of the fifth loader builds forty-eight bytes and sends sixteen; the
// recorded extent holds the first thirty-two, so its one extent is those, and
// the sixteen past them are the routine's scratch.
TEST(RomEnumerate, ABytePastTheRecordedExtentIsScratch) {
  const EnumeratedRun* run = runOf(enumeratingRuns().second, 0x008D00u, 2u);
  ASSERT_NE(run, nullptr);
  ASSERT_EQ(run->extents.size(), 1u);
  EXPECT_EQ(run->extents[0].memory, 0x7F0400u);
  EXPECT_EQ(run->extents[0].bytes, 32u);
  EXPECT_EQ(run->extents[0].classes, std::vector<RegisterClass>{RegisterClass::Vram});
  const std::vector<std::string>& notes = enumeratingRuns().notes;
  const auto found = std::find_if(notes.begin(), notes.end(), [](const std::string& note) {
    return note.rfind("enumerate sub_008D00: ", 0) == 0 && note.find("never carried") != std::string::npos;
  });
  ASSERT_NE(found, notes.end());
  EXPECT_NE(found->find("$7F:0420-$7F:042F"), std::string::npos) << *found;
}

// Entry 1 of the fifth loader sends all forty-eight bytes it builds on its own
// channel: one extent of forty-eight, its class VRAM, carried by that transfer.
TEST(RomEnumerate, AValuesOwnTransferNamesItsBytesClass) {
  const EnumeratedRun* run = runOf(enumeratingRuns().second, 0x008D00u, 1u);
  ASSERT_NE(run, nullptr);
  ASSERT_EQ(run->extents.size(), 1u);
  const EnumeratedExtent& extent = run->extents[0];
  EXPECT_EQ(extent.memory, 0x7F0400u);
  EXPECT_EQ(extent.bytes, 48u);
  EXPECT_EQ(extent.classes, std::vector<RegisterClass>{RegisterClass::Vram});
  ASSERT_EQ(extent.carried.size(), 1u);
  EXPECT_EQ(extent.carried[0].cls, RegisterClass::Vram);
  EXPECT_EQ(extent.carried[0].kind, MovedKind::Dma);
  EXPECT_EQ(extent.carried[0].registerAddress, 0x002118u) << "VMDATAL";
  EXPECT_EQ(extent.carried[0].site, 0x008D54u) << "the start on channel 4";
}

// Values whose extents differ in their bytes are outcomes of their own: the
// third loader's four entries each build a different blob.
TEST(RomEnumerate, AnOutcomeWithDifferentBytesKeepsItsOwnLine) {
  const RunObservation& run = enumeratingRuns().second;
  std::vector<std::vector<std::uint32_t>> outcomes;
  for (const EnumeratedRun& r : run.enumerated) {
    if (r.routine == 0x008800u) outcomes.push_back(r.values);
  }
  EXPECT_EQ(outcomes, (std::vector<std::vector<std::uint32_t>>{{0}, {1}, {2}, {3}}));
}

// A routine the vertical-blank handler calls writes inside the handler, and
// its writes are its own: the fifth loader keeps every value the handler
// bounds, every byte it builds is inside what the run saw it write, and the
// handler's frame count is none of its.
TEST(RomEnumerate, ARoutineAHandlerCallsKeepsItsWrites) {
  const Enumerated& runs = enumeratingRuns();
  for (std::uint32_t value = 0; value < 5; ++value) {
    EXPECT_NE(runOf(runs.second, 0x008D00u, value), nullptr) << value;
  }
  const EnumeratedRun* whole = runOf(runs.second, 0x008D00u, 1u);
  ASSERT_NE(whole, nullptr);
  ASSERT_EQ(whole->extents.size(), 1u);
  EXPECT_EQ(whole->extents[0].memory, 0x7F0400u);
  EXPECT_EQ(whole->extents[0].bytes, 48u) << "$7F:0400-$7F:042F, all of it inside the footprint";
  for (const std::string& note : runs.notes) {
    if (note.rfind("enumerate sub_008D00", 0) != 0) continue;
    EXPECT_EQ(note.find("outside what the run saw"), std::string::npos) << note;
    EXPECT_EQ(note.find("never saw"), std::string::npos) << note;
    EXPECT_EQ(note.find("$7E:0020"), std::string::npos) << note;
  }
}

// A loader that reads its index, then calls the instruction its own caller
// returns to — the caller's `RTS` — between its two stores: the run of a value
// ends at the return to its caller, with the stack back above the entry's,
// not at the first arrival at that address.
std::vector<std::uint8_t> reenteringImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0x8Fu, 0x10u, 0x00u, 0x7Eu,  // $800C STA $7E:0010
      0x20u, 0x00u, 0x81u,         // $8010 JSR !$8100
      0x80u, 0xFEu,                // $8013 BRA *
  });
  put(rom, 0x0100u, {
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $8100 LDA $7E:0010
      0x29u, 0x01u, 0x00u,         // $8104 AND #$0001
      0x0Au,                       // $8107 ASL A
      0xAAu,                       // $8108 TAX
      0xA9u, 0x00u, 0x00u,         // $8109 LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $810C LDY #$0000
      0x20u, 0x00u, 0x82u,         // $810F JSR !$8200      the loader
      0x60u,                       // $8112 RTS             where the loader returns, and what it calls
  });
  put(rom, 0x0200u, {
      0xE2u, 0x20u,                // $8200 SEP #$20
      0xBFu, 0x00u, 0x90u, 0x00u,  // $8202 LDA $00:9000,X  $11 for either entry
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8206 STA $7F:0000
      0x20u, 0x12u, 0x81u,         // $820A JSR !$8112      its caller's return address
      0xA9u, 0x22u,                // $820D LDA #$22
      0x8Fu, 0x01u, 0x00u, 0x7Fu,  // $820F STA $7F:0001
      0xA9u, 0x00u,                // $8213 LDA #$00
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $8215 STA $00:4300    DMAP0
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8219 STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $821D STA $00:4303    A1T0 high
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $8221 STA $00:4306    DAS0 high
      0x8Fu, 0x21u, 0x21u, 0x00u,  // $8225 STA $00:2121    CGADD
      0xA9u, 0x22u,                // $8229 LDA #$22
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $822B STA $00:4301    BBAD0 = CGDATA
      0xA9u, 0x7Fu,                // $822F LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $8231 STA $00:4304    A1B0
      0xA9u, 0x02u,                // $8235 LDA #$02
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $8237 STA $00:4305    DAS0 low: 2
      0xA9u, 0x01u,                // $823B LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $823D STA $00:420B    MDMAEN
      0xC2u, 0x20u,                // $8241 REP #$20
      0x60u,                       // $8243 RTS
  });
  rom[0x1000u] = 0x11u;  // $9000: entry 0's byte
  rom[0x1002u] = 0x11u;  // $9002: entry 1's
  return rom;
}

TEST(RomEnumerate, AReturnIsTheStackPointerAsWellAsTheAddress) {
  const std::vector<std::uint8_t> rom = reenteringImage();
  const Enumerated runs = enumerateRun(rom, disassembled(rom));
  for (const std::uint32_t value : {0u, 2u}) {
    const EnumeratedRun* run = runOf(runs.second, 0x008200u, value);
    ASSERT_NE(run, nullptr) << value;
    const EnumeratedExtent* built = extentAt(*run, 0x7F0000u);
    ASSERT_NE(built, nullptr) << value;
    EXPECT_EQ(built->values, (std::vector<std::uint8_t>{0x11u, 0x22u})) << "both stores, the second after the call";
  }
}

// ---- outcomes, lines and the files ------------------------------------------

// A loader indexed in A whose table's two entries point at one blob of four
// bytes, copied to `$7F:0000` and sent to the palette: both values build the
// same bytes from the same source.
std::vector<std::uint8_t> sharedBlobImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $800C LDA $7E:0010
      0x29u, 0x01u, 0x00u,         // $8010 AND #$0001      A in {0, 1}
      0xA2u, 0x00u, 0x00u,         // $8013 LDX #$0000
      0xA0u, 0x00u, 0x00u,         // $8016 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8019 JSR !$8100
      0x80u, 0xFEu,                // $801C BRA *
  });
  put(rom, 0x0100u, {
      0x0Au,                       // $8100 ASL A           sub_008100
      0xAAu,                       // $8101 TAX
      0xBFu, 0x00u, 0x82u, 0x00u,  // $8102 LDA $00:8200,X  the blob's address
      0x85u, 0x00u,                // $8106 STA $00         the pointer; its bank at $02 powers on zero
      0xE2u, 0x20u,                // $8108 SEP #$20
      0xA0u, 0x00u, 0x00u,         // $810A LDY #$0000
      0xA2u, 0x00u, 0x00u,         // $810D LDX #$0000
      0xB7u, 0x00u,                // $8110 LDA [$00],Y
      0x9Fu, 0x00u, 0x00u, 0x7Fu,  // $8112 STA $7F:0000,X
      0xC8u,                       // $8116 INY
      0xE8u,                       // $8117 INX
      0xE0u, 0x04u, 0x00u,         // $8118 CPX #$0004
      0xD0u, 0xF3u,                // $811B BNE $8110
  });
  putPaletteSend(rom, 0x011Du, 4);  // $811D
  put(rom, 0x0149u, {0x60u});        // $8149 RTS
  put(rom, 0x0200u, {0x00u, 0x83u, 0x00u, 0x83u});           // $8200: both entries at $8300
  put(rom, 0x0300u, {0x11u, 0x22u, 0x33u, 0x44u});           // $8300: the blob
  return rom;
}

TEST(RomEnumerate, ValuesWithOneOutcomeShareALine) {
  const std::vector<std::uint8_t> rom = sharedBlobImage();
  const CartridgeDisassembly d = disassembled(rom);
  const auto outcome = std::find_if(d.enumerated.begin(), d.enumerated.end(),
                                    [](const EnumeratedRun& r) { return r.routine == 0x008100u; });
  ASSERT_NE(outcome, d.enumerated.end()) << renderManifest(d);
  EXPECT_EQ(outcome->values, (std::vector<std::uint32_t>{0, 1}));
  EXPECT_EQ(std::count_if(d.enumerated.begin(), d.enumerated.end(),
                          [](const EnumeratedRun& r) { return r.routine == 0x008100u; }),
            1);
  const std::vector<std::string> lines = enumeratedLinesBy(renderManifest(d), "sub_008100");
  ASSERT_EQ(lines.size(), 1u) << renderManifest(d);
  EXPECT_NE(lines[0].find(" A=$0000-$0001 entered from "), std::string::npos) << lines[0];
}

TEST(RomEnumerate, TheValuesOfALineAreWrittenAsRuns) {
  EXPECT_EQ(inputValuesText(16, std::vector<std::uint32_t>{0, 2, 4, 6, 8, 10, 12, 14}), "$0000-$000E/2");
  EXPECT_EQ(inputValuesText(8, std::vector<std::uint32_t>{0, 1, 2, 3}), "$00-$03");
  EXPECT_EQ(inputValuesText(8, std::vector<std::uint32_t>{0, 3}), "$00,$03");
  EXPECT_EQ(inputValuesText(8, std::vector<std::uint32_t>{1, 2, 3, 7, 9, 11, 20}), "$01-$03,$07-$0B/2,$14");
  EXPECT_EQ(inputValuesText(16, std::vector<std::uint32_t>{0, 1}), "$0000-$0001");
  EXPECT_EQ(inputValuesText(16, std::vector<std::uint32_t>{4}), "$0004");
}

// A loader indexed in X that writes `$7F:0000` and `$7F:0100` and sends one
// of them on channel 0 to the register its entry names: entry 0, the run's,
// sends `$7F:0000` to the palette; entry 1 sends `$7F:0100`, which no recorded
// extent holds, to `WMDATA`, which no file is named for.
std::vector<std::uint8_t> portSendingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $800C LDA $7E:0010
      0x29u, 0x01u, 0x00u,         // $8010 AND #$0001
      0x0Au,                       // $8013 ASL A
      0xAAu,                       // $8014 TAX             X in {0, 2}
      0xA9u, 0x00u, 0x00u,         // $8015 LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $8018 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $801B JSR !$8100
      0x80u, 0xFEu,                // $801E BRA *
  });
  put(rom, 0x0100u, {
      0xE2u, 0x20u,                // $8100 SEP #$20        sub_008100
      0xBFu, 0x00u, 0x90u, 0x00u,  // $8102 LDA $00:9000,X  the entry's byte
      0x8Fu, 0x00u, 0x00u, 0x7Fu,  // $8106 STA $7F:0000
      0x8Fu, 0x00u, 0x01u, 0x7Fu,  // $810A STA $7F:0100
      0xBFu, 0x00u, 0x91u, 0x00u,  // $810E LDA $00:9100,X  the entry's register
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $8112 STA $00:4301    BBAD0
      0xBFu, 0x00u, 0x92u, 0x00u,  // $8116 LDA $00:9200,X  the entry's buffer, high byte
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $811A STA $00:4303    A1T0 high
      0xA9u, 0x00u,                // $811E LDA #$00
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $8120 STA $00:4300    DMAP0
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8124 STA $00:4302    A1T0 low
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $8128 STA $00:4306    DAS0 high
      0xA9u, 0x7Fu,                // $812C LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $812E STA $00:4304    A1B0
      0xA9u, 0x01u,                // $8132 LDA #$01
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $8134 STA $00:4305    DAS0 low: 1
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $8138 STA $00:420B    MDMAEN
      0xC2u, 0x20u,                // $813C REP #$20
      0x60u,                       // $813E RTS
  });
  put(rom, 0x1000u, {0x5Au, 0x00u, 0xA5u});  // $9000: each entry's byte
  put(rom, 0x1100u, {0x22u, 0x00u, 0x80u});  // $9100: CGDATA for entry 0, WMDATA for entry 1
  put(rom, 0x1200u, {0x00u, 0x00u, 0x01u});  // $9200: $7F:0000 for entry 0, $7F:0100 for entry 1
  return rom;
}

TEST(RomEnumerate, ABufferSentToARegisterNoFileIsNamedForIsNoted) {
  const CartridgeDisassembly d = disassembled(portSendingImage());
  EXPECT_TRUE(hasNote(d.notes, "enumerate sub_008100 X=$0002: wrote $7F:0100 bytes 1, carried to WMDATA, which no file "
                               "is named for; not lifted"))
      << renderManifest(d);
}

// A loader indexed in X over {0, 2, 4, 6} that copies the sixteen bytes at
// `$9000` into `$7F:0000` and sends them on channel 0 to the register its
// entry names: `CGDATA` for entry 0, the run's; `OAMDATA` for entry 1;
// `VMDATAL` for entries 2 and 3.
std::vector<std::uint8_t> crossClassImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $800C LDA $7E:0010
      0x29u, 0x06u, 0x00u,         // $8010 AND #$0006
      0xAAu,                       // $8013 TAX             X in {0, 2, 4, 6}
      0xA9u, 0x00u, 0x00u,         // $8014 LDA #$0000
      0xA0u, 0x00u, 0x00u,         // $8017 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $801A JSR !$8100
      0x80u, 0xFEu,                // $801D BRA *
  });
  put(rom, 0x0100u, {
      0xE2u, 0x20u,                // $8100 SEP #$20        sub_008100
      0xBFu, 0x00u, 0x91u, 0x00u,  // $8102 LDA $00:9100,X  the entry's register
      0x8Fu, 0x01u, 0x43u, 0x00u,  // $8106 STA $00:4301    BBAD0
      0xA2u, 0x00u, 0x00u,         // $810A LDX #$0000
      0xBFu, 0x00u, 0x90u, 0x00u,  // $810D LDA $00:9000,X
      0x9Fu, 0x00u, 0x00u, 0x7Fu,  // $8111 STA $7F:0000,X
      0xE8u,                       // $8115 INX
      0xE0u, 0x10u, 0x00u,         // $8116 CPX #$0010
      0xD0u, 0xF2u,                // $8119 BNE $810D
      0xA9u, 0x00u,                // $811B LDA #$00
      0x8Fu, 0x00u, 0x43u, 0x00u,  // $811D STA $00:4300    DMAP0
      0x8Fu, 0x02u, 0x43u, 0x00u,  // $8121 STA $00:4302    A1T0 low
      0x8Fu, 0x03u, 0x43u, 0x00u,  // $8125 STA $00:4303    A1T0 high
      0x8Fu, 0x06u, 0x43u, 0x00u,  // $8129 STA $00:4306    DAS0 high
      0xA9u, 0x7Fu,                // $812D LDA #$7F
      0x8Fu, 0x04u, 0x43u, 0x00u,  // $812F STA $00:4304    A1B0
      0xA9u, 0x10u,                // $8133 LDA #$10
      0x8Fu, 0x05u, 0x43u, 0x00u,  // $8135 STA $00:4305    DAS0 low: 16
      0xA9u, 0x01u,                // $8139 LDA #$01
      0x8Fu, 0x0Bu, 0x42u, 0x00u,  // $813B STA $00:420B    MDMAEN
      0xC2u, 0x20u,                // $813F REP #$20
      0x60u,                       // $8141 RTS
  });
  put(rom, 0x1000u, {0x10u, 0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0x76u, 0x87u,
                     0x98u, 0xA9u, 0xBAu, 0xCBu, 0xDCu, 0xEDu, 0xFEu, 0x0Fu});  // $9000: the bytes
  put(rom, 0x1100u, {0x22u, 0x00u, 0x04u, 0x00u, 0x18u, 0x00u, 0x18u, 0x00u});  // $9100: CGDATA, OAMDATA, VMDATAL twice
  return rom;
}

// The recorded palette file keeps its directory, kind and class; the classes
// the toolkit's runs built its bytes into for the other entries follow `also`
// in class order, whatever order the pieces arrived in.
TEST(RomEnumerate, AnOutcomeSentElsewhereJoinsTheRecordedFileUnderItsClass) {
  const CartridgeDisassembly d = disassembled(crossClassImage());
  const std::string manifest = renderManifest(d);
  const auto file = std::find_if(d.assets.begin(), d.assets.end(),
                                 [](const AssetFile& a) { return a.file == "cgram/00_9000.bin"; });
  ASSERT_NE(file, d.assets.end()) << manifest;
  EXPECT_EQ(file->kind, MovedKind::Staged);
  EXPECT_EQ(file->classes, (std::vector<RegisterClass>{RegisterClass::Cgram}));
  EXPECT_EQ(file->also, (std::vector<RegisterClass>{RegisterClass::Vram, RegisterClass::Oam}));
  EXPECT_NE(manifest.find("asset    cgram/00_9000.bin Cgram as staged from $00:9000 bytes 16 also Vram+Oam\n"),
            std::string::npos)
      << manifest;
  const std::vector<std::string> lines = enumeratedLinesBy(manifest, "sub_008100");
  const auto names = [&](const std::string& to, const std::string& value) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
      return line.starts_with("enumerated cgram/00_9000.bin ") &&
             line.find(" to " + to + " by sub_008100 " + value + " ") != std::string::npos;
    });
  };
  EXPECT_TRUE(names("Cgram", "X=$0002")) << manifest;
  EXPECT_TRUE(names("Oam", "X=$0002")) << manifest;
  EXPECT_TRUE(names("Cgram", "X=$0004,$0006")) << manifest;
  EXPECT_TRUE(names("Vram", "X=$0004,$0006")) << manifest;
  const auto outcome = [&](const std::vector<std::uint32_t>& values) -> const EnumeratedRun* {
    const auto found = std::find_if(d.enumerated.begin(), d.enumerated.end(), [&](const EnumeratedRun& r) {
      return r.routine == 0x008100u && r.values == values;
    });
    return found == d.enumerated.end() ? nullptr : &*found;
  };
  const EnumeratedRun* oam = outcome({2});
  ASSERT_NE(oam, nullptr) << manifest;
  ASSERT_EQ(oam->extents.size(), 1u);
  EXPECT_EQ(oam->extents[0].classes, (std::vector<RegisterClass>{RegisterClass::Cgram, RegisterClass::Oam}));
  const EnumeratedRun* vram = outcome({4, 6});
  ASSERT_NE(vram, nullptr) << manifest;
  ASSERT_EQ(vram->extents.size(), 1u);
  EXPECT_EQ(vram->extents[0].classes, (std::vector<RegisterClass>{RegisterClass::Vram, RegisterClass::Cgram}));
}

// A loader indexed in X over {0, 2, 4, 6} that copies sixteen bytes from the
// blob its entry points at into `$7F:0000` and sends them to the palette. Its
// table points at `$9000`, `$9014`, `$900A` and `$900A`; reset calls it for
// entries 0 and 1, so the run records two files with four bytes between them,
// and the last two entries' bytes lie across both. Sources that touch are one
// run of bytes, so the gap is what keeps the two files apart.
std::vector<std::uint8_t> spanningImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE
      0xC2u, 0x30u,                // $8002 REP #$30
      0xA2u, 0xFFu, 0x1Fu,         // $8004 LDX #$1FFF
      0x9Au,                       // $8007 TXS
      0xA9u, 0x00u, 0x00u,         // $8008 LDA #$0000
      0x5Bu,                       // $800B TCD
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $800C LDA $7E:0010
      0x29u, 0x06u, 0x00u,         // $8010 AND #$0006
      0xAAu,                       // $8013 TAX             X in {0, 2, 4, 6}: entry 0
      0xA0u, 0x00u, 0x00u,         // $8014 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8017 JSR !$8100
      0xAFu, 0x10u, 0x00u, 0x7Eu,  // $801A LDA $7E:0010
      0x09u, 0x02u, 0x00u,         // $801E ORA #$0002
      0x29u, 0x06u, 0x00u,         // $8021 AND #$0006
      0xAAu,                       // $8024 TAX             X in {2, 6}: entry 1
      0xA0u, 0x00u, 0x00u,         // $8025 LDY #$0000
      0x20u, 0x00u, 0x81u,         // $8028 JSR !$8100
      0x80u, 0xFEu,                // $802B BRA *
  });
  put(rom, 0x0100u, {
      0xBFu, 0x00u, 0x82u, 0x00u,  // $8100 LDA $00:8200,X  sub_008100: the blob's address
      0x85u, 0x00u,                // $8104 STA $00         the pointer; its bank at $02 powers on zero
      0xE2u, 0x20u,                // $8106 SEP #$20
      0xA0u, 0x00u, 0x00u,         // $8108 LDY #$0000
      0xA2u, 0x00u, 0x00u,         // $810B LDX #$0000
      0xB7u, 0x00u,                // $810E LDA [$00],Y
      0x9Fu, 0x00u, 0x00u, 0x7Fu,  // $8110 STA $7F:0000,X
      0xC8u,                       // $8114 INY
      0xE8u,                       // $8115 INX
      0xE0u, 0x10u, 0x00u,         // $8116 CPX #$0010
      0xD0u, 0xF3u,                // $8119 BNE $810E
  });
  putPaletteSend(rom, 0x011Bu, 16);  // $811B
  put(rom, 0x0147u, {0x60u});         // $8147 RTS
  put(rom, 0x0200u, {0x00u, 0x90u, 0x14u, 0x90u, 0x0Au, 0x90u, 0x0Au, 0x90u});  // $8200: $9000, $9014, $900A, $900A
  for (std::size_t i = 0; i < 36; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x40u + i);  // $9000: two blobs
  return rom;
}

// A piece whose bytes lie across two recorded files would merge them: it is
// refused, and both files stand as the run recorded them.
TEST(RomEnumerate, AnEnumeratedPieceAcrossTwoRecordedFilesIsRefused) {
  const CartridgeDisassembly d = disassembled(spanningImage());
  const std::string manifest = renderManifest(d);
  EXPECT_TRUE(hasNote(d.notes, "enumerate sub_008100 X=$0004,$0006: its source at $00:900A lies across "
                               "cgram/00_9000.bin and cgram/00_9014.bin; not lifted"))
      << manifest;
  for (const std::string name : {"cgram/00_9000.bin", "cgram/00_9014.bin"}) {
    const auto file = std::find_if(d.assets.begin(), d.assets.end(), [&](const AssetFile& a) { return a.file == name; });
    ASSERT_NE(file, d.assets.end()) << name << "\n" << manifest;
    EXPECT_EQ(file->bytes.size(), 16u) << name;
    EXPECT_TRUE(file->also.empty()) << name;
  }
  for (const std::string& line : enumeratedLinesBy(manifest, "sub_008100")) {
    EXPECT_EQ(line.find("X=$0004"), std::string::npos) << line;
  }
}

TEST(RomEnumerate, LandingsThatMoveAFileOrChangeItsFormAreRefused) {
  const PortLanding map{.memory = PortMemory::Vram,
                        .lowest = 0,
                        .highest = 15,
                        .shown = true,
                        .areas = kAreaTilemap1,
                        .depths = 0,
                        .palette = std::nullopt};
  PortLanding tiles2 = map;
  tiles2.areas = kAreaTiles1;
  tiles2.depths = kDepth2;
  PortLanding tiles4 = tiles2;
  tiles4.depths = kDepth4;
  PortLanding palette = map;
  palette.memory = PortMemory::Cgram;
  palette.areas = kAreaPalette;
  const std::vector<PortLanding> maps{map};
  const std::vector<PortLanding> twoBit{tiles2};
  EXPECT_FALSE(landingsKeepTheFile(maps, std::vector<PortLanding>{tiles2})) << "a map and tiles: the directory moves";
  EXPECT_FALSE(landingsKeepTheFile(twoBit, std::vector<PortLanding>{tiles4})) << "two bits and four: the form changes";
  EXPECT_TRUE(landingsKeepTheFile(twoBit, std::vector<PortLanding>{tiles2}));
  EXPECT_TRUE(landingsKeepTheFile(twoBit, std::vector<PortLanding>{palette})) << "a palette landing says nothing of VRAM";
}

}  // namespace
}  // namespace snaggletooth::disasm
