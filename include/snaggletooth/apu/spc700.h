#pragma once

// The SPC700 — the SNES audio CPU's instruction-set core.
//
// The core runs over an abstract bus (the ApuBus concept): the machine glue and
// the flat-RAM test harness both satisfy it, so the same interpreter is exercised
// on plain RAM and, later, on the register-overlaid APU. Reads may have side
// effects on real hardware, so the core issues every documented memory access.

#include <concepts>
#include <cstdint>

namespace snaggletooth {

// The SPC700's view of its 64KB address space. A conforming bus answers an 8-bit
// read for any 16-bit address and accepts an 8-bit write to one.
template <typename B>
concept ApuBus = requires(B bus, std::uint16_t address, std::uint8_t value) {
  { bus.read(address) } -> std::same_as<std::uint8_t>;
  bus.write(address, value);
};

enum class RunState : std::uint8_t { Running, Sleeping, Stopped };

// PSW flag masks: the program status word is a single packed byte.
inline constexpr std::uint8_t kFlagN = 0x80;  // negative (high bit of the result)
inline constexpr std::uint8_t kFlagV = 0x40;  // signed overflow
inline constexpr std::uint8_t kFlagP = 0x20;  // direct page — moves the page to $0100
inline constexpr std::uint8_t kFlagB = 0x10;  // break
inline constexpr std::uint8_t kFlagH = 0x08;  // half-carry (nibble boundary)
inline constexpr std::uint8_t kFlagI = 0x04;  // interrupt enable (no interrupts on the S-SMP)
inline constexpr std::uint8_t kFlagZ = 0x02;  // zero
inline constexpr std::uint8_t kFlagC = 0x01;  // carry

// The whole CPU state as a value: snapshot by copy, restore by assignment.
struct Spc700State {
  std::uint16_t pc = 0;
  std::uint8_t a = 0;
  std::uint8_t x = 0;
  std::uint8_t y = 0;
  std::uint8_t sp = 0;
  std::uint8_t psw = 0;
  RunState run = RunState::Running;
};

class Spc700 {
 public:
  Spc700() = default;
  explicit Spc700(Spc700State state) : state_(state) {}

  [[nodiscard]] const Spc700State& state() const noexcept { return state_; }
  void restore(Spc700State state) noexcept { state_ = state; }

  // Executes one instruction and returns its documented cycle count. On a
  // non-Running core this returns 2 cycles and touches neither state nor the bus.
  template <ApuBus B>
  std::uint32_t step(B& bus);

 private:
  // The direct-page base: the P flag selects $0100 over $0000.
  [[nodiscard]] std::uint16_t dpBase() const noexcept {
    return (state_.psw & kFlagP) ? 0x0100u : 0x0000u;
  }

  // Sets N and Z from a result, leaving the other flags untouched.
  void setNZ(std::uint8_t v) noexcept {
    state_.psw = static_cast<std::uint8_t>((state_.psw & ~(kFlagN | kFlagZ)) |
                                           (v & kFlagN) | (v == 0 ? kFlagZ : 0));
  }

  // Sets the carry flag from a boolean, leaving the others untouched.
  void setCarry(bool carry) noexcept {
    state_.psw = static_cast<std::uint8_t>(carry ? (state_.psw | kFlagC)
                                                 : (state_.psw & ~kFlagC));
  }

  // Add-with-carry core: adds rhs and the current carry to lhs, sets N/V/H/Z/C
  // from the binary result, and returns it. V is signed overflow (both inputs
  // agree in sign but the result disagrees); H is the carry out of bit 3.
  std::uint8_t adcOp(std::uint8_t lhs, std::uint8_t rhs) noexcept {
    const unsigned carryIn = (state_.psw & kFlagC) ? 1u : 0u;
    const unsigned sum = unsigned{lhs} + unsigned{rhs} + carryIn;
    const std::uint8_t result = static_cast<std::uint8_t>(sum);
    const bool halfCarry = ((lhs & 0x0Fu) + (rhs & 0x0Fu) + carryIn) > 0x0Fu;
    const bool overflow = (~(lhs ^ rhs) & (lhs ^ result) & 0x80u) != 0;
    std::uint8_t psw =
        static_cast<std::uint8_t>(state_.psw &
                                  ~(kFlagN | kFlagV | kFlagH | kFlagZ | kFlagC));
    if (result & 0x80u) psw |= kFlagN;
    if (overflow) psw |= kFlagV;
    if (halfCarry) psw |= kFlagH;
    if (result == 0) psw |= kFlagZ;
    if (sum > 0xFFu) psw |= kFlagC;
    state_.psw = psw;
    return result;
  }

  // Subtract-with-borrow reuses the adder against the ones-complement operand:
  // A - M - !C == A + ~M + C, so carry, half-carry and overflow all fall out of
  // the same binary addition (carry set means no borrow).
  std::uint8_t sbcOp(std::uint8_t lhs, std::uint8_t rhs) noexcept {
    return adcOp(lhs, static_cast<std::uint8_t>(rhs ^ 0xFFu));
  }

  // Compare: sets N and Z from lhs - rhs and C when no borrow occurs
  // (lhs >= rhs). The difference is discarded; no operand is written and H and V
  // are untouched.
  void cmpOp(std::uint8_t lhs, std::uint8_t rhs) noexcept {
    const std::uint8_t result = static_cast<std::uint8_t>(lhs - rhs);
    std::uint8_t psw =
        static_cast<std::uint8_t>(state_.psw & ~(kFlagN | kFlagZ | kFlagC));
    if (result & 0x80u) psw |= kFlagN;
    if (result == 0) psw |= kFlagZ;
    if (lhs >= rhs) psw |= kFlagC;
    state_.psw = psw;
  }

  // Shift/rotate cores: each moves the bit leaving the byte into carry, then
  // sets N and Z from the result. LSR shifts a zero in, so it always clears N;
  // ROL/ROR feed the old carry into the vacated bit.
  std::uint8_t aslOp(std::uint8_t v) noexcept {
    setCarry((v & 0x80u) != 0);
    const std::uint8_t result = static_cast<std::uint8_t>(v << 1);
    setNZ(result);
    return result;
  }
  std::uint8_t lsrOp(std::uint8_t v) noexcept {
    setCarry((v & 0x01u) != 0);
    const std::uint8_t result = static_cast<std::uint8_t>(v >> 1);
    setNZ(result);
    return result;
  }
  std::uint8_t rolOp(std::uint8_t v) noexcept {
    const std::uint8_t carryIn = (state_.psw & kFlagC) ? 1u : 0u;
    setCarry((v & 0x80u) != 0);
    const std::uint8_t result = static_cast<std::uint8_t>((v << 1) | carryIn);
    setNZ(result);
    return result;
  }
  std::uint8_t rorOp(std::uint8_t v) noexcept {
    const std::uint8_t carryIn = (state_.psw & kFlagC) ? 0x80u : 0u;
    setCarry((v & 0x01u) != 0);
    const std::uint8_t result = static_cast<std::uint8_t>((v >> 1) | carryIn);
    setNZ(result);
    return result;
  }

  // Reads the byte at PC and advances it (a program fetch).
  template <ApuBus B>
  std::uint8_t fetch(B& bus) {
    return bus.read(state_.pc++);
  }

  // Effective-address helpers. Each consumes its operand bytes from the program
  // stream and issues any pointer reads, returning the target address.
  template <ApuBus B>
  std::uint16_t addrDp(B& bus) {
    return static_cast<std::uint16_t>(dpBase() + fetch(bus));
  }
  template <ApuBus B>
  std::uint16_t addrDpX(B& bus) {
    return static_cast<std::uint16_t>(dpBase() + ((fetch(bus) + state_.x) & 0xFF));
  }
  template <ApuBus B>
  std::uint16_t addrDpY(B& bus) {
    return static_cast<std::uint16_t>(dpBase() + ((fetch(bus) + state_.y) & 0xFF));
  }
  template <ApuBus B>
  std::uint16_t addrAbs(B& bus) {
    std::uint16_t lo = fetch(bus);
    std::uint16_t hi = fetch(bus);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  template <ApuBus B>
  std::uint16_t addrAbsX(B& bus) {
    return static_cast<std::uint16_t>(addrAbs(bus) + state_.x);
  }
  template <ApuBus B>
  std::uint16_t addrAbsY(B& bus) {
    return static_cast<std::uint16_t>(addrAbs(bus) + state_.y);
  }
  // [dp+X]: X is added to the direct-page offset before the pointer lookup; the
  // two pointer bytes wrap within the direct page.
  template <ApuBus B>
  std::uint16_t addrIndX(B& bus) {
    std::uint8_t d = static_cast<std::uint8_t>(fetch(bus) + state_.x);
    std::uint16_t lo = bus.read(static_cast<std::uint16_t>(dpBase() + d));
    std::uint16_t hi =
        bus.read(static_cast<std::uint16_t>(dpBase() + ((d + 1) & 0xFF)));
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  // [dp]+Y: the pointer is looked up from the direct page (wrapping within it),
  // then Y is added to the resolved 16-bit address.
  template <ApuBus B>
  std::uint16_t addrIndY(B& bus) {
    std::uint8_t d = fetch(bus);
    std::uint16_t lo = bus.read(static_cast<std::uint16_t>(dpBase() + d));
    std::uint16_t hi =
        bus.read(static_cast<std::uint16_t>(dpBase() + ((d + 1) & 0xFF)));
    return static_cast<std::uint16_t>((lo | (hi << 8)) + state_.y);
  }

  Spc700State state_{};
};

template <ApuBus B>
std::uint32_t Spc700::step(B& bus) {
  if (state_.run != RunState::Running) {
    return 2;  // halted: no state change, no bus access
  }
  const std::uint8_t opcode = fetch(bus);
  switch (opcode) {
    // ---- 8-bit move: memory to register (N,Z from the loaded value) ----
    case 0xE8: state_.a = fetch(bus); setNZ(state_.a); return 2;                                  // MOV A,#imm
    case 0xE6: state_.a = bus.read(static_cast<std::uint16_t>(dpBase() + state_.x)); setNZ(state_.a); return 3;  // MOV A,(X)
    case 0xBF: {                                                                                  // MOV A,(X)+
      state_.a = bus.read(static_cast<std::uint16_t>(dpBase() + state_.x));
      ++state_.x;
      setNZ(state_.a);
      return 4;
    }
    case 0xE4: state_.a = bus.read(addrDp(bus));   setNZ(state_.a); return 3;                     // MOV A,dp
    case 0xF4: state_.a = bus.read(addrDpX(bus));  setNZ(state_.a); return 4;                     // MOV A,dp+X
    case 0xE5: state_.a = bus.read(addrAbs(bus));  setNZ(state_.a); return 4;                     // MOV A,!abs
    case 0xF5: state_.a = bus.read(addrAbsX(bus)); setNZ(state_.a); return 5;                     // MOV A,!abs+X
    case 0xF6: state_.a = bus.read(addrAbsY(bus)); setNZ(state_.a); return 5;                     // MOV A,!abs+Y
    case 0xE7: state_.a = bus.read(addrIndX(bus)); setNZ(state_.a); return 6;                     // MOV A,[dp+X]
    case 0xF7: state_.a = bus.read(addrIndY(bus)); setNZ(state_.a); return 6;                     // MOV A,[dp]+Y
    case 0xCD: state_.x = fetch(bus); setNZ(state_.x); return 2;                                  // MOV X,#imm
    case 0xF8: state_.x = bus.read(addrDp(bus));   setNZ(state_.x); return 3;                     // MOV X,dp
    case 0xF9: state_.x = bus.read(addrDpY(bus));  setNZ(state_.x); return 4;                     // MOV X,dp+Y
    case 0xE9: state_.x = bus.read(addrAbs(bus));  setNZ(state_.x); return 4;                     // MOV X,!abs
    case 0x8D: state_.y = fetch(bus); setNZ(state_.y); return 2;                                  // MOV Y,#imm
    case 0xEB: state_.y = bus.read(addrDp(bus));   setNZ(state_.y); return 3;                     // MOV Y,dp
    case 0xFB: state_.y = bus.read(addrDpX(bus));  setNZ(state_.y); return 4;                     // MOV Y,dp+X
    case 0xEC: state_.y = bus.read(addrAbs(bus));  setNZ(state_.y); return 4;                     // MOV Y,!abs

    // ---- 8-bit move: register to memory (no flags) ----
    case 0xC6: bus.write(static_cast<std::uint16_t>(dpBase() + state_.x), state_.a); return 4;    // MOV (X),A
    case 0xAF: {                                                                                  // MOV (X)+,A
      bus.write(static_cast<std::uint16_t>(dpBase() + state_.x), state_.a);
      ++state_.x;
      return 4;
    }
    case 0xC4: bus.write(addrDp(bus),   state_.a); return 4;                                      // MOV dp,A
    case 0xD4: bus.write(addrDpX(bus),  state_.a); return 5;                                      // MOV dp+X,A
    case 0xC5: bus.write(addrAbs(bus),  state_.a); return 5;                                      // MOV !abs,A
    case 0xD5: bus.write(addrAbsX(bus), state_.a); return 6;                                      // MOV !abs+X,A
    case 0xD6: bus.write(addrAbsY(bus), state_.a); return 6;                                      // MOV !abs+Y,A
    case 0xC7: bus.write(addrIndX(bus), state_.a); return 7;                                      // MOV [dp+X],A
    case 0xD7: bus.write(addrIndY(bus), state_.a); return 7;                                      // MOV [dp]+Y,A
    case 0xD8: bus.write(addrDp(bus),   state_.x); return 4;                                      // MOV dp,X
    case 0xD9: bus.write(addrDpY(bus),  state_.x); return 5;                                      // MOV dp+Y,X
    case 0xC9: bus.write(addrAbs(bus),  state_.x); return 5;                                      // MOV !abs,X
    case 0xCB: bus.write(addrDp(bus),   state_.y); return 4;                                      // MOV dp,Y
    case 0xDB: bus.write(addrDpX(bus),  state_.y); return 5;                                      // MOV dp+X,Y
    case 0xCC: bus.write(addrAbs(bus),  state_.y); return 5;                                      // MOV !abs,Y

    // ---- 8-bit move: register/register and special direct-page moves ----
    case 0x7D: state_.a = state_.x;  setNZ(state_.a); return 2;                                   // MOV A,X
    case 0xDD: state_.a = state_.y;  setNZ(state_.a); return 2;                                   // MOV A,Y
    case 0x5D: state_.x = state_.a;  setNZ(state_.x); return 2;                                   // MOV X,A
    case 0xFD: state_.y = state_.a;  setNZ(state_.y); return 2;                                   // MOV Y,A
    case 0x9D: state_.x = state_.sp; setNZ(state_.x); return 2;                                   // MOV X,SP
    case 0xBD: state_.sp = state_.x; return 2;                                                    // MOV SP,X (no flags)
    case 0xFA: {                                                                                  // MOV dp,dp (encoded source-first: FA src dst)
      std::uint8_t src = fetch(bus);
      std::uint8_t val = bus.read(static_cast<std::uint16_t>(dpBase() + src));
      std::uint8_t dst = fetch(bus);
      bus.write(static_cast<std::uint16_t>(dpBase() + dst), val);
      return 5;
    }
    case 0x8F: {                                                                                  // MOV dp,#imm (encoded immediate-first: 8F imm dst)
      std::uint8_t imm = fetch(bus);
      std::uint8_t dst = fetch(bus);
      bus.write(static_cast<std::uint16_t>(dpBase() + dst), imm);
      return 5;
    }

    // ---- 8-bit arithmetic: ADC (A = A + operand + C; N,V,H,Z,C) ----
    case 0x88: state_.a = adcOp(state_.a, fetch(bus)); return 2;                                  // ADC A,#imm
    case 0x86: state_.a = adcOp(state_.a, bus.read(static_cast<std::uint16_t>(dpBase() + state_.x))); return 3;  // ADC A,(X)
    case 0x84: state_.a = adcOp(state_.a, bus.read(addrDp(bus)));   return 3;                     // ADC A,dp
    case 0x94: state_.a = adcOp(state_.a, bus.read(addrDpX(bus)));  return 4;                     // ADC A,dp+X
    case 0x85: state_.a = adcOp(state_.a, bus.read(addrAbs(bus)));  return 4;                     // ADC A,!abs
    case 0x95: state_.a = adcOp(state_.a, bus.read(addrAbsX(bus))); return 5;                     // ADC A,!abs+X
    case 0x96: state_.a = adcOp(state_.a, bus.read(addrAbsY(bus))); return 5;                     // ADC A,!abs+Y
    case 0x87: state_.a = adcOp(state_.a, bus.read(addrIndX(bus))); return 6;                     // ADC A,[dp+X]
    case 0x97: state_.a = adcOp(state_.a, bus.read(addrIndY(bus))); return 6;                     // ADC A,[dp]+Y
    case 0x99: {                                                                                  // ADC (X),(Y)
      std::uint16_t dst = static_cast<std::uint16_t>(dpBase() + state_.x);
      std::uint8_t rhs = bus.read(static_cast<std::uint16_t>(dpBase() + state_.y));
      bus.write(dst, adcOp(bus.read(dst), rhs));
      return 5;
    }
    case 0x89: {                                                                                  // ADC dp,dp (encoded source-first)
      std::uint8_t src = bus.read(static_cast<std::uint16_t>(dpBase() + fetch(bus)));
      std::uint16_t dst = addrDp(bus);
      bus.write(dst, adcOp(bus.read(dst), src));
      return 6;
    }
    case 0x98: {                                                                                  // ADC dp,#imm (encoded immediate-first)
      std::uint8_t imm = fetch(bus);
      std::uint16_t dst = addrDp(bus);
      bus.write(dst, adcOp(bus.read(dst), imm));
      return 5;
    }

    // ---- 8-bit arithmetic: SBC (A = A - operand - !C; N,V,H,Z,C) ----
    case 0xA8: state_.a = sbcOp(state_.a, fetch(bus)); return 2;                                  // SBC A,#imm
    case 0xA6: state_.a = sbcOp(state_.a, bus.read(static_cast<std::uint16_t>(dpBase() + state_.x))); return 3;  // SBC A,(X)
    case 0xA4: state_.a = sbcOp(state_.a, bus.read(addrDp(bus)));   return 3;                     // SBC A,dp
    case 0xB4: state_.a = sbcOp(state_.a, bus.read(addrDpX(bus)));  return 4;                     // SBC A,dp+X
    case 0xA5: state_.a = sbcOp(state_.a, bus.read(addrAbs(bus)));  return 4;                     // SBC A,!abs
    case 0xB5: state_.a = sbcOp(state_.a, bus.read(addrAbsX(bus))); return 5;                     // SBC A,!abs+X
    case 0xB6: state_.a = sbcOp(state_.a, bus.read(addrAbsY(bus))); return 5;                     // SBC A,!abs+Y
    case 0xA7: state_.a = sbcOp(state_.a, bus.read(addrIndX(bus))); return 6;                     // SBC A,[dp+X]
    case 0xB7: state_.a = sbcOp(state_.a, bus.read(addrIndY(bus))); return 6;                     // SBC A,[dp]+Y
    case 0xB9: {                                                                                  // SBC (X),(Y)
      std::uint16_t dst = static_cast<std::uint16_t>(dpBase() + state_.x);
      std::uint8_t rhs = bus.read(static_cast<std::uint16_t>(dpBase() + state_.y));
      bus.write(dst, sbcOp(bus.read(dst), rhs));
      return 5;
    }
    case 0xA9: {                                                                                  // SBC dp,dp (encoded source-first)
      std::uint8_t src = bus.read(static_cast<std::uint16_t>(dpBase() + fetch(bus)));
      std::uint16_t dst = addrDp(bus);
      bus.write(dst, sbcOp(bus.read(dst), src));
      return 6;
    }
    case 0xB8: {                                                                                  // SBC dp,#imm (encoded immediate-first)
      std::uint8_t imm = fetch(bus);
      std::uint16_t dst = addrDp(bus);
      bus.write(dst, sbcOp(bus.read(dst), imm));
      return 5;
    }

    // ---- 8-bit arithmetic: CMP (operand1 - operand2, result discarded; N,Z,C) ----
    case 0x68: cmpOp(state_.a, fetch(bus)); return 2;                                             // CMP A,#imm
    case 0x66: cmpOp(state_.a, bus.read(static_cast<std::uint16_t>(dpBase() + state_.x))); return 3;  // CMP A,(X)
    case 0x64: cmpOp(state_.a, bus.read(addrDp(bus)));   return 3;                                // CMP A,dp
    case 0x74: cmpOp(state_.a, bus.read(addrDpX(bus)));  return 4;                                // CMP A,dp+X
    case 0x65: cmpOp(state_.a, bus.read(addrAbs(bus)));  return 4;                                // CMP A,!abs
    case 0x75: cmpOp(state_.a, bus.read(addrAbsX(bus))); return 5;                                // CMP A,!abs+X
    case 0x76: cmpOp(state_.a, bus.read(addrAbsY(bus))); return 5;                                // CMP A,!abs+Y
    case 0x67: cmpOp(state_.a, bus.read(addrIndX(bus))); return 6;                                // CMP A,[dp+X]
    case 0x77: cmpOp(state_.a, bus.read(addrIndY(bus))); return 6;                                // CMP A,[dp]+Y
    case 0x79: {                                                                                  // CMP (X),(Y)
      std::uint8_t rhs = bus.read(static_cast<std::uint16_t>(dpBase() + state_.y));
      cmpOp(bus.read(static_cast<std::uint16_t>(dpBase() + state_.x)), rhs);
      return 5;
    }
    case 0x69: {                                                                                  // CMP dp,dp (encoded source-first)
      std::uint8_t src = bus.read(static_cast<std::uint16_t>(dpBase() + fetch(bus)));
      cmpOp(bus.read(addrDp(bus)), src);
      return 6;
    }
    case 0x78: {                                                                                  // CMP dp,#imm (encoded immediate-first)
      std::uint8_t imm = fetch(bus);
      cmpOp(bus.read(addrDp(bus)), imm);
      return 5;
    }
    case 0xC8: cmpOp(state_.x, fetch(bus)); return 2;                                             // CMP X,#imm
    case 0x3E: cmpOp(state_.x, bus.read(addrDp(bus)));  return 3;                                 // CMP X,dp
    case 0x1E: cmpOp(state_.x, bus.read(addrAbs(bus))); return 4;                                 // CMP X,!abs
    case 0xAD: cmpOp(state_.y, fetch(bus)); return 2;                                             // CMP Y,#imm
    case 0x7E: cmpOp(state_.y, bus.read(addrDp(bus)));  return 3;                                 // CMP Y,dp
    case 0x5E: cmpOp(state_.y, bus.read(addrAbs(bus))); return 4;                                 // CMP Y,!abs

    // ---- 8-bit boolean logic: AND (A &= operand; N,Z) ----
    case 0x28: state_.a &= fetch(bus); setNZ(state_.a); return 2;                                 // AND A,#imm
    case 0x26: state_.a &= bus.read(static_cast<std::uint16_t>(dpBase() + state_.x)); setNZ(state_.a); return 3;  // AND A,(X)
    case 0x24: state_.a &= bus.read(addrDp(bus));   setNZ(state_.a); return 3;                    // AND A,dp
    case 0x34: state_.a &= bus.read(addrDpX(bus));  setNZ(state_.a); return 4;                    // AND A,dp+X
    case 0x25: state_.a &= bus.read(addrAbs(bus));  setNZ(state_.a); return 4;                    // AND A,!abs
    case 0x35: state_.a &= bus.read(addrAbsX(bus)); setNZ(state_.a); return 5;                    // AND A,!abs+X
    case 0x36: state_.a &= bus.read(addrAbsY(bus)); setNZ(state_.a); return 5;                    // AND A,!abs+Y
    case 0x27: state_.a &= bus.read(addrIndX(bus)); setNZ(state_.a); return 6;                    // AND A,[dp+X]
    case 0x37: state_.a &= bus.read(addrIndY(bus)); setNZ(state_.a); return 6;                    // AND A,[dp]+Y
    case 0x39: {                                                                                  // AND (X),(Y)
      std::uint16_t dst = static_cast<std::uint16_t>(dpBase() + state_.x);
      std::uint8_t rhs = bus.read(static_cast<std::uint16_t>(dpBase() + state_.y));
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) & rhs);
      setNZ(r); bus.write(dst, r); return 5;
    }
    case 0x29: {                                                                                  // AND dp,dp (encoded source-first)
      std::uint8_t src = bus.read(static_cast<std::uint16_t>(dpBase() + fetch(bus)));
      std::uint16_t dst = addrDp(bus);
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) & src);
      setNZ(r); bus.write(dst, r); return 6;
    }
    case 0x38: {                                                                                  // AND dp,#imm (encoded immediate-first)
      std::uint8_t imm = fetch(bus);
      std::uint16_t dst = addrDp(bus);
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) & imm);
      setNZ(r); bus.write(dst, r); return 5;
    }

    // ---- 8-bit boolean logic: OR (A |= operand; N,Z) ----
    case 0x08: state_.a |= fetch(bus); setNZ(state_.a); return 2;                                 // OR A,#imm
    case 0x06: state_.a |= bus.read(static_cast<std::uint16_t>(dpBase() + state_.x)); setNZ(state_.a); return 3;  // OR A,(X)
    case 0x04: state_.a |= bus.read(addrDp(bus));   setNZ(state_.a); return 3;                    // OR A,dp
    case 0x14: state_.a |= bus.read(addrDpX(bus));  setNZ(state_.a); return 4;                    // OR A,dp+X
    case 0x05: state_.a |= bus.read(addrAbs(bus));  setNZ(state_.a); return 4;                    // OR A,!abs
    case 0x15: state_.a |= bus.read(addrAbsX(bus)); setNZ(state_.a); return 5;                    // OR A,!abs+X
    case 0x16: state_.a |= bus.read(addrAbsY(bus)); setNZ(state_.a); return 5;                    // OR A,!abs+Y
    case 0x07: state_.a |= bus.read(addrIndX(bus)); setNZ(state_.a); return 6;                    // OR A,[dp+X]
    case 0x17: state_.a |= bus.read(addrIndY(bus)); setNZ(state_.a); return 6;                    // OR A,[dp]+Y
    case 0x19: {                                                                                  // OR (X),(Y)
      std::uint16_t dst = static_cast<std::uint16_t>(dpBase() + state_.x);
      std::uint8_t rhs = bus.read(static_cast<std::uint16_t>(dpBase() + state_.y));
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) | rhs);
      setNZ(r); bus.write(dst, r); return 5;
    }
    case 0x09: {                                                                                  // OR dp,dp (encoded source-first)
      std::uint8_t src = bus.read(static_cast<std::uint16_t>(dpBase() + fetch(bus)));
      std::uint16_t dst = addrDp(bus);
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) | src);
      setNZ(r); bus.write(dst, r); return 6;
    }
    case 0x18: {                                                                                  // OR dp,#imm (encoded immediate-first)
      std::uint8_t imm = fetch(bus);
      std::uint16_t dst = addrDp(bus);
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) | imm);
      setNZ(r); bus.write(dst, r); return 5;
    }

    // ---- 8-bit boolean logic: EOR (A ^= operand; N,Z) ----
    case 0x48: state_.a ^= fetch(bus); setNZ(state_.a); return 2;                                 // EOR A,#imm
    case 0x46: state_.a ^= bus.read(static_cast<std::uint16_t>(dpBase() + state_.x)); setNZ(state_.a); return 3;  // EOR A,(X)
    case 0x44: state_.a ^= bus.read(addrDp(bus));   setNZ(state_.a); return 3;                    // EOR A,dp
    case 0x54: state_.a ^= bus.read(addrDpX(bus));  setNZ(state_.a); return 4;                    // EOR A,dp+X
    case 0x45: state_.a ^= bus.read(addrAbs(bus));  setNZ(state_.a); return 4;                    // EOR A,!abs
    case 0x55: state_.a ^= bus.read(addrAbsX(bus)); setNZ(state_.a); return 5;                    // EOR A,!abs+X
    case 0x56: state_.a ^= bus.read(addrAbsY(bus)); setNZ(state_.a); return 5;                    // EOR A,!abs+Y
    case 0x47: state_.a ^= bus.read(addrIndX(bus)); setNZ(state_.a); return 6;                    // EOR A,[dp+X]
    case 0x57: state_.a ^= bus.read(addrIndY(bus)); setNZ(state_.a); return 6;                    // EOR A,[dp]+Y
    case 0x59: {                                                                                  // EOR (X),(Y)
      std::uint16_t dst = static_cast<std::uint16_t>(dpBase() + state_.x);
      std::uint8_t rhs = bus.read(static_cast<std::uint16_t>(dpBase() + state_.y));
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) ^ rhs);
      setNZ(r); bus.write(dst, r); return 5;
    }
    case 0x49: {                                                                                  // EOR dp,dp (encoded source-first)
      std::uint8_t src = bus.read(static_cast<std::uint16_t>(dpBase() + fetch(bus)));
      std::uint16_t dst = addrDp(bus);
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) ^ src);
      setNZ(r); bus.write(dst, r); return 6;
    }
    case 0x58: {                                                                                  // EOR dp,#imm (encoded immediate-first)
      std::uint8_t imm = fetch(bus);
      std::uint16_t dst = addrDp(bus);
      std::uint8_t r = static_cast<std::uint8_t>(bus.read(dst) ^ imm);
      setNZ(r); bus.write(dst, r); return 5;
    }

    // ---- 8-bit increment / decrement (N,Z; no carry) ----
    case 0xBC: ++state_.a; setNZ(state_.a); return 2;                                             // INC A
    case 0x3D: ++state_.x; setNZ(state_.x); return 2;                                             // INC X
    case 0xFC: ++state_.y; setNZ(state_.y); return 2;                                             // INC Y
    case 0xAB: { std::uint16_t a = addrDp(bus);  std::uint8_t v = static_cast<std::uint8_t>(bus.read(a) + 1); setNZ(v); bus.write(a, v); return 4; }  // INC dp
    case 0xBB: { std::uint16_t a = addrDpX(bus); std::uint8_t v = static_cast<std::uint8_t>(bus.read(a) + 1); setNZ(v); bus.write(a, v); return 5; }  // INC dp+X
    case 0xAC: { std::uint16_t a = addrAbs(bus); std::uint8_t v = static_cast<std::uint8_t>(bus.read(a) + 1); setNZ(v); bus.write(a, v); return 5; }  // INC !abs
    case 0x9C: --state_.a; setNZ(state_.a); return 2;                                             // DEC A
    case 0x1D: --state_.x; setNZ(state_.x); return 2;                                             // DEC X
    case 0xDC: --state_.y; setNZ(state_.y); return 2;                                             // DEC Y
    case 0x8B: { std::uint16_t a = addrDp(bus);  std::uint8_t v = static_cast<std::uint8_t>(bus.read(a) - 1); setNZ(v); bus.write(a, v); return 4; }  // DEC dp
    case 0x9B: { std::uint16_t a = addrDpX(bus); std::uint8_t v = static_cast<std::uint8_t>(bus.read(a) - 1); setNZ(v); bus.write(a, v); return 5; }  // DEC dp+X
    case 0x8C: { std::uint16_t a = addrAbs(bus); std::uint8_t v = static_cast<std::uint8_t>(bus.read(a) - 1); setNZ(v); bus.write(a, v); return 5; }  // DEC !abs

    // ---- 8-bit shift / rotation (N,Z,C) ----
    case 0x1C: state_.a = aslOp(state_.a); return 2;                                              // ASL A
    case 0x0B: { std::uint16_t a = addrDp(bus);  bus.write(a, aslOp(bus.read(a))); return 4; }    // ASL dp
    case 0x1B: { std::uint16_t a = addrDpX(bus); bus.write(a, aslOp(bus.read(a))); return 5; }    // ASL dp+X
    case 0x0C: { std::uint16_t a = addrAbs(bus); bus.write(a, aslOp(bus.read(a))); return 5; }    // ASL !abs
    case 0x5C: state_.a = lsrOp(state_.a); return 2;                                              // LSR A
    case 0x4B: { std::uint16_t a = addrDp(bus);  bus.write(a, lsrOp(bus.read(a))); return 4; }    // LSR dp
    case 0x5B: { std::uint16_t a = addrDpX(bus); bus.write(a, lsrOp(bus.read(a))); return 5; }    // LSR dp+X
    case 0x4C: { std::uint16_t a = addrAbs(bus); bus.write(a, lsrOp(bus.read(a))); return 5; }    // LSR !abs
    case 0x3C: state_.a = rolOp(state_.a); return 2;                                              // ROL A
    case 0x2B: { std::uint16_t a = addrDp(bus);  bus.write(a, rolOp(bus.read(a))); return 4; }    // ROL dp
    case 0x3B: { std::uint16_t a = addrDpX(bus); bus.write(a, rolOp(bus.read(a))); return 5; }    // ROL dp+X
    case 0x2C: { std::uint16_t a = addrAbs(bus); bus.write(a, rolOp(bus.read(a))); return 5; }    // ROL !abs
    case 0x7C: state_.a = rorOp(state_.a); return 2;                                              // ROR A
    case 0x6B: { std::uint16_t a = addrDp(bus);  bus.write(a, rorOp(bus.read(a))); return 4; }    // ROR dp
    case 0x7B: { std::uint16_t a = addrDpX(bus); bus.write(a, rorOp(bus.read(a))); return 5; }    // ROR dp+X
    case 0x6C: { std::uint16_t a = addrAbs(bus); bus.write(a, rorOp(bus.read(a))); return 5; }    // ROR !abs

    // ---- nibble exchange (N,Z) ----
    case 0x9F:                                                                                    // XCN A
      state_.a = static_cast<std::uint8_t>((state_.a << 4) | (state_.a >> 4));
      setNZ(state_.a);
      return 5;

    default:
      // No opcode outside the MOV and 8-bit ALU families is implemented yet.
      // Returning a zero cycle count with no state change makes an unrouted
      // opcode fail the vector suite loudly.
      return 0;
  }
}

}  // namespace snaggletooth
