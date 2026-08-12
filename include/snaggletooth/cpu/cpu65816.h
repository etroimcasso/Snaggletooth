#pragma once

// The 65816 — the SNES main CPU core.
//
// The core is cycle-stepped: stepCycle() executes exactly one chip cycle, and every
// cycle is narrated through the bus — a cycle that reaches memory issues a read or a
// write, and a cycle that does not still drives an address, reported as internal().
// A machine that prices memory by region, or seizes the bus part-way through an
// instruction, needs that grain; so does a test that compares the core against a
// per-cycle recording of the real chip.
//
// Instruction progress is part of the state value, not the call stack: an
// instruction register, a cycle index within the instruction, and a few scratch
// fields. So state() and restore() are legal at any cycle, mid-instruction included,
// and a snapshot is still a plain copy of the struct.
//
// stepInstruction() runs cycles to the next instruction boundary and returns how
// many it took — the whole-instruction view, for hosts and for tests that care about
// final state. Cycle totals are never computed from a formula; they are however many
// cycles the instruction actually executed.
//
// The core runs over an abstract bus (the SnesBus concept): the flat test harness
// and the full SNES machine both satisfy it, so the same interpreter drives plain
// 16 MB RAM and, later, the mapped system bus. Reads may have side effects on real
// hardware, so the core issues every documented memory access.
//
// The 65816 is a 6502 grown to 24-bit addressing and 16-bit data. Two mode flags
// pick the data widths: the m flag sizes the accumulator and memory (8-bit when
// set, 16-bit when clear) and the x flag sizes the X and Y index registers the same
// way. A separate emulation flag (e) reproduces a 6502: while e is set, m and x are
// forced set, the stack is pinned to page one, and the index high bytes read zero.

#include <concepts>
#include <cstdint>

namespace snaggletooth {

// What a bus access is for. A machine prices a cycle by the region it reaches, and
// the pins the chip drives differ by purpose: an opcode fetch asserts both program
// and data valid, an operand fetch only program, a data access only data, the
// cycles of a read-modify-write also assert the memory lock, and an interrupt
// vector pull asserts the vector-pull pin.
enum class CycleKind : std::uint8_t {
  OpcodeFetch,
  OperandFetch,
  DataRead,
  DataWrite,
  RmwRead,
  RmwWrite,
  // The cycle between a read-modify-write's read and its write, where the value
  // changes inside the chip. No address is valid and nothing crosses the bus, but
  // the memory lock stays asserted across it.
  RmwModify,
  // The same cycle in emulation mode, where the chip drives a write of the byte it
  // just read before writing the modified one — so a read-modify-write writes its
  // address twice there. Emulation mode holds the read/write line low for both.
  RmwModifyWrite,
  VectorRead,
};

// The 65816's view of the system: a 24-bit address space with 8-bit data. A
// conforming bus answers an 8-bit read for any address, accepts an 8-bit write to
// one, and accepts internal(), the cycle that drives an address without a valid
// access — with the kind given when the pins differ from a plain internal cycle.
// Only the low 24 bits of an address are meaningful.
template <typename B>
concept SnesBus = requires(B bus, std::uint32_t address, std::uint8_t value,
                           CycleKind kind) {
  { bus.read(address, kind) } -> std::same_as<std::uint8_t>;
  bus.write(address, value, kind);
  bus.internal(address);
  bus.internal(address, kind);
};

enum class CpuRunState : std::uint8_t { Running, Waiting, Stopped };

// Processor-status flag masks. In emulation mode bit 4 is the break flag rather
// than x, and bit 5 is unused; the m and x widths are forced regardless, so the
// width queries below read the e flag directly instead of these bits.
inline constexpr std::uint8_t kCpuFlagN = 0x80;  // negative (high bit of the result)
inline constexpr std::uint8_t kCpuFlagV = 0x40;  // signed overflow
inline constexpr std::uint8_t kCpuFlagM = 0x20;  // 8-bit accumulator/memory when set
inline constexpr std::uint8_t kCpuFlagX = 0x10;  // 8-bit index registers when set
inline constexpr std::uint8_t kCpuFlagD = 0x08;  // decimal mode
inline constexpr std::uint8_t kCpuFlagI = 0x04;  // interrupt disable
inline constexpr std::uint8_t kCpuFlagZ = 0x02;  // zero
inline constexpr std::uint8_t kCpuFlagC = 0x01;  // carry

// The whole CPU state as a value: snapshot by copy, restore by assignment. The
// accumulator, index registers, direct register and stack pointer are 16 bits;
// which halves are live depends on the m and x widths. `e` is the emulation flag
// (the hidden bit XCE exchanges with carry).
struct Cpu65816State {
  std::uint16_t pc = 0;   // program counter (within the program bank)
  std::uint16_t s = 0;    // stack pointer
  std::uint16_t a = 0;    // accumulator (A is the low byte, B the high byte)
  std::uint16_t x = 0;    // X index register
  std::uint16_t y = 0;    // Y index register
  std::uint16_t d = 0;    // direct-page register
  std::uint8_t p = 0;     // processor status
  std::uint8_t dbr = 0;   // data bank register
  std::uint8_t pbr = 0;   // program bank register
  bool e = false;         // emulation mode

  // How far into an instruction the core is. These carry no architectural meaning
  // — they are the progress a running instruction has made, held as values so a
  // snapshot taken mid-instruction restores to the same cycle.
  std::uint8_t ir = 0;       // the instruction being executed
  std::uint8_t tcu = 0;      // cycle index within it; 0 means the next cycle fetches
  std::uint32_t ea = 0;      // effective-address scratch
  std::uint16_t ptr = 0;     // indirect-pointer scratch
  std::uint16_t tmp = 0;     // data scratch (a low byte latched while the high pends)
  bool pageCross = false;    // an index addition carried past a page

  // The interrupt inputs, sampled from the pins the machine drives.
  bool nmiPending = false;   // latched by a high-to-low edge on the NMI line
  bool irqLine = false;      // the IRQ line's current level

  CpuRunState run = CpuRunState::Running;
};

class Cpu65816 {
 public:
  Cpu65816() = default;
  explicit Cpu65816(Cpu65816State state) : state_(state) {}

  [[nodiscard]] const Cpu65816State& state() const noexcept { return state_; }
  void restore(Cpu65816State state) noexcept { state_ = state; }

  // Executes exactly one chip cycle. The cycle either reaches memory — a read or a
  // write, tagged with what it is for — or drives an address without a valid
  // access, reported as internal(). A halted core (waiting or stopped) touches the
  // bus not at all; its host prices the idle cycle from the run state.
  template <SnesBus B>
  void stepCycle(B& bus);

  // Runs cycles until the next instruction boundary and returns how many it took.
  // Called mid-instruction it finishes the instruction in progress rather than
  // starting one. The count is what executed, not a formula.
  template <SnesBus B>
  std::uint32_t stepInstruction(B& bus);

  // Whether the core sits between instructions — the only point at which an
  // instruction can begin, and the state a completed instruction leaves behind.
  [[nodiscard]] bool atInstructionBoundary() const noexcept { return state_.tcu == 0; }

  // The interrupt lines, driven by the machine. NMI is edge-sensitive: the falling
  // edge latches a pending request that survives until it is serviced. IRQ is
  // level-sensitive: the core samples the line's current level, so a source must
  // hold it asserted until acknowledged.
  void setNmiLine(bool asserted) noexcept {
    if (asserted && !nmiLine_) state_.nmiPending = true;
    nmiLine_ = asserted;
  }
  void setIrqLine(bool asserted) noexcept { state_.irqLine = asserted; }

  // Whether an opcode runs on the cycle engine. The engine carries the implied,
  // accumulator, immediate and direct-page instructions; the rest execute whole, so
  // stepCycle cannot observe them a cycle at a time while stepInstruction runs them
  // correctly. Ask before stepping an instruction cycle by cycle.
  [[nodiscard]] static constexpr bool cycleStepped(std::uint8_t opcode) noexcept;

 private:
  // ---- widths -------------------------------------------------------------
  // The accumulator/memory is 8-bit when the m flag is set; the index registers
  // are 8-bit when the x flag is set. Emulation mode forces both to 8-bit.
  [[nodiscard]] bool accum8() const noexcept {
    return state_.e || (state_.p & kCpuFlagM);
  }
  [[nodiscard]] bool index8() const noexcept {
    return state_.e || (state_.p & kCpuFlagX);
  }
  // The cycle-count adjustment terms: one extra cycle per extra byte moved for a
  // 16-bit access, and one extra when the direct register's low byte is non-zero.
  [[nodiscard]] std::uint32_t accCyc() const noexcept { return accum8() ? 1u : 0u; }
  [[nodiscard]] std::uint32_t indexCyc() const noexcept { return index8() ? 1u : 0u; }
  [[nodiscard]] std::uint32_t dpCyc() const noexcept {
    return (state_.d & 0xFFu) != 0u ? 1u : 0u;
  }
  // An index register as used in an address computation: the high byte reads zero
  // when the index registers are 8-bit.
  [[nodiscard]] std::uint16_t idxX() const noexcept {
    return index8() ? static_cast<std::uint16_t>(state_.x & 0xFFu) : state_.x;
  }
  [[nodiscard]] std::uint16_t idxY() const noexcept {
    return index8() ? static_cast<std::uint16_t>(state_.y & 0xFFu) : state_.y;
  }

  // Reapplies the mode invariants that hold continuously: the index high bytes
  // are zero while the index registers are 8-bit, and the stack pointer's high
  // byte is $01 while emulation mode is on.
  void normalize() noexcept {
    if (index8()) {
      state_.x &= 0xFFu;
      state_.y &= 0xFFu;
    }
    if (state_.e) state_.s = static_cast<std::uint16_t>(0x0100u | (state_.s & 0xFFu));
  }

  // ---- flags --------------------------------------------------------------
  void setNZ8(std::uint8_t v) noexcept {
    state_.p = static_cast<std::uint8_t>((state_.p & ~(kCpuFlagN | kCpuFlagZ)) |
                                         (v & kCpuFlagN) | (v == 0 ? kCpuFlagZ : 0));
  }
  void setNZ16(std::uint16_t v) noexcept {
    state_.p = static_cast<std::uint8_t>((state_.p & ~(kCpuFlagN | kCpuFlagZ)) |
                                         ((v & 0x8000u) ? kCpuFlagN : 0) |
                                         (v == 0 ? kCpuFlagZ : 0));
  }
  void setCarry(bool carry) noexcept {
    state_.p = static_cast<std::uint8_t>(carry ? (state_.p | kCpuFlagC)
                                               : (state_.p & ~kCpuFlagC));
  }
  void setOverflow(bool v) noexcept {
    state_.p = static_cast<std::uint8_t>(v ? (state_.p | kCpuFlagV)
                                           : (state_.p & ~kCpuFlagV));
  }
  void setN(bool n) noexcept {
    state_.p = static_cast<std::uint8_t>(n ? (state_.p | kCpuFlagN)
                                           : (state_.p & ~kCpuFlagN));
  }
  void setZ(bool z) noexcept {
    state_.p = static_cast<std::uint8_t>(z ? (state_.p | kCpuFlagZ)
                                           : (state_.p & ~kCpuFlagZ));
  }

  // Replaces the whole status byte and reapplies the invariants it governs.
  // Emulation mode holds the m and x bits set, and narrowing the index registers to
  // 8-bit clears their high bytes at once — so PLP, REP and SEP settle the width the
  // same way XCE does, within the instruction rather than on the next one.
  void writeP(std::uint8_t v) noexcept {
    if (state_.e) v = static_cast<std::uint8_t>(v | kCpuFlagM | kCpuFlagX);
    state_.p = v;
    normalize();
  }

  // Loads leave the unused half of a register alone (an 8-bit load preserves the
  // high byte of the accumulator; an 8-bit index load clears the index high byte)
  // and set N and Z from the loaded value at its width.
  void loadA(std::uint16_t v) noexcept {
    if (accum8()) {
      state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | (v & 0xFFu));
      setNZ8(static_cast<std::uint8_t>(v));
    } else {
      state_.a = v;
      setNZ16(v);
    }
  }
  void loadX(std::uint16_t v) noexcept {
    if (index8()) {
      state_.x = static_cast<std::uint16_t>(v & 0xFFu);
      setNZ8(static_cast<std::uint8_t>(v));
    } else {
      state_.x = v;
      setNZ16(v);
    }
  }
  void loadY(std::uint16_t v) noexcept {
    if (index8()) {
      state_.y = static_cast<std::uint16_t>(v & 0xFFu);
      setNZ8(static_cast<std::uint8_t>(v));
    } else {
      state_.y = v;
      setNZ16(v);
    }
  }

  // ---- arithmetic and logic -----------------------------------------------
  // ADC and SBC share one adder: SBC adds the ones-complement of the operand with
  // the same carry in, so the borrow, the carry and the signed overflow all fall
  // out of the same addition (A - M - !C is A + ~M + C). Binary mode is a plain
  // two's-complement add; decimal mode adjusts each BCD nibble on the fly and costs
  // no extra cycle. N, Z and C describe the final result at the accumulator width.
  // The v flag is a signed-overflow flag; BCD is an unsigned representation, so v
  // has no defined meaning in decimal mode (it is computed but not meaningful).
  void adcOp(std::uint16_t operand) { addWithCarry(operand, /*subtract=*/false); }
  void sbcOp(std::uint16_t operand) { addWithCarry(operand, /*subtract=*/true); }

  // BCD subtract, nibble by nibble: each digit is a binary subtract with the borrow
  // from below; a digit that underflows is corrected by subtracting six and borrows
  // into the next. Matches the hardware on out-of-range nibbles too. `digits` is 2
  // for an 8-bit accumulator, 4 for 16-bit. Only the result is decimal; the carry
  // and overflow flags come from the binary subtraction.
  [[nodiscard]] static std::uint32_t decimalSubtract(std::uint32_t a, std::uint32_t operand,
                                                     std::uint32_t carryIn, int digits) noexcept {
    int borrow = static_cast<int>(1u - carryIn);
    std::uint32_t res = 0;
    for (int i = 0; i < digits; ++i) {
      const int an = static_cast<int>((a >> (4 * i)) & 0x0Fu);
      const int mn = static_cast<int>((operand >> (4 * i)) & 0x0Fu);
      int d = an - mn - borrow;
      if (d < 0) {
        d -= 0x06;
        borrow = 1;
      } else {
        borrow = 0;
      }
      res |= static_cast<std::uint32_t>(d & 0x0F) << (4 * i);
    }
    return res;
  }

  void addWithCarry(std::uint16_t operand, bool subtract) {
    const std::uint32_t cin = (state_.p & kCpuFlagC) ? 1u : 0u;
    const bool decimal = (state_.p & kCpuFlagD) != 0;
    if (accum8()) {
      const std::uint32_t a = state_.a & 0xFFu;
      const std::uint32_t m = (subtract ? (operand ^ 0xFFu) : operand) & 0xFFu;
      const std::uint32_t bin = a + m + cin;  // the binary sum of the ones-complement form
      const bool carryBin = (bin & 0x100u) != 0;
      // Overflow comes from the binary sum for a binary add and for SBC in either
      // mode; decimal ADC takes it from the high-nibble sum before the final adjust.
      bool ovf = ((~(a ^ m) & (a ^ (bin & 0xFFu))) & 0x80u) != 0;
      std::uint32_t res = bin & 0xFFu;
      bool carry = carryBin;
      if (decimal && !subtract) {
        std::uint32_t lo = (a & 0x0Fu) + (operand & 0x0Fu) + cin;
        if (lo > 0x09u) lo = ((lo + 0x06u) & 0x0Fu) + 0x10u;
        std::uint32_t t = (a & 0xF0u) + (operand & 0xF0u) + lo;
        ovf = ((~(a ^ operand) & (a ^ t)) & 0x80u) != 0;
        if (t > 0x9Fu) t += 0x60u;
        carry = t > 0xFFu;
        res = t & 0xFFu;
      } else if (decimal) {
        res = decimalSubtract(a, operand, cin, 2);
      }
      state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | res);
      setNZ8(static_cast<std::uint8_t>(res));
      setCarry(carry);
      setOverflow(ovf);
    } else {
      const std::uint32_t a = state_.a;
      const std::uint32_t m = (subtract ? (operand ^ 0xFFFFu) : operand) & 0xFFFFu;
      const std::uint32_t bin = a + m + cin;
      const bool carryBin = (bin & 0x10000u) != 0;
      bool ovf = ((~(a ^ m) & (a ^ (bin & 0xFFFFu))) & 0x8000u) != 0;
      std::uint32_t res = bin & 0xFFFFu;
      bool carry = carryBin;
      if (decimal && !subtract) {
        std::uint32_t lo = (a & 0x000Fu) + (operand & 0x000Fu) + cin;
        if (lo > 0x09u) lo = ((lo + 0x06u) & 0x000Fu) + 0x10u;
        std::uint32_t t = (a & 0x00F0u) + (operand & 0x00F0u) + lo;
        if (t > 0x9Fu) t = ((t + 0x60u) & 0x00FFu) + 0x100u;
        t = (a & 0x0F00u) + (operand & 0x0F00u) + t;
        if (t > 0x9FFu) t = ((t + 0x600u) & 0x0FFFu) + 0x1000u;
        t = (a & 0xF000u) + (operand & 0xF000u) + t;
        ovf = ((~(a ^ operand) & (a ^ t)) & 0x8000u) != 0;
        if (t > 0x9FFFu) t += 0x6000u;
        carry = t > 0xFFFFu;
        res = t & 0xFFFFu;
      } else if (decimal) {
        res = decimalSubtract(a, operand, cin, 4);
      }
      state_.a = static_cast<std::uint16_t>(res);
      setNZ16(static_cast<std::uint16_t>(res));
      setCarry(carry);
      setOverflow(ovf);
    }
  }

  // CMP/CPX/CPY subtract the operand from a register without carry-in and without
  // storing: only N, Z and C move (no overflow), C set when the register is at or
  // above the operand (no borrow). The comparison runs at the register's width.
  void compareWith(std::uint16_t reg, std::uint16_t operand, bool eightBit) {
    if (eightBit) {
      const std::uint8_t r = static_cast<std::uint8_t>(reg) -
                             static_cast<std::uint8_t>(operand);
      setNZ8(r);
      setCarry((reg & 0xFFu) >= (operand & 0xFFu));
    } else {
      const std::uint16_t r = static_cast<std::uint16_t>(reg - operand);
      setNZ16(r);
      setCarry(reg >= operand);
    }
  }

  // The bitwise operators combine the operand into the accumulator at its width and
  // set N and Z from the result; an 8-bit operation leaves the accumulator's high
  // byte intact.
  void andOp(std::uint16_t operand) {
    if (accum8()) {
      const std::uint8_t r = static_cast<std::uint8_t>(state_.a) &
                             static_cast<std::uint8_t>(operand);
      state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | r);
      setNZ8(r);
    } else {
      state_.a = static_cast<std::uint16_t>(state_.a & operand);
      setNZ16(state_.a);
    }
  }
  void oraOp(std::uint16_t operand) {
    if (accum8()) {
      const std::uint8_t r = static_cast<std::uint8_t>(state_.a) |
                             static_cast<std::uint8_t>(operand);
      state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | r);
      setNZ8(r);
    } else {
      state_.a = static_cast<std::uint16_t>(state_.a | operand);
      setNZ16(state_.a);
    }
  }
  void eorOp(std::uint16_t operand) {
    if (accum8()) {
      const std::uint8_t r = static_cast<std::uint8_t>(state_.a) ^
                             static_cast<std::uint8_t>(operand);
      state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | r);
      setNZ8(r);
    } else {
      state_.a = static_cast<std::uint16_t>(state_.a ^ operand);
      setNZ16(state_.a);
    }
  }

  // BIT tests the operand against the accumulator with a bitwise AND. Z always
  // reflects that AND. In the non-immediate forms N and V take the top two bits of
  // the operand itself (not the AND); the immediate form leaves N and V alone —
  // the one instruction whose flags depend on its addressing mode.
  void bitOp(std::uint16_t operand, bool immediate) {
    if (accum8()) {
      setZ((static_cast<std::uint8_t>(state_.a) &
            static_cast<std::uint8_t>(operand)) == 0);
      if (!immediate) {
        setN((operand & 0x80u) != 0);
        setOverflow((operand & 0x40u) != 0);
      }
    } else {
      setZ((state_.a & operand) == 0);
      if (!immediate) {
        setN((operand & 0x8000u) != 0);
        setOverflow((operand & 0x4000u) != 0);
      }
    }
  }

  // ---- shifts, rotates, increments (read-modify-write cores) --------------
  // Each returns the modified value at the given width and sets the flags it owns;
  // the caller writes the result back, to the accumulator or to memory. N and Z come
  // from the result; the shifts and rotates also move the carry.
  [[nodiscard]] std::uint16_t aslOp(std::uint16_t v, bool eight) noexcept {
    if (eight) {
      setCarry((v & 0x80u) != 0);
      const std::uint8_t r = static_cast<std::uint8_t>(v << 1);
      setNZ8(r);
      return r;
    }
    setCarry((v & 0x8000u) != 0);
    const std::uint16_t r = static_cast<std::uint16_t>(v << 1);
    setNZ16(r);
    return r;
  }
  // LSR shifts a zero into the top bit, so it always clears N.
  [[nodiscard]] std::uint16_t lsrOp(std::uint16_t v, bool eight) noexcept {
    setCarry((v & 0x01u) != 0);
    if (eight) {
      const std::uint8_t r = static_cast<std::uint8_t>((v & 0xFFu) >> 1);
      setNZ8(r);
      return r;
    }
    const std::uint16_t r = static_cast<std::uint16_t>(v >> 1);
    setNZ16(r);
    return r;
  }
  // ROL and ROR rotate through the carry: the old carry fills the vacated bit and
  // the bit shifted out becomes the new carry.
  [[nodiscard]] std::uint16_t rolOp(std::uint16_t v, bool eight) noexcept {
    const std::uint16_t cin = (state_.p & kCpuFlagC) ? 1u : 0u;
    if (eight) {
      setCarry((v & 0x80u) != 0);
      const std::uint8_t r = static_cast<std::uint8_t>((v << 1) | cin);
      setNZ8(r);
      return r;
    }
    setCarry((v & 0x8000u) != 0);
    const std::uint16_t r = static_cast<std::uint16_t>((v << 1) | cin);
    setNZ16(r);
    return r;
  }
  [[nodiscard]] std::uint16_t rorOp(std::uint16_t v, bool eight) noexcept {
    const std::uint16_t cin = (state_.p & kCpuFlagC) ? 1u : 0u;
    setCarry((v & 0x01u) != 0);
    if (eight) {
      const std::uint8_t r = static_cast<std::uint8_t>(((v & 0xFFu) >> 1) | (cin << 7));
      setNZ8(r);
      return r;
    }
    const std::uint16_t r = static_cast<std::uint16_t>((v >> 1) | (cin << 15));
    setNZ16(r);
    return r;
  }
  // INC and DEC step a value by one at the given width; they move only N and Z.
  [[nodiscard]] std::uint16_t incOp(std::uint16_t v, bool eight) noexcept {
    if (eight) {
      const std::uint8_t r = static_cast<std::uint8_t>(v + 1);
      setNZ8(r);
      return r;
    }
    const std::uint16_t r = static_cast<std::uint16_t>(v + 1);
    setNZ16(r);
    return r;
  }
  [[nodiscard]] std::uint16_t decOp(std::uint16_t v, bool eight) noexcept {
    if (eight) {
      const std::uint8_t r = static_cast<std::uint8_t>(v - 1);
      setNZ8(r);
      return r;
    }
    const std::uint16_t r = static_cast<std::uint16_t>(v - 1);
    setNZ16(r);
    return r;
  }
  // TSB and TRB test the accumulator's bits against memory, set only Z from that AND,
  // and write the memory back with those bits set (TSB) or cleared (TRB).
  [[nodiscard]] std::uint16_t tsbOp(std::uint16_t v, bool eight) noexcept {
    const std::uint16_t a = eight ? static_cast<std::uint16_t>(state_.a & 0xFFu) : state_.a;
    setZ((a & v) == 0);
    return static_cast<std::uint16_t>(v | a);
  }
  [[nodiscard]] std::uint16_t trbOp(std::uint16_t v, bool eight) noexcept {
    const std::uint16_t a = eight ? static_cast<std::uint16_t>(state_.a & 0xFFu) : state_.a;
    setZ((a & v) == 0);
    return static_cast<std::uint16_t>(v & ~a);
  }

  // The accumulator as a read-modify-write target at its current width.
  [[nodiscard]] std::uint16_t accValue() const noexcept {
    return accum8() ? static_cast<std::uint16_t>(state_.a & 0xFFu) : state_.a;
  }
  void putAcc(std::uint16_t r) noexcept {
    if (accum8()) {
      state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | (r & 0xFFu));
    } else {
      state_.a = r;
    }
  }
  // INX/INY/DEX/DEY step an index register by one at the index width, setting N,Z.
  void stepIndex(std::uint16_t& reg, int delta) noexcept {
    if (index8()) {
      const std::uint8_t r = static_cast<std::uint8_t>(static_cast<int>(reg) + delta);
      reg = r;
      setNZ8(r);
    } else {
      const std::uint16_t r = static_cast<std::uint16_t>(static_cast<int>(reg) + delta);
      reg = r;
      setNZ16(r);
    }
  }

  // ---- program fetch ------------------------------------------------------
  // The address the program counter currently points at, in the program bank.
  [[nodiscard]] std::uint32_t pcAddr() const noexcept {
    return (static_cast<std::uint32_t>(state_.pbr) << 16) | state_.pc;
  }
  // Reads the byte at the program counter and advances it. The program counter is
  // 16-bit and wraps within the program bank; the bank register does not change.
  template <SnesBus B>
  std::uint8_t fetch(B& bus) {
    const std::uint8_t v = bus.read(pcAddr(), CycleKind::OperandFetch);
    state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
    return v;
  }
  template <SnesBus B>
  std::uint16_t fetchWord(B& bus) {
    const std::uint16_t lo = fetch(bus);
    const std::uint16_t hi = fetch(bus);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  template <SnesBus B>
  std::uint32_t fetchLong(B& bus) {
    const std::uint32_t lo = fetch(bus);
    const std::uint32_t mid = fetch(bus);
    const std::uint32_t hi = fetch(bus);
    return lo | (mid << 8) | (hi << 16);
  }

  // ---- effective addresses ------------------------------------------------
  // How a resolved operand's consecutive bytes wrap: flat 24-bit for data reached
  // through the data bank or a pointer; within bank zero for the direct page and
  // the stack; and within the page for the direct page while emulation mode wraps
  // it (the documented $00-direct-low case).
  enum class AddrKind : std::uint8_t { Flat, Bank0, Direct };

  struct Operand {
    std::uint32_t addr = 0;
    AddrKind kind = AddrKind::Flat;
    bool pageCross = false;  // an index addition carried past a page — one extra cycle
  };

  // Emulation mode wraps the direct page within a page when the direct register's
  // low byte is zero; otherwise direct-page arithmetic wraps within bank zero.
  [[nodiscard]] bool dpPageWrap() const noexcept {
    return state_.e && (state_.d & 0xFFu) == 0u;
  }

  // The bank-zero address of a direct-page operand at offset `ll` plus an index.
  [[nodiscard]] std::uint32_t dpAddr(std::uint8_t ll, std::uint16_t index) const noexcept {
    if (dpPageWrap()) {
      return (state_.d & 0xFF00u) | ((ll + index) & 0xFFu);
    }
    return static_cast<std::uint16_t>(state_.d + ll + index);
  }

  // The address one byte on from `addr`, wrapping per the operand's kind.
  [[nodiscard]] std::uint32_t nextByte(std::uint32_t addr, AddrKind kind) const noexcept {
    switch (kind) {
      case AddrKind::Flat:
        return (addr + 1u) & 0xFFFFFFu;
      case AddrKind::Bank0:
        return (addr + 1u) & 0xFFFFu;
      case AddrKind::Direct:
        if (dpPageWrap()) return (addr & 0xFF00u) | ((addr + 1u) & 0xFFu);
        return (addr + 1u) & 0xFFFFu;
    }
    return (addr + 1u) & 0xFFFFFFu;
  }

  // A 16-bit pointer read from the direct page. The base address already carries
  // the direct-page page-wrap (dpAddr computes it); the pointer's own two bytes are
  // consecutive and wrap only at the bank-zero boundary, never within a page. So a
  // pointer based at the last byte of a page reads its high byte from the next page
  // even in emulation mode with a page-aligned direct register — matching the 24-bit
  // and stack pointer reads below.
  template <SnesBus B>
  std::uint16_t readDpWord(B& bus, std::uint32_t base) {
    const std::uint16_t lo = bus.read(base, CycleKind::DataRead);
    const std::uint16_t hi =
        bus.read(nextByte(base, AddrKind::Bank0), CycleKind::DataRead);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  // The 24-bit indirect-long modes are new to the 65816: their pointer wraps at
  // the bank-zero boundary but never within a page, even while emulation mode
  // page-wraps the old direct-page modes — so its bytes step with the bank-zero
  // rule, not the direct-page one.
  template <SnesBus B>
  std::uint32_t readDpLong(B& bus, std::uint32_t base) {
    const std::uint32_t b0 = bus.read(base, CycleKind::DataRead);
    const std::uint32_t a1 = nextByte(base, AddrKind::Bank0);
    const std::uint32_t b1 = bus.read(a1, CycleKind::DataRead);
    const std::uint32_t b2 =
        bus.read(nextByte(a1, AddrKind::Bank0), CycleKind::DataRead);
    return b0 | (b1 << 8) | (b2 << 16);
  }
  template <SnesBus B>
  std::uint16_t readStackWord(B& bus, std::uint32_t base) {
    const std::uint16_t lo = bus.read(base, CycleKind::DataRead);
    const std::uint16_t hi =
        bus.read(nextByte(base, AddrKind::Bank0), CycleKind::DataRead);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }

  // Data-bank-relative addresses, formed from the operand and the data bank.
  [[nodiscard]] std::uint32_t bankAddr(std::uint16_t offset) const noexcept {
    return (static_cast<std::uint32_t>(state_.dbr) << 16) | offset;
  }
  // Whether adding an 8-bit index to a 16-bit base crosses a page (carries out of
  // the low byte) — the only case that costs a cycle, and only with 8-bit indexes.
  [[nodiscard]] static bool crossesPage(std::uint16_t base, std::uint16_t index) noexcept {
    return ((base & 0xFFu) + (index & 0xFFu)) > 0xFFu;
  }

  template <SnesBus B>
  Operand eaAbs(B& bus) {
    return {bankAddr(fetchWord(bus)), AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaAbsX(B& bus) {
    const std::uint16_t base = fetchWord(bus);
    const std::uint16_t index = idxX();
    return {(bankAddr(base) + index) & 0xFFFFFFu, AddrKind::Flat,
            crossesPage(base, index)};
  }
  template <SnesBus B>
  Operand eaAbsY(B& bus) {
    const std::uint16_t base = fetchWord(bus);
    const std::uint16_t index = idxY();
    return {(bankAddr(base) + index) & 0xFFFFFFu, AddrKind::Flat,
            crossesPage(base, index)};
  }
  template <SnesBus B>
  Operand eaLong(B& bus) {
    return {fetchLong(bus), AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaLongX(B& bus) {
    return {(fetchLong(bus) + idxX()) & 0xFFFFFFu, AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaStack(B& bus) {
    return {static_cast<std::uint16_t>(state_.s + fetch(bus)), AddrKind::Bank0, false};
  }
  template <SnesBus B>
  Operand eaDirInd(B& bus) {
    const std::uint16_t ptr = readDpWord(bus, dpAddr(fetch(bus), 0));
    return {bankAddr(ptr), AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaDirIndY(B& bus) {
    const std::uint16_t ptr = readDpWord(bus, dpAddr(fetch(bus), 0));
    const std::uint16_t index = idxY();
    return {(bankAddr(ptr) + index) & 0xFFFFFFu, AddrKind::Flat,
            crossesPage(ptr, index)};
  }
  template <SnesBus B>
  Operand eaDirIndX(B& bus) {
    const std::uint16_t ptr = readDpWord(bus, dpAddr(fetch(bus), idxX()));
    return {bankAddr(ptr), AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaDirIndLong(B& bus) {
    return {readDpLong(bus, dpAddr(fetch(bus), 0)), AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaDirIndLongY(B& bus) {
    return {(readDpLong(bus, dpAddr(fetch(bus), 0)) + idxY()) & 0xFFFFFFu,
            AddrKind::Flat, false};
  }
  template <SnesBus B>
  Operand eaStackIndY(B& bus) {
    const std::uint16_t ptr =
        readStackWord(bus, static_cast<std::uint16_t>(state_.s + fetch(bus)));
    return {(bankAddr(ptr) + idxY()) & 0xFFFFFFu, AddrKind::Flat, false};
  }

  // ---- data access at an effective address --------------------------------
  template <SnesBus B>
  std::uint16_t readValue(B& bus, const Operand& op, bool eightBit,
                          CycleKind kind = CycleKind::DataRead) {
    const std::uint16_t lo = bus.read(op.addr, kind);
    if (eightBit) return lo;
    const std::uint16_t hi = bus.read(nextByte(op.addr, op.kind), kind);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  template <SnesBus B>
  void writeValue(B& bus, const Operand& op, std::uint16_t v, bool eightBit,
                  CycleKind kind = CycleKind::DataWrite) {
    bus.write(op.addr, static_cast<std::uint8_t>(v), kind);
    if (!eightBit) {
      bus.write(nextByte(op.addr, op.kind), static_cast<std::uint8_t>(v >> 8), kind);
    }
  }

  // ---- stack --------------------------------------------------------------
  // Pushes write to the current stack pointer and then decrement it; pulls
  // pre-increment and then read. The stack lives in bank zero; emulation mode keeps
  // it inside page one, so the pointer wraps within $01xx there and across bank zero
  // otherwise. A 16-bit push stores the high byte first (at the higher address),
  // matching the little-endian order a pull reads back low byte first.
  template <SnesBus B>
  void push8(B& bus, std::uint8_t v) {
    bus.write(state_.s, v, CycleKind::DataWrite);
    state_.s = state_.e ? static_cast<std::uint16_t>(0x0100u | ((state_.s - 1) & 0xFFu))
                        : static_cast<std::uint16_t>(state_.s - 1);
  }
  template <SnesBus B>
  void push16(B& bus, std::uint16_t v) {
    push8(bus, static_cast<std::uint8_t>(v >> 8));
    push8(bus, static_cast<std::uint8_t>(v));
  }
  template <SnesBus B>
  std::uint8_t pull8(B& bus) {
    state_.s = state_.e ? static_cast<std::uint16_t>(0x0100u | ((state_.s + 1) & 0xFFu))
                        : static_cast<std::uint16_t>(state_.s + 1);
    return bus.read(state_.s, CycleKind::DataRead);
  }
  template <SnesBus B>
  std::uint16_t pull16(B& bus) {
    const std::uint16_t lo = pull8(bus);
    const std::uint16_t hi = pull8(bus);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }

  // The 65816-new stack instructions (PEA/PEI/PER, PHB/PHK/PHD, PLB/PLD) let the
  // stack pointer leave page one during the access in emulation mode, then force its
  // high byte back to $01 afterward — unlike the 6502-original push/pull above, which
  // wrap the pointer within page one on every access. In native mode the two behave
  // identically. Each such instruction transfers once, then calls settleStack.
  template <SnesBus B>
  void pushWide8(B& bus, std::uint8_t v) {
    bus.write(state_.s, v, CycleKind::DataWrite);
    state_.s = static_cast<std::uint16_t>(state_.s - 1);
  }
  template <SnesBus B>
  void pushWide16(B& bus, std::uint16_t v) {
    pushWide8(bus, static_cast<std::uint8_t>(v >> 8));
    pushWide8(bus, static_cast<std::uint8_t>(v));
  }
  template <SnesBus B>
  std::uint8_t pullWide8(B& bus) {
    state_.s = static_cast<std::uint16_t>(state_.s + 1);
    return bus.read(state_.s, CycleKind::DataRead);
  }
  template <SnesBus B>
  std::uint16_t pullWide16(B& bus) {
    const std::uint16_t lo = pullWide8(bus);
    const std::uint16_t hi = pullWide8(bus);
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  void settleStack() noexcept {
    if (state_.e) state_.s = static_cast<std::uint16_t>(0x0100u | (state_.s & 0xFFu));
  }

  // Reads an operand, applies a read-modify-write operator at the accumulator width,
  // and writes the result back to the same address.
  template <SnesBus B>
  void rmwMem(B& bus, const Operand& op,
              std::uint16_t (Cpu65816::*fn)(std::uint16_t, bool)) {
    const std::uint16_t v = readValue(bus, op, accum8(), CycleKind::RmwRead);
    writeValue(bus, op, (this->*fn)(v, accum8()), accum8(), CycleKind::RmwWrite);
  }

  // ---- the cycle engine ---------------------------------------------------
  // Executes one cycle of the instruction already in the instruction register, and
  // answers whether that cycle was its last. The cycle index says which cycle is
  // due; every branch a cycle can take is decided by state the earlier cycles have
  // already latched, so a handler never has to look ahead.
  template <SnesBus B>
  bool executeCycle(B& bus);

  // ---- the direct-page instructions ---------------------------------------
  // How a direct-page instruction reaches memory: it reads its operand, writes one,
  // or reads and writes the same address with the memory lock held between.
  enum class DpAccess : std::uint8_t { Read, Write, Modify };
  // Which index register is added to the direct-page offset, if any.
  enum class DpIndex : std::uint8_t { None, X, Y };

  // The shape of one direct-page instruction: how it reaches memory, how its
  // address is indexed, and which width sizes its operand.
  struct DpForm {
    DpAccess access = DpAccess::Read;
    DpIndex index = DpIndex::None;
    bool indexWidth = false;  // sized by the index registers rather than by A
  };

  // Fills in the form of a direct-page instruction and answers whether the opcode is
  // one. The three direct-page addressing modes share a single cycle sequence, so
  // the opcode's only per-instruction contributions are this shape and the operator
  // applied once the operand has arrived.
  [[nodiscard]] static constexpr bool directPageForm(std::uint8_t opcode,
                                                     DpForm& form) noexcept;

  // The address the direct-page offset was fetched from — where the chip parks for
  // the direct-register and indexing cycles. The program counter has stepped past
  // the offset by then and does not move again within the instruction.
  [[nodiscard]] std::uint32_t operandAddr() const noexcept {
    return (static_cast<std::uint32_t>(state_.pbr) << 16) |
           static_cast<std::uint16_t>(state_.pc - 1);
  }

  // The cycle index at which a direct-page instruction first reaches memory: past
  // the opcode fetch and the offset fetch, plus a cycle when the direct register
  // holds a low byte and another when the address is indexed.
  [[nodiscard]] std::uint8_t dpDataCycle(const DpForm& form) const noexcept {
    return static_cast<std::uint8_t>(2u + dpCyc() +
                                     (form.index == DpIndex::None ? 0u : 1u));
  }

  // The index added to the direct-page offset for this form.
  [[nodiscard]] std::uint16_t dpIndexValue(const DpForm& form) const noexcept {
    switch (form.index) {
      case DpIndex::X: return idxX();
      case DpIndex::Y: return idxY();
      case DpIndex::None: break;
    }
    return 0;
  }

  // Executes one cycle of a direct-page instruction.
  template <SnesBus B>
  bool executeDirectPageCycle(B& bus, const DpForm& form);

  // Applies a direct-page instruction that has finished reading its operand.
  void applyDirectPageRead(std::uint8_t opcode, std::uint16_t value);

  // The value a direct-page store writes.
  [[nodiscard]] std::uint16_t directPageStoreValue(std::uint8_t opcode) const noexcept;

  // The operator a direct-page read-modify-write applies to the value it read.
  [[nodiscard]] std::uint16_t directPageModify(std::uint8_t opcode, std::uint16_t value,
                                               bool eightBit);

  // The immediate operand's width: the accumulator instructions take an operand as
  // wide as the accumulator, the index instructions one as wide as the index
  // registers.
  [[nodiscard]] bool immediateIsEightBit(std::uint8_t opcode) const noexcept {
    switch (opcode) {
      case 0xA2:  // LDX #imm
      case 0xA0:  // LDY #imm
      case 0xE0:  // CPX #imm
      case 0xC0:  // CPY #imm
        return index8();
      default:
        return accum8();
    }
  }

  // Applies an instruction whose immediate operand has finished arriving.
  void applyImmediate(std::uint8_t opcode, std::uint16_t operand);

  // Applies an instruction that reads nothing and writes nothing — it works on the
  // registers alone, and the internal cycle it spends has already been narrated.
  void applyImplied(std::uint8_t opcode);

  // Executes an instruction the cycle engine does not carry, all at once: the
  // accesses land in order but within a single call, starting from the cycle after
  // the opcode fetch. Returns the instruction's full documented cycle total.
  template <SnesBus B>
  std::uint32_t stepWhole(B& bus, std::uint8_t opcode);

  Cpu65816State state_{};
  // The NMI pin's last observed level, kept so a high-to-low transition can be
  // told from a line that is merely still asserted. It describes the machine
  // outside the chip rather than the chip, so it is not part of the CPU state.
  bool nmiLine_ = false;
};

constexpr bool Cpu65816::cycleStepped(std::uint8_t opcode) noexcept {
  switch (opcode) {
    // Implied and accumulator instructions: two cycles, the second internal.
    case 0x0A: case 0x18: case 0x1A: case 0x1B: case 0x2A: case 0x38: case 0x3A:
    case 0x3B: case 0x4A: case 0x58: case 0x5B: case 0x6A: case 0x78: case 0x7B:
    case 0x88: case 0x8A: case 0x98: case 0x9A: case 0x9B: case 0xA8: case 0xAA:
    case 0xB8: case 0xBA: case 0xBB: case 0xC8: case 0xCA: case 0xD8: case 0xE8:
    case 0xEA: case 0xF8: case 0xFB:
    // The accumulator byte exchange, which spends two internal cycles.
    case 0xEB:
    // The immediate operands, one or two bytes wide.
    case 0x09: case 0x29: case 0x49: case 0x69: case 0x89: case 0xA0: case 0xA2:
    case 0xA9: case 0xC0: case 0xC9: case 0xE0: case 0xE9:
    // The status-mask instructions and the reserved two-byte no-op.
    case 0x42: case 0xC2: case 0xE2:
      return true;
    default:
      break;
  }
  DpForm form{};
  return directPageForm(opcode, form);
}

constexpr bool Cpu65816::directPageForm(std::uint8_t opcode, DpForm& form) noexcept {
  switch (opcode) {
    // ---- read the operand at the accumulator width ----
    case 0x05: case 0x24: case 0x25: case 0x45: case 0x65: case 0xA5:
    case 0xC5: case 0xE5:
      form = {.access = DpAccess::Read, .index = DpIndex::None, .indexWidth = false};
      return true;
    case 0x15: case 0x34: case 0x35: case 0x55: case 0x75: case 0xB5:
    case 0xD5: case 0xF5:
      form = {.access = DpAccess::Read, .index = DpIndex::X, .indexWidth = false};
      return true;

    // ---- read the operand at the index width ----
    case 0xA4: case 0xA6: case 0xC4: case 0xE4:
      form = {.access = DpAccess::Read, .index = DpIndex::None, .indexWidth = true};
      return true;
    case 0xB4:
      form = {.access = DpAccess::Read, .index = DpIndex::X, .indexWidth = true};
      return true;
    case 0xB6:
      form = {.access = DpAccess::Read, .index = DpIndex::Y, .indexWidth = true};
      return true;

    // ---- store a register (STZ stores zero at the accumulator width) ----
    case 0x64: case 0x85:
      form = {.access = DpAccess::Write, .index = DpIndex::None, .indexWidth = false};
      return true;
    case 0x74: case 0x95:
      form = {.access = DpAccess::Write, .index = DpIndex::X, .indexWidth = false};
      return true;
    case 0x84: case 0x86:
      form = {.access = DpAccess::Write, .index = DpIndex::None, .indexWidth = true};
      return true;
    case 0x94:
      form = {.access = DpAccess::Write, .index = DpIndex::X, .indexWidth = true};
      return true;
    case 0x96:
      form = {.access = DpAccess::Write, .index = DpIndex::Y, .indexWidth = true};
      return true;

    // ---- read, modify and write the same address ----
    case 0x04: case 0x06: case 0x14: case 0x26: case 0x46: case 0x66:
    case 0xC6: case 0xE6:
      form = {.access = DpAccess::Modify, .index = DpIndex::None, .indexWidth = false};
      return true;
    case 0x16: case 0x36: case 0x56: case 0x76: case 0xD6: case 0xF6:
      form = {.access = DpAccess::Modify, .index = DpIndex::X, .indexWidth = false};
      return true;

    default:
      return false;
  }
}

inline void Cpu65816::applyImmediate(std::uint8_t opcode, std::uint16_t operand) {
  switch (opcode) {
    case 0xA9: loadA(operand); break;                              // LDA #imm
    case 0xA2: loadX(operand); break;                              // LDX #imm
    case 0xA0: loadY(operand); break;                              // LDY #imm
    case 0x69: adcOp(operand); break;                              // ADC #imm
    case 0xE9: sbcOp(operand); break;                              // SBC #imm
    case 0xC9: compareWith(state_.a, operand, accum8()); break;    // CMP #imm
    case 0xE0: compareWith(state_.x, operand, index8()); break;    // CPX #imm
    case 0xC0: compareWith(state_.y, operand, index8()); break;    // CPY #imm
    case 0x29: andOp(operand); break;                              // AND #imm
    case 0x49: eorOp(operand); break;                              // EOR #imm
    case 0x09: oraOp(operand); break;                              // ORA #imm
    case 0x89: bitOp(operand, /*immediate=*/true); break;          // BIT #imm
    default: break;
  }
}

inline void Cpu65816::applyImplied(std::uint8_t opcode) {
  switch (opcode) {
    // ---- register transfers. The destination's width decides how many bits move
    //      and sets N,Z — except the transfers into the stack pointer, which set no
    //      flags ----
    case 0xAA:  // TAX
      if (index8()) { state_.x = static_cast<std::uint16_t>(state_.a & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.a)); }
      else { state_.x = state_.a; setNZ16(state_.a); }
      break;
    case 0xA8:  // TAY
      if (index8()) { state_.y = static_cast<std::uint16_t>(state_.a & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.a)); }
      else { state_.y = state_.a; setNZ16(state_.a); }
      break;
    case 0xBA:  // TSX
      if (index8()) { state_.x = static_cast<std::uint16_t>(state_.s & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.s)); }
      else { state_.x = state_.s; setNZ16(state_.s); }
      break;
    case 0x8A:  // TXA
      if (accum8()) { state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | (state_.x & 0xFFu)); setNZ8(static_cast<std::uint8_t>(state_.x)); }
      else { state_.a = state_.x; setNZ16(state_.x); }
      break;
    case 0x98:  // TYA
      if (accum8()) { state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | (state_.y & 0xFFu)); setNZ8(static_cast<std::uint8_t>(state_.y)); }
      else { state_.a = state_.y; setNZ16(state_.y); }
      break;
    case 0x9B:  // TXY
      if (index8()) { state_.y = static_cast<std::uint16_t>(state_.x & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.x)); }
      else { state_.y = state_.x; setNZ16(state_.x); }
      break;
    case 0xBB:  // TYX
      if (index8()) { state_.x = static_cast<std::uint16_t>(state_.y & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.y)); }
      else { state_.x = state_.y; setNZ16(state_.y); }
      break;
    case 0x9A:  // TXS (SH is $01 in emulation, so only XL moves there)
      state_.s = state_.e ? static_cast<std::uint16_t>(0x0100u | (state_.x & 0xFFu)) : state_.x;
      break;

    // ---- 16-bit transfers to and from the direct and stack registers. These move
    //      the whole 16-bit accumulator regardless of the m width and set N,Z on the
    //      16-bit result — except TCS, which sets no flags ----
    case 0x5B: state_.d = state_.a; setNZ16(state_.d); break;  // TCD
    case 0x7B: state_.a = state_.d; setNZ16(state_.a); break;  // TDC
    case 0x3B: state_.a = state_.s; setNZ16(state_.a); break;  // TSC
    case 0x1B:  // TCS (SH is $01 in emulation, so only the low byte moves)
      state_.s = state_.e ? static_cast<std::uint16_t>(0x0100u | (state_.a & 0xFFu)) : state_.a;
      break;

    // ---- step a value by one (accumulator or index width; N,Z) ----
    case 0x1A: putAcc(incOp(accValue(), accum8())); break;  // INC A
    case 0x3A: putAcc(decOp(accValue(), accum8())); break;  // DEC A
    case 0xE8: stepIndex(state_.x, +1); break;              // INX
    case 0xC8: stepIndex(state_.y, +1); break;              // INY
    case 0xCA: stepIndex(state_.x, -1); break;              // DEX
    case 0x88: stepIndex(state_.y, -1); break;              // DEY

    // ---- shifts and rotates on the accumulator (accumulator width; N,Z,C) ----
    case 0x0A: putAcc(aslOp(accValue(), accum8())); break;  // ASL A
    case 0x4A: putAcc(lsrOp(accValue(), accum8())); break;  // LSR A
    case 0x2A: putAcc(rolOp(accValue(), accum8())); break;  // ROL A
    case 0x6A: putAcc(rorOp(accValue(), accum8())); break;  // ROR A

    // ---- status-flag set / clear ----
    case 0x18: state_.p = static_cast<std::uint8_t>(state_.p & ~kCpuFlagC); break;  // CLC
    case 0x38: state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagC); break;   // SEC
    case 0x58: state_.p = static_cast<std::uint8_t>(state_.p & ~kCpuFlagI); break;  // CLI
    case 0x78: state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagI); break;   // SEI
    case 0xD8: state_.p = static_cast<std::uint8_t>(state_.p & ~kCpuFlagD); break;  // CLD
    case 0xF8: state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagD); break;   // SED
    case 0xB8: state_.p = static_cast<std::uint8_t>(state_.p & ~kCpuFlagV); break;  // CLV

    // ---- XBA: exchange the accumulator halves (N,Z on the new low byte, always
    //      8-bit) ----
    case 0xEB:
      state_.a = static_cast<std::uint16_t>(((state_.a & 0xFFu) << 8) | ((state_.a >> 8) & 0xFFu));
      setNZ8(static_cast<std::uint8_t>(state_.a));
      break;

    // ---- XCE: exchange carry and emulation. Entering emulation forces the 8-bit
    //      widths, the index high bytes to zero, and the stack to page one ----
    case 0xFB: {
      const bool oldCarry = (state_.p & kCpuFlagC) != 0;
      setCarry(state_.e);
      state_.e = oldCarry;
      if (state_.e) {
        state_.p = static_cast<std::uint8_t>(state_.p | kCpuFlagM | kCpuFlagX);
        state_.x &= 0xFFu;
        state_.y &= 0xFFu;
        state_.s = static_cast<std::uint16_t>(0x0100u | (state_.s & 0xFFu));
      }
      break;
    }

    // ---- no operation ----
    case 0xEA: break;  // NOP
    default: break;
  }
}

inline void Cpu65816::applyDirectPageRead(std::uint8_t opcode, std::uint16_t value) {
  switch (opcode) {
    case 0xA5: case 0xB5: loadA(value); break;                       // LDA
    case 0xA6: case 0xB6: loadX(value); break;                       // LDX
    case 0xA4: case 0xB4: loadY(value); break;                       // LDY
    case 0x65: case 0x75: adcOp(value); break;                       // ADC
    case 0xE5: case 0xF5: sbcOp(value); break;                       // SBC
    case 0x25: case 0x35: andOp(value); break;                       // AND
    case 0x45: case 0x55: eorOp(value); break;                       // EOR
    case 0x05: case 0x15: oraOp(value); break;                       // ORA
    case 0x24: case 0x34: bitOp(value, /*immediate=*/false); break;  // BIT
    case 0xC5: case 0xD5: compareWith(state_.a, value, accum8()); break;  // CMP
    case 0xE4: compareWith(state_.x, value, index8()); break;             // CPX
    case 0xC4: compareWith(state_.y, value, index8()); break;             // CPY
    default: break;
  }
}

inline std::uint16_t Cpu65816::directPageStoreValue(std::uint8_t opcode) const noexcept {
  switch (opcode) {
    case 0x85: case 0x95: return state_.a;  // STA
    case 0x86: case 0x96: return state_.x;  // STX
    case 0x84: case 0x94: return state_.y;  // STY
    default: return 0;                      // STZ stores zero
  }
}

inline std::uint16_t Cpu65816::directPageModify(std::uint8_t opcode, std::uint16_t value,
                                                bool eightBit) {
  switch (opcode) {
    case 0x06: case 0x16: return aslOp(value, eightBit);  // ASL
    case 0x46: case 0x56: return lsrOp(value, eightBit);  // LSR
    case 0x26: case 0x36: return rolOp(value, eightBit);  // ROL
    case 0x66: case 0x76: return rorOp(value, eightBit);  // ROR
    case 0xE6: case 0xF6: return incOp(value, eightBit);  // INC
    case 0xC6: case 0xD6: return decOp(value, eightBit);  // DEC
    case 0x04: return tsbOp(value, eightBit);             // TSB
    default: return trbOp(value, eightBit);               // TRB
  }
}

// A direct-page instruction fetches its offset, spends a cycle for a direct register
// with a low byte and another for an index register, then reaches memory. The offset
// and the index are added within bank zero — except in emulation mode with a
// page-aligned direct register, where the sum stays inside the direct page.
template <SnesBus B>
bool Cpu65816::executeDirectPageCycle(B& bus, const DpForm& form) {
  const std::uint8_t opcode = state_.ir;
  const bool eightBit = form.indexWidth ? index8() : accum8();
  const std::uint8_t dataCycle = dpDataCycle(form);

  if (state_.tcu == 1) {
    state_.tmp = fetch(bus);  // the direct-page offset
    return false;
  }
  if (state_.tcu < dataCycle) {
    // The direct-register and indexing cycles: the address the offset came from is
    // still driven while the chip does the addition.
    bus.internal(operandAddr());
    return false;
  }

  const std::uint8_t step = static_cast<std::uint8_t>(state_.tcu - dataCycle);
  if (step == 0) {
    state_.ea = dpAddr(static_cast<std::uint8_t>(state_.tmp), dpIndexValue(form));
  }
  const std::uint32_t high = nextByte(state_.ea, AddrKind::Direct);

  switch (form.access) {
    case DpAccess::Read:
      if (step == 0) {
        const std::uint16_t lo = bus.read(state_.ea, CycleKind::DataRead);
        if (!eightBit) {
          state_.tmp = lo;
          return false;
        }
        applyDirectPageRead(opcode, lo);
        return true;
      }
      applyDirectPageRead(opcode, static_cast<std::uint16_t>(
                                      state_.tmp | (bus.read(high, CycleKind::DataRead) << 8)));
      return true;

    case DpAccess::Write: {
      const std::uint16_t value = directPageStoreValue(opcode);
      if (step == 0) {
        bus.write(state_.ea, static_cast<std::uint8_t>(value), CycleKind::DataWrite);
        return eightBit;
      }
      bus.write(high, static_cast<std::uint8_t>(value >> 8), CycleKind::DataWrite);
      return true;
    }

    case DpAccess::Modify:
      // Eight bits wide: read, modify, write. Sixteen: both bytes are read, then
      // written back high byte first — the reverse of the order they were read in.
      if (eightBit) {
        if (step == 0) {
          state_.tmp = bus.read(state_.ea, CycleKind::RmwRead);
          return false;
        }
        if (step == 1) {
          if (state_.e) {
            bus.write(state_.ea, static_cast<std::uint8_t>(state_.tmp),
                      CycleKind::RmwModifyWrite);
          } else {
            bus.internal(state_.ea, CycleKind::RmwModify);
          }
          state_.tmp = directPageModify(opcode, state_.tmp, true);
          return false;
        }
        bus.write(state_.ea, static_cast<std::uint8_t>(state_.tmp), CycleKind::RmwWrite);
        return true;
      }
      switch (step) {
        case 0:
          state_.tmp = bus.read(state_.ea, CycleKind::RmwRead);
          return false;
        case 1:
          state_.tmp = static_cast<std::uint16_t>(
              state_.tmp | (bus.read(high, CycleKind::RmwRead) << 8));
          return false;
        case 2:
          bus.internal(high, CycleKind::RmwModify);
          state_.tmp = directPageModify(opcode, state_.tmp, false);
          return false;
        case 3:
          bus.write(high, static_cast<std::uint8_t>(state_.tmp >> 8), CycleKind::RmwWrite);
          return false;
        default:
          bus.write(state_.ea, static_cast<std::uint8_t>(state_.tmp), CycleKind::RmwWrite);
          return true;
      }
  }
  return true;
}

template <SnesBus B>
bool Cpu65816::executeCycle(B& bus) {
  const std::uint8_t opcode = state_.ir;
  if (DpForm form{}; directPageForm(opcode, form)) {
    return executeDirectPageCycle(bus, form);
  }
  switch (opcode) {
    // ---- REP / SEP: fetch the mask, then spend an internal cycle parked on the
    //      mask's own address. The status byte settles at the end of that cycle, so
    //      the widths the cycle itself runs under are still the old ones ----
    case 0xC2:  // REP #imm
    case 0xE2:  // SEP #imm
      if (state_.tcu == 1) {
        state_.ea = pcAddr();
        state_.tmp = fetch(bus);
        return false;
      }
      bus.internal(state_.ea);
      writeP(opcode == 0xC2
                 ? static_cast<std::uint8_t>(state_.p & ~static_cast<std::uint8_t>(state_.tmp))
                 : static_cast<std::uint8_t>(state_.p | static_cast<std::uint8_t>(state_.tmp)));
      return true;

    // ---- WDM: a reserved two-byte no-op. It steps the program counter over its
    //      second byte without reading it — the cycle drives the address and takes
    //      nothing from the bus ----
    case 0x42:
      bus.internal(pcAddr());
      state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
      return true;

    // ---- XBA: two internal cycles, both parked at the program counter ----
    case 0xEB:
      bus.internal(pcAddr());
      if (state_.tcu == 1) return false;
      applyImplied(opcode);
      return true;

    // ---- an immediate operand: one byte, or two when the instruction's register
    //      is 16-bit. The width is settled before the instruction begins, so the
    //      first operand cycle already knows whether a second one follows ----
    case 0x09: case 0x29: case 0x49: case 0x69: case 0x89:
    case 0xA0: case 0xA2: case 0xA9: case 0xC0: case 0xC9:
    case 0xE0: case 0xE9:
      if (state_.tcu == 1) {
        state_.tmp = fetch(bus);
        if (!immediateIsEightBit(opcode)) return false;
        applyImmediate(opcode, state_.tmp);
        return true;
      }
      state_.tmp = static_cast<std::uint16_t>(state_.tmp | (fetch(bus) << 8));
      applyImmediate(opcode, state_.tmp);
      return true;

    // ---- an implied or accumulator instruction: one internal cycle parked at the
    //      program counter, and the register work settles at the end of it ----
    default:
      bus.internal(pcAddr());
      applyImplied(opcode);
      return true;
  }
}

template <SnesBus B>
void Cpu65816::stepCycle(B& bus) {
  // A waiting or stopped core drives no address and touches no memory; the cycle
  // still passes, and its host prices it from the run state.
  if (state_.run != CpuRunState::Running) return;

  if (atInstructionBoundary()) {
    // The mode invariants settle on the fetch cycle, so an instruction always runs
    // under a consistent view of the widths.
    normalize();
    state_.ir = bus.read(pcAddr(), CycleKind::OpcodeFetch);
    state_.pc = static_cast<std::uint16_t>(state_.pc + 1);
    state_.tcu = 1;
    return;
  }

  if (!cycleStepped(state_.ir)) {
    // The instruction in progress runs whole rather than a cycle at a time, so
    // there is no single cycle to run here. The core returns to a boundary without
    // touching the bus: the cycle that was asked for plainly did not happen, rather
    // than being filled in with invented traffic. Use stepInstruction instead.
    state_.tcu = 0;
    return;
  }

  state_.tcu = executeCycle(bus) ? std::uint8_t{0}
                                 : static_cast<std::uint8_t>(state_.tcu + 1);
}

template <SnesBus B>
std::uint32_t Cpu65816::stepInstruction(B& bus) {
  if (state_.run != CpuRunState::Running) return 1;

  std::uint32_t cycles = 0;
  if (atInstructionBoundary()) {
    stepCycle(bus);  // the opcode fetch
    cycles = 1;
    if (!cycleStepped(state_.ir)) {
      state_.tcu = 0;
      return stepWhole(bus, state_.ir);
    }
  }
  while (!atInstructionBoundary()) {
    stepCycle(bus);
    ++cycles;
  }
  return cycles;
}

template <SnesBus B>
std::uint32_t Cpu65816::stepWhole(B& bus, std::uint8_t opcode) {
  switch (opcode) {
    // ---- LDA: load the accumulator (N,Z at the accumulator width) ----
    case 0xAD: { Operand o = eaAbs(bus);        loadA(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // LDA abs
    case 0xBD: { Operand o = eaAbsX(bus);       loadA(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDA abs,X
    case 0xB9: { Operand o = eaAbsY(bus);       loadA(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDA abs,Y
    case 0xAF: { Operand o = eaLong(bus);       loadA(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // LDA long
    case 0xBF: { Operand o = eaLongX(bus);      loadA(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // LDA long,X
    case 0xA1: { Operand o = eaDirIndX(bus);    loadA(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // LDA (dir,X)
    case 0xB1: { Operand o = eaDirIndY(bus);    loadA(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDA (dir),Y
    case 0xB2: { Operand o = eaDirInd(bus);     loadA(readValue(bus, o, accum8())); return 6u - accCyc() + dpCyc(); }        // LDA (dir)
    case 0xA7: { Operand o = eaDirIndLong(bus); loadA(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // LDA [dir]
    case 0xB7: { Operand o = eaDirIndLongY(bus);loadA(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // LDA [dir],Y
    case 0xA3: { Operand o = eaStack(bus);      loadA(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // LDA stk,S
    case 0xB3: { Operand o = eaStackIndY(bus);  loadA(readValue(bus, o, accum8())); return 8u - accCyc(); }                  // LDA (stk,S),Y

    // ---- LDX / LDY: load an index register (N,Z at the index width) ----
    case 0xAE: { Operand o = eaAbs(bus);   loadX(readValue(bus, o, index8())); return 5u - indexCyc(); }            // LDX abs
    case 0xBE: { Operand o = eaAbsY(bus);  loadX(readValue(bus, o, index8())); return 6u - 2u * indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDX abs,Y
    case 0xAC: { Operand o = eaAbs(bus);   loadY(readValue(bus, o, index8())); return 5u - indexCyc(); }            // LDY abs
    case 0xBC: { Operand o = eaAbsX(bus);  loadY(readValue(bus, o, index8())); return 6u - 2u * indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDY abs,X

    // ---- STA: store the accumulator (no flags; indexed stores take no page-cross
    //      discount — the extra cycle is always paid) ----
    case 0x8D: { Operand o = eaAbs(bus);        writeValue(bus, o, state_.a, accum8()); return 5u - accCyc(); }            // STA abs
    case 0x9D: { Operand o = eaAbsX(bus);       writeValue(bus, o, state_.a, accum8()); return 6u - accCyc(); }            // STA abs,X
    case 0x99: { Operand o = eaAbsY(bus);       writeValue(bus, o, state_.a, accum8()); return 6u - accCyc(); }            // STA abs,Y
    case 0x8F: { Operand o = eaLong(bus);       writeValue(bus, o, state_.a, accum8()); return 6u - accCyc(); }            // STA long
    case 0x9F: { Operand o = eaLongX(bus);      writeValue(bus, o, state_.a, accum8()); return 6u - accCyc(); }            // STA long,X
    case 0x81: { Operand o = eaDirIndX(bus);    writeValue(bus, o, state_.a, accum8()); return 7u - accCyc() + dpCyc(); }  // STA (dir,X)
    case 0x91: { Operand o = eaDirIndY(bus);    writeValue(bus, o, state_.a, accum8()); return 7u - accCyc() + dpCyc(); }  // STA (dir),Y
    case 0x92: { Operand o = eaDirInd(bus);     writeValue(bus, o, state_.a, accum8()); return 6u - accCyc() + dpCyc(); }  // STA (dir)
    case 0x87: { Operand o = eaDirIndLong(bus); writeValue(bus, o, state_.a, accum8()); return 7u - accCyc() + dpCyc(); }  // STA [dir]
    case 0x97: { Operand o = eaDirIndLongY(bus);writeValue(bus, o, state_.a, accum8()); return 7u - accCyc() + dpCyc(); }  // STA [dir],Y
    case 0x83: { Operand o = eaStack(bus);      writeValue(bus, o, state_.a, accum8()); return 5u - accCyc(); }            // STA stk,S
    case 0x93: { Operand o = eaStackIndY(bus);  writeValue(bus, o, state_.a, accum8()); return 8u - accCyc(); }            // STA (stk,S),Y

    // ---- STX / STY: store an index register (no flags) ----
    case 0x8E: { Operand o = eaAbs(bus);  writeValue(bus, o, state_.x, index8()); return 5u - indexCyc(); }            // STX abs
    case 0x8C: { Operand o = eaAbs(bus);  writeValue(bus, o, state_.y, index8()); return 5u - indexCyc(); }            // STY abs

    // ---- STZ: store zero (accumulator width; no flags) ----
    case 0x9C: { Operand o = eaAbs(bus);  writeValue(bus, o, 0, accum8()); return 5u - accCyc(); }            // STZ abs
    case 0x9E: { Operand o = eaAbsX(bus); writeValue(bus, o, 0, accum8()); return 6u - accCyc(); }            // STZ abs,X

    // ---- ADC: add with carry (accumulator width; N,V,Z,C) ----
    case 0x6D: { Operand o = eaAbs(bus);        adcOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // ADC abs
    case 0x7D: { Operand o = eaAbsX(bus);       adcOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // ADC abs,X
    case 0x79: { Operand o = eaAbsY(bus);       adcOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // ADC abs,Y
    case 0x6F: { Operand o = eaLong(bus);       adcOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // ADC long
    case 0x7F: { Operand o = eaLongX(bus);      adcOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // ADC long,X
    case 0x61: { Operand o = eaDirIndX(bus);    adcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // ADC (dir,X)
    case 0x71: { Operand o = eaDirIndY(bus);    adcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // ADC (dir),Y
    case 0x72: { Operand o = eaDirInd(bus);     adcOp(readValue(bus, o, accum8())); return 6u - accCyc() + dpCyc(); }        // ADC (dir)
    case 0x67: { Operand o = eaDirIndLong(bus); adcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // ADC [dir]
    case 0x77: { Operand o = eaDirIndLongY(bus);adcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // ADC [dir],Y
    case 0x63: { Operand o = eaStack(bus);      adcOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // ADC stk,S
    case 0x73: { Operand o = eaStackIndY(bus);  adcOp(readValue(bus, o, accum8())); return 8u - accCyc(); }                  // ADC (stk,S),Y

    // ---- SBC: subtract with carry (accumulator width; N,V,Z,C) ----
    case 0xED: { Operand o = eaAbs(bus);        sbcOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // SBC abs
    case 0xFD: { Operand o = eaAbsX(bus);       sbcOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // SBC abs,X
    case 0xF9: { Operand o = eaAbsY(bus);       sbcOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // SBC abs,Y
    case 0xEF: { Operand o = eaLong(bus);       sbcOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // SBC long
    case 0xFF: { Operand o = eaLongX(bus);      sbcOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // SBC long,X
    case 0xE1: { Operand o = eaDirIndX(bus);    sbcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // SBC (dir,X)
    case 0xF1: { Operand o = eaDirIndY(bus);    sbcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // SBC (dir),Y
    case 0xF2: { Operand o = eaDirInd(bus);     sbcOp(readValue(bus, o, accum8())); return 6u - accCyc() + dpCyc(); }        // SBC (dir)
    case 0xE7: { Operand o = eaDirIndLong(bus); sbcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // SBC [dir]
    case 0xF7: { Operand o = eaDirIndLongY(bus);sbcOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // SBC [dir],Y
    case 0xE3: { Operand o = eaStack(bus);      sbcOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // SBC stk,S
    case 0xF3: { Operand o = eaStackIndY(bus);  sbcOp(readValue(bus, o, accum8())); return 8u - accCyc(); }                  // SBC (stk,S),Y

    // ---- CMP: compare with the accumulator (accumulator width; N,Z,C) ----
    case 0xCD: { Operand o = eaAbs(bus);        compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 5u - accCyc(); }                  // CMP abs
    case 0xDD: { Operand o = eaAbsX(bus);       compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // CMP abs,X
    case 0xD9: { Operand o = eaAbsY(bus);       compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // CMP abs,Y
    case 0xCF: { Operand o = eaLong(bus);       compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 6u - accCyc(); }                  // CMP long
    case 0xDF: { Operand o = eaLongX(bus);      compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 6u - accCyc(); }                  // CMP long,X
    case 0xC1: { Operand o = eaDirIndX(bus);    compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 7u - accCyc() + dpCyc(); }        // CMP (dir,X)
    case 0xD1: { Operand o = eaDirIndY(bus);    compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // CMP (dir),Y
    case 0xD2: { Operand o = eaDirInd(bus);     compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 6u - accCyc() + dpCyc(); }        // CMP (dir)
    case 0xC7: { Operand o = eaDirIndLong(bus); compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 7u - accCyc() + dpCyc(); }        // CMP [dir]
    case 0xD7: { Operand o = eaDirIndLongY(bus);compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 7u - accCyc() + dpCyc(); }        // CMP [dir],Y
    case 0xC3: { Operand o = eaStack(bus);      compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 5u - accCyc(); }                  // CMP stk,S
    case 0xD3: { Operand o = eaStackIndY(bus);  compareWith(state_.a, readValue(bus, o, accum8()), accum8()); return 8u - accCyc(); }                  // CMP (stk,S),Y

    // ---- CPX / CPY: compare with an index register (index width; N,Z,C) ----
    case 0xEC: { Operand o = eaAbs(bus); compareWith(state_.x, readValue(bus, o, index8()), index8()); return 5u - indexCyc(); }            // CPX abs
    case 0xCC: { Operand o = eaAbs(bus); compareWith(state_.y, readValue(bus, o, index8()), index8()); return 5u - indexCyc(); }            // CPY abs

    // ---- AND: bitwise AND into the accumulator (accumulator width; N,Z) ----
    case 0x2D: { Operand o = eaAbs(bus);        andOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // AND abs
    case 0x3D: { Operand o = eaAbsX(bus);       andOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // AND abs,X
    case 0x39: { Operand o = eaAbsY(bus);       andOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // AND abs,Y
    case 0x2F: { Operand o = eaLong(bus);       andOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // AND long
    case 0x3F: { Operand o = eaLongX(bus);      andOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // AND long,X
    case 0x21: { Operand o = eaDirIndX(bus);    andOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // AND (dir,X)
    case 0x31: { Operand o = eaDirIndY(bus);    andOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // AND (dir),Y
    case 0x32: { Operand o = eaDirInd(bus);     andOp(readValue(bus, o, accum8())); return 6u - accCyc() + dpCyc(); }        // AND (dir)
    case 0x27: { Operand o = eaDirIndLong(bus); andOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // AND [dir]
    case 0x37: { Operand o = eaDirIndLongY(bus);andOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // AND [dir],Y
    case 0x23: { Operand o = eaStack(bus);      andOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // AND stk,S
    case 0x33: { Operand o = eaStackIndY(bus);  andOp(readValue(bus, o, accum8())); return 8u - accCyc(); }                  // AND (stk,S),Y

    // ---- EOR: bitwise exclusive-OR into the accumulator (accumulator width; N,Z) ----
    case 0x4D: { Operand o = eaAbs(bus);        eorOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // EOR abs
    case 0x5D: { Operand o = eaAbsX(bus);       eorOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // EOR abs,X
    case 0x59: { Operand o = eaAbsY(bus);       eorOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // EOR abs,Y
    case 0x4F: { Operand o = eaLong(bus);       eorOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // EOR long
    case 0x5F: { Operand o = eaLongX(bus);      eorOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // EOR long,X
    case 0x41: { Operand o = eaDirIndX(bus);    eorOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // EOR (dir,X)
    case 0x51: { Operand o = eaDirIndY(bus);    eorOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // EOR (dir),Y
    case 0x52: { Operand o = eaDirInd(bus);     eorOp(readValue(bus, o, accum8())); return 6u - accCyc() + dpCyc(); }        // EOR (dir)
    case 0x47: { Operand o = eaDirIndLong(bus); eorOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // EOR [dir]
    case 0x57: { Operand o = eaDirIndLongY(bus);eorOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // EOR [dir],Y
    case 0x43: { Operand o = eaStack(bus);      eorOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // EOR stk,S
    case 0x53: { Operand o = eaStackIndY(bus);  eorOp(readValue(bus, o, accum8())); return 8u - accCyc(); }                  // EOR (stk,S),Y

    // ---- ORA: bitwise OR into the accumulator (accumulator width; N,Z) ----
    case 0x0D: { Operand o = eaAbs(bus);        oraOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // ORA abs
    case 0x1D: { Operand o = eaAbsX(bus);       oraOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // ORA abs,X
    case 0x19: { Operand o = eaAbsY(bus);       oraOp(readValue(bus, o, accum8())); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // ORA abs,Y
    case 0x0F: { Operand o = eaLong(bus);       oraOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // ORA long
    case 0x1F: { Operand o = eaLongX(bus);      oraOp(readValue(bus, o, accum8())); return 6u - accCyc(); }                  // ORA long,X
    case 0x01: { Operand o = eaDirIndX(bus);    oraOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // ORA (dir,X)
    case 0x11: { Operand o = eaDirIndY(bus);    oraOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // ORA (dir),Y
    case 0x12: { Operand o = eaDirInd(bus);     oraOp(readValue(bus, o, accum8())); return 6u - accCyc() + dpCyc(); }        // ORA (dir)
    case 0x07: { Operand o = eaDirIndLong(bus); oraOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // ORA [dir]
    case 0x17: { Operand o = eaDirIndLongY(bus);oraOp(readValue(bus, o, accum8())); return 7u - accCyc() + dpCyc(); }        // ORA [dir],Y
    case 0x03: { Operand o = eaStack(bus);      oraOp(readValue(bus, o, accum8())); return 5u - accCyc(); }                  // ORA stk,S
    case 0x13: { Operand o = eaStackIndY(bus);  oraOp(readValue(bus, o, accum8())); return 8u - accCyc(); }                  // ORA (stk,S),Y

    // ---- BIT: test bits against the accumulator (accumulator width). Non-immediate
    //      forms take N,V from the operand's top two bits and set Z from the AND;
    //      the immediate form sets Z alone ----
    case 0x2C: { Operand o = eaAbs(bus);  bitOp(readValue(bus, o, accum8()), false); return 5u - accCyc(); }            // BIT abs
    case 0x3C: { Operand o = eaAbsX(bus); bitOp(readValue(bus, o, accum8()), false); return 6u - accCyc() - indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // BIT abs,X

    // ---- INC / DEC: step a value by one (accumulator width; N,Z) ----
    case 0xEE: { Operand o = eaAbs(bus);  rmwMem(bus, o, &Cpu65816::incOp); return 8u - 2u * accCyc(); }            // INC abs
    case 0xFE: { Operand o = eaAbsX(bus); rmwMem(bus, o, &Cpu65816::incOp); return 9u - 2u * accCyc(); }            // INC abs,X
    case 0xCE: { Operand o = eaAbs(bus);  rmwMem(bus, o, &Cpu65816::decOp); return 8u - 2u * accCyc(); }            // DEC abs
    case 0xDE: { Operand o = eaAbsX(bus); rmwMem(bus, o, &Cpu65816::decOp); return 9u - 2u * accCyc(); }            // DEC abs,X

    // ---- ASL / LSR / ROL / ROR: shifts and rotates (accumulator width; N,Z,C) ----
    case 0x0E: { Operand o = eaAbs(bus);  rmwMem(bus, o, &Cpu65816::aslOp); return 8u - 2u * accCyc(); }            // ASL abs
    case 0x1E: { Operand o = eaAbsX(bus); rmwMem(bus, o, &Cpu65816::aslOp); return 9u - 2u * accCyc(); }            // ASL abs,X
    case 0x4E: { Operand o = eaAbs(bus);  rmwMem(bus, o, &Cpu65816::lsrOp); return 8u - 2u * accCyc(); }            // LSR abs
    case 0x5E: { Operand o = eaAbsX(bus); rmwMem(bus, o, &Cpu65816::lsrOp); return 9u - 2u * accCyc(); }            // LSR abs,X
    case 0x2E: { Operand o = eaAbs(bus);  rmwMem(bus, o, &Cpu65816::rolOp); return 8u - 2u * accCyc(); }            // ROL abs
    case 0x3E: { Operand o = eaAbsX(bus); rmwMem(bus, o, &Cpu65816::rolOp); return 9u - 2u * accCyc(); }            // ROL abs,X
    case 0x6E: { Operand o = eaAbs(bus);  rmwMem(bus, o, &Cpu65816::rorOp); return 8u - 2u * accCyc(); }            // ROR abs
    case 0x7E: { Operand o = eaAbsX(bus); rmwMem(bus, o, &Cpu65816::rorOp); return 9u - 2u * accCyc(); }            // ROR abs,X

    // ---- TSB / TRB: test-and-set / test-and-reset memory bits against A (Z only) ----
    case 0x0C: { Operand o = eaAbs(bus); rmwMem(bus, o, &Cpu65816::tsbOp); return 8u - 2u * accCyc(); }            // TSB abs
    case 0x1C: { Operand o = eaAbs(bus); rmwMem(bus, o, &Cpu65816::trbOp); return 8u - 2u * accCyc(); }            // TRB abs

    // ---- push a register onto the stack (no flags) ----
    case 0x48:  // PHA
      if (accum8()) push8(bus, static_cast<std::uint8_t>(state_.a)); else push16(bus, state_.a);
      return 4u - accCyc();
    case 0xDA:  // PHX
      if (index8()) push8(bus, static_cast<std::uint8_t>(state_.x)); else push16(bus, state_.x);
      return 4u - indexCyc();
    case 0x5A:  // PHY
      if (index8()) push8(bus, static_cast<std::uint8_t>(state_.y)); else push16(bus, state_.y);
      return 4u - indexCyc();
    case 0x08: push8(bus, state_.p); return 3;                          // PHP
    case 0x8B: pushWide8(bus, state_.dbr); settleStack(); return 3;     // PHB
    case 0x4B: pushWide8(bus, state_.pbr); settleStack(); return 3;     // PHK
    case 0x0B: pushWide16(bus, state_.d); settleStack(); return 4;      // PHD
    case 0xF4: pushWide16(bus, fetchWord(bus)); settleStack(); return 5;  // PEA #imm
    case 0xD4: { const std::uint16_t v = readDpWord(bus, dpAddr(fetch(bus), 0)); pushWide16(bus, v); settleStack(); return 6u + dpCyc(); }  // PEI dir
    case 0x62: { const std::uint16_t rel = fetchWord(bus); pushWide16(bus, static_cast<std::uint16_t>(state_.pc + rel)); settleStack(); return 6; }  // PER

    // ---- pull a register from the stack ----
    case 0x68: loadA(accum8() ? pull8(bus) : pull16(bus)); return 5u - accCyc();     // PLA (N,Z at the accumulator width)
    case 0xFA: loadX(index8() ? pull8(bus) : pull16(bus)); return 5u - indexCyc();   // PLX (N,Z at the index width)
    case 0x7A: loadY(index8() ? pull8(bus) : pull16(bus)); return 5u - indexCyc();   // PLY
    case 0x28: writeP(pull8(bus)); return 4;                                          // PLP
    case 0xAB: { const std::uint8_t v = pullWide8(bus); settleStack(); state_.dbr = v; setNZ8(v); return 4; }    // PLB
    case 0x2B: { const std::uint16_t v = pullWide16(bus); settleStack(); state_.d = v; setNZ16(v); return 5; }   // PLD

    default:
      // An opcode the core does not decode. It returns a zero cycle count, which a
      // vector's non-zero count rejects loudly rather than silently.
      return 0;
  }
}

}  // namespace snaggletooth
