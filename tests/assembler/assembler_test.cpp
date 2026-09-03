// The assembler's common layer, held to `docs/assembly-lexicon.md`.
//
// Every case here pins a sentence of that page: the line form, comments, case,
// column one, the three number forms, character and string literals, labels and
// forward references, EQU, the expression grammar, ORG and the data directives,
// and the diagnostics. The SPC700 dialect stands in for the 16-bit address width
// and the 65816 dialect for the 24-bit one; nothing here depends on either
// instruction set beyond one or two instructions used as bytes.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816_asm.h"
#include "spc700_asm.h"

namespace snaggletooth::assembler {
namespace {

using Bytes = std::vector<std::uint8_t>;

// The bytes one SPC700 source emits, which must be one range and error-free.
Bytes spc(const std::string& source) {
  const Assembly assembly = assembleSpc700(source, "t.asm");
  EXPECT_TRUE(assembly.ok()) << (assembly.errors.empty() ? "" : assembly.errors.front().message);
  EXPECT_EQ(assembly.ranges.size(), 1u);
  return assembly.ranges.empty() ? Bytes{} : assembly.ranges.front().bytes;
}

// The first error one SPC700 source raises.
Diagnostic firstError(const std::string& source) {
  const Assembly assembly = assembleSpc700(source, "t.asm");
  EXPECT_FALSE(assembly.ok()) << "assembled without error";
  return assembly.errors.empty() ? Diagnostic{} : assembly.errors.front();
}

}  // namespace

// §2: a line is `[label:] [instruction | directive] [; comment]`, every element
// optional, and a blank line is legal.
TEST(Assembler, ALineIsALabelAnInstructionAndAComment) {
  const Assembly assembly = assembleSpc700(
      "        ORG $0400\n"
      "\n"
      "; a comment on its own\n"
      "loop:   MOV A,(X)+       ; all three\n"
      "        BNE loop\n"
      "done:\n",
      "t.asm");
  ASSERT_TRUE(assembly.ok());
  ASSERT_EQ(assembly.ranges.size(), 1u);
  EXPECT_EQ(assembly.ranges.front().start, 0x0400u);
  EXPECT_EQ(assembly.ranges.front().bytes, (Bytes{0xBF, 0xD0, 0xFD}));
  EXPECT_EQ(assembly.symbols.at("loop"), 0x0400u);
  EXPECT_EQ(assembly.symbols.at("done"), 0x0403u);
}

// §2.1: a semicolon inside a literal is an ordinary character.
TEST(Assembler, ASemicolonInsideALiteralIsNotAComment) {
  EXPECT_EQ(spc("        DB \"a;b\"     ; the comment starts here\n"), (Bytes{0x61, 0x3B, 0x62}));
  EXPECT_EQ(spc("        DB ';'\n"), (Bytes{0x3B}));
}

// §2.2: mnemonics, register names and directives are case-insensitive; labels
// are not.
TEST(Assembler, MnemonicsAreCaseInsensitiveAndLabelsAreNot) {
  EXPECT_EQ(spc("        mov a,$10\n"), spc("        MOV A,$10\n"));
  EXPECT_EQ(spc("        Mov A,$10\n"), (Bytes{0xE4, 0x10}));
  EXPECT_EQ(spc("        org $0400\n        db 1\n"), (Bytes{0x01}));

  const Assembly two = assembleSpc700("Loop:\nloop:\n        NOP\n", "t.asm");
  ASSERT_TRUE(two.ok());
  EXPECT_EQ(two.symbols.count("Loop"), 1u);
  EXPECT_EQ(two.symbols.count("loop"), 1u);
}

// §2.3 and §4.1: a label begins in column 1 with a colon; anything indented is an
// instruction; a name spelling a mnemonic, register or directive is rejected.
TEST(Assembler, ALabelBeginsInColumnOneWithAColon) {
  EXPECT_NE(firstError("loop\n        NOP\n").message.find("colon"), std::string::npos);
  EXPECT_NE(firstError("        loop:\n").message.find("indented"), std::string::npos);
  EXPECT_NE(firstError("mov:\n        NOP\n").message.find("cannot be a label"),
            std::string::npos);
  EXPECT_NE(firstError("X:\n        NOP\n").message.find("cannot be a label"),
            std::string::npos);
  EXPECT_NE(firstError("org:\n        NOP\n").message.find("cannot be a label"),
            std::string::npos);
  EXPECT_NE(firstError("1st:\n        NOP\n").message.find("column 1"), std::string::npos);
}

// §3: `$` is hexadecimal, `%` binary, bare digits decimal; `0x` is not accepted;
// a number's written width does not affect encoding.
TEST(Assembler, ThreeNumberFormsAndNoZeroX) {
  EXPECT_EQ(spc("        DB $1F,%10110001,31\n"), (Bytes{0x1F, 0xB1, 0x1F}));
  EXPECT_EQ(spc("        DB $0A\n"), spc("        DB $A\n"));
  EXPECT_EQ(spc("        DW $0A2B,2603\n"), (Bytes{0x2B, 0x0A, 0x2B, 0x0A}));
  EXPECT_NE(firstError("        DB 0x1F\n").message.find("$"), std::string::npos);
  EXPECT_NE(firstError("        DB $\n").message.find("hexadecimal digits"), std::string::npos);
}

// §3.1: a character literal is its ASCII code, and the seven escapes are read.
TEST(Assembler, CharacterLiteralsAndTheirEscapes) {
  EXPECT_EQ(spc("        DB 'A','a','0'\n"), (Bytes{0x41, 0x61, 0x30}));
  EXPECT_EQ(spc("        DB '\\\\','\\'','\\\"','\\n','\\r','\\t','\\0'\n"),
            (Bytes{0x5C, 0x27, 0x22, 0x0A, 0x0D, 0x09, 0x00}));
  EXPECT_EQ(spc("        DB \"a\\\"b\",0\n"), (Bytes{0x61, 0x22, 0x62, 0x00}));
  EXPECT_NE(firstError("        DB '\\q'\n").message.find("not an escape"), std::string::npos);
  EXPECT_NE(firstError("        DB 'AB'\n").message.find("one character"), std::string::npos);
  EXPECT_NE(firstError("        DB \"open\n").message.find("not closed"), std::string::npos);
}

// §4.1: a label takes the address of the byte that follows it; two labels on
// consecutive lines are one address; forward references resolve on the second
// pass; redefinition is an error.
TEST(Assembler, LabelsForwardReferencesAndRedefinition) {
  const Assembly assembly = assembleSpc700(
      "        ORG $0400\n"
      "        JMP !end\n"
      "first:\n"
      "second:\n"
      "        NOP\n"
      "end:    RET\n",
      "t.asm");
  ASSERT_TRUE(assembly.ok()) << assembly.errors.front().message;
  ASSERT_EQ(assembly.ranges.size(), 1u);
  EXPECT_EQ(assembly.ranges.front().bytes, (Bytes{0x5F, 0x04, 0x04, 0x00, 0x6F}));
  EXPECT_EQ(assembly.symbols.at("first"), assembly.symbols.at("second"));
  EXPECT_EQ(assembly.symbols.at("end"), 0x0404u);

  const Diagnostic twice = firstError("dup:\n        NOP\ndup:\n        NOP\n");
  EXPECT_EQ(twice.line, 3u);
  EXPECT_NE(twice.message.find("already defined"), std::string::npos);
  EXPECT_NE(firstError("        JMP !nowhere\n").message.find("not defined"), std::string::npos);
}

// §4.2: EQU binds a name to a value, in column 1 with no colon, and may not refer
// forward.
TEST(Assembler, EquBindsAValueAndMayNotReferForward) {
  EXPECT_EQ(spc("T0OUT   EQU $FD\n        MOV X,T0OUT\n"), (Bytes{0xF8, 0xFD}));
  EXPECT_EQ(spc("base    EQU $10\nnext    EQU base+1\n        DB next\n"), (Bytes{0x11}));
  const Diagnostic forward = firstError("early   EQU late\nlate    EQU 1\n");
  EXPECT_EQ(forward.line, 1u);
  EXPECT_NE(forward.message.find("must be known when it is read"), std::string::npos);
  EXPECT_NE(firstError("A       EQU 1\n").message.find("cannot be a name"), std::string::npos);
  EXPECT_NE(firstError("        EQU 1\n").message.find("column 1"), std::string::npos);
}

// §4.3: terms joined by `+` and `-`, left to right, no other operator, no
// precedence, no parenthesis; `*` is the address of the current line; arithmetic
// wraps at the address width, and a value must fit where it is used.
TEST(Assembler, ExpressionsAreLeftToRightPlusAndMinus) {
  EXPECT_EQ(spc("        DB 1+2-1\n"), (Bytes{0x02}));
  EXPECT_EQ(spc("        DB 'A'+1\n"), (Bytes{0x42}));
  EXPECT_EQ(spc("        ORG $0400\n        DW *\n        DW *+2\n"),
            (Bytes{0x00, 0x04, 0x04, 0x04}));
  EXPECT_EQ(spc("        DW 3-5\n"), (Bytes{0xFE, 0xFF})) << "wraps at 16 bits";
  EXPECT_NE(firstError("        DB 3-5\n").message.find("does not fit in a byte"),
            std::string::npos);
  EXPECT_NE(firstError("        DB 2*3\n").message.find("unexpected `*`"), std::string::npos);
  EXPECT_NE(firstError("        DB (1+2)\n").message.find("expected"), std::string::npos);
  EXPECT_NE(firstError("        DB 1+\n").message.find("expected"), std::string::npos);

  const Assembly named = assembleSpc700(
      "        ORG $0400\n"
      "start:  MOV A,#end-start\n"
      "        MOV !template+13,Y\n"
      "end:\n"
      "template: DS 16\n",
      "t.asm");
  ASSERT_TRUE(named.ok());
  EXPECT_EQ(named.ranges.front().bytes[1], 5u);
  EXPECT_EQ(named.ranges.front().bytes[3], 0x12u);  // $0405 + 13 = $0412
  EXPECT_EQ(named.ranges.front().bytes[4], 0x04u);
}

// §5.1: ORG sets the address; a file without one starts at zero; ORG may move
// forward leaving a gap, and the output names the ranges written; it may not
// move back over bytes already emitted.
TEST(Assembler, OrgPlacesBytesAndNeverOverwrites) {
  EXPECT_EQ(assembleSpc700("        DB 1\n", "t.asm").ranges.front().start, 0u);

  const Assembly two = assembleSpc700(
      "        ORG $0400\n"
      "        DB 1,2\n"
      "        ORG $0500\n"
      "        DB 3\n",
      "t.asm");
  ASSERT_TRUE(two.ok());
  ASSERT_EQ(two.ranges.size(), 2u);
  EXPECT_EQ(two.ranges[0].start, 0x0400u);
  EXPECT_EQ(two.ranges[0].bytes, (Bytes{1, 2}));
  EXPECT_EQ(two.ranges[1].start, 0x0500u);
  EXPECT_EQ(two.ranges[1].bytes, (Bytes{3}));

  // An ORG to exactly the next address continues the range.
  const Assembly joined = assembleSpc700(
      "        ORG $0400\n        DB 1\n        ORG $0401\n        DB 2\n", "t.asm");
  ASSERT_TRUE(joined.ok());
  ASSERT_EQ(joined.ranges.size(), 1u);
  EXPECT_EQ(joined.ranges.front().bytes, (Bytes{1, 2}));

  const Diagnostic back = firstError(
      "        ORG $0400\n        DB 1,2,3,4\n        ORG $0402\n        DB 9\n");
  EXPECT_EQ(back.line, 4u);
  EXPECT_NE(back.message.find("overlaps"), std::string::npos);
  EXPECT_NE(back.message.find("$0402"), std::string::npos);

  // Backward into a gap nothing wrote is not an overlap; the ranges come out in
  // address order.
  const Assembly gap = assembleSpc700(
      "        ORG $0500\n        DB 5\n        ORG $0400\n        DB 4\n", "t.asm");
  ASSERT_TRUE(gap.ok());
  ASSERT_EQ(gap.ranges.size(), 2u);
  EXPECT_EQ(gap.ranges[0].start, 0x0400u);
  EXPECT_EQ(gap.ranges[1].start, 0x0500u);

  EXPECT_NE(firstError("        ORG later\nlater:  NOP\n").message.find("must be known"),
            std::string::npos);
  EXPECT_NE(firstError("        ORG $FFFF\n        DW 1\n").message.find("past the end"),
            std::string::npos);
}

// §5.2 and §5.3: DB emits bytes and strings, DW words low byte first, DL 24-bit
// values in the wide dialect only, DS a run of one byte.
TEST(Assembler, TheDataDirectives) {
  EXPECT_EQ(spc("        DB $01,$02,$03\n"), (Bytes{1, 2, 3}));
  EXPECT_EQ(spc("        DB \"text\",0\n"), (Bytes{'t', 'e', 'x', 't', 0}));
  EXPECT_EQ(spc("        DW $0A2B,table+2\ntable:\n"), (Bytes{0x2B, 0x0A, 0x06, 0x00}));
  EXPECT_EQ(spc("        DS 3\n"), (Bytes{0, 0, 0}));
  EXPECT_EQ(spc("        DS 2,$FF\n"), (Bytes{0xFF, 0xFF}));
  EXPECT_NE(firstError("        DL $123456\n").message.find("24 bits"), std::string::npos);
  EXPECT_NE(firstError("        DW \"ab\"\n").message.find("belongs to DB"), std::string::npos);
  EXPECT_NE(firstError("        DB\n").message.find("at least one"), std::string::npos);
  EXPECT_NE(firstError("        DB 1,,2\n").message.find("empty item"), std::string::npos);
  EXPECT_NE(firstError("        DS n\nn:      NOP\n").message.find("must be known"),
            std::string::npos);
  // §4.3: arithmetic wraps at the address width, so a 20-bit literal is its low
  // 16 bits before the width check, and only then must fit.
  EXPECT_EQ(spc("        DW $12345\n"), (Bytes{0x45, 0x23}));
  EXPECT_NE(firstError("        DB $12345\n").message.find("does not fit in a byte"),
            std::string::npos);

  const Assembly wide = assembleCpu65816("        DL $7E:1234,handler\nhandler:\n", "t.asm");
  ASSERT_TRUE(wide.ok());
  EXPECT_EQ(wide.ranges.front().bytes, (Bytes{0x34, 0x12, 0x7E, 0x06, 0x00, 0x00}));
}

// §3: the bank-separated literal belongs to the dialect whose addresses are 24
// bits wide.
TEST(Assembler, BankSeparatedLiteralsBelongToTheWideDialect) {
  EXPECT_NE(firstError("        DW $7E:1234\n").message.find("16 bits"), std::string::npos);
  // §4.3: a 24-bit value where a word is required is reported, not truncated.
  const Assembly wide = assembleCpu65816("        DW $01:8000\n", "t.asm");
  ASSERT_FALSE(wide.ok());
  EXPECT_NE(wide.errors.front().message.find("does not fit in a word"), std::string::npos);
  EXPECT_EQ(assembleCpu65816("        DW $00:8000\n", "t.asm").ranges.front().bytes,
            (Bytes{0x00, 0x80}));
  EXPECT_NE(assembleCpu65816("        DL $100:0000\n", "t.asm").errors.front().message.find("8 bits"),
            std::string::npos);
}

// §7: every diagnostic names the file, the line, and what was expected; one run
// reports every error; a file with an error emits nothing.
TEST(Assembler, DiagnosticsNameTheFileTheLineAndTheExpectation) {
  const Assembly assembly = assembleSpc700(
      "        ORG $0400\n"
      "        DB 300\n"
      "        NOP\n"
      "        MOV A,\n"
      "        BNE nowhere\n",
      "driver.asm");
  ASSERT_EQ(assembly.errors.size(), 3u);
  EXPECT_EQ(assembly.errors[0].file, "driver.asm");
  EXPECT_EQ(assembly.errors[0].line, 2u);
  EXPECT_NE(assembly.errors[0].message.find("does not fit in a byte"), std::string::npos);
  EXPECT_EQ(assembly.errors[1].line, 4u);
  EXPECT_EQ(assembly.errors[2].line, 5u);
  EXPECT_NE(assembly.errors[2].message.find("nowhere"), std::string::npos);
  EXPECT_FALSE(assembly.ok());
  EXPECT_TRUE(assembly.ranges.empty());
}

// §2.3: whitespace separates tokens and is otherwise insignificant; tabs and
// CRLF line endings are read.
TEST(Assembler, WhitespaceIsInsignificantAwayFromColumnOne) {
  EXPECT_EQ(spc("        MOV   A , #$01\n"), (Bytes{0xE8, 0x01}));
  EXPECT_EQ(spc("\tMOV\tA,#$01\t; tabbed\r\n"), (Bytes{0xE8, 0x01}));
  EXPECT_EQ(spc("        DB 1 + 2 , 3\n"), (Bytes{3, 3}));
}

TEST(Assembler, TheImageHelperLaysRangesIntoAWindow) {
  const Assembly assembly = assembleSpc700(
      "        ORG $0402\n        DB 1,2\n        ORG $0406\n        DB 3\n", "t.asm");
  ASSERT_TRUE(assembly.ok());
  const auto laid = image(assembly, 0x0400, 8, 0xEE);
  ASSERT_TRUE(laid.has_value());
  EXPECT_EQ(*laid, (Bytes{0xEE, 0xEE, 1, 2, 0xEE, 0xEE, 3, 0xEE}));
  EXPECT_FALSE(image(assembly, 0x0403, 8).has_value());
  EXPECT_FALSE(image(assembly, 0x0400, 4).has_value());
}

// The helpers a dialect builds its syntax on.
TEST(Assembler, ExpressionEndStopsBeforeAnIndexSuffixWrittenWithPlus) {
  EXPECT_EQ(expressionEnd("$10+X", 0), 3u);
  EXPECT_EQ(expressionEnd("$10+Y]", 0), 3u);
  EXPECT_EQ(expressionEnd("a+b", 0), 3u);
  EXPECT_EQ(expressionEnd("a+X1", 0), 4u) << "X1 is a name, not the register";
  EXPECT_EQ(expressionEnd("end-start,Y", 0), 9u);
  EXPECT_EQ(expressionEnd("$7E:1234,X", 0), 8u);
  EXPECT_EQ(expressionEnd("'\\'',", 0), 4u);
  EXPECT_EQ(expressionEnd("*+2)", 0), 3u);
  EXPECT_EQ(expressionEnd(",X", 0), 0u);
  EXPECT_EQ(compact("MOV A , \"a b\" , ' '"), "MOVA,\"a b\",' '");
  EXPECT_EQ(upper("Mov a,x"), "MOV A,X");
  EXPECT_EQ(hex(0x1F, 4), "$001F");
}

}  // namespace snaggletooth::assembler
