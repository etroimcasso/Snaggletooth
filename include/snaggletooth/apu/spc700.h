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

    default:
      // No non-MOV opcode is implemented yet. Returning a zero cycle count with
      // no state change makes an unrouted opcode fail the vector suite loudly.
      return 0;
  }
}

}  // namespace snaggletooth
