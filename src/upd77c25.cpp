#include "snaggletooth/upd77c25/upd77c25.h"

#include <cstdint>

namespace snaggletooth {

namespace {

using Flags = Upd77c25State;

constexpr std::uint16_t kPcMask = 0x7FF;  // the program counter is 11 bits
constexpr std::uint16_t kRpMask = 0x3FF;  // the data ROM pointer is 10 bits
constexpr std::uint8_t kSpMask = 0x3;     // four stack entries
constexpr std::uint8_t kKlmRamBit = 0x40;  // @KLM reads K from RAM[DP | 40h]

// The bits a program's write to SR reaches: everything but RQM, DRS and bits 6 to 2.
// That bits 6 to 2 read zero is fullsnes alone, and fullsnes marks it with a question.
constexpr std::uint16_t kSrProgramWritable =
    Flags::kSrUsf1 | Flags::kSrUsf0 | Flags::kSrDma | Flags::kSrDrc | Flags::kSrSoc |
    Flags::kSrSic | Flags::kSrEi | Flags::kSrP1 | Flags::kSrP0;

// The word's form, from its top two bits: 0x an ALU word, 10 a jump, 11 a load.
constexpr unsigned formOf(std::uint32_t word) { return (word >> 22) & 0x3u; }
constexpr unsigned kFormJump = 0x2;
constexpr unsigned kFormLoad = 0x3;

// The ALU word's fields.
constexpr bool returnsAfter(std::uint32_t word) { return ((word >> 22) & 0x1u) != 0; }
constexpr Upd77c25Operand operandOf(std::uint32_t word) {
  return static_cast<Upd77c25Operand>((word >> 20) & 0x3u);
}
constexpr Upd77c25AluOp aluOpOf(std::uint32_t word) {
  return static_cast<Upd77c25AluOp>((word >> 16) & 0xFu);
}
constexpr bool selectsAccB(std::uint32_t word) { return ((word >> 15) & 0x1u) != 0; }
constexpr unsigned dplOf(std::uint32_t word) { return (word >> 13) & 0x3u; }
constexpr std::uint8_t dphOf(std::uint32_t word) {
  return static_cast<std::uint8_t>((word >> 9) & 0xFu);
}
constexpr bool decrementsRp(std::uint32_t word) { return ((word >> 8) & 0x1u) != 0; }
constexpr Upd77c25Source sourceOf(std::uint32_t word) {
  return static_cast<Upd77c25Source>((word >> 4) & 0xFu);
}

// The DST field, shared by the ALU word and the load word.
constexpr Upd77c25Destination destinationOf(std::uint32_t word) {
  return static_cast<Upd77c25Destination>(word & 0xFu);
}

// The jump word's fields.
constexpr std::uint16_t branchOf(std::uint32_t word) {
  return static_cast<std::uint16_t>((word >> 13) & 0x1FFu);
}
constexpr std::uint16_t nextAddressOf(std::uint32_t word) {
  return static_cast<std::uint16_t>((word >> 2) & kPcMask);
}

// The load word's immediate.
constexpr std::uint16_t immediateOf(std::uint32_t word) {
  return static_cast<std::uint16_t>((word >> 6) & 0xFFFFu);
}

// The DPL field's adjustments to DP's low nibble.
constexpr unsigned kDplIncrement = 1;
constexpr unsigned kDplDecrement = 2;
constexpr unsigned kDplClear = 3;

constexpr bool flagSet(std::uint8_t flags, std::uint8_t flag) { return (flags & flag) != 0; }

// One ALU operation's result and the flags it leaves.
struct AluResult {
  std::uint16_t value;
  std::uint8_t flags;
};

// The flags of OR, AND, XOR, NOT, the shifts and XCHG: S1 and S0 the result's sign, C
// the bit a shift moved out (0 for the others), Z, and both overflow flags clear.
// fullsnes gives S1 as officially undefined and as the result's sign in practice; the
// sign is fullsnes alone.
constexpr std::uint8_t logicalFlags(std::uint16_t result, bool carry) {
  std::uint8_t flags = 0;
  if ((result & 0x8000u) != 0) flags |= Flags::kFlagS1 | Flags::kFlagS0;
  if (carry) flags |= Flags::kFlagC;
  if (result == 0) flags |= Flags::kFlagZ;
  return flags;
}

// The flags of the additions and subtractions: S0, C and Z from the result, OV0 when
// the signed result left -8000h..7FFFh. On an overflow S1 takes the result's sign and
// OV1 flips; with none, both keep their values.
constexpr std::uint8_t arithmeticFlags(std::uint8_t before, std::uint16_t result,
                                       bool carry, bool overflow) {
  auto flags = static_cast<std::uint8_t>(before & (Flags::kFlagS1 | Flags::kFlagOv1));
  if ((result & 0x8000u) != 0) flags |= Flags::kFlagS0;
  if (carry) flags |= Flags::kFlagC;
  if (result == 0) flags |= Flags::kFlagZ;
  if (overflow) {
    flags |= Flags::kFlagOv0;
    flags = static_cast<std::uint8_t>(flags & ~Flags::kFlagS1);
    if ((result & 0x8000u) != 0) flags |= Flags::kFlagS1;
    flags ^= Flags::kFlagOv1;
  }
  return flags;
}

// `lhs + rhs + carryIn`, with the carry out of bit 15 and the signed overflow.
constexpr AluResult add(std::uint16_t lhs, std::uint16_t rhs, unsigned carryIn,
                        std::uint8_t before) {
  const unsigned sum = unsigned{lhs} + unsigned{rhs} + carryIn;
  const int signedSum = int{static_cast<std::int16_t>(lhs)} +
                        int{static_cast<std::int16_t>(rhs)} + static_cast<int>(carryIn);
  const auto value = static_cast<std::uint16_t>(sum);
  return {value, arithmeticFlags(before, value, sum > 0xFFFFu,
                                 signedSum > 0x7FFF || signedSum < -0x8000)};
}

// `lhs - rhs - borrowIn`, C set on a borrow, with the signed overflow.
constexpr AluResult subtract(std::uint16_t lhs, std::uint16_t rhs, unsigned borrowIn,
                             std::uint8_t before) {
  const int difference = int{lhs} - int{rhs} - static_cast<int>(borrowIn);
  const int signedDifference = int{static_cast<std::int16_t>(lhs)} -
                               int{static_cast<std::int16_t>(rhs)} -
                               static_cast<int>(borrowIn);
  const auto value = static_cast<std::uint16_t>(difference);
  return {value, arithmeticFlags(before, value, difference < 0,
                                 signedDifference > 0x7FFF || signedDifference < -0x8000)};
}

// One ALU operation on `acc` and the operand `p`. `otherCarry` is the C flag of the
// accumulator the instruction does not name, which ADC, SBB and RCL1 take in.
constexpr AluResult compute(Upd77c25AluOp op, std::uint16_t acc, std::uint16_t p,
                            bool otherCarry, std::uint8_t before) {
  const unsigned carryIn = otherCarry ? 1u : 0u;
  switch (op) {
    case Upd77c25AluOp::Nop:
      return {acc, before};
    case Upd77c25AluOp::Or: {
      const auto value = static_cast<std::uint16_t>(acc | p);
      return {value, logicalFlags(value, false)};
    }
    case Upd77c25AluOp::And: {
      const auto value = static_cast<std::uint16_t>(acc & p);
      return {value, logicalFlags(value, false)};
    }
    case Upd77c25AluOp::Xor: {
      const auto value = static_cast<std::uint16_t>(acc ^ p);
      return {value, logicalFlags(value, false)};
    }
    case Upd77c25AluOp::Sub:
      return subtract(acc, p, 0, before);
    case Upd77c25AluOp::Add:
      return add(acc, p, 0, before);
    case Upd77c25AluOp::Sbb:
      return subtract(acc, p, carryIn, before);
    case Upd77c25AluOp::Adc:
      return add(acc, p, carryIn, before);
    case Upd77c25AluOp::Dec:
      return subtract(acc, 1, 0, before);
    case Upd77c25AluOp::Inc:
      return add(acc, 1, 0, before);
    case Upd77c25AluOp::Not: {
      const auto value = static_cast<std::uint16_t>(acc ^ 0xFFFFu);
      return {value, logicalFlags(value, false)};
    }
    case Upd77c25AluOp::Sar1: {
      const auto value = static_cast<std::uint16_t>((acc >> 1) | (acc & 0x8000u));
      return {value, logicalFlags(value, (acc & 0x0001u) != 0)};
    }
    case Upd77c25AluOp::Rcl1: {
      const auto value = static_cast<std::uint16_t>((acc << 1) | carryIn);
      return {value, logicalFlags(value, (acc & 0x8000u) != 0)};
    }
    case Upd77c25AluOp::Sll2: {
      const auto value = static_cast<std::uint16_t>((acc << 2) | 0x0003u);
      return {value, logicalFlags(value, false)};
    }
    case Upd77c25AluOp::Sll4: {
      const auto value = static_cast<std::uint16_t>((acc << 4) | 0x000Fu);
      return {value, logicalFlags(value, false)};
    }
    case Upd77c25AluOp::Xchg: {
      const auto value = static_cast<std::uint16_t>((acc << 8) | (acc >> 8));
      return {value, logicalFlags(value, false)};
    }
  }
  return {acc, before};
}

}  // namespace

void Upd77c25::reset() noexcept {
  state_.pc = 0;
  state_.flagA = 0;
  state_.flagB = 0;
  state_.sr = 0;
  state_.rp = kRpMask;
}

void Upd77c25::stepCycle() noexcept {
  const std::uint32_t word = program_->code[state_.pc & kPcMask];
  state_.pc = static_cast<std::uint16_t>((state_.pc + 1) & kPcMask);
  switch (formOf(word)) {
    case kFormJump:
      executeJump(word);
      break;
    case kFormLoad:
      executeLoad(word);
      break;
    default:
      executeAlu(word);
      break;
  }
}

// The order of the steps inside one ALU instruction is fullsnes alone: fullsnes gives
// the fields and the prohibited combinations, and nothing on the order. The core reads
// the source first, with its side effect; forms the operand; runs the operation on the
// accumulator the A bit names, writing it and its flags; writes the moved value to the
// destination, so a destination naming that same accumulator holds the moved value;
// adjusts DP and RP, so a destination of DP or RP is adjusted after it is written;
// returns when RT is set; and recomputes the product when K or L was written.
void Upd77c25::executeAlu(std::uint32_t word) noexcept {
  const std::uint16_t bus = readSource(sourceOf(word));

  std::uint16_t p = 0;
  switch (operandOf(word)) {
    case Upd77c25Operand::Ram:
      p = state_.ram[state_.dp];
      break;
    case Upd77c25Operand::Bus:
      p = bus;
      break;
    case Upd77c25Operand::ProductHigh:
      p = static_cast<std::uint16_t>(state_.product >> 16);
      break;
    case Upd77c25Operand::ProductLow:
      p = static_cast<std::uint16_t>(state_.product);
      break;
  }

  const bool accB = selectsAccB(word);
  std::uint16_t& acc = accB ? state_.accB : state_.accA;
  std::uint8_t& flags = accB ? state_.flagB : state_.flagA;
  const bool otherCarry = flagSet(accB ? state_.flagA : state_.flagB, Flags::kFlagC);
  const AluResult result = compute(aluOpOf(word), acc, p, otherCarry, flags);
  acc = result.value;
  flags = result.flags;

  const bool factorWritten = writeDestination(destinationOf(word), bus);

  const auto dpLow = static_cast<std::uint8_t>(state_.dp & 0x0Fu);
  std::uint8_t newLow = dpLow;
  switch (dplOf(word)) {
    case kDplIncrement:
      newLow = static_cast<std::uint8_t>((dpLow + 1) & 0x0Fu);
      break;
    case kDplDecrement:
      newLow = static_cast<std::uint8_t>((dpLow - 1) & 0x0Fu);
      break;
    case kDplClear:
      newLow = 0;
      break;
    default:
      break;
  }
  const auto dpHigh = static_cast<std::uint8_t>((state_.dp ^ (dphOf(word) << 4)) & 0xF0u);
  state_.dp = static_cast<std::uint8_t>(dpHigh | newLow);
  if (decrementsRp(word)) state_.rp = static_cast<std::uint16_t>((state_.rp - 1) & kRpMask);

  if (returnsAfter(word)) state_.pc = pop();
  if (factorWritten) updateProduct();
}

// A jump whose condition holds moves PC to NA; CALL first writes the address after the
// jump to the stack. A BRCH value fullsnes gives for no µPD77C25 jump is never taken;
// that is fullsnes alone, which lists those values for no instruction of this chip.
void Upd77c25::executeJump(std::uint32_t word) noexcept {
  const std::uint16_t branch = branchOf(word);
  if (static_cast<Upd77c25Branch>(branch) == Upd77c25Branch::Call) {
    push(state_.pc);
    state_.pc = nextAddressOf(word);
    return;
  }
  if (branchTaken(branch)) state_.pc = nextAddressOf(word);
}

void Upd77c25::executeLoad(std::uint32_t word) noexcept {
  if (writeDestination(destinationOf(word), immediateOf(word))) updateProduct();
}

std::uint16_t Upd77c25::readSource(Upd77c25Source source) noexcept {
  switch (source) {
    case Upd77c25Source::Trb:
      return state_.trb;
    case Upd77c25Source::AccA:
      return state_.accA;
    case Upd77c25Source::AccB:
      return state_.accB;
    case Upd77c25Source::Tr:
      return state_.tr;
    case Upd77c25Source::Dp:
      return state_.dp;
    case Upd77c25Source::Rp:
      return state_.rp;
    case Upd77c25Source::Ro:
      return program_->data[state_.rp & kRpMask];
    case Upd77c25Source::Sgn:
      // fullsnes gives SGN as 8000h less AccA's S1; the A bit does not choose it.
      return static_cast<std::uint16_t>(flagSet(state_.flagA, Flags::kFlagS1) ? 0x7FFF : 0x8000);
    case Upd77c25Source::Dr:
      programTouchedDataRegister();
      return state_.dr;
    case Upd77c25Source::Drnf:
      return state_.dr;
    case Upd77c25Source::Sr:
      return state_.sr;
    case Upd77c25Source::Sim:
    case Upd77c25Source::Sil:
      return 0;
    case Upd77c25Source::K:
      return state_.k;
    case Upd77c25Source::L:
      return state_.l;
    case Upd77c25Source::Mem:
      return state_.ram[state_.dp];
  }
  return 0;
}

bool Upd77c25::writeDestination(Upd77c25Destination destination,
                                std::uint16_t value) noexcept {
  switch (destination) {
    case Upd77c25Destination::None:
    case Upd77c25Destination::Sol:
    case Upd77c25Destination::Som:
      return false;
    case Upd77c25Destination::AccA:
      state_.accA = value;
      return false;
    case Upd77c25Destination::AccB:
      state_.accB = value;
      return false;
    case Upd77c25Destination::Tr:
      state_.tr = value;
      return false;
    case Upd77c25Destination::Dp:
      state_.dp = static_cast<std::uint8_t>(value);
      return false;
    case Upd77c25Destination::Rp:
      state_.rp = static_cast<std::uint16_t>(value & kRpMask);
      return false;
    case Upd77c25Destination::Dr:
      state_.dr = value;
      programTouchedDataRegister();
      return false;
    case Upd77c25Destination::Sr:
      state_.sr = static_cast<std::uint16_t>(
          (value & kSrProgramWritable) | (state_.sr & (Flags::kSrRqm | Flags::kSrDrs)));
      return false;
    case Upd77c25Destination::K:
      state_.k = value;
      return true;
    case Upd77c25Destination::Klr:
      state_.k = value;
      state_.l = program_->data[state_.rp & kRpMask];
      return true;
    case Upd77c25Destination::Klm:
      state_.l = value;
      state_.k = state_.ram[state_.dp | kKlmRamBit];
      return true;
    case Upd77c25Destination::L:
      state_.l = value;
      return true;
    case Upd77c25Destination::Trb:
      state_.trb = value;
      return false;
    case Upd77c25Destination::Mem:
      state_.ram[state_.dp] = value;
      return false;
  }
  return false;
}

bool Upd77c25::branchTaken(std::uint16_t branch) const noexcept {
  const std::uint8_t a = state_.flagA;
  const std::uint8_t b = state_.flagB;
  const unsigned dpLow = state_.dp & 0x0Fu;
  switch (static_cast<Upd77c25Branch>(branch)) {
    case Upd77c25Branch::Jmp:
      return true;
    case Upd77c25Branch::Call:
      return false;
    case Upd77c25Branch::Jnca:
      return !flagSet(a, Flags::kFlagC);
    case Upd77c25Branch::Jca:
      return flagSet(a, Flags::kFlagC);
    case Upd77c25Branch::Jncb:
      return !flagSet(b, Flags::kFlagC);
    case Upd77c25Branch::Jcb:
      return flagSet(b, Flags::kFlagC);
    case Upd77c25Branch::Jnza:
      return !flagSet(a, Flags::kFlagZ);
    case Upd77c25Branch::Jza:
      return flagSet(a, Flags::kFlagZ);
    case Upd77c25Branch::Jnzb:
      return !flagSet(b, Flags::kFlagZ);
    case Upd77c25Branch::Jzb:
      return flagSet(b, Flags::kFlagZ);
    case Upd77c25Branch::Jnova0:
      return !flagSet(a, Flags::kFlagOv0);
    case Upd77c25Branch::Jova0:
      return flagSet(a, Flags::kFlagOv0);
    case Upd77c25Branch::Jnovb0:
      return !flagSet(b, Flags::kFlagOv0);
    case Upd77c25Branch::Jovb0:
      return flagSet(b, Flags::kFlagOv0);
    case Upd77c25Branch::Jnova1:
      return !flagSet(a, Flags::kFlagOv1);
    case Upd77c25Branch::Jova1:
      return flagSet(a, Flags::kFlagOv1);
    case Upd77c25Branch::Jnovb1:
      return !flagSet(b, Flags::kFlagOv1);
    case Upd77c25Branch::Jovb1:
      return flagSet(b, Flags::kFlagOv1);
    case Upd77c25Branch::Jnsa0:
      return !flagSet(a, Flags::kFlagS0);
    case Upd77c25Branch::Jsa0:
      return flagSet(a, Flags::kFlagS0);
    case Upd77c25Branch::Jnsb0:
      return !flagSet(b, Flags::kFlagS0);
    case Upd77c25Branch::Jsb0:
      return flagSet(b, Flags::kFlagS0);
    case Upd77c25Branch::Jnsa1:
      return !flagSet(a, Flags::kFlagS1);
    case Upd77c25Branch::Jsa1:
      return flagSet(a, Flags::kFlagS1);
    case Upd77c25Branch::Jnsb1:
      return !flagSet(b, Flags::kFlagS1);
    case Upd77c25Branch::Jsb1:
      return flagSet(b, Flags::kFlagS1);
    case Upd77c25Branch::Jdpl0:
      return dpLow == 0x0u;
    case Upd77c25Branch::Jdpln0:
      return dpLow != 0x0u;
    case Upd77c25Branch::Jdplf:
      return dpLow == 0xFu;
    case Upd77c25Branch::Jdplnf:
      return dpLow != 0xFu;
    case Upd77c25Branch::Jnsiak:
    case Upd77c25Branch::Jnsoak:
      return true;
    case Upd77c25Branch::Jsiak:
    case Upd77c25Branch::Jsoak:
      return false;
    case Upd77c25Branch::Jnrqm:
      return !requestingHost();
    case Upd77c25Branch::Jrqm:
      return requestingHost();
  }
  return false;
}

// K·L·2 as a signed 16 by 16 bit product, doubled, recomputed after an instruction that
// wrote K or L; an instruction reads the product as it stood before it. Both are
// fullsnes alone, and fullsnes gives the result of -8000h · -8000h · 2 as unknown: the
// core keeps the arithmetic's 32-bit wrap, 80000000h.
void Upd77c25::updateProduct() noexcept {
  const std::int64_t product = std::int64_t{static_cast<std::int16_t>(state_.k)} *
                               std::int64_t{static_cast<std::int16_t>(state_.l)} * 2;
  state_.product = static_cast<std::uint32_t>(product);
}

// The program read or wrote DR through SRC DR or DST @DR: RQM is set and the host's
// next 16-bit access starts at the low byte. That the program's access restarts the
// host's fragments is fullsnes alone, which gives only that DRS toggles per fragment.
void Upd77c25::programTouchedDataRegister() noexcept {
  state_.sr = static_cast<std::uint16_t>((state_.sr | Flags::kSrRqm) & ~Flags::kSrDrs);
}

// The stack is four entries with a two-bit pointer that wraps, so a fifth call writes
// over the first entry; fullsnes gives "4-levels" and nothing past it, so the wrap is
// fullsnes alone.
void Upd77c25::push(std::uint16_t address) noexcept {
  state_.stack[state_.sp & kSpMask] = address;
  state_.sp = static_cast<std::uint8_t>((state_.sp + 1) & kSpMask);
}

std::uint16_t Upd77c25::pop() noexcept {
  state_.sp = static_cast<std::uint8_t>((state_.sp - 1) & kSpMask);
  return static_cast<std::uint16_t>(state_.stack[state_.sp] & kPcMask);
}

// The host's DR access follows SR whether RQM is set or not; fullsnes gives the access
// only as the completion of one the program requested, so the access without a request
// is fullsnes alone.
std::uint8_t Upd77c25::hostRead(bool status) noexcept {
  if (status) return static_cast<std::uint8_t>(state_.sr >> 8);
  if ((state_.sr & Flags::kSrDrc) != 0) {
    state_.sr = static_cast<std::uint16_t>(state_.sr & ~Flags::kSrRqm);
    return static_cast<std::uint8_t>(state_.dr);
  }
  if ((state_.sr & Flags::kSrDrs) == 0) {
    state_.sr = static_cast<std::uint16_t>(state_.sr | Flags::kSrDrs);
    return static_cast<std::uint8_t>(state_.dr);
  }
  state_.sr = static_cast<std::uint16_t>(state_.sr & ~(Flags::kSrDrs | Flags::kSrRqm));
  return static_cast<std::uint8_t>(state_.dr >> 8);
}

// An 8-bit host write replaces DR's low byte and keeps its high byte; fullsnes does not
// say what becomes of the high byte, so keeping it is fullsnes alone.
void Upd77c25::hostWrite(std::uint8_t value) noexcept {
  if ((state_.sr & Flags::kSrDrc) != 0) {
    state_.dr = static_cast<std::uint16_t>((state_.dr & 0xFF00u) | value);
    state_.sr = static_cast<std::uint16_t>(state_.sr & ~Flags::kSrRqm);
    return;
  }
  if ((state_.sr & Flags::kSrDrs) == 0) {
    state_.dr = static_cast<std::uint16_t>((state_.dr & 0xFF00u) | value);
    state_.sr = static_cast<std::uint16_t>(state_.sr | Flags::kSrDrs);
    return;
  }
  state_.dr = static_cast<std::uint16_t>((state_.dr & 0x00FFu) | (unsigned{value} << 8));
  state_.sr = static_cast<std::uint16_t>(state_.sr & ~(Flags::kSrDrs | Flags::kSrRqm));
}

}  // namespace snaggletooth
