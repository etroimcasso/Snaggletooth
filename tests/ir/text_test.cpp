// The program file: written from a program and read back to the same program.
//
// A case per construct the grammar has — every operation, place, width, step,
// access kind and condition an effect can carry; every addressing mode, flow,
// target, register name and mode a node can carry; the interrupt sequences; a
// data run, a label, a warning, two regions, the image line, the version — each
// rendered, parsed and held equal on every field. Then what a reader refuses,
// each naming its line; that writing what was read gives the same bytes; and
// that every example cartridge's program survives the round trip whole.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_text.h"
#include "rom/rom_disasm.h"

namespace snaggletooth::ir {
namespace {

using disasm::CartridgeDisassembly;
using disasm::CartridgeRequest;
using disasm::Cpu65816Mode;
using disasm::Line;
using disasm::RegionListing;

// One instruction placed at `address`, decoded under `mode` and lifted.
Node nodeOf(std::vector<std::uint8_t> bytes, Address address, const Cpu65816Mode& mode,
            bool patched = false) {
  const std::optional<disasm::Instruction> decoded =
      disasm::decodeAt(bytes, address, address, mode);
  EXPECT_TRUE(decoded.has_value());
  return liftInstruction(*decoded, mode, patched);
}

// A file with one region covering bank zero's upper half, no labels and no data.
ProgramFile bankZero() {
  ProgramFile file;
  file.imageBytes = 32768;
  file.map = "LoROM";
  file.regions.push_back({.file = "bank_00.asm",
                          .first = 0x008000u,
                          .last = 0x00FFFFu,
                          .warnings = {},
                          .labels = {},
                          .data = {}});
  return file;
}

Program programOf(std::vector<Node> nodes) {
  Program program;
  program.nodes = std::move(nodes);
  program.nmi = interruptSequence(Interrupt::Nmi);
  program.irq = interruptSequence(Interrupt::Irq);
  return program;
}

// Written, read back, and held equal; then written again to the same bytes.
void roundTrip(const Program& program, const ProgramFile& file, const char* what) {
  const std::string text = renderProgram(program, file);
  std::string error;
  const std::optional<Parsed> parsed = parseProgram(text, error);
  ASSERT_TRUE(parsed.has_value()) << what << ": " << error;
  EXPECT_TRUE(equivalent(parsed->program, program)) << what;
  EXPECT_EQ(parsed->file, file) << what;
  EXPECT_EQ(renderProgram(parsed->program, parsed->file), text) << what;
}

// The error a text is refused with, or an empty string when it is read.
std::string refusal(const std::string& text) {
  std::string error;
  const std::optional<Parsed> parsed = parseProgram(text, error);
  return parsed ? std::string() : error;
}

Effect effect(Op op, Operand dst, Operand a, Operand b, Width width) {
  Effect e;
  e.op = op;
  e.dst = dst;
  e.a = a;
  e.b = b;
  e.width = width;
  return e;
}

Operand at(Place place) { return Operand{place, 0}; }
Operand imm(std::uint32_t value) { return Operand{Place::Imm, value}; }

// Every region of a disassembly lifted with its image — both readings of a
// two-way address — the nodes of all of them in address order.
Program programOfTree(const CartridgeDisassembly& d) {
  Program all;
  for (const RegionListing& region : d.regions) {
    std::vector<std::uint8_t> image;
    for (const Line& line : region.listing.lines) {
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

// What a disassembly's tree carries that its program does not.
ProgramFile fileOfTree(const CartridgeDisassembly& d) {
  ProgramFile file;
  file.imageBytes = d.imageBytes;
  file.map = d.header.map == CartridgeMap::LoRom    ? "LoROM"
             : d.header.map == CartridgeMap::HiRom  ? "HiROM"
                                                    : "ExHiROM";
  for (const RegionListing& region : d.regions) {
    ProgramRegion out;
    out.file = region.region.file;
    out.first = region.region.first;
    out.last = region.region.last;
    out.warnings = region.listing.warnings;
    for (const auto& [address, name] : region.listing.labels) out.labels.push_back({address, name});
    for (const Line& line : region.listing.lines) {
      if (!line.isCode && !line.data.empty()) out.data.push_back({line.address, line.data});
    }
    file.regions.push_back(std::move(out));
  }
  return file;
}

CartridgeDisassembly disassemble(std::span<const std::uint8_t> rom) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  return disasm::disassembleCartridge(request);
}

// ---- the head of the file --------------------------------------------------------

TEST(Text, TheFileOpensWithTheVersionAndTheImage) {
  const std::string text = renderProgram(programOf({}), bankZero());
  EXPECT_TRUE(text.starts_with("snagir 1\nimage 32768 LoROM\n\nregion bank_00.asm $00:8000-$00:FFFF\n"))
      << text;
  roundTrip(programOf({}), bankZero(), "an empty program");
}

TEST(Text, TheImageLineCarriesTheSizeAndTheMap) {
  ProgramFile file = bankZero();
  file.imageBytes = 4u * 1024u * 1024u;
  file.map = "HiROM";
  file.regions[0] = {.file = "bank_C0.asm", .first = 0xC00000u, .last = 0xC0FFFFu,
                     .warnings = {}, .labels = {}, .data = {}};
  roundTrip(programOf({}), file, "a HiROM image");
}

// ---- the effect layer, construct by construct -----------------------------------------

TEST(Text, EveryOperationRoundTrips) {
  std::vector<Effect> effects;
  for (std::size_t i = 0; i <= static_cast<std::size_t>(Op::Cycles); ++i) {
    const Op op = static_cast<Op>(i);
    Effect e = effect(op, at(Place::T0), at(Place::A), imm(0x1234u), Width::Word);
    if (op == Op::Load || op == Op::Store || op == Op::StoreRmw) e.step = Step::Bank;
    if (op == Op::Push || op == Op::Pull) e.pinned = true;
    effects.push_back(e);
  }
  Node node = nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::reset());
  node.effects = effects;
  roundTrip(programOf({node}), bankZero(), "every operation");
  EXPECT_EQ(effects.size(), 37u);
}

TEST(Text, EveryPlaceRoundTripsAndAFlagIsWrittenQualified) {
  std::vector<Effect> effects;
  for (std::size_t i = static_cast<std::size_t>(Place::A); i <= static_cast<std::size_t>(Place::FlagC); ++i) {
    effects.push_back(effect(Op::Set, at(static_cast<Place>(i)), at(static_cast<Place>(i)), {}, Width::Byte));
  }
  Node node = nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::reset());
  node.effects = effects;
  roundTrip(programOf({node}), bankZero(), "every place");

  EXPECT_EQ(renderEffect(effect(Op::Set, at(Place::X), imm(2u), {}, Width::Word)), "Set X <- $2  [16]");
  EXPECT_EQ(renderEffect(effect(Op::Set, at(Place::FlagX), imm(1u), {}, Width::Byte)), "Set P.X <- $1  [8]");
  EXPECT_EQ(renderEffect(effect(Op::Set, at(Place::D), imm(0u), {}, Width::Word)), "Set D <- $0  [16]");
  EXPECT_EQ(renderEffect(effect(Op::Set, at(Place::FlagD), imm(0u), {}, Width::Byte)), "Set P.D <- $0  [8]");
  EXPECT_EQ(placeName(Place::FlagX), "X");  // the vocabulary's own name is not qualified
}

TEST(Text, EveryWidthStepAccessAndPinRoundTrips) {
  std::vector<Effect> effects;
  for (std::size_t w = 0; w <= static_cast<std::size_t>(Width::ByX); ++w) {
    for (std::size_t s = 0; s <= static_cast<std::size_t>(Step::DirectPointer); ++s) {
      for (std::size_t a = 0; a <= static_cast<std::size_t>(Access::Vector); ++a) {
        Effect e = effect(Op::Load, at(Place::T1), at(Place::T0), {}, static_cast<Width>(w));
        e.step = static_cast<Step>(s);
        e.access = static_cast<Access>(a);
        effects.push_back(e);
        Effect store = effect(Op::StoreRmw, {}, at(Place::T0), at(Place::A), static_cast<Width>(w));
        store.step = static_cast<Step>(s);
        store.access = static_cast<Access>(a);
        effects.push_back(store);
      }
    }
  }
  Effect pinned = effect(Op::Push, {}, at(Place::PC), {}, Width::Word);
  pinned.pinned = true;
  Effect unpinned = effect(Op::Pull, at(Place::A), {}, {}, Width::ByM);
  effects.push_back(pinned);
  effects.push_back(unpinned);
  Node node = nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::reset());
  node.effects = effects;
  roundTrip(programOf({node}), bankZero(), "every width, step, access and pin");
  EXPECT_EQ(renderEffect(pinned), "Push PC  [16 pinned]");
  EXPECT_EQ(renderEffect(unpinned), "Pull A <-  [byM unpinned]");
  EXPECT_EQ(renderEffect(effects[3]), "StoreRmw T0, A  [8 flat rmw]");
}

TEST(Text, EveryConditionRoundTrips) {
  std::vector<Effect> effects;
  for (std::size_t c = 0; c <= static_cast<std::size_t>(When::IndexCrossed); ++c) {
    for (const bool andE : {false, true}) {
      if (c == 0 && andE) continue;  // written `if e`; the case below
      Effect e = effect(Op::Cycles, {}, imm(1u), {}, Width::Byte);
      e.when.when = static_cast<When>(c);
      e.when.andEmulation = andE;
      if (e.when.when == When::FlagSet || e.when.when == When::FlagClear) e.when.place = Place::FlagZ;
      if (e.when.when == When::PlaceIs || e.when.when == When::PlaceIsNot) {
        e.when.place = Place::DBR;
        e.when.value = 0x7Eu;
      }
      effects.push_back(e);
    }
  }
  Node node = nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::reset());
  node.effects = effects;
  roundTrip(programOf({node}), bankZero(), "every condition");
  EXPECT_EQ(renderEffect(effects[6]), "Cycles $1  [8]  if set P.Z and e");
  EXPECT_EQ(renderEffect(effects[11]), "Cycles $1  [8]  if is not DBR $7E");
  EXPECT_EQ(renderEffect(effects[13]), "Cycles $1  [8]  if D.lo");
}

// An effect that always runs, and the emulation flag is set, is `if e`: the
// interpreter reads the two alike, and the file writes the one.
TEST(Text, AlwaysAndEmulationIsWrittenAsIfEmulation) {
  Effect always = effect(Op::Cycles, {}, imm(1u), {}, Width::Byte);
  always.when.andEmulation = true;
  EXPECT_EQ(renderEffect(always), "Cycles $1  [8]  if e");
  Node node = nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::reset());
  node.effects = {always};
  std::string error;
  const std::optional<Parsed> parsed = parseProgram(renderProgram(programOf({node}), bankZero()), error);
  ASSERT_TRUE(parsed.has_value()) << error;
  Cond expected;
  expected.when = When::Emulation;
  EXPECT_EQ(parsed->program.nodes[0].effects[0].when, expected);
}

// ---- the instruction layer, construct by construct ---------------------------------

TEST(Text, EveryAddressingModeRoundTrips) {
  const Cpu65816Mode native = Cpu65816Mode::native(false, false);
  const std::vector<std::vector<std::uint8_t>> forms = {
      {0xEAu},                       // NOP, implied
      {0x0Au},                       // ASL A
      {0xA9u, 0x34u, 0x12u},         // LDA #imm(M)
      {0xA2u, 0x34u, 0x12u},         // LDX #imm(X)
      {0xC2u, 0x30u},                // REP #byte
      {0xA5u, 0x10u},                // LDA dp
      {0xB5u, 0x10u},                // LDA dp,X
      {0xB6u, 0x10u},                // LDX dp,Y
      {0xB2u, 0x10u},                // LDA (dp)
      {0xA1u, 0x10u},                // LDA (dp,X)
      {0xB1u, 0x10u},                // LDA (dp),Y
      {0xA7u, 0x10u},                // LDA [dp]
      {0xB7u, 0x10u},                // LDA [dp],Y
      {0xA3u, 0x03u},                // LDA sr,S
      {0xB3u, 0x03u},                // LDA (sr,S),Y
      {0xADu, 0x00u, 0x21u},         // LDA abs
      {0xBDu, 0x00u, 0x21u},         // LDA abs,X
      {0xB9u, 0x00u, 0x21u},         // LDA abs,Y
      {0xAFu, 0x00u, 0x21u, 0x00u},  // LDA long
      {0xBFu, 0x00u, 0x21u, 0x00u},  // LDA long,X
      {0x6Cu, 0x00u, 0x80u},         // JMP (abs)
      {0xDCu, 0x00u, 0x80u},         // JML [abs]
      {0x7Cu, 0x00u, 0x80u},         // JMP (abs,X)
      {0xD0u, 0x10u},                // BNE rel
      {0x82u, 0x10u, 0x00u},         // BRL rel16
      {0x54u, 0x7Eu, 0x00u},         // MVN src,dst
      {0xF4u, 0x34u, 0x12u},         // PEA #abs
      {0x62u, 0x10u, 0x00u},         // PER rel16
  };
  std::vector<Node> nodes;
  Address address = 0x008000u;
  std::vector<Addressing> seen;
  for (const std::vector<std::uint8_t>& bytes : forms) {
    Node node = nodeOf(bytes, address, native);
    seen.push_back(node.instruction.addressing);
    nodes.push_back(node);
    address += static_cast<Address>(bytes.size());
  }
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(Addressing::PushRelative) + 1u);
  for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], static_cast<Addressing>(i)) << i;
  roundTrip(programOf(nodes), bankZero(), "every addressing mode");

  // A block move carries both banks; the two `rel16` forms are told apart by
  // their mnemonics.
  const std::string text = renderProgram(programOf(nodes), bankZero());
  EXPECT_NE(text.find("MVN src,dst  operand $0 operand2 $7E  length 3"), std::string::npos) << text;
  EXPECT_NE(text.find("$00:803A  BRL rel16  operand $804D  length 3  flow jump target $00:804D"), std::string::npos)
      << text;
  EXPECT_NE(text.find("$00:8043  PER rel16  operand $8056  length 3  flow continue  e=0"), std::string::npos)
      << text;
}

TEST(Text, EveryFlowAndATargetRoundTrip) {
  const Cpu65816Mode native = Cpu65816Mode::native(true, true);
  const std::vector<Node> nodes = {
      nodeOf({0xEAu}, 0x008000u, native),                // continue
      nodeOf({0xD0u, 0x02u}, 0x008001u, native),         // branch, target $8005
      nodeOf({0x4Cu, 0x40u, 0x80u}, 0x008003u, native),  // jump, target
      nodeOf({0x20u, 0x40u, 0x80u}, 0x008006u, native),  // call, target
      nodeOf({0x60u}, 0x008009u, native),                // return
      nodeOf({0xDBu}, 0x00800Au, native),                // halt
      nodeOf({0x6Cu, 0x00u, 0x80u}, 0x00800Bu, native),  // jump with no target the bytes name
  };
  const std::vector<Flow> flows = {Flow::Continue, Flow::Branch, Flow::Jump, Flow::Call,
                                   Flow::Return, Flow::Halt, Flow::Jump};
  for (std::size_t i = 0; i < nodes.size(); ++i) EXPECT_EQ(nodes[i].instruction.flow, flows[i]) << i;
  EXPECT_EQ(nodes[1].instruction.target, 0x008005u);
  EXPECT_FALSE(nodes[6].instruction.target.has_value());
  roundTrip(programOf(nodes), bankZero(), "every flow");
  const std::string text = renderProgram(programOf(nodes), bankZero());
  EXPECT_NE(text.find("BNE rel  operand $8005  length 2  flow branch target $00:8005  e=0"), std::string::npos)
      << text;
  EXPECT_NE(text.find("JMP (abs)  operand $8000  length 3  flow jump  e=0"), std::string::npos) << text;
}

TEST(Text, ARegisterNameAndThePatchedMarkRoundTrip) {
  const Cpu65816Mode native = Cpu65816Mode::native(true, true);
  const Node named = nodeOf({0x8Fu, 0x00u, 0x21u, 0x00u}, 0x008000u, native);  // STA $00:2100
  const Node patched = nodeOf({0xEAu}, 0x008004u, native, true);
  EXPECT_EQ(named.registerName, "INIDISP");
  roundTrip(programOf({named, patched}), bankZero(), "a register name and a patched node");
  const std::string text = renderProgram(programOf({named, patched}), bankZero());
  EXPECT_NE(text.find("$00:8000  STA long  operand $2100  length 4  flow continue  e=0 m=8 x=8  base 5/5/6/6  INIDISP\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("  e=0 m=8 x=8  base 2/2/2/2  patched\n"), std::string::npos) << text;

  // The name read back is the register table's own storage.
  std::string error;
  const std::optional<Parsed> parsed = parseProgram(text, error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->program.nodes[0].registerName.data(), disasm::cpu65816RegisterName(0x002100u).data());
  EXPECT_EQ(parsed->program.nodes[0].instruction.mnemonic.data(), named.instruction.mnemonic.data());
}

TEST(Text, EveryModeRoundTrips) {
  std::vector<Node> nodes;
  Address address = 0x008000u;
  const std::vector<Cpu65816Mode> modes = {
      Cpu65816Mode::reset(),
      Cpu65816Mode::native(true, true),
      Cpu65816Mode::native(true, false),
      Cpu65816Mode::native(false, true),
      Cpu65816Mode::native(false, false),
      Cpu65816Mode::nativeUnknown(),
  };
  for (const Cpu65816Mode& mode : modes) nodes.push_back(nodeOf({0xEAu}, address++, mode));
  Cpu65816Mode half = Cpu65816Mode::native(true, true);
  half.indexKnown = false;
  nodes.push_back(nodeOf({0xEAu}, address++, half));
  roundTrip(programOf(nodes), bankZero(), "every mode");
  const std::string text = renderProgram(programOf(nodes), bankZero());
  EXPECT_NE(text.find("  e=1  base"), std::string::npos);
  EXPECT_NE(text.find("  e=0 m=16 x=8  base"), std::string::npos);
  EXPECT_NE(text.find("  e=0 m=? x=?  base"), std::string::npos);
  EXPECT_NE(text.find("  e=0 m=8 x=?  base"), std::string::npos);
}

// The file writes `?` for a width the trace did not know, and the bit behind
// it is not in the file: a program read back is equivalent to the one written,
// and `==` sees the bit.
TEST(Text, AWidthNotKnownIsNotAValueTheFileCarries) {
  Mode a;
  a.emulation = false;
  a.accumulatorKnown = false;
  a.accumulator8 = true;
  Mode b = a;
  b.accumulator8 = false;
  EXPECT_TRUE(equivalent(a, b));
  EXPECT_NE(a, b);
  b.accumulatorKnown = true;
  EXPECT_FALSE(equivalent(a, b));
  a.accumulatorKnown = true;
  EXPECT_FALSE(equivalent(a, b));
  a.accumulator8 = false;
  EXPECT_TRUE(equivalent(a, b));
  EXPECT_EQ(a, b);

  // Every other field is held by `equivalent` as `==` holds it.
  const Cpu65816Mode native = Cpu65816Mode::native(true, true);
  const Node node = nodeOf({0xA9u, 0x01u}, 0x008000u, native);
  Node other = node;
  EXPECT_TRUE(equivalent(node, other));
  other.patched = true;
  EXPECT_FALSE(equivalent(node, other));
  other = node;
  other.effects.pop_back();
  EXPECT_FALSE(equivalent(node, other));
  other = node;
  other.cost.base[2] = 9;
  EXPECT_FALSE(equivalent(node, other));
  other = node;
  other.instruction.operand = 2;
  EXPECT_FALSE(equivalent(node, other));
  Program one = programOf({node});
  Program two = programOf({node, node});
  EXPECT_FALSE(equivalent(one, two));
  two = one;
  two.irq.pop_back();
  EXPECT_FALSE(equivalent(one, two));

  // A node the trace read under an unknown width comes back with the bit the
  // trace's own contexts carry; the parsed program is equivalent, not equal.
  const Node unknown = nodeOf({0xEAu}, 0x008000u, Cpu65816Mode::nativeUnknown());
  EXPECT_TRUE(unknown.mode.accumulator8);
  std::string error;
  const std::optional<Parsed> parsed = parseProgram(renderProgram(programOf({unknown}), bankZero()), error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_FALSE(parsed->program.nodes[0].mode.accumulator8);
  EXPECT_TRUE(equivalent(parsed->program, programOf({unknown})));
  EXPECT_NE(parsed->program, programOf({unknown}));
}

TEST(Text, TheCostsAreWrittenInCostIndexOrder) {
  Node node = nodeOf({0xA9u, 0x01u}, 0x008000u, Cpu65816Mode::native(true, true));  // LDA #imm
  node.cost.base = {2, 3, 4, 5};
  roundTrip(programOf({node}), bankZero(), "the costs");
  EXPECT_NE(renderNode(node).find("  base 2/3/4/5\n"), std::string::npos) << renderNode(node);
}

TEST(Text, TheInterruptSequencesRoundTrip) {
  const Program program = programOf({});
  EXPECT_FALSE(program.nmi.empty());
  EXPECT_FALSE(program.irq.empty());
  EXPECT_NE(program.nmi, program.irq);
  roundTrip(program, bankZero(), "the sequences");
  const std::string text = renderProgram(program, bankZero());
  EXPECT_NE(text.find("\nnmi\n    "), std::string::npos) << text;
  EXPECT_NE(text.find("\nirq\n    "), std::string::npos) << text;
}

// ---- what the file carries that the program does not ---------------------------------

TEST(Text, ADataRunALabelAWarningAndTwoRegionsRoundTrip) {
  ProgramFile file;
  file.imageBytes = 65536;
  file.map = "LoROM";
  file.regions.push_back({.file = "bank_00.asm",
                          .first = 0x008000u,
                          .last = 0x00FFFFu,
                          .warnings = {"$00:8010 is reached with e=0 m=8 x=8 and with e=0 m=16 x=8"},
                          .labels = {{0x008000u, "reset"}, {0x008003u, "loc_008003"}},
                          .data = {{0x008004u, {0x00u, 0xFFu, 0x7Eu}}}});
  file.regions.push_back({.file = "bank_01.asm",
                          .first = 0x018000u,
                          .last = 0x01FFFFu,
                          .warnings = {},
                          .labels = {{0x018000u, "sub_018000"}},
                          .data = {{0x018001u, {0x60u}}}});
  const Cpu65816Mode native = Cpu65816Mode::native(true, true);
  const std::vector<Node> nodes = {
      nodeOf({0x4Cu, 0x03u, 0x80u}, 0x008000u, native),
      nodeOf({0xEAu}, 0x008003u, native),
      nodeOf({0x60u}, 0x018000u, native),
  };
  roundTrip(programOf(nodes), file, "two regions");
  const std::string text = renderProgram(programOf(nodes), file);
  EXPECT_NE(text.find("\nregion bank_00.asm $00:8000-$00:FFFF\n"
                      "warning bank_00.asm $00:8010 is reached with e=0 m=8 x=8 and with e=0 m=16 x=8\n"
                      "\nlabel $00:8000 reset\n$00:8000  JMP abs"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("\nlabel $00:8003 loc_008003\n$00:8003  NOP   operand"), std::string::npos) << text;
  EXPECT_NE(text.find("\ndata $00:8004 00FF7E\n\nregion bank_01.asm"), std::string::npos) << text;
  EXPECT_NE(text.find("\nlabel $01:8000 sub_018000\n$01:8000  RTS"), std::string::npos) << text;
  EXPECT_NE(text.find("\ndata $01:8001 60\n\nnmi\n"), std::string::npos) << text;
}

TEST(Text, ATwoWayAddressIsTwoNodesInOrder) {
  const Node first = nodeOf({0xA9u, 0x01u}, 0x008000u, Cpu65816Mode::native(true, true));
  const Node second = nodeOf({0xA9u, 0x01u, 0x00u}, 0x008000u, Cpu65816Mode::native(false, true));
  EXPECT_NE(first, second);
  roundTrip(programOf({first, second}), bankZero(), "a two-way address");
  const std::string text = renderProgram(programOf({first, second}), bankZero());
  const std::size_t one = text.find("$00:8000  LDA #imm(M)  operand $1  length 2");
  const std::size_t two = text.find("$00:8000  LDA #imm(M)  operand $1  length 3");
  ASSERT_NE(one, std::string::npos) << text;
  ASSERT_NE(two, std::string::npos) << text;
  EXPECT_LT(one, two);
}

TEST(Text, TheNodesComeBackInAddressOrderWhateverOrderTheRegionsCameIn) {
  ProgramFile file = bankZero();
  file.regions.insert(file.regions.begin(), {.file = "bank_01.asm",
                                             .first = 0x018000u,
                                             .last = 0x01FFFFu,
                                             .warnings = {},
                                             .labels = {},
                                             .data = {}});
  const Cpu65816Mode native = Cpu65816Mode::native(true, true);
  const Program program = programOf({nodeOf({0xEAu}, 0x008000u, native), nodeOf({0x60u}, 0x018000u, native)});
  const std::string text = renderProgram(program, file);
  EXPECT_LT(text.find("region bank_01.asm"), text.find("region bank_00.asm"));
  EXPECT_LT(text.find("$01:8000  RTS"), text.find("$00:8000  NOP"));
  roundTrip(program, file, "regions out of address order");
}

TEST(Text, ANodeNoRegionHoldsIsRefusedByTheWriter) {
  const Program program = programOf({nodeOf({0xEAu}, 0x018000u, Cpu65816Mode::native(true, true))});
  EXPECT_THROW(static_cast<void>(renderProgram(program, bankZero())), std::invalid_argument);
}

// ---- what a reader refuses, each naming its line ----------------------------------------

TEST(Text, AnUnknownVersionIsRefused) {
  EXPECT_EQ(refusal("snagir 2\nimage 1 LoROM\nnmi\nirq\n"), "line 1: version 2 is not one this reader knows");
  EXPECT_EQ(refusal("image 1 LoROM\n"), "line 1: the first line is not `snagir 1`");
  EXPECT_EQ(refusal(""), "line 1: the first line is not `snagir 1`");
}

TEST(Text, AnUnknownRecordIsRefusedWithItsLine) {
  const std::string text = renderProgram(programOf({}), bankZero());
  const std::size_t lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  EXPECT_EQ(refusal(text + "routine $00:8000 reset\n"),
            "line " + std::to_string(lines + 1) + ": `routine` is not a record");
  EXPECT_EQ(refusal("snagir 1\nimage 1 LoROM\n\nfact one\n"), "line 4: `fact` is not a record");
}

TEST(Text, ATruncatedNodeLineIsRefusedWithItsLine) {
  const std::string head = "snagir 1\nimage 32768 LoROM\n\nregion bank_00.asm $00:8000-$00:FFFF\n\n";
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1\n"), "line 6: the node lacks its flow");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  flow continue  e=1  base 2/2/2/2\n"),
            "line 6: `flow` where `length` belongs");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1  flow continue  e=1  base 2/2/2\n"),
            "line 6: `2/2/2` is not four costs");
  EXPECT_EQ(refusal(head + "$00:8000  NOQ   operand $0  length 1  flow continue  e=1  base 2/2/2/2\n"),
            "line 6: `NOQ` is not a mnemonic");
  EXPECT_EQ(refusal(head + "$00:8000  LDA (abs)  operand $0  length 3  flow continue  e=1  base 2/2/2/2\n"),
            "line 6: `LDA` with `(abs)` names no opcode");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1  flow continue  e=1  base 2/2/2/2  INIDISP\n"),
            "line 6: `INIDISP` is not the register at $0");
  EXPECT_EQ(refusal(head + "$00:8000  STA long  operand $2100  length 4  flow continue  e=1  base 5/5/5/5  NMITIMEN\n"),
            "line 6: `NMITIMEN` is not the register at $2100");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1  flow continue  e=1  base 2/2/2/2\n"
                           "    Sett PC <- $8001  [16]\n"),
            "line 7: `Sett` is not an operation");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1  flow continue  e=1  base 2/2/2/2\n"
                           "    Set imm <- $8001  [16]\n"),
            "line 7: `imm` is not a place");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1  flow continue  e=1  base 2/2/2/2\n"
                           "    Set PC <- $8001  [16 flat]\n"),
            "line 7: a step on an operation that carries none");
  EXPECT_EQ(refusal(head + "$00:8000  NOP   operand $0  length 1  flow continue  e=1  base 2/2/2/2\n"
                           "    Set PC <- $8001  [16]  if maybe\n"),
            "line 7: `if maybe` is not a condition");
}

TEST(Text, ARecordOutOfItsPlaceIsRefused) {
  EXPECT_EQ(refusal("snagir 1\nimage 1 LoROM\n\nlabel $00:8000 reset\n"), "line 4: a label before any region");
  EXPECT_EQ(refusal("snagir 1\nimage 1 LoROM\n\n    Set PC <- $1  [16]\n"),
            "line 4: an effect with no node or sequence above it");
  const std::string head = "snagir 1\nimage 32768 LoROM\n\nregion bank_00.asm $00:8000-$00:FFFF\n";
  EXPECT_EQ(refusal(head + "data $01:8000 00\n"), "line 5: $01:8000 lies outside bank_00.asm's range");
  EXPECT_EQ(refusal(head + "data $00:8004 00\nlabel $00:8000 reset\n"), "line 6: $00:8000 is out of address order");
  EXPECT_EQ(refusal(head + "nmi\nirq\nnmi\n"), "line 7: `nmi` is written once, after the last region");
  EXPECT_EQ(refusal(head + "nmi\n"), "line 6: the file ends before its irq sequence");
  EXPECT_EQ(refusal(head + "nmi\nirq\nlabel $00:8000 reset\n"), "line 7: a label after the interrupt sequences");
}

// ---- the round trip over the example cartridges ----------------------------------------

TEST(Text, WritingWhatWasReadGivesTheSameBytes) {
  const CartridgeDisassembly d = disassemble(examples::mixedImage());
  const std::string text = renderProgram(programOfTree(d), fileOfTree(d));
  std::string error;
  const std::optional<Parsed> parsed = parseProgram(text, error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(renderProgram(parsed->program, parsed->file), text);
  EXPECT_GT(text.size(), 2000u);
}

TEST(Text, EveryExampleCartridgeSurvivesTheRoundTrip) {
  std::size_t nodes = 0;
  for (const examples::Example& example : examples::examples()) {
    const CartridgeDisassembly d = disassemble(example.build());
    const Program program = programOfTree(d);
    const ProgramFile file = fileOfTree(d);
    roundTrip(program, file, example.name.data());
    nodes += program.nodes.size();
    // Every byte of a region is in a node's instruction or in a data run — the
    // first reading's, where an address is read two ways.
    for (const ProgramRegion& region : file.regions) {
      std::size_t bytes = 0;
      for (const DataRun& run : region.data) bytes += run.bytes.size();
      std::optional<Address> previous;
      for (const Node& node : program.nodes) {
        const Address at = node.instruction.address;
        if (at < region.first || at > region.last) continue;
        if (previous != at) bytes += node.instruction.length;
        previous = at;
      }
      EXPECT_EQ(bytes, static_cast<std::size_t>(region.last - region.first + 1u))
          << example.name << " " << region.file;
    }
  }
  EXPECT_GT(nodes, 500u);
}

}  // namespace
}  // namespace snaggletooth::ir
