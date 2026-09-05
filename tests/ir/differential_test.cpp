// The run beside the core: a cartridge built by hand, traced and lifted, then
// replayed on the machine with the interpreter held to every access, every
// register and every cycle. The cases pin what a clean run reports, what an
// interrupt, a released wait, a transfer and an unlifted address do to it, that
// code run through a mirror bank is looked up and reported where the tree places
// it, and that a deliberate break in one effect — a wrong value, a wrong address,
// a dropped flag write, a dropped cycle, a missing or an extra access — is named
// with the node, the effect and the two values.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_differential.h"
#include "rom/input_script.h"
#include "rom/rom_disasm.h"

namespace snaggletooth::ir {
namespace {

using examples::hdmaImage;
using examples::mirroredImage;
using examples::mixedImage;
using examples::ramCodeImage;
using examples::transferImage;
using examples::waitingImage;

constexpr std::uint64_t kFrame = 262u * 1364u;  // one NTSC frame of the master clock

// A cartridge's program: every region traced from its vectors — and, when
// asked, from what a run of `runCycles` reached and landed on — and lifted, the
// nodes of all of them in address order.
Program programOf(const std::vector<std::uint8_t>& rom, std::uint64_t runCycles = 0) {
  disasm::CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = runCycles != 0;
  request.runMasterCycles = runCycles;
  const disasm::CartridgeDisassembly d = disasm::disassembleCartridge(request);
  Program all;
  for (const disasm::RegionListing& region : d.regions) {
    std::vector<std::uint8_t> image;
    for (const disasm::Line& line : region.listing.lines) {
      const std::vector<std::uint8_t>& bytes = line.isCode ? line.instruction.bytes : line.data;
      image.insert(image.end(), bytes.begin(), bytes.end());
    }
    Program one = lift65816(region.listing, image, region.region.first);
    all.nodes.insert(all.nodes.end(), one.nodes.begin(), one.nodes.end());
    all.nmi = one.nmi;
    all.irq = one.irq;
  }
  std::stable_sort(all.nodes.begin(), all.nodes.end(), [](const Node& a, const Node& b) {
    return a.instruction.address < b.instruction.address;
  });
  return all;
}

Node* nodeAt(Program& program, Address address) {
  for (Node& node : program.nodes) {
    if (node.instruction.address == address) return &node;
  }
  ADD_FAILURE() << "no node at " << address;
  return nullptr;
}

Effect* firstEffect(Node& node, Op op) {
  for (Effect& e : node.effects) {
    if (e.op == op) return &e;
  }
  ADD_FAILURE() << "no such effect in the node at " << node.instruction.address;
  return nullptr;
}

DifferentialReport replay(const std::vector<std::uint8_t>& rom, const Program& program,
                          std::uint64_t cycles = 5 * kFrame) {
  Replay r;
  r.rom = rom;
  r.masterCycles = cycles;
  return differential(program, r);
}

std::string describe(const Divergence& d) {
  return d.name + " at " + std::to_string(d.site) + ": " + d.what + " expected " +
         std::to_string(d.expected) + " got " + std::to_string(d.actual);
}

// ---- a clean run --------------------------------------------------------------------

TEST(Differential, ACleanRunReportsNoDivergenceAndEverythingItChecked) {
  const std::vector<std::uint8_t> rom = mixedImage();
  const DifferentialReport report = replay(rom, programOf(rom));
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  EXPECT_EQ(report.interrupts, 3u);
  EXPECT_GT(report.instructions, 20u);
  EXPECT_GT(report.cpuCycles, report.instructions);
  EXPECT_EQ(report.unlifted, 0u);
  EXPECT_EQ(report.heldSteps, 0u);
  EXPECT_EQ(report.constructs.at("NMI"), 3u);
  EXPECT_EQ(report.constructs.at("STP"), 1u);
  EXPECT_EQ(report.constructs.at("a block move byte"), 2u);
  EXPECT_EQ(report.constructs.at("a block move re-entered"), 1u);
  EXPECT_EQ(report.constructs.at("a 16-bit read-modify-write"), 1u);
  EXPECT_EQ(report.constructs.at("JSR or JSL"), 1u);
  EXPECT_EQ(report.constructs.at("RTS or RTL"), 1u);
  EXPECT_EQ(report.constructs.at("PLB or PLD"), 1u);
  EXPECT_EQ(report.constructs.at("an indexed absolute form"), 1u);
  EXPECT_GE(report.constructs.at("a taken branch"), 2u);
  EXPECT_GE(report.constructs.at("a store to a hardware register"), 1u);
  EXPECT_GE(report.constructs.at("a load from a hardware register"), 3u);
  EXPECT_EQ(report.constructs.at("IRQ"), 0u) << "a construct the run never reached reads zero";
  EXPECT_EQ(report.forms.at("STA abs e=0 m=16 x=16"), 1u);
  // The handler is traced from the native vector, where the widths are the
  // live flags': its nodes select by them, and the run proves the selection.
  EXPECT_EQ(report.forms.at("RTI  e=0 m=? x=?"), 3u);
  EXPECT_EQ(report.constructs.at("a node with a live-flag width"), 9u);
}

TEST(Differential, AWaitIsReleasedByTheInterruptThatFollows) {
  const std::vector<std::uint8_t> rom = waitingImage();
  const DifferentialReport report = replay(rom, programOf(rom));
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  EXPECT_EQ(report.interrupts, 2u);
  EXPECT_EQ(report.releases, 2u);
  EXPECT_GT(report.haltedCycles, 1000u);
  EXPECT_EQ(report.constructs.at("WAI"), 2u);
  EXPECT_EQ(report.constructs.at("a wait released"), 2u);
}

TEST(Differential, ATransfersAccessesAreNotTheCpusAndItsBytesAreReadBack) {
  const std::vector<std::uint8_t> rom = transferImage();
  const DifferentialReport report = replay(rom, programOf(rom));
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  // The engine held the bus inside the first NOP; the CPU's own cycles for that
  // NOP were still two, and the load after it read what the engine moved.
  EXPECT_EQ(report.forms.at("NOP  e=1"), 2u);
  EXPECT_EQ(report.forms.at("LDA abs e=1"), 1u);
}

TEST(Differential, AnHdmaEventThatHoldsTheCpuAtABoundaryIsASkippedStep) {
  const std::vector<std::uint8_t> rom = hdmaImage();
  const DifferentialReport report = replay(rom, programOf(rom));
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  EXPECT_EQ(report.interrupts, 3u);
  EXPECT_GE(report.heldSteps, 1u);
}

TEST(Differential, AnInstructionWithNoNodeIsCountedAndTheInterpreterRealigned) {
  const std::vector<std::uint8_t> rom = ramCodeImage();
  // The tree traced from what a run landed on, so the stores after the
  // routine's returns are nodes; the routine itself, in work RAM, never is.
  const DifferentialReport report = replay(rom, programOf(rom, kFrame));
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  EXPECT_EQ(report.unlifted, 4u) << "INC A and RTL, then DEC A and RTL, in work RAM";
  ASSERT_EQ(report.unliftedSites.size(), 2u);
  EXPECT_EQ(report.unliftedSites[0], 0x7E2000u);
  EXPECT_EQ(report.unliftedSites[1], 0x7E2001u);
  // The stores after each return ran from the registers the routine left:
  // checked, not diverged.
  EXPECT_EQ(report.forms.at("STA abs e=1"), 3u);
  EXPECT_EQ(report.forms.at("STA dp e=1"), 4u);
  EXPECT_EQ(report.constructs.at("RTI"), 1u) << "the RTI into the frame the program built";
}

// ---- a program run through a mirror bank -----------------------------------------------

TEST(Differential, CodeRunThroughAMirrorBankIsCheckedAgainstTheNodesTheTreePlaces) {
  const std::vector<std::uint8_t> rom = mirroredImage();
  const DifferentialReport report = replay(rom, programOf(rom));
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  EXPECT_EQ(report.unlifted, 0u) << "the four instructions at $80:8010 are the nodes at $00:8010";
  EXPECT_TRUE(report.unliftedSites.empty());
  EXPECT_EQ(report.forms.at("STA abs e=0 m=8 x=8"), 1u);
  EXPECT_EQ(report.forms.at("INC abs e=0 m=8 x=8"), 1u);
  EXPECT_EQ(report.constructs.at("STP"), 1u);
}

TEST(Differential, ADivergenceInAMirrorBankNamesTheSiteTheTreePlaces) {
  const std::vector<std::uint8_t> rom = mirroredImage();
  Program program = programOf(rom);
  Node* inc = nodeAt(program, 0x008015u);  // INC !$0100, run as $80:8015
  ASSERT_NE(inc, nullptr);
  Effect* increment = firstEffect(*inc, Op::Inc);
  ASSERT_NE(increment, nullptr);
  increment->op = Op::Dec;
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x008015u) << "the address a reader finds in the tree, not $80:8015";
  EXPECT_EQ(d.name, "INC");
  EXPECT_EQ(d.what, "write value");
  EXPECT_EQ(d.expected, 0x13u);
  EXPECT_EQ(d.actual, 0x11u);
}

TEST(Differential, TheRunEndsAtTheBudgetWhenTheProgramDoesNotStop) {
  const std::vector<std::uint8_t> rom = mixedImage();
  const DifferentialReport report = replay(rom, programOf(rom), 20'000u);
  EXPECT_FALSE(report.stopped);
  EXPECT_GE(report.masterCycles, 20'000u);
  EXPECT_EQ(report.interrupts, 0u);
}

// ---- a break in one effect is named --------------------------------------------------

TEST(Differential, AWrongStoredValueIsNamedWithTheNodeTheEffectAndBothValues) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* sta = nodeAt(program, 0x00800Au);  // STA !$0100
  ASSERT_NE(sta, nullptr);
  Effect* store = firstEffect(*sta, Op::Store);
  ASSERT_NE(store, nullptr);
  store->b.place = Place::X;  // the value stored is X, not A
  const std::size_t index = static_cast<std::size_t>(store - sta->effects.data());
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x00800Au);
  EXPECT_EQ(d.name, "STA");
  EXPECT_FALSE(d.mode.emulation);
  ASSERT_TRUE(d.effect.has_value());
  EXPECT_EQ(*d.effect, index);
  EXPECT_EQ(d.what, "write value");
  EXPECT_EQ(d.expected, 0x34u) << "the low byte of $1234, first";
  EXPECT_EQ(d.actual, 0x02u) << "the low byte of X";
}

TEST(Differential, AWrongReadAddressIsNamed) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* lda = nodeAt(program, 0x008016u);  // LDA !$FFFF,X, X = 2: into bank $01
  ASSERT_NE(lda, nullptr);
  Effect* address = firstEffect(*lda, Op::BankAddress);
  ASSERT_NE(address, nullptr);
  address->a.value = 0xFFFEu;
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x008016u);
  EXPECT_EQ(d.what, "read address");
  EXPECT_EQ(d.expected, 0x010001u) << "the machine crossed into the next bank";
  EXPECT_EQ(d.actual, 0x010000u);
}

TEST(Differential, AMaskedRequestIsNotTakenAndAWaitItEndsRunsNoSequence) {
  const std::vector<std::uint8_t> rom = examples::irqImage();
  const DifferentialReport report = replay(rom, programOf(rom), 8 * kFrame);
  ASSERT_TRUE(report.divergences.empty()) << describe(report.divergences.front());
  EXPECT_TRUE(report.stopped);
  EXPECT_EQ(report.interrupts, 2u) << "both after the flag is cleared";
  EXPECT_EQ(report.releases, 1u) << "the wait, ended by the asserted line while masked";
  EXPECT_EQ(report.constructs.at("IRQ"), 2u);
  EXPECT_EQ(report.constructs.at("NMI"), 0u);
  EXPECT_EQ(report.constructs.at("WAI"), 1u);
  EXPECT_EQ(report.constructs.at("a wait released"), 1u);
}

TEST(Differential, TheRecordedRunIsPresentedOnTheFrameItNames) {
  // Start held on one frame only. The cartridge sets its flag only if the
  // tenth interrupt's auto-read saw Start, and stops only if the flag is set.
  const std::vector<std::uint8_t> rom = examples::framePressImage();
  const Program program = programOf(rom);
  auto runWith = [&](std::uint32_t frame) {
    std::string error;
    const std::optional<disasm::InputScript> script = disasm::parseInputScript(
        "frame " + std::to_string(frame) + " 1 start\nframe " + std::to_string(frame + 1) +
            " 1 none\n",
        error);
    EXPECT_TRUE(script.has_value()) << error;
    Replay r;
    r.rom = rom;
    r.masterCycles = 20 * kFrame;
    r.input = script.value_or(disasm::InputScript{});
    return differential(program, r);
  };
  const DifferentialReport onTime = runWith(9);
  ASSERT_TRUE(onTime.divergences.empty()) << describe(onTime.divergences.front());
  EXPECT_TRUE(onTime.stopped) << "Start on the frame the tenth interrupt reads";
  const DifferentialReport early = runWith(8);
  EXPECT_FALSE(early.stopped);
  const DifferentialReport late = runWith(10);
  EXPECT_FALSE(late.stopped);
  EXPECT_TRUE(early.divergences.empty() && late.divergences.empty());
}

TEST(Differential, ADroppedFlagWriteIsARegisterDivergenceWithNoEffectNamed) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* cmp = nodeAt(program, 0x008029u);  // CMP #$03
  ASSERT_NE(cmp, nullptr);
  Effect* compare = firstEffect(*cmp, Op::Cmp);
  ASSERT_NE(compare, nullptr);
  compare->op = Op::Set;  // no flags written; the destination is none, so nothing else moves
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x008029u);
  EXPECT_EQ(d.what, "register p");
  EXPECT_FALSE(d.effect.has_value());
  EXPECT_NE(d.expected, d.actual);
}

TEST(Differential, ADroppedCycleIsACycleDivergence) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* bne = nodeAt(program, 0x00802Bu);  // BNE $8023
  ASSERT_NE(bne, nullptr);
  Effect* taken = firstEffect(*bne, Op::Cycles);
  ASSERT_NE(taken, nullptr);
  taken->a.value = 0;
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x00802Bu);
  EXPECT_EQ(d.what, "cycles");
  EXPECT_EQ(d.expected, 3u);
  EXPECT_EQ(d.actual, 2u);
}

TEST(Differential, AMissingAccessIsTheMachinesAccessTheNodeDidNotMake) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* sta = nodeAt(program, 0x008014u);  // STA $10
  ASSERT_NE(sta, nullptr);
  std::erase_if(sta->effects, [](const Effect& e) { return e.op == Op::Store; });
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x008014u);
  EXPECT_EQ(d.what, "accesses the machine made that the node did not");
  EXPECT_EQ(d.expected, 0x000010u);
  EXPECT_EQ(d.actual, 1u);
}

TEST(Differential, AnExtraAccessIsAReadTheMachineDidNotMake) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* pha = nodeAt(program, 0x008019u);  // PHA
  ASSERT_NE(pha, nullptr);
  Effect extra;
  extra.op = Op::Load;
  extra.dst.place = Place::T3;
  extra.a.place = Place::D;
  extra.width = Width::Byte;
  pha->effects.push_back(extra);
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x008019u);
  EXPECT_EQ(d.what, "a read the machine did not make");
  ASSERT_TRUE(d.effect.has_value());
  EXPECT_EQ(*d.effect, pha->effects.size() - 1);
}

TEST(Differential, AWrongAccessKindIsNamed) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* inc = nodeAt(program, 0x00800Du);  // INC !$0100, sixteen bits
  ASSERT_NE(inc, nullptr);
  Effect* load = firstEffect(*inc, Op::Load);
  ASSERT_NE(load, nullptr);
  load->access = Access::Data;
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.site, 0x00800Du);
  EXPECT_EQ(d.what, "read kind");
  EXPECT_EQ(d.expected, static_cast<std::uint32_t>(CycleKind::RmwRead));
}

TEST(Differential, ABreakInTheInterruptSequenceIsNamedAsTheSequence) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  // The sequence pushes the status byte; push P's complement instead.
  Effect* push = nullptr;
  for (Effect& e : program.nmi) {
    if (e.op == Op::Push && e.width == Width::Byte) push = &e;
  }
  ASSERT_NE(push, nullptr);
  push->a.place = Place::D;
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  const Divergence& d = report.divergences.front();
  EXPECT_EQ(d.name, "NMI");
  EXPECT_EQ(d.what, "write value");
  ASSERT_TRUE(d.effect.has_value());
}

// After a divergence the interpreter takes the machine's registers, so the
// store that follows a wrong load stores what the machine stored, and the run's
// one disagreement is the load's.
TEST(Differential, TheStepAfterADivergenceRunsFromTheMachinesRegisters) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* lda = nodeAt(program, 0x008007u);  // LDA #$1234, then STA !$0100
  ASSERT_NE(lda, nullptr);
  Effect* load = firstEffect(*lda, Op::SetNZ);
  ASSERT_NE(load, nullptr);
  load->a.value = 0x4321u;
  const DifferentialReport report = replay(rom, program);
  ASSERT_FALSE(report.divergences.empty());
  for (const Divergence& d : report.divergences) {
    EXPECT_EQ(d.site, 0x008007u) << d.what << ": the store after it ran from the machine's A";
  }
  EXPECT_EQ(report.forms.at("STA abs e=0 m=16 x=16"), 1u);
  EXPECT_TRUE(report.stopped);
}

TEST(Differential, TheRunGoesOnRealignedAfterADivergenceUpToTheLimit) {
  const std::vector<std::uint8_t> rom = mixedImage();
  Program program = programOf(rom);
  Node* inc = nodeAt(program, 0x008303u);  // INC !$0104, in the handler
  ASSERT_NE(inc, nullptr);
  Effect* increment = firstEffect(*inc, Op::Inc);
  ASSERT_NE(increment, nullptr);
  increment->op = Op::Dec;  // the count goes the other way every time
  Replay unlimited;
  unlimited.rom = rom;
  unlimited.masterCycles = 5 * kFrame;
  unlimited.divergenceLimit = 1000;
  const DifferentialReport whole = differential(program, unlimited);
  EXPECT_TRUE(whole.stopped) << "realigned every time, the program still reaches its stop";
  // One step per interrupt diverges; a step may record more than one
  // disagreement — the value written and the flags after it.
  std::vector<std::uint64_t> steps;
  for (const Divergence& d : whole.divergences) {
    EXPECT_EQ(d.site, 0x008303u);
    if (steps.empty() || steps.back() != d.instruction) steps.push_back(d.instruction);
  }
  EXPECT_EQ(steps.size(), 3u) << "once per interrupt";

  Replay one = unlimited;
  one.divergenceLimit = 1;
  const DifferentialReport first = differential(program, one);
  ASSERT_FALSE(first.divergences.empty());
  for (const Divergence& d : first.divergences) {
    EXPECT_EQ(d.instruction, first.divergences.front().instruction) << "the run ends at that step";
  }
  EXPECT_FALSE(first.stopped);
  EXPECT_LT(first.instructions, whole.instructions);
}

}  // namespace
}  // namespace snaggletooth::ir
