// The SPC700 assembler, held to `docs/spc700-assembly.md`.
//
// The load-bearing case is EveryOpcodeRoundTripsThroughItsOwnText: every opcode
// is decoded by the disassembler and its text assembled back, under two operand
// patterns, and the bytes must be the ones decoded. The two tools share one
// table, so the case proves the assembler's syntax is the inverse of the
// disassembler's rendering for all 256 forms. The other cases pin the dialect
// page: each addressing mode's bytes, branch targets, bit operands, the call
// forms, and the two-operand order.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "spc700_asm.h"
#include "spc700_disasm.h"

namespace snaggletooth::assembler {
namespace {

using Bytes = std::vector<std::uint8_t>;

// One instruction at $0400, assembled.
Bytes one(const std::string& line) {
  const Assembly assembly = assembleSpc700("        ORG $0400\n        " + line + "\n", "t.asm");
  EXPECT_TRUE(assembly.ok()) << line << ": "
                             << (assembly.errors.empty() ? "" : assembly.errors.front().message);
  return assembly.ranges.empty() ? Bytes{} : assembly.ranges.front().bytes;
}

std::string errorOf(const std::string& source) {
  const Assembly assembly = assembleSpc700(source, "t.asm");
  EXPECT_FALSE(assembly.ok()) << "assembled without error";
  return assembly.errors.empty() ? std::string() : assembly.errors.front().message;
}

}  // namespace

// §2: the operand syntax of every addressing mode, with the bytes each example
// on the page encodes to.
TEST(Spc700Asm, EveryAddressingModeAssemblesAsThePageShows) {
  EXPECT_EQ(one("NOP"), (Bytes{0x00}));
  EXPECT_EQ(one("MOV A,X"), (Bytes{0x7D}));
  EXPECT_EQ(one("MUL YA"), (Bytes{0xCF}));
  EXPECT_EQ(one("MOV A,#$01"), (Bytes{0xE8, 0x01}));
  EXPECT_EQ(one("MOV A,$10"), (Bytes{0xE4, 0x10}));
  EXPECT_EQ(one("MOV A,$10+X"), (Bytes{0xF4, 0x10}));
  EXPECT_EQ(one("MOV X,$10+Y"), (Bytes{0xF9, 0x10}));
  EXPECT_EQ(one("MOV A,!$0A2B"), (Bytes{0xE5, 0x2B, 0x0A}));
  EXPECT_EQ(one("MOV A,!$0B0B+X"), (Bytes{0xF5, 0x0B, 0x0B}));
  EXPECT_EQ(one("MOV A,!$0B0B+Y"), (Bytes{0xF6, 0x0B, 0x0B}));
  EXPECT_EQ(one("MOV A,(X)"), (Bytes{0xE6}));
  EXPECT_EQ(one("MOV A,(X)+"), (Bytes{0xBF}));
  EXPECT_EQ(one("MOV A,[$10+X]"), (Bytes{0xE7, 0x10}));
  EXPECT_EQ(one("MOV A,[$10]+Y"), (Bytes{0xF7, 0x10}));
  EXPECT_EQ(one("ADC (X),(Y)"), (Bytes{0x99}));
  EXPECT_EQ(one("MOV $12,$34"), (Bytes{0xFA, 0x34, 0x12}));
  EXPECT_EQ(one("MOV $12,#$34"), (Bytes{0x8F, 0x34, 0x12}));
  EXPECT_EQ(one("MOV1 C,!$1234.5"), (Bytes{0xAA, 0x34, 0xB2}));
  EXPECT_EQ(one("AND1 C,/!$1234.5"), (Bytes{0x6A, 0x34, 0xB2}));
  EXPECT_EQ(one("BNE $0400"), (Bytes{0xD0, 0xFE}));
  EXPECT_EQ(one("BBS $10.3,$0400"), (Bytes{0x63, 0x10, 0xFD}));
  EXPECT_EQ(one("DBNZ Y,$0400"), (Bytes{0xFE, 0xFE}));
  EXPECT_EQ(one("CBNE $10+X,$0400"), (Bytes{0xDE, 0x10, 0xFD}));
  EXPECT_EQ(one("MOVW YA,$10"), (Bytes{0xBA, 0x10}));
  EXPECT_EQ(one("JMP [!$1234+X]"), (Bytes{0x1F, 0x34, 0x12}));
  EXPECT_EQ(one("CALL !$1234"), (Bytes{0x3F, 0x34, 0x12}));
  EXPECT_EQ(one("PUSH PSW"), (Bytes{0x0D}));
  EXPECT_EQ(one("MOV SP,X"), (Bytes{0xBD}));
}

// §2: the `!` on an absolute operand is required; a direct-page operand is one
// byte however many digits were written.
TEST(Spc700Asm, TheAbsoluteMarkerIsRequired) {
  EXPECT_EQ(one("MOV A,$0010"), (Bytes{0xE4, 0x10}));
  EXPECT_EQ(one("MOV A,!$10"), (Bytes{0xE5, 0x10, 0x00}));
  EXPECT_NE(errorOf("        MOV A,$1234\n").find("does not fit in a byte"), std::string::npos);
}

// §2.1: a branch takes the address it goes to; the displacement is measured from
// the end of the instruction; out of reach is reported, not truncated.
TEST(Spc700Asm, BranchTargetsAreAddressesNotDisplacements) {
  const Assembly loop = assembleSpc700(
      "        ORG $0400\n"
      "loop:   MOV A,(X)+\n"
      "        BNE loop        ; not BNE -3\n",
      "t.asm");
  ASSERT_TRUE(loop.ok());
  EXPECT_EQ(loop.ranges.front().bytes, (Bytes{0xBF, 0xD0, 0xFD}));

  // Forward to the edge of reach, and one past it.
  const Assembly edge = assembleSpc700(
      "        ORG $0400\n        BRA far\n        DS 127\nfar:    RET\n", "t.asm");
  ASSERT_TRUE(edge.ok());
  EXPECT_EQ(edge.ranges.front().bytes[1], 0x7Fu);
  const std::string past = errorOf("        ORG $0400\n        BRA far\n        DS 128\nfar:    RET\n");
  EXPECT_NE(past.find("-128 to +127"), std::string::npos);
  EXPECT_NE(past.find("128 bytes"), std::string::npos);
  EXPECT_EQ(one("BRA $0382"), (Bytes{0x2F, 0x80}));  // -128 exactly
}

// §2.2: an absolute bit operand is a 13-bit address and a 3-bit index; the
// address must fit `$0000`–`$1FFF` and the index 0–7. The bit index is what
// follows the last `.`, so a name with a `.` in it still reads.
TEST(Spc700Asm, BitOperandsPackThirteenBitsAndAnIndex) {
  EXPECT_EQ(one("SET1 $10.0"), (Bytes{0x02, 0x10}));
  EXPECT_EQ(one("CLR1 $10.7"), (Bytes{0xF2, 0x10}));
  EXPECT_EQ(one("NOT1 !$1FFF.7"), (Bytes{0xEA, 0xFF, 0xFF}));
  EXPECT_EQ(one("MOV1 !$0000.0,C"), (Bytes{0xCA, 0x00, 0x00}));
  EXPECT_NE(errorOf("        NOT1 !$2000.0\n").find("13 bits"), std::string::npos);
  EXPECT_NE(errorOf("        NOT1 !$1000.8\n").find("0 to 7"), std::string::npos);

  const Assembly dotted = assembleSpc700(
      "flags.io EQU $10\n        SET1 flags.io.3\n        BBC flags.io.3,*\n", "t.asm");
  ASSERT_TRUE(dotted.ok()) << (dotted.errors.empty() ? "" : dotted.errors.front().message);
  EXPECT_EQ(dotted.ranges.front().bytes, (Bytes{0x62, 0x10, 0x73, 0x10, 0xFD}));
}

// §2.3: TCALL's operand is the entry number and rides in the opcode; PCALL takes
// the one-byte offset into page $FF; BRK takes nothing.
TEST(Spc700Asm, TheCallFormsCarryTheirDestinationInTheOpcode) {
  EXPECT_EQ(one("TCALL 0"), (Bytes{0x01}));
  EXPECT_EQ(one("TCALL 15"), (Bytes{0xF1}));
  EXPECT_EQ(one("tcall 7"), (Bytes{0x71}));
  EXPECT_NE(errorOf("        TCALL 16\n").find("not a form"), std::string::npos);
  EXPECT_EQ(one("PCALL $12"), (Bytes{0x4F, 0x12}));
  EXPECT_NE(errorOf("        PCALL $FF12\n").find("does not fit in a byte"), std::string::npos);
  EXPECT_EQ(one("BRK"), (Bytes{0x0F}));
  EXPECT_NE(errorOf("        BRK #$12\n").find("not a form"), std::string::npos);

  // And the disassembler writes PCALL the same way, naming the destination in
  // the comment rather than the operand.
  const std::array<std::uint8_t, 2> pcall = {0x4F, 0x12};
  const std::optional<disasm::Instruction> decoded = disasm::decodeAt(pcall, 0x0400, 0x0400);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->text, "PCALL $12");
  EXPECT_EQ(decoded->note, "$FF12");
  EXPECT_EQ(decoded->target.value_or(0), 0xFF12u);
}

// §2.4: `MOV $12,$34` moves $34 into $12 — destination first in the text, source
// first in the bytes; the whole two-operand family behaves the same.
TEST(Spc700Asm, TwoOperandDirectPageOrderIsDestinationFirst) {
  EXPECT_EQ(one("MOV $12,$34"), (Bytes{0xFA, 0x34, 0x12}));
  EXPECT_EQ(one("ADC $12,$34"), (Bytes{0x89, 0x34, 0x12}));
  EXPECT_EQ(one("SBC $12,$34"), (Bytes{0xA9, 0x34, 0x12}));
  EXPECT_EQ(one("CMP $12,$34"), (Bytes{0x69, 0x34, 0x12}));
  EXPECT_EQ(one("AND $12,$34"), (Bytes{0x29, 0x34, 0x12}));
  EXPECT_EQ(one("OR $12,$34"), (Bytes{0x09, 0x34, 0x12}));
  EXPECT_EQ(one("EOR $12,$34"), (Bytes{0x49, 0x34, 0x12}));
  EXPECT_EQ(one("OR $12,#$34"), (Bytes{0x18, 0x34, 0x12}));
  EXPECT_EQ(one("CMP $12,#$34"), (Bytes{0x78, 0x34, 0x12}));
}

// A symbol stands anywhere a number does, including inside an indexed operand,
// where `+X` is the suffix and not a term.
TEST(Spc700Asm, SymbolsStandInEveryOperand) {
  const Assembly assembly = assembleSpc700(
      "table   EQU $0B0B\n"
      "port    EQU $F4\n"
      "        ORG $0400\n"
      "        MOV A,!table+X\n"
      "        MOV A,!table+1+Y\n"
      "        MOV port,#'A'\n"
      "        MOV A,[port+X]\n"
      "        CMP A,#table-$0B00\n",
      "t.asm");
  ASSERT_TRUE(assembly.ok()) << assembly.errors.front().message;
  EXPECT_EQ(assembly.ranges.front().bytes,
            (Bytes{0xF5, 0x0B, 0x0B, 0xF6, 0x0C, 0x0B, 0x8F, 0x41, 0xF4, 0xE7, 0xF4, 0x68, 0x0B}));
}

TEST(Spc700Asm, WhatIsNotAnInstructionOrAFormIsReported) {
  EXPECT_NE(errorOf("        LDA #$12\n").find("not an SPC700 instruction"), std::string::npos);
  EXPECT_NE(errorOf("        MOV A,A\n").find("not a form of MOV"), std::string::npos);
  EXPECT_NE(errorOf("        MOV A,$10,X\n").find("not a form of MOV"), std::string::npos);
  EXPECT_NE(errorOf("        MOV A,$10+Z\n").find("`Z` is not defined"), std::string::npos);
  EXPECT_NE(errorOf("        MOV A,#\n").find("not a form of MOV"), std::string::npos);
  EXPECT_NE(errorOf("mov:    NOP\n").find("cannot be a label"), std::string::npos);
  EXPECT_NE(errorOf("PSW:    NOP\n").find("cannot be a label"), std::string::npos);
  EXPECT_NE(errorOf("YA:     NOP\n").find("cannot be a label"), std::string::npos);
}

// The inverse of the disassembler for every opcode: decode bytes, assemble the
// text at the same address, and the bytes come back.
TEST(Spc700Asm, EveryOpcodeRoundTripsThroughItsOwnText) {
  constexpr std::uint16_t kAt = 0x2000;
  const std::array<std::array<std::uint8_t, 2>, 2> patterns = {{{0x12, 0x34}, {0xF0, 0x0F}}};
  for (const std::array<std::uint8_t, 2>& operands : patterns) {
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
      const std::array<std::uint8_t, 3> image = {static_cast<std::uint8_t>(opcode), operands[0],
                                                 operands[1]};
      const std::optional<disasm::Instruction> decoded = disasm::decodeAt(image, kAt, kAt);
      ASSERT_TRUE(decoded.has_value()) << "opcode $" << std::hex << opcode;
      const Assembly assembly =
          assembleSpc700("        ORG $2000\n        " + decoded->text + "\n", "t.asm");
      ASSERT_TRUE(assembly.ok()) << decoded->text << ": "
                                 << (assembly.errors.empty() ? "" : assembly.errors.front().message);
      ASSERT_EQ(assembly.ranges.size(), 1u) << decoded->text;
      EXPECT_EQ(assembly.ranges.front().bytes, decoded->bytes) << decoded->text;
    }
  }
}

// A listing the disassembler renders — labels, directives, data runs, register
// and patched-byte annotations, warnings — assembles back to the bytes it came
// from, with everything that is not an instruction riding in comments.
TEST(Spc700Asm, ARenderedListingAssemblesBackToItsBytes) {
  const Bytes program = {
      0xCD, 0xEF,        // $0400  MOV X,#$EF
      0xBD,              // $0402  MOV SP,X
      0xE8, 0x00,        // $0403  MOV A,#$00
      0xC4, 0xF2,        // $0405  MOV $F2,A          ; DSPADDR
      0x3F, 0x10, 0x04,  // $0407  CALL !$0410
      0x6E, 0x10, 0xFA,  // $040A  DBNZ $10,$0407
      0x4F, 0x20,        // $040D  PCALL $20
      0xFF,              // $040F  STOP
      0xE4, 0xF4,        // $0410  MOV A,$F4          ; CPUIO0
      0x6F,              // $0412  RET
      'h', 'i', 0x00,    // $0413  never reached
      0x0F,              // $0416  never reached
  };
  Bytes prior = program;
  prior[1] = 0xCF;  // MOV X,#$CF before the program patched itself
  disasm::DisasmRequest request;
  request.image = program;
  request.base = 0x0400;
  request.priorImage = prior;
  const disasm::Listing listing = disasm::trace(request);
  const std::string text = disasm::render(listing);
  EXPECT_NE(text.find("PATCHED"), std::string::npos);
  EXPECT_NE(text.find("DSPADDR"), std::string::npos);
  EXPECT_NE(text.find("sub_0410:"), std::string::npos);
  // The call and the branch name the labels the listing defines; PCALL keeps
  // its byte, since its operand is not the address it reaches.
  EXPECT_NE(text.find("        CALL !sub_0410 "), std::string::npos) << text;
  EXPECT_NE(text.find("        DBNZ $10,loc_0407 "), std::string::npos) << text;
  EXPECT_NE(text.find("        PCALL $20 "), std::string::npos) << text;

  const Assembly assembly = assembleSpc700(text, "listing.asm");
  ASSERT_TRUE(assembly.ok()) << assembly.errors.front().line << ": "
                             << assembly.errors.front().message << "\n" << text;
  ASSERT_EQ(assembly.ranges.size(), 1u);
  EXPECT_EQ(assembly.ranges.front().start, 0x0400u);
  EXPECT_EQ(assembly.ranges.front().bytes, program);
}

}  // namespace snaggletooth::assembler
