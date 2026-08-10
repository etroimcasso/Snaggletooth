#pragma once

// The 65816 — the SNES main CPU's instruction-set core.
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

// The 65816's view of the system: a 24-bit address space with 8-bit data. A
// conforming bus answers an 8-bit read for any address and accepts an 8-bit write
// to one; only the low 24 bits of the address are meaningful.
template <typename B>
concept SnesBus = requires(B bus, std::uint32_t address, std::uint8_t value) {
  { bus.read(address) } -> std::same_as<std::uint8_t>;
  bus.write(address, value);
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
  CpuRunState run = CpuRunState::Running;
};

class Cpu65816 {
 public:
  Cpu65816() = default;
  explicit Cpu65816(Cpu65816State state) : state_(state) {}

  [[nodiscard]] const Cpu65816State& state() const noexcept { return state_; }
  void restore(Cpu65816State state) noexcept { state_ = state; }

  // Executes one instruction and returns its chip cycle count (the machine
  // converts chip cycles to master cycles). Cycle counts follow the documented
  // per-instruction totals, including the memory-width, index-width, direct-page
  // and page-cross adjustments.
  template <SnesBus B>
  std::uint32_t step(B& bus);

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

  // ---- program fetch ------------------------------------------------------
  // Reads the byte at the program counter and advances it. The program counter is
  // 16-bit and wraps within the program bank; the bank register does not change.
  template <SnesBus B>
  std::uint8_t fetch(B& bus) {
    const std::uint8_t v =
        bus.read((static_cast<std::uint32_t>(state_.pbr) << 16) | state_.pc);
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

  // A 16-bit pointer read from the direct page (its bytes wrap per the direct-page
  // rule); a 24-bit pointer likewise; and a 16-bit pointer read from the stack
  // area (bank-zero wrap, no page wrap — stack-relative is a native-only mode).
  template <SnesBus B>
  std::uint16_t readDpWord(B& bus, std::uint32_t base) {
    const std::uint16_t lo = bus.read(base);
    const std::uint16_t hi = bus.read(nextByte(base, AddrKind::Direct));
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  // The 24-bit indirect-long modes are new to the 65816: their pointer wraps at
  // the bank-zero boundary but never within a page, even while emulation mode
  // page-wraps the old direct-page modes — so its bytes step with the bank-zero
  // rule, not the direct-page one.
  template <SnesBus B>
  std::uint32_t readDpLong(B& bus, std::uint32_t base) {
    const std::uint32_t b0 = bus.read(base);
    const std::uint32_t a1 = nextByte(base, AddrKind::Bank0);
    const std::uint32_t b1 = bus.read(a1);
    const std::uint32_t b2 = bus.read(nextByte(a1, AddrKind::Bank0));
    return b0 | (b1 << 8) | (b2 << 16);
  }
  template <SnesBus B>
  std::uint16_t readStackWord(B& bus, std::uint32_t base) {
    const std::uint16_t lo = bus.read(base);
    const std::uint16_t hi = bus.read(nextByte(base, AddrKind::Bank0));
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
  Operand eaDir(B& bus) {
    return {dpAddr(fetch(bus), 0), AddrKind::Direct, false};
  }
  template <SnesBus B>
  Operand eaDirX(B& bus) {
    return {dpAddr(fetch(bus), idxX()), AddrKind::Direct, false};
  }
  template <SnesBus B>
  Operand eaDirY(B& bus) {
    return {dpAddr(fetch(bus), idxY()), AddrKind::Direct, false};
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
  std::uint16_t readValue(B& bus, const Operand& op, bool eightBit) {
    const std::uint16_t lo = bus.read(op.addr);
    if (eightBit) return lo;
    const std::uint16_t hi = bus.read(nextByte(op.addr, op.kind));
    return static_cast<std::uint16_t>(lo | (hi << 8));
  }
  template <SnesBus B>
  void writeValue(B& bus, const Operand& op, std::uint16_t v, bool eightBit) {
    bus.write(op.addr, static_cast<std::uint8_t>(v));
    if (!eightBit) {
      bus.write(nextByte(op.addr, op.kind), static_cast<std::uint8_t>(v >> 8));
    }
  }

  Cpu65816State state_{};
};

template <SnesBus B>
std::uint32_t Cpu65816::step(B& bus) {
  normalize();
  const std::uint8_t opcode = fetch(bus);
  switch (opcode) {
    // ---- LDA: load the accumulator (N,Z at the accumulator width) ----
    case 0xA9: {  // LDA #imm
      const std::uint16_t v = accum8() ? fetch(bus) : fetchWord(bus);
      loadA(v);
      return 3u - accCyc();
    }
    case 0xA5: { Operand o = eaDir(bus);        loadA(readValue(bus, o, accum8())); return 4u - accCyc() + dpCyc(); }        // LDA dir
    case 0xB5: { Operand o = eaDirX(bus);       loadA(readValue(bus, o, accum8())); return 5u - accCyc() + dpCyc(); }        // LDA dir,X
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
    case 0xA2: {  // LDX #imm
      const std::uint16_t v = index8() ? fetch(bus) : fetchWord(bus);
      loadX(v);
      return 3u - indexCyc();
    }
    case 0xA6: { Operand o = eaDir(bus);   loadX(readValue(bus, o, index8())); return 4u - indexCyc() + dpCyc(); }  // LDX dir
    case 0xB6: { Operand o = eaDirY(bus);  loadX(readValue(bus, o, index8())); return 5u - indexCyc() + dpCyc(); }  // LDX dir,Y
    case 0xAE: { Operand o = eaAbs(bus);   loadX(readValue(bus, o, index8())); return 5u - indexCyc(); }            // LDX abs
    case 0xBE: { Operand o = eaAbsY(bus);  loadX(readValue(bus, o, index8())); return 6u - 2u * indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDX abs,Y
    case 0xA0: {  // LDY #imm
      const std::uint16_t v = index8() ? fetch(bus) : fetchWord(bus);
      loadY(v);
      return 3u - indexCyc();
    }
    case 0xA4: { Operand o = eaDir(bus);   loadY(readValue(bus, o, index8())); return 4u - indexCyc() + dpCyc(); }  // LDY dir
    case 0xB4: { Operand o = eaDirX(bus);  loadY(readValue(bus, o, index8())); return 5u - indexCyc() + dpCyc(); }  // LDY dir,X
    case 0xAC: { Operand o = eaAbs(bus);   loadY(readValue(bus, o, index8())); return 5u - indexCyc(); }            // LDY abs
    case 0xBC: { Operand o = eaAbsX(bus);  loadY(readValue(bus, o, index8())); return 6u - 2u * indexCyc() + indexCyc() * (o.pageCross ? 1u : 0u); }  // LDY abs,X

    // ---- STA: store the accumulator (no flags; indexed stores take no page-cross
    //      discount — the extra cycle is always paid) ----
    case 0x85: { Operand o = eaDir(bus);        writeValue(bus, o, state_.a, accum8()); return 4u - accCyc() + dpCyc(); }  // STA dir
    case 0x95: { Operand o = eaDirX(bus);       writeValue(bus, o, state_.a, accum8()); return 5u - accCyc() + dpCyc(); }  // STA dir,X
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
    case 0x86: { Operand o = eaDir(bus);  writeValue(bus, o, state_.x, index8()); return 4u - indexCyc() + dpCyc(); }  // STX dir
    case 0x96: { Operand o = eaDirY(bus); writeValue(bus, o, state_.x, index8()); return 5u - indexCyc() + dpCyc(); }  // STX dir,Y
    case 0x8E: { Operand o = eaAbs(bus);  writeValue(bus, o, state_.x, index8()); return 5u - indexCyc(); }            // STX abs
    case 0x84: { Operand o = eaDir(bus);  writeValue(bus, o, state_.y, index8()); return 4u - indexCyc() + dpCyc(); }  // STY dir
    case 0x94: { Operand o = eaDirX(bus); writeValue(bus, o, state_.y, index8()); return 5u - indexCyc() + dpCyc(); }  // STY dir,X
    case 0x8C: { Operand o = eaAbs(bus);  writeValue(bus, o, state_.y, index8()); return 5u - indexCyc(); }            // STY abs

    // ---- STZ: store zero (accumulator width; no flags) ----
    case 0x64: { Operand o = eaDir(bus);  writeValue(bus, o, 0, accum8()); return 4u - accCyc() + dpCyc(); }  // STZ dir
    case 0x74: { Operand o = eaDirX(bus); writeValue(bus, o, 0, accum8()); return 5u - accCyc() + dpCyc(); }  // STZ dir,X
    case 0x9C: { Operand o = eaAbs(bus);  writeValue(bus, o, 0, accum8()); return 5u - accCyc(); }            // STZ abs
    case 0x9E: { Operand o = eaAbsX(bus); writeValue(bus, o, 0, accum8()); return 6u - accCyc(); }            // STZ abs,X

    // ---- register transfers (2 cycles; the destination's width decides how many
    //      bits move, and sets N,Z — except the transfers into the stack pointer,
    //      which set no flags) ----
    case 0xAA:  // TAX
      if (index8()) { state_.x = static_cast<std::uint16_t>(state_.a & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.a)); }
      else { state_.x = state_.a; setNZ16(state_.a); }
      return 2;
    case 0xA8:  // TAY
      if (index8()) { state_.y = static_cast<std::uint16_t>(state_.a & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.a)); }
      else { state_.y = state_.a; setNZ16(state_.a); }
      return 2;
    case 0xBA:  // TSX
      if (index8()) { state_.x = static_cast<std::uint16_t>(state_.s & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.s)); }
      else { state_.x = state_.s; setNZ16(state_.s); }
      return 2;
    case 0x8A:  // TXA
      if (accum8()) { state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | (state_.x & 0xFFu)); setNZ8(static_cast<std::uint8_t>(state_.x)); }
      else { state_.a = state_.x; setNZ16(state_.x); }
      return 2;
    case 0x98:  // TYA
      if (accum8()) { state_.a = static_cast<std::uint16_t>((state_.a & 0xFF00u) | (state_.y & 0xFFu)); setNZ8(static_cast<std::uint8_t>(state_.y)); }
      else { state_.a = state_.y; setNZ16(state_.y); }
      return 2;
    case 0x9B:  // TXY
      if (index8()) { state_.y = static_cast<std::uint16_t>(state_.x & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.x)); }
      else { state_.y = state_.x; setNZ16(state_.x); }
      return 2;
    case 0xBB:  // TYX
      if (index8()) { state_.x = static_cast<std::uint16_t>(state_.y & 0xFFu); setNZ8(static_cast<std::uint8_t>(state_.y)); }
      else { state_.x = state_.y; setNZ16(state_.y); }
      return 2;
    case 0x9A:  // TXS (no flags; SH is $01 in emulation, so only XL moves there)
      state_.s = state_.e ? static_cast<std::uint16_t>(0x0100u | (state_.x & 0xFFu)) : state_.x;
      return 2;

    // ---- 16-bit transfers to and from the direct and stack registers. These move
    //      the whole 16-bit accumulator regardless of the m width and set N,Z on
    //      the 16-bit result — except TCS, which sets no flags ----
    case 0x5B: state_.d = state_.a; setNZ16(state_.d); return 2;  // TCD
    case 0x7B: state_.a = state_.d; setNZ16(state_.a); return 2;  // TDC
    case 0x3B: state_.a = state_.s; setNZ16(state_.a); return 2;  // TSC
    case 0x1B:  // TCS (no flags; SH is $01 in emulation, so only the low byte moves)
      state_.s = state_.e ? static_cast<std::uint16_t>(0x0100u | (state_.a & 0xFFu)) : state_.a;
      return 2;

    // ---- XBA: exchange the accumulator halves (N,Z on the new low byte, always
    //      8-bit) ----
    case 0xEB:
      state_.a = static_cast<std::uint16_t>(((state_.a & 0xFFu) << 8) | ((state_.a >> 8) & 0xFFu));
      setNZ8(static_cast<std::uint8_t>(state_.a));
      return 3;

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
      return 2;
    }

    default:
      // Not yet routed. This family lands opcode by opcode across the sub-blocks;
      // an opcode that reaches here before its family is implemented returns zero,
      // which a vector's non-zero cycle count rejects loudly rather than silently.
      return 0;
  }
}

}  // namespace snaggletooth
