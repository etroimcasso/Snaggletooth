// What every path proves, over one hand-built cartridge that proves each
// register the way real code does and dispatches through three tables — see
// `tools/examples/proving/`. The cases pin what is proven and what is not: an
// immediate into the direct register, an index into the stack pointer, the data
// bank through the stack both ways; a value carried across a call that gives the
// register back; a direct-page operand reaching the register the direct register
// puts it on; two paths agreeing and two disagreeing; a table an index bounded by
// a mask derives whole, one bounded by a compare and a branch derives whole, one
// nothing bounds is left alone with its stop; what a call the analysis cannot
// follow leaves behind; what the manifest writes, reads back and traces from;
// and that the rule the value facts had before is a subset of the rule they have
// now, over every example cartridge.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816_disasm.h"
#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_dataflow.h"
#include "rom/rom_disasm.h"
#include "rom/rom_facts.h"

namespace snaggletooth::ir {
namespace {

using disasm::CartridgeDisassembly;
using disasm::CartridgeRequest;
using disasm::Cpu65816Mode;
using disasm::HardwareAccess;
using disasm::StateFact;
using examples::provingImage;

CartridgeDisassembly disassemble(std::span<const std::uint8_t> rom, CartridgeRequest request = {}) {
  request.rom = rom;
  request.captureSound = false;
  return disasm::disassembleCartridge(request);
}

const StateFact* stateAt(const CartridgeDisassembly& d, Address address) {
  for (const StateFact& state : d.states) {
    if (state.address == address) return &state;
  }
  return nullptr;
}

const HardwareAccess* accessAt(const CartridgeDisassembly& d, Address site, std::string_view name) {
  for (const HardwareAccess& fact : d.accesses) {
    if (fact.site == site && fact.name == name) return &fact;
  }
  return nullptr;
}

std::vector<const disasm::DerivedTarget*> derivedFrom(const CartridgeDisassembly& d, Address site) {
  std::vector<const disasm::DerivedTarget*> out;
  for (const disasm::DerivedTarget& derived : d.derived) {
    if (derived.site == site) out.push_back(&derived);
  }
  return out;
}

bool stopsAt(const CartridgeDisassembly& d, Address address) {
  return std::any_of(d.stops.begin(), d.stops.end(),
                     [address](const disasm::TraceStop& stop) { return stop.address == address; });
}

std::optional<std::string> labelAt(const CartridgeDisassembly& d, Address address) {
  for (const disasm::RegionListing& region : d.regions) {
    const auto found = region.listing.labels.find(address);
    if (found != region.listing.labels.end()) return found->second;
  }
  return std::nullopt;
}

const std::vector<std::uint8_t>& proving() {
  static const std::vector<std::uint8_t> rom = provingImage();
  return rom;
}

const CartridgeDisassembly& provingTree() {
  static const CartridgeDisassembly tree = disassemble(proving());
  return tree;
}

// One instruction placed at `address`, decoded under `mode` and lifted.
Node nodeOf(std::vector<std::uint8_t> bytes, Address address, const Cpu65816Mode& mode) {
  const std::optional<disasm::Instruction> decoded = disasm::decodeAt(bytes, address, address, mode);
  EXPECT_TRUE(decoded.has_value());
  return liftInstruction(*decoded, mode);
}

const ImageReader kNoImage = [](Address) { return std::optional<std::uint8_t>{}; };

}  // namespace

// ---- what the idioms prove ----------------------------------------------------------

TEST(Dataflow, AnImmediateTransferredIntoTheDirectRegisterProvesIt) {
  const CartridgeDisassembly& d = provingTree();
  // `LDA #$4300` / `TCD` at reset carries into the routine the reset code calls;
  // the routine's second caller brings the three values its own paths carry.
  const StateFact* routine = stateAt(d, 0x008100u);
  ASSERT_NE(routine, nullptr);
  EXPECT_EQ(routine->d, (std::vector<std::uint32_t>{0x0000u, 0x0100u, 0x0200u, 0x4300u}));
  // `LDA #$0000` / `TCD` after it is what the label at $802A sees.
  const StateFact* later = stateAt(d, 0x00802Au);
  ASSERT_NE(later, nullptr);
  EXPECT_EQ(later->d, std::vector<std::uint32_t>{0x0000u});
}

TEST(Dataflow, AnImmediateTransferredIntoTheStackPointerProvesItAndACallMovesIt) {
  const CartridgeDisassembly& d = provingTree();
  const StateFact* routine = stateAt(d, 0x008100u);
  ASSERT_NE(routine, nullptr);
  EXPECT_EQ(routine->s, std::vector<std::uint32_t>{0x1FFDu}) << "the call pushed two bytes onto $1FFF";
  const StateFact* after = stateAt(d, 0x00802Au);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->s, std::vector<std::uint32_t>{0x1FFFu}) << "the routine is balanced, so the return gives it back";
}

TEST(Dataflow, TheProgramBankPushedAndPulledIntoTheDataBankProvesIt) {
  const CartridgeDisassembly& d = provingTree();
  const StateFact* target = stateAt(d, 0x008300u);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->dbr, std::vector<std::uint32_t>{0x00u});
}

TEST(Dataflow, AnEffectiveAddressPushedAndPulledTwiceProvesTheDataBank) {
  const CartridgeDisassembly& d = provingTree();
  const StateFact* label = stateAt(d, 0x00802Au);
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->dbr, std::vector<std::uint32_t>{0x7Eu}) << "PEA $7E7E, then PLB twice";
}

TEST(Dataflow, TheResetVectorBeginsWithTheDirectRegisterAndDataBankClearedAndNothingElse) {
  const CartridgeDisassembly& d = provingTree();
  const StateFact* reset = stateAt(d, 0x008000u);
  ASSERT_NE(reset, nullptr);
  EXPECT_EQ(reset->d, std::vector<std::uint32_t>{0u});
  EXPECT_EQ(reset->dbr, std::vector<std::uint32_t>{0u});
  EXPECT_TRUE(reset->s.empty()) << "the chip sets the stack pointer's high byte only";
}

// ---- what the facts gain ------------------------------------------------------------

TEST(Dataflow, ADirectPageOperandUnderAProvenDirectRegisterReachesTheRegisterItLandsOn) {
  const CartridgeDisassembly& d = provingTree();
  const HardwareAccess* dmap = accessAt(d, 0x008010u, "DMAP0");
  ASSERT_NE(dmap, nullptr) << "STA $00 with D = $4300 is DMAP0";
  EXPECT_EQ(dmap->registerAddress, 0x4300u);
  ASSERT_TRUE(dmap->value.has_value());
  EXPECT_EQ(*dmap->value, 0x01u);
  const HardwareAccess* bbad = accessAt(d, 0x008014u, "BBAD0");
  ASSERT_NE(bbad, nullptr);
  ASSERT_TRUE(bbad->value.has_value());
  EXPECT_EQ(*bbad->value, 0x18u);
}

TEST(Dataflow, AValueIsProvenAcrossACallThatGivesTheRegisterBack) {
  const CartridgeDisassembly& d = provingTree();
  // The routine pushes A, writes CGADD, pulls A and returns; the store after the
  // call writes what the load before it loaded.
  const HardwareAccess* fact = accessAt(d, 0x008019u, "A1T0L");
  ASSERT_NE(fact, nullptr);
  ASSERT_TRUE(fact->value.has_value());
  EXPECT_EQ(*fact->value, 0x18u);
  // The routine's second caller loads $80 and gets $80 back, not the meet of
  // both callers.
  const HardwareAccess* second = accessAt(d, 0x008415u, "INIDISP");
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(second->value.has_value());
  EXPECT_EQ(*second->value, 0x80u);
  // And the three writes describe one transfer, to the register BBAD0 names.
  ASSERT_FALSE(d.dmas.empty());
  EXPECT_EQ(d.dmas.front().channel, 0u);
  EXPECT_EQ(d.dmas.front().destinationName, "VMDATAL");
}

TEST(Dataflow, ADirectPageOperandUnderAnUnknownDirectRegisterProducesNothing) {
  const CartridgeDisassembly& d = provingTree();
  for (const HardwareAccess& fact : d.accesses) {
    EXPECT_NE(fact.site, 0x00860Au) << "the interrupt handler proves nothing about D";
    EXPECT_NE(fact.site, 0x008500u) << "nothing is known after the call the analysis cannot follow";
    EXPECT_NE(fact.site, 0x0083B4u) << "nothing is known after a software interrupt";
    EXPECT_NE(fact.site, 0x008380u) << "the paths disagree on D";
  }
  // The handler's absolute store keeps the rule it had.
  const HardwareAccess* screen = accessAt(d, 0x008605u, "INIDISP");
  ASSERT_NE(screen, nullptr);
  ASSERT_TRUE(screen->value.has_value());
  EXPECT_EQ(*screen->value, 0x80u);
}

// ---- paths that meet ------------------------------------------------------------

TEST(Dataflow, TwoPathsThatAgreeProveTheValue) {
  const CartridgeDisassembly& d = provingTree();
  const StateFact* agree = stateAt(d, 0x008390u);
  ASSERT_NE(agree, nullptr);
  EXPECT_EQ(agree->d, std::vector<std::uint32_t>{0x0000u});
  EXPECT_EQ(agree->dbr, std::vector<std::uint32_t>{0x00u});
  EXPECT_EQ(agree->s, std::vector<std::uint32_t>{0x1FFFu});
}

TEST(Dataflow, TwoPathsThatDisagreeAreReportedNotChosen) {
  const CartridgeDisassembly& d = provingTree();
  const StateFact* meet = stateAt(d, 0x008380u);
  ASSERT_NE(meet, nullptr);
  EXPECT_EQ(meet->d, (std::vector<std::uint32_t>{0x0100u, 0x0200u}));
  const std::string manifest = disasm::renderManifest(d);
  EXPECT_NE(manifest.find("state    $00:8380 D=$0100|$0200 DBR=$00 S=$1FFF\n"), std::string::npos) << manifest;
  // Downstream the three values stay apart rather than becoming one.
  const StateFact* after = stateAt(d, 0x0083A0u);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->d, (std::vector<std::uint32_t>{0x0000u, 0x0100u, 0x0200u}));
}

// ---- the tables -------------------------------------------------------------------

TEST(Dataflow, AMaskBoundsTheIndexAndTheTableDerivesWhole) {
  const CartridgeDisassembly& d = provingTree();
  const std::vector<const disasm::DerivedTarget*> derived = derivedFrom(d, 0x008033u);
  ASSERT_EQ(derived.size(), 8u) << "AND #$07, ASL, TAX: eight slots";
  for (std::size_t i = 0; i < derived.size(); ++i) {
    EXPECT_EQ(derived[i]->pointer, 0x008200u + 2u * i);
    EXPECT_EQ(derived[i]->target, 0x008300u + 0x10u * i);
    EXPECT_FALSE(derived[i]->call);
    EXPECT_EQ(derived[i]->name, std::string("loc_0083") + "0123456789ABCDEF"[i] + "0");
  }
  EXPECT_FALSE(stopsAt(d, 0x008033u)) << "every destination is derived, so the jump is not a stop";
}

TEST(Dataflow, ACompareAndABranchOnTheCarryBoundTheIndex) {
  const CartridgeDisassembly& d = provingTree();
  const std::vector<const disasm::DerivedTarget*> derived = derivedFrom(d, 0x0083A9u);
  ASSERT_EQ(derived.size(), 3u) << "CMP #$03 / BCS: the values below three fall through";
  EXPECT_EQ(derived[0]->target, 0x008400u);
  EXPECT_EQ(derived[1]->target, 0x008410u);
  EXPECT_EQ(derived[2]->target, 0x008420u);
  EXPECT_FALSE(stopsAt(d, 0x0083A9u));
}

TEST(Dataflow, AnIndexNothingBoundsDerivesNothingAndTheStopStands) {
  const CartridgeDisassembly& d = provingTree();
  EXPECT_TRUE(derivedFrom(d, 0x0083B1u).empty());
  EXPECT_TRUE(stopsAt(d, 0x0083B1u));
  EXPECT_FALSE(labelAt(d, 0x008430u).has_value()) << "the third table's target is never reached";
}

TEST(Dataflow, DerivedTargetsAreTracedFromAndDeriveMore) {
  const CartridgeDisassembly& d = provingTree();
  // The second table is only reached through the first table's targets, so its
  // three destinations exist because the analysis and the trace took turns.
  EXPECT_TRUE(labelAt(d, 0x008300u).has_value());
  EXPECT_TRUE(labelAt(d, 0x0083A0u).has_value());
  EXPECT_TRUE(labelAt(d, 0x008400u).has_value());
  EXPECT_TRUE(labelAt(d, 0x008500u).has_value());
  EXPECT_EQ(d.derived.size(), 11u);
  EXPECT_EQ(d.stops.size(), 2u) << "the routine's jump through work RAM, and the unbounded table";
}

// ---- what is beyond the analysis -----------------------------------------------------

TEST(Dataflow, AfterACallToARoutineTheAnalysisCannotFollowNothingIsKnown) {
  const CartridgeDisassembly& d = provingTree();
  // Two paths reach the label: one through the unfollowable call, one straight
  // from a table target with everything known. What one path does not know, the
  // label does not know.
  EXPECT_EQ(stateAt(d, 0x008500u), nullptr) << "a label at which nothing is proven has no line";
  EXPECT_TRUE(stopsAt(d, 0x008120u));
}

TEST(Dataflow, ASoftwareInterruptLeavesNothingKnownAfterIt) {
  const CartridgeDisassembly& d = provingTree();
  EXPECT_EQ(stateAt(d, 0x0083B4u), nullptr) << "the handler is beyond the analysis";
  EXPECT_FALSE(stopsAt(d, 0x00837Bu)) << "a BRK continues at its vector's handler and is no stop";
}

TEST(Dataflow, AnInterruptVectorBeginsWithNothingProven) {
  const CartridgeDisassembly& d = provingTree();
  EXPECT_EQ(stateAt(d, 0x008600u), nullptr);
}

// ---- the manifest -----------------------------------------------------------------

TEST(Dataflow, TheManifestWritesDerivedAndStateLines) {
  const CartridgeDisassembly& d = provingTree();
  const std::string manifest = disasm::renderManifest(d);
  EXPECT_NE(manifest.find("derived  $00:8300 loc_008300 e=0 m=8 x=16 from $00:8033 via $00:8200\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("derived  $00:8420 loc_008420 e=0 m=8 x=16 from $00:83A9 via $00:8224\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("state    $00:8100 D=$0000|$0100|$0200|$4300 DBR=$00 S=$1FFD\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("state    $00:802A D=$0000 DBR=$7E S=$1FFF\n"), std::string::npos) << manifest;
  EXPECT_NE(manifest.find("state    $00:8000 D=$0000 DBR=$00 S=?\n"), std::string::npos) << manifest;
}

TEST(Dataflow, DerivedLinesReadBackAndAreTracedFrom) {
  const CartridgeDisassembly& d = provingTree();
  std::string error;
  const std::optional<disasm::ManifestInput> input = disasm::parseManifest(disasm::renderManifest(d), error);
  ASSERT_TRUE(input.has_value()) << error;
  EXPECT_EQ(input->derived.size(), 11u);
  EXPECT_EQ(input->derived.front().site, 0x008033u);
  EXPECT_EQ(input->derived.front().pointer, 0x008200u);

  // A derived line a manifest carries for a destination the bytes here cannot
  // derive — the third table's — is traced from as an entry, keeps its line, and
  // answers the stop.
  const std::optional<disasm::ManifestInput> written = disasm::parseManifest(
      "derived  $00:8430 loc_008430 e=0 m=8 x=16 from $00:83B1 via $00:8240\n", error);
  ASSERT_TRUE(written.has_value()) << error;
  CartridgeRequest request;
  request.derived = written->derived;
  const CartridgeDisassembly again = disassemble(proving(), request);
  EXPECT_TRUE(labelAt(again, 0x008430u).has_value());
  EXPECT_EQ(derivedFrom(again, 0x0083B1u).size(), 1u);
  EXPECT_FALSE(stopsAt(again, 0x0083B1u));
  EXPECT_EQ(again.derived.size(), 12u);
}

TEST(Dataflow, AReachedAndADerivedLineForOneDestinationBothStandUnderOneName) {
  CartridgeRequest request;
  request.reached.push_back(disasm::ReachedTarget{.target = 0x008300u,
                                                  .mode = Cpu65816Mode::native(true, false),
                                                  .site = 0x008033u,
                                                  .call = false,
                                                  .name = {}});
  const CartridgeDisassembly d = disassemble(proving(), request);
  ASSERT_EQ(d.reached.size(), 1u);
  EXPECT_EQ(d.reached.front().name, "loc_008300");
  const std::vector<const disasm::DerivedTarget*> derived = derivedFrom(d, 0x008033u);
  ASSERT_EQ(derived.size(), 8u);
  EXPECT_EQ(derived.front()->name, "loc_008300") << "a run saw it and the bytes prove it: two lines, one name";
  const std::string manifest = disasm::renderManifest(d);
  EXPECT_NE(manifest.find("reached  $00:8300 loc_008300"), std::string::npos);
  EXPECT_NE(manifest.find("derived  $00:8300 loc_008300"), std::string::npos);
}

TEST(Dataflow, AnUnknownKindIsStillAnErrorAndAMalformedDerivedLineNamesItself) {
  std::string error;
  EXPECT_FALSE(disasm::parseManifest("derived  $00:8430 loc_008430 e=0 m=8 x=16 from $00:83B1\n", error).has_value());
  EXPECT_NE(error.find("via"), std::string::npos);
  EXPECT_FALSE(disasm::parseManifest("proven $00:8000 D=$0000\n", error).has_value());
  EXPECT_NE(error.find("not a manifest line"), std::string::npos);
}

// ---- the rule before is inside the rule now -------------------------------------------

TEST(Dataflow, TheValueRuleBeforeIsASubsetOfTheValueRuleNow) {
  std::size_t compared = 0;
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    const CartridgeDisassembly d = disassemble(rom);
    const std::vector<HardwareAccess> before = disasm::hardwareAccesses(d);  // no proof: the rule before
    for (const HardwareAccess& old : before) {
      const HardwareAccess* now = accessAt(d, old.site, old.name);
      ASSERT_NE(now, nullptr) << example.name << ": a fact the rule before produced is gone";
      EXPECT_EQ(now->kind, old.kind);
      if (old.value) {
        ASSERT_TRUE(now->value.has_value()) << example.name;
        EXPECT_EQ(*now->value, *old.value) << example.name;
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 40u);
}

// ---- the effects over a state ------------------------------------------------------

TEST(Dataflow, AnEightBitLoadKeepsTheHighByteAndAnIndexLoadClearsIt) {
  State before;
  before.registers.aHigh = Values::one(0x12u);
  before.registers.xHigh = Values::one(0x34u);
  const Cpu65816Mode narrow = Cpu65816Mode::native(true, false);
  const Evaluation lda = evaluate(nodeOf({0xA9u, 0x56u}, 0x008000u, narrow), before, kNoImage);
  EXPECT_EQ(lda.after.registers.aLow, Values::one(0x56u));
  EXPECT_EQ(lda.after.registers.aHigh, Values::one(0x12u));
  const Evaluation ldx = evaluate(nodeOf({0xA2u, 0x78u}, 0x008000u, Cpu65816Mode::native(true, true)), before, kNoImage);
  EXPECT_EQ(ldx.after.registers.xLow, Values::one(0x78u));
  EXPECT_EQ(ldx.after.registers.xHigh, Values::one(0u));
}

// The chip holds the index high bytes at zero while the index registers are
// eight bits wide, whatever a path left there; the analysis re-establishes it
// before every node, and leaves the bytes alone under sixteen-bit indexes.
TEST(Dataflow, TheIndexHighBytesAreHeldZeroUnderEightBitIndexRegisters) {
  State before;  // X and Y not known at all
  const Evaluation narrow = evaluate(nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::native(true, true)), before, kNoImage);
  EXPECT_EQ(narrow.after.registers.xHigh, Values::one(0u));
  EXPECT_EQ(narrow.after.registers.yHigh, Values::one(0u));
  EXPECT_FALSE(narrow.after.registers.xLow.known);
  const Evaluation wide = evaluate(nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::native(true, false)), before, kNoImage);
  EXPECT_FALSE(wide.after.registers.xHigh.known);
}

TEST(Dataflow, AMaskOverAValueNotKnownBoundsItToTheMasksSubsets) {
  const Evaluation ev = evaluate(nodeOf({0x29u, 0x05u}, 0x008000u, Cpu65816Mode::native(true, true)), State{}, kNoImage);
  EXPECT_EQ(ev.after.registers.aLow, (Values{true, {0u, 1u, 4u, 5u}, std::nullopt, 0}));
  const Evaluation wide = evaluate(nodeOf({0x29u, 0xFFu, 0x03u}, 0x008000u, Cpu65816Mode::native(false, true)), State{}, kNoImage);
  EXPECT_FALSE(wide.after.registers.aLow.known) << "a ten-bit mask admits more values than are worth knowing";
}

// `ASL` then `AND #$01FF` over a value nothing knows admits the even values under
// the mask and no others — a table indexed by it has no odd slot.
TEST(Dataflow, AShiftBeforeAMaskKeepsTheBitItClearedOutOfTheBound) {
  const Cpu65816Mode wide = Cpu65816Mode::native(false, false);
  const Evaluation shifted = evaluate(nodeOf({0x0Au}, 0x008000u, wide), State{}, kNoImage);
  EXPECT_FALSE(shifted.after.registers.aLow.known);
  EXPECT_EQ(shifted.after.registers.aLow.zeroBits & 1u, 1u);
  const Evaluation masked = evaluate(nodeOf({0x29u, 0xFFu, 0x01u}, 0x008000u, wide), shifted.after, kNoImage);
  const Values a = masked.after.registers.a();
  ASSERT_TRUE(a.known);
  EXPECT_EQ(a.values.size(), 256u);
  for (const std::uint32_t v : a.values) EXPECT_EQ(v & 1u, 0u);
  EXPECT_EQ(a.values.back(), 0x1FEu);
}

TEST(Dataflow, AStoreCarriesTheAddressesAndValuesItCanMove) {
  State before = resetState();
  before.registers.d = Values::one(0x4300u);
  before.registers.aLow = Values::one(0x18u);
  before.registers.xLow = Values{true, {0u, 1u}, std::nullopt, 0};
  before.registers.xHigh = Values::one(0u);
  // STA $00,X under D = $4300 with X one of two values reaches two addresses.
  const Evaluation ev = evaluate(nodeOf({0x95u, 0x00u}, 0x008000u, Cpu65816Mode::native(true, true)), before, kNoImage);
  ASSERT_EQ(ev.accesses.size(), 1u);
  EXPECT_EQ(ev.accesses[0].op, Op::Store);
  EXPECT_EQ(ev.accesses[0].address, (Values{true, {0x4300u, 0x4301u}, std::nullopt, 0}));
  EXPECT_EQ(ev.accesses[0].value, Values::one(0x18u));
}

// `PEA $1234` pushes $12 then $34, so the first `PLB` pulls $34 and the second $12.
TEST(Dataflow, APullTakesTheLastBytePushedAndASixteenBitPushPutsTheLowByteOnTop) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  State state = resetState();
  state.registers.s = Values::one(0x1FFFu);
  state = evaluate(nodeOf({0xF4u, 0x34u, 0x12u}, 0x008000u, mode), state, kNoImage).after;
  EXPECT_EQ(state.registers.s, Values::one(0x1FFDu));
  ASSERT_EQ(state.pushed.size(), 2u);
  state = evaluate(nodeOf({0xABu}, 0x008003u, mode), state, kNoImage).after;
  EXPECT_EQ(state.registers.dbr, Values::one(0x34u)) << "the low byte was pushed last";
  state = evaluate(nodeOf({0xABu}, 0x008004u, mode), state, kNoImage).after;
  EXPECT_EQ(state.registers.dbr, Values::one(0x12u));
  EXPECT_EQ(state.registers.s, Values::one(0x1FFFu));
  EXPECT_TRUE(state.pushed.empty());
}

// A store to the byte under the stack pointer, or to an address that could be
// anywhere when the stack pointer is not known, leaves nothing a pull can trust.
TEST(Dataflow, AStoreTheStackCouldBeUnderForgetsWhatWasPushed) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  State known = resetState();
  known.registers.s = Values::one(0x1FFFu);
  known.registers.aLow = Values::one(0x55u);
  known = evaluate(nodeOf({0x48u}, 0x008000u, mode), known, kNoImage).after;  // PHA: $55 at $1FFF
  State aside = evaluate(nodeOf({0x8Du, 0x00u, 0x10u}, 0x008001u, mode), known, kNoImage).after;  // STA !$1000
  EXPECT_EQ(aside.pushed.size(), 1u) << "$1000 is not under the stack";
  State under = evaluate(nodeOf({0x8Du, 0xFFu, 0x1Fu}, 0x008001u, mode), known, kNoImage).after;  // STA !$1FFF
  EXPECT_TRUE(under.pushed.empty()) << "$1FFF is the byte just pushed";
  under = evaluate(nodeOf({0x68u}, 0x008004u, mode), under, kNoImage).after;  // PLA
  EXPECT_FALSE(under.registers.aLow.known);

  State unknownStack;
  unknownStack.registers.aLow = Values::one(0x55u);
  unknownStack = evaluate(nodeOf({0x48u}, 0x008000u, mode), unknownStack, kNoImage).after;
  unknownStack = evaluate(nodeOf({0x8Du, 0x00u, 0x10u}, 0x008001u, mode), unknownStack, kNoImage).after;
  EXPECT_TRUE(unknownStack.pushed.empty()) << "with S not known, any store could reach the stack";
}

TEST(Dataflow, AnArithmeticResultWithTheCarryInItIsNotKnown) {
  State before;
  before.registers.aLow = Values::one(0x10u);
  before.registers.aHigh = Values::one(0u);
  const Evaluation adc = evaluate(nodeOf({0x69u, 0x01u}, 0x008000u, Cpu65816Mode::native(true, true)), before, kNoImage);
  EXPECT_FALSE(adc.after.registers.aLow.known) << "the carry is not followed";
  const Evaluation inc = evaluate(nodeOf({0x1Au}, 0x008000u, Cpu65816Mode::native(true, true)), before, kNoImage);
  EXPECT_EQ(inc.after.registers.aLow, Values::one(0x11u)) << "an increment has no carry in";
}

}  // namespace snaggletooth::ir
