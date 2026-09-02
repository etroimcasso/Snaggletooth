#include "spc700_disasm.h"

#include <array>
#include <cstdio>
#include <string>
#include <utility>

#include "snaggletooth/apu/spc700.h"

namespace snaggletooth::disasm {
namespace {

// What operand bytes follow an opcode, and how each one reads. The mnemonic text
// carries the surrounding syntax — a register name, an index suffix, the brackets
// of an indirect form — and leaves a numbered slot where each operand lands. So
// this enumerates the operand bytes alone, and every instruction sharing a byte
// shape shares an entry here however differently it prints.
enum class Operands : std::uint8_t {
  None,    // no operand bytes
  Imm,     // one byte, an immediate value
  Dp,      // one byte, a direct-page offset
  Abs,     // two bytes, an address, low byte first
  AbsBit,  // two bytes: an address in the low 13 bits, a bit index in the top 3
  Rel,     // one byte, a displacement from the end of the instruction
  DpRel,   // a direct-page offset, then a displacement
  DpDp,    // a source offset, then a destination offset
  ImmDp,   // an immediate byte, then a destination offset
  Upage,   // one byte, an offset into page $FF
};

// How many bytes an operand shape adds to the opcode.
constexpr std::uint8_t operandBytes(Operands operands) noexcept {
  switch (operands) {
    case Operands::None: return 0;
    case Operands::Imm:
    case Operands::Dp:
    case Operands::Rel:
    case Operands::Upage: return 1;
    case Operands::Abs:
    case Operands::AbsBit:
    case Operands::DpRel:
    case Operands::DpDp:
    case Operands::ImmDp: return 2;
  }
  return 0;
}

// One row of the instruction table. `opcode` repeats the index so the row is
// readable on its own and a test can prove the table is in order.
struct OpcodeInfo {
  std::uint8_t opcode = 0;
  const char* text = "";
  Operands operands = Operands::None;
  Flow flow = Flow::Continue;
};

// The instruction table. `%1` and `%2` are the operand slots, filled in the order
// the bytes appear — so the two-operand direct-page forms, whose source byte comes
// first but prints second, name their slots out of order on purpose.
constexpr std::array<OpcodeInfo, 256> kOpcodes = {{
    {.opcode = 0x00, .text = "NOP", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x01, .text = "TCALL 0", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x02, .text = "SET1 $%1.0", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x03, .text = "BBS $%1.0,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x04, .text = "OR A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x05, .text = "OR A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x06, .text = "OR A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x07, .text = "OR A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x08, .text = "OR A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0x09, .text = "OR $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0x0A, .text = "OR1 C,!$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0x0B, .text = "ASL $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x0C, .text = "ASL !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x0D, .text = "PUSH PSW", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x0E, .text = "TSET1 !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x0F, .text = "BRK", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x10, .text = "BPL $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0x11, .text = "TCALL 1", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x12, .text = "CLR1 $%1.0", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x13, .text = "BBC $%1.0,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x14, .text = "OR A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x15, .text = "OR A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x16, .text = "OR A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x17, .text = "OR A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x18, .text = "OR $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0x19, .text = "OR (X),(Y)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x1A, .text = "DECW $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x1B, .text = "ASL $%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x1C, .text = "ASL A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x1D, .text = "DEC X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x1E, .text = "CMP X,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x1F, .text = "JMP [!$%1+X]", .operands = Operands::Abs, .flow = Flow::Jump},
    {.opcode = 0x20, .text = "CLRP", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x21, .text = "TCALL 2", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x22, .text = "SET1 $%1.1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x23, .text = "BBS $%1.1,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x24, .text = "AND A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x25, .text = "AND A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x26, .text = "AND A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x27, .text = "AND A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x28, .text = "AND A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0x29, .text = "AND $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0x2A, .text = "OR1 C,/!$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0x2B, .text = "ROL $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x2C, .text = "ROL !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x2D, .text = "PUSH A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x2E, .text = "CBNE $%1,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x2F, .text = "BRA $%1", .operands = Operands::Rel, .flow = Flow::Jump},
    {.opcode = 0x30, .text = "BMI $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0x31, .text = "TCALL 3", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x32, .text = "CLR1 $%1.1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x33, .text = "BBC $%1.1,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x34, .text = "AND A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x35, .text = "AND A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x36, .text = "AND A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x37, .text = "AND A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x38, .text = "AND $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0x39, .text = "AND (X),(Y)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x3A, .text = "INCW $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x3B, .text = "ROL $%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x3C, .text = "ROL A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x3D, .text = "INC X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x3E, .text = "CMP X,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x3F, .text = "CALL !$%1", .operands = Operands::Abs, .flow = Flow::Call},
    {.opcode = 0x40, .text = "SETP", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x41, .text = "TCALL 4", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x42, .text = "SET1 $%1.2", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x43, .text = "BBS $%1.2,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x44, .text = "EOR A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x45, .text = "EOR A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x46, .text = "EOR A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x47, .text = "EOR A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x48, .text = "EOR A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0x49, .text = "EOR $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0x4A, .text = "AND1 C,!$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0x4B, .text = "LSR $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x4C, .text = "LSR !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x4D, .text = "PUSH X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x4E, .text = "TCLR1 !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x4F, .text = "PCALL $%1", .operands = Operands::Upage, .flow = Flow::Call},
    {.opcode = 0x50, .text = "BVC $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0x51, .text = "TCALL 5", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x52, .text = "CLR1 $%1.2", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x53, .text = "BBC $%1.2,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x54, .text = "EOR A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x55, .text = "EOR A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x56, .text = "EOR A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x57, .text = "EOR A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x58, .text = "EOR $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0x59, .text = "EOR (X),(Y)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x5A, .text = "CMPW YA,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x5B, .text = "LSR $%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x5C, .text = "LSR A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x5D, .text = "MOV X,A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x5E, .text = "CMP Y,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x5F, .text = "JMP !$%1", .operands = Operands::Abs, .flow = Flow::Jump},
    {.opcode = 0x60, .text = "CLRC", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x61, .text = "TCALL 6", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x62, .text = "SET1 $%1.3", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x63, .text = "BBS $%1.3,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x64, .text = "CMP A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x65, .text = "CMP A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x66, .text = "CMP A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x67, .text = "CMP A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x68, .text = "CMP A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0x69, .text = "CMP $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0x6A, .text = "AND1 C,/!$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0x6B, .text = "ROR $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x6C, .text = "ROR !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x6D, .text = "PUSH Y", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x6E, .text = "DBNZ $%1,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x6F, .text = "RET", .operands = Operands::None, .flow = Flow::Return},
    {.opcode = 0x70, .text = "BVS $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0x71, .text = "TCALL 7", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x72, .text = "CLR1 $%1.3", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x73, .text = "BBC $%1.3,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x74, .text = "CMP A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x75, .text = "CMP A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x76, .text = "CMP A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x77, .text = "CMP A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x78, .text = "CMP $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0x79, .text = "CMP (X),(Y)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x7A, .text = "ADDW YA,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x7B, .text = "ROR $%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x7C, .text = "ROR A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x7D, .text = "MOV A,X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x7E, .text = "CMP Y,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x7F, .text = "RETI", .operands = Operands::None, .flow = Flow::Return},
    {.opcode = 0x80, .text = "SETC", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x81, .text = "TCALL 8", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x82, .text = "SET1 $%1.4", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x83, .text = "BBS $%1.4,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x84, .text = "ADC A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x85, .text = "ADC A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x86, .text = "ADC A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x87, .text = "ADC A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x88, .text = "ADC A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0x89, .text = "ADC $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0x8A, .text = "EOR1 C,!$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0x8B, .text = "DEC $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x8C, .text = "DEC !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x8D, .text = "MOV Y,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0x8E, .text = "POP PSW", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x8F, .text = "MOV $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0x90, .text = "BCC $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0x91, .text = "TCALL 9", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0x92, .text = "CLR1 $%1.4", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x93, .text = "BBC $%1.4,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0x94, .text = "ADC A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x95, .text = "ADC A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x96, .text = "ADC A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0x97, .text = "ADC A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x98, .text = "ADC $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0x99, .text = "ADC (X),(Y)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x9A, .text = "SUBW YA,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x9B, .text = "DEC $%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0x9C, .text = "DEC A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x9D, .text = "MOV X,SP", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x9E, .text = "DIV YA,X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0x9F, .text = "XCN A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xA0, .text = "EI", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xA1, .text = "TCALL 10", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0xA2, .text = "SET1 $%1.5", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xA3, .text = "BBS $%1.5,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xA4, .text = "SBC A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xA5, .text = "SBC A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xA6, .text = "SBC A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xA7, .text = "SBC A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xA8, .text = "SBC A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0xA9, .text = "SBC $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0xAA, .text = "MOV1 C,!$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0xAB, .text = "INC $%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xAC, .text = "INC !$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xAD, .text = "CMP Y,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0xAE, .text = "POP A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xAF, .text = "MOV (X)+,A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xB0, .text = "BCS $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0xB1, .text = "TCALL 11", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0xB2, .text = "CLR1 $%1.5", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xB3, .text = "BBC $%1.5,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xB4, .text = "SBC A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xB5, .text = "SBC A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xB6, .text = "SBC A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xB7, .text = "SBC A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xB8, .text = "SBC $%2,#$%1", .operands = Operands::ImmDp, .flow = Flow::Continue},
    {.opcode = 0xB9, .text = "SBC (X),(Y)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xBA, .text = "MOVW YA,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xBB, .text = "INC $%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xBC, .text = "INC A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xBD, .text = "MOV SP,X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xBE, .text = "DAS A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xBF, .text = "MOV A,(X)+", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xC0, .text = "DI", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xC1, .text = "TCALL 12", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0xC2, .text = "SET1 $%1.6", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xC3, .text = "BBS $%1.6,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xC4, .text = "MOV $%1,A", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xC5, .text = "MOV !$%1,A", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xC6, .text = "MOV (X),A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xC7, .text = "MOV [$%1+X],A", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xC8, .text = "CMP X,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0xC9, .text = "MOV !$%1,X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xCA, .text = "MOV1 !$%1.%2,C", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0xCB, .text = "MOV $%1,Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xCC, .text = "MOV !$%1,Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xCD, .text = "MOV X,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0xCE, .text = "POP X", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xCF, .text = "MUL YA", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xD0, .text = "BNE $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0xD1, .text = "TCALL 13", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0xD2, .text = "CLR1 $%1.6", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xD3, .text = "BBC $%1.6,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xD4, .text = "MOV $%1+X,A", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xD5, .text = "MOV !$%1+X,A", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xD6, .text = "MOV !$%1+Y,A", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xD7, .text = "MOV [$%1]+Y,A", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xD8, .text = "MOV $%1,X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xD9, .text = "MOV $%1+Y,X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xDA, .text = "MOVW $%1,YA", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xDB, .text = "MOV $%1+X,Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xDC, .text = "DEC Y", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xDD, .text = "MOV A,Y", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xDE, .text = "CBNE $%1+X,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xDF, .text = "DAA A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xE0, .text = "CLRV", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xE1, .text = "TCALL 14", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0xE2, .text = "SET1 $%1.7", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xE3, .text = "BBS $%1.7,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xE4, .text = "MOV A,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xE5, .text = "MOV A,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xE6, .text = "MOV A,(X)", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xE7, .text = "MOV A,[$%1+X]", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xE8, .text = "MOV A,#$%1", .operands = Operands::Imm, .flow = Flow::Continue},
    {.opcode = 0xE9, .text = "MOV X,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xEA, .text = "NOT1 !$%1.%2", .operands = Operands::AbsBit, .flow = Flow::Continue},
    {.opcode = 0xEB, .text = "MOV Y,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xEC, .text = "MOV Y,!$%1", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xED, .text = "NOTC", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xEE, .text = "POP Y", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xEF, .text = "SLEEP", .operands = Operands::None, .flow = Flow::Halt},
    {.opcode = 0xF0, .text = "BEQ $%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0xF1, .text = "TCALL 15", .operands = Operands::None, .flow = Flow::Call},
    {.opcode = 0xF2, .text = "CLR1 $%1.7", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xF3, .text = "BBC $%1.7,$%2", .operands = Operands::DpRel, .flow = Flow::Branch},
    {.opcode = 0xF4, .text = "MOV A,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xF5, .text = "MOV A,!$%1+X", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xF6, .text = "MOV A,!$%1+Y", .operands = Operands::Abs, .flow = Flow::Continue},
    {.opcode = 0xF7, .text = "MOV A,[$%1]+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xF8, .text = "MOV X,$%1", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xF9, .text = "MOV X,$%1+Y", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xFA, .text = "MOV $%2,$%1", .operands = Operands::DpDp, .flow = Flow::Continue},
    {.opcode = 0xFB, .text = "MOV Y,$%1+X", .operands = Operands::Dp, .flow = Flow::Continue},
    {.opcode = 0xFC, .text = "INC Y", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xFD, .text = "MOV Y,A", .operands = Operands::None, .flow = Flow::Continue},
    {.opcode = 0xFE, .text = "DBNZ Y,$%1", .operands = Operands::Rel, .flow = Flow::Branch},
    {.opcode = 0xFF, .text = "STOP", .operands = Operands::None, .flow = Flow::Halt},
}};

// ---- measuring an instruction's cost -------------------------------------
// A flat 64KB bus. Every probe runs against it, so the measurement never depends
// on a register's side effect: the cost of an instruction is a property of the
// instruction, and a probe that reached a live register would be measuring the
// machine instead.
struct ProbeBus {
  std::array<std::uint8_t, 65536> ram{};
  std::uint8_t read(std::uint16_t address) { return ram[address]; }
  void write(std::uint16_t address, std::uint8_t value) { ram[address] = value; }
};

// Where a probe places the instruction it measures. Any address clear of the
// direct page and the stack works; this one is clear of both.
constexpr std::uint16_t kProbeAddress = 0x2000;

// The state a probe starts from, with the condition of a conditional instruction
// forced the way `takeIt` asks. An instruction with no condition ignores it.
Spc700State probeState(std::uint8_t opcode, bool takeIt, ProbeBus& bus) {
  Spc700State state;
  state.pc = kProbeAddress;
  state.sp = 0xFF;

  // The relative branches: each tests one status bit, and the opcode's high nibble
  // says which bit and which way. Set the whole status byte so the tested bit lands
  // the way the probe wants and no other instruction is disturbed.
  auto branchOn = [&](std::uint8_t flag, bool whenSet) {
    const bool wantSet = takeIt == whenSet;
    state.psw = wantSet ? flag : std::uint8_t{0};
  };
  switch (opcode) {
    case 0x10: branchOn(kFlagN, false); break;  // BPL
    case 0x30: branchOn(kFlagN, true); break;   // BMI
    case 0x50: branchOn(kFlagV, false); break;  // BVC
    case 0x70: branchOn(kFlagV, true); break;   // BVS
    case 0x90: branchOn(kFlagC, false); break;  // BCC
    case 0xB0: branchOn(kFlagC, true); break;   // BCS
    case 0xD0: branchOn(kFlagZ, false); break;  // BNE
    case 0xF0: branchOn(kFlagZ, true); break;   // BEQ

    // CBNE branches when A differs from the byte it read. Both forms read direct
    // page $00, because the probe's operand byte is zero and X is zero with it.
    case 0x2E:
    case 0xDE:
      state.a = takeIt ? std::uint8_t{1} : std::uint8_t{0};
      break;

    // DBNZ branches while the decremented value is not yet zero.
    case 0x6E: bus.ram[0x0000] = takeIt ? 2 : 1; break;
    case 0xFE: state.y = takeIt ? 2 : 1; break;

    default:
      // BBS and BBC read one bit of a direct-page byte. The bit index is the
      // opcode's top three bits, and an even high nibble branches on the bit set.
      if ((opcode & 0x0F) == 0x03) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (opcode >> 5));
        const bool branchesOnSet = ((opcode >> 4) & 1u) == 0u;
        const bool wantSet = takeIt == branchesOnSet;
        bus.ram[0x0000] = wantSet ? bit : std::uint8_t{0};
      }
      break;
  }
  return state;
}

// Runs one instruction on a fresh bus and returns the cycles it took.
std::uint8_t measure(std::uint8_t opcode, bool takeIt) {
  ProbeBus bus;
  bus.ram[kProbeAddress] = opcode;
  // Every operand byte stays zero: a direct-page offset of $00, an absolute
  // address of $0000, a displacement of zero. All are in range on a flat bus.
  Spc700 cpu{probeState(opcode, takeIt, bus)};
  return static_cast<std::uint8_t>(cpu.stepInstruction(bus));
}

// Whether an opcode's cost can depend on a condition — the relative branches and
// the four instructions that read a byte before deciding.
constexpr bool isConditional(std::uint8_t opcode) noexcept {
  switch (opcode) {
    case 0x10: case 0x30: case 0x50: case 0x70:
    case 0x90: case 0xB0: case 0xD0: case 0xF0:
    case 0x2E: case 0xDE: case 0x6E: case 0xFE:
      return true;
    default:
      return (opcode & 0x0F) == 0x03;  // BBS / BBC
  }
}

std::string hex8(std::uint8_t value) {
  char buffer[3];
  std::snprintf(buffer, sizeof buffer, "%02X", value);
  return buffer;
}

std::string hex16(std::uint16_t value) {
  char buffer[5];
  std::snprintf(buffer, sizeof buffer, "%04X", value);
  return buffer;
}

// Replaces `%1` and `%2` in a template with the operands rendered for it.
std::string fill(const char* text, const std::string& first, const std::string& second) {
  std::string out;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '%' && (p[1] == '1' || p[1] == '2')) {
      out += (p[1] == '1') ? first : second;
      ++p;
      continue;
    }
    out += *p;
  }
  return out;
}

}  // namespace

const std::array<CycleCost, 256>& cycleTable() {
  static const std::array<CycleCost, 256> table = [] {
    std::array<CycleCost, 256> costs{};
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
      const std::uint8_t byte = static_cast<std::uint8_t>(opcode);
      costs[opcode].base = measure(byte, false);
      costs[opcode].taken = isConditional(byte) ? measure(byte, true) : std::uint8_t{0};
    }
    return costs;
  }();
  return table;
}

std::string_view registerName(std::uint16_t address) {
  switch (address) {
    case 0x00F0: return "TEST";
    case 0x00F1: return "CONTROL";
    case 0x00F2: return "DSPADDR";
    case 0x00F3: return "DSPDATA";
    case 0x00F4: return "CPUIO0";
    case 0x00F5: return "CPUIO1";
    case 0x00F6: return "CPUIO2";
    case 0x00F7: return "CPUIO3";
    case 0x00F8: return "AUXIO4";
    case 0x00F9: return "AUXIO5";
    case 0x00FA: return "T0TARGET";
    case 0x00FB: return "T1TARGET";
    case 0x00FC: return "T2TARGET";
    case 0x00FD: return "T0OUT";
    case 0x00FE: return "T1OUT";
    case 0x00FF: return "T2OUT";
    default: return {};
  }
}

std::optional<Decoded> Spc700Backend::decode(std::span<const std::uint8_t> image, Address base,
                                             Address at, Context context) const {
  if (at > 0xFFFFu || base > 0xFFFFu) return std::nullopt;
  if (at < base) return std::nullopt;
  const std::size_t offset = static_cast<std::size_t>(at - base);
  if (offset >= image.size()) return std::nullopt;

  const std::uint8_t opcode = image[offset];
  const OpcodeInfo& info = kOpcodes[opcode];
  const std::uint8_t extra = operandBytes(info.operands);
  if (offset + 1u + extra > image.size()) return std::nullopt;

  const std::uint8_t first = extra >= 1 ? image[offset + 1] : std::uint8_t{0};
  const std::uint8_t second = extra >= 2 ? image[offset + 2] : std::uint8_t{0};
  const std::uint16_t address = static_cast<std::uint16_t>(at);

  Instruction out;
  out.address = address;
  out.opcode = opcode;
  out.length = static_cast<std::uint8_t>(1 + extra);
  out.flow = info.flow;
  out.cycles = cycleTable()[opcode];
  out.bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(offset),
                   image.begin() + static_cast<std::ptrdiff_t>(offset + out.length));

  const std::uint16_t after = static_cast<std::uint16_t>(address + out.length);
  const std::uint16_t word = static_cast<std::uint16_t>(first | (second << 8));

  std::string slot1;
  std::string slot2;
  switch (info.operands) {
    case Operands::None:
      break;
    case Operands::Imm:
    case Operands::Dp:
      slot1 = hex8(first);
      break;
    case Operands::Upage:
      slot1 = "FF" + hex8(first);
      out.target = static_cast<std::uint16_t>(0xFF00u | first);
      break;
    case Operands::Abs:
      slot1 = hex16(word);
      if (info.flow == Flow::Jump && opcode == 0x5F) out.target = word;
      if (info.flow == Flow::Call) out.target = word;
      break;
    case Operands::AbsBit:
      slot1 = hex16(static_cast<std::uint16_t>(word & 0x1FFFu));
      slot2 = std::to_string(word >> 13);
      break;
    case Operands::Rel: {
      const std::uint16_t destination = static_cast<std::uint16_t>(
          after + static_cast<std::int8_t>(first));
      slot1 = hex16(destination);
      out.target = destination;
      break;
    }
    case Operands::DpRel: {
      const std::uint16_t destination = static_cast<std::uint16_t>(
          after + static_cast<std::int8_t>(second));
      slot1 = hex8(first);
      slot2 = hex16(destination);
      out.target = destination;
      break;
    }
    case Operands::DpDp:
    case Operands::ImmDp:
      slot1 = hex8(first);
      slot2 = hex8(second);
      break;
  }

  // The address the operand names, for the register annotation: a two-byte
  // instruction's operand byte read as a direct-page address, a three-byte
  // instruction's operand word.
  if (extra == 1) out.operandAddress = first;
  if (extra == 2) out.operandAddress = word;

  out.text = fill(info.text, slot1, slot2);
  return Decoded{.instruction = std::move(out), .next = context};
}

std::string_view Spc700Backend::registerName(Address address) const {
  return address > 0xFFFFu ? std::string_view{}
                           : disasm::registerName(static_cast<std::uint16_t>(address));
}

const Spc700Backend& spc700Backend() {
  static const Spc700Backend backend;
  return backend;
}

std::optional<Instruction> decodeAt(std::span<const std::uint8_t> image,
                                    std::uint16_t base, std::uint16_t address) {
  std::optional<Decoded> decoded = spc700Backend().decode(image, base, address, Context{});
  if (!decoded) return std::nullopt;
  return std::move(decoded->instruction);
}

Listing trace(const DisasmRequest& request) { return trace(spc700Backend(), request); }

}  // namespace snaggletooth::disasm
