#include "ir/ir_dataflow.h"

#include <algorithm>
#include <array>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "cpu65816_disasm.h"
#include "ir/ir_render.h"

namespace snaggletooth::ir {
namespace {

// ---- sets of values -------------------------------------------------------------

std::uint32_t maskOf(unsigned bits) { return bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u; }

// `values` sorted and distinct, or not known when there are too many to be
// worth knowing.
Values settle(std::vector<std::uint32_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (values.size() > kMostValues) return Values::none();
  return Values{true, std::move(values), std::nullopt, 0};
}

// The bits every value of a set leaves zero, or the zero bits of a value that is
// not known.
std::uint32_t zeroBitsOf(const Values& v, unsigned bits) {
  const std::uint32_t mask = maskOf(bits);
  if (!v.known) return v.zeroBits & mask;
  std::uint32_t set = 0;
  for (const std::uint32_t x : v.values) set |= x;
  return ~set & mask;
}

// A value that is not known, with these bits known to be zero.
Values zeros(std::uint32_t zeroBits) {
  Values v;
  v.zeroBits = zeroBits;
  return v;
}

// What two paths prove together: the values either proves when both prove
// some; the one name when neither does and both carry it; nothing otherwise.
Values unionOf(const Values& a, const Values& b) {
  if (!a.known || !b.known) {
    if (!a.known && !b.known && a.symbol && a.symbol == b.symbol) return a;
    return zeros(zeroBitsOf(a, 32) & zeroBitsOf(b, 32));
  }
  std::vector<std::uint32_t> out(a.values);
  out.insert(out.end(), b.values.begin(), b.values.end());
  return settle(std::move(out));
}

// `f` over every value; a name does not survive arithmetic.
template <typename F>
Values mapValues(const Values& a, F f) {
  if (!a.known) return Values::none();
  std::vector<std::uint32_t> out;
  out.reserve(a.values.size());
  for (const std::uint32_t v : a.values) out.push_back(f(v));
  return settle(std::move(out));
}

template <typename F>
Values product(const Values& a, const Values& b, F f) {
  if (!a.known || !b.known) return Values::none();
  if (a.values.size() * b.values.size() > 4u * kMostValues) return Values::none();
  std::vector<std::uint32_t> out;
  out.reserve(a.values.size() * b.values.size());
  for (const std::uint32_t x : a.values) {
    for (const std::uint32_t y : b.values) out.push_back(f(x, y));
  }
  return settle(std::move(out));
}

template <typename F>
Values filter(const Values& a, F keep) {
  if (!a.known) return Values::none();
  std::vector<std::uint32_t> out;
  for (const std::uint32_t v : a.values) {
    if (keep(v)) out.push_back(v);
  }
  return Values{true, std::move(out), std::nullopt, 0};
}

// Every value a mask admits: the subsets of its bits, when there are few enough.
Values submasks(std::uint32_t mask) {
  std::vector<std::uint32_t> out;
  std::uint32_t subset = 0;
  do {
    out.push_back(subset);
    if (out.size() > kMostValues) return zeros(~mask);
    subset = (subset - mask) & mask;
  } while (subset != 0);
  return settle(std::move(out));
}

// The values below `bound`.
Values below(std::uint32_t bound) {
  if (bound > kMostValues) return Values::none();
  std::vector<std::uint32_t> out;
  for (std::uint32_t v = 0; v < bound; ++v) out.push_back(v);
  return Values{true, std::move(out), std::nullopt, 0};
}

// A name's width in bits: a byte of a register, or a whole sixteen-bit one.
unsigned symbolBits(const Symbol& symbol) { return symbol.part == 2 ? 16 : 8; }

// The bytes of a value. A whole name splits into its two byte names; a byte name
// is its own low byte; a stack pointer that has moved is neither.
Values lowByte(const Values& v) {
  if (v.known) return mapValues(v, [](std::uint32_t x) { return x & 0xFFu; });
  if (v.symbol && v.symbol->offset == 0 && v.symbol->part != 1) {
    return Values{false, {}, Symbol{v.symbol->place, 0, 0}, 0};
  }
  return zeros(v.zeroBits & 0xFFu);
}
Values highByte(const Values& v) {
  if (v.known) return mapValues(v, [](std::uint32_t x) { return (x >> 8) & 0xFFu; });
  if (v.symbol && v.symbol->offset == 0 && v.symbol->part == 2) {
    return Values{false, {}, Symbol{v.symbol->place, 1, 0}, 0};
  }
  return zeros((v.zeroBits >> 8) & 0xFFu);
}
Values join16(const Values& low, const Values& high) {
  if (low.known && high.known) {
    return product(low, high, [](std::uint32_t l, std::uint32_t h) { return (h << 8) | l; });
  }
  if (!low.known && !high.known && low.symbol && high.symbol && low.symbol->place == high.symbol->place &&
      low.symbol->part == 0 && high.symbol->part == 1 && low.symbol->offset == 0 &&
      high.symbol->offset == 0) {
    return Values{false, {}, Symbol{low.symbol->place, 2, 0}, 0};
  }
  return zeros(zeroBitsOf(low, 8) | (zeroBitsOf(high, 8) << 8));
}

// ---- the state's meet -------------------------------------------------------------

RegisterState meet(const RegisterState& a, const RegisterState& b) {
  return RegisterState{.aLow = unionOf(a.aLow, b.aLow),
                       .aHigh = unionOf(a.aHigh, b.aHigh),
                       .xLow = unionOf(a.xLow, b.xLow),
                       .xHigh = unionOf(a.xHigh, b.xHigh),
                       .yLow = unionOf(a.yLow, b.yLow),
                       .yHigh = unionOf(a.yHigh, b.yHigh),
                       .d = unionOf(a.d, b.d),
                       .s = unionOf(a.s, b.s),
                       .dbr = unionOf(a.dbr, b.dbr)};
}

State meet(const State& a, const State& b) {
  State out;
  out.registers = meet(a.registers, b.registers);
  out.compare = (a.compare && b.compare && *a.compare == *b.compare) ? a.compare : std::nullopt;
  // Two paths that pushed the same depth agree byte by byte; two that did not
  // leave nothing a pull can trust.
  if (a.pushed.size() == b.pushed.size()) {
    out.pushed.reserve(a.pushed.size());
    for (std::size_t i = 0; i < a.pushed.size(); ++i) out.pushed.push_back(unionOf(a.pushed[i], b.pushed[i]));
  }
  return out;
}

bool softwareInterrupt(const Node& node) {
  return node.instruction.mnemonic == "BRK" || node.instruction.mnemonic == "COP";
}

bool blockMove(const Node& node) {
  return node.instruction.mnemonic == "MVN" || node.instruction.mnemonic == "MVP";
}

// ---- one node over a state --------------------------------------------------------

struct Abstract {
  const Node& node;
  const ImageReader& image;
  const StackReach& stack;
  State state;
  Values pc;
  Values pbr;
  std::array<Values, 4> temps;
  // Of a temporary holding a bank-relative address whose bank is not known, the
  // sixteen-bit offsets it can hold — enough to say whether a store through it
  // could reach the stack, which lives in bank zero and its mirrors.
  std::array<Values, 4> offsets;
  std::vector<ProvenAccess> accesses;
  bool emulation;

  Abstract(const Node& n, const State& before, const ImageReader& reader, const StackReach& reach)
      : node(n), image(reader), stack(reach), state(before), emulation(n.mode.emulation) {
    pc = Values::one(n.instruction.address & 0xFFFFu);
    pbr = Values::one(n.instruction.address >> 16);
    for (Values& t : temps) t = Values::one(0);
    for (Values& o : offsets) o = Values::none();
    state.compare = std::nullopt;
    // The invariants the chip holds between instructions: the index high bytes
    // zero while the index registers are eight bits wide, the stack in page one
    // under emulation.
    if (emulation || (n.mode.indexKnown && n.mode.index8)) {
      state.registers.xHigh = Values::one(0);
      state.registers.yHigh = Values::one(0);
    }
    if (emulation) state.registers.s = mapValues(state.registers.s, pinned);
  }

  static std::uint32_t pinned(std::uint32_t s) { return 0x0100u | (s & 0xFFu); }

  // The width of an effect, or nothing where a width the trace did not settle
  // decides it.
  [[nodiscard]] std::optional<unsigned> bits(Width width) const {
    switch (width) {
      case Width::Byte: return 8;
      case Width::Word: return 16;
      case Width::Long: return 24;
      case Width::ByM:
        if (emulation) return 8;
        if (!node.mode.accumulatorKnown) return std::nullopt;
        return node.mode.accumulator8 ? 8 : 16;
      case Width::ByX:
        if (emulation) return 8;
        if (!node.mode.indexKnown) return std::nullopt;
        return node.mode.index8 ? 8 : 16;
    }
    return 8;
  }

  // ---- reading and writing places ----
  [[nodiscard]] Values raw(Operand operand) const {
    const RegisterState& r = state.registers;
    switch (operand.place) {
      case Place::Imm: return Values::one(operand.value);
      case Place::A: return r.a();
      case Place::X: return r.x();
      case Place::Y: return r.y();
      case Place::S: return r.s;
      case Place::D: return r.d;
      case Place::PC: return pc;
      case Place::PBR: return pbr;
      case Place::DBR: return r.dbr;
      case Place::T0: return temps[0];
      case Place::T1: return temps[1];
      case Place::T2: return temps[2];
      case Place::T3: return temps[3];
      case Place::None: return Values::one(0);
      default: return Values::none();  // P, E and the flags are not followed
    }
  }
  // A place at a width: a byte of the three two-byte registers, or the whole
  // masked. A name survives a mask no narrower than it.
  [[nodiscard]] Values at(Operand operand, unsigned width) const {
    const RegisterState& r = state.registers;
    if (width == 8) {
      switch (operand.place) {
        case Place::A: return r.aLow;
        case Place::X: return r.xLow;
        case Place::Y: return r.yLow;
        default: break;
      }
    }
    const Values value = raw(operand);
    const std::uint32_t mask = maskOf(width);
    if (!value.known) {
      if (value.symbol && width >= symbolBits(*value.symbol)) return value;
      return zeros((value.zeroBits & mask) | ~mask);
    }
    return mapValues(value, [mask](std::uint32_t v) { return v & mask; });
  }

  // A register written at eight bits follows its own rule: the accumulator
  // keeps its high byte, an index register clears its own. A name written to a
  // place wide enough for it is kept.
  void put(Place place, const Values& value, unsigned width) {
    RegisterState& r = state.registers;
    const std::uint32_t mask = maskOf(width);
    Values v = value;
    if (v.known) {
      v = mapValues(value, [mask](std::uint32_t x) { return x & mask; });
    } else if (v.symbol && width < symbolBits(*v.symbol)) {
      v = zeros(v.zeroBits & mask);
    } else if (!v.symbol) {
      v = zeros(v.zeroBits & mask);
    }
    auto word = [](const Values& x) {
      if (!x.known) return x;
      return mapValues(x, [](std::uint32_t y) { return y & 0xFFFFu; });
    };
    switch (place) {
      case Place::A:
        if (width == 8) {
          r.aLow = v;
        } else {
          r.aLow = lowByte(v);
          r.aHigh = highByte(v);
        }
        break;
      case Place::X:
        r.xLow = lowByte(v);
        r.xHigh = width == 8 ? Values::one(0) : highByte(v);
        break;
      case Place::Y:
        r.yLow = lowByte(v);
        r.yHigh = width == 8 ? Values::one(0) : highByte(v);
        break;
      case Place::S: r.s = word(v); break;
      case Place::D: r.d = word(v); break;
      case Place::PC: pc = word(v); break;
      case Place::PBR: pbr = lowByte(v); break;
      case Place::DBR: r.dbr = lowByte(v); break;
      case Place::T0: temps[0] = v; offsets[0] = Values::none(); break;
      case Place::T1: temps[1] = v; offsets[1] = Values::none(); break;
      case Place::T2: temps[2] = v; offsets[2] = Values::none(); break;
      case Place::T3: temps[3] = v; offsets[3] = Values::none(); break;
      default: break;
    }
  }

  [[nodiscard]] Values offsetsOf(Operand operand) const {
    switch (operand.place) {
      case Place::T0: return offsets[0];
      case Place::T1: return offsets[1];
      case Place::T2: return offsets[2];
      case Place::T3: return offsets[3];
      default: return Values::none();
    }
  }

  // Not knowing a register any more: both bytes of the three that have two.
  void forget(Place place) {
    RegisterState& r = state.registers;
    switch (place) {
      case Place::A: r.aLow = r.aHigh = Values::none(); break;
      case Place::X: r.xLow = r.xHigh = Values::none(); break;
      case Place::Y: r.yLow = r.yHigh = Values::none(); break;
      case Place::S: r.s = Values::none(); break;
      case Place::D: r.d = Values::none(); break;
      case Place::DBR: r.dbr = Values::none(); break;
      case Place::PC: pc = Values::none(); break;
      case Place::PBR: pbr = Values::none(); break;
      case Place::T0: temps[0] = Values::none(); break;
      case Place::T1: temps[1] = Values::none(); break;
      case Place::T2: temps[2] = Values::none(); break;
      case Place::T3: temps[3] = Values::none(); break;
      default: break;
    }
  }

  // ---- the stack ----
  // The stack pointer moved by `delta` bytes: within page one under emulation for
  // the forms the 6502 had; a named stack pointer, in native mode, keeps its name
  // and counts the move.
  void moveStack(int delta, bool pin) {
    Values& s = state.registers.s;
    if (s.known) {
      const bool e = emulation;
      s = mapValues(s, [e, pin, delta](std::uint32_t value) {
        const std::uint32_t moved = (value + static_cast<std::uint32_t>(delta)) & 0xFFFFu;
        return (e && pin) ? pinned(moved) : moved;
      });
    } else if (s.symbol && s.symbol->place == Place::S && s.symbol->part == 2 && !emulation) {
      s.symbol->offset += delta;
    } else {
      s = Values::none();
    }
  }
  // A byte pushed joins what this path has pushed; a byte pulled is the last one
  // pushed, or not known when the path pushed nothing it still holds.
  void pushByte(const Values& byte, bool pin) {
    moveStack(-1, pin);
    state.pushed.push_back(byte);
  }
  Values pullByte(bool pin) {
    moveStack(1, pin);
    if (state.pushed.empty()) return Values::none();
    Values byte = state.pushed.back();
    state.pushed.pop_back();
    return byte;
  }
  void settleStack() {
    if (emulation) state.registers.s = mapValues(state.registers.s, pinned);
  }
  // A store the stack could be under: an address the stack cannot lie at is no
  // threat; one it could lie at is checked against the bytes this path has
  // pushed when the stack pointer is known, and when it is not, could be any of
  // them. Either way what was pushed is not known after such a store. A path
  // that has pushed nothing it still holds has nothing to lose.
  [[nodiscard]] bool stackCouldBeAt(Address address) const {
    return stack ? stack(address) : address < 0x10000u;
  }
  void storeMayReachStack(const Values& addresses, const Values& knownOffsets) {
    if (state.pushed.empty()) return;
    // An address whose bank is not known but whose offset is could be the
    // stack's only where bank zero's own memory at that offset could be.
    const Values& candidates = addresses.known ? addresses : knownOffsets;
    if (!candidates.known) {
      state.pushed.clear();
      return;
    }
    const std::optional<std::uint32_t> s = state.registers.s.single();
    const std::uint32_t depth = static_cast<std::uint32_t>(state.pushed.size());
    for (const std::uint32_t address : candidates.values) {
      if (!stackCouldBeAt(address)) continue;
      const std::uint32_t offset = address & 0xFFFFu;
      if (!s || (offset > *s && offset <= *s + depth)) {
        state.pushed.clear();
        return;
      }
    }
  }

  // ---- the image ----
  // The bytes at every address of `addresses`, stepping as `step` says, or not
  // known where any lies outside the image or the step depends on a direct
  // register that is not known.
  [[nodiscard]] Values load(const Values& addresses, unsigned width, Step step) const {
    if (!addresses.known) return Values::none();
    std::optional<std::uint32_t> wrapLow;  // whether the direct page wraps: needs D
    if (emulation && (step == Step::Direct || step == Step::DirectPointer)) {
      const std::optional<std::uint32_t> d = state.registers.d.single();
      if (!d) return Values::none();
      wrapLow = step == Step::Direct ? ((*d & 0xFFu) == 0 ? 1u : 0u) : (*d == 0 ? 1u : 0u);
    }
    std::vector<std::uint32_t> out;
    for (const std::uint32_t start : addresses.values) {
      std::uint32_t value = 0;
      Address at = start & 0xFFFFFFu;
      for (unsigned byte = 0; byte < width / 8; ++byte) {
        const std::optional<std::uint8_t> read = image(at);
        if (!read) return Values::none();
        value |= static_cast<std::uint32_t>(*read) << (8 * byte);
        switch (step) {
          case Step::Flat: at = (at + 1u) & 0xFFFFFFu; break;
          case Step::Bank0: at = (at + 1u) & 0xFFFFu; break;
          case Step::Bank: at = (at & 0xFF0000u) | ((at + 1u) & 0xFFFFu); break;
          case Step::Direct:
          case Step::DirectPointer:
            at = (wrapLow && *wrapLow) ? ((at & 0xFF00u) | ((at + 1u) & 0xFFu))
                                       : ((at + 1u) & 0xFFFFu);
            break;
        }
      }
      out.push_back(value);
    }
    return settle(std::move(out));
  }

  // ---- one effect ----
  [[nodiscard]] bool holds(const Cond& cond) const {
    switch (cond.when) {
      case When::Always: return true;
      case When::Emulation: return emulation;
      case When::Native: return !emulation;
      default: return false;  // a flag, a count or a carry the analysis does not follow
    }
  }

  void apply(const Effect& e, std::size_t index) {
    if (e.op == Op::Cycles || !holds(e.when)) return;
    const std::optional<unsigned> width = bits(e.width);

    switch (e.op) {
      case Op::Set:
      case Op::SetNZ:
        if (width) put(e.dst.place, at(e.a, *width), *width);
        else forget(e.dst.place);
        break;

      case Op::Add:
      case Op::Sub:
      case Op::And:
      case Op::Or:
      case Op::Xor:
      case Op::Shr: {
        if (!width) {
          forget(e.dst.place);
          break;
        }
        const Values a = raw(e.a);
        const Values b = raw(e.b);
        Values v;
        switch (e.op) {
          case Op::Add: v = product(a, b, [](std::uint32_t x, std::uint32_t y) { return x + y; }); break;
          case Op::Sub: v = product(a, b, [](std::uint32_t x, std::uint32_t y) { return x - y; }); break;
          case Op::And:
            // A mask over a value that is not known bounds it to the mask's
            // subsets: the one thing a mask says about any value.
            if (!a.known && b.single()) {
              v = submasks(*b.single() & maskOf(*width) & ~a.zeroBits);
            } else {
              v = product(a, b, [](std::uint32_t x, std::uint32_t y) { return x & y; });
            }
            break;
          case Op::Or: v = product(a, b, [](std::uint32_t x, std::uint32_t y) { return x | y; }); break;
          case Op::Xor: v = product(a, b, [](std::uint32_t x, std::uint32_t y) { return x ^ y; }); break;
          default: v = product(a, b, [](std::uint32_t x, std::uint32_t y) { return y >= 32 ? 0u : x >> y; }); break;
        }
        put(e.dst.place, v, *width);
        break;
      }

      case Op::DirectAddress: {
        const Values offset = lowByte(raw(e.a));
        const Values indexed = raw(e.b);
        const Values sum = product(offset, indexed, [](std::uint32_t o, std::uint32_t i) { return o + i; });
        const bool e1 = emulation;
        put(e.dst.place,
            product(state.registers.d, sum,
                    [e1](std::uint32_t d, std::uint32_t oi) {
                      const bool wrap = e1 && (d & 0xFFu) == 0;
                      return wrap ? ((d & 0xFF00u) | (oi & 0xFFu)) : ((d + oi) & 0xFFFFu);
                    }),
            24);
        break;
      }
      case Op::BankAddress: {
        const Values offset = mapValues(raw(e.a), [](std::uint32_t v) { return v & 0xFFFFu; });
        const Values base = product(state.registers.dbr, offset, [](std::uint32_t bank, std::uint32_t o) {
          return (bank << 16) | o;
        });
        const Values address = product(base, raw(e.b), [](std::uint32_t b, std::uint32_t i) { return b + i; });
        put(e.dst.place, address, 24);
        if (!address.known) {
          const Values within = product(offset, raw(e.b), [](std::uint32_t o, std::uint32_t i) { return (o + i) & 0xFFFFu; });
          switch (e.dst.place) {
            case Place::T0: offsets[0] = within; break;
            case Place::T1: offsets[1] = within; break;
            case Place::T2: offsets[2] = within; break;
            case Place::T3: offsets[3] = within; break;
            default: break;
          }
        }
        break;
      }
      case Op::LongAddress:
        put(e.dst.place,
            product(raw(e.a), raw(e.b), [](std::uint32_t a, std::uint32_t i) { return (a & 0xFFFFFFu) + i; }),
            24);
        break;
      case Op::ProgramAddress:
        put(e.dst.place,
            product(pbr, raw(e.a), [](std::uint32_t bank, std::uint32_t a) { return (bank << 16) | (a & 0xFFFFu); }),
            24);
        break;
      case Op::StackAddress:
        put(e.dst.place,
            product(state.registers.s, raw(e.a), [](std::uint32_t s, std::uint32_t a) { return (s + a) & 0xFFFFu; }),
            24);
        break;

      case Op::Load: {
        const Values addresses = mapValues(raw(e.a), [](std::uint32_t v) { return v & 0xFFFFFFu; });
        const Values value = width ? load(addresses, *width, e.step) : Values::none();
        accesses.push_back(ProvenAccess{.effect = index, .op = e.op, .width = e.width, .address = addresses, .value = value});
        if (width) put(e.dst.place, value, *width);
        else forget(e.dst.place);
        break;
      }
      case Op::Store:
      case Op::StoreRmw: {
        const Values addresses = mapValues(raw(e.a), [](std::uint32_t v) { return v & 0xFFFFFFu; });
        // Under a width the trace did not settle, the low byte is written either
        // way and is what the access carries.
        Values value = at(e.b, width ? *width : 8);
        if (!value.known) value = Values::none();
        accesses.push_back(ProvenAccess{.effect = index, .op = e.op, .width = e.width, .address = addresses, .value = value});
        storeMayReachStack(addresses, offsetsOf(e.a));
        break;
      }

      case Op::Push: {
        if (!width) {
          state.pushed.clear();
          forget(Place::S);
          break;
        }
        const Values v = at(e.a, *width);
        if (*width == 16) pushByte(highByte(v), e.pinned);
        pushByte(lowByte(v), e.pinned);
        break;
      }
      case Op::Pull: {
        if (!width) {
          state.pushed.clear();
          forget(Place::S);
          forget(e.dst.place);
          break;
        }
        const Values low = pullByte(e.pinned);
        Values v = low;
        if (*width == 16) v = join16(low, pullByte(e.pinned));
        put(e.dst.place, v, *width);
        break;
      }
      case Op::SettleStack: settleStack(); break;

      case Op::Adc:
      case Op::Sbc:
      case Op::Rol:
      case Op::Ror:
        forget(e.dst.place);  // the carry, and the decimal flag, are not followed
        break;
      case Op::Cmp:
        if (width && e.b.place == Place::Imm &&
            (e.a.place == Place::A || e.a.place == Place::X || e.a.place == Place::Y)) {
          state.compare = Compare{.place = e.a.place, .value = e.b.value & maskOf(*width), .bits = *width};
        }
        break;
      case Op::Bit:
      case Op::BitImm:
        break;
      case Op::Asl:
      case Op::Lsr:
      case Op::Inc:
      case Op::Dec: {
        if (!width) {
          forget(e.dst.place);
          break;
        }
        const std::uint32_t mask = maskOf(*width);
        const Values a = at(e.a, *width);
        Values v;
        switch (e.op) {
          // A shift of a value that is not known still clears the bit it shifts in.
          case Op::Asl:
            v = a.known ? mapValues(a, [mask](std::uint32_t x) { return (x << 1) & mask; })
                        : zeros(((a.zeroBits << 1) | 1u) & mask);
            break;
          case Op::Lsr:
            v = a.known ? mapValues(a, [](std::uint32_t x) { return x >> 1; })
                        : zeros((a.zeroBits >> 1) | (1u << (*width - 1)));
            break;
          case Op::Inc: v = mapValues(a, [mask](std::uint32_t x) { return (x + 1u) & mask; }); break;
          default: v = mapValues(a, [mask](std::uint32_t x) { return (x - 1u) & mask; }); break;
        }
        put(e.dst.place, v, *width);
        break;
      }
      case Op::Tsb:
      case Op::Trb: {
        if (!width) {
          forget(e.dst.place);
          break;
        }
        const Values a = at(e.a, *width);
        const Values b = at(e.b, *width);
        put(e.dst.place,
            e.op == Op::Tsb ? product(a, b, [](std::uint32_t x, std::uint32_t y) { return x | y; })
                            : product(a, b, [](std::uint32_t x, std::uint32_t y) { return x & ~y; }),
            *width);
        break;
      }
      case Op::WriteP:
        // A narrowing clears the index high bytes and a widening leaves them;
        // the node after says which it was, and holds the zero if so.
        state.registers.xHigh = unionOf(state.registers.xHigh, Values::one(0));
        state.registers.yHigh = unionOf(state.registers.yHigh, Values::one(0));
        break;
      case Op::Xba:
        std::swap(state.registers.aLow, state.registers.aHigh);
        break;
      case Op::Xce:
        state.registers.xHigh = unionOf(state.registers.xHigh, Values::one(0));
        state.registers.yHigh = unionOf(state.registers.yHigh, Values::one(0));
        state.registers.s = unionOf(state.registers.s, mapValues(state.registers.s, pinned));
        break;
      case Op::Halt:
      case Op::Cycles:
        break;
    }
  }
};

// ---- modes ------------------------------------------------------------------------

disasm::Cpu65816Mode cpuMode(const Mode& mode) {
  return disasm::Cpu65816Mode{.emulation = mode.emulation,
                              .accumulator8 = mode.accumulator8,
                              .index8 = mode.index8,
                              .accumulatorKnown = mode.accumulatorKnown,
                              .indexKnown = mode.indexKnown,
                              .carryKnown = false,
                              .carry = false};
}

Mode irMode(const disasm::Cpu65816Mode& mode) {
  return Mode{.emulation = mode.emulation,
              .accumulator8 = mode.accumulator8,
              .index8 = mode.index8,
              .accumulatorKnown = mode.accumulatorKnown,
              .indexKnown = mode.indexKnown};
}

// The mode execution carries out of a node, as the trace carries it. `XCE` is
// resolved by the trace from the carry it knew about, which the node does not
// carry; a successor is then found by address alone.
Mode modeAfter(const Node& node) {
  std::string note;
  const std::uint8_t operand = static_cast<std::uint8_t>(node.instruction.operand & 0xFFu);
  return irMode(disasm::cpu65816ModeAfter(opcodeOf(node.instruction), operand, cpuMode(node.mode), note));
}

// The flag a conditional branch tests, and the state that takes it.
struct BranchOn {
  Place flag = Place::None;
  bool whenSet = false;
};

BranchOn branchOn(const Node& node) {
  for (const Effect& e : node.effects) {
    if (e.op != Op::Set || e.dst.place != Place::PC) continue;
    if (e.when.when == When::FlagSet) return {e.when.place, true};
    if (e.when.when == When::FlagClear) return {e.when.place, false};
  }
  return {};
}

}  // namespace

// ---- the public pieces -------------------------------------------------------------

Values RegisterState::a() const { return join16(aLow, aHigh); }
Values RegisterState::x() const { return join16(xLow, xHigh); }
Values RegisterState::y() const { return join16(yLow, yHigh); }

State resetState() {
  State state;
  state.registers.d = Values::one(0);
  state.registers.dbr = Values::one(0);
  return state;
}

State nothingProven() { return State{}; }

Evaluation evaluate(const Node& node, const State& before, const ImageReader& image,
                    const StackReach& stack) {
  Abstract run(node, before, image, stack);
  for (std::size_t i = 0; i < node.effects.size(); ++i) run.apply(node.effects[i], i);
  if (blockMove(node)) {
    // The move runs its node once per byte and ends with the count run past
    // zero; the indexes have stepped by a count the analysis does not follow.
    run.state.registers.aLow = Values::one(0xFFu);
    run.state.registers.aHigh = Values::one(0xFFu);
    run.forget(Place::X);
    run.forget(Place::Y);
  }
  Evaluation out;
  out.after = std::move(run.state);
  out.accesses = std::move(run.accesses);
  out.pc = std::move(run.pc);
  out.pbr = std::move(run.pbr);
  return out;
}

// ---- the analysis ----------------------------------------------------------------

namespace {

// The registers a routine gives back as it took them, as bits.
enum Transparent : unsigned { TA = 1u, TX = 2u, TY = 4u, TD = 8u, TDBR = 16u };

// What a routine does to whoever calls it: the nodes it holds, its returns, the
// roots its own calls name, which registers come back as they went in, whether
// the stack pointer does, and whether anything on its paths is beyond the
// analysis — a jump the bytes do not name, a software interrupt, an address with
// no node — in which case nothing is known after a call to it.
struct Summary {
  std::set<std::size_t> held;
  std::vector<std::size_t> returns;
  std::vector<std::size_t> callees;
  unsigned transparent = 0;
  bool balanced = false;
  bool opaque = false;
};

struct Graph {
  const Program& program;
  const Canonical& canonical;
  std::vector<std::vector<std::size_t>> sightingsOf;  // per node: the sighting targets' nodes
  std::vector<std::optional<std::size_t>> next;       // per node: the node after it
  std::vector<std::optional<std::size_t>> target;     // per node: its constant target's node

  // The nodes at an address: the range of indexes.
  [[nodiscard]] std::pair<std::size_t, std::size_t> at(Address address) const {
    const auto& nodes = program.nodes;
    auto first = std::lower_bound(nodes.begin(), nodes.end(), address,
                                  [](const Node& n, Address a) { return n.instruction.address < a; });
    auto last = first;
    while (last != nodes.end() && last->instruction.address == address) ++last;
    return {static_cast<std::size_t>(first - nodes.begin()), static_cast<std::size_t>(last - nodes.begin())};
  }

  // The node a path from `from` reaches at `address`: the one decoded under the
  // mode the path carries there when the address reads more than one way.
  [[nodiscard]] std::optional<std::size_t> nodeFor(std::size_t from, Address address) const {
    const std::optional<Address> home = canonical(address);
    if (!home) return std::nullopt;
    const auto [first, last] = at(*home);
    if (first == last) return std::nullopt;
    if (last - first == 1) return first;
    const Mode after = modeAfter(program.nodes[from]);
    for (std::size_t i = first; i < last; ++i) {
      if (program.nodes[i].mode == after) return i;
    }
    return first;
  }

  // The successors a walk follows without entering a call: fall-through,
  // branch target, jump target, sightings. `beyond` is set where a jump names
  // nothing the analysis can follow.
  [[nodiscard]] std::vector<std::size_t> walkSuccessors(std::size_t i, bool& beyond) const {
    const Node& node = program.nodes[i];
    std::vector<std::size_t> out;
    auto push = [&](const std::optional<std::size_t>& s) {
      if (s) out.push_back(*s);
      else beyond = true;
    };
    switch (node.instruction.flow) {
      case Flow::Continue:
        push(next[i]);
        break;
      case Flow::Call:
        if (softwareInterrupt(node)) beyond = true;
        push(next[i]);
        break;
      case Flow::Branch:
        push(next[i]);
        push(target[i]);
        break;
      case Flow::Jump:
        if (target[i]) {
          out.push_back(*target[i]);
        } else if (!sightingsOf[i].empty()) {
          out.insert(out.end(), sightingsOf[i].begin(), sightingsOf[i].end());
        } else {
          beyond = true;
        }
        break;
      case Flow::Return:
      case Flow::Halt:
        break;
    }
    return out;
  }

  // The roots a call names: its constant target, or the sightings at it.
  [[nodiscard]] std::vector<std::size_t> callees(std::size_t i) const {
    const Node& node = program.nodes[i];
    if (node.instruction.flow != Flow::Call || softwareInterrupt(node)) return {};
    if (target[i]) return {*target[i]};
    return sightingsOf[i];
  }
};

Summary walk(const Graph& graph, std::size_t root) {
  Summary summary;
  std::vector<std::size_t> pending = {root};
  while (!pending.empty()) {
    const std::size_t i = pending.back();
    pending.pop_back();
    if (!summary.held.insert(i).second) continue;
    const Node& node = graph.program.nodes[i];
    if (node.instruction.flow == Flow::Return) summary.returns.push_back(i);
    if (node.instruction.flow == Flow::Call && !softwareInterrupt(node)) {
      const std::vector<std::size_t> callees = graph.callees(i);
      if (callees.empty()) summary.opaque = true;
      summary.callees.insert(summary.callees.end(), callees.begin(), callees.end());
    }
    bool beyond = false;
    for (const std::size_t s : graph.walkSuccessors(i, beyond)) pending.push_back(s);
    if (beyond) summary.opaque = true;
  }
  std::sort(summary.callees.begin(), summary.callees.end());
  summary.callees.erase(std::unique(summary.callees.begin(), summary.callees.end()), summary.callees.end());
  return summary;
}

State unknownState() { return State{}; }

// What a call brings back to the instruction after it, given what the caller
// held before and after the call instruction and what the routine's return
// proves: a register the routine gives back as it took it is the caller's, the
// stack pointer is the caller's before the call when the routine is balanced,
// and everything else is the return's. A routine beyond the analysis never
// reaches here; its caller is given nothing known.
State returned(const Summary& summary, const State& callerBefore, const State& callerAfter,
               const std::optional<State>& atReturn) {
  State state = atReturn ? *atReturn : unknownState();
  const RegisterState& c = callerAfter.registers;
  RegisterState& r = state.registers;
  if (summary.transparent & TA) { r.aLow = c.aLow; r.aHigh = c.aHigh; }
  if (summary.transparent & TX) { r.xLow = c.xLow; r.xHigh = c.xHigh; }
  if (summary.transparent & TY) { r.yLow = c.yLow; r.yHigh = c.yHigh; }
  if (summary.transparent & TD) r.d = c.d;
  if (summary.transparent & TDBR) r.dbr = c.dbr;
  if (summary.balanced) r.s = callerBefore.registers.s;
  state.compare = std::nullopt;
  state.pushed.clear();
  return state;
}

// The propagation: `in` is what every path so far proves before a node, and a
// node whose `in` changes is run again, until nothing changes. The main run
// covers the whole program and follows calls into their routines; a probe covers
// one routine's own nodes from a named entry and answers each call from its
// callee's summary.
class Propagation {
 public:
  Propagation(const Graph& graph, const ImageReader& image, const StackReach& stack,
              const std::map<std::size_t, Summary>& summaries)
      : graph_(graph), image_(image), stack_(stack), summaries_(summaries), in_(graph.program.nodes.size()),
        out_(graph.program.nodes.size()), visits_(graph.program.nodes.size(), 0),
        queued_(graph.program.nodes.size(), false) {}

  // Restricts the run to these nodes: a probe.
  void restrict(const std::set<std::size_t>& held) { held_ = &held; }

  void join(std::size_t i, const State& state) {
    if (held_ && !held_->count(i)) return;
    if (!in_[i]) {
      in_[i] = state;
    } else {
      State merged = meet(*in_[i], state);
      if (merged == *in_[i]) return;
      in_[i] = std::move(merged);
    }
    if (!queued_[i]) {
      queued_[i] = true;
      pending_.push_back(i);
    }
  }

  // Runs to the fixed point. `callersOf` says which call sites a return node
  // returns to, for the main run; a probe passes none and records its returns.
  void run(const std::vector<std::vector<std::size_t>>* callersOf, std::vector<DerivedTarget>* derived) {
    std::set<std::tuple<Address, Address, Address>> derivedSeen;
    while (!pending_.empty()) {
      const std::size_t i = pending_.front();
      pending_.pop_front();
      queued_[i] = false;
      const Node& node = graph_.program.nodes[i];
      // A node run many times over is a loop whose counter the sets are counting
      // up; what is more than one value there is let go, which keeps the fixed
      // point near.
      if (++visits_[i] > 2048u) {
        RegisterState& r = in_[i]->registers;
        for (Values* v : {&r.aLow, &r.aHigh, &r.xLow, &r.xHigh, &r.yLow, &r.yHigh, &r.d, &r.s, &r.dbr}) {
          if (v->known && v->values.size() > 1) *v = Values::none();
        }
      }
      Evaluation ev = evaluate(node, *in_[i], image_, stack_);
      out_[i] = ev.after;

      // A jump or call through a table: the indexed forms, whose pointer the
      // index selects. The plain and long indirect forms read a pointer that is
      // a value in memory, not a slot of a table, and are left to the run.
      const Flow flow = node.instruction.flow;
      if (derived && node.instruction.addressing == Addressing::AbsoluteIndexedIndirect) {
        deriveFrom(i, ev, derivedSeen, *derived);
      }

      switch (flow) {
        case Flow::Continue:
          if (graph_.next[i]) join(*graph_.next[i], ev.after);
          break;
        case Flow::Halt:
          break;
        case Flow::Branch:
          if (graph_.next[i]) join(*graph_.next[i], refined(i, ev.after, false));
          if (graph_.target[i]) join(*graph_.target[i], refined(i, ev.after, true));
          break;
        case Flow::Jump:
          if (graph_.target[i]) {
            join(*graph_.target[i], ev.after);
          } else {
            for (const std::size_t s : graph_.sightingsOf[i]) join(s, ev.after);
          }
          break;
        case Flow::Call: {
          if (softwareInterrupt(node)) {
            if (graph_.next[i]) join(*graph_.next[i], unknownState());
            break;
          }
          if (!held_) {
            State into = ev.after;
            into.compare = std::nullopt;
            into.pushed.clear();
            for (const std::size_t root : graph_.callees(i)) join(root, into);
          }
          fireReturns(i);
          break;
        }
        case Flow::Return:
          if (callersOf) {
            for (const std::size_t site : (*callersOf)[i]) fireReturns(site);
          }
          break;
      }
    }
  }

  [[nodiscard]] const std::optional<State>& in(std::size_t i) const { return in_[i]; }
  [[nodiscard]] const std::optional<State>& out(std::size_t i) const { return out_[i]; }
  [[nodiscard]] std::vector<std::optional<State>>& states() { return in_; }

 private:
  // A branch on the carry after a compare against an immediate bounds the
  // compared register on each of its two edges.
  [[nodiscard]] State refined(std::size_t i, const State& after, bool taken) const {
    State state = after;
    const std::optional<Compare> compare = in_[i]->compare;
    const BranchOn test = branchOn(graph_.program.nodes[i]);
    if (!compare || test.flag != Place::FlagC) return state;
    const bool carrySet = taken ? test.whenSet : !test.whenSet;
    const std::uint32_t bound = compare->value;
    auto keep = [carrySet, bound](std::uint32_t v) { return carrySet ? v >= bound : v < bound; };
    RegisterState& r = state.registers;
    Values* low = compare->place == Place::A ? &r.aLow : compare->place == Place::X ? &r.xLow : &r.yLow;
    Values* high = compare->place == Place::A ? &r.aHigh : compare->place == Place::X ? &r.xHigh : &r.yHigh;
    if (compare->bits == 8) {
      *low = low->known ? filter(*low, keep) : (carrySet ? Values::none() : below(bound));
    } else {
      Values whole = join16(*low, *high);
      whole = whole.known ? filter(whole, keep) : (carrySet ? Values::none() : below(bound));
      *low = lowByte(whole);
      *high = highByte(whole);
    }
    return state;
  }

  // What a call site's callees bring back to the instruction after it.
  void fireReturns(std::size_t site) {
    if (!graph_.next[site] || !out_[site]) return;
    const std::vector<std::size_t> roots = graph_.callees(site);
    if (roots.empty()) {
      join(*graph_.next[site], unknownState());
      return;
    }
    for (const std::size_t root : roots) {
      const auto found = summaries_.find(root);
      if (found == summaries_.end() || found->second.opaque) {
        join(*graph_.next[site], unknownState());
        continue;
      }
      const Summary& summary = found->second;
      if (held_) {
        // A probe does not run the callee: what the callee proves at its
        // return is not known here, only what it gives back untouched.
        join(*graph_.next[site], returned(summary, *in_[site], *out_[site], std::nullopt));
        continue;
      }
      for (const std::size_t r : summary.returns) {
        if (out_[r]) join(*graph_.next[site], returned(summary, *in_[site], *out_[site], out_[r]));
      }
    }
  }

  // A jump or call through a table the bytes place in the image: every pointer
  // the index selects, read as the chip reads it, is a destination.
  void deriveFrom(std::size_t i, const Evaluation& ev, std::set<std::tuple<Address, Address, Address>>& seen,
                  std::vector<DerivedTarget>& derived) {
    const Node& node = graph_.program.nodes[i];
    const Abstract probe(node, *in_[i], image_, stack_);
    for (const ProvenAccess& access : ev.accesses) {
      if (access.op != Op::Load || !access.value.known) continue;
      const std::optional<unsigned> width = probe.bits(access.width);
      if (!width) continue;
      for (const std::uint32_t pointer : access.address.values) {
        const Values one = probe.load(Values::one(pointer), *width, node.effects[access.effect].step);
        const std::optional<std::uint32_t> read = one.single();
        // The pointer of an indexed form is in the program bank and names an
        // address in it.
        const std::optional<std::uint32_t> bank = ev.pbr.single();
        if (!read || !bank) continue;
        const Address target = (*bank << 16) | (*read & 0xFFFFu);
        if (seen.insert({node.instruction.address, target, pointer}).second) {
          derived.push_back(DerivedTarget{.site = node.instruction.address,
                                          .pointer = pointer,
                                          .target = target,
                                          .call = node.instruction.flow == Flow::Call});
        }
        // A destination that is already code takes the site's state now; one
        // that is not is traced on the next run, and takes it then.
        if (node.instruction.flow == Flow::Jump) {
          if (const std::optional<std::size_t> to = graph_.nodeFor(i, target)) join(*to, ev.after);
        }
      }
    }
  }

  const Graph& graph_;
  const ImageReader& image_;
  const StackReach& stack_;
  const std::map<std::size_t, Summary>& summaries_;
  std::vector<std::optional<State>> in_;
  std::vector<std::optional<State>> out_;
  std::vector<unsigned> visits_;
  std::vector<bool> queued_;
  std::deque<std::size_t> pending_;
  const std::set<std::size_t>* held_ = nullptr;
};

// The routine run from a named entry — every register the name of what it held
// on entry, the stack empty — and its returns read: a register whose name comes
// back whole gave it back; a stack pointer whose name comes back moved by
// exactly the return's own pull is balanced.
void probe(const Graph& graph, const ImageReader& image, const StackReach& stack,
           const std::map<std::size_t, Summary>& summaries, std::size_t root, Summary& summary) {
  summary.transparent = 0;
  summary.balanced = false;
  if (summary.opaque || summary.returns.empty()) return;

  State entry;
  entry.registers.aLow = Values::entry(Place::A, 0);
  entry.registers.aHigh = Values::entry(Place::A, 1);
  entry.registers.xLow = Values::entry(Place::X, 0);
  entry.registers.xHigh = Values::entry(Place::X, 1);
  entry.registers.yLow = Values::entry(Place::Y, 0);
  entry.registers.yHigh = Values::entry(Place::Y, 1);
  entry.registers.d = Values::entry(Place::D, 2);
  entry.registers.s = Values::entry(Place::S, 2);
  entry.registers.dbr = Values::entry(Place::DBR, 0);

  Propagation run(graph, image, stack, summaries);
  run.restrict(summary.held);
  run.join(root, entry);
  run.run(nullptr, nullptr);

  unsigned transparent = TA | TX | TY | TD | TDBR;
  bool balanced = true;
  for (const std::size_t r : summary.returns) {
    const std::optional<State>& at = run.out(r);
    if (!at) {
      transparent = 0;
      balanced = false;
      break;
    }
    const RegisterState& s = at->registers;
    if (!(s.aLow == Values::entry(Place::A, 0) && s.aHigh == Values::entry(Place::A, 1))) transparent &= ~TA;
    if (!(s.xLow == Values::entry(Place::X, 0) && s.xHigh == Values::entry(Place::X, 1))) transparent &= ~TX;
    if (!(s.yLow == Values::entry(Place::Y, 0) && s.yHigh == Values::entry(Place::Y, 1))) transparent &= ~TY;
    if (s.d != Values::entry(Place::D, 2)) transparent &= ~TD;
    if (s.dbr != Values::entry(Place::DBR, 0)) transparent &= ~TDBR;
    const std::string_view mnemonic = graph.program.nodes[r].instruction.mnemonic;
    const int pulled = mnemonic == "RTS" ? 2 : mnemonic == "RTL" ? 3 : -1;
    Values expected = Values::entry(Place::S, 2);
    expected.symbol->offset = pulled;
    if (pulled < 0 || s.s != expected) balanced = false;
  }
  summary.transparent = transparent;
  summary.balanced = balanced;
}

}  // namespace

Dataflow::Dataflow(const Program& program, const std::vector<FlowEntry>& entries,
                   const std::vector<Sighting>& sightings, ImageReader image, Canonical canonical,
                   StackReach stack)
    : program_(program), before_(program.nodes.size()) {
  const std::size_t count = program.nodes.size();
  Graph graph{program, canonical, {}, {}, {}};
  graph.sightingsOf.resize(count);
  graph.next.resize(count);
  graph.target.resize(count);

  // The edges the instructions name.
  for (std::size_t i = 0; i < count; ++i) {
    const Instruction& instruction = program.nodes[i].instruction;
    const Address bank = instruction.address & 0xFF0000u;
    const Address after = bank | ((instruction.address + instruction.length) & 0xFFFFu);
    graph.next[i] = graph.nodeFor(i, after);
    if (instruction.target) graph.target[i] = graph.nodeFor(i, *instruction.target);
  }
  // The edges a run or an earlier analysis proved.
  for (const Sighting& sighting : sightings) {
    const std::optional<Address> site = canonical(sighting.site);
    if (!site) continue;
    const auto [first, last] = graph.at(*site);
    for (std::size_t i = first; i < last; ++i) {
      const std::optional<std::size_t> to = graph.nodeFor(i, sighting.target);
      if (to) graph.sightingsOf[i].push_back(*to);
    }
  }

  // Every routine a call names, walked; a callee's callees are roots too.
  std::map<std::size_t, Summary> summaries;
  std::vector<std::size_t> roots;
  for (std::size_t i = 0; i < count; ++i) {
    for (const std::size_t root : graph.callees(i)) roots.push_back(root);
  }
  while (!roots.empty()) {
    const std::size_t root = roots.back();
    roots.pop_back();
    if (summaries.find(root) != summaries.end()) continue;
    Summary summary = walk(graph, root);
    roots.insert(roots.end(), summary.callees.begin(), summary.callees.end());
    summaries[root] = std::move(summary);
  }
  // What lies beyond the analysis in a callee lies beyond it in every caller.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& [root, summary] : summaries) {
      if (summary.opaque) continue;
      for (const std::size_t callee : summary.callees) {
        if (summaries.at(callee).opaque) {
          summary.opaque = true;
          changed = true;
          break;
        }
      }
    }
  }
  // Each routine probed from a named entry, with its callees' answers folded
  // in, until no routine gives back more than it did the round before.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& [root, summary] : summaries) {
      const unsigned transparent = summary.transparent;
      const bool balanced = summary.balanced;
      probe(graph, image, stack, summaries, root, summary);
      if (summary.transparent != transparent || summary.balanced != balanced) changed = true;
    }
  }
  // Which call sites a return node returns to: every site whose callee holds it.
  std::vector<std::vector<std::size_t>> callersOf(count);
  for (std::size_t i = 0; i < count; ++i) {
    for (const std::size_t root : graph.callees(i)) {
      for (const std::size_t r : summaries.at(root).returns) callersOf[r].push_back(i);
    }
  }

  Propagation main(graph, image, stack, summaries);
  for (const FlowEntry& entry : entries) {
    const std::optional<Address> home = canonical(entry.address);
    if (!home) continue;
    const auto [first, last] = graph.at(*home);
    for (std::size_t i = first; i < last; ++i) main.join(i, entry.state);
  }
  main.run(&callersOf, &derived_);
  before_ = std::move(main.states());

  std::sort(derived_.begin(), derived_.end(), [](const DerivedTarget& a, const DerivedTarget& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return a.pointer < b.pointer;
  });
}

const State* Dataflow::before(std::size_t node) const {
  if (node >= before_.size() || !before_[node]) return nullptr;
  return &*before_[node];
}

const State* Dataflow::before(Address address) const {
  const auto& nodes = program_.nodes;
  auto it = std::lower_bound(nodes.begin(), nodes.end(), address,
                             [](const Node& n, Address a) { return n.instruction.address < a; });
  for (; it != nodes.end() && it->instruction.address == address; ++it) {
    const std::size_t i = static_cast<std::size_t>(it - nodes.begin());
    if (before_[i]) return &*before_[i];
  }
  return nullptr;
}

std::size_t Dataflow::reachedNodes() const {
  std::size_t reached = 0;
  for (const std::optional<State>& state : before_) {
    if (state) ++reached;
  }
  return reached;
}

}  // namespace snaggletooth::ir
