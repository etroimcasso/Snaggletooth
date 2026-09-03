#include "cpu65816_disasm.h"

#include <cstdio>
#include <string>
#include <utility>

#include "snaggletooth/cpu/cpu65816.h"

namespace snaggletooth::disasm {
namespace {

// ---- the mode, packed ------------------------------------------------------
// One bit per flag. A width that is not known packs as zero, and the carry packs
// as zero when it is not known, so equal readings pack to equal bits.
constexpr std::uint32_t kBitAccumulator8 = 1u << 0;
constexpr std::uint32_t kBitIndex8 = 1u << 1;
constexpr std::uint32_t kBitEmulation = 1u << 2;
constexpr std::uint32_t kBitAccumulatorKnown = 1u << 3;
constexpr std::uint32_t kBitIndexKnown = 1u << 4;
constexpr std::uint32_t kBitCarryKnown = 1u << 5;
constexpr std::uint32_t kBitCarry = 1u << 6;

// The bits that decide how bytes read: the emulation flag and the two widths with
// what is known about them. The carry bits are outside it.
constexpr std::uint32_t kReadingBits =
    kBitAccumulator8 | kBitIndex8 | kBitEmulation | kBitAccumulatorKnown | kBitIndexKnown;

// ---- the instruction table ---------------------------------------------------
using Mode = Cpu65816Addressing;
using OpcodeInfo = Cpu65816Opcode;

// The table, one row per opcode, in the order of the datasheet's opcode matrix.
constexpr std::array<OpcodeInfo, 256> kOpcodes = {{
    {0x00, "BRK", Mode::ImmediateByte, Flow::Call},
    {0x01, "ORA", Mode::DirectIndirectX, Flow::Continue},
    {0x02, "COP", Mode::ImmediateByte, Flow::Call},
    {0x03, "ORA", Mode::StackRelative, Flow::Continue},
    {0x04, "TSB", Mode::Direct, Flow::Continue},
    {0x05, "ORA", Mode::Direct, Flow::Continue},
    {0x06, "ASL", Mode::Direct, Flow::Continue},
    {0x07, "ORA", Mode::DirectIndirectLong, Flow::Continue},
    {0x08, "PHP", Mode::Implied, Flow::Continue},
    {0x09, "ORA", Mode::ImmediateM, Flow::Continue},
    {0x0A, "ASL", Mode::Accumulator, Flow::Continue},
    {0x0B, "PHD", Mode::Implied, Flow::Continue},
    {0x0C, "TSB", Mode::Absolute, Flow::Continue},
    {0x0D, "ORA", Mode::Absolute, Flow::Continue},
    {0x0E, "ASL", Mode::Absolute, Flow::Continue},
    {0x0F, "ORA", Mode::AbsoluteLong, Flow::Continue},
    {0x10, "BPL", Mode::Relative, Flow::Branch},
    {0x11, "ORA", Mode::DirectIndirectY, Flow::Continue},
    {0x12, "ORA", Mode::DirectIndirect, Flow::Continue},
    {0x13, "ORA", Mode::StackRelativeY, Flow::Continue},
    {0x14, "TRB", Mode::Direct, Flow::Continue},
    {0x15, "ORA", Mode::DirectX, Flow::Continue},
    {0x16, "ASL", Mode::DirectX, Flow::Continue},
    {0x17, "ORA", Mode::DirectIndirectLongY, Flow::Continue},
    {0x18, "CLC", Mode::Implied, Flow::Continue},
    {0x19, "ORA", Mode::AbsoluteY, Flow::Continue},
    {0x1A, "INC", Mode::Accumulator, Flow::Continue},
    {0x1B, "TCS", Mode::Implied, Flow::Continue},
    {0x1C, "TRB", Mode::Absolute, Flow::Continue},
    {0x1D, "ORA", Mode::AbsoluteX, Flow::Continue},
    {0x1E, "ASL", Mode::AbsoluteX, Flow::Continue},
    {0x1F, "ORA", Mode::AbsoluteLongX, Flow::Continue},
    {0x20, "JSR", Mode::Absolute, Flow::Call},
    {0x21, "AND", Mode::DirectIndirectX, Flow::Continue},
    {0x22, "JSL", Mode::AbsoluteLong, Flow::Call},
    {0x23, "AND", Mode::StackRelative, Flow::Continue},
    {0x24, "BIT", Mode::Direct, Flow::Continue},
    {0x25, "AND", Mode::Direct, Flow::Continue},
    {0x26, "ROL", Mode::Direct, Flow::Continue},
    {0x27, "AND", Mode::DirectIndirectLong, Flow::Continue},
    {0x28, "PLP", Mode::Implied, Flow::Continue},
    {0x29, "AND", Mode::ImmediateM, Flow::Continue},
    {0x2A, "ROL", Mode::Accumulator, Flow::Continue},
    {0x2B, "PLD", Mode::Implied, Flow::Continue},
    {0x2C, "BIT", Mode::Absolute, Flow::Continue},
    {0x2D, "AND", Mode::Absolute, Flow::Continue},
    {0x2E, "ROL", Mode::Absolute, Flow::Continue},
    {0x2F, "AND", Mode::AbsoluteLong, Flow::Continue},
    {0x30, "BMI", Mode::Relative, Flow::Branch},
    {0x31, "AND", Mode::DirectIndirectY, Flow::Continue},
    {0x32, "AND", Mode::DirectIndirect, Flow::Continue},
    {0x33, "AND", Mode::StackRelativeY, Flow::Continue},
    {0x34, "BIT", Mode::DirectX, Flow::Continue},
    {0x35, "AND", Mode::DirectX, Flow::Continue},
    {0x36, "ROL", Mode::DirectX, Flow::Continue},
    {0x37, "AND", Mode::DirectIndirectLongY, Flow::Continue},
    {0x38, "SEC", Mode::Implied, Flow::Continue},
    {0x39, "AND", Mode::AbsoluteY, Flow::Continue},
    {0x3A, "DEC", Mode::Accumulator, Flow::Continue},
    {0x3B, "TSC", Mode::Implied, Flow::Continue},
    {0x3C, "BIT", Mode::AbsoluteX, Flow::Continue},
    {0x3D, "AND", Mode::AbsoluteX, Flow::Continue},
    {0x3E, "ROL", Mode::AbsoluteX, Flow::Continue},
    {0x3F, "AND", Mode::AbsoluteLongX, Flow::Continue},
    {0x40, "RTI", Mode::Implied, Flow::Return},
    {0x41, "EOR", Mode::DirectIndirectX, Flow::Continue},
    {0x42, "WDM", Mode::ImmediateByte, Flow::Continue},
    {0x43, "EOR", Mode::StackRelative, Flow::Continue},
    {0x44, "MVP", Mode::BlockMove, Flow::Continue},
    {0x45, "EOR", Mode::Direct, Flow::Continue},
    {0x46, "LSR", Mode::Direct, Flow::Continue},
    {0x47, "EOR", Mode::DirectIndirectLong, Flow::Continue},
    {0x48, "PHA", Mode::Implied, Flow::Continue},
    {0x49, "EOR", Mode::ImmediateM, Flow::Continue},
    {0x4A, "LSR", Mode::Accumulator, Flow::Continue},
    {0x4B, "PHK", Mode::Implied, Flow::Continue},
    {0x4C, "JMP", Mode::Absolute, Flow::Jump},
    {0x4D, "EOR", Mode::Absolute, Flow::Continue},
    {0x4E, "LSR", Mode::Absolute, Flow::Continue},
    {0x4F, "EOR", Mode::AbsoluteLong, Flow::Continue},
    {0x50, "BVC", Mode::Relative, Flow::Branch},
    {0x51, "EOR", Mode::DirectIndirectY, Flow::Continue},
    {0x52, "EOR", Mode::DirectIndirect, Flow::Continue},
    {0x53, "EOR", Mode::StackRelativeY, Flow::Continue},
    {0x54, "MVN", Mode::BlockMove, Flow::Continue},
    {0x55, "EOR", Mode::DirectX, Flow::Continue},
    {0x56, "LSR", Mode::DirectX, Flow::Continue},
    {0x57, "EOR", Mode::DirectIndirectLongY, Flow::Continue},
    {0x58, "CLI", Mode::Implied, Flow::Continue},
    {0x59, "EOR", Mode::AbsoluteY, Flow::Continue},
    {0x5A, "PHY", Mode::Implied, Flow::Continue},
    {0x5B, "TCD", Mode::Implied, Flow::Continue},
    {0x5C, "JML", Mode::AbsoluteLong, Flow::Jump},
    {0x5D, "EOR", Mode::AbsoluteX, Flow::Continue},
    {0x5E, "LSR", Mode::AbsoluteX, Flow::Continue},
    {0x5F, "EOR", Mode::AbsoluteLongX, Flow::Continue},
    {0x60, "RTS", Mode::Implied, Flow::Return},
    {0x61, "ADC", Mode::DirectIndirectX, Flow::Continue},
    {0x62, "PER", Mode::PushRelative, Flow::Continue},
    {0x63, "ADC", Mode::StackRelative, Flow::Continue},
    {0x64, "STZ", Mode::Direct, Flow::Continue},
    {0x65, "ADC", Mode::Direct, Flow::Continue},
    {0x66, "ROR", Mode::Direct, Flow::Continue},
    {0x67, "ADC", Mode::DirectIndirectLong, Flow::Continue},
    {0x68, "PLA", Mode::Implied, Flow::Continue},
    {0x69, "ADC", Mode::ImmediateM, Flow::Continue},
    {0x6A, "ROR", Mode::Accumulator, Flow::Continue},
    {0x6B, "RTL", Mode::Implied, Flow::Return},
    {0x6C, "JMP", Mode::AbsoluteIndirect, Flow::Jump},
    {0x6D, "ADC", Mode::Absolute, Flow::Continue},
    {0x6E, "ROR", Mode::Absolute, Flow::Continue},
    {0x6F, "ADC", Mode::AbsoluteLong, Flow::Continue},
    {0x70, "BVS", Mode::Relative, Flow::Branch},
    {0x71, "ADC", Mode::DirectIndirectY, Flow::Continue},
    {0x72, "ADC", Mode::DirectIndirect, Flow::Continue},
    {0x73, "ADC", Mode::StackRelativeY, Flow::Continue},
    {0x74, "STZ", Mode::DirectX, Flow::Continue},
    {0x75, "ADC", Mode::DirectX, Flow::Continue},
    {0x76, "ROR", Mode::DirectX, Flow::Continue},
    {0x77, "ADC", Mode::DirectIndirectLongY, Flow::Continue},
    {0x78, "SEI", Mode::Implied, Flow::Continue},
    {0x79, "ADC", Mode::AbsoluteY, Flow::Continue},
    {0x7A, "PLY", Mode::Implied, Flow::Continue},
    {0x7B, "TDC", Mode::Implied, Flow::Continue},
    {0x7C, "JMP", Mode::AbsoluteIndexedIndirect, Flow::Jump},
    {0x7D, "ADC", Mode::AbsoluteX, Flow::Continue},
    {0x7E, "ROR", Mode::AbsoluteX, Flow::Continue},
    {0x7F, "ADC", Mode::AbsoluteLongX, Flow::Continue},
    {0x80, "BRA", Mode::Relative, Flow::Jump},
    {0x81, "STA", Mode::DirectIndirectX, Flow::Continue},
    {0x82, "BRL", Mode::RelativeLong, Flow::Jump},
    {0x83, "STA", Mode::StackRelative, Flow::Continue},
    {0x84, "STY", Mode::Direct, Flow::Continue},
    {0x85, "STA", Mode::Direct, Flow::Continue},
    {0x86, "STX", Mode::Direct, Flow::Continue},
    {0x87, "STA", Mode::DirectIndirectLong, Flow::Continue},
    {0x88, "DEY", Mode::Implied, Flow::Continue},
    {0x89, "BIT", Mode::ImmediateM, Flow::Continue},
    {0x8A, "TXA", Mode::Implied, Flow::Continue},
    {0x8B, "PHB", Mode::Implied, Flow::Continue},
    {0x8C, "STY", Mode::Absolute, Flow::Continue},
    {0x8D, "STA", Mode::Absolute, Flow::Continue},
    {0x8E, "STX", Mode::Absolute, Flow::Continue},
    {0x8F, "STA", Mode::AbsoluteLong, Flow::Continue},
    {0x90, "BCC", Mode::Relative, Flow::Branch},
    {0x91, "STA", Mode::DirectIndirectY, Flow::Continue},
    {0x92, "STA", Mode::DirectIndirect, Flow::Continue},
    {0x93, "STA", Mode::StackRelativeY, Flow::Continue},
    {0x94, "STY", Mode::DirectX, Flow::Continue},
    {0x95, "STA", Mode::DirectX, Flow::Continue},
    {0x96, "STX", Mode::DirectY, Flow::Continue},
    {0x97, "STA", Mode::DirectIndirectLongY, Flow::Continue},
    {0x98, "TYA", Mode::Implied, Flow::Continue},
    {0x99, "STA", Mode::AbsoluteY, Flow::Continue},
    {0x9A, "TXS", Mode::Implied, Flow::Continue},
    {0x9B, "TXY", Mode::Implied, Flow::Continue},
    {0x9C, "STZ", Mode::Absolute, Flow::Continue},
    {0x9D, "STA", Mode::AbsoluteX, Flow::Continue},
    {0x9E, "STZ", Mode::AbsoluteX, Flow::Continue},
    {0x9F, "STA", Mode::AbsoluteLongX, Flow::Continue},
    {0xA0, "LDY", Mode::ImmediateX, Flow::Continue},
    {0xA1, "LDA", Mode::DirectIndirectX, Flow::Continue},
    {0xA2, "LDX", Mode::ImmediateX, Flow::Continue},
    {0xA3, "LDA", Mode::StackRelative, Flow::Continue},
    {0xA4, "LDY", Mode::Direct, Flow::Continue},
    {0xA5, "LDA", Mode::Direct, Flow::Continue},
    {0xA6, "LDX", Mode::Direct, Flow::Continue},
    {0xA7, "LDA", Mode::DirectIndirectLong, Flow::Continue},
    {0xA8, "TAY", Mode::Implied, Flow::Continue},
    {0xA9, "LDA", Mode::ImmediateM, Flow::Continue},
    {0xAA, "TAX", Mode::Implied, Flow::Continue},
    {0xAB, "PLB", Mode::Implied, Flow::Continue},
    {0xAC, "LDY", Mode::Absolute, Flow::Continue},
    {0xAD, "LDA", Mode::Absolute, Flow::Continue},
    {0xAE, "LDX", Mode::Absolute, Flow::Continue},
    {0xAF, "LDA", Mode::AbsoluteLong, Flow::Continue},
    {0xB0, "BCS", Mode::Relative, Flow::Branch},
    {0xB1, "LDA", Mode::DirectIndirectY, Flow::Continue},
    {0xB2, "LDA", Mode::DirectIndirect, Flow::Continue},
    {0xB3, "LDA", Mode::StackRelativeY, Flow::Continue},
    {0xB4, "LDY", Mode::DirectX, Flow::Continue},
    {0xB5, "LDA", Mode::DirectX, Flow::Continue},
    {0xB6, "LDX", Mode::DirectY, Flow::Continue},
    {0xB7, "LDA", Mode::DirectIndirectLongY, Flow::Continue},
    {0xB8, "CLV", Mode::Implied, Flow::Continue},
    {0xB9, "LDA", Mode::AbsoluteY, Flow::Continue},
    {0xBA, "TSX", Mode::Implied, Flow::Continue},
    {0xBB, "TYX", Mode::Implied, Flow::Continue},
    {0xBC, "LDY", Mode::AbsoluteX, Flow::Continue},
    {0xBD, "LDA", Mode::AbsoluteX, Flow::Continue},
    {0xBE, "LDX", Mode::AbsoluteY, Flow::Continue},
    {0xBF, "LDA", Mode::AbsoluteLongX, Flow::Continue},
    {0xC0, "CPY", Mode::ImmediateX, Flow::Continue},
    {0xC1, "CMP", Mode::DirectIndirectX, Flow::Continue},
    {0xC2, "REP", Mode::ImmediateByte, Flow::Continue},
    {0xC3, "CMP", Mode::StackRelative, Flow::Continue},
    {0xC4, "CPY", Mode::Direct, Flow::Continue},
    {0xC5, "CMP", Mode::Direct, Flow::Continue},
    {0xC6, "DEC", Mode::Direct, Flow::Continue},
    {0xC7, "CMP", Mode::DirectIndirectLong, Flow::Continue},
    {0xC8, "INY", Mode::Implied, Flow::Continue},
    {0xC9, "CMP", Mode::ImmediateM, Flow::Continue},
    {0xCA, "DEX", Mode::Implied, Flow::Continue},
    {0xCB, "WAI", Mode::Implied, Flow::Continue},
    {0xCC, "CPY", Mode::Absolute, Flow::Continue},
    {0xCD, "CMP", Mode::Absolute, Flow::Continue},
    {0xCE, "DEC", Mode::Absolute, Flow::Continue},
    {0xCF, "CMP", Mode::AbsoluteLong, Flow::Continue},
    {0xD0, "BNE", Mode::Relative, Flow::Branch},
    {0xD1, "CMP", Mode::DirectIndirectY, Flow::Continue},
    {0xD2, "CMP", Mode::DirectIndirect, Flow::Continue},
    {0xD3, "CMP", Mode::StackRelativeY, Flow::Continue},
    {0xD4, "PEI", Mode::DirectIndirect, Flow::Continue},
    {0xD5, "CMP", Mode::DirectX, Flow::Continue},
    {0xD6, "DEC", Mode::DirectX, Flow::Continue},
    {0xD7, "CMP", Mode::DirectIndirectLongY, Flow::Continue},
    {0xD8, "CLD", Mode::Implied, Flow::Continue},
    {0xD9, "CMP", Mode::AbsoluteY, Flow::Continue},
    {0xDA, "PHX", Mode::Implied, Flow::Continue},
    {0xDB, "STP", Mode::Implied, Flow::Halt},
    {0xDC, "JML", Mode::AbsoluteIndirectLong, Flow::Jump},
    {0xDD, "CMP", Mode::AbsoluteX, Flow::Continue},
    {0xDE, "DEC", Mode::AbsoluteX, Flow::Continue},
    {0xDF, "CMP", Mode::AbsoluteLongX, Flow::Continue},
    {0xE0, "CPX", Mode::ImmediateX, Flow::Continue},
    {0xE1, "SBC", Mode::DirectIndirectX, Flow::Continue},
    {0xE2, "SEP", Mode::ImmediateByte, Flow::Continue},
    {0xE3, "SBC", Mode::StackRelative, Flow::Continue},
    {0xE4, "CPX", Mode::Direct, Flow::Continue},
    {0xE5, "SBC", Mode::Direct, Flow::Continue},
    {0xE6, "INC", Mode::Direct, Flow::Continue},
    {0xE7, "SBC", Mode::DirectIndirectLong, Flow::Continue},
    {0xE8, "INX", Mode::Implied, Flow::Continue},
    {0xE9, "SBC", Mode::ImmediateM, Flow::Continue},
    {0xEA, "NOP", Mode::Implied, Flow::Continue},
    {0xEB, "XBA", Mode::Implied, Flow::Continue},
    {0xEC, "CPX", Mode::Absolute, Flow::Continue},
    {0xED, "SBC", Mode::Absolute, Flow::Continue},
    {0xEE, "INC", Mode::Absolute, Flow::Continue},
    {0xEF, "SBC", Mode::AbsoluteLong, Flow::Continue},
    {0xF0, "BEQ", Mode::Relative, Flow::Branch},
    {0xF1, "SBC", Mode::DirectIndirectY, Flow::Continue},
    {0xF2, "SBC", Mode::DirectIndirect, Flow::Continue},
    {0xF3, "SBC", Mode::StackRelativeY, Flow::Continue},
    {0xF4, "PEA", Mode::PushAbsolute, Flow::Continue},
    {0xF5, "SBC", Mode::DirectX, Flow::Continue},
    {0xF6, "INC", Mode::DirectX, Flow::Continue},
    {0xF7, "SBC", Mode::DirectIndirectLongY, Flow::Continue},
    {0xF8, "SED", Mode::Implied, Flow::Continue},
    {0xF9, "SBC", Mode::AbsoluteY, Flow::Continue},
    {0xFA, "PLX", Mode::Implied, Flow::Continue},
    {0xFB, "XCE", Mode::Implied, Flow::Continue},
    {0xFC, "JSR", Mode::AbsoluteIndexedIndirect, Flow::Call},
    {0xFD, "SBC", Mode::AbsoluteX, Flow::Continue},
    {0xFE, "INC", Mode::AbsoluteX, Flow::Continue},
    {0xFF, "SBC", Mode::AbsoluteLongX, Flow::Continue},
}};

// ---- measuring an instruction's cost ------------------------------------
// A flat 64 KB bus that answers every bank from the same bytes. Every probe runs
// against it, so the measurement never depends on a register's side effect: the
// cost of an instruction is a property of the instruction, and a probe that
// reached a live register would be measuring the machine instead.
struct ProbeBus {
  std::array<std::uint8_t, 65536> ram{};
  std::uint8_t read(std::uint32_t address, CycleKind) { return ram[address & 0xFFFFu]; }
  void write(std::uint32_t address, std::uint8_t value, CycleKind) {
    ram[address & 0xFFFFu] = value;
  }
  void internal(std::uint32_t) {}
  void internal(std::uint32_t, CycleKind) {}
};

// Where a probe places the instruction it measures: clear of the direct page,
// the stack, and the vectors.
constexpr std::uint16_t kProbeAddress = 0x2000;

// The eight relative branches, the only instructions whose cost depends on a
// condition.
constexpr bool isConditional(std::uint8_t opcode) noexcept {
  switch (opcode) {
    case 0x10: case 0x30: case 0x50: case 0x70:
    case 0x90: case 0xB0: case 0xD0: case 0xF0:
      return true;
    default:
      return false;
  }
}

// The state a probe starts from under one setting of the mode flags, with a
// branch's condition forced the way `takeIt` asks. The direct register is zero
// and no index reaches across a page, which is what the datasheet's own cycle
// table assumes.
Cpu65816State probeState(std::uint8_t opcode, bool emulation, bool accumulator8, bool index8,
                         bool takeIt) {
  Cpu65816State state;
  state.pc = kProbeAddress;
  state.s = 0x01FF;
  state.e = emulation;
  state.p = static_cast<std::uint8_t>((accumulator8 ? kCpuFlagM : 0) |
                                      (index8 ? kCpuFlagX : 0));

  // Each branch tests one status bit, and the opcode says which bit and which
  // way. Set that bit so the test lands the way the probe wants.
  auto branchOn = [&](std::uint8_t flag, bool whenSet) {
    const bool wantSet = takeIt == whenSet;
    if (wantSet) state.p = static_cast<std::uint8_t>(state.p | flag);
  };
  switch (opcode) {
    case 0x10: branchOn(kCpuFlagN, false); break;  // BPL
    case 0x30: branchOn(kCpuFlagN, true); break;   // BMI
    case 0x50: branchOn(kCpuFlagV, false); break;  // BVC
    case 0x70: branchOn(kCpuFlagV, true); break;   // BVS
    case 0x90: branchOn(kCpuFlagC, false); break;  // BCC
    case 0xB0: branchOn(kCpuFlagC, true); break;   // BCS
    case 0xD0: branchOn(kCpuFlagZ, false); break;  // BNE
    case 0xF0: branchOn(kCpuFlagZ, true); break;   // BEQ
    default: break;
  }
  return state;
}

// Runs one instruction on a fresh bus and returns the cycles it took. Every
// operand byte stays zero: a direct-page offset of $00, an address of $0000, a
// displacement of zero, a block move of one byte. All are in range on a flat bus.
std::uint8_t measure(std::uint8_t opcode, bool emulation, bool accumulator8, bool index8,
                     bool takeIt) {
  ProbeBus bus;
  bus.ram[kProbeAddress] = opcode;
  Cpu65816 cpu{probeState(opcode, emulation, accumulator8, index8, takeIt)};
  return static_cast<std::uint8_t>(cpu.stepInstruction(bus));
}

// Which of the measured tables a setting of the flags selects. Emulation mode
// forces both widths, so its one table sits beside the four native ones.
std::size_t tableIndex(bool emulation, bool accumulator8, bool index8) noexcept {
  if (emulation) return 4;
  return (accumulator8 ? 0u : 2u) | (index8 ? 0u : 1u);
}

// ---- rendering -------------------------------------------------------------
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

// A 24-bit address as the dialect writes a long operand: `$BB:HHLL`.
std::string longOperand(Address address) { return formatAddress(address, 24); }

// The cost of an opcode under a mode. Where a width is unknown the cost is looked
// up under both settings of it, and is known only when they agree.
CycleCost costUnder(std::uint8_t opcode, const Cpu65816Mode& mode);

}  // namespace

// ---- the mode, applied ------------------------------------------------------
// Only a handful of instructions move the flags; everything else passes the
// mode through, and every instruction but CLC and SEC forgets what was known
// about the carry.
Cpu65816Mode cpu65816ModeAfter(std::uint8_t opcode, std::uint8_t operand,
                               const Cpu65816Mode& mode, std::string& note) {
  Cpu65816Mode next = mode;
  next.carryKnown = false;
  next.carry = false;
  switch (opcode) {
    case 0x18:  // CLC
      next.carryKnown = true;
      next.carry = false;
      break;
    case 0x38:  // SEC
      next.carryKnown = true;
      next.carry = true;
      break;

    // REP clears the flags its mask names and SEP sets them; the widths they
    // name become known either way. Emulation mode holds both widths at eight
    // whatever the mask says, and that rule is applied where the mode is packed
    // (`contextOf`), so nothing here tests for it.
    case 0xC2:  // REP #imm
      if (operand & kCpuFlagM) { next.accumulator8 = false; next.accumulatorKnown = true; }
      if (operand & kCpuFlagX) { next.index8 = false; next.indexKnown = true; }
      break;
    case 0xE2:  // SEP #imm
      if (operand & kCpuFlagM) { next.accumulator8 = true; next.accumulatorKnown = true; }
      if (operand & kCpuFlagX) { next.index8 = true; next.indexKnown = true; }
      break;

    // PLP and RTI load the status byte from the stack, which the image cannot
    // say anything about. Emulation mode keeps the widths forced, by the same
    // rule as above.
    case 0x28:  // PLP
    case 0x40:  // RTI
      next.accumulatorKnown = false;
      next.indexKnown = false;
      break;

    // XCE exchanges the carry with the emulation flag. The carry is known when
    // CLC or SEC is the instruction before; then the mode after is settled, and
    // entering emulation forces both widths to eight. Leaving it keeps the
    // widths where emulation held them. With the carry unknown the mode is
    // kept and the line says so.
    case 0xFB:  // XCE
      if (mode.carryKnown) {
        next.emulation = mode.carry;
        if (next.emulation) {
          next.accumulator8 = true;
          next.index8 = true;
          next.accumulatorKnown = true;
          next.indexKnown = true;
        }
        next.carryKnown = true;
        next.carry = mode.emulation;
      } else {
        note = "XCE with the carry not set by the instruction before; the mode is kept";
      }
      break;

    default:
      break;
  }
  return next;
}

const std::array<Cpu65816Opcode, 256>& cpu65816Opcodes() { return kOpcodes; }

namespace {

CycleCost costUnder(std::uint8_t opcode, const Cpu65816Mode& mode) {
  std::optional<CycleCost> found;
  bool agree = true;
  for (int a = 0; a < 2 && agree; ++a) {
    const bool accumulator8 = mode.accumulatorKnown ? mode.accumulator8 : (a == 0);
    if (mode.accumulatorKnown && a == 1) break;
    for (int x = 0; x < 2; ++x) {
      const bool index8 = mode.indexKnown ? mode.index8 : (x == 0);
      if (mode.indexKnown && x == 1) break;
      const CycleCost cost = cpu65816CycleTable(mode.emulation, accumulator8, index8)[opcode];
      if (!found) {
        found = cost;
      } else if (found->base != cost.base || found->taken != cost.taken) {
        agree = false;
        break;
      }
    }
  }
  if (agree && found) return *found;
  return CycleCost{.base = 0, .taken = 0, .known = false};
}

// The eight DMA channels' register names, built once: the table names them with
// the channel digit in the middle.
const std::array<std::array<std::string, 16>, 8>& dmaRegisterNames() {
  static const std::array<std::array<std::string, 16>, 8> names = [] {
    std::array<std::array<std::string, 16>, 8> out{};
    for (unsigned channel = 0; channel < 8; ++channel) {
      const std::string n = std::to_string(channel);
      out[channel][0x0] = "DMAP" + n;
      out[channel][0x1] = "BBAD" + n;
      out[channel][0x2] = "A1T" + n + "L";
      out[channel][0x3] = "A1T" + n + "H";
      out[channel][0x4] = "A1B" + n;
      out[channel][0x5] = "DAS" + n + "L";
      out[channel][0x6] = "DAS" + n + "H";
      out[channel][0x7] = "DASB" + n;
      out[channel][0x8] = "A2A" + n + "L";
      out[channel][0x9] = "A2A" + n + "H";
      out[channel][0xA] = "NLTR" + n;
      out[channel][0xB] = "UNUSED" + n;
      out[channel][0xF] = "UNUSED" + n;
    }
    return out;
  }();
  return names;
}

}  // namespace

Context contextOf(const Cpu65816Mode& mode) noexcept {
  Cpu65816Mode m = mode;
  if (m.emulation) {
    m.accumulator8 = true;
    m.index8 = true;
    m.accumulatorKnown = true;
    m.indexKnown = true;
  }
  if (!m.accumulatorKnown) m.accumulator8 = false;
  if (!m.indexKnown) m.index8 = false;
  if (!m.carryKnown) m.carry = false;
  std::uint32_t bits = 0;
  if (m.accumulator8) bits |= kBitAccumulator8;
  if (m.index8) bits |= kBitIndex8;
  if (m.emulation) bits |= kBitEmulation;
  if (m.accumulatorKnown) bits |= kBitAccumulatorKnown;
  if (m.indexKnown) bits |= kBitIndexKnown;
  if (m.carryKnown) bits |= kBitCarryKnown;
  if (m.carry) bits |= kBitCarry;
  return Context{.bits = bits};
}

Cpu65816Mode modeOf(Context context) noexcept {
  const std::uint32_t bits = context.bits;
  return Cpu65816Mode{
      .emulation = (bits & kBitEmulation) != 0,
      .accumulator8 = (bits & kBitAccumulator8) != 0,
      .index8 = (bits & kBitIndex8) != 0,
      .accumulatorKnown = (bits & kBitAccumulatorKnown) != 0,
      .indexKnown = (bits & kBitIndexKnown) != 0,
      .carryKnown = (bits & kBitCarryKnown) != 0,
      .carry = (bits & kBitCarry) != 0,
  };
}

const std::array<CycleCost, 256>& cpu65816CycleTable(bool emulation, bool accumulator8,
                                                     bool index8) {
  static const std::array<std::array<CycleCost, 256>, 5> tables = [] {
    std::array<std::array<CycleCost, 256>, 5> out{};
    for (std::size_t index = 0; index < 5; ++index) {
      const bool e = index == 4;
      const bool a8 = e || (index & 2u) == 0u;
      const bool x8 = e || (index & 1u) == 0u;
      for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const std::uint8_t byte = static_cast<std::uint8_t>(opcode);
        out[index][opcode].base = measure(byte, e, a8, x8, false);
        out[index][opcode].taken =
            isConditional(byte) ? measure(byte, e, a8, x8, true) : std::uint8_t{0};
      }
    }
    return out;
  }();
  return tables[tableIndex(emulation, accumulator8, index8)];
}

std::string_view cpu65816RegisterName(Address address) {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  const bool visible = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
  if (!visible) return {};
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);

  static constexpr std::array<std::string_view, 64> kPpu = {
      "INIDISP", "OBJSEL",  "OAMADDL", "OAMADDH", "OAMDATA", "BGMODE",  "MOSAIC",  "BG1SC",
      "BG2SC",   "BG3SC",   "BG4SC",   "BG12NBA", "BG34NBA", "BG1HOFS", "BG1VOFS", "BG2HOFS",
      "BG2VOFS", "BG3HOFS", "BG3VOFS", "BG4HOFS", "BG4VOFS", "VMAIN",   "VMADDL",  "VMADDH",
      "VMDATAL", "VMDATAH", "M7SEL",   "M7A",     "M7B",     "M7C",     "M7D",     "M7X",
      "M7Y",     "CGADD",   "CGDATA",  "W12SEL",  "W34SEL",  "WOBJSEL", "WH0",     "WH1",
      "WH2",     "WH3",     "WBGLOG",  "WOBJLOG", "TM",      "TS",      "TMW",     "TSW",
      "CGWSEL",  "CGADSUB", "COLDATA", "SETINI",  "MPYL",    "MPYM",    "MPYH",    "SLHV",
      "OAMDATAREAD", "VMDATALREAD", "VMDATAHREAD", "CGDATAREAD", "OPHCT", "OPVCT", "STAT77",
      "STAT78",
  };
  if (offset >= 0x2100u && offset <= 0x213Fu) return kPpu[offset - 0x2100u];

  switch (offset) {
    case 0x2140: return "APUIO0";
    case 0x2141: return "APUIO1";
    case 0x2142: return "APUIO2";
    case 0x2143: return "APUIO3";
    case 0x2180: return "WMDATA";
    case 0x2181: return "WMADDL";
    case 0x2182: return "WMADDM";
    case 0x2183: return "WMADDH";
    case 0x4016: return "JOYSER0/JOYOUT";
    case 0x4017: return "JOYSER1";
    case 0x4200: return "NMITIMEN";
    case 0x4201: return "WRIO";
    case 0x4202: return "WRMPYA";
    case 0x4203: return "WRMPYB";
    case 0x4204: return "WRDIVL";
    case 0x4205: return "WRDIVH";
    case 0x4206: return "WRDIVB";
    case 0x4207: return "HTIMEL";
    case 0x4208: return "HTIMEH";
    case 0x4209: return "VTIMEL";
    case 0x420A: return "VTIMEH";
    case 0x420B: return "MDMAEN";
    case 0x420C: return "HDMAEN";
    case 0x420D: return "MEMSEL";
    case 0x4210: return "RDNMI";
    case 0x4211: return "TIMEUP";
    case 0x4212: return "HVBJOY";
    case 0x4213: return "RDIO";
    case 0x4214: return "RDDIVL";
    case 0x4215: return "RDDIVH";
    case 0x4216: return "RDMPYL";
    case 0x4217: return "RDMPYH";
    case 0x4218: return "JOY1L";
    case 0x4219: return "JOY1H";
    case 0x421A: return "JOY2L";
    case 0x421B: return "JOY2H";
    case 0x421C: return "JOY3L";
    case 0x421D: return "JOY3H";
    case 0x421E: return "JOY4L";
    case 0x421F: return "JOY4H";
    default: break;
  }

  if (offset >= 0x4300u && offset <= 0x437Fu) {
    const std::string& name = dmaRegisterNames()[(offset >> 4) & 7u][offset & 0xFu];
    return name;
  }
  return {};
}

std::optional<Decoded> Cpu65816Backend::decode(std::span<const std::uint8_t> image, Address base,
                                               Address at, Context context) const {
  if (at > 0xFFFFFFu || base > 0xFFFFFFu) return std::nullopt;
  if (at < base) return std::nullopt;
  const std::size_t offset = static_cast<std::size_t>(at - base);
  if (offset >= image.size()) return std::nullopt;

  const Cpu65816Mode mode = modeOf(context);
  const std::uint8_t opcode = image[offset];
  const OpcodeInfo& info = kOpcodes[opcode];

  // An immediate under a width the mode does not know has no length to read
  // with; `unreadable` says so first, and a caller that skipped it gets nothing.
  if (!unreadable(image, base, at, context).empty()) return std::nullopt;

  const std::uint8_t extra = cpu65816OperandBytes(info.mode, mode.accumulator8, mode.index8);
  if (offset + 1u + extra > image.size()) return std::nullopt;

  const std::uint8_t first = extra >= 1 ? image[offset + 1] : std::uint8_t{0};
  const std::uint8_t second = extra >= 2 ? image[offset + 2] : std::uint8_t{0};
  const std::uint8_t third = extra >= 3 ? image[offset + 3] : std::uint8_t{0};
  const std::uint16_t word = static_cast<std::uint16_t>(first | (second << 8));
  const Address bank = at & 0xFF0000u;

  Instruction out;
  out.address = at;
  out.opcode = opcode;
  out.length = static_cast<std::uint8_t>(1 + extra);
  out.flow = info.flow;
  out.cycles = costUnder(opcode, mode);
  out.bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(offset),
                   image.begin() + static_cast<std::ptrdiff_t>(offset + out.length));

  const Address after = following(at, out.length);
  std::string operand;
  // What precedes the target when it is written as a symbol: the absolute forms'
  // `!`, the long forms' `>`, nothing before a branch's.
  std::string symbolMarker;
  switch (info.mode) {
    case Mode::Implied:
      break;
    case Mode::Accumulator:
      operand = "A";
      break;
    case Mode::ImmediateM:
    case Mode::ImmediateX:
      operand = extra == 1 ? "#$" + hex8(first) : "#$" + hex16(word);
      break;
    case Mode::ImmediateByte:
      operand = "#$" + hex8(first);
      break;
    case Mode::Direct: operand = "$" + hex8(first); break;
    case Mode::DirectX: operand = "$" + hex8(first) + ",X"; break;
    case Mode::DirectY: operand = "$" + hex8(first) + ",Y"; break;
    case Mode::DirectIndirect: operand = "($" + hex8(first) + ")"; break;
    case Mode::DirectIndirectX: operand = "($" + hex8(first) + ",X)"; break;
    case Mode::DirectIndirectY: operand = "($" + hex8(first) + "),Y"; break;
    case Mode::DirectIndirectLong: operand = "[$" + hex8(first) + "]"; break;
    case Mode::DirectIndirectLongY: operand = "[$" + hex8(first) + "],Y"; break;
    case Mode::StackRelative: operand = "$" + hex8(first) + ",S"; break;
    case Mode::StackRelativeY: operand = "($" + hex8(first) + ",S),Y"; break;

    // The absolute forms name an address in the data bank, which the image
    // cannot say; a jump or call through one lands in the program bank.
    case Mode::Absolute:
      operand = "!$" + hex16(word);
      if (info.flow == Flow::Jump || info.flow == Flow::Call) {
        out.target = bank | word;
        symbolMarker = "!";
      } else {
        out.operandAddress = word;
      }
      break;
    case Mode::AbsoluteX:
      operand = "!$" + hex16(word) + ",X";
      out.operandAddress = word;
      break;
    case Mode::AbsoluteY:
      operand = "!$" + hex16(word) + ",Y";
      out.operandAddress = word;
      break;
    case Mode::AbsoluteLong: {
      const Address address = word | (static_cast<Address>(third) << 16);
      operand = longOperand(address);
      if (info.flow == Flow::Jump || info.flow == Flow::Call) {
        out.target = address;
        symbolMarker = ">";
      } else {
        out.operandAddress = address;
      }
      break;
    }
    case Mode::AbsoluteLongX: {
      const Address address = word | (static_cast<Address>(third) << 16);
      operand = longOperand(address) + ",X";
      out.operandAddress = address;
      break;
    }
    case Mode::AbsoluteIndirect:
      operand = "(!$" + hex16(word) + ")";
      out.operandAddress = word;
      break;
    case Mode::AbsoluteIndirectLong:
      operand = "[!$" + hex16(word) + "]";
      out.operandAddress = word;
      break;
    case Mode::AbsoluteIndexedIndirect:
      operand = "(!$" + hex16(word) + ",X)";
      out.operandAddress = bank | word;
      break;

    // A displacement is added to the address after the instruction and wraps
    // within the program bank.
    case Mode::Relative: {
      const Address target =
          bank | ((after + static_cast<Address>(static_cast<std::int8_t>(first))) & 0xFFFFu);
      operand = longOperand(target);
      out.target = target;
      break;
    }
    case Mode::RelativeLong: {
      const Address target =
          bank | ((after + static_cast<Address>(static_cast<std::int16_t>(word))) & 0xFFFFu);
      operand = longOperand(target);
      out.target = target;
      break;
    }
    case Mode::PushRelative: {
      const Address named =
          bank | ((after + static_cast<Address>(static_cast<std::int16_t>(word))) & 0xFFFFu);
      operand = longOperand(named);
      break;
    }
    case Mode::PushAbsolute:
      operand = "$" + hex16(word);
      break;

    // A block move stores the destination bank first and the source bank
    // second; the dialect writes the source first.
    case Mode::BlockMove:
      operand = "$" + hex8(second) + ",$" + hex8(first);
      break;
  }

  out.text = operand.empty() ? std::string(info.mnemonic)
                             : std::string(info.mnemonic) + " " + operand;
  // Every form with a constant target — a branch, an absolute or long jump or
  // call — takes a symbol where the address was, behind the marker its mode
  // needs.
  if (out.target) {
    out.symbolic = SymbolicText{.before = std::string(info.mnemonic) + " " + symbolMarker,
                                .after = ""};
  }
  const Cpu65816Mode next = cpu65816ModeAfter(opcode, first, mode, out.note);
  return Decoded{.instruction = std::move(out), .next = contextOf(next)};
}

std::string_view Cpu65816Backend::registerName(Address address) const {
  return cpu65816RegisterName(address);
}

std::string Cpu65816Backend::unreadable(std::span<const std::uint8_t> image, Address base,
                                        Address at, Context context) const {
  if (at > 0xFFFFFFu || base > 0xFFFFFFu || at < base) return {};
  const std::size_t offset = static_cast<std::size_t>(at - base);
  if (offset >= image.size()) return {};

  // An immediate's width is the width of the register it loads. Where the mode
  // does not know that width the bytes cannot be read, and the trace is told
  // rather than handed a guess.
  const Cpu65816Mode mode = modeOf(context);
  const OpcodeInfo& info = kOpcodes[image[offset]];
  if (info.mode == Mode::ImmediateM && !mode.accumulatorKnown) {
    return std::string(info.mnemonic) + " # under an accumulator width the trace does not know";
  }
  if (info.mode == Mode::ImmediateX && !mode.indexKnown) {
    return std::string(info.mnemonic) + " # under an index width the trace does not know";
  }
  return {};
}

std::string Cpu65816Backend::describe(Context context) const {
  const Cpu65816Mode mode = modeOf(context);
  auto width = [](bool known, bool eight) -> std::string {
    if (!known) return "?";
    return eight ? "8" : "16";
  };
  return "e=" + std::string(mode.emulation ? "1" : "0") +
         " m=" + width(mode.accumulatorKnown, mode.accumulator8) +
         " x=" + width(mode.indexKnown, mode.index8);
}

bool Cpu65816Backend::conflicts(Context first, Context second) const {
  return (first.bits & kReadingBits) != (second.bits & kReadingBits);
}

std::vector<std::string> Cpu65816Backend::directives(std::optional<Context> before,
                                                     Context now) const {
  const Cpu65816Mode mode = modeOf(now);
  std::optional<Cpu65816Mode> left;
  if (before) left = modeOf(*before);
  std::vector<std::string> out;

  // A region begins in native mode, so emulation has to be said at its start and
  // wherever the instruction above did not leave the chip there. It forces both
  // widths, so nothing more is said under it.
  if (mode.emulation) {
    if (!left || !left->emulation) out.emplace_back("EMULATION");
    return out;
  }
  if (left && left->emulation) out.emplace_back("NATIVE");

  if (mode.accumulatorKnown &&
      (!left || !left->accumulatorKnown || left->accumulator8 != mode.accumulator8)) {
    out.emplace_back(mode.accumulator8 ? "A8" : "A16");
  }
  if (mode.indexKnown && (!left || !left->indexKnown || left->index8 != mode.index8)) {
    out.emplace_back(mode.index8 ? "X8" : "X16");
  }
  return out;
}

const Cpu65816Backend& cpu65816Backend() {
  static const Cpu65816Backend backend;
  return backend;
}

std::optional<Instruction> decodeAt(std::span<const std::uint8_t> image, Address base,
                                    Address address, const Cpu65816Mode& mode) {
  std::optional<Decoded> decoded = cpu65816Backend().decode(image, base, address, contextOf(mode));
  if (!decoded) return std::nullopt;
  return std::move(decoded->instruction);
}

}  // namespace snaggletooth::disasm
