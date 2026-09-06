// The shadow, rule by rule: what an origin is, how it travels through every
// operation the interpreter runs, how it rests in work RAM, and what it never
// touches.
//
// Every case here drives the interpreter over a node lifted from real bytes
// with the shadow attached, through a bus that answers reads from a flat
// memory laid over a LoROM map, and reads the shadow back. The cases are chosen
// so each rule stated in `ir/ir_provenance.h` is the thing that would break: a
// load taking the bytes' origin and never the address's, an operation taking
// the union, a flag carrying nothing, a narrow write following the register's
// own rule, the cap widening a set to its hull and saying so, a read inside a
// run changing nothing and runs that touch being one, a helper's runs reaching
// a source through the caller that read on from them, a byte loaded from work
// RAM carried out as the buffer it came from, a stream's source being the run
// its invocation read, and a run with the shadow computing exactly what a run
// without one does.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_interpret.h"
#include "ir/ir_provenance.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::ir {
namespace {

using disasm::Cpu65816Mode;

constexpr std::size_t kImageBytes = 0x8000u;  // one LoROM bank: $00:8000-$00:FFFF is offset 0-$7FFF
constexpr std::size_t kCap = 4;

// A flat memory the interpreter reads through, answering every address.
struct FlatBus final : Bus {
  std::map<Address, std::uint8_t> mem;
  std::uint8_t read(Address address, Access) override {
    const auto it = mem.find(address & 0xFFFFFFu);
    return it == mem.end() ? std::uint8_t{0} : it->second;
  }
  void write(Address address, std::uint8_t value, Access) override { mem[address & 0xFFFFFFu] = value; }
};

// What the shadow told a host about the bytes the CPU carried out of work RAM:
// every call, in order.
struct Carries final : CarrySink {
  struct Byte {
    std::uint32_t registerAddress;
    Address memory;
    bool continues;
  };
  std::vector<Byte> carried;
  std::vector<std::pair<std::uint32_t, bool>> ended;  // the register, and whether the sequence was recorded
  void carriedByte(std::uint32_t registerAddress, Address memory, bool continues) override {
    carried.push_back(Byte{.registerAddress = registerAddress, .memory = memory, .continues = continues});
  }
  void carryEnded(std::uint32_t registerAddress, bool recorded) override {
    ended.emplace_back(registerAddress, recorded);
  }
};

// One program: the interpreter, its shadow and its memory, with instructions
// placed at $00:8000 onward and run one after another.
struct Machine {
  Provenance shadow{CartridgeMap::LoRom, kImageBytes, kCap};
  Interpreter interpreter;
  FlatBus bus;
  Carries carries;
  Address expectedNext = 0x008000u;  // where falling through the last instruction leads

  Machine() {
    interpreter.shadow = &shadow;
    shadow.carries = &carries;
    interpreter.registers.pc = 0x8000u;
    interpreter.registers.e = false;
    interpreter.registers.p = 0x30u;  // native, A8, X8
    interpreter.registers.s = 0x01FFu;
  }

  // Places `bytes` at the next address and runs them as one instruction under
  // the interpreter's live mode, with the shadow told the site.
  void run(std::vector<std::uint8_t> bytes) {
    const Registers& r = interpreter.registers;
    const Address at = (static_cast<Address>(r.pbr) << 16) | r.pc;
    for (std::size_t i = 0; i < bytes.size(); ++i) bus.mem[at + static_cast<Address>(i)] = bytes[i];
    const Cpu65816Mode mode =
        r.e ? Cpu65816Mode::reset() : Cpu65816Mode::native((r.p & 0x20u) != 0, (r.p & 0x10u) != 0);
    const std::optional<disasm::Instruction> decoded = disasm::decodeAt(bytes, at, at, mode);
    ASSERT_TRUE(decoded.has_value()) << "the bytes do not decode";
    const Node node = liftInstruction(*decoded, mode);
    shadow.site = at;
    if (at != expectedNext) shadow.flowBroke();
    expectedNext = at + node.instruction.length;
    interpreter.execute(node, bus);
    if (node.instruction.flow == Flow::Call) shadow.called();
    if (node.instruction.flow == Flow::Return) shadow.returned();
  }

  // The bytes at an image address, as data the program reads.
  void image(Address address, std::vector<std::uint8_t> bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) bus.mem[address + static_cast<Address>(i)] = bytes[i];
  }

  const OriginSet& originAt(Address address) {
    const std::optional<Origin> origin = shadow.originOf(address);
    static const OriginSet none;
    return origin ? shadow.origins().of(*origin) : none;
  }
};

OriginSet imageSet(std::initializer_list<OriginInterval> intervals) {
  OriginSet set;
  set.image = intervals;
  return set;
}

// The offset of a LoROM bank-zero address.
std::size_t offsetOf(Address address) { return address & 0x7FFFu; }

}  // namespace

// ---- origins as values ---------------------------------------------------------

TEST(Provenance, AnImageByteIsItsOwnOriginAndIsInternedOnce) {
  Origins origins(kCap);
  const Origin a = origins.image(0x1234u);
  const Origin b = origins.image(0x1234u);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, kNoOrigin);
  EXPECT_EQ(origins.of(a), imageSet({{0x1234u, 0x1234u}}));
  EXPECT_EQ(origins.interned(), 2u);  // nothing, and the one byte
}

TEST(Provenance, AUnionJoinsTouchingBytesIntoOneIntervalAndKeepsAGap) {
  Origins origins(kCap);
  const Origin ab = origins.unite(origins.image(10), origins.image(11));
  EXPECT_EQ(origins.of(ab), imageSet({{10, 11}}));
  const Origin abd = origins.unite(ab, origins.image(13));
  EXPECT_EQ(origins.of(abd), imageSet({{10, 11}, {13, 13}}));
  // The same union asked twice is the same index, in either order.
  EXPECT_EQ(origins.unite(origins.image(13), ab), abd);
  EXPECT_FALSE(origins.of(abd).approximate);
}

TEST(Provenance, NothingIsTheIdentityOfAUnion) {
  Origins origins(kCap);
  const Origin a = origins.image(5);
  EXPECT_EQ(origins.unite(a, kNoOrigin), a);
  EXPECT_EQ(origins.unite(kNoOrigin, a), a);
  EXPECT_EQ(origins.unite(kNoOrigin, kNoOrigin), kNoOrigin);
}

TEST(Provenance, ARegisterAndTheSaveAreMarksTheUnionKeeps) {
  Origins origins(kCap);
  const Origin joypad = origins.hardwareRegister(0x4218u);
  const Origin save = origins.save();
  const Origin all = origins.unite(origins.unite(joypad, save), origins.image(7));
  const OriginSet& set = origins.of(all);
  EXPECT_EQ(set.image, (std::vector<OriginInterval>{{7, 7}}));
  EXPECT_EQ(set.registers, (std::vector<std::uint32_t>{0x4218u}));
  EXPECT_TRUE(set.save);
  EXPECT_FALSE(set.empty());
  EXPECT_TRUE(origins.of(kNoOrigin).empty());
}

TEST(Provenance, AboveTheCapAUnionIsItsHullAndSaysSo) {
  Origins origins(kCap);
  Origin comb = kNoOrigin;
  for (std::size_t offset = 0; offset < 2 * kCap; offset += 2) comb = origins.unite(comb, origins.image(offset));
  // Four singletons two apart are at the cap, and exact.
  EXPECT_EQ(origins.of(comb).image.size(), kCap);
  EXPECT_FALSE(origins.of(comb).approximate);
  // The fifth widens.
  comb = origins.unite(comb, origins.image(2 * kCap));
  const OriginSet& set = origins.of(comb);
  ASSERT_EQ(set.image.size(), 1u);
  EXPECT_EQ(set.image.front(), (OriginInterval{0, 2 * kCap}));
  EXPECT_TRUE(set.approximate);
  EXPECT_EQ(set.imageBytes(), 2 * kCap + 1);
  // Approximate stays approximate through every union after.
  EXPECT_TRUE(origins.of(origins.unite(comb, origins.image(100))).approximate);
}

TEST(Provenance, AccumulatingOutsideTheTableHasNoCap) {
  Origins origins(kCap);
  OriginSet acc;
  for (std::size_t offset = 0; offset < 4 * kCap; offset += 2) origins.accumulate(acc, origins.image(offset));
  EXPECT_EQ(acc.image.size(), 2 * kCap);
  EXPECT_FALSE(acc.approximate);
  OriginSet other = imageSet({{1, 1}});
  Origins::merge(acc, other);
  EXPECT_EQ(acc.image.front(), (OriginInterval{0, 2}));
}

// ---- the rules, through the interpreter --------------------------------------

TEST(Provenance, ALoadTakesTheOriginOfTheBytesNeverOfTheAddress) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u});
  // The index comes from the image too; the loaded value's origin is the byte's alone.
  m.run({0xAEu, 0x01u, 0x90u});  // LDX !$9001 -> X = $22, origin $1001
  m.run({0xBDu, 0x00u, 0x90u});  // LDA !$9000,X -> the byte at $9022
  m.image(0x009022u, {0x33u});
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{offsetOf(0x9022u), offsetOf(0x9022u)}}));
}

TEST(Provenance, AnOperationTakesTheUnionOfItsOperands) {
  Machine m;
  m.image(0x009000u, {0x01u, 0x02u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x18u});                // CLC
  m.run({0x6Du, 0x01u, 0x90u});  // ADC !$9001
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1000u, 0x1001u}}));
  EXPECT_EQ(m.interpreter.registers.a & 0xFFu, 0x03u);
}

TEST(Provenance, AConstantAndAFlagCarryNothing) {
  Machine m;
  m.run({0xA9u, 0x5Au});         // LDA #$5A
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_TRUE(m.originAt(0x7E0200u).empty());
  ASSERT_TRUE(m.shadow.writerOf(0x7E0200u).has_value());
  // A compare sets flags from an image byte; a value computed from the flags
  // alone — a rotate of a constant — carries nothing.
  m.image(0x009000u, {0x80u});
  m.run({0xCDu, 0x00u, 0x90u});  // CMP !$9000
  m.run({0xA9u, 0x01u});         // LDA #$01
  m.run({0x2Au});                // ROL A
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201
  EXPECT_TRUE(m.originAt(0x7E0201u).empty());
}

TEST(Provenance, TheStatusRegisterCarriesNothingThroughAPullAndAPush) {
  Machine m;
  m.image(0x009000u, {0x31u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000: an origin
  m.run({0x48u});                // PHA: the byte on the stack carries it
  m.run({0x28u});                // PLP: into the status register, which carries nothing
  m.run({0x08u});                // PHP: back onto the stack, carrying nothing
  m.run({0x68u});                // PLA
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_TRUE(m.originAt(0x7E0200u).empty());
  EXPECT_EQ(m.interpreter.registers.p, 0x31u);
}

TEST(Provenance, ANarrowWriteFollowsTheRegistersOwnRule) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  m.run({0xC2u, 0x20u});         // REP #$20: A16
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000: A = $2211, origins $1000 and $1001
  m.run({0xE2u, 0x20u});         // SEP #$20: A8
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002: the low byte alone
  m.run({0xEBu});                // XBA: the high byte, still from $1001, comes down
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1001u, 0x1001u}}));
  // An index register written narrow clears its high byte, origin and all.
  m.run({0xC2u, 0x10u});         // REP #$10: X16
  m.run({0xAEu, 0x00u, 0x90u});  // LDX !$9000
  m.run({0xE2u, 0x10u});         // SEP #$10: X8
  m.run({0xAEu, 0x02u, 0x90u});  // LDX !$9002
  m.run({0xC2u, 0x10u});         // REP #$10
  m.run({0x8Eu, 0x02u, 0x02u});  // STX !$0202: two bytes, the high one zero from nowhere
  EXPECT_EQ(m.originAt(0x7E0202u), imageSet({{0x1002u, 0x1002u}}));
  EXPECT_TRUE(m.originAt(0x7E0203u).empty());
}

TEST(Provenance, AStoreLandsInTheShadowWithItsSite) {
  Machine m;
  m.image(0x009000u, {0x11u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x34u, 0x12u});  // STA !$1234, at $00:8003
  const std::optional<Writer> writer = m.shadow.writerOf(0x7E1234u);
  ASSERT_TRUE(writer.has_value());
  EXPECT_EQ(writer->site, 0x008003u);
  EXPECT_FALSE(writer->engine);
  // Through the mirror, the same byte.
  EXPECT_EQ(m.originAt(0x001234u), m.originAt(0x7E1234u));
  // A byte nothing wrote has no writer and no origin.
  EXPECT_FALSE(m.shadow.writerOf(0x7E1235u).has_value());
  EXPECT_TRUE(m.originAt(0x7E1235u).empty());
  // Outside work RAM there is no shadow at all.
  EXPECT_FALSE(m.shadow.originOf(0x009000u).has_value());
}

TEST(Provenance, TheStackCarriesOriginsThroughAPushAndAPull) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u});
  m.run({0xC2u, 0x20u});         // REP #$20
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x48u});                // PHA
  m.run({0xA9u, 0x00u, 0x00u});  // LDA #$0000
  m.run({0x68u});                // PLA
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1000u, 0x1000u}}));
  EXPECT_EQ(m.originAt(0x7E0201u), imageSet({{0x1001u, 0x1001u}}));
}

TEST(Provenance, AReadModifyWriteKeepsTheBytesOrigin) {
  Machine m;
  m.image(0x009000u, {0x11u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.run({0xEEu, 0x00u, 0x02u});  // INC !$0200
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1000u, 0x1000u}}));
  EXPECT_EQ(m.bus.mem[0x000200u], 0x12u);  // the bus saw bank zero; the shadow saw work RAM
}

TEST(Provenance, AHardwareRegisterAndTheSaveAreMarksAValueCarries) {
  Machine m;
  m.run({0xADu, 0x18u, 0x42u});  // LDA !$4218: JOY1L
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.originAt(0x7E0200u).registers, (std::vector<std::uint32_t>{0x4218u}));
  m.run({0xAFu, 0x00u, 0x00u, 0x70u});  // LDA $70:0000: the save under LoROM
  m.run({0x8Du, 0x01u, 0x02u});         // STA !$0201
  EXPECT_TRUE(m.originAt(0x7E0201u).save);
  // Open bus carries nothing.
  m.run({0xAFu, 0x00u, 0x50u, 0x00u});  // LDA $00:5000
  m.run({0x8Du, 0x02u, 0x02u});         // STA !$0202
  EXPECT_TRUE(m.originAt(0x7E0202u).empty());
}

TEST(Provenance, ThePortIsPairedWithWhereItReached) {
  Machine m;
  m.image(0x009000u, {0x11u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.shadow.portWrites.assign({0x7E0500u});
  m.run({0x8Du, 0x80u, 0x21u});  // STA !$2180: the port, reaching $7E:0500
  EXPECT_EQ(m.originAt(0x7E0500u), imageSet({{0x1000u, 0x1000u}}));
  EXPECT_TRUE(m.shadow.portWrites.empty());
  m.shadow.portReads.assign({0x7E0500u});
  m.run({0xADu, 0x80u, 0x21u});  // LDA !$2180: what the port reached
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1000u, 0x1000u}}));
}

TEST(Provenance, AnEngineWriteLandsUnderItsTrigger) {
  Machine m;
  m.shadow.written(0x7F0000u, m.shadow.origins().image(0x2000u), Writer{.site = 0x008123u, .engine = true});
  EXPECT_EQ(m.originAt(0x7F0000u), imageSet({{0x2000u, 0x2000u}}));
  const std::optional<Writer> writer = m.shadow.writerOf(0x7F0000u);
  ASSERT_TRUE(writer.has_value());
  EXPECT_TRUE(writer->engine);
  EXPECT_EQ(writer->site, 0x008123u);
  // Anywhere but work RAM, nothing changes.
  m.shadow.written(0x009000u, m.shadow.origins().image(1), Writer{});
  EXPECT_FALSE(m.shadow.originOf(0x009000u).has_value());
  // An engine's byte belongs to no invocation: its source is its origin alone,
  // whatever the running code has read around it.
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
  for (std::uint8_t i = 0; i < 4; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9000 .. LDA !$9003
  m.shadow.written(0x7F0001u, m.shadow.origins().image(0x1001u), Writer{.site = 0x008123u, .engine = true});
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0001u), (std::vector<OriginInterval>{{0x1001u, 0x1001u}}));
}

TEST(Provenance, AHelpersReadsJoinItsCallersWhenItReturns) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: a helper that reads the stream
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003
  m.run({0x60u});                // RTS
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001, back in the caller
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  // The caller's invocation read the run through its helper: the source is
  // the four bytes, not the one.
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1003u}}));
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1001u, 0x1001u}}));
}

// ---- the runs an invocation reads --------------------------------------------

TEST(Provenance, AReadThatTouchesARunExtendsItWhateverCameBetween) {
  // Two files read a chunk at a time, turn and turn about: each stays one run.
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  m.image(0x009100u, {0x44u, 0x55u, 0x66u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0xADu, 0x00u, 0x91u});  // LDA !$9100
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0xADu, 0x01u, 0x91u});  // LDA !$9101
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200: from $9002
  m.run({0xADu, 0x02u, 0x91u});  // LDA !$9102
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201: from $9102
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1002u}}));
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0201u), (std::vector<OriginInterval>{{0x1100u, 0x1102u}}));
}

TEST(Provenance, AReadInsideARunChangesNothing) {
  // A decoder that peeks — reads the byte it is at twice — and one that starts
  // its stream over: the same run either way.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u, 0x19u});
  for (std::uint8_t i = 0; i < 6; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9000 .. !$9005
  m.run({0xADu, 0x05u, 0x90u});                                    // LDA !$9005: the peek
  for (std::uint8_t i = 6; i < 10; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9006 .. !$9009
  m.run({0x8Du, 0x00u, 0x02u});                                    // STA !$0200: from $9009
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1009u}}));
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003: from the middle again
  m.run({0xADu, 0x04u, 0x90u});  // LDA !$9004
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201: from $9004
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0201u), (std::vector<OriginInterval>{{0x1000u, 0x1009u}}));
}

TEST(Provenance, AHelpersRunOverTwoOfTheCallersRunsMakesOneRun) {
  // The caller has read two pieces with a gap between; a helper reads across
  // both and returns; the caller then reads inside the gap. One run holds it
  // all — never two overlapping, of which a lookup would take the narrower.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u});
  for (std::uint8_t i = 0; i < 3; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9000 .. !$9002
  for (std::uint8_t i = 6; i < 9; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9006 .. !$9008
  m.run({0x20u, 0x00u, 0x81u});                                   // JSR !$8100
  for (std::uint8_t i = 1; i < 8; ++i) m.run({0xADu, i, 0x90u});  // $8100 LDA !$9001 .. !$9007
  m.run({0x60u});                                                 // RTS
  m.run({0xADu, 0x04u, 0x90u});                                   // LDA !$9004
  m.run({0x8Du, 0x00u, 0x02u});                                   // STA !$0200
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1008u}}));
}

TEST(Provenance, AReadBelowARunsFirstByteExtendsItDownward) {
  // A copy that walks its source from the end: one run, read backwards.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u});
  for (std::uint8_t i = 6; i-- > 0;) m.run({0xADu, i, 0x90u});  // LDA !$9005 .. !$9000
  m.run({0x8Du, 0x00u, 0x02u});                                  // STA !$0200: from $9000
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1005u}}));
}

TEST(Provenance, ARunStrictlyInsideACallersRunIsTheSource) {
  // The caller read the block whole earlier; a helper reads two bytes inside
  // it and writes them out. The caller's run neither begins nor ends where
  // the helper's does — it had the bytes already — so the helper's run is the
  // source.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u});
  for (std::uint8_t i = 0; i < 6; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9000 .. !$9005
  m.run({0x20u, 0x00u, 0x81u});                                    // JSR !$8100
  m.run({0xADu, 0x02u, 0x90u});                                    // $8100 LDA !$9002
  m.run({0x8Du, 0x00u, 0x02u});                                    // STA !$0200
  m.run({0xADu, 0x03u, 0x90u});                                    // LDA !$9003
  m.run({0x8Du, 0x01u, 0x02u});                                    // STA !$0201
  m.run({0x60u});                                                  // RTS
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1002u, 0x1003u}}));
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0201u), (std::vector<OriginInterval>{{0x1002u, 0x1003u}}));
}

TEST(Provenance, TheChainRunsThroughACallerThatWroteNothing) {
  // A loader calls a decoder once per chunk and writes nothing itself; the
  // chunks are one run in the loader, and the loader has returned by the
  // time the bytes are carried out. The loader's run is still the source.
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: the loader
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200: the decoder, chunk one
  m.run({0xADu, 0x00u, 0x90u});  // $8200 LDA !$9000
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201
  m.run({0x60u});                // RTS
  m.run({0x20u, 0x00u, 0x82u});  // JSR !$8200: chunk two
  m.run({0xADu, 0x02u, 0x90u});  // $8200 LDA !$9002
  m.run({0x8Du, 0x02u, 0x02u});  // STA !$0202
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003
  m.run({0x8Du, 0x03u, 0x02u});  // STA !$0203
  m.run({0x60u});                // RTS
  m.run({0x60u});                // RTS: the loader returns, having written nothing
  m.run({0xADu, 0x00u, 0x91u});  // LDA !$9100: the caller reads elsewhere
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1003u}}));
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0203u), (std::vector<OriginInterval>{{0x1000u, 0x1003u}}));
}

TEST(Provenance, RunsThatComeToTouchAreOneRun) {
  // A table's entries read in whatever order the code asks for them: the
  // entries lie end to end, and the runs that grow to meet are one run.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u});
  m.run({0xADu, 0x04u, 0x90u});  // LDA !$9004: the third entry first
  m.run({0xADu, 0x05u, 0x90u});  // LDA !$9005
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000: then the first
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002: then the second, which meets both
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200: from $9003
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1005u}}));
}

TEST(Provenance, AHelpersRunsJoinItsCallersByTheSameRule) {
  // The caller reads one file, a helper reads another and returns, the caller
  // goes on with its own: still two runs.
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u});
  m.image(0x009100u, {0x33u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0xADu, 0x00u, 0x91u});  // $8100 LDA !$9100
  m.run({0x60u});                // RTS
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200: from $9001
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1001u}}));
}

TEST(Provenance, AnInvocationsBytesReachTheSourceThroughTheCallerItsRunsJoined) {
  // A decoder called once per chunk: each call reads its chunk and writes work
  // RAM; the chunks are one run in the caller, and that run is the source.
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: the first chunk
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201
  m.run({0x60u});                // RTS
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: the second chunk
  m.run({0xADu, 0x02u, 0x90u});  // $8100 LDA !$9002
  m.run({0x8Du, 0x02u, 0x02u});  // STA !$0202
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003
  m.run({0x8Du, 0x03u, 0x02u});  // STA !$0203
  m.run({0x60u});                // RTS
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1003u}}));
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0203u), (std::vector<OriginInterval>{{0x1000u, 0x1003u}}));
}

TEST(Provenance, ForgettingThePlacesLeavesWorkRamAlone) {
  Machine m;
  m.image(0x009000u, {0x11u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.shadow.forgetPlaces();
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201: the accumulator's origin is gone
  EXPECT_TRUE(m.originAt(0x7E0201u).empty());
  EXPECT_EQ(m.originAt(0x7E0200u), imageSet({{0x1000u, 0x1000u}}));
}

// ---- streams --------------------------------------------------------------------

TEST(Provenance, ConsecutiveStoresFromConsecutiveBytesAreOneStream) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
  m.run({0xC2u, 0x20u});         // REP #$20
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x18u, 0x21u});  // STA !$2118: two bytes, at $00:8005
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002
  m.run({0x8Du, 0x18u, 0x21u});  // STA !$2118: two more, the instruction after
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  const Stream& stream = m.shadow.streams().front();
  EXPECT_EQ(stream.site, 0x008005u);
  EXPECT_EQ(stream.registerAddress, 0x2118u);
  EXPECT_EQ(stream.first, 0x1000u);
  EXPECT_EQ(stream.bytes, 4u);
  EXPECT_EQ(stream.times, 1u);
}

TEST(Provenance, AStoreThatIsNotTheNextByteEndsTheStream) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: the next byte
  m.run({0xA9u, 0x00u});         // LDA #$00
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: from nowhere, the stream ends
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: one byte alone is not a stream
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().bytes, 2u);
}

TEST(Provenance, AStoreFromAByteElsewhereStartsANewStream) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x05u, 0x90u});  // LDA !$9005: one image byte, not the next
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: a new sequence
  m.run({0xADu, 0x06u, 0x90u});  // LDA !$9006
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: continues it
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().first, 0x1005u);
  EXPECT_EQ(m.shadow.streams().front().bytes, 2u);
}

TEST(Provenance, AStoreFromASiteOffTheStraightRunEndsTheStream) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000, at $8000
  m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104, at $8003
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001, at $8006
  m.interpreter.registers.pc = 0x8100u;  // a jump the flow made elsewhere
  m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104, at $8100: not on a straight run from $8003
  m.shadow.finish();
  EXPECT_TRUE(m.shadow.streams().empty());
}

TEST(Provenance, TheSameStreamSeenAgainIsCountedNotRepeated) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u});
  for (int pass = 0; pass < 3; ++pass) {
    m.interpreter.registers.pc = 0x8000u;
    m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
    m.run({0x8Du, 0x40u, 0x21u});  // STA !$2140
    m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
    m.run({0x8Du, 0x41u, 0x21u});  // STA !$2141: the pair's second register continues it
    m.run({0xA9u, 0x00u});         // LDA #$00
    m.run({0x8Du, 0x40u, 0x21u});  // STA !$2140: ends it
  }
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().registerAddress, 0x2140u);
  EXPECT_EQ(m.shadow.streams().front().times, 3u);
}

TEST(Provenance, AStreamsSourceIsTheRunItsInvocationRead) {
  // Four bytes read, the middle two carried: the source is the four.
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  const Stream& stream = m.shadow.streams().front();
  EXPECT_EQ(stream.first, 0x1001u);
  EXPECT_EQ(stream.bytes, 2u);
  EXPECT_EQ(stream.source, (OriginInterval{0x1000u, 0x1003u}));
  EXPECT_FALSE(stream.memory.has_value());
}

TEST(Provenance, AStreamCarriedByAHelperFindsItsRunAfterTheHelperReturned) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002: read, not carried
  m.run({0x60u});                // RTS: the helper wrote no work RAM
  m.run({0xA9u, 0x00u});         // LDA #$00
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: ends the stream, in the caller
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().source, (OriginInterval{0x1000u, 0x1002u}));
}

// ---- what the CPU carries out of work RAM ---------------------------------------

TEST(Provenance, AStoreFromAByteLoadedFromWorkRamIsCarriedAsTheBufferItCameFrom) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  // A buffer built from consecutive image bytes, which an image stream would
  // also have claimed: the buffer wins.
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201
  m.run({0xADu, 0x00u, 0x02u});  // LDA !$0200
  m.run({0x8Du, 0x18u, 0x21u});  // STA !$2118
  m.run({0xADu, 0x01u, 0x02u});  // LDA !$0201
  m.run({0x8Du, 0x18u, 0x21u});  // STA !$2118
  m.shadow.finish();
  ASSERT_EQ(m.carries.carried.size(), 2u);
  EXPECT_EQ(m.carries.carried[0].registerAddress, 0x2118u);
  EXPECT_EQ(m.carries.carried[0].memory, 0x7E0200u);
  EXPECT_FALSE(m.carries.carried[0].continues);
  EXPECT_EQ(m.carries.carried[1].memory, 0x7E0201u);
  EXPECT_TRUE(m.carries.carried[1].continues);
  ASSERT_EQ(m.carries.ended.size(), 1u);
  EXPECT_EQ(m.carries.ended.front(), std::make_pair(0x2118u, true));
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  const Stream& stream = m.shadow.streams().front();
  ASSERT_TRUE(stream.memory.has_value());
  EXPECT_EQ(*stream.memory, 0x7E0200u);
  EXPECT_EQ(stream.bytes, 2u);
  EXPECT_EQ(stream.site, 0x00800Fu);
}

TEST(Provenance, ACarryContinuesOnlyFromTheNextAddressOnAStraightRun) {
  Machine m;
  m.run({0xADu, 0x00u, 0x02u});  // LDA !$0200
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x05u, 0x02u});  // LDA !$0205: not the next byte
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: a new sequence, the first alone
  m.run({0xADu, 0x06u, 0x02u});  // LDA !$0206
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: continues it
  m.run({0xADu, 0x07u, 0x02u});  // LDA !$0207
  m.interpreter.registers.pc = 0x8100u;  // a jump the flow made elsewhere
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: off the straight run, a new sequence
  m.shadow.finish();
  ASSERT_EQ(m.carries.ended.size(), 3u);
  EXPECT_FALSE(m.carries.ended[0].second);  // one byte is not a sequence
  EXPECT_TRUE(m.carries.ended[1].second);   // $0205, $0206
  EXPECT_FALSE(m.carries.ended[2].second);  // $0207 alone
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(*m.shadow.streams().front().memory, 0x7E0205u);
  EXPECT_EQ(m.shadow.streams().front().bytes, 2u);
}

TEST(Provenance, ACarriedValueIsTheByteLoadedNotOneComputedFromIt) {
  Machine m;
  m.run({0xADu, 0x00u, 0x02u});  // LDA !$0200
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: carried
  m.run({0xADu, 0x01u, 0x02u});  // LDA !$0201
  m.run({0x1Au});                // INC A: not the byte at $0201 now
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: ends the sequence, carries nothing
  m.shadow.finish();
  ASSERT_EQ(m.carries.carried.size(), 1u);
  EXPECT_TRUE(m.shadow.streams().empty());
}

TEST(Provenance, AWordLoadedFromWorkRamCarriesBothBytesInOrder) {
  Machine m;
  m.run({0xC2u, 0x20u});         // REP #$20
  m.run({0xADu, 0x00u, 0x02u});  // LDA !$0200: two bytes
  m.run({0x8Du, 0x18u, 0x21u});  // STA !$2118: the low to VMDATAL, the high to VMDATAH
  m.run({0xADu, 0x02u, 0x02u});  // LDA !$0202
  m.run({0x8Du, 0x18u, 0x21u});  // STA !$2118
  m.shadow.finish();
  ASSERT_EQ(m.carries.carried.size(), 4u);
  EXPECT_EQ(m.carries.carried[1].memory, 0x7E0201u);
  EXPECT_EQ(m.carries.carried[3].memory, 0x7E0203u);
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().bytes, 4u);
}

TEST(Provenance, TheExchangeCarriesTheAddressesWithTheBytes) {
  Machine m;
  m.run({0xC2u, 0x20u});         // REP #$20
  m.run({0xADu, 0x00u, 0x02u});  // LDA !$0200: the low byte from $0200, the high from $0201
  m.run({0xEBu});                // XBA
  m.run({0xE2u, 0x20u});         // SEP #$20
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122: the byte that was at $0201
  m.shadow.finish();
  ASSERT_EQ(m.carries.carried.size(), 1u);
  EXPECT_EQ(m.carries.carried.front().memory, 0x7E0201u);
}

TEST(Provenance, AByteReadThroughThePortIsCarriedAsTheByteThePortReached) {
  Machine m;
  m.shadow.portReads.push_back(0x7E0300u);
  m.run({0xADu, 0x80u, 0x21u});  // LDA !$2180: the port, reaching $7E:0300
  m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104
  m.shadow.portReads.push_back(0x7E0301u);
  m.run({0xADu, 0x80u, 0x21u});  // LDA !$2180: the next byte
  m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(*m.shadow.streams().front().memory, 0x7E0300u);
  EXPECT_EQ(m.shadow.streams().front().bytes, 2u);
}

TEST(Provenance, ACarryAndAnImageStreamNeverContinueEachOther) {
  // A buffer carried from index 2 ends at index 4; an image byte at offset 4
  // stored next is not its next byte, whatever the numbers say.
  Machine m;
  m.run({0xADu, 0x02u, 0x00u});  // LDA !$0002: work RAM
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x03u, 0x00u});  // LDA !$0003
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x04u, 0x80u});  // LDA !$8004: the image, offset 4
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().bytes, 2u);
  ASSERT_EQ(m.carries.ended.size(), 1u);
  EXPECT_TRUE(m.carries.ended.front().second);
}

TEST(Provenance, TheSameCarrySeenAgainIsCountedNotRepeated) {
  Machine m;
  for (int pass = 0; pass < 2; ++pass) {
    m.interpreter.registers.pc = 0x8000u;
    m.run({0xADu, 0x00u, 0x02u});  // LDA !$0200
    m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104
    m.run({0xADu, 0x01u, 0x02u});  // LDA !$0201
    m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104
    m.run({0xA9u, 0x00u});         // LDA #$00
    m.run({0x8Du, 0x04u, 0x21u});  // STA !$2104: ends it
  }
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().times, 2u);
  EXPECT_EQ(m.carries.carried.size(), 4u);  // every byte of every sighting is told
}

// ---- beside the interpreter, never in it ---------------------------------------

TEST(Provenance, TheShadowChangesNoValue) {
  // The same program through an interpreter with the shadow and one without:
  // every register and every byte of memory agree.
  Machine with;
  Machine without;
  without.interpreter.shadow = nullptr;
  for (Machine* m : {&with, &without}) {
    m->image(0x009000u, {0x11u, 0x22u, 0x33u, 0x44u});
    m->run({0xC2u, 0x30u});         // REP #$30
    m->run({0xADu, 0x00u, 0x90u});  // LDA !$9000
    m->run({0x18u});                // CLC
    m->run({0x6Du, 0x02u, 0x90u});  // ADC !$9002
    m->run({0x48u});                // PHA
    m->run({0x68u});                // PLA
    m->run({0x8Du, 0x00u, 0x02u});  // STA !$0200
    m->run({0xEEu, 0x00u, 0x02u});  // INC !$0200
    m->run({0xEBu});                // XBA
    m->run({0x8Du, 0x18u, 0x21u});  // STA !$2118
  }
  EXPECT_EQ(with.interpreter.registers, without.interpreter.registers);
  EXPECT_EQ(with.bus.mem, without.bus.mem);
  EXPECT_EQ(with.originAt(0x7E0200u), imageSet({{0x1000u, 0x1003u}}));
}

TEST(Provenance, AnInterpreterStartsWithNoShadow) {
  Interpreter interpreter;
  EXPECT_EQ(interpreter.shadow, nullptr);
}

}  // namespace snaggletooth::ir
