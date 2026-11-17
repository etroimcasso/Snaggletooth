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
// a source through the caller that read on from them, an invocation living
// until the stack pointer passes where its return address was pushed and a
// transfer into the pointer passing nothing, a byte loaded from work
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
// A LoROM board with a save and no chip, the board every case below runs on
// unless it says otherwise.
constexpr CartridgeBoard kBoard{.map = CartridgeMap::LoRom, .coprocessor = Coprocessor::None, .saveRamBytes = 8192u};

struct Machine {
  Provenance shadow;
  Interpreter interpreter;
  FlatBus bus;
  Carries carries;
  Address expectedNext = 0x008000u;  // where falling through the last instruction leads

  explicit Machine(const CartridgeBoard& board = kBoard) : shadow{board, kImageBytes, kCap} {
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
    const std::uint16_t stackBefore = r.s;
    const bool emulationBefore = r.e;
    interpreter.execute(node, bus);
    const bool page = emulationBefore && r.e;
    if (node.instruction.flow == Flow::Call) {
      shadow.called(stackBefore, r.s, page);
    } else if (!loadsStackPointer(node.instruction)) {
      shadow.stackMoved(stackBefore, r.s, page);
    }
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

// The board decides what a LoROM save window and a cartridge bank's lower half
// hold: with no save the window is the image through the upper half, so a read
// there carries the image byte's origin; on a coprocessor's board the half is the
// chip's, and a read there carries nothing, as open bus does.
TEST(Provenance, ABareWindowReadIsTheImageAndAChipsHalfIsNothing) {
  Machine bare{CartridgeBoard{.map = CartridgeMap::LoRom, .coprocessor = Coprocessor::None, .saveRamBytes = 0u}};
  bare.run({0xAFu, 0x34u, 0x12u, 0x70u});  // LDA $70:1234: the image at $70:9234, offset $1234 of a one-bank image
  bare.run({0x8Du, 0x00u, 0x02u});         // STA !$0200
  EXPECT_EQ(bare.originAt(0x7E0200u), imageSet({{0x1234u, 0x1234u}}));

  Machine chip{CartridgeBoard{.map = CartridgeMap::LoRom, .coprocessor = Coprocessor::Dsp, .saveRamBytes = 0u}};
  chip.run({0xAFu, 0x34u, 0x12u, 0x60u});  // LDA $60:1234: the chip's half
  chip.run({0x8Du, 0x00u, 0x02u});         // STA !$0200
  EXPECT_TRUE(chip.originAt(0x7E0200u).empty());
  chip.run({0xAFu, 0x34u, 0x12u, 0x70u});  // LDA $70:1234: the chip's too, with no save
  chip.run({0x8Du, 0x01u, 0x02u});         // STA !$0201
  EXPECT_TRUE(chip.originAt(0x7E0201u).empty());
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

// ---- the sources of a hull -----------------------------------------------------

// Five bytes sixteen apart, summed: one interval more than the cap holds, so
// the sum's origin is their hull, $1000-$1040, and says it is approximate.
TEST(Provenance, AHullsSourcesAreTheRunsItOverlaps) {
  Machine m;
  for (std::uint8_t i = 0; i < 5; ++i) m.image(0x009000u + 0x10u * i, {static_cast<std::uint8_t>(i + 1u)});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  for (std::uint8_t i = 1; i < 5; ++i) m.run({0x6Du, static_cast<std::uint8_t>(0x10u * i), 0x90u});  // ADC !$9010 .. !$9040
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  const OriginSet origin = m.originAt(0x7E0200u);
  ASSERT_TRUE(origin.approximate);
  ASSERT_EQ(origin.image, (std::vector<OriginInterval>{{0x1000u, 0x1040u}}));
  // Each byte read is a run of its own, and nothing between two of them is one.
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u),
            (std::vector<OriginInterval>{{0x1000u, 0x1000u},
                                         {0x1010u, 0x1010u},
                                         {0x1020u, 0x1020u},
                                         {0x1030u, 0x1030u},
                                         {0x1040u, 0x1040u}}));
}

// The hull of a helper's byte overlaps two runs its caller read before the
// call, each between two of the helper's own, and one a routine the helper
// called read for it. The helper's own runs are its sources, the one its
// callee read among them; the caller's two are not, while the helper runs or
// after it has returned.
TEST(Provenance, AHullsSourcesAreTheWritingInvocationsOwnRunsAndNotItsCallers) {
  Machine m;
  for (std::uint8_t i = 0; i < 6; ++i) m.image(0x009000u + 0x10u * i, {static_cast<std::uint8_t>(i + 1u)});
  m.image(0x009045u, {0x99u});
  m.run({0xADu, 0x10u, 0x90u});  // LDA !$9010: the caller's own reads
  m.run({0xADu, 0x45u, 0x90u});  // LDA !$9045
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: the helper
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0x20u, 0x00u, 0x82u});  // JSR !$8200: a routine the helper calls
  m.run({0x6Du, 0x20u, 0x90u});  // $8200 ADC !$9020
  m.run({0x60u});                // RTS: its run joins the helper's
  for (std::uint8_t i = 3; i < 6; ++i) m.run({0x6Du, static_cast<std::uint8_t>(0x10u * i), 0x90u});  // ADC !$9030 .. !$9050
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200, in the helper
  ASSERT_TRUE(m.originAt(0x7E0200u).approximate);
  ASSERT_EQ(m.originAt(0x7E0200u).image, (std::vector<OriginInterval>{{0x1000u, 0x1050u}}));
  const std::vector<OriginInterval> own = {{0x1000u, 0x1000u}, {0x1020u, 0x1020u}, {0x1030u, 0x1030u},
                                           {0x1040u, 0x1040u}, {0x1050u, 0x1050u}};
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), own) << "the helper running";
  m.run({0x60u});  // RTS
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), own) << "the helper returned, held by its byte";
}

// A run of the writer's own that a caller's run grew from is followed out to
// the caller's, as an exact interval's is.
TEST(Provenance, AHullsRunIsFollowedOutToTheCallerThatReadOnFromIt) {
  Machine m;
  for (std::uint8_t i = 0; i < 5; ++i) m.image(0x009000u + 0x10u * i, {static_cast<std::uint8_t>(i + 1u)});
  m.image(0x009001u, {0x77u});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  for (std::uint8_t i = 1; i < 5; ++i) m.run({0x6Du, static_cast<std::uint8_t>(0x10u * i), 0x90u});  // ADC !$9010 .. !$9040
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.run({0x60u});                // RTS
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001, in the caller: its run from $1000 reaches $1001
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u),
            (std::vector<OriginInterval>{{0x1000u, 0x1001u},
                                         {0x1010u, 0x1010u},
                                         {0x1020u, 0x1020u},
                                         {0x1030u, 0x1030u},
                                         {0x1040u, 0x1040u}}));
}

// A helper that ended with its runs joining nothing, held by the byte it
// wrote: its runs are read where they were kept.
TEST(Provenance, AHullsSourcesAreReadFromAnInvocationThatEndedHeld) {
  Machine m;
  for (std::uint8_t i = 0; i < 5; ++i) m.image(0x009000u + 0x10u * i, {static_cast<std::uint8_t>(i + 1u)});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  for (std::uint8_t i = 1; i < 5; ++i) m.run({0x6Du, static_cast<std::uint8_t>(0x10u * i), 0x90u});  // ADC !$9010 .. !$9040
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.shadow.abandon();
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u),
            (std::vector<OriginInterval>{{0x1000u, 0x1000u},
                                         {0x1010u, 0x1010u},
                                         {0x1020u, 0x1020u},
                                         {0x1030u, 0x1030u},
                                         {0x1040u, 0x1040u}}));
}

// A hull no run overlaps is its own source; a run counts from the byte it
// shares with the hull, at either end, and is given whole.
TEST(Provenance, AHullNoRunOverlapsIsItsOwnSource) {
  Machine m;
  Origins& origins = m.shadow.origins();
  Origin hull = kNoOrigin;
  for (std::size_t offset = 0x1000u; offset <= 0x1040u; offset += 0x10u) hull = origins.unite(hull, origins.image(offset));
  ASSERT_TRUE(origins.of(hull).approximate);
  const std::vector<OriginInterval> itself = {{0x1000u, 0x1040u}};
  // An engine's byte belongs to no invocation.
  m.shadow.written(0x7F0000u, hull, Writer{.site = 0x008123u, .engine = true});
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0000u), itself);
  // The running invocation's byte, when it has read nothing.
  m.shadow.written(0x7F0001u, hull, Writer{.site = 0x008000u, .engine = false});
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0001u), itself);
  // A run that begins past the hull's last byte is no source of it.
  m.image(0x009041u, {0x55u});
  m.run({0xADu, 0x41u, 0x90u});  // LDA !$9041
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0001u), itself);
  // Once the run reaches the hull's last byte it is one.
  m.image(0x009040u, {0x66u});
  m.run({0xADu, 0x40u, 0x90u});  // LDA !$9040
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0001u), (std::vector<OriginInterval>{{0x1040u, 0x1041u}}));
  // And so is one that begins below the hull and reaches its first byte.
  m.image(0x008FFFu, {0x77u});
  m.image(0x009000u, {0x88u});
  m.run({0xADu, 0xFFu, 0x8Fu});  // LDA !$8FFF
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0001u), (std::vector<OriginInterval>{{0x0FFFu, 0x1000u}, {0x1040u, 0x1041u}}));
}

// A hull across the whole image, with a run at each end of it and one in the
// middle.
TEST(Provenance, AHullAcrossTheImageGivesTheRunsAtItsEndsAndBetween) {
  Machine m;
  Origins& origins = m.shadow.origins();
  Origin hull = kNoOrigin;
  for (std::size_t i = 0; i < 5; ++i) hull = origins.unite(hull, origins.image(i * (kImageBytes - 1u) / 4u));
  ASSERT_TRUE(origins.of(hull).approximate);
  ASSERT_EQ(origins.of(hull).image, (std::vector<OriginInterval>{{0u, kImageBytes - 1u}}));
  m.image(0x009000u, {0x11u});
  m.image(0x00C000u, {0x22u});
  m.image(0x00FFFFu, {0x33u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0xADu, 0x00u, 0xC0u});  // LDA !$C000
  m.run({0xADu, 0xFFu, 0xFFu});  // LDA !$FFFF
  m.shadow.written(0x7F0000u, hull, Writer{.site = 0x008009u, .engine = false});
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0000u),
            (std::vector<OriginInterval>{{0x1000u, 0x1000u}, {0x4000u, 0x4000u}, {0x7FFFu, 0x7FFFu}}));
}

// A hull far wider than any image, with a run on its second byte and one
// further in: the walk goes from run to run, so it answers at once however
// many bytes lie between two of them.
TEST(Provenance, AHullsWalkGoesFromRunToRunHoweverWideItIs) {
  Machine m;
  Origins& origins = m.shadow.origins();
  constexpr std::size_t kFar = std::size_t{1} << 40;
  Origin hull = kNoOrigin;
  for (std::size_t i = 0; i < 5; ++i) hull = origins.unite(hull, origins.image(i * (kFar / 4u)));
  ASSERT_EQ(origins.of(hull).image, (std::vector<OriginInterval>{{0u, kFar}}));
  m.image(0x009000u, {0x11u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0xADu, 0x01u, 0x80u});  // LDA !$8001: the image's second byte
  m.shadow.written(0x7F0000u, hull, Writer{.site = 0x008006u, .engine = false});
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0000u), (std::vector<OriginInterval>{{1u, 1u}, {0x1000u, 0x1000u}}));
}

// ---- an answer asked again -----------------------------------------------------

// A byte's sources are asked for while its helper runs, after the helper has
// returned and its run has joined the caller's, and after the caller has read
// on: each answer is the one the shadow gives as it then stands. A byte with
// the same origin that an engine wrote, and one the same invocation wrote from
// another byte, are each answered for themselves.
TEST(Provenance, AnAnswerAskedAgainIsTheShadowsAsItThenStands) {
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u});
  m.image(0x009010u, {0x20u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001: the caller's run, $1000-$1001
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0xADu, 0x02u, 0x90u});  // $8100 LDA !$9002
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  m.run({0xADu, 0x10u, 0x90u});  // LDA !$9010
  m.run({0x8Du, 0x01u, 0x02u});  // STA !$0201
  const std::vector<OriginInterval> alone = {{0x1002u, 0x1002u}};
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), alone) << "the helper running: its own run";
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), alone) << "asked again";
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0201u), (std::vector<OriginInterval>{{0x1010u, 0x1010u}}))
      << "the same invocation's byte from another origin";
  m.run({0x60u});  // RTS: the helper's run joins the caller's, which it touches
  const std::vector<OriginInterval> joined = {{0x1000u, 0x1002u}};
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), joined) << "the helper returned";
  // An engine's byte with the same origin belongs to no invocation.
  m.shadow.written(0x7F0000u, *m.shadow.originOf(0x7E0200u), Writer{.site = 0x008123u, .engine = true});
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0000u), alone);
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), joined);
  m.run({0xADu, 0x03u, 0x90u});  // LDA !$9003, back in the caller: its run reaches $1003
  // The caller's run now holds the helper's strictly inside, so it is not followed.
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), alone) << "the caller read on";
  EXPECT_EQ(m.shadow.sourcesOf(0x7F0000u), alone);
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

// ---- the console's reset --------------------------------------------------------

TEST(Provenance, AbandoningTheCallStackLeavesTheRootAndItsRuns) {
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u});
  m.run({0xADu, 0x00u, 0x90u});  // LDA !$9000: the root's own run
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200
  ASSERT_EQ(m.shadow.depth(), 3u);
  m.shadow.abandon();
  EXPECT_EQ(m.shadow.depth(), 1u);
  m.shadow.abandon();
  EXPECT_EQ(m.shadow.depth(), 1u) << "the root is never abandoned";
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001, in the root again
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1001u}}));
}

TEST(Provenance, AnAbandonedInvocationsRunsJoinNothing) {
  // A helper reads four bytes and the reset ends it there: a return would have
  // joined its run to the root's, and the abandoned one does not.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u});
  m.run({0x20u, 0x00u, 0x81u});                                   // JSR !$8100
  for (std::uint8_t i = 0; i < 4; ++i) m.run({0xADu, i, 0x90u});  // $8100 LDA !$9000 .. !$9003
  m.shadow.abandon();
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002, in the root
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1002u, 0x1002u}}));
}

TEST(Provenance, AnAbandonedInvocationsByteNamesItsOwnRunAndNoCaller) {
  // The root read eight bytes; a helper reads the first two of them and writes
  // one to work RAM, and the reset ends it. Held by the byte, its run stays
  // readable, and the source stops there: no caller is left whose run grew from it.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u});
  for (std::uint8_t i = 0; i < 8; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9000 .. !$9007
  m.run({0x20u, 0x00u, 0x81u});                                    // JSR !$8100
  m.run({0xADu, 0x00u, 0x90u});                                    // $8100 LDA !$9000
  m.run({0xADu, 0x01u, 0x90u});                                    // LDA !$9001
  m.run({0x8Du, 0x00u, 0x02u});                                    // STA !$0200: from $9001
  ASSERT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1007u}}))
      << "while the helper is live, the root's run is followed out to";
  m.shadow.abandon();
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1001u}}));
}

// ---- the stack pointer and the invocations -------------------------------------

TEST(Provenance, ACallMarksTheInvocationWhereItsPushesLeftThePointer) {
  Machine m;
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: pushes at $01FF and $01FE
  EXPECT_EQ(m.interpreter.registers.s, 0x01FDu);
  EXPECT_EQ(m.shadow.depth(), 2u);
  EXPECT_NE(m.shadow.running(), root);
  EXPECT_TRUE(m.shadow.live(m.shadow.running()));
  EXPECT_TRUE(m.shadow.live(root));
}

TEST(Provenance, AReturnPastTheMarkEndsTheInvocationAndItsCallerRuns) {
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  const std::uint32_t helper = m.shadow.running();
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x60u});                // RTS: pulls $01FE and $01FF, past the mark
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_FALSE(m.shadow.live(helper));
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002, in the caller
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1002u}}));
}

TEST(Provenance, APulledReturnAddressEndsTheInvocation) {
  // A routine that pulls its own return address and runs on: from the pull,
  // its reads are its caller's.
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u, 0x12u, 0x13u, 0x14u});
  const std::uint32_t root = m.shadow.running();
  for (std::uint8_t i = 1; i < 4; ++i) m.run({0xADu, i, 0x90u});  // LDA !$9001 .. !$9003
  m.run({0x20u, 0x00u, 0x81u});                                    // JSR !$8100
  m.run({0xADu, 0x00u, 0x90u});                                    // $8100 LDA !$9000
  m.run({0x68u});                                                  // PLA: the return address's low byte
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
  m.run({0x68u});                // PLA: its high byte, at the root
  m.run({0xADu, 0x04u, 0x90u});  // LDA !$9004
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200: the root's write, from the root's run
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1004u}}));
}

TEST(Provenance, AReturnToAPushedAddressEndsNothing) {
  Machine m;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: S $01FD
  const std::uint32_t helper = m.shadow.running();
  m.run({0xF4u, 0xFFu, 0x81u});  // $8100 PEA $81FF: S $01FB
  m.run({0x60u});                // RTS: a jump to $8200, S $01FD again
  EXPECT_EQ(m.interpreter.registers.pc, 0x8200u);
  EXPECT_EQ(m.shadow.depth(), 2u);
  EXPECT_EQ(m.shadow.running(), helper);
  m.run({0x60u});  // $8200 RTS: through the call's address
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_FALSE(m.shadow.live(helper));
}

TEST(Provenance, ATransferIntoThePointerEndsNothing) {
  Machine m;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: marked at $01FD
  const std::uint32_t helper = m.shadow.running();
  m.run({0xC2u, 0x10u});         // $8100 REP #$10
  m.run({0xA2u, 0xFFu, 0x01u});  // LDX #$01FF
  m.run({0x9Au});                // TXS: above the mark
  EXPECT_EQ(m.interpreter.registers.s, 0x01FFu);
  EXPECT_EQ(m.shadow.depth(), 2u);
  EXPECT_EQ(m.shadow.running(), helper);
  m.run({0xC2u, 0x20u});         // REP #$20
  m.run({0xA9u, 0x80u, 0x01u});  // LDA #$0180
  m.run({0x1Bu});                // TCS: below it
  EXPECT_EQ(m.interpreter.registers.s, 0x0180u);
  EXPECT_EQ(m.shadow.depth(), 2u);
  EXPECT_EQ(m.shadow.running(), helper);
}

TEST(Provenance, APushPastAMarkEndsTheInvocationItOverwrites) {
  Machine m;
  m.image(0x009000u, {0x10u, 0x11u});
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: marked at $01FD
  const std::uint32_t helper = m.shadow.running();
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0xC2u, 0x10u});         // REP #$10
  m.run({0xA2u, 0xFFu, 0x01u});  // LDX #$01FF
  m.run({0x9Au});                // TXS: S $01FF, above the mark
  m.run({0x48u});                // PHA: writes $01FF
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.run({0x48u});  // PHA: writes $01FE, the return address's low byte
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_FALSE(m.shadow.live(helper));
  m.run({0x48u});                // PHA: writes $01FD
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x00u, 0x02u});  // STA !$0200: the helper's read joined the root's
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.sourcesOf(0x7E0200u), (std::vector<OriginInterval>{{0x1000u, 0x1001u}}));
}

TEST(Provenance, APushPastAMarkGivesItsCalleesToItsCaller) {
  // `a`, called from `p`, calls `b` on a stack of its own; `b` moves to just
  // above `a`'s mark and pushes over it. `a` ends, and `b` is `p`'s.
  Machine m;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: p, marked at $01FD
  const std::uint32_t p = m.shadow.running();
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200: a, marked at $01FB
  const std::uint32_t a = m.shadow.running();
  m.run({0xC2u, 0x10u});         // $8200 REP #$10
  m.run({0xA2u, 0xFFu, 0x02u});  // LDX #$02FF
  m.run({0x9Au});                // TXS
  m.run({0x20u, 0x00u, 0x83u});  // JSR !$8300: b, marked at $02FD
  const std::uint32_t b = m.shadow.running();
  m.run({0xA2u, 0xFCu, 0x01u});  // $8300 LDX #$01FC
  m.run({0x9Au});                // TXS
  m.run({0x48u});                // PHA: writes $01FC, past a's mark
  EXPECT_FALSE(m.shadow.live(a));
  EXPECT_EQ(m.shadow.running(), b);
  EXPECT_TRUE(m.shadow.within(p));
  EXPECT_EQ(m.shadow.depth(), 3u);
  m.run({0xA2u, 0xFDu, 0x02u});  // LDX #$02FD: b's mark
  m.run({0x9Au});                // TXS
  m.run({0x60u});                // RTS: through b's address
  EXPECT_EQ(m.shadow.running(), p);
  EXPECT_EQ(m.shadow.depth(), 2u);
}

TEST(Provenance, AnInvocationOnAnotherStackStaysLiveAndIsReturnedInto) {
  Machine m;
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: a, marked at $01FD
  const std::uint32_t a = m.shadow.running();
  m.run({0xC2u, 0x10u});         // $8100 REP #$10
  m.run({0xA2u, 0xFFu, 0x02u});  // LDX #$02FF
  m.run({0x9Au});                // TXS: a stack of its own
  m.run({0x20u, 0x00u, 0x82u});  // JSR !$8200: b, marked at $02FD
  const std::uint32_t b = m.shadow.running();
  EXPECT_EQ(m.shadow.depth(), 3u);
  m.run({0xA2u, 0xFDu, 0x01u});  // $8200 LDX #$01FD: a's mark
  m.run({0x9Au});                // TXS
  m.run({0x60u});                // RTS: through a's address
  EXPECT_FALSE(m.shadow.live(a));
  EXPECT_TRUE(m.shadow.live(b));
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.run({0xA2u, 0xFDu, 0x02u});  // LDX #$02FD: b's mark
  m.run({0x9Au});                // TXS
  m.run({0x60u});                // RTS: through b's address
  EXPECT_FALSE(m.shadow.live(b));
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_EQ(m.shadow.depth(), 1u);
}

TEST(Provenance, ACalleeLeftBehindByAReturnIsTheRoots) {
  // As above with `a` called from `p`: once `a` is returned through, `b` has
  // no caller left, and a return through `b` leaves the root running.
  Machine m;
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: p, marked at $01FD
  const std::uint32_t p = m.shadow.running();
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200: a, marked at $01FB
  const std::uint32_t a = m.shadow.running();
  m.run({0xC2u, 0x10u});         // $8200 REP #$10
  m.run({0xA2u, 0xFFu, 0x02u});  // LDX #$02FF
  m.run({0x9Au});                // TXS
  m.run({0x20u, 0x00u, 0x83u});  // JSR !$8300: b, marked at $02FD
  const std::uint32_t b = m.shadow.running();
  m.run({0xA2u, 0xFBu, 0x01u});  // $8300 LDX #$01FB: a's mark
  m.run({0x9Au});                // TXS
  m.run({0x60u});                // RTS: through a's address, into p
  EXPECT_FALSE(m.shadow.live(a));
  EXPECT_EQ(m.shadow.running(), p);
  m.run({0xA2u, 0xFDu, 0x02u});  // LDX #$02FD: b's mark
  m.run({0x9Au});                // TXS
  m.run({0x60u});                // RTS: through b's address
  EXPECT_FALSE(m.shadow.live(b));
  EXPECT_TRUE(m.shadow.live(p));
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_EQ(m.shadow.depth(), 2u);
}

TEST(Provenance, TheStackWrapsInsidePageOneInEmulationMode) {
  Machine m;
  m.interpreter.registers.e = true;
  m.interpreter.registers.s = 0x0100u;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: pushes at $0100 and $01FF
  EXPECT_EQ(m.interpreter.registers.s, 0x01FEu);
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.run({0x60u});  // $8100 RTS: pulls $01FF and $0100
  EXPECT_EQ(m.interpreter.registers.s, 0x0100u);
  EXPECT_EQ(m.shadow.depth(), 1u);

  // The same two moves told around the bank: neither is a push or a pull.
  Machine control;
  control.shadow.called(0x0100u, 0x01FEu, false);
  control.shadow.stackMoved(0x01FEu, 0x0100u, false);
  EXPECT_EQ(control.shadow.depth(), 2u);
}

TEST(Provenance, APushAroundPageOnePassesAMarkAtItsTop) {
  // Called at S = $0101 in emulation mode: the pushes land at $0101 and $0100
  // and the mark is $01FF, around the page. A push from $0100 writes the
  // return address's low byte and wraps to $01FF, past the mark.
  Machine m;
  m.interpreter.registers.e = true;
  m.interpreter.registers.s = 0x0101u;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  EXPECT_EQ(m.interpreter.registers.s, 0x01FFu);
  ASSERT_EQ(m.shadow.depth(), 2u);
  m.run({0xA2u, 0x00u});  // $8100 LDX #$00
  m.run({0x9Au});         // TXS: S = $0100
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.run({0x48u});  // PHA: writes $0100
  EXPECT_EQ(m.interpreter.registers.s, 0x01FFu);
  EXPECT_EQ(m.shadow.depth(), 1u);
}

TEST(Provenance, AnInterruptIsACallThatWithinStopsAt) {
  Machine m;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: p
  const std::uint32_t p = m.shadow.running();
  const std::uint16_t s = m.interpreter.registers.s;
  m.interpreter.registers.s = static_cast<std::uint16_t>(s - 4u);
  m.shadow.interrupted(s, static_cast<std::uint16_t>(s - 4u), false);  // a hardware interrupt's four pushes
  const std::uint32_t handler = m.shadow.running();
  EXPECT_FALSE(m.shadow.within(p));
  EXPECT_TRUE(m.shadow.within(handler));
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200: q, from the handler
  const std::uint32_t q = m.shadow.running();
  EXPECT_TRUE(m.shadow.within(q));
  EXPECT_TRUE(m.shadow.within(handler));
  EXPECT_FALSE(m.shadow.within(p));
  m.interpreter.registers.s = s;
  m.shadow.stackMoved(static_cast<std::uint16_t>(s - 4u), s, false);  // the RTI's pulls
  EXPECT_FALSE(m.shadow.live(handler));
  EXPECT_EQ(m.shadow.running(), p);
  EXPECT_TRUE(m.shadow.within(p));
}

TEST(Provenance, WithinFollowsTheCallers) {
  Machine m;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: outer
  const std::uint32_t outer = m.shadow.running();
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200: inner
  const std::uint32_t inner = m.shadow.running();
  EXPECT_TRUE(m.shadow.within(outer));
  EXPECT_TRUE(m.shadow.within(inner));
  m.run({0x60u});  // $8200 RTS
  EXPECT_FALSE(m.shadow.within(inner));
  EXPECT_FALSE(m.shadow.live(inner));
  EXPECT_TRUE(m.shadow.within(outer));
}

TEST(Provenance, AMoveOfMoreThanFourBytesPassesNothing) {
  Machine m;
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: marked at $01FD
  m.shadow.stackMoved(0x01FDu, 0x01FDu + 100u, false);
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.shadow.stackMoved(0x0250u, 0x0250u - 100u, false);
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.shadow.stackMoved(0x01FDu, 0x0202u, false);  // five bytes
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.shadow.stackMoved(0x0201u, 0x01FCu, false);  // five bytes down
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.shadow.stackMoved(0x01FDu, 0x0201u, false);  // four bytes: a pull past the mark
  EXPECT_EQ(m.shadow.depth(), 1u);
}

TEST(Provenance, AReturnThroughTwoMarksEndsBoth) {
  Machine m;
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: a, marked at $01FD
  m.run({0x20u, 0x00u, 0x82u});  // $8100 JSR !$8200: b, marked at $01FB
  EXPECT_EQ(m.shadow.depth(), 3u);
  m.run({0x6Bu});  // $8200 RTL: pulls $01FC, $01FD and $01FE
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
}

TEST(Provenance, TheRootIsNeverEnded) {
  Machine m;
  const std::uint32_t root = m.shadow.running();
  m.run({0x68u});  // PLA with nothing called
  m.run({0x68u});
  m.run({0x48u});  // PHA
  m.run({0x48u});
  m.run({0x48u});
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_TRUE(m.shadow.live(root));
}

TEST(Provenance, AbandonEndsEveryInvocationOnEveryStack) {
  Machine m;
  const std::uint32_t root = m.shadow.running();
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100: a, marked at $01FD
  const std::uint32_t a = m.shadow.running();
  m.run({0xC2u, 0x10u});         // $8100 REP #$10
  m.run({0xA2u, 0xFFu, 0x02u});  // LDX #$02FF
  m.run({0x9Au});                // TXS
  m.run({0x20u, 0x00u, 0x82u});  // JSR !$8200: b, marked at $02FD
  const std::uint32_t b = m.shadow.running();
  ASSERT_EQ(m.shadow.depth(), 3u);
  m.shadow.abandon();
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
  EXPECT_FALSE(m.shadow.live(a));
  EXPECT_FALSE(m.shadow.live(b));
  m.run({0x60u});  // $8200 RTS: past where b was marked; the abandon cleared the mark
  EXPECT_EQ(m.shadow.depth(), 1u);
  m.run({0x20u, 0x00u, 0x83u});  // JSR !$8300: a new invocation in a freed slot
  EXPECT_EQ(m.shadow.depth(), 2u);
  m.run({0x60u});  // $8300 RTS
  EXPECT_EQ(m.shadow.depth(), 1u);
  EXPECT_EQ(m.shadow.running(), root);
}

TEST(Provenance, LoadsStackPointerNamesTheThreeTransfers) {
  for (const char* name : {"TCS", "TXS", "XCE"}) {
    Instruction instruction;
    instruction.mnemonic = name;
    EXPECT_TRUE(loadsStackPointer(instruction)) << name;
  }
  for (const char* name : {"PHA", "PLA", "RTS", "JSR", "TSC", "TSX"}) {
    Instruction instruction;
    instruction.mnemonic = name;
    EXPECT_FALSE(loadsStackPointer(instruction)) << name;
  }
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

TEST(Provenance, AStreamClosedWhileItsHelperRunsIsTheHelpersRun) {
  // The helper reads three bytes and carries the first two; the run ends with
  // the helper still running, and the root read nothing.
  Machine m;
  m.image(0x009000u, {0x11u, 0x22u, 0x33u});
  m.run({0x20u, 0x00u, 0x81u});  // JSR !$8100
  m.run({0xADu, 0x00u, 0x90u});  // $8100 LDA !$9000
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x01u, 0x90u});  // LDA !$9001
  m.run({0x8Du, 0x22u, 0x21u});  // STA !$2122
  m.run({0xADu, 0x02u, 0x90u});  // LDA !$9002: read, not carried
  m.shadow.finish();
  ASSERT_EQ(m.shadow.streams().size(), 1u);
  EXPECT_EQ(m.shadow.streams().front().source, (OriginInterval{0x1000u, 0x1002u}));
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
