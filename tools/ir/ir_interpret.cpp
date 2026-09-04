#include "ir/ir_interpret.h"

#include <array>
#include <stdexcept>

namespace snaggletooth::ir {
namespace {

constexpr std::uint8_t kFlagN = 0x80;
constexpr std::uint8_t kFlagV = 0x40;
constexpr std::uint8_t kFlagM = 0x20;
constexpr std::uint8_t kFlagX = 0x10;
constexpr std::uint8_t kFlagD = 0x08;
constexpr std::uint8_t kFlagI = 0x04;
constexpr std::uint8_t kFlagZ = 0x02;
constexpr std::uint8_t kFlagC = 0x01;

std::uint8_t flagMask(Place flag) {
  switch (flag) {
    case Place::FlagN: return kFlagN;
    case Place::FlagV: return kFlagV;
    case Place::FlagM: return kFlagM;
    case Place::FlagX: return kFlagX;
    case Place::FlagD: return kFlagD;
    case Place::FlagI: return kFlagI;
    case Place::FlagZ: return kFlagZ;
    case Place::FlagC: return kFlagC;
    default: throw std::logic_error("not a flag");
  }
}

// One node's run: the registers, the temporaries, and what the effects so far
// have established.
struct Run65816 {
  Registers& r;
  Bus& bus;
  std::array<std::uint32_t, 4> temps{};
  bool crossed = false;  // the last bank-relative address's low-byte addition carried
  std::uint32_t cycles = 0;

  // ---- widths and masks ----
  [[nodiscard]] unsigned bits(Width width) const {
    switch (width) {
      case Width::Byte: return 8;
      case Width::Word: return 16;
      case Width::Long: return 24;
      case Width::ByM: return r.accumulator8() ? 8 : 16;
      case Width::ByX: return r.index8() ? 8 : 16;
    }
    return 8;
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
      case Place::S: return r.s;
      case Place::D: return r.d;
      case Place::PC: return r.pc;
      case Place::PBR: return r.pbr;
      case Place::DBR: return r.dbr;
      case Place::P: return r.p;
      case Place::E: return r.e ? 1u : 0u;
      case Place::T0: return temps[0];
      case Place::T1: return temps[1];
      case Place::T2: return temps[2];
      case Place::T3: return temps[3];
      case Place::None: return 0;
      default: return (r.p & flagMask(operand.place)) != 0 ? 1u : 0u;
    }
  }
  [[nodiscard]] std::uint32_t at(Operand operand, unsigned bits) const {
    return raw(operand) & mask(bits);
  }

  // A register written at eight bits follows its own rule: the accumulator keeps
  // its high byte, an index register clears its own, and the rest take the value
  // as it is.
  void put(Place place, std::uint32_t value, unsigned bits) {
    const std::uint32_t v = value & mask(bits);
    switch (place) {
      case Place::A:
        r.a = bits == 8 ? static_cast<std::uint16_t>((r.a & 0xFF00u) | v)
                        : static_cast<std::uint16_t>(v);
        break;
      case Place::X: r.x = static_cast<std::uint16_t>(v); break;
      case Place::Y: r.y = static_cast<std::uint16_t>(v); break;
      case Place::S: r.s = static_cast<std::uint16_t>(v); break;
      case Place::D: r.d = static_cast<std::uint16_t>(v); break;
      case Place::PC: r.pc = static_cast<std::uint16_t>(v); break;
      case Place::PBR: r.pbr = static_cast<std::uint8_t>(v); break;
      case Place::DBR: r.dbr = static_cast<std::uint8_t>(v); break;
      case Place::P: r.p = static_cast<std::uint8_t>(v); break;
      case Place::E: r.e = (v & 1u) != 0; break;
      case Place::T0: temps[0] = v; break;
      case Place::T1: temps[1] = v; break;
      case Place::T2: temps[2] = v; break;
      case Place::T3: temps[3] = v; break;
      case Place::None: break;
      case Place::Imm: throw std::logic_error("a constant is not a destination");
      default: {
        const std::uint8_t m = flagMask(place);
        r.p = static_cast<std::uint8_t>((v & 1u) != 0 ? (r.p | m) : (r.p & ~m));
        break;
      }
    }
  }

  // ---- flags ----
  void setNZ(std::uint32_t value, unsigned bits) {
    const std::uint32_t v = value & mask(bits);
    r.p = static_cast<std::uint8_t>((r.p & ~(kFlagN | kFlagZ)) |
                                    ((v & top(bits)) != 0 ? kFlagN : 0) | (v == 0 ? kFlagZ : 0));
  }
  void setFlag(std::uint8_t flag, bool on) {
    r.p = static_cast<std::uint8_t>(on ? (r.p | flag) : (r.p & ~flag));
  }
  [[nodiscard]] bool flag(std::uint8_t flag) const { return (r.p & flag) != 0; }

  // The invariants the chip holds between instructions.
  void normalize() {
    if (r.index8()) {
      r.x &= 0xFFu;
      r.y &= 0xFFu;
    }
    if (r.e) r.s = static_cast<std::uint16_t>(0x0100u | (r.s & 0xFFu));
  }

  // ---- addresses ----
  [[nodiscard]] bool directPageWrap() const { return r.e && (r.d & 0xFFu) == 0; }
  [[nodiscard]] Address next(Address address, Step step) const {
    switch (step) {
      case Step::Flat: return (address + 1u) & 0xFFFFFFu;
      case Step::Bank0: return (address + 1u) & 0xFFFFu;
      case Step::Bank: return (address & 0xFF0000u) | ((address + 1u) & 0xFFFFu);
      case Step::Direct:
        if (directPageWrap()) return (address & 0xFF00u) | ((address + 1u) & 0xFFu);
        return (address + 1u) & 0xFFFFu;
      case Step::DirectPointer:
        if (r.e && r.d == 0) return (address & 0xFF00u) | ((address + 1u) & 0xFFu);
        return (address + 1u) & 0xFFFFu;
    }
    return (address + 1u) & 0xFFFFFFu;
  }

  // ---- the bus ----
  [[nodiscard]] std::uint32_t load(Address address, unsigned bits, Step step, Access access) {
    std::uint32_t value = 0;
    Address at = address & 0xFFFFFFu;
    for (unsigned byte = 0; byte < bits / 8; ++byte) {
      value |= static_cast<std::uint32_t>(bus.read(at, access)) << (8 * byte);
      at = next(at, step);
    }
    return value;
  }
  void store(Address address, std::uint32_t value, unsigned bits, Step step, Access access) {
    Address at = address & 0xFFFFFFu;
    for (unsigned byte = 0; byte < bits / 8; ++byte) {
      bus.write(at, static_cast<std::uint8_t>(value >> (8 * byte)), access);
      at = next(at, step);
    }
  }
  // A read-modify-write's write-back: the high byte first, at the stepped
  // address, then the low byte at the address itself.
  void storeHighFirst(Address address, std::uint32_t value, unsigned bits, Step step,
                      Access access) {
    const Address low = address & 0xFFFFFFu;
    if (bits == 16) bus.write(next(low, step), static_cast<std::uint8_t>(value >> 8), access);
    bus.write(low, static_cast<std::uint8_t>(value), access);
  }

  // ---- the stack ----
  void pushByte(std::uint8_t value, bool pinned) {
    bus.write(r.s, value, Access::Data);
    r.s = (r.e && pinned) ? static_cast<std::uint16_t>(0x0100u | ((r.s - 1u) & 0xFFu))
                          : static_cast<std::uint16_t>(r.s - 1u);
  }
  std::uint8_t pullByte(bool pinned) {
    r.s = (r.e && pinned) ? static_cast<std::uint16_t>(0x0100u | ((r.s + 1u) & 0xFFu))
                          : static_cast<std::uint16_t>(r.s + 1u);
    return bus.read(r.s, Access::Data);
  }
  void settle() {
    if (r.e) r.s = static_cast<std::uint16_t>(0x0100u | (r.s & 0xFFu));
  }

  // ---- arithmetic ----
  // ADC and SBC share one adder, the way the chip does: SBC adds the ones'
  // complement of the operand with the same carry in. Binary mode is a plain add;
  // decimal mode adjusts each nibble. N, Z and C describe the result at the
  // width; V comes from the binary sum, except for a decimal ADC, which takes it
  // from the high-nibble sum before the final adjust.
  [[nodiscard]] std::uint32_t addWithCarry(std::uint32_t a, std::uint32_t operand, bool subtract,
                                           unsigned bits) {
    const std::uint32_t cin = flag(kFlagC) ? 1u : 0u;
    const bool decimal = flag(kFlagD);
    const std::uint32_t m = (subtract ? ~operand : operand) & mask(bits);
    const std::uint32_t bin = a + m + cin;
    bool carry = (bin & (1u << bits)) != 0;
    bool overflow = ((~(a ^ m) & (a ^ bin)) & top(bits)) != 0;
    std::uint32_t result = bin & mask(bits);
    if (decimal && !subtract) {
      std::uint32_t t = 0;
      std::uint32_t carryNibble = cin;
      for (unsigned nibble = 0; nibble < bits / 4; ++nibble) {
        const unsigned shift = 4 * nibble;
        std::uint32_t sum =
            ((a >> shift) & 0xFu) + ((operand >> shift) & 0xFu) + carryNibble;
        const bool last = nibble + 1 == bits / 4;
        if (last) {
          // The top nibble decides V before it is adjusted, and C after.
          t |= sum << shift;
          overflow = ((~(a ^ operand) & (a ^ t)) & top(bits)) != 0;
          if (sum > 9) t += 6u << shift;
          carry = t > mask(bits);
          result = t & mask(bits);
        } else {
          if (sum > 9) {
            sum = ((sum + 6u) & 0xFu);
            carryNibble = 1;
          } else {
            carryNibble = 0;
          }
          t |= sum << shift;
        }
      }
    } else if (decimal) {
      int borrow = static_cast<int>(1u - cin);
      std::uint32_t res = 0;
      for (unsigned nibble = 0; nibble < bits / 4; ++nibble) {
        const int an = static_cast<int>((a >> (4 * nibble)) & 0xFu);
        const int mn = static_cast<int>((operand >> (4 * nibble)) & 0xFu);
        int d = an - mn - borrow;
        if (d < 0) {
          d -= 6;
          borrow = 1;
        } else {
          borrow = 0;
        }
        res |= static_cast<std::uint32_t>(d & 0xF) << (4 * nibble);
      }
      result = res;
    }
    setNZ(result, bits);
    setFlag(kFlagC, carry);
    setFlag(kFlagV, overflow);
    return result;
  }

  // ---- conditions ----
  [[nodiscard]] static Operand placeOperand(Place place) {
    Operand o;
    o.place = place;
    return o;
  }
  [[nodiscard]] bool holds(const Cond& cond) const {
    if (cond.andEmulation && !r.e) return false;
    switch (cond.when) {
      case When::Always: return true;
      case When::Emulation: return r.e;
      case When::Native: return !r.e;
      case When::FlagSet: return flag(flagMask(cond.place));
      case When::FlagClear: return !flag(flagMask(cond.place));
      case When::PlaceIs: return raw(placeOperand(cond.place)) == cond.value;
      case When::PlaceIsNot: return raw(placeOperand(cond.place)) != cond.value;
      case When::DirectLowByte: return (r.d & 0xFFu) != 0;
      case When::IndexCrossed: return r.index8() && crossed;
    }
    return false;
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

      case Op::DirectAddress: {
        const std::uint32_t offset = raw(e.a) & 0xFFu;
        const std::uint32_t index = raw(e.b);
        const std::uint32_t address =
            directPageWrap() ? ((r.d & 0xFF00u) | ((offset + index) & 0xFFu))
                             : ((r.d + offset + index) & 0xFFFFu);
        put(e.dst.place, address, 24);
        break;
      }
      case Op::BankAddress: {
        const std::uint32_t offset = raw(e.a) & 0xFFFFu;
        const std::uint32_t index = raw(e.b);
        crossed = ((offset & 0xFFu) + (index & 0xFFu)) > 0xFFu;
        put(e.dst.place, (static_cast<std::uint32_t>(r.dbr) << 16 | offset) + index, 24);
        break;
      }
      case Op::LongAddress: put(e.dst.place, (raw(e.a) & 0xFFFFFFu) + raw(e.b), 24); break;
      case Op::ProgramAddress:
        put(e.dst.place, static_cast<std::uint32_t>(r.pbr) << 16 | (raw(e.a) & 0xFFFFu), 24);
        break;
      case Op::StackAddress: put(e.dst.place, (r.s + raw(e.a)) & 0xFFFFu, 24); break;

      case Op::Load: put(e.dst.place, load(raw(e.a), w, e.step, e.access), w); break;
      case Op::Store: store(raw(e.a), at(e.b, w), w, e.step, e.access); break;
      case Op::StoreRmw: storeHighFirst(raw(e.a), at(e.b, w), w, e.step, e.access); break;
      case Op::Push: {
        const std::uint32_t v = at(e.a, w);
        if (w == 16) pushByte(static_cast<std::uint8_t>(v >> 8), e.pinned);
        pushByte(static_cast<std::uint8_t>(v), e.pinned);
        break;
      }
      case Op::Pull: {
        std::uint32_t v = pullByte(e.pinned);
        if (w == 16) v |= static_cast<std::uint32_t>(pullByte(e.pinned)) << 8;
        put(e.dst.place, v, w);
        break;
      }
      case Op::SettleStack: settle(); break;

      case Op::Adc:
        put(e.dst.place, addWithCarry(at(e.a, w), at(e.b, w), false, w), w);
        break;
      case Op::Sbc:
        put(e.dst.place, addWithCarry(at(e.a, w), at(e.b, w), true, w), w);
        break;
      case Op::Cmp: {
        const std::uint32_t a = at(e.a, w);
        const std::uint32_t b = at(e.b, w);
        setNZ(a - b, w);
        setFlag(kFlagC, a >= b);
        break;
      }
      case Op::Bit: {
        const std::uint32_t b = at(e.b, w);
        setFlag(kFlagZ, (at(e.a, w) & b) == 0);
        setFlag(kFlagN, (b & top(w)) != 0);
        setFlag(kFlagV, (b & (top(w) >> 1)) != 0);
        break;
      }
      case Op::BitImm: setFlag(kFlagZ, (at(e.a, w) & at(e.b, w)) == 0); break;
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
      case Op::Tsb:
      case Op::Trb: {
        const std::uint32_t a = at(e.a, w);
        const std::uint32_t b = at(e.b, w);
        setFlag(kFlagZ, (a & b) == 0);
        put(e.dst.place, e.op == Op::Tsb ? (a | b) : (a & ~b), w);
        break;
      }
      case Op::WriteP: {
        std::uint8_t v = static_cast<std::uint8_t>(at(e.a, 8));
        if (r.e) v = static_cast<std::uint8_t>(v | kFlagM | kFlagX);
        r.p = v;
        normalize();
        break;
      }
      case Op::Xba:
        r.a = static_cast<std::uint16_t>(((r.a & 0xFFu) << 8) | ((r.a >> 8) & 0xFFu));
        setNZ(r.a & 0xFFu, 8);
        break;
      case Op::Xce: {
        const bool carry = flag(kFlagC);
        setFlag(kFlagC, r.e);
        r.e = carry;
        if (r.e) {
          r.p = static_cast<std::uint8_t>(r.p | kFlagM | kFlagX);
          r.x &= 0xFFu;
          r.y &= 0xFFu;
          r.s = static_cast<std::uint16_t>(0x0100u | (r.s & 0xFFu));
        }
        break;
      }
      case Op::Halt: r.run = raw(e.a) == 0 ? Run::Waiting : Run::Stopped; break;
      case Op::Cycles: cycles += raw(e.a); break;
    }
  }
};

}  // namespace

std::uint32_t Interpreter::execute(const Node& node, Bus& bus) {
  Run65816 run{registers, bus};
  run.normalize();
  run.cycles = node.cost.base[costIndex(registers.accumulator8(), registers.index8())];
  for (effectIndex = 0; effectIndex < node.effects.size(); ++effectIndex) {
    run.apply(node.effects[effectIndex]);
  }
  return run.cycles;
}

std::uint32_t Interpreter::interrupt(const std::vector<Effect>& sequence, Bus& bus) {
  release();
  Run65816 run{registers, bus};
  run.normalize();
  for (effectIndex = 0; effectIndex < sequence.size(); ++effectIndex) {
    run.apply(sequence[effectIndex]);
  }
  return run.cycles;
}

}  // namespace snaggletooth::ir
