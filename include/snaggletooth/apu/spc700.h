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

  // 16-bit N and Z: N is bit 15 of the word, Z is set when the whole word is
  // zero. The other flags are untouched.
  void setNZ16(std::uint16_t v) noexcept {
    state_.psw = static_cast<std::uint8_t>((state_.psw & ~(kFlagN | kFlagZ)) |
                                           ((v & 0x8000u) ? kFlagN : 0) |
                                           (v == 0 ? kFlagZ : 0));
  }

  // 16-bit add core: two chained byte adds with the carry threaded from the low
  // byte into the high byte. N/V/H/C are taken from the high-byte add (H is the
  // nibble carry at bit 11->12, V its signed overflow); Z is the full 16-bit
  // result. ADDW passes carryIn 0; SUBW passes the ones-complement addend with
  // carryIn 1, so borrow, half-borrow and overflow all fall out of this one add.
  std::uint16_t addwCore(std::uint16_t ya, std::uint16_t addend,
                         unsigned carryIn) noexcept {
    const unsigned loA = ya & 0xFFu, loB = addend & 0xFFu;
    const unsigned hiA = (ya >> 8) & 0xFFu, hiB = (addend >> 8) & 0xFFu;
    const unsigned lo = loA + loB + carryIn;
    const unsigned hi = hiA + hiB + (lo >> 8);
    const std::uint16_t result =
        static_cast<std::uint16_t>(((hi & 0xFFu) << 8) | (lo & 0xFFu));
    const bool halfCarry = ((hiA & 0x0Fu) + (hiB & 0x0Fu) + (lo >> 8)) > 0x0Fu;
    const bool overflow = (~(hiA ^ hiB) & (hiA ^ hi) & 0x80u) != 0;
    std::uint8_t psw = static_cast<std::uint8_t>(
        state_.psw & ~(kFlagN | kFlagV | kFlagH | kFlagZ | kFlagC));
    if (result & 0x8000u) psw |= kFlagN;
    if (overflow) psw |= kFlagV;
    if (halfCarry) psw |= kFlagH;
    if (result == 0) psw |= kFlagZ;
    if (hi > 0xFFu) psw |= kFlagC;
    state_.psw = psw;
    return result;
  }
  std::uint16_t addwOp(std::uint16_t ya, std::uint16_t word) noexcept {
    return addwCore(ya, word, 0);
  }
  std::uint16_t subwOp(std::uint16_t ya, std::uint16_t word) noexcept {
    return addwCore(ya, static_cast<std::uint16_t>(~word), 1);
  }

  // 16-bit compare: N and Z from YA - word and C on no borrow (YA >= word). The
  // difference is discarded; H and V are untouched.
  void cmpwOp(std::uint16_t ya, std::uint16_t word) noexcept {
    const std::uint16_t diff = static_cast<std::uint16_t>(ya - word);
    std::uint8_t psw =
        static_cast<std::uint8_t>(state_.psw & ~(kFlagN | kFlagZ | kFlagC));
    if (diff & 0x8000u) psw |= kFlagN;
    if (diff == 0) psw |= kFlagZ;
    if (ya >= word) psw |= kFlagC;
    state_.psw = psw;
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

  // A 16-bit word in the direct page: the two bytes live at dp and dp+1, and the
  // high byte's address wraps within the direct page (dp = $FF reads its high
  // byte from the page base).
  template <ApuBus B>
  std::uint16_t readWordDp(B& bus, std::uint8_t d) {
    std::uint16_t lo = bus.read(static_cast<std::uint16_t>(dpBase() + d));
    std::uint16_t hi =
        bus.read(static_cast<std::uint16_t>(dpBase() + ((d + 1) & 0xFF)));
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  template <ApuBus B>
  void writeWordDp(B& bus, std::uint8_t d, std::uint16_t v) {
    bus.write(static_cast<std::uint16_t>(dpBase() + d),
              static_cast<std::uint8_t>(v));
    bus.write(static_cast<std::uint16_t>(dpBase() + ((d + 1) & 0xFF)),
              static_cast<std::uint8_t>(v >> 8));
  }

  // The stack lives in page $01, addressed by SP as its low byte. A push writes
  // then post-decrements SP; a pop pre-increments then reads. SP is an 8-bit
  // register, so it wraps within the page.
  template <ApuBus B>
  void push(B& bus, std::uint8_t v) {
    bus.write(static_cast<std::uint16_t>(0x0100u + state_.sp), v);
    --state_.sp;
  }
  template <ApuBus B>
  std::uint8_t pop(B& bus) {
    ++state_.sp;
    return bus.read(static_cast<std::uint16_t>(0x0100u + state_.sp));
  }
  // A 16-bit value on the stack: the high byte is pushed first, so the low byte
  // lands at the lower address and is the first one popped back.
  template <ApuBus B>
  void pushWord(B& bus, std::uint16_t w) {
    push(bus, static_cast<std::uint8_t>(w >> 8));
    push(bus, static_cast<std::uint8_t>(w));
  }
  template <ApuBus B>
  std::uint16_t popWord(B& bus) {
    std::uint16_t lo = pop(bus);
    std::uint16_t hi = pop(bus);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }

  // Consumes the relative-offset byte and, when the condition holds, adds it to
  // PC as a signed displacement — PC already points past the whole instruction.
  // Returns the taken or not-taken cycle count.
  template <ApuBus B>
  std::uint32_t branchIf(B& bus, bool taken, std::uint32_t takenCycles,
                         std::uint32_t notTakenCycles) {
    const std::int8_t rel = static_cast<std::int8_t>(fetch(bus));
    if (taken) {
      state_.pc = static_cast<std::uint16_t>(state_.pc + rel);
      return takenCycles;
    }
    return notTakenCycles;
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

    // ---- 16-bit word moves (YA is the pair A=low, Y=high; N,Z from the word) ----
    case 0xBA: {                                                                                  // MOVW YA,dp
      std::uint16_t w = readWordDp(bus, fetch(bus));
      state_.a = static_cast<std::uint8_t>(w);
      state_.y = static_cast<std::uint8_t>(w >> 8);
      setNZ16(w);
      return 5;
    }
    case 0xDA: {                                                                                  // MOVW dp,YA (no flags)
      std::uint8_t d = fetch(bus);
      static_cast<void>(bus.read(static_cast<std::uint16_t>(dpBase() + d)));  // documented dummy read of the low byte
      writeWordDp(bus, d, static_cast<std::uint16_t>(state_.a | (state_.y << 8)));
      return 5;
    }

    // ---- 16-bit word increment / decrement (N,Z from the word) ----
    case 0x3A: {                                                                                  // INCW dp
      std::uint8_t d = fetch(bus);
      std::uint16_t w = static_cast<std::uint16_t>(readWordDp(bus, d) + 1);
      setNZ16(w);
      writeWordDp(bus, d, w);
      return 6;
    }
    case 0x1A: {                                                                                  // DECW dp
      std::uint8_t d = fetch(bus);
      std::uint16_t w = static_cast<std::uint16_t>(readWordDp(bus, d) - 1);
      setNZ16(w);
      writeWordDp(bus, d, w);
      return 6;
    }

    // ---- 16-bit word arithmetic (N,V,H,Z,C; H is the carry on the high byte) ----
    case 0x7A: {                                                                                  // ADDW YA,dp
      std::uint16_t r = addwOp(static_cast<std::uint16_t>(state_.a | (state_.y << 8)),
                               readWordDp(bus, fetch(bus)));
      state_.a = static_cast<std::uint8_t>(r);
      state_.y = static_cast<std::uint8_t>(r >> 8);
      return 5;
    }
    case 0x9A: {                                                                                  // SUBW YA,dp
      std::uint16_t r = subwOp(static_cast<std::uint16_t>(state_.a | (state_.y << 8)),
                               readWordDp(bus, fetch(bus)));
      state_.a = static_cast<std::uint8_t>(r);
      state_.y = static_cast<std::uint8_t>(r >> 8);
      return 5;
    }
    case 0x5A:                                                                                    // CMPW YA,dp
      cmpwOp(static_cast<std::uint16_t>(state_.a | (state_.y << 8)),
             readWordDp(bus, fetch(bus)));
      return 4;

    // ---- multiply / divide ----
    case 0xCF: {                                                                                  // MUL YA (YA = Y*A; N,Z from Y only)
      std::uint16_t p = static_cast<std::uint16_t>(unsigned{state_.y} * unsigned{state_.a});
      state_.a = static_cast<std::uint8_t>(p);
      state_.y = static_cast<std::uint8_t>(p >> 8);
      setNZ(state_.y);
      return 9;
    }
    case 0x9E: {                                                                                  // DIV YA,X (A = YA/X, Y = YA%X)
      // The documented 9-iteration restoring division: the 17-bit accumulator
      // ends as YYYYYYYY V AAAAAAAA, so Y and A are the quotient/remainder and
      // bit 8 is the overflow flag. N,Z come from A; H is the nibble comparison
      // X&$F <= Y&$F on the entry values. The result past a quotient of 511 is
      // hardware garbage the algorithm still reproduces.
      const unsigned entryX = state_.x, entryY = state_.y;
      std::uint32_t yva = static_cast<std::uint32_t>((entryY << 8) | state_.a);
      const std::uint32_t x9 = static_cast<std::uint32_t>(entryX) << 9;
      for (int i = 0; i < 9; ++i) {
        yva = ((yva << 1) | ((yva >> 16) & 1u)) & 0x1FFFFu;  // rotate left within 17 bits
        if (yva >= x9) yva ^= 1u;
        if (yva & 1u) yva = (yva - x9) & 0x1FFFFu;
      }
      state_.a = static_cast<std::uint8_t>(yva & 0xFFu);
      state_.y = static_cast<std::uint8_t>((yva >> 9) & 0xFFu);
      std::uint8_t psw = static_cast<std::uint8_t>(
          state_.psw & ~(kFlagN | kFlagV | kFlagH | kFlagZ));
      if (state_.a & 0x80u) psw |= kFlagN;
      if (state_.a == 0) psw |= kFlagZ;
      if (yva & 0x100u) psw |= kFlagV;
      if ((entryX & 0x0Fu) <= (entryY & 0x0Fu)) psw |= kFlagH;
      state_.psw = psw;
      return 12;
    }

    // ---- decimal adjust (N,Z,C; H is read but never changed) ----
    case 0xDF: {                                                                                  // DAA A
      std::uint16_t a = state_.a;
      if ((state_.psw & kFlagC) || a > 0x99u) {
        a += 0x60u;
        state_.psw = static_cast<std::uint8_t>(state_.psw | kFlagC);
      }
      if ((state_.psw & kFlagH) || (state_.a & 0x0Fu) > 0x09u) a += 0x06u;
      state_.a = static_cast<std::uint8_t>(a);
      setNZ(state_.a);
      return 3;
    }
    case 0xBE: {                                                                                  // DAS A
      std::uint16_t a = state_.a;
      if (!(state_.psw & kFlagC) || a > 0x99u) {
        a -= 0x60u;
        state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagC);
      }
      if (!(state_.psw & kFlagH) || (state_.a & 0x0Fu) > 0x09u) a -= 0x06u;
      state_.a = static_cast<std::uint8_t>(a);
      setNZ(state_.a);
      return 3;
    }

    // ---- single-bit set / clear on a direct-page byte (bit in opcode; no flags) ----
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2:       // SET1 dp.0..7
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2: {     // CLR1 dp.0..7
      const std::uint8_t mask = static_cast<std::uint8_t>(1u << (opcode >> 5));
      const std::uint16_t addr = addrDp(bus);
      const std::uint8_t m = bus.read(addr);
      bus.write(addr, (opcode & 0x10u) ? static_cast<std::uint8_t>(m & ~mask)
                                       : static_cast<std::uint8_t>(m | mask));
      return 4;
    }

    // ---- test and set/clear bits in an absolute byte (N,Z as for A - memory) ----
    case 0x0E: {                                                                                  // TSET1 !abs
      const std::uint16_t addr = addrAbs(bus);
      const std::uint8_t m = bus.read(addr);
      setNZ(static_cast<std::uint8_t>(state_.a - m));
      bus.write(addr, static_cast<std::uint8_t>(m | state_.a));
      return 6;
    }
    case 0x4E: {                                                                                  // TCLR1 !abs
      const std::uint16_t addr = addrAbs(bus);
      const std::uint8_t m = bus.read(addr);
      setNZ(static_cast<std::uint8_t>(state_.a - m));
      bus.write(addr, static_cast<std::uint8_t>(m & ~state_.a));
      return 6;
    }

    // ---- carry-bit logic against one bit of an absolute byte (the 16-bit
    //      operand carries a 13-bit address in the low bits and the bit index in
    //      the top 3; only C is affected) ----
    case 0x4A: {                                                                                  // AND1 C,m.b
      const std::uint16_t operand = addrAbs(bus);
      const bool b = ((bus.read(static_cast<std::uint16_t>(operand & 0x1FFFu)) >>
                       (operand >> 13)) & 1u) != 0u;
      setCarry((state_.psw & kFlagC) && b);
      return 4;
    }
    case 0x6A: {                                                                                  // AND1 C,/m.b
      const std::uint16_t operand = addrAbs(bus);
      const bool b = ((bus.read(static_cast<std::uint16_t>(operand & 0x1FFFu)) >>
                       (operand >> 13)) & 1u) != 0u;
      setCarry((state_.psw & kFlagC) && !b);
      return 4;
    }
    case 0x0A: {                                                                                  // OR1 C,m.b
      const std::uint16_t operand = addrAbs(bus);
      const bool b = ((bus.read(static_cast<std::uint16_t>(operand & 0x1FFFu)) >>
                       (operand >> 13)) & 1u) != 0u;
      setCarry((state_.psw & kFlagC) || b);
      return 5;
    }
    case 0x2A: {                                                                                  // OR1 C,/m.b
      const std::uint16_t operand = addrAbs(bus);
      const bool b = ((bus.read(static_cast<std::uint16_t>(operand & 0x1FFFu)) >>
                       (operand >> 13)) & 1u) != 0u;
      setCarry((state_.psw & kFlagC) || !b);
      return 5;
    }
    case 0x8A: {                                                                                  // EOR1 C,m.b
      const std::uint16_t operand = addrAbs(bus);
      const bool b = ((bus.read(static_cast<std::uint16_t>(operand & 0x1FFFu)) >>
                       (operand >> 13)) & 1u) != 0u;
      setCarry(((state_.psw & kFlagC) != 0) != b);
      return 5;
    }
    case 0xEA: {                                                                                  // NOT1 m.b (no flags)
      const std::uint16_t operand = addrAbs(bus);
      const std::uint16_t addr = static_cast<std::uint16_t>(operand & 0x1FFFu);
      const std::uint8_t mask = static_cast<std::uint8_t>(1u << (operand >> 13));
      bus.write(addr, static_cast<std::uint8_t>(bus.read(addr) ^ mask));
      return 5;
    }
    case 0xAA: {                                                                                  // MOV1 C,m.b
      const std::uint16_t operand = addrAbs(bus);
      const bool b = ((bus.read(static_cast<std::uint16_t>(operand & 0x1FFFu)) >>
                       (operand >> 13)) & 1u) != 0u;
      setCarry(b);
      return 4;
    }
    case 0xCA: {                                                                                  // MOV1 m.b,C (no flags)
      const std::uint16_t operand = addrAbs(bus);
      const std::uint16_t addr = static_cast<std::uint16_t>(operand & 0x1FFFu);
      const std::uint8_t mask = static_cast<std::uint8_t>(1u << (operand >> 13));
      const std::uint8_t m = bus.read(addr);
      bus.write(addr, (state_.psw & kFlagC) ? static_cast<std::uint8_t>(m | mask)
                                            : static_cast<std::uint8_t>(m & ~mask));
      return 6;
    }

    // ---- conditional and unconditional relative branches (2 cycles, +2 when
    //      taken; BRA is always taken) ----
    case 0x2F: return branchIf(bus, true, 4, 4);                                                  // BRA
    case 0xF0: return branchIf(bus, (state_.psw & kFlagZ) != 0, 4, 2);                            // BEQ
    case 0xD0: return branchIf(bus, (state_.psw & kFlagZ) == 0, 4, 2);                            // BNE
    case 0xB0: return branchIf(bus, (state_.psw & kFlagC) != 0, 4, 2);                            // BCS
    case 0x90: return branchIf(bus, (state_.psw & kFlagC) == 0, 4, 2);                            // BCC
    case 0x70: return branchIf(bus, (state_.psw & kFlagV) != 0, 4, 2);                            // BVS
    case 0x50: return branchIf(bus, (state_.psw & kFlagV) == 0, 4, 2);                            // BVC
    case 0x30: return branchIf(bus, (state_.psw & kFlagN) != 0, 4, 2);                            // BMI
    case 0x10: return branchIf(bus, (state_.psw & kFlagN) == 0, 4, 2);                            // BPL

    // ---- branch on one direct-page bit: BBS on set / BBC on clear. The bit index
    //      is in opcode bits 5-7; BBC is the odd high nibble (5/7 cycles) ----
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3:       // BBS dp.0..7,rel
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3: {     // BBC dp.0..7,rel
      const std::uint8_t high = static_cast<std::uint8_t>(opcode >> 4);
      const std::uint8_t bit = static_cast<std::uint8_t>(high >> 1);
      const bool branchOnSet = (high & 1u) == 0u;  // even high nibble = BBS
      const std::uint8_t m = bus.read(addrDp(bus));
      const bool isSet = ((m >> bit) & 1u) != 0u;
      return branchIf(bus, isSet == branchOnSet, 7, 5);
    }

    // ---- compare-and-branch / decrement-and-branch. The internal compare or
    //      decrement leaves PSW untouched — these set no flags ----
    case 0x2E: return branchIf(bus, state_.a != bus.read(addrDp(bus)),  7, 5);                    // CBNE dp,rel
    case 0xDE: return branchIf(bus, state_.a != bus.read(addrDpX(bus)), 8, 6);                    // CBNE dp+X,rel
    case 0x6E: {                                                                                  // DBNZ dp,rel
      const std::uint16_t addr = addrDp(bus);
      const std::uint8_t v = static_cast<std::uint8_t>(bus.read(addr) - 1);
      bus.write(addr, v);
      return branchIf(bus, v != 0, 7, 5);
    }
    case 0xFE: --state_.y; return branchIf(bus, state_.y != 0, 6, 4);                             // DBNZ Y,rel

    // ---- jumps ----
    case 0x5F: state_.pc = addrAbs(bus); return 3;                                                // JMP !abs
    case 0x1F: {                                                                                  // JMP [!abs+X]
      const std::uint16_t ptr = addrAbsX(bus);
      const std::uint16_t lo = bus.read(ptr);
      const std::uint16_t hi = bus.read(static_cast<std::uint16_t>(ptr + 1));
      state_.pc = static_cast<std::uint16_t>(lo | (hi << 8));
      return 6;
    }

    // ---- subroutine calls and returns. The pushed return address is the next
    //      instruction with no 6502-style pre-decrement; RET pops it directly ----
    case 0x3F: {                                                                                  // CALL !abs
      const std::uint16_t target = addrAbs(bus);
      pushWord(bus, state_.pc);
      state_.pc = target;
      return 8;
    }
    case 0x4F: {                                                                                  // PCALL up (CALL $FF00+up)
      const std::uint16_t target = static_cast<std::uint16_t>(0xFF00u | fetch(bus));
      pushWord(bus, state_.pc);
      state_.pc = target;
      return 6;
    }
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:       // TCALL 0..7
    case 0x81: case 0x91: case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1: {     // TCALL 8..15
      const std::uint8_t n = static_cast<std::uint8_t>(opcode >> 4);
      const std::uint16_t vec = static_cast<std::uint16_t>(0xFFDEu - 2u * n);
      pushWord(bus, state_.pc);
      const std::uint16_t lo = bus.read(vec);
      const std::uint16_t hi = bus.read(static_cast<std::uint16_t>(vec + 1));
      state_.pc = static_cast<std::uint16_t>(lo | (hi << 8));
      return 8;
    }
    case 0x6F: state_.pc = popWord(bus); return 5;                                                // RET
    case 0x7F: {                                                                                  // RETI
      state_.psw = pop(bus);
      state_.pc = popWord(bus);
      return 6;
    }
    case 0x0F: {                                                                                  // BRK
      pushWord(bus, state_.pc);
      push(bus, state_.psw);
      state_.psw = static_cast<std::uint8_t>((state_.psw | kFlagB) & ~kFlagI);
      const std::uint16_t lo = bus.read(static_cast<std::uint16_t>(0xFFDEu));
      const std::uint16_t hi = bus.read(static_cast<std::uint16_t>(0xFFDFu));
      state_.pc = static_cast<std::uint16_t>(lo | (hi << 8));
      return 8;
    }

    // ---- stack push / pop (POP sets no flags except POP PSW, which restores
    //      the whole status word) ----
    case 0x2D: push(bus, state_.a);   return 4;                                                   // PUSH A
    case 0x4D: push(bus, state_.x);   return 4;                                                   // PUSH X
    case 0x6D: push(bus, state_.y);   return 4;                                                   // PUSH Y
    case 0x0D: push(bus, state_.psw); return 4;                                                   // PUSH PSW
    case 0xAE: state_.a = pop(bus);   return 4;                                                   // POP A
    case 0xCE: state_.x = pop(bus);   return 4;                                                   // POP X
    case 0xEE: state_.y = pop(bus);   return 4;                                                   // POP Y
    case 0x8E: state_.psw = pop(bus); return 4;                                                   // POP PSW

    // ---- status-flag operations ----
    case 0x60: state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagC); return 2;            // CLRC
    case 0x80: state_.psw = static_cast<std::uint8_t>(state_.psw |  kFlagC); return 2;            // SETC
    case 0xED: state_.psw = static_cast<std::uint8_t>(state_.psw ^  kFlagC); return 3;            // NOTC
    case 0xE0: state_.psw = static_cast<std::uint8_t>(state_.psw & ~(kFlagV | kFlagH)); return 2; // CLRV (clears V and H)
    case 0x20: state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagP); return 2;            // CLRP
    case 0x40: state_.psw = static_cast<std::uint8_t>(state_.psw |  kFlagP); return 2;            // SETP
    case 0xA0: state_.psw = static_cast<std::uint8_t>(state_.psw |  kFlagI); return 3;            // EI
    case 0xC0: state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagI); return 3;            // DI

    // ---- no-operation and halt. SLEEP and STOP advance PC past the opcode, set
    //      the run state, and take 7 cycles — the opcode fetch, a dummy read of
    //      the next byte, then the halt loop's wait/read cycles the oracle
    //      captures (the SNESdev table's 3/2 are its notional counts). A later
    //      step on a halted core returns 2 and touches nothing (the guard above). ----
    case 0x00: return 2;                                                                          // NOP
    case 0xEF: state_.run = RunState::Sleeping; return 7;                                         // SLEEP
    case 0xFF: state_.run = RunState::Stopped;  return 7;                                         // STOP

    default:
      // Every opcode is now routed; this arm is unreachable. It stays as a loud
      // failure — zero cycles, no state change — so any future dispatch gap
      // reddens the vector suite instead of passing silently.
      return 0;
  }
}

}  // namespace snaggletooth
