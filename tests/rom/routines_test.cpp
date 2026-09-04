// The routines a trace holds: where each begins, which lines it holds, which
// routines it calls, and its role — the hardware its own lines reach and the
// hardware its calls reach.
//
// Checked against one hand-built cartridge whose reset code takes every flow a
// walk has to answer: a branch with both arms, a jump past the bytes after it,
// two calls, a halt; a called routine that falls through into the next label's
// code; two routines that call each other; an interrupt handler that ends in a
// jump through a pointer; and a target only a run reached. A halt, a return and
// a jump each have another routine's code right after them, so falling through
// any of them would show.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "rom/rom_disasm.h"
#include "rom/rom_facts.h"
#include "rom/rom_verify.h"

namespace snaggletooth::disasm {
namespace {

using examples::loRomImage;
using examples::put;
using examples::threeBankImage;

// The cartridge. Every address the cases name is commented.
std::vector<std::uint8_t> routineImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x3A] = 0x69u;  // nmi (emulation) -> $8069
  rom[site + 0x3B] = 0x80u;

  // reset: a branch, a jump, two calls, and a halt.
  put(rom, 0x0000u, {
                        0x18u,                     // $8000 CLC
                        0xFBu,                     // $8001 XCE          -> native
                        0xE2u, 0x30u,              // $8002 SEP #$30     -> A8, X8
                        0xA9u, 0x8Fu,              // $8004 LDA #$8F
                        0x8Du, 0x00u, 0x21u,       // $8006 STA !$2100   INIDISP, Display
                        0x20u, 0x40u, 0x80u,       // $8009 JSR !$8040   sub_008040
                        0x20u, 0x60u, 0x80u,       // $800C JSR !$8060   sub_008060
                        0xADu, 0x10u, 0x42u,       // $800F LDA !$4210   RDNMI, Interrupt
                        0xF0u, 0x03u,              // $8012 BEQ $8017    both arms are reset's
                        0x4Cu, 0x20u, 0x80u,       // $8014 JMP !$8020   loc_008020
                        0xEAu,                     // $8017 NOP          loc_008017
                        0x8Du, 0x05u, 0x21u,       // $8018 STA !$2105   BGMODE, Background
                        0xDBu,                     // $801B STP          a halt
                        0x60u,                     // $801C RTS          after_halt, a person's entry
                    });
  // The jump's target, which jumps back to the line after the branch's target;
  // the bytes after the BRA belong to a routine only a run reached.
  put(rom, 0x0020u, {
                        0xA9u, 0x00u,              // $8020 LDA #$00
                        0x8Du, 0x22u, 0x21u,       // $8022 STA !$2122   CGDATA, Cgram
                        0x80u, 0xF1u,              // $8025 BRA $8018
                        0x8Du, 0x40u, 0x21u,       // $8027 STA !$2140   APUIO0, Apu — sub_008027
                        0x60u,                     // $802A RTS
                    });
  // sub_008040 calls sub_008060 and falls through into sub_008048, which calls
  // sub_008060 too and returns.
  put(rom, 0x0040u, {
                        0xA9u, 0x80u,              // $8040 LDA #$80
                        0x8Du, 0x16u, 0x21u,       // $8042 STA !$2116   VMADDL, Vram
                        0x20u, 0x60u, 0x80u,       // $8045 JSR !$8060
                        0x20u, 0x60u, 0x80u,       // $8048 JSR !$8060   sub_008048
                        0xA9u, 0x01u,              // $804B LDA #$01
                        0x8Du, 0x0Bu, 0x42u,       // $804D STA !$420B   MDMAEN, DmaControl
                        0x60u,                     // $8050 RTS
                    });
  // sub_008060 sets a channel's destination and calls sub_008048: a cycle.
  put(rom, 0x0060u, {
                        0xA9u, 0x04u,              // $8060 LDA #$04
                        0x8Du, 0x01u, 0x43u,       // $8062 STA !$4301   BBAD0 = $04: OAMDATA, Oam
                        0x20u, 0x48u, 0x80u,       // $8065 JSR !$8048
                        0x60u,                     // $8068 RTS
                        // nmi, right after the return, reads a flag and jumps
                        // through a table.
                        0xADu, 0x12u, 0x42u,       // $8069 LDA !$4212   HVBJOY, Interrupt
                        0x7Cu, 0x00u, 0x81u,       // $806C JMP (!$8100,X)
                        0x60u,                     // $806F RTS — never reached
                    });
  return rom;
}

CartridgeDisassembly disassembled(std::span<const std::uint8_t> rom, bool withRun = false) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries.push_back(TraceEntry{.address = 0x00801Cu,
                                       .mode = Cpu65816Mode::native(true, true),
                                       .name = "after_halt"});
  if (withRun) {
    request.reached.push_back(ReachedTarget{.target = 0x008027u,
                                            .mode = Cpu65816Mode::native(true, true),
                                            .site = 0x00806Cu,
                                            .call = true,
                                            .name = {}});
  }
  return disassembleCartridge(request);
}

const Routine* routineAt(const std::vector<Routine>& routines, Address address) {
  for (const Routine& routine : routines) {
    if (routine.address == address) return &routine;
  }
  return nullptr;
}

bool holds(const Routine& routine, Address address) {
  for (const Address at : routine.lines) {
    if (at == address) return true;
  }
  return false;
}

}  // namespace

// ---- where a routine begins -------------------------------------------------

TEST(RomRoutines, EveryEntryAndEveryCallTargetIsARoutine) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  std::vector<Address> starts;
  for (const Routine& routine : d.routines) starts.push_back(routine.address);
  EXPECT_EQ(starts, (std::vector<Address>{0x008000u, 0x00801Cu, 0x008040u, 0x008048u, 0x008060u,
                                          0x008069u}));
  EXPECT_EQ(routineAt(d.routines, 0x008000u)->label, "reset");
  EXPECT_EQ(routineAt(d.routines, 0x00801Cu)->label, "after_halt");
  EXPECT_EQ(routineAt(d.routines, 0x008040u)->label, "sub_008040");
  EXPECT_EQ(routineAt(d.routines, 0x008069u)->label, "nmi");
}

// A label only a branch or a jump names begins no routine of its own: it is
// inside the routine that reaches it.
TEST(RomRoutines, ABranchOrJumpTargetIsNotARoutine) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_EQ(routineAt(d.routines, 0x008017u), nullptr);
  EXPECT_EQ(routineAt(d.routines, 0x008018u), nullptr);
  EXPECT_EQ(routineAt(d.routines, 0x008020u), nullptr);
}

TEST(RomRoutines, ATargetARunReachedIsARoutine) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom, true);
  const Routine* seen = routineAt(d.routines, 0x008027u);
  ASSERT_NE(seen, nullptr);
  EXPECT_EQ(seen->label, "sub_008027");
  EXPECT_EQ(seen->lines, (std::vector<Address>{0x008027u, 0x00802Au}));
  EXPECT_EQ(seen->reaches, (std::vector<RegisterClass>{RegisterClass::Apu}));
  EXPECT_EQ(routineAt(d.routines, 0x008027u), seen);
  EXPECT_EQ(routineAt(disassembled(rom).routines, 0x008027u), nullptr)
      << "without the run the bytes there were never reached";
}

// ---- what a routine holds ---------------------------------------------------

TEST(RomRoutines, ABranchReachesBothArms) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  const Routine& reset = *routineAt(d.routines, 0x008000u);
  EXPECT_TRUE(holds(reset, 0x008014u)) << "the fall-through";
  EXPECT_TRUE(holds(reset, 0x008017u)) << "the target, which nothing else reaches";
}

TEST(RomRoutines, AJumpReachesItsTargetAndNotTheBytesAfterIt) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom, true);
  const Routine& reset = *routineAt(d.routines, 0x008000u);
  EXPECT_TRUE(holds(reset, 0x008020u)) << "JMP !$8020";
  EXPECT_TRUE(holds(reset, 0x008025u)) << "and the BRA at its end";
  EXPECT_FALSE(holds(reset, 0x008027u)) << "the code after the BRA is another routine's";
}

TEST(RomRoutines, AHaltEndsTheWalk) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  const Routine& reset = *routineAt(d.routines, 0x008000u);
  EXPECT_TRUE(holds(reset, 0x00801Bu)) << "the STP itself";
  EXPECT_FALSE(holds(reset, 0x00801Cu)) << "the code right after it is another routine's";
  EXPECT_EQ(reset.lines.size(), 16u);
  EXPECT_EQ(reset.bytes, 35u);
}

TEST(RomRoutines, AReturnEndsTheWalk) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  const Routine& sub60 = *routineAt(d.routines, 0x008060u);
  EXPECT_EQ(sub60.lines, (std::vector<Address>{0x008060u, 0x008062u, 0x008065u, 0x008068u}))
      << "the code right after the RTS is another routine's";
  EXPECT_EQ(sub60.bytes, 9u);
}

TEST(RomRoutines, ACallDoesNotEnterTheRoutineItNames) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  const Routine& reset = *routineAt(d.routines, 0x008000u);
  EXPECT_FALSE(holds(reset, 0x008040u));
  EXPECT_FALSE(holds(reset, 0x008060u));
  EXPECT_TRUE(holds(reset, 0x00800Cu)) << "execution resumes after the call";
}

// Nothing but a return, a halt or an unnamed target ends the walk, so a routine
// with no return of its own runs on into the next label's code.
TEST(RomRoutines, FallingThroughIntoTheNextLabelHoldsItsLinesToo) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  const Routine& sub40 = *routineAt(d.routines, 0x008040u);
  EXPECT_EQ(sub40.lines, (std::vector<Address>{0x008040u, 0x008042u, 0x008045u, 0x008048u,
                                               0x00804Bu, 0x00804Du, 0x008050u}));
  EXPECT_EQ(sub40.bytes, 17u);
}

TEST(RomRoutines, ALineTwoRoutinesReachIsInBoth) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_TRUE(holds(*routineAt(d.routines, 0x008040u), 0x00804Du));
  EXPECT_TRUE(holds(*routineAt(d.routines, 0x008048u), 0x00804Du));
}

TEST(RomRoutines, AJumpThroughAPointerEndsTheWalk) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom, true);
  const Routine& nmi = *routineAt(d.routines, 0x008069u);
  EXPECT_EQ(nmi.lines, (std::vector<Address>{0x008069u, 0x00806Cu}));
  EXPECT_EQ(nmi.bytes, 6u);
  EXPECT_TRUE(nmi.calls.empty()) << "a target the run saw is not a call the bytes name";
}

// ---- the call graph ---------------------------------------------------------

TEST(RomRoutines, TheCallsAreTheRoutinesTheCallInstructionsName) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_EQ(routineAt(d.routines, 0x008000u)->calls, (std::vector<Address>{0x008040u, 0x008060u}));
  EXPECT_EQ(routineAt(d.routines, 0x008040u)->calls, (std::vector<Address>{0x008060u}))
      << "two calls to one routine are one edge";
  EXPECT_EQ(routineAt(d.routines, 0x008048u)->calls, (std::vector<Address>{0x008060u}));
  EXPECT_EQ(routineAt(d.routines, 0x008060u)->calls, (std::vector<Address>{0x008048u}));
  EXPECT_TRUE(routineAt(d.routines, 0x008069u)->calls.empty());
}

// ---- the role ---------------------------------------------------------------

TEST(RomRoutines, TheRoleIsWhatTheRoutinesOwnLinesReach) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_EQ(routineAt(d.routines, 0x008000u)->reaches,
            (std::vector<RegisterClass>{RegisterClass::Display, RegisterClass::Background,
                                        RegisterClass::Cgram, RegisterClass::Interrupt}))
      << "the classes in the table's order, each once, and none from the calls";
  EXPECT_EQ(routineAt(d.routines, 0x008040u)->reaches,
            (std::vector<RegisterClass>{RegisterClass::Vram, RegisterClass::DmaControl}))
      << "the lines it fell through into count as its own";
  EXPECT_EQ(routineAt(d.routines, 0x008069u)->reaches,
            (std::vector<RegisterClass>{RegisterClass::Interrupt}));
}

// A transfer reaches the register its channel is pointed at, so the role holds
// the destination's class beside the channel register's own.
TEST(RomRoutines, ATransferReachesItsDestination) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_EQ(routineAt(d.routines, 0x008060u)->reaches,
            (std::vector<RegisterClass>{RegisterClass::Oam, RegisterClass::DmaChannel}));
}

TEST(RomRoutines, TheRoleThroughCallsFollowsEveryCallInTurn) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_EQ(routineAt(d.routines, 0x008000u)->through,
            (std::vector<RegisterClass>{RegisterClass::Vram, RegisterClass::Oam,
                                        RegisterClass::DmaControl, RegisterClass::DmaChannel}));
  // Two routines that call each other each reach the other's classes, and the
  // walk ends.
  EXPECT_EQ(routineAt(d.routines, 0x008060u)->through,
            (std::vector<RegisterClass>{RegisterClass::Oam, RegisterClass::DmaControl,
                                        RegisterClass::DmaChannel}));
  EXPECT_EQ(routineAt(d.routines, 0x008048u)->through,
            (std::vector<RegisterClass>{RegisterClass::Oam, RegisterClass::DmaControl,
                                        RegisterClass::DmaChannel}));
  EXPECT_TRUE(routineAt(d.routines, 0x008069u)->through.empty());
}

// ---- the manifest -----------------------------------------------------------

TEST(RomRoutines, TheManifestCarriesEveryRoutineWithEveryFieldPresent) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("routine  $00:8000 reset lines 16 bytes 35 calls sub_008040,sub_008060 "
                          "reaches Display,Background,Cgram,Interrupt "
                          "through Vram,Oam,DmaControl,DmaChannel\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("routine  $00:8069 nmi lines 2 bytes 6 calls none reaches Interrupt "
                          "through none\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("routine  $00:801C after_halt lines 1 bytes 1 calls none reaches none "
                          "through none\n"),
            std::string::npos)
      << manifest;
}

TEST(RomRoutines, TheRoutineKindParsesAndAnUnknownKindIsStillAnError) {
  const std::vector<std::uint8_t> rom = routineImage();
  const CartridgeDisassembly d = disassembled(rom);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(d), error);
  ASSERT_TRUE(input.has_value()) << error;
  EXPECT_FALSE(parseManifest("role $00:8000 reset\n", error).has_value());
  EXPECT_NE(error.find("not a manifest line"), std::string::npos);
}

// The routines ride in the manifest and nowhere else, so a tree that verified
// before them verifies after them.
TEST(RomRoutines, TheTreeStillAssemblesToItsImage) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembled(rom);
  EXPECT_FALSE(d.routines.empty());

  std::map<std::string, std::string> tree;
  for (const RegionListing& region : d.regions) {
    tree[region.region.file] = renderRegion(region, d);
  }
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
