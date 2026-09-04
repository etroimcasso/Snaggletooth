// The 65816 disassembler.
//
// The load-bearing case here is DeclaredLengthMatchesTheInterpreter: under every
// setting of the mode flags it walks all 256 opcodes and compares the length the
// disassembler reports against the distance the interpreter actually moves the
// program counter. The instruction table is hand-authored and an immediate's
// length depends on the flags, so that case is what stops a wrong operand shape
// — or a width applied to the wrong instruction — from reading as a plausible
// instruction, the failure that makes a disassembler worse than useless because
// its output still looks like code.
//
// The rest pins what is new beside the SPC700 backend: the widths moving through
// REP, SEP and XCE, the refusal to read an immediate under a width the trace does
// not know, a conflict between two widths reported rather than chosen, the
// per-entry modes, the width directives the listing carries, the costs measured
// under each flag setting, and the registers named only in the banks that show
// them.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpu65816_disasm.h"
#include "snaggletooth/cpu/cpu65816.h"

namespace snaggletooth::disasm {
namespace {

// A flat 64 KB bus answering every bank from the same bytes — the shape the
// disassembler's own cost probe runs against.
struct FlatBus {
  std::array<std::uint8_t, 65536> ram{};
  std::uint8_t read(std::uint32_t address, CycleKind) { return ram[address & 0xFFFFu]; }
  void write(std::uint32_t address, std::uint8_t value, CycleKind) {
    ram[address & 0xFFFFu] = value;
  }
  void internal(std::uint32_t) {}
  void internal(std::uint32_t, CycleKind) {}
};

constexpr Address kAt = 0x008000;

// The five settings of the flags that read bytes differently: native mode under
// each pair of widths, and emulation mode, which forces both to eight.
const std::array<Cpu65816Mode, 5> kSettings = {
    Cpu65816Mode::native(true, true),
    Cpu65816Mode::native(true, false),
    Cpu65816Mode::native(false, true),
    Cpu65816Mode::native(false, false),
    Cpu65816Mode::reset(),
};

// Decodes one opcode with zero operand bytes behind it.
Instruction decodeBare(std::uint8_t opcode, const Cpu65816Mode& mode) {
  const std::array<std::uint8_t, 4> image = {opcode, 0x00, 0x00, 0x00};
  const std::optional<Instruction> decoded = decodeAt(image, kAt, kAt, mode);
  EXPECT_TRUE(decoded.has_value()) << "opcode $" << std::hex << int{opcode};
  return decoded.value_or(Instruction{});
}

// The text of one instruction decoded at the base under a mode.
std::string textOf(std::vector<std::uint8_t> bytes, const Cpu65816Mode& mode,
                   Address at = kAt) {
  const std::optional<Instruction> decoded = decodeAt(bytes, at, at, mode);
  EXPECT_TRUE(decoded.has_value());
  return decoded ? decoded->text : std::string{};
}

Listing traceFrom(const std::vector<std::uint8_t>& image, const Cpu65816Mode& mode,
                  Address base = kAt) {
  Request request;
  request.image = image;
  request.base = base;
  request.context = contextOf(mode);
  return trace(cpu65816Backend(), request);
}

std::vector<const Line*> codeLines(const Listing& listing) {
  std::vector<const Line*> out;
  for (const Line& line : listing.lines) {
    if (line.isCode) out.push_back(&line);
  }
  return out;
}

}  // namespace

TEST(Cpu65816Disasm, EveryOpcodeDecodesToText) {
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    const Instruction decoded =
        decodeBare(static_cast<std::uint8_t>(opcode), Cpu65816Mode::native(true, true));
    EXPECT_FALSE(decoded.text.empty()) << "opcode $" << std::hex << opcode;
    EXPECT_GE(decoded.length, 1);
    EXPECT_LE(decoded.length, 4);
    EXPECT_EQ(decoded.bytes.size(), decoded.length);
    EXPECT_EQ(decoded.opcode, opcode);
  }
}

// The cross-check. Under every setting of the flags, for every instruction that
// falls through to the next one, the length the table declares must equal the
// distance the interpreter moves the program counter.
TEST(Cpu65816Disasm, DeclaredLengthMatchesTheInterpreter) {
  for (const Cpu65816Mode& mode : kSettings) {
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
      const std::uint8_t byte = static_cast<std::uint8_t>(opcode);
      const Instruction decoded = decodeBare(byte, mode);
      if (decoded.flow != Flow::Continue) continue;

      FlatBus bus;
      bus.ram[kAt & 0xFFFFu] = byte;  // operand bytes stay zero, as in decodeBare

      Cpu65816State start;
      start.pc = static_cast<std::uint16_t>(kAt);
      start.s = 0x01FF;
      start.e = mode.emulation;
      start.p = static_cast<std::uint8_t>((mode.accumulator8 ? kCpuFlagM : 0) |
                                          (mode.index8 ? kCpuFlagX : 0));
      Cpu65816 cpu{start};
      cpu.stepInstruction(bus);

      const std::uint16_t advanced =
          static_cast<std::uint16_t>(cpu.state().pc - static_cast<std::uint16_t>(kAt));
      EXPECT_EQ(advanced, decoded.length)
          << "opcode $" << std::hex << opcode << " (" << decoded.text << ") under "
          << cpu65816Backend().describe(contextOf(mode));
    }
  }
}

// An immediate is as wide as the register it loads: the accumulator's width for
// the accumulator instructions, the index width for LDX, LDY, CPX and CPY. The
// masks and signatures are always one byte.
TEST(Cpu65816Disasm, ImmediateWidthFollowsTheFlags) {
  const Cpu65816Mode wideAccumulator = Cpu65816Mode::native(false, true);
  const Cpu65816Mode wideIndex = Cpu65816Mode::native(true, false);

  EXPECT_EQ(decodeBare(0xA9, wideAccumulator).length, 3);  // LDA #
  EXPECT_EQ(decodeBare(0xA9, wideIndex).length, 2);
  EXPECT_EQ(decodeBare(0xA2, wideIndex).length, 3);        // LDX #
  EXPECT_EQ(decodeBare(0xA2, wideAccumulator).length, 2);
  EXPECT_EQ(decodeBare(0xA0, wideIndex).length, 3);        // LDY #
  EXPECT_EQ(decodeBare(0xE0, wideIndex).length, 3);        // CPX #
  EXPECT_EQ(decodeBare(0xC0, wideIndex).length, 3);        // CPY #
  EXPECT_EQ(decodeBare(0x89, wideAccumulator).length, 3);  // BIT #
  EXPECT_EQ(decodeBare(0x69, wideAccumulator).length, 3);  // ADC #
  EXPECT_EQ(decodeBare(0xC9, wideIndex).length, 2);        // CMP # follows the accumulator

  constexpr std::array<std::uint8_t, 5> kOneByteImmediates = {0xC2, 0xE2, 0x42, 0x00, 0x02};
  for (std::uint8_t opcode : kOneByteImmediates) {  // REP SEP WDM BRK COP
    EXPECT_EQ(decodeBare(opcode, Cpu65816Mode::native(false, false)).length, 2)
        << "opcode $" << std::hex << int{opcode};
  }

  // Emulation mode holds both widths at eight whatever the mode says.
  Cpu65816Mode emulated = Cpu65816Mode::native(false, false);
  emulated.emulation = true;
  EXPECT_EQ(decodeBare(0xA9, emulated).length, 2);
  EXPECT_EQ(decodeBare(0xA2, emulated).length, 2);
}

TEST(Cpu65816Disasm, RepAndSepSteerTheInstructionsAfterThem) {
  const std::vector<std::uint8_t> image = {
      0xC2, 0x30,        // REP #$30 — both widths sixteen
      0xA9, 0x34, 0x12,  // LDA #$1234
      0xE2, 0x20,        // SEP #$20 — the accumulator back to eight
      0xA9, 0x12,        // LDA #$12
      0xA2, 0x34, 0x12,  // LDX #$1234 — the index is still sixteen
      0x60,              // RTS
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::native(true, true));
  const std::vector<const Line*> lines = codeLines(listing);
  ASSERT_EQ(lines.size(), 6u);
  EXPECT_EQ(lines[0]->instruction.text, "REP #$30");
  EXPECT_EQ(lines[1]->instruction.text, "LDA #$1234");
  EXPECT_EQ(lines[2]->instruction.text, "SEP #$20");
  EXPECT_EQ(lines[3]->instruction.text, "LDA #$12");
  EXPECT_EQ(lines[4]->instruction.text, "LDX #$1234");
  EXPECT_EQ(lines[5]->instruction.text, "RTS");
  EXPECT_TRUE(listing.warnings.empty());
}

// The same bytes traced from the reset vector: emulation mode ignores the mask,
// so the load that read as three bytes above reads as two here.
TEST(Cpu65816Disasm, EmulationModeHoldsBothWidthsAtEight) {
  const std::vector<std::uint8_t> image = {
      0xC2, 0x30,        // REP #$30 — no effect on the widths
      0xA9, 0x34,        // LDA #$34
      0x60,              // RTS
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::reset());
  const std::vector<const Line*> lines = codeLines(listing);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[1]->instruction.text, "LDA #$34");
  EXPECT_EQ(lines[1]->instruction.length, 2);
}

// PLP and RTI make the widths unknown in native mode; under emulation the flag
// forces both to eight, so they stay known — through the one function the
// assembler follows too, so a NATIVE after an emulation RTI keeps them.
TEST(Cpu65816Disasm, ReturnUnderEmulationKeepsTheWidthsKnown) {
  std::string note;
  const Cpu65816Mode afterRti = cpu65816ModeAfter(0x40, 0, Cpu65816Mode::reset(), note);
  EXPECT_TRUE(afterRti.emulation);
  EXPECT_TRUE(afterRti.accumulatorKnown);
  EXPECT_TRUE(afterRti.indexKnown);
  EXPECT_TRUE(afterRti.accumulator8);
  EXPECT_TRUE(afterRti.index8);
  const Cpu65816Mode afterPlp = cpu65816ModeAfter(0x28, 0, Cpu65816Mode::reset(), note);
  EXPECT_TRUE(afterPlp.accumulatorKnown);
  EXPECT_TRUE(afterPlp.indexKnown);
  const Cpu65816Mode native = cpu65816ModeAfter(0x40, 0, Cpu65816Mode::native(true, true), note);
  EXPECT_FALSE(native.accumulatorKnown) << "in native mode the stacked status byte is unknown";
  EXPECT_FALSE(native.indexKnown);
}

// XCE takes the carry the instruction before it set. CLC then XCE enters native
// mode with the widths still eight; SEC then XCE enters emulation and forces
// them.
TEST(Cpu65816Disasm, TheCarryDecidesWhatXceDoes) {
  const std::vector<std::uint8_t> entering = {
      0x18,              // CLC
      0xFB,              // XCE — native
      0xC2, 0x30,        // REP #$30 — now takes effect
      0xA9, 0x34, 0x12,  // LDA #$1234
      0x60,              // RTS
  };
  const Listing native = traceFrom(entering, Cpu65816Mode::reset());
  ASSERT_EQ(codeLines(native).size(), 5u);
  EXPECT_EQ(codeLines(native)[3]->instruction.text, "LDA #$1234");
  EXPECT_TRUE(native.warnings.empty());

  const std::vector<std::uint8_t> leaving = {
      0x38,        // SEC
      0xFB,        // XCE — emulation, both widths forced to eight
      0xA9, 0x12,  // LDA #$12
      0x60,        // RTS
  };
  const Listing emulated = traceFrom(leaving, Cpu65816Mode::native(false, false));
  ASSERT_EQ(codeLines(emulated).size(), 4u);
  EXPECT_EQ(codeLines(emulated)[2]->instruction.text, "LDA #$12");
  EXPECT_TRUE(modeOf(codeLines(emulated)[2]->context).emulation);
  // The instructions above already say the widths changed, so nothing is restated.
  EXPECT_TRUE(codeLines(emulated)[2]->directives.empty());
}

TEST(Cpu65816Disasm, XceWithAnUnknownCarryKeepsTheModeAndSaysSo) {
  const std::vector<std::uint8_t> image = {
      0xEA,              // NOP — says nothing about the carry
      0xFB,              // XCE
      0xA9, 0x34, 0x12,  // LDA #$1234 — still sixteen bits wide
      0x60,              // RTS
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::native(false, false));
  const std::vector<const Line*> lines = codeLines(listing);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_NE(lines[1]->instruction.note.find("carry"), std::string::npos);
  EXPECT_EQ(lines[2]->instruction.text, "LDA #$1234");
  EXPECT_FALSE(modeOf(lines[2]->context).emulation);

  // The carry memory spans one instruction: a SEC two instructions before an
  // XCE does not reach it, so the mode is kept here too.
  const std::vector<std::uint8_t> stale = {
      0x38,              // SEC
      0xEA,              // NOP — forgets what SEC said
      0xFB,              // XCE
      0xA9, 0x34, 0x12,  // LDA #$1234 — still sixteen bits wide
      0x60,              // RTS
  };
  const Listing kept = traceFrom(stale, Cpu65816Mode::native(false, false));
  ASSERT_EQ(codeLines(kept).size(), 5u);
  EXPECT_EQ(codeLines(kept)[3]->instruction.text, "LDA #$1234");
  EXPECT_NE(codeLines(kept)[2]->instruction.note.find("carry"), std::string::npos);
}

// An immediate under a width the trace does not know is not guessed at. The path
// stops there, the listing says why, and the bytes stay data.
TEST(Cpu65816Disasm, AnImmediateUnderAnUnknownWidthStopsTheTrace) {
  const std::vector<std::uint8_t> image = {
      0x28,        // PLP — the widths after it are whatever the stack held
      0xA9, 0x12,  // LDA #imm — one byte or two, nobody can say
      0x60,        // RTS
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::native(true, true));
  ASSERT_EQ(codeLines(listing).size(), 1u);
  EXPECT_EQ(codeLines(listing)[0]->instruction.text, "PLP");
  ASSERT_EQ(listing.warnings.size(), 1u);
  EXPECT_NE(listing.warnings[0].find("$00:8001"), std::string::npos);
  EXPECT_NE(listing.warnings[0].find("cannot be read"), std::string::npos);
  EXPECT_NE(listing.warnings[0].find("LDA #"), std::string::npos);

  // An instruction that needs no width still reads under an unknown one.
  EXPECT_TRUE(decodeAt(image, kAt, kAt + 3, Cpu65816Mode::nativeUnknown()).has_value());
  EXPECT_FALSE(decodeAt(image, kAt, kAt + 1, Cpu65816Mode::nativeUnknown()).has_value());
}

// Two paths reaching one address with different widths is reported, naming
// both, and the first reading is kept.
TEST(Cpu65816Disasm, TwoWidthsReachingOneAddressAreReportedNotGuessed) {
  const std::vector<std::uint8_t> image = {
      0xC2, 0x20,        // $8000  REP #$20 — accumulator sixteen
      0x20, 0x08, 0x80,  // $8002  JSR !$8008 — reaches $8008 sixteen wide
      0xE2, 0x20,        // $8005  SEP #$20 — accumulator eight
      0xEA,              // $8007  NOP — falls into $8008 eight wide
      0xA9, 0x34, 0x12,  // $8008  LDA #imm
      0x60,              // $800B  RTS
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::native(true, true));
  ASSERT_EQ(listing.warnings.size(), 1u);
  EXPECT_NE(listing.warnings[0].find("$00:8008"), std::string::npos);
  EXPECT_NE(listing.warnings[0].find("m=16"), std::string::npos);
  EXPECT_NE(listing.warnings[0].find("m=8"), std::string::npos);
  for (const Line* line : codeLines(listing)) {
    if (line->address == 0x008008u) {
      EXPECT_EQ(line->instruction.text, "LDA #$1234");
      // The instruction above left the accumulator eight wide; the reading kept
      // here is sixteen, and the listing has to say so for the source to assemble.
      EXPECT_EQ(line->directives, (std::vector<std::string>{"A16"}));
    }
  }
}

// The carry memory rides in the context but is not a reading: an address reached
// once right after CLC and once from a branch is not a conflict.
TEST(Cpu65816Disasm, WhatIsKnownAboutTheCarryIsNotAConflict) {
  const std::vector<std::uint8_t> image = {
      0x18,        // $8000  CLC
      0xEA,        // $8001  NOP — reached with the carry known, then from the branch without
      0x80, 0xFD,  // $8002  BRA $8001
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::native(true, true));
  EXPECT_TRUE(listing.warnings.empty());
  EXPECT_EQ(codeLines(listing).size(), 3u);
}

// Each entry starts in the mode given for it, so a reset handler and an
// interrupt handler trace together from one request.
TEST(Cpu65816Disasm, EachEntryStartsInItsOwnMode) {
  const std::vector<std::uint8_t> image = {
      0xA9, 0x12,  // $8000  LDA #$12 — the reset path, emulation mode
      0x60,        // $8002  RTS
      0xA9, 0x12,  // $8003  LDA #imm — the interrupt path, width unknown
      0x60,        // $8005  RTS
  };
  Request request;
  request.image = image;
  request.base = kAt;
  request.entries = {kAt, kAt + 3};
  request.entryContexts = {contextOf(Cpu65816Mode::reset()),
                           contextOf(Cpu65816Mode::nativeUnknown())};
  const Listing listing = trace(cpu65816Backend(), request);
  ASSERT_EQ(codeLines(listing).size(), 2u);
  EXPECT_EQ(codeLines(listing)[0]->instruction.text, "LDA #$12");
  ASSERT_EQ(listing.warnings.size(), 1u);
  EXPECT_NE(listing.warnings[0].find("$00:8003"), std::string::npos);
}

// The costs are measured by running the interpreter under each setting of the
// flags; these pin the measurement against the datasheet's own table, which
// assumes eight-bit widths, and against the extra byte a wide register moves.
TEST(Cpu65816Disasm, MeasuredCostsMatchTheDatasheetsTable) {
  const std::array<CycleCost, 256>& emulated = cpu65816CycleTable(true, true, true);
  EXPECT_EQ(emulated[0xEA].base, 2);  // NOP
  EXPECT_EQ(emulated[0xA9].base, 2);  // LDA #
  EXPECT_EQ(emulated[0xA5].base, 3);  // LDA dp
  EXPECT_EQ(emulated[0xAD].base, 4);  // LDA abs
  EXPECT_EQ(emulated[0xBD].base, 4);  // LDA abs,X — no page crossed
  EXPECT_EQ(emulated[0xB1].base, 5);  // LDA (dp),Y
  EXPECT_EQ(emulated[0xA7].base, 6);  // LDA [dp]
  EXPECT_EQ(emulated[0xAF].base, 5);  // LDA long
  EXPECT_EQ(emulated[0x04].base, 5);  // TSB dp
  EXPECT_EQ(emulated[0x0A].base, 2);  // ASL A
  EXPECT_EQ(emulated[0x20].base, 6);  // JSR abs
  EXPECT_EQ(emulated[0x22].base, 8);  // JSL
  EXPECT_EQ(emulated[0x60].base, 6);  // RTS
  EXPECT_EQ(emulated[0x6B].base, 6);  // RTL
  EXPECT_EQ(emulated[0x48].base, 3);  // PHA
  EXPECT_EQ(emulated[0x68].base, 4);  // PLA
  EXPECT_EQ(emulated[0xF4].base, 5);  // PEA
  EXPECT_EQ(emulated[0xD4].base, 6);  // PEI
  EXPECT_EQ(emulated[0x62].base, 6);  // PER
  EXPECT_EQ(emulated[0x54].base, 7);  // MVN, per byte
  EXPECT_EQ(emulated[0xEB].base, 3);  // XBA
  EXPECT_EQ(emulated[0xC2].base, 3);  // REP
  EXPECT_EQ(emulated[0x00].base, 7);  // BRK in emulation mode
  EXPECT_EQ(emulated[0x40].base, 6);  // RTI in emulation mode

  const std::array<CycleCost, 256>& native8 = cpu65816CycleTable(false, true, true);
  EXPECT_EQ(native8[0x00].base, 8);   // BRK pushes the program bank too
  EXPECT_EQ(native8[0x40].base, 7);   // RTI pulls it back
  EXPECT_EQ(native8[0xBD].base, 4);   // LDA abs,X — eight-bit index, no crossing

  const std::array<CycleCost, 256>& native16 = cpu65816CycleTable(false, false, false);
  EXPECT_EQ(native16[0xA9].base, 3);  // LDA # — one more byte
  EXPECT_EQ(native16[0xA5].base, 4);  // LDA dp — one more byte
  EXPECT_EQ(native16[0xAD].base, 5);  // LDA abs
  EXPECT_EQ(native16[0xBD].base, 6);  // LDA abs,X — a wide index always pays, and a wide load
  EXPECT_EQ(cpu65816CycleTable(false, true, false)[0xBD].base, 5);  // the index alone
  EXPECT_EQ(native16[0x48].base, 4);  // PHA — two bytes
  EXPECT_EQ(native16[0xEA].base, 2);  // NOP — unchanged

  // Emulation mode ignores the width arguments.
  EXPECT_EQ(&cpu65816CycleTable(true, false, false), &emulated);
}

TEST(Cpu65816Disasm, ConditionalBranchesCarryBothCosts) {
  const std::array<CycleCost, 256>& costs = cpu65816CycleTable(false, true, true);
  constexpr std::array<std::uint8_t, 8> kBranches = {0x10, 0x30, 0x50, 0x70,
                                                     0x90, 0xB0, 0xD0, 0xF0};
  for (std::uint8_t branch : kBranches) {
    EXPECT_EQ(costs[branch].base, 2) << "branch $" << std::hex << int{branch};
    EXPECT_EQ(costs[branch].taken, 3) << "branch $" << std::hex << int{branch};
  }
  EXPECT_EQ(costs[0x80].base, 3);   // BRA is always taken
  EXPECT_EQ(costs[0x80].taken, 0);
  EXPECT_EQ(costs[0x82].base, 4);   // BRL
  EXPECT_EQ(costs[0x82].taken, 0);
  EXPECT_EQ(costs[0xEA].taken, 0);  // NOP has no condition
}

// Where a width is unknown, an instruction whose cost depends on it has no cost
// to print; one whose cost does not still has.
TEST(Cpu65816Disasm, TheCostIsUnknownWhereTheWidthIs) {
  const Cpu65816Mode unknown = Cpu65816Mode::nativeUnknown();
  EXPECT_FALSE(decodeBare(0xA5, unknown).cycles.known);  // LDA dp: 3 or 4
  EXPECT_TRUE(decodeBare(0xEA, unknown).cycles.known);   // NOP: 2 either way
  EXPECT_EQ(decodeBare(0xEA, unknown).cycles.base, 2);
  EXPECT_TRUE(decodeBare(0xE8, unknown).cycles.known);   // INX: 2 either way
  EXPECT_TRUE(decodeBare(0xA5, Cpu65816Mode::native(true, false)).cycles.known);

  const std::vector<std::uint8_t> image = {0xA5, 0x12, 0x60};  // LDA $12 / RTS
  const std::string text = render(traceFrom(image, unknown));
  EXPECT_NE(text.find("A5 12     ?"), std::string::npos) << text;
}

TEST(Cpu65816Disasm, RelativeTargetsStayInTheBank) {
  // BNE +4 at $C0:FFFC is a two-byte instruction: $C0:FFFE + 4 wraps to $C0:0002.
  const std::vector<std::uint8_t> forward = {0xD0, 0x04, 0x60};
  const std::optional<Instruction> wrapped =
      decodeAt(forward, 0xC0FFFC, 0xC0FFFC, Cpu65816Mode::native(true, true));
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(wrapped->target.value_or(0), 0xC00002u);
  EXPECT_EQ(wrapped->text, "BNE $C0:0002");
  EXPECT_EQ(wrapped->flow, Flow::Branch);

  // And backwards, through the sign.
  const std::vector<std::uint8_t> back = {0xD0, 0xFE};
  const std::optional<Instruction> loop =
      decodeAt(back, kAt, kAt, Cpu65816Mode::native(true, true));
  ASSERT_TRUE(loop.has_value());
  EXPECT_EQ(loop->target.value_or(0), kAt);

  // BRL carries a 16-bit displacement from the end of its three bytes.
  const std::vector<std::uint8_t> longBranch = {0x82, 0x00, 0x10};
  const std::optional<Instruction> brl =
      decodeAt(longBranch, kAt, kAt, Cpu65816Mode::native(true, true));
  ASSERT_TRUE(brl.has_value());
  EXPECT_EQ(brl->target.value_or(0), 0x009003u);
  EXPECT_EQ(brl->text, "BRL $00:9003");
  EXPECT_EQ(brl->flow, Flow::Jump);
}

// A long jump or call names its bank; an absolute one lands in the program
// bank; one through a pointer has no constant destination.
TEST(Cpu65816Disasm, JumpsAndCallsCarryTheBankTheyLandIn) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  const Address at = 0xC08000;

  const std::vector<std::uint8_t> jml = {0x5C, 0x34, 0x12, 0x7E};
  const std::optional<Instruction> longJump = decodeAt(jml, at, at, mode);
  ASSERT_TRUE(longJump.has_value());
  EXPECT_EQ(longJump->text, "JML $7E:1234");
  EXPECT_EQ(longJump->target.value_or(0), 0x7E1234u);
  EXPECT_EQ(longJump->flow, Flow::Jump);

  const std::vector<std::uint8_t> jsl = {0x22, 0x34, 0x12, 0x7E};
  const std::optional<Instruction> longCall = decodeAt(jsl, at, at, mode);
  ASSERT_TRUE(longCall.has_value());
  EXPECT_EQ(longCall->text, "JSL $7E:1234");
  EXPECT_EQ(longCall->target.value_or(0), 0x7E1234u);
  EXPECT_EQ(longCall->flow, Flow::Call);

  const std::vector<std::uint8_t> jmp = {0x4C, 0x34, 0x12};
  const std::optional<Instruction> absoluteJump = decodeAt(jmp, at, at, mode);
  ASSERT_TRUE(absoluteJump.has_value());
  EXPECT_EQ(absoluteJump->text, "JMP !$1234");
  EXPECT_EQ(absoluteJump->target.value_or(0), 0xC01234u);

  const std::vector<std::uint8_t> jsr = {0x20, 0x34, 0x12};
  EXPECT_EQ(decodeAt(jsr, at, at, mode)->target.value_or(0), 0xC01234u);

  const std::vector<std::uint8_t> indexed = {0xFC, 0x34, 0x12};
  const std::optional<Instruction> indexedCall = decodeAt(indexed, at, at, mode);
  ASSERT_TRUE(indexedCall.has_value());
  EXPECT_EQ(indexedCall->text, "JSR (!$1234,X)");
  EXPECT_FALSE(indexedCall->target.has_value());
  EXPECT_EQ(indexedCall->flow, Flow::Call);

  const std::vector<std::uint8_t> indirect = {0x6C, 0x34, 0x12};
  EXPECT_EQ(decodeAt(indirect, at, at, mode)->text, "JMP (!$1234)");
  EXPECT_FALSE(decodeAt(indirect, at, at, mode)->target.has_value());

  const std::vector<std::uint8_t> indirectLong = {0xDC, 0x34, 0x12};
  EXPECT_EQ(decodeAt(indirectLong, at, at, mode)->text, "JML [!$1234]");
  EXPECT_EQ(decodeAt(indirectLong, at, at, mode)->flow, Flow::Jump);
}

// Every form with a constant target carries the text that writes it as a
// symbol, behind the marker its mode needs: `!` for an absolute jump or call,
// `>` for a long one, nothing for a branch. A form whose operand is not its
// target — a pointer, a data address, PER's pushed address — carries none.
TEST(Cpu65816Disasm, TargetsCarryTheirSymbolicForm) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  auto symbolic = [&](std::vector<std::uint8_t> image) -> std::optional<SymbolicText> {
    const std::optional<Instruction> decoded = decodeAt(image, kAt, kAt, mode);
    EXPECT_TRUE(decoded.has_value());
    return decoded ? decoded->symbolic : std::nullopt;
  };
  const std::optional<SymbolicText> jump = symbolic({0x4C, 0x34, 0x12});  // JMP !$1234
  ASSERT_TRUE(jump.has_value());
  EXPECT_EQ(jump->before, "JMP !");
  EXPECT_EQ(jump->after, "");
  EXPECT_EQ(symbolic({0x20, 0x34, 0x12})->before, "JSR !");
  EXPECT_EQ(symbolic({0x5C, 0x34, 0x12, 0x7E})->before, "JML >");
  EXPECT_EQ(symbolic({0x22, 0x34, 0x12, 0x7E})->before, "JSL >");
  EXPECT_EQ(symbolic({0xD0, 0x10})->before, "BNE ");
  EXPECT_EQ(symbolic({0x80, 0x10})->before, "BRA ");
  EXPECT_EQ(symbolic({0x82, 0x00, 0x10})->before, "BRL ");
  EXPECT_FALSE(symbolic({0x7C, 0x34, 0x12}).has_value());        // JMP (!$1234,X)
  EXPECT_FALSE(symbolic({0x6C, 0x34, 0x12}).has_value());        // JMP (!$1234)
  EXPECT_FALSE(symbolic({0xDC, 0x34, 0x12}).has_value());        // JML [!$1234]
  EXPECT_FALSE(symbolic({0xAD, 0x34, 0x12}).has_value());        // LDA !$1234
  EXPECT_FALSE(symbolic({0xAF, 0x34, 0x12, 0x7E}).has_value());  // LDA $7E:1234
  EXPECT_FALSE(symbolic({0x62, 0x00, 0x10}).has_value());        // PER $00:9003
  EXPECT_FALSE(symbolic({0x00, 0x12}).has_value());              // BRK #$12
}

// The block moves store the destination bank first; the dialect writes the
// source first. Getting this backwards moves the bytes the wrong way.
TEST(Cpu65816Disasm, BlockMovesPrintTheSourceBankFirst) {
  const Cpu65816Mode mode = Cpu65816Mode::native(false, false);
  EXPECT_EQ(textOf({0x54, 0x7E, 0x00}, mode), "MVN $00,$7E");
  EXPECT_EQ(textOf({0x44, 0x7F, 0x7E}, mode), "MVP $7E,$7F");
}

// Every addressing form, printed as the dialect writes it.
TEST(Cpu65816Disasm, TheFormsPrintAsTheDialectWritesThem) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  EXPECT_EQ(textOf({0xA5, 0x12}, mode), "LDA $12");
  EXPECT_EQ(textOf({0xB5, 0x12}, mode), "LDA $12,X");
  EXPECT_EQ(textOf({0xB6, 0x12}, mode), "LDX $12,Y");
  EXPECT_EQ(textOf({0xB2, 0x12}, mode), "LDA ($12)");
  EXPECT_EQ(textOf({0xA1, 0x12}, mode), "LDA ($12,X)");
  EXPECT_EQ(textOf({0xB1, 0x12}, mode), "LDA ($12),Y");
  EXPECT_EQ(textOf({0xA7, 0x12}, mode), "LDA [$12]");
  EXPECT_EQ(textOf({0xB7, 0x12}, mode), "LDA [$12],Y");
  EXPECT_EQ(textOf({0xA3, 0x12}, mode), "LDA $12,S");
  EXPECT_EQ(textOf({0xB3, 0x12}, mode), "LDA ($12,S),Y");
  EXPECT_EQ(textOf({0xAD, 0x34, 0x12}, mode), "LDA !$1234");
  EXPECT_EQ(textOf({0xBD, 0x34, 0x12}, mode), "LDA !$1234,X");
  EXPECT_EQ(textOf({0xB9, 0x34, 0x12}, mode), "LDA !$1234,Y");
  EXPECT_EQ(textOf({0xAF, 0x34, 0x12, 0x7E}, mode), "LDA $7E:1234");
  EXPECT_EQ(textOf({0xBF, 0x34, 0x12, 0x7E}, mode), "LDA $7E:1234,X");
  EXPECT_EQ(textOf({0x0A}, mode), "ASL A");
  EXPECT_EQ(textOf({0xEA}, mode), "NOP");
  EXPECT_EQ(textOf({0xC2, 0x30}, mode), "REP #$30");
  EXPECT_EQ(textOf({0x00, 0x12}, mode), "BRK #$12");
  EXPECT_EQ(textOf({0x42, 0x12}, mode), "WDM #$12");
  EXPECT_EQ(textOf({0xF4, 0x34, 0x12}, mode), "PEA $1234");
  EXPECT_EQ(textOf({0xD4, 0x12}, mode), "PEI ($12)");
  EXPECT_EQ(textOf({0x62, 0x00, 0x10}, mode), "PER $00:9003");
  EXPECT_EQ(textOf({0x7C, 0x34, 0x12}, mode), "JMP (!$1234,X)");
}

// A register is named on an operand that lands in a bank that shows the
// registers, and nowhere else. An absolute operand is taken in bank zero; a
// direct-page operand is never named, since the direct register is unknown.
TEST(Cpu65816Disasm, RegistersAreNamedOnlyInTheBanksThatShowThem) {
  auto noteOf = [](std::vector<std::uint8_t> image) {
    image.push_back(0x60);  // RTS, so the trace ends inside the image
    const Listing listing = traceFrom(image, Cpu65816Mode::native(true, true));
    return listing.lines.front().instruction.note;
  };
  EXPECT_EQ(noteOf({0x8D, 0x00, 0x21}), "INIDISP");         // STA !$2100
  EXPECT_EQ(noteOf({0x8F, 0x00, 0x21, 0x7E}), "");          // STA $7E:2100 — work RAM
  EXPECT_EQ(noteOf({0x8F, 0x00, 0x21, 0x80}), "INIDISP");   // STA $80:2100
  EXPECT_EQ(noteOf({0x9D, 0x00, 0x43}), "DMAP0");           // STA !$4300,X
  EXPECT_EQ(noteOf({0x9D, 0x15, 0x43}), "DAS1L");           // STA !$4315,X
  EXPECT_EQ(noteOf({0x85, 0x00}), "");                      // STA $00 — the direct page
  EXPECT_EQ(noteOf({0xAD, 0x16, 0x40}), "JOYSER0/JOYOUT");  // LDA !$4016
  EXPECT_EQ(noteOf({0x8D, 0x00, 0x42}), "NMITIMEN");        // STA !$4200
  EXPECT_EQ(noteOf({0xAF, 0x10, 0x42, 0x3F}), "RDNMI");     // LDA $3F:4210
  EXPECT_EQ(noteOf({0xAF, 0x10, 0x42, 0x40}), "");          // LDA $40:4210 — ROM
  EXPECT_EQ(noteOf({0xA9, 0x00}), "");                      // LDA #$00 — a value

  EXPECT_EQ(cpu65816RegisterName(0x00213F), "STAT78");
  EXPECT_EQ(cpu65816RegisterName(0x00437F), "UNUSED7");
  EXPECT_EQ(cpu65816RegisterName(0x00437C), "");
  EXPECT_EQ(cpu65816RegisterName(0x002143), "APUIO3");
  EXPECT_EQ(cpu65816RegisterName(0x002180), "WMDATA");
  EXPECT_EQ(cpu65816RegisterName(0xBF2100), "INIDISP");
  EXPECT_EQ(cpu65816RegisterName(0xC02100), "");
  EXPECT_EQ(cpu65816RegisterName(0x000000), "");
}

// The listing carries the width directives an assembler needs, and only those:
// at the start of every region, and where the trace read an instruction under a
// width the instruction above did not leave. A REP or SEP says its own change,
// so nothing is restated after it.
TEST(Cpu65816Disasm, WidthDirectivesRideInTheListing) {
  const std::vector<std::uint8_t> image = {
      0xC2, 0x30,        // REP #$30
      0xA9, 0x34, 0x12,  // LDA #$1234
      0xE2, 0x20,        // SEP #$20
      0xA9, 0x12,        // LDA #$12
      0x60,              // RTS
  };
  const Listing listing = traceFrom(image, Cpu65816Mode::native(true, true));
  const std::vector<const Line*> lines = codeLines(listing);
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_EQ(lines[0]->directives, (std::vector<std::string>{"A8", "X8"}));
  for (std::size_t i = 1; i < lines.size(); ++i) {
    EXPECT_TRUE(lines[i]->directives.empty()) << "line " << i;
  }

  // Rendered, the directives sit indented like instructions, once each, before
  // the first instruction.
  const std::string text = render(listing);
  const std::size_t a8 = text.find("        A8\n");
  const std::size_t x8 = text.find("        X8\n");
  const std::size_t rep = text.find("REP #$30");
  ASSERT_NE(a8, std::string::npos) << text;
  ASSERT_NE(x8, std::string::npos) << text;
  EXPECT_LT(a8, rep);
  EXPECT_LT(x8, rep);
  EXPECT_EQ(text.find("        A8\n", a8 + 1), std::string::npos) << text;
  EXPECT_EQ(text.find("        A16\n"), std::string::npos) << text;
  EXPECT_EQ(text.find("        X16\n"), std::string::npos) << text;

  // After a run of data the widths are restated, since an assembler may start
  // from the label there.
  const std::vector<std::uint8_t> split = {
      0x60,        // $8000  RTS
      0xFF,        // $8001  never reached
      0xA9, 0x12,  // $8002  LDA #$12
      0x60,        // $8004  RTS
  };
  Request request;
  request.image = split;
  request.base = kAt;
  request.entries = {kAt, kAt + 2};
  request.context = contextOf(Cpu65816Mode::native(true, true));
  const Listing two = trace(cpu65816Backend(), request);
  ASSERT_EQ(codeLines(two).size(), 3u);
  EXPECT_EQ(codeLines(two)[0]->directives, (std::vector<std::string>{"A8", "X8"}));
  EXPECT_EQ(codeLines(two)[1]->directives, (std::vector<std::string>{"A8", "X8"}));
  EXPECT_TRUE(codeLines(two)[2]->directives.empty());
}

TEST(Cpu65816Disasm, TheBackendRefusesWhatItCannotRead) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  const std::array<std::uint8_t, 1> truncated = {0xAD};  // LDA !abs wants two more
  EXPECT_FALSE(decodeAt(truncated, kAt, kAt, mode).has_value());
  EXPECT_FALSE(decodeAt(truncated, kAt, kAt + 0x1000, mode).has_value());
  EXPECT_FALSE(decodeAt(truncated, kAt, kAt - 1, mode).has_value());
  const std::array<std::uint8_t, 3> wide = {0xA9, 0x00, 0x00};
  EXPECT_TRUE(decodeAt(wide, kAt, kAt, Cpu65816Mode::native(false, true)).has_value());
  const std::array<std::uint8_t, 2> narrow = {0xA9, 0x00};
  EXPECT_FALSE(decodeAt(narrow, kAt, kAt, Cpu65816Mode::native(false, true)).has_value());
  EXPECT_FALSE(cpu65816Backend()
                   .decode(wide, kAt, 0x01008000u, contextOf(mode))
                   .has_value());
}

TEST(Cpu65816Disasm, TheModeRoundTripsThroughTheContext) {
  const Cpu65816Backend& backend = cpu65816Backend();
  EXPECT_EQ(backend.name(), "65816");
  EXPECT_EQ(backend.addressBits(), 24u);

  EXPECT_EQ(backend.describe(contextOf(Cpu65816Mode::native(false, true))), "e=0 m=16 x=8");
  EXPECT_EQ(backend.describe(contextOf(Cpu65816Mode::nativeUnknown())), "e=0 m=? x=?");
  EXPECT_EQ(backend.describe(contextOf(Cpu65816Mode::reset())), "e=1 m=8 x=8");

  const Cpu65816Mode wide = Cpu65816Mode::native(false, false);
  EXPECT_EQ(modeOf(contextOf(wide)), wide);

  // Emulation mode packs the same whatever widths were asked for, and an
  // unknown width packs the same whatever value rode with it.
  Cpu65816Mode emulatedWide = wide;
  emulatedWide.emulation = true;
  EXPECT_EQ(contextOf(emulatedWide), contextOf(Cpu65816Mode::reset()));
  Cpu65816Mode unknownWide = Cpu65816Mode::nativeUnknown();
  unknownWide.accumulator8 = false;
  EXPECT_EQ(contextOf(unknownWide), contextOf(Cpu65816Mode::nativeUnknown()));

  // Two contexts differing only in the carry memory do not conflict; two
  // differing in a width do.
  Cpu65816Mode afterClc = Cpu65816Mode::native(true, true);
  afterClc.carryKnown = true;
  EXPECT_FALSE(backend.conflicts(contextOf(afterClc), contextOf(Cpu65816Mode::native(true, true))));
  EXPECT_TRUE(backend.conflicts(contextOf(Cpu65816Mode::native(true, true)),
                                contextOf(Cpu65816Mode::native(false, true))));
  EXPECT_TRUE(backend.conflicts(contextOf(Cpu65816Mode::native(true, true)),
                                contextOf(Cpu65816Mode::nativeUnknown())));
}

}  // namespace snaggletooth::disasm
