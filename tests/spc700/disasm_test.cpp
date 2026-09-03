// The SPC700 disassembler.
//
// The load-bearing case here is DeclaredLengthMatchesTheInterpreter: it walks all
// 256 opcodes and compares the length the disassembler reports against the distance
// the interpreter actually moves the program counter. The instruction table is
// hand-authored, so that case is what stops a wrong operand shape from reading as a
// plausible instruction — the failure mode that makes a disassembler worse than
// useless, because its output still looks like code.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/spc700.h"
#include "spc700_disasm.h"

namespace snaggletooth::disasm {
namespace {

// A flat 64KB bus, the same shape the disassembler's own cost probe runs against.
struct FlatBus {
  std::array<std::uint8_t, 65536> ram{};
  std::uint8_t read(std::uint16_t address) { return ram[address]; }
  void write(std::uint16_t address, std::uint8_t value) { ram[address] = value; }
};

constexpr std::uint16_t kAt = 0x2000;

// Decodes one opcode with zero operand bytes behind it.
Instruction decodeBare(std::uint8_t opcode) {
  const std::array<std::uint8_t, 3> image = {opcode, 0x00, 0x00};
  const std::optional<Instruction> decoded = decodeAt(image, kAt, kAt);
  EXPECT_TRUE(decoded.has_value()) << "opcode $" << std::hex << int{opcode};
  return decoded.value_or(Instruction{});
}

}  // namespace

TEST(Spc700Disasm, EveryOpcodeDecodesToText) {
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    const Instruction decoded = decodeBare(static_cast<std::uint8_t>(opcode));
    EXPECT_FALSE(decoded.text.empty()) << "opcode $" << std::hex << opcode;
    EXPECT_GE(decoded.length, 1);
    EXPECT_LE(decoded.length, 3);
    EXPECT_EQ(decoded.bytes.size(), decoded.length);
    EXPECT_EQ(decoded.opcode, opcode);
  }
}

// The cross-check. For every instruction that falls through to the next one, the
// length the table declares must equal the distance the interpreter moves PC.
TEST(Spc700Disasm, DeclaredLengthMatchesTheInterpreter) {
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    const std::uint8_t byte = static_cast<std::uint8_t>(opcode);
    const Instruction decoded = decodeBare(byte);
    if (decoded.flow != Flow::Continue) continue;

    FlatBus bus;
    bus.ram[kAt] = byte;  // operand bytes stay zero, as in decodeBare

    Spc700State start;
    start.pc = kAt;
    start.sp = 0xFF;
    Spc700 cpu{start};
    cpu.stepInstruction(bus);

    const std::uint16_t advanced =
        static_cast<std::uint16_t>(cpu.state().pc - kAt);
    EXPECT_EQ(advanced, decoded.length)
        << "opcode $" << std::hex << opcode << " (" << decoded.text << ")";
  }
}

// The costs are measured by running the interpreter, so these pin the measurement
// against the cycle shapes the core composes: an implied instruction is its opcode
// fetch plus one setup cycle plus the internal cycles its form declares, and a read
// is the fetch plus the mode's setup plus the read itself.
TEST(Spc700Disasm, MeasuredCostsMatchTheCoresCycleShapes) {
  const std::array<CycleCost, 256>& costs = cycleTable();
  EXPECT_EQ(costs[0x00].base, 2);   // NOP — fetch, one setup cycle
  EXPECT_EQ(costs[0xDF].base, 3);   // DAA A — one internal cycle
  EXPECT_EQ(costs[0x9F].base, 5);   // XCN A — three internal cycles
  EXPECT_EQ(costs[0xCF].base, 9);   // MUL YA — seven
  EXPECT_EQ(costs[0x9E].base, 12);  // DIV YA,X — ten
  EXPECT_EQ(costs[0xE8].base, 2);   // MOV A,#imm — the operand fetch is the read
  EXPECT_EQ(costs[0xE4].base, 3);   // MOV A,dp
  EXPECT_EQ(costs[0xF4].base, 4);   // MOV A,dp+X — one indexing cycle more
  EXPECT_EQ(costs[0xE5].base, 4);   // MOV A,!abs
  EXPECT_EQ(costs[0xF5].base, 5);   // MOV A,!abs+X
  EXPECT_EQ(costs[0xE7].base, 6);   // MOV A,[dp+X]
}

// A conditional instruction carries both costs; an unconditional one carries one.
TEST(Spc700Disasm, ConditionalInstructionsCarryBothCosts) {
  const std::array<CycleCost, 256>& costs = cycleTable();
  constexpr std::array<std::uint8_t, 8> kRelativeBranches = {
      0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0};
  for (std::uint8_t branch : kRelativeBranches) {
    EXPECT_GT(costs[branch].taken, costs[branch].base)
        << "branch $" << std::hex << int{branch} << " costs no more when taken";
  }
  EXPECT_GT(costs[0x2E].taken, costs[0x2E].base);  // CBNE dp,rel
  EXPECT_GT(costs[0x6E].taken, costs[0x6E].base);  // DBNZ dp,rel
  EXPECT_GT(costs[0xFE].taken, costs[0xFE].base);  // DBNZ Y,rel
  EXPECT_GT(costs[0x03].taken, costs[0x03].base);  // BBS dp.0,rel
  EXPECT_EQ(costs[0x00].taken, 0);                 // NOP has no condition
  EXPECT_EQ(costs[0x2F].taken, 0);                 // BRA is always taken
}

TEST(Spc700Disasm, RelativeTargetsMeasureFromTheEndOfTheInstruction) {
  // BEQ +$0C at $2000 is a two-byte instruction, so the target is $2002 + $0C.
  const std::array<std::uint8_t, 2> image = {0xF0, 0x0C};
  const std::optional<Instruction> decoded = decodeAt(image, kAt, kAt);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->target.value_or(0), 0x200E);
  EXPECT_EQ(decoded->text, "BEQ $200E");

  // And backwards, through the sign.
  const std::array<std::uint8_t, 2> back = {0xF0, 0xFE};
  const std::optional<Instruction> loop = decodeAt(back, kAt, kAt);
  ASSERT_TRUE(loop.has_value());
  EXPECT_EQ(loop->target.value_or(0), kAt);
}

// Every form whose target is an address the dialect can name carries the text
// that writes it as a symbol: the call and the absolute jump behind `!`, a
// branch bare, a direct-page branch after its first operand. PCALL's operand is
// an offset byte and TCALL's an entry number, and a jump through a table has no
// constant target, so none of those carries one.
TEST(Spc700Disasm, TargetsCarryTheirSymbolicForm) {
  auto symbolic = [](std::initializer_list<std::uint8_t> bytes) -> std::optional<SymbolicText> {
    const std::vector<std::uint8_t> image(bytes);
    const std::optional<Instruction> decoded = decodeAt(image, kAt, kAt);
    EXPECT_TRUE(decoded.has_value());
    return decoded ? decoded->symbolic : std::nullopt;
  };
  std::optional<SymbolicText> call = symbolic({0x3F, 0x34, 0x12});  // CALL !$1234
  ASSERT_TRUE(call.has_value());
  EXPECT_EQ(call->before, "CALL !");
  EXPECT_EQ(call->after, "");
  std::optional<SymbolicText> jump = symbolic({0x5F, 0x34, 0x12});  // JMP !$1234
  ASSERT_TRUE(jump.has_value());
  EXPECT_EQ(jump->before, "JMP !");
  std::optional<SymbolicText> branch = symbolic({0xF0, 0x0C});  // BEQ $200E
  ASSERT_TRUE(branch.has_value());
  EXPECT_EQ(branch->before, "BEQ ");
  EXPECT_EQ(branch->after, "");
  std::optional<SymbolicText> bit = symbolic({0x03, 0x10, 0x0C});  // BBS $10.0,$200F
  ASSERT_TRUE(bit.has_value());
  EXPECT_EQ(bit->before, "BBS $10.0,");
  EXPECT_EQ(bit->after, "");
  std::optional<SymbolicText> count = symbolic({0x6E, 0x10, 0x0C});  // DBNZ $10,$200F
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(count->before, "DBNZ $10,");
  std::optional<SymbolicText> indexed = symbolic({0xDE, 0x10, 0x0C});  // CBNE $10+X,$200F
  ASSERT_TRUE(indexed.has_value());
  EXPECT_EQ(indexed->before, "CBNE $10+X,");
  EXPECT_FALSE(symbolic({0x4F, 0x12}).has_value());        // PCALL $12
  EXPECT_FALSE(symbolic({0x01}).has_value());              // TCALL 0
  EXPECT_FALSE(symbolic({0x1F, 0x34, 0x12}).has_value());  // JMP [!$1234+X]
  EXPECT_FALSE(symbolic({0xE5, 0x34, 0x12}).has_value());  // MOV A,!$1234
}

// The two-operand direct-page forms carry their source byte first and print it
// second. Getting this backwards produces a valid-looking instruction with its
// operands swapped, which is why it has its own case.
TEST(Spc700Disasm, TwoOperandDirectPageFormsPrintDestinationFirst) {
  const std::array<std::uint8_t, 3> move = {0xFA, 0x34, 0x12};  // MOV dp,dp
  EXPECT_EQ(decodeAt(move, kAt, kAt)->text, "MOV $12,$34");

  const std::array<std::uint8_t, 3> immediate = {0x8F, 0x34, 0x12};  // MOV dp,#imm
  EXPECT_EQ(decodeAt(immediate, kAt, kAt)->text, "MOV $12,#$34");
}

TEST(Spc700Disasm, AbsoluteBitOperandSplitsAddressFromIndex) {
  // MOV1 C,m.b: the address is the low 13 bits, the bit index the top 3.
  const std::uint16_t operand = static_cast<std::uint16_t>(0x1234u | (5u << 13));
  const std::array<std::uint8_t, 3> image = {
      0xAA, static_cast<std::uint8_t>(operand & 0xFFu),
      static_cast<std::uint8_t>(operand >> 8)};
  EXPECT_EQ(decodeAt(image, kAt, kAt)->text, "MOV1 C,!$1234.5");
}

TEST(Spc700Disasm, OperandsRunningPastTheImageDoNotDecode) {
  const std::array<std::uint8_t, 1> truncated = {0xE5};  // MOV A,!abs wants two more
  EXPECT_FALSE(decodeAt(truncated, kAt, kAt).has_value());
  EXPECT_FALSE(decodeAt(truncated, kAt, 0x3000).has_value());  // outside the image
}

// The trace follows control flow, so bytes it cannot reach are data. A linear
// sweep would decode the trailing bytes here as instructions.
TEST(Spc700Disasm, UnreachedBytesAreDataRatherThanInstructions) {
  const std::vector<std::uint8_t> image = {
      0xE8, 0x01,  // MOV A,#$01
      0x6F,        // RET
      0xE4, 0xE4, 0xE4, 0xE4,  // unreachable
  };
  DisasmRequest request;
  request.image = image;
  request.base = 0x0400;
  const Listing listing = trace(request);

  std::size_t code = 0;
  std::size_t data = 0;
  for (const Line& line : listing.lines) {
    if (line.isCode) ++code;
    else data += line.data.size();
  }
  EXPECT_EQ(code, 2u);  // the move and the return, and nothing past them
  EXPECT_EQ(data, 4u);
}

TEST(Spc700Disasm, ACallReturnsSoBytesAfterItStayCode) {
  const std::vector<std::uint8_t> image = {
      0x3F, 0x05, 0x04,  // CALL !$0405
      0x00,              // NOP — reached, because a call comes back
      0x6F,              // RET
      0x6F,              // RET, the call's own target at $0405
  };
  DisasmRequest request;
  request.image = image;
  request.base = 0x0400;
  const Listing listing = trace(request);
  for (const Line& line : listing.lines) {
    EXPECT_TRUE(line.isCode) << "byte at $" << std::hex << line.address
                             << " was not reached";
  }
}

TEST(Spc700Disasm, PatchedBytesAreCalledOutAgainstThePriorImage) {
  const std::vector<std::uint8_t> after = {0xE8, 0x7D, 0x6F};   // MOV A,#$7D / RET
  const std::vector<std::uint8_t> before = {0xE8, 0x00, 0x6F};  // MOV A,#$00 / RET
  DisasmRequest request;
  request.image = after;
  request.base = 0x0A00;
  request.priorImage = before;
  const Listing listing = trace(request);

  ASSERT_FALSE(listing.lines.empty());
  ASSERT_TRUE(listing.lines.front().isCode);
  EXPECT_NE(listing.lines.front().instruction.note.find("PATCHED"), std::string::npos);
  // The untouched instruction beside it carries no such note.
  EXPECT_EQ(listing.lines.back().instruction.note.find("PATCHED"), std::string::npos);
}

TEST(Spc700Disasm, HardwareRegistersAreNamedOnTheOperand) {
  EXPECT_EQ(registerName(0x00F2), "DSPADDR");
  EXPECT_EQ(registerName(0x00FD), "T0OUT");
  EXPECT_TRUE(registerName(0x0030).empty());

  const std::vector<std::uint8_t> image = {0xE4, 0xF2, 0x6F};  // MOV A,$F2 / RET
  DisasmRequest request;
  request.image = image;
  request.base = 0x0400;
  const Listing listing = trace(request);
  EXPECT_EQ(listing.lines.front().instruction.note, "DSPADDR");
}

// The listing is source: it carries a placement directive, and everything that is
// not an instruction is a directive or a comment.
TEST(Spc700Disasm, ListingRendersAsAssemblableSource) {
  const std::vector<std::uint8_t> image = {0xE8, 0x01, 0x6F, 0xAB, 0xCD};
  DisasmRequest request;
  request.image = image;
  request.base = 0x0400;
  const std::string text = render(trace(request));

  EXPECT_NE(text.find("ORG $0400"), std::string::npos);
  EXPECT_NE(text.find("MOV A,#$01"), std::string::npos);
  EXPECT_NE(text.find("DB $AB,$CD"), std::string::npos);
  // Every non-blank line is a label, a directive, an instruction, or a comment —
  // never a bare hex dump.
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.empty()) continue;
    const bool comment = line[0] == ';';
    const bool label = line.back() == ':';
    const bool indented = line.rfind("        ", 0) == 0;
    EXPECT_TRUE(comment || label || indented) << "not source: " << line;
  }
}


// Only an operand that names memory is annotated. An immediate is a value and a
// branch displacement is a code address, and either can land in $F0-$FF by
// coincidence; naming a register on them tells the reader the instruction touches
// hardware it does not.
TEST(Spc700Disasm, OnlyMemoryOperandsAreNamed) {
  auto noteOf = [](std::vector<std::uint8_t> image) {
    image.push_back(0x6F);  // RET, so the trace ends inside the image
    DisasmRequest request;
    request.image = image;
    request.base = 0x0400;
    const Listing listing = trace(request);
    return listing.lines.front().instruction.note;
  };
  EXPECT_EQ(noteOf({0xE8, 0xF2}), "");            // MOV A,#$F2 — a value
  EXPECT_EQ(noteOf({0xD0, 0xF8}), "");            // BNE -8 — a displacement
  EXPECT_EQ(noteOf({0x4F, 0xF4}), "$FFF4");       // PCALL $F4 — code in page $FF; the comment is where it lands
  EXPECT_EQ(noteOf({0xE4, 0xF2}), "DSPADDR");     // MOV A,$F2
  EXPECT_EQ(noteOf({0xE5, 0xF4, 0x00}), "CPUIO0"); // MOV A,!$00F4
  EXPECT_EQ(noteOf({0x8F, 0x00, 0xF4}), "CPUIO0"); // MOV $F4,#$00 — the destination
  EXPECT_EQ(noteOf({0xFA, 0x00, 0xF5}), "CPUIO1"); // MOV $F5,$00 — the destination
  EXPECT_EQ(noteOf({0x03, 0xFD, 0x02}), "T0OUT");  // BBS $FD.0,rel — the tested byte
  EXPECT_EQ(noteOf({0xAA, 0xF2, 0x00}), "DSPADDR"); // MOV1 C,!$00F2.0
}

}  // namespace snaggletooth::disasm
