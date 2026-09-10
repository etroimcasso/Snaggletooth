#include <array>
#include <stdexcept>
#include <string>

#include "ir/ir_interpret.h"

// The sound CPU's interpreter. Every rule here is the chip's, as the core in
// `snaggletooth/apu/spc700.h` performs it: the flags an operation sets, the page
// a direct operand lives in, the stack in page one, the multiply's and the
// divide's exact arithmetic. This translation unit includes no decoder and no
// listing, and names no byte.

namespace snaggletooth::ir {
namespace {

constexpr std::uint8_t kFlagN = 0x80;
constexpr std::uint8_t kFlagV = 0x40;
constexpr std::uint8_t kFlagP = 0x20;
constexpr std::uint8_t kFlagB = 0x10;
constexpr std::uint8_t kFlagH = 0x08;
constexpr std::uint8_t kFlagI = 0x04;
constexpr std::uint8_t kFlagZ = 0x02;
constexpr std::uint8_t kFlagC = 0x01;

std::uint8_t flagMask(Place flag) {
  switch (flag) {
    case Place::FlagN: return kFlagN;
    case Place::FlagV: return kFlagV;
    case Place::FlagP: return kFlagP;
    case Place::FlagB: return kFlagB;
    case Place::FlagH: return kFlagH;
    case Place::FlagI: return kFlagI;
    case Place::FlagZ: return kFlagZ;
    case Place::FlagC: return kFlagC;
    default: throw std::logic_error("not a flag of the sound CPU");
  }
}

[[noreturn]] void notTheSoundCpus(const char* what) {
  throw std::logic_error(std::string(what) + " has no meaning on the sound CPU");
}

// One node's run: the registers, the temporaries, and the cycles so far.
struct RunSpc700 {
  Spc700Registers& r;
  Bus& bus;
  std::array<std::uint32_t, 4> temps{};
  std::uint32_t cycles = 0;

  // ---- widths and masks ----
  [[nodiscard]] static unsigned bits(Width width) {
    switch (width) {
      case Width::Byte: return 8;
      case Width::Word: return 16;
      case Width::Long:
      case Width::ByM:
      case Width::ByX: break;
    }
    notTheSoundCpus("the width");
  }
  [[nodiscard]] static std::uint32_t mask(unsigned bits) { return (1u << bits) - 1u; }
  [[nodiscard]] static std::uint32_t top(unsigned bits) { return 1u << (bits - 1); }

  // ---- reading and writing places ----
  [[nodiscard]] std::uint32_t raw(Operand operand) const {
    switch (operand.place) {
      case Place::Imm: return operand.value;
      case Place::A: return r.a;
      case Place::X: return r.x;
      case Place::Y: return r.y;
      case Place::S: return r.sp;
      case Place::PC: return r.pc;
      case Place::P: return r.psw;
      case Place::YA: return static_cast<std::uint32_t>(r.y) << 8 | r.a;
      case Place::T0: return temps[0];
      case Place::T1: return temps[1];
      case Place::T2: return temps[2];
      case Place::T3: return temps[3];
      case Place::None: return 0;
      case Place::FlagN:
      case Place::FlagV:
      case Place::FlagP:
      case Place::FlagB:
      case Place::FlagH:
      case Place::FlagI:
      case Place::FlagZ:
      case Place::FlagC:
        return (r.psw & flagMask(operand.place)) != 0 ? 1u : 0u;
      case Place::D:
      case Place::PBR:
      case Place::DBR:
      case Place::E:
      case Place::FlagM:
      case Place::FlagX:
      case Place::FlagD:
        break;
    }
    notTheSoundCpus("the place");
  }
  [[nodiscard]] std::uint32_t at(Operand operand, unsigned bits) const {
    return raw(operand) & mask(bits);
  }

  void put(Place place, std::uint32_t value, unsigned bits) {
    const std::uint32_t v = value & mask(bits);
    switch (place) {
      case Place::A: r.a = static_cast<std::uint8_t>(v); break;
      case Place::X: r.x = static_cast<std::uint8_t>(v); break;
      case Place::Y: r.y = static_cast<std::uint8_t>(v); break;
      case Place::S: r.sp = static_cast<std::uint8_t>(v); break;
      case Place::PC: r.pc = static_cast<std::uint16_t>(v); break;
      case Place::P: r.psw = static_cast<std::uint8_t>(v); break;
      case Place::YA:
        r.a = static_cast<std::uint8_t>(v);
        r.y = static_cast<std::uint8_t>(v >> 8);
        break;
      case Place::T0: temps[0] = v; break;
      case Place::T1: temps[1] = v; break;
      case Place::T2: temps[2] = v; break;
      case Place::T3: temps[3] = v; break;
      case Place::None: break;
      case Place::Imm: throw std::logic_error("a constant is not a destination");
      case Place::FlagN:
      case Place::FlagV:
      case Place::FlagP:
      case Place::FlagB:
      case Place::FlagH:
      case Place::FlagI:
      case Place::FlagZ:
      case Place::FlagC: {
        const std::uint8_t m = flagMask(place);
        r.psw = static_cast<std::uint8_t>((v & 1u) != 0 ? (r.psw | m) : (r.psw & ~m));
        break;
      }
      case Place::D:
      case Place::PBR:
      case Place::DBR:
      case Place::E:
      case Place::FlagM:
      case Place::FlagX:
      case Place::FlagD:
        notTheSoundCpus("the place");
    }
  }

  // ---- flags ----
  void setNZ(std::uint32_t value, unsigned bits) {
    const std::uint32_t v = value & mask(bits);
    r.psw = static_cast<std::uint8_t>((r.psw & ~(kFlagN | kFlagZ)) |
                                      ((v & top(bits)) != 0 ? kFlagN : 0) | (v == 0 ? kFlagZ : 0));
  }
  void setFlag(std::uint8_t flag, bool on) {
    r.psw = static_cast<std::uint8_t>(on ? (r.psw | flag) : (r.psw & ~flag));
  }
  [[nodiscard]] bool flag(std::uint8_t flag) const { return (r.psw & flag) != 0; }

  // ---- addresses ----
  [[nodiscard]] static Address next(Address address, Step step) {
    switch (step) {
      case Step::Flat: return (address + 1u) & 0xFFFFu;
      case Step::Page: return (address & 0xFF00u) | ((address + 1u) & 0xFFu);
      case Step::Bank0:
      case Step::Bank:
      case Step::Direct:
      case Step::DirectPointer: break;
    }
    notTheSoundCpus("the step");
  }

  // ---- the bus ----
  [[nodiscard]] std::uint32_t load(Address address, unsigned bits, Step step, Access access) {
    std::uint32_t value = 0;
    Address at = address & 0xFFFFu;
    for (unsigned byte = 0; byte < bits / 8; ++byte) {
      value |= static_cast<std::uint32_t>(bus.read(at, access)) << (8 * byte);
      at = next(at, step);
    }
    return value;
  }
  void store(Address address, std::uint32_t value, unsigned bits, Step step, Access access) {
    Address at = address & 0xFFFFu;
    for (unsigned byte = 0; byte < bits / 8; ++byte) {
      bus.write(at, static_cast<std::uint8_t>(value >> (8 * byte)), access);
      at = next(at, step);
    }
  }

  // ---- the stack, in page one ----
  void pushByte(std::uint8_t value) {
    bus.write(0x0100u | r.sp, value, Access::Data);
    r.sp = static_cast<std::uint8_t>(r.sp - 1u);
  }
  std::uint8_t pullByte() {
    r.sp = static_cast<std::uint8_t>(r.sp + 1u);
    return bus.read(0x0100u | r.sp, Access::Data);
  }

  // ---- arithmetic ----
  // An eight-bit add with carry sets N, V, H, Z and C from the binary sum; a
  // subtract adds the ones' complement with the same carry in. A sixteen-bit add
  // chains two byte adds with the carry threaded between them, and takes N, V, H
  // and C from the high byte's add and Z from the whole word.
  [[nodiscard]] std::uint32_t addWithCarry(std::uint32_t a, std::uint32_t operand, bool subtract,
                                           unsigned bits) {
    const std::uint32_t m = (subtract ? ~operand : operand) & mask(bits);
    const std::uint32_t cin = flag(kFlagC) ? 1u : 0u;
    std::uint8_t psw = static_cast<std::uint8_t>(r.psw & ~(kFlagN | kFlagV | kFlagH | kFlagZ | kFlagC));
    std::uint32_t result;
    if (bits == 8) {
      const std::uint32_t sum = a + m + cin;
      result = sum & 0xFFu;
      if (((a & 0x0Fu) + (m & 0x0Fu) + cin) > 0x0Fu) psw |= kFlagH;
      if ((~(a ^ m) & (a ^ sum) & 0x80u) != 0) psw |= kFlagV;
      if (sum > 0xFFu) psw |= kFlagC;
    } else {
      const std::uint32_t loA = a & 0xFFu, loB = m & 0xFFu;
      const std::uint32_t hiA = (a >> 8) & 0xFFu, hiB = (m >> 8) & 0xFFu;
      const std::uint32_t lo = loA + loB + cin;
      const std::uint32_t hi = hiA + hiB + (lo >> 8);
      result = ((hi & 0xFFu) << 8) | (lo & 0xFFu);
      if (((hiA & 0x0Fu) + (hiB & 0x0Fu) + (lo >> 8)) > 0x0Fu) psw |= kFlagH;
      if ((~(hiA ^ hiB) & (hiA ^ hi) & 0x80u) != 0) psw |= kFlagV;
      if (hi > 0xFFu) psw |= kFlagC;
    }
    if ((result & top(bits)) != 0) psw |= kFlagN;
    if (result == 0) psw |= kFlagZ;
    r.psw = psw;
    return result;
  }

  // The divide: nine steps of restoring division over a 17-bit accumulator, so a
  // quotient past 511 leaves what the chip leaves. N and Z come from the quotient,
  // V from the seventeenth bit, and H from the low nibbles of the divisor and the
  // dividend's high byte as they were.
  [[nodiscard]] std::uint32_t divide(std::uint32_t ya, std::uint32_t x) {
    const std::uint32_t entryY = (ya >> 8) & 0xFFu;
    std::uint32_t yva = ya & 0xFFFFu;
    const std::uint32_t x9 = (x & 0xFFu) << 9;
    for (int i = 0; i < 9; ++i) {
      yva = ((yva << 1) | ((yva >> 16) & 1u)) & 0x1FFFFu;
      if (yva >= x9) yva ^= 1u;
      if (yva & 1u) yva = (yva - x9) & 0x1FFFFu;
    }
    const std::uint32_t quotient = yva & 0xFFu;
    const std::uint32_t remainder = (yva >> 9) & 0xFFu;
    std::uint8_t psw = static_cast<std::uint8_t>(r.psw & ~(kFlagN | kFlagV | kFlagH | kFlagZ));
    if (quotient & 0x80u) psw |= kFlagN;
    if (quotient == 0) psw |= kFlagZ;
    if (yva & 0x100u) psw |= kFlagV;
    if ((x & 0x0Fu) <= (entryY & 0x0Fu)) psw |= kFlagH;
    r.psw = psw;
    return (remainder << 8) | quotient;
  }

  // The decimal adjusts: sixty is added or taken from a value past ninety-nine
  // or with the carry set, six likewise on the half carry or a low nibble past
  // nine; C moves with the sixty and H is read but never written.
  [[nodiscard]] std::uint32_t decimalAdjust(std::uint32_t a, bool subtract) {
    std::uint32_t v = a & 0xFFu;
    if (!subtract) {
      if (flag(kFlagC) || v > 0x99u) {
        v += 0x60u;
        setFlag(kFlagC, true);
      }
      if (flag(kFlagH) || (a & 0x0Fu) > 0x09u) v += 0x06u;
    } else {
      if (!flag(kFlagC) || v > 0x99u) {
        v -= 0x60u;
        setFlag(kFlagC, false);
      }
      if (!flag(kFlagH) || (a & 0x0Fu) > 0x09u) v -= 0x06u;
    }
    v &= 0xFFu;
    setNZ(v, 8);
    return v;
  }

  // ---- conditions ----
  [[nodiscard]] static Operand placeOperand(Place place) {
    Operand o;
    o.place = place;
    return o;
  }
  [[nodiscard]] bool holds(const Cond& cond) const {
    if (cond.andEmulation) notTheSoundCpus("the condition");
    switch (cond.when) {
      case When::Always: return true;
      case When::FlagSet: return flag(flagMask(cond.place));
      case When::FlagClear: return !flag(flagMask(cond.place));
      case When::PlaceIs: return raw(placeOperand(cond.place)) == cond.value;
      case When::PlaceIsNot: return raw(placeOperand(cond.place)) != cond.value;
      case When::Emulation:
      case When::Native:
      case When::DirectLowByte:
      case When::IndexCrossed: break;
    }
    notTheSoundCpus("the condition");
  }

  // ---- one effect ----
  void apply(const Effect& e) {
    if (!holds(e.when)) return;
    const unsigned w = bits(e.width);
    switch (e.op) {
      case Op::Set: put(e.dst.place, at(e.a, w), w); break;
      case Op::SetNZ: {
        const std::uint32_t v = at(e.a, w);
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Add: put(e.dst.place, raw(e.a) + raw(e.b), w); break;
      case Op::Sub: put(e.dst.place, raw(e.a) - raw(e.b), w); break;
      case Op::And: put(e.dst.place, raw(e.a) & raw(e.b), w); break;
      case Op::Or: put(e.dst.place, raw(e.a) | raw(e.b), w); break;
      case Op::Xor: put(e.dst.place, raw(e.a) ^ raw(e.b), w); break;
      case Op::Shr: put(e.dst.place, raw(e.a) >> raw(e.b), w); break;
      case Op::Shl: put(e.dst.place, raw(e.a) << raw(e.b), w); break;

      case Op::PageAddress: {
        const std::uint32_t page = flag(kFlagP) ? 0x0100u : 0x0000u;
        put(e.dst.place, page | ((raw(e.a) + raw(e.b)) & 0xFFu), 16);
        break;
      }

      case Op::Load: put(e.dst.place, load(raw(e.a), w, e.step, e.access), w); break;
      case Op::Store: store(raw(e.a), at(e.b, w), w, e.step, e.access); break;
      case Op::Push: {
        const std::uint32_t v = at(e.a, w);
        if (w == 16) pushByte(static_cast<std::uint8_t>(v >> 8));
        pushByte(static_cast<std::uint8_t>(v));
        break;
      }
      case Op::Pull: {
        std::uint32_t v = pullByte();
        if (w == 16) v |= static_cast<std::uint32_t>(pullByte()) << 8;
        put(e.dst.place, v, w);
        break;
      }

      case Op::Adc: put(e.dst.place, addWithCarry(at(e.a, w), at(e.b, w), false, w), w); break;
      case Op::Sbc: put(e.dst.place, addWithCarry(at(e.a, w), at(e.b, w), true, w), w); break;
      case Op::Cmp: {
        const std::uint32_t a = at(e.a, w);
        const std::uint32_t b = at(e.b, w);
        setNZ(a - b, w);
        setFlag(kFlagC, a >= b);
        break;
      }
      case Op::Asl: {
        const std::uint32_t a = at(e.a, w);
        setFlag(kFlagC, (a & top(w)) != 0);
        const std::uint32_t v = (a << 1) & mask(w);
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Lsr: {
        const std::uint32_t a = at(e.a, w);
        setFlag(kFlagC, (a & 1u) != 0);
        const std::uint32_t v = a >> 1;
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Rol: {
        const std::uint32_t a = at(e.a, w);
        const std::uint32_t cin = flag(kFlagC) ? 1u : 0u;
        setFlag(kFlagC, (a & top(w)) != 0);
        const std::uint32_t v = ((a << 1) | cin) & mask(w);
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Ror: {
        const std::uint32_t a = at(e.a, w);
        const std::uint32_t cin = flag(kFlagC) ? top(w) : 0u;
        setFlag(kFlagC, (a & 1u) != 0);
        const std::uint32_t v = (a >> 1) | cin;
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Inc: {
        const std::uint32_t v = (at(e.a, w) + 1u) & mask(w);
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Dec: {
        const std::uint32_t v = (at(e.a, w) - 1u) & mask(w);
        put(e.dst.place, v, w);
        setNZ(v, w);
        break;
      }
      case Op::Daa: put(e.dst.place, decimalAdjust(at(e.a, 8), false), 8); break;
      case Op::Das: put(e.dst.place, decimalAdjust(at(e.a, 8), true), 8); break;
      case Op::Mul: {
        const std::uint32_t product = at(e.a, 8) * at(e.b, 8);
        put(e.dst.place, product, 16);
        setNZ(product >> 8, 8);
        break;
      }
      case Op::Div: put(e.dst.place, divide(at(e.a, 16), at(e.b, 8)), 16); break;

      case Op::Halt: r.run = raw(e.a) == 0 ? Run::Waiting : Run::Stopped; break;
      case Op::Cycles: cycles += raw(e.a); break;

      case Op::DirectAddress:
      case Op::BankAddress:
      case Op::LongAddress:
      case Op::ProgramAddress:
      case Op::StackAddress:
      case Op::StoreRmw:
      case Op::SettleStack:
      case Op::Bit:
      case Op::BitImm:
      case Op::Tsb:
      case Op::Trb:
      case Op::WriteP:
      case Op::Xba:
      case Op::Xce:
        notTheSoundCpus("the operation");
    }
  }
};

}  // namespace

std::uint32_t Spc700Interpreter::execute(const Node& node, Bus& bus) {
  RunSpc700 run{registers, bus};
  run.cycles = node.cost.base[0];
  for (effectIndex = 0; effectIndex < node.effects.size(); ++effectIndex) {
    run.apply(node.effects[effectIndex]);
  }
  return run.cycles;
}

}  // namespace snaggletooth::ir
