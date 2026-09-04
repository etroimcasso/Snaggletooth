// The 65816 assembler, held to `docs/65816-assembly.md`.
//
// The load-bearing case is EveryOpcodeRoundTripsUnderEveryMode: every opcode is
// decoded under each of the five settings of the mode flags and its text
// assembled back under the same mode, and the bytes must be the ones decoded.
// The two tools share one table and one mode tracker, so the case proves the
// assembler is the inverse of the disassembler for all 256 forms at every width.
// The other cases pin the dialect page: each addressing mode's bytes, the
// immediates and the widths, the regions and the directives that state a mode,
// the branches, the jumps, the block moves and the stack forms.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpu65816_asm.h"
#include "cpu65816_disasm.h"

namespace snaggletooth::assembler {
namespace {

using Bytes = std::vector<std::uint8_t>;
using disasm::Cpu65816Mode;

// One instruction at $00:8000 with both widths eight, assembled.
Bytes one(const std::string& line) {
  const Assembly assembly = assembleCpu65816(
      "        ORG $00:8000\n        A8\n        X8\n        " + line + "\n", "t.asm");
  EXPECT_TRUE(assembly.ok()) << line << ": "
                             << (assembly.errors.empty() ? "" : assembly.errors.front().message);
  return assembly.ranges.empty() ? Bytes{} : assembly.ranges.front().bytes;
}

Bytes bytesOf(const std::string& source) {
  const Assembly assembly = assembleCpu65816(source, "t.asm");
  EXPECT_TRUE(assembly.ok()) << (assembly.errors.empty() ? "" : assembly.errors.front().message);
  return assembly.ranges.empty() ? Bytes{} : assembly.ranges.front().bytes;
}

std::string errorOf(const std::string& source) {
  const Assembly assembly = assembleCpu65816(source, "t.asm");
  EXPECT_FALSE(assembly.ok()) << "assembled without error";
  return assembly.errors.empty() ? std::string() : assembly.errors.front().message;
}

}  // namespace

// §2: the operand syntax of every addressing mode, with the bytes each example
// on the page encodes to.
TEST(Cpu65816Asm, EveryAddressingModeAssemblesAsThePageShows) {
  EXPECT_EQ(one("NOP"), (Bytes{0xEA}));
  EXPECT_EQ(one("TXA"), (Bytes{0x8A}));
  EXPECT_EQ(one("XCE"), (Bytes{0xFB}));
  EXPECT_EQ(one("ASL A"), (Bytes{0x0A}));
  EXPECT_EQ(one("INC A"), (Bytes{0x1A}));
  EXPECT_EQ(one("DEC A"), (Bytes{0x3A}));
  EXPECT_EQ(one("LDA #$12"), (Bytes{0xA9, 0x12}));
  EXPECT_EQ(one("LDA $12"), (Bytes{0xA5, 0x12}));
  EXPECT_EQ(one("LDA $12,X"), (Bytes{0xB5, 0x12}));
  EXPECT_EQ(one("LDX $12,Y"), (Bytes{0xB6, 0x12}));
  EXPECT_EQ(one("LDA ($12)"), (Bytes{0xB2, 0x12}));
  EXPECT_EQ(one("LDA ($12,X)"), (Bytes{0xA1, 0x12}));
  EXPECT_EQ(one("LDA ($12),Y"), (Bytes{0xB1, 0x12}));
  EXPECT_EQ(one("LDA [$12]"), (Bytes{0xA7, 0x12}));
  EXPECT_EQ(one("LDA [$12],Y"), (Bytes{0xB7, 0x12}));
  EXPECT_EQ(one("LDA $12,S"), (Bytes{0xA3, 0x12}));
  EXPECT_EQ(one("LDA ($12,S),Y"), (Bytes{0xB3, 0x12}));
  EXPECT_EQ(one("LDA !$1234"), (Bytes{0xAD, 0x34, 0x12}));
  EXPECT_EQ(one("LDA !$1234,X"), (Bytes{0xBD, 0x34, 0x12}));
  EXPECT_EQ(one("LDA !$1234,Y"), (Bytes{0xB9, 0x34, 0x12}));
  EXPECT_EQ(one("LDA $7E:1234"), (Bytes{0xAF, 0x34, 0x12, 0x7E}));
  EXPECT_EQ(one("LDA $7E:1234,X"), (Bytes{0xBF, 0x34, 0x12, 0x7E}));
  EXPECT_EQ(one("JMP (!$1234)"), (Bytes{0x6C, 0x34, 0x12}));
  EXPECT_EQ(one("JML [!$1234]"), (Bytes{0xDC, 0x34, 0x12}));
  EXPECT_EQ(one("JMP (!$1234,X)"), (Bytes{0x7C, 0x34, 0x12}));
  EXPECT_EQ(one("JSR (!$1234,X)"), (Bytes{0xFC, 0x34, 0x12}));
  EXPECT_EQ(one("BNE $00:8002"), (Bytes{0xD0, 0x00}));
  EXPECT_EQ(one("BRL $00:8003"), (Bytes{0x82, 0x00, 0x00}));
  EXPECT_EQ(one("MVN $00,$7E"), (Bytes{0x54, 0x7E, 0x00}));
  EXPECT_EQ(one("STZ !$4200"), (Bytes{0x9C, 0x00, 0x42}));
  EXPECT_EQ(one("lda ($12 , s) , y"), (Bytes{0xB3, 0x12}));
}

// §2.1: direct, absolute and long are told apart by their marking, never by the
// size of a number; `!` with a bank is an error.
TEST(Cpu65816Asm, DirectAbsoluteAndLongAreMarkedNotSized) {
  EXPECT_EQ(one("LDA $0012"), (Bytes{0xA5, 0x12}));
  EXPECT_EQ(one("LDA !$0012"), (Bytes{0xAD, 0x12, 0x00}));
  EXPECT_NE(errorOf("        LDA $1234\n").find("does not fit in a byte"), std::string::npos);
  EXPECT_NE(errorOf("        LDA !$7E:1234\n").find("already says the operand is long"),
            std::string::npos);
  EXPECT_NE(errorOf("        LDA (!$7E:1234)\n").find("already says the operand is long"),
            std::string::npos);
  EXPECT_NE(errorOf("        STZ $7E:1234\n").find("does not take a long operand"),
            std::string::npos);

  const Bytes symbolic = bytesOf(
      "table   EQU $7E:1234\n"
      "        ORG $00:8000\n"
      "        LDA >table\n"
      "        LDA >table,X\n"
      "        JSL >table\n");
  EXPECT_EQ(symbolic, (Bytes{0xAF, 0x34, 0x12, 0x7E, 0xBF, 0x34, 0x12, 0x7E, 0x22, 0x34, 0x12, 0x7E}));
  // A 24-bit value where 16 are required is reported, not truncated to the
  // bank's offset.
  EXPECT_NE(errorOf("table   EQU $7E:1234\n        LDA !table\n").find("does not fit in 16 bits"),
            std::string::npos);
}

// §2.1: an absolute operand takes a 24-bit value in the instruction's own bank as
// its offset — the value a label in the same file has — and reports a value in
// any other bank.
TEST(Cpu65816Asm, AnAbsoluteOperandTakesALabelInItsOwnBank) {
  EXPECT_EQ(bytesOf("        ORG $01:8000\nloop:   NOP\n        JMP !loop\n"),
            (Bytes{0xEA, 0x4C, 0x00, 0x80}));
  EXPECT_EQ(bytesOf("        ORG $7E:2000\n        A8\n        X8\n"
                    "table:  LDA !table,X\n        JSR !table\n        JMP (!table,X)\n"),
            (Bytes{0xBD, 0x00, 0x20, 0x20, 0x00, 0x20, 0x7C, 0x00, 0x20}));
  EXPECT_EQ(bytesOf("        ORG $01:8000\n        JMP !$8000\n"), (Bytes{0x4C, 0x00, 0x80}))
      << "a 16-bit value is an offset in whatever bank";
  const std::string far = errorOf("far     EQU $02:8000\n        ORG $01:8000\n        JMP !far\n");
  EXPECT_NE(far.find("$028000"), std::string::npos);
  EXPECT_NE(far.find("not in bank $01"), std::string::npos);
  EXPECT_NE(errorOf("        ORG $01:8000\n        BNE $02:8000\n").find("another bank"),
            std::string::npos)
      << "a branch's rule is its own";
}

// §2.2: an immediate is as wide as the register it loads, and the width comes
// from the mode, never from the digits.
TEST(Cpu65816Asm, ImmediatesFollowTheRegisterWidths) {
  EXPECT_EQ(bytesOf("        A16\n        LDA #$12\n"), (Bytes{0xA9, 0x12, 0x00}));
  EXPECT_EQ(bytesOf("        A16\n        LDA #$0012\n"), (Bytes{0xA9, 0x12, 0x00}));
  EXPECT_EQ(bytesOf("        A8\n        LDA #$0012\n"), (Bytes{0xA9, 0x12}));
  EXPECT_NE(errorOf("        A8\n        LDA #$1234\n").find("does not fit in a byte"),
            std::string::npos);
  EXPECT_EQ(bytesOf("        X16\n        LDX #$1234\n        LDY #1\n        CPX #2\n        CPY #3\n"),
            (Bytes{0xA2, 0x34, 0x12, 0xA0, 0x01, 0x00, 0xE0, 0x02, 0x00, 0xC0, 0x03, 0x00}));
  EXPECT_EQ(bytesOf("        A16\n        X8\n        ADC #1\n        LDX #2\n"),
            (Bytes{0x69, 0x01, 0x00, 0xA2, 0x02}));

  // REP and SEP move the widths by the bits in their mask; their own operand,
  // WDM's, and the BRK/COP signature are always one byte.
  EXPECT_EQ(bytesOf("        A8\n        X8\n        REP #$30\n        LDA #1\n        LDX #2\n"
                    "        SEP #$20\n        LDA #3\n        LDX #4\n"),
            (Bytes{0xC2, 0x30, 0xA9, 0x01, 0x00, 0xA2, 0x02, 0x00, 0xE2, 0x20, 0xA9, 0x03,
                   0xA2, 0x04, 0x00}));
  EXPECT_EQ(bytesOf("        A16\n        REP #$30\n        SEP #$10\n        WDM #$12\n"
                    "        BRK #$12\n        COP #$12\n"),
            (Bytes{0xC2, 0x30, 0xE2, 0x10, 0x42, 0x12, 0x00, 0x12, 0x02, 0x12}));
}

// §2.2 and §3: a width is unknown at the start of every region and after PLP or
// RTI, and an immediate assembled under an unknown width is an error naming the
// width it needs.
TEST(Cpu65816Asm, AnImmediateUnderAnUnknownWidthIsAnError) {
  const std::string start = errorOf("        LDA #$12\n");
  EXPECT_NE(start.find("accumulator width"), std::string::npos);
  EXPECT_NE(start.find("A8 or A16"), std::string::npos);
  EXPECT_NE(errorOf("        LDX #$12\n").find("X8 or X16"), std::string::npos);
  EXPECT_NE(errorOf("        A8\n        PLP\n        LDA #$12\n").find("A8 or A16"),
            std::string::npos);
  EXPECT_NE(errorOf("        X8\n        RTI\n        LDX #$12\n").find("X8 or X16"),
            std::string::npos);
  // What needs no width still assembles under an unknown one.
  EXPECT_EQ(bytesOf("        LDA $12\n        REP #$20\n        LDA #1\n"),
            (Bytes{0xA5, 0x12, 0xC2, 0x20, 0xA9, 0x01, 0x00}));
}

// §2.2: XCE enters emulation mode after SEC and leaves it after CLC; with any
// other instruction before it the mode is kept. Emulation forces both widths to
// eight, and REP and SEP move nothing there.
TEST(Cpu65816Asm, XceFollowsTheCarryTheInstructionBeforeSet) {
  EXPECT_EQ(bytesOf("        A16\n        X16\n        SEC\n        XCE\n        LDA #$12\n"),
            (Bytes{0x38, 0xFB, 0xA9, 0x12}));
  EXPECT_EQ(bytesOf("        EMULATION\n        CLC\n        XCE\n        REP #$30\n        LDA #$1234\n"),
            (Bytes{0x18, 0xFB, 0xC2, 0x30, 0xA9, 0x34, 0x12}));
  EXPECT_EQ(bytesOf("        A16\n        NOP\n        XCE\n        LDA #$1234\n"),
            (Bytes{0xEA, 0xFB, 0xA9, 0x34, 0x12}));
  EXPECT_EQ(bytesOf("        A16\n        SEC\n        NOP\n        XCE\n        LDA #$1234\n"),
            (Bytes{0x38, 0xEA, 0xFB, 0xA9, 0x34, 0x12}))
      << "the carry memory spans one instruction";
  EXPECT_EQ(bytesOf("        EMULATION\n        REP #$30\n        LDA #$12\n        LDX #$34\n"),
            (Bytes{0xC2, 0x30, 0xA9, 0x12, 0xA2, 0x34}));
  EXPECT_EQ(bytesOf("        EMULATION\n        PLP\n        LDA #$12\n"), (Bytes{0x28, 0xA9, 0x12}))
      << "in emulation mode the widths stay forced after PLP";
}

// §3.2: NATIVE keeps the widths where emulation held them, and emulation holds
// them at eight through every instruction — so an RTI or a PLP under EMULATION,
// which would make the widths unknown in native mode, leaves the immediate after
// a NATIVE sized. This is the shape of an interrupt handler that ends in RTI with
// the code the trace reached in native mode right after it.
TEST(Cpu65816Asm, NativeAfterAnEmulationReturnKeepsTheWidthsEight) {
  EXPECT_EQ(bytesOf("        EMULATION\n        RTI\n        NATIVE\n        LDA #$09\n"),
            (Bytes{0x40, 0xA9, 0x09}));
  EXPECT_EQ(bytesOf("        EMULATION\n        PLP\n        NATIVE\n        LDX #$09\n"),
            (Bytes{0x28, 0xA2, 0x09}));
  EXPECT_NE(errorOf("        A8\n        RTI\n        LDA #$09\n").find("A8 or A16"),
            std::string::npos)
      << "in native mode the same RTI does make the width unknown";
}

// §3: a region begins at the start of the file, at every ORG and after every
// data directive, in native mode with both widths unknown; EMULATION says a
// region begins in emulation mode, NATIVE returns to native.
TEST(Cpu65816Asm, RegionsBeginNativeWithBothWidthsUnknown) {
  EXPECT_NE(errorOf("        A16\n        LDA #1\n        ORG $00:8100\n        LDA #1\n")
                .find("A8 or A16"),
            std::string::npos);
  EXPECT_NE(errorOf("        A16\n        LDA #1\n        DB 0\n        LDA #1\n").find("A8 or A16"),
            std::string::npos);
  EXPECT_NE(errorOf("        A16\n        LDA #1\n        DS 4\n        LDA #1\n").find("A8 or A16"),
            std::string::npos);
  EXPECT_EQ(bytesOf("        ORG $00:8000\n        EMULATION\n        LDA #1\n"
                    "        ORG $00:8002\n        REP #$20\n        LDA #1\n"),
            (Bytes{0xA9, 0x01, 0xC2, 0x20, 0xA9, 0x01, 0x00}))
      << "the region after ORG is native, so REP widens";
  EXPECT_EQ(bytesOf("        EMULATION\n        LDA #1\n        NATIVE\n        REP #$20\n        LDA #1\n"),
            (Bytes{0xA9, 0x01, 0xC2, 0x20, 0xA9, 0x01, 0x00}));
  EXPECT_EQ(bytesOf("        EMULATION\n        LDA #1\n        NATIVE\n        LDA #1\n"),
            (Bytes{0xA9, 0x01, 0xA9, 0x01}))
      << "NATIVE keeps the widths emulation held";
  EXPECT_NE(errorOf("        EMULATION\n        A16\n").find("emulation mode"), std::string::npos);
  EXPECT_NE(errorOf("        EMULATION\n        X16\n").find("emulation mode"), std::string::npos);
  EXPECT_NE(errorOf("        A8 1\n").find("takes no operand"), std::string::npos);
  EXPECT_NE(errorOf("        NATIVE now\n").find("takes no operand"), std::string::npos);
  EXPECT_NE(errorOf("A16:\n        NOP\n").find("cannot be a label"), std::string::npos);
  EXPECT_NE(errorOf("EMULATION:\n        NOP\n").find("cannot be a label"), std::string::npos);
}

// §2.2: the mask of REP and SEP decides the width of what follows, so it has to
// be known when the line is read.
TEST(Cpu65816Asm, RepAndSepMasksMustBeKnownWhenRead) {
  const Assembly assembly = assembleCpu65816("        REP #mask\nmask    EQU $30\n", "t.asm");
  ASSERT_FALSE(assembly.ok());
  EXPECT_EQ(assembly.errors.front().line, 1u);
  EXPECT_NE(assembly.errors.front().message.find("must be known when it is read"),
            std::string::npos);
  EXPECT_EQ(bytesOf("mask    EQU $30\n        REP #mask\n        LDA #1\n"),
            (Bytes{0xC2, 0x30, 0xA9, 0x01, 0x00}));
}

// §2.3: a branch takes the address it goes to, the displacement is from the end
// of the instruction, out of reach is reported, and a branch stays in its bank
// with the displacement wrapping the offset within it.
TEST(Cpu65816Asm, BranchesTakeAddressesAndStayInTheirBank) {
  EXPECT_EQ(bytesOf("        ORG $00:8000\nloop:   LDA !$2140\n        BNE loop\n"),
            (Bytes{0xAD, 0x40, 0x21, 0xD0, 0xFB}));
  const std::string far = errorOf("        ORG $00:8000\n        BNE far\n        DS 128\nfar:    RTS\n");
  EXPECT_NE(far.find("-128 to +127"), std::string::npos);
  EXPECT_NE(far.find("$00:8082"), std::string::npos);
  EXPECT_EQ(bytesOf("        ORG $00:8000\n        BRA far\n        DS 127\nfar:    RTS\n")[1], 0x7Fu);
  EXPECT_NE(errorOf("        ORG $00:8000\n        BNE $01:8000\n").find("another bank"),
            std::string::npos);
  EXPECT_EQ(bytesOf("        ORG $00:FFFE\n        BRA $00:0000\n"), (Bytes{0x80, 0x00}))
      << "the offset wraps within the bank";
  EXPECT_EQ(bytesOf("        ORG $7E:8000\n        BRL $7E:0000\n"), (Bytes{0x82, 0xFD, 0x7F}));
  EXPECT_EQ(bytesOf("        ORG $00:8000\n        BRL $00:8003\n        BRL $00:0000\n"),
            (Bytes{0x82, 0x00, 0x00, 0x82, 0xFA, 0x7F}));
}

// §2.4: the jumps and calls, absolute and long, direct and through a pointer.
TEST(Cpu65816Asm, TheJumpsAndCalls) {
  EXPECT_EQ(one("JMP !$1234"), (Bytes{0x4C, 0x34, 0x12}));
  EXPECT_EQ(one("JML $7E:1234"), (Bytes{0x5C, 0x34, 0x12, 0x7E}));
  EXPECT_EQ(one("JSR !$1234"), (Bytes{0x20, 0x34, 0x12}));
  EXPECT_EQ(one("JSL $7E:1234"), (Bytes{0x22, 0x34, 0x12, 0x7E}));
  EXPECT_NE(errorOf("        JML !$1234\n").find("no `!abs` form"), std::string::npos);
  EXPECT_NE(errorOf("        JMP $1234\n").find("no `dp` form"), std::string::npos);
  EXPECT_NE(errorOf("        JSR $7E:1234\n").find("no `long` form"), std::string::npos);
  EXPECT_NE(errorOf("        LDA (!$1234)\n").find("no `(!abs)` form"), std::string::npos);
  EXPECT_NE(errorOf("        NOP A\n").find("no `A` form"), std::string::npos);
  EXPECT_NE(errorOf("        INC\n").find("no `implied` form"), std::string::npos);
}

// §2.5: a block move is written source first and stored destination first.
TEST(Cpu65816Asm, BlockMovesWriteSourceFirstAndStoreDestinationFirst) {
  EXPECT_EQ(one("MVN $00,$7E"), (Bytes{0x54, 0x7E, 0x00}));
  EXPECT_EQ(one("MVP $7E,$00"), (Bytes{0x44, 0x00, 0x7E}));
  EXPECT_EQ(bytesOf("src     EQU $01\ndst     EQU $02\n        MVN src,dst\n"),
            (Bytes{0x54, 0x02, 0x01}));
  EXPECT_NE(errorOf("        MVN $100,$00\n").find("one byte"), std::string::npos);
  EXPECT_NE(errorOf("        MVN $00\n").find("no `dp` form"), std::string::npos);
}

// §2.6: the stack forms and the signatures.
TEST(Cpu65816Asm, TheStackFormsAndTheSignatures) {
  EXPECT_EQ(one("PEA $1234"), (Bytes{0xF4, 0x34, 0x12}));
  EXPECT_EQ(one("PEI ($12)"), (Bytes{0xD4, 0x12}));
  EXPECT_EQ(one("PER $00:8003"), (Bytes{0x62, 0x00, 0x00}));
  EXPECT_EQ(bytesOf("        ORG $00:8000\n        PER label\n        NOP\nlabel:  RTS\n"),
            (Bytes{0x62, 0x01, 0x00, 0xEA, 0x60}));
  EXPECT_EQ(one("BRK #$12"), (Bytes{0x00, 0x12}));
  EXPECT_EQ(one("COP #$12"), (Bytes{0x02, 0x12}));
  EXPECT_EQ(one("WDM #$12"), (Bytes{0x42, 0x12}));
  EXPECT_NE(errorOf("        BRK\n").find("no `implied` form"), std::string::npos);
  EXPECT_NE(errorOf("        PEA #$1234\n").find("no `#imm` form"), std::string::npos);
}

// The disassembler states the mode a region begins in, and only what the
// instruction above did not already leave: EMULATION at an emulation-mode start
// with no width directives under it, the widths at a native start, NATIVE where
// the instruction above left emulation.
TEST(Cpu65816Asm, TheDisassemblerStatesTheModeARegionBeginsIn) {
  const disasm::Cpu65816Backend& backend = disasm::cpu65816Backend();
  using disasm::contextOf;
  EXPECT_EQ(backend.directives(std::nullopt, contextOf(Cpu65816Mode::reset())),
            (std::vector<std::string>{"EMULATION"}));
  EXPECT_EQ(backend.directives(std::nullopt, contextOf(Cpu65816Mode::native(true, true))),
            (std::vector<std::string>{"A8", "X8"}));
  EXPECT_EQ(backend.directives(std::nullopt, contextOf(Cpu65816Mode::nativeUnknown())),
            (std::vector<std::string>{}));
  EXPECT_EQ(backend.directives(contextOf(Cpu65816Mode::reset()), contextOf(Cpu65816Mode::reset())),
            (std::vector<std::string>{}));
  EXPECT_EQ(backend.directives(contextOf(Cpu65816Mode::native(false, false)),
                               contextOf(Cpu65816Mode::reset())),
            (std::vector<std::string>{"EMULATION"}));
  EXPECT_EQ(backend.directives(contextOf(Cpu65816Mode::reset()),
                               contextOf(Cpu65816Mode::native(true, true))),
            (std::vector<std::string>{"NATIVE"}));
  EXPECT_EQ(backend.directives(contextOf(Cpu65816Mode::reset()),
                               contextOf(Cpu65816Mode::native(false, true))),
            (std::vector<std::string>{"NATIVE", "A16"}));
}

// The inverse of the disassembler for every opcode under every setting of the
// flags: decode bytes under a mode, assemble the text under the same mode at the
// same address, and the bytes come back.
TEST(Cpu65816Asm, EveryOpcodeRoundTripsUnderEveryMode) {
  constexpr disasm::Address kAt = 0x008000;
  struct Setting {
    Cpu65816Mode mode;
    const char* directives;
  };
  const std::array<Setting, 5> settings = {{
      {Cpu65816Mode::reset(), "        EMULATION\n"},
      {Cpu65816Mode::native(true, true), "        A8\n        X8\n"},
      {Cpu65816Mode::native(true, false), "        A8\n        X16\n"},
      {Cpu65816Mode::native(false, true), "        A16\n        X8\n"},
      {Cpu65816Mode::native(false, false), "        A16\n        X16\n"},
  }};
  const std::array<std::array<std::uint8_t, 3>, 2> patterns = {{{0x12, 0x34, 0x56}, {0xF0, 0x0F, 0x7E}}};
  for (const Setting& setting : settings) {
    for (const std::array<std::uint8_t, 3>& operands : patterns) {
      for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const std::array<std::uint8_t, 4> image = {static_cast<std::uint8_t>(opcode), operands[0],
                                                   operands[1], operands[2]};
        const std::optional<disasm::Instruction> decoded =
            disasm::decodeAt(image, kAt, kAt, setting.mode);
        ASSERT_TRUE(decoded.has_value()) << "opcode $" << std::hex << opcode;
        const Assembly assembly = assembleCpu65816(
            "        ORG $00:8000\n" + std::string(setting.directives) + "        " +
                decoded->text + "\n",
            "t.asm");
        ASSERT_TRUE(assembly.ok()) << decoded->text << " under " << setting.directives << ": "
                                   << (assembly.errors.empty() ? "" : assembly.errors.front().message);
        ASSERT_EQ(assembly.ranges.size(), 1u) << decoded->text;
        EXPECT_EQ(assembly.ranges.front().bytes, decoded->bytes)
            << decoded->text << " under " << setting.directives;
      }
    }
  }
}

// A listing the disassembler renders from a reset handler and an interrupt
// handler — the EMULATION region, the widths moving through XCE, REP and SEP, a
// call, data runs, a second entry with unknown widths — assembles back to the
// bytes it came from.
TEST(Cpu65816Asm, ARenderedListingAssemblesBackToItsBytes) {
  const Bytes program = {
      0x78,              // $8000  SEI                 (emulation)
      0x18,              // $8001  CLC
      0xFB,              // $8002  XCE                 (native, widths 8)
      0xC2, 0x30,        // $8003  REP #$30            (widths 16)
      0xA9, 0x34, 0x12,  // $8005  LDA #$1234
      0xE2, 0x20,        // $8008  SEP #$20            (a 8, x 16)
      0xA9, 0x12,        // $800A  LDA #$12
      0x20, 0x14, 0x80,  // $800C  JSR !$8014
      0x9C, 0x00, 0x21,  // $800F  STZ !$2100          ; INIDISP
      0xDB,              // $8012  STP
      0xFF,              // $8013  never reached
      0xA2, 0x34, 0x12,  // $8014  LDX #$1234          (x still 16)
      0x60,              // $8017  RTS
      'n', 'm', 'i',     // $8018  never reached
      0xC2, 0x20,        // $801B  nmi: REP #$20       (widths unknown, then a 16)
      0xA9, 0x00, 0x00,  // $801D  LDA #$0000
      0x40,              // $8020  RTI
  };
  disasm::Request request;
  request.image = program;
  request.base = 0x008000;
  request.entries = {0x008000, 0x00801B};
  request.entryContexts = {disasm::contextOf(Cpu65816Mode::reset()),
                           disasm::contextOf(Cpu65816Mode::nativeUnknown())};
  const disasm::Listing listing = disasm::trace(disasm::cpu65816Backend(), request);
  ASSERT_TRUE(listing.warnings.empty()) << listing.warnings.front();
  const std::string text = disasm::render(listing);
  EXPECT_NE(text.find("        EMULATION\n"), std::string::npos) << text;
  EXPECT_NE(text.find("INIDISP"), std::string::npos);
  EXPECT_NE(text.find("        JSR !sub_008014 "), std::string::npos) << text;
  EXPECT_NE(text.find("\nsub_008014:\n"), std::string::npos);

  const Assembly assembly = assembleCpu65816(text, "listing.asm");
  ASSERT_TRUE(assembly.ok()) << assembly.errors.front().line << ": "
                             << assembly.errors.front().message << "\n" << text;
  ASSERT_EQ(assembly.ranges.size(), 1u);
  EXPECT_EQ(assembly.ranges.front().start, 0x008000u);
  EXPECT_EQ(assembly.ranges.front().bytes, program);
}

}  // namespace snaggletooth::assembler
