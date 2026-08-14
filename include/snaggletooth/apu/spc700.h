#pragma once

// The SPC700 — the SNES audio CPU's instruction-set core.
//
// The core is cycle-stepped: stepCycle() executes exactly one chip cycle. A cycle
// either reaches memory — one read or one write, never both — or is internal, and an
// internal cycle touches the bus not at all. A machine that advances timers and the
// sample clock alongside execution needs that grain; so does a test that compares the
// core against a per-cycle recording of the real chip.
//
// Instruction progress is part of the state value, not the call stack: an instruction
// register, a cycle index within the instruction, and a few scratch fields. So state()
// and restore() are legal at any cycle, mid-instruction included, and a snapshot is
// still a plain copy of the struct.
//
// stepInstruction() runs cycles to the next instruction boundary and returns how many
// it took — the whole-instruction view, for hosts and for tests that care about final
// state.
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

  // How far into an instruction the core is. These carry no architectural meaning —
  // they are the progress a running instruction has made, held as values so a
  // snapshot taken mid-instruction restores to the same cycle.
  std::uint8_t ir = 0;    // the instruction being executed
  std::uint8_t tcu = 0;   // cycle index within it; 0 means the next cycle fetches
  std::uint16_t ea = 0;   // effective-address scratch
  std::uint16_t ptr = 0;  // pointer / second-address scratch
  std::uint16_t tmp = 0;  // data scratch (an operand byte, or a value in flight)
  bool taken = false;     // a branch condition, settled before the cycles it prices
};

class Spc700 {
 public:
  Spc700() = default;
  explicit Spc700(Spc700State state) : state_(state) {}

  [[nodiscard]] const Spc700State& state() const noexcept { return state_; }
  void restore(Spc700State state) noexcept { state_ = state; }

  // Executes exactly one chip cycle. The cycle either reaches memory — one read or
  // one write — or is internal, in which case it touches the bus not at all. A halted
  // core (sleeping or stopped) touches neither state nor bus; its host prices the idle
  // cycle from the run state.
  template <ApuBus B>
  void stepCycle(B& bus);

  // Runs cycles until the next instruction boundary and returns how many it took.
  // Called mid-instruction it finishes the instruction in progress rather than
  // starting one. On a non-Running core this returns 2 cycles and touches neither
  // state nor the bus.
  template <ApuBus B>
  std::uint32_t stepInstruction(B& bus);

  // Whether the core sits between instructions — the only point at which an
  // instruction can begin, and the state a completed instruction leaves behind.
  [[nodiscard]] bool atInstructionBoundary() const noexcept { return state_.tcu == 0; }

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

  // The stack lives in page $01, addressed by SP as its low byte. `step` counts
  // entries from the one SP names — a push reaches 0 then -1, a pop reaches +1 then
  // +2 — and the count wraps inside the page, because SP is an 8-bit register. The
  // register itself settles on the instruction's last cycle, so every address in a
  // stack instruction is measured from where SP began.
  [[nodiscard]] std::uint16_t stackAddr(int step) const noexcept {
    return static_cast<std::uint16_t>(0x0100u + ((state_.sp + step) & 0xFF));
  }

  // ---- the cycle engine ---------------------------------------------------
  // Executes one cycle of the instruction already in the instruction register, and
  // answers whether that cycle was its last. The cycle index says which cycle is due;
  // every branch a cycle can take is decided by state the earlier cycles latched, so
  // a handler never has to look ahead.
  template <ApuBus B>
  bool executeCycle(B& bus);

  // ---- reaching memory ----------------------------------------------------
  // Where a memory instruction's operand lives. Each mode is a fixed cycle sequence:
  // the operand bytes it fetches, the internal cycles it spends, and the pointer
  // reads it makes before its address is settled.
  enum class AddrMode : std::uint8_t {
    Implied,             // no operand at all
    Immediate,           // the byte after the opcode
    Dp,                  // dp + offset
    DpX,                 // dp + (offset + X), wrapping inside the page
    DpY,                 // dp + (offset + Y), wrapping inside the page
    Abs,                 // a two-byte address
    AbsX,                // that address plus X
    AbsY,                // that address plus Y
    AbsBit,              // a two-byte operand: an address in the low 13 bits, a bit index
                         // in the top 3
    Indirect,            // (X): dp + X
    IndirectIncrement,   // (X)+: the same, and X steps after
    IndexedIndirect,     // [dp+X]: X indexes the offset, then a pointer
    IndirectIndexed,     // [dp]+Y: a pointer, then Y indexes the address
    IndirectToIndirect,  // (X),(Y): the Y side is the source, the X side the target
    DpToDp,              // two direct-page offsets: source then destination
    ImmediateToDp,       // an immediate byte and a direct-page offset
  };

  // What an instruction does once its address is settled.
  enum class MemAccess : std::uint8_t {
    Read,      // read the byte and apply it to a register or to the carry flag
    Write,     // put a value there
    Modify,    // read the byte, compute from it, and write the result back
    Internal,  // read the byte and settle on it inside the chip, writing nothing
  };

  // What an instruction that reaches a destination does on the cycles between settling
  // its address and writing. Most forms read the byte once: a modify needs it, and a
  // plain write discards it but still makes the access, so a register that clears when
  // read sees it. The two test-and-set instructions read it twice; the bit store reads
  // it and then spends a cycle inside the chip; one store spends its only such cycle
  // inside the chip; and the two-operand moves have no such cycle at all.
  enum class DestinationCycle : std::uint8_t {
    None,
    Internal,
    Read,
    ReadTwice,
    ReadThenWait,
  };

  // How many cycles a form spends between settling its address and writing.
  [[nodiscard]] static constexpr std::uint8_t destinationCycles(
      DestinationCycle destination) noexcept {
    switch (destination) {
      case DestinationCycle::None: return 0;
      case DestinationCycle::Internal:
      case DestinationCycle::Read: return 1;
      case DestinationCycle::ReadTwice:
      case DestinationCycle::ReadThenWait: return 2;
    }
    return 0;
  }

  // The shape of one memory instruction: where its operand lives, what it does there,
  // how it reaches its destination, and how many internal cycles it spends.
  struct MemForm {
    AddrMode mode = AddrMode::Implied;
    MemAccess access = MemAccess::Read;
    DestinationCycle destination = DestinationCycle::Read;
    std::uint8_t internals = 0;  // extra internal cycles, on the implied forms only
  };

  // Fills in the shape of a memory instruction and answers whether the opcode is one
  // the cycle engine carries. Instructions sharing a mode share its cycle sequence, so
  // an opcode's only per-instruction contributions are this shape and the payload
  // applied once its operand has arrived.
  [[nodiscard]] static constexpr bool memoryForm(std::uint8_t opcode,
                                                 MemForm& form) noexcept;

  // How many cycles a mode spends before it reaches memory: its operand fetches, its
  // internal cycles, and its pointer reads.
  [[nodiscard]] static constexpr std::uint8_t setupCycles(AddrMode mode) noexcept;

  // Where a direct-page word instruction's cycles go. One offset names both of its
  // bytes — the second wraps inside the page — and the shapes differ in what happens
  // between them.
  enum class WordForm : std::uint8_t {
    None,     // not a direct-page word instruction
    Read,     // the low byte · a cycle inside the chip · the high byte
    Compare,  // the low byte · the high byte, with no cycle between them
    Modify,   // the low byte · write it back · the high byte · write it back
    Store,    // read the low byte · write the low byte · write the high byte
  };

  // Which of those shapes an opcode runs, and whether it is a word instruction at all.
  [[nodiscard]] static constexpr WordForm wordForm(std::uint8_t opcode) noexcept;

  // Whether an indirect mode spends its internal cycle before reading its pointer.
  // The read forms and [dp+X] do; the write form of [dp]+Y spends it after.
  [[nodiscard]] static constexpr bool waitsBeforePointer(const MemForm& form) noexcept {
    return form.mode != AddrMode::IndirectIndexed || form.access == MemAccess::Read;
  }

  // The address of one byte of an indirect mode's pointer. Both bytes live in the
  // direct page, and the second wraps inside it.
  [[nodiscard]] std::uint16_t pointerAddr(const MemForm& form,
                                          std::uint8_t byte) const noexcept {
    const std::uint8_t index =
        form.mode == AddrMode::IndexedIndirect ? state_.x : std::uint8_t{0};
    return dpAddr(static_cast<std::uint8_t>(state_.tmp + index + byte));
  }

  // Executes one cycle of a memory instruction.
  template <ApuBus B>
  bool executeMemoryCycle(B& bus, const MemForm& form);

  // Executes one of the cycles a mode spends settling its address. Answers whether the
  // instruction is finished, which only an implied one is.
  template <ApuBus B>
  bool executeSetupCycle(B& bus, const MemForm& form);

  // Applies an instruction that has finished reading its operand.
  void applyMemoryRead(std::uint8_t opcode, std::uint8_t value);

  // The value a store writes.
  [[nodiscard]] std::uint8_t memoryStoreValue(std::uint8_t opcode) const noexcept;

  // The value a read-modify-write instruction writes back: computed from the byte at
  // the destination and, for the two-operand forms, the source byte beside it. The
  // flags it sets are the instruction's, so this runs on the cycle that writes.
  [[nodiscard]] std::uint8_t memoryModifyValue(std::uint8_t opcode, std::uint8_t destination,
                                               std::uint8_t source);

  // Applies an implied instruction — a register-to-register move, a register's own
  // increment, shift or nibble exchange, or one of the multi-cycle computations the
  // chip runs entirely inside itself.
  void applyImplied(std::uint8_t opcode);

  // Applies an instruction that settles on the byte it read without writing anything —
  // a comparison, or a carry-bit operation that spends a last cycle inside the chip.
  void applyInternalResult(std::uint8_t opcode, std::uint8_t destination,
                           std::uint8_t source);

  // Executes one cycle of a direct-page word instruction.
  template <ApuBus B>
  bool executeWordCycle(B& bus, WordForm form);

  // Applies a word instruction that has both of its bytes.
  void applyWordRead(std::uint8_t opcode, std::uint16_t word);

  // ---- reaching the program counter ---------------------------------------
  // What an instruction that moves the program counter does with it. Each kind is
  // its own cycle sequence: a branch reads a displacement and adds it, a jump takes
  // a destination, a call puts the return address on the stack around the same work,
  // and a return takes one back off it.
  enum class CtrlKind : std::uint8_t {
    Branch,           // the eight conditional relative branches, and BRA
    BitBranch,        // BBS/BBC dp.b and CBNE dp: a direct-page byte, then the offset
    CompareIndexed,   // CBNE dp+X: the same, with the indexing cycle before the byte
    DecrementDp,      // DBNZ dp: the decremented byte is written before the offset
    DecrementY,       // DBNZ Y: the offset's own address is read a second time
    Jump,             // JMP !abs
    JumpIndexed,      // JMP [!abs+X]: an indexed pointer, its halves a linear byte apart
    Call,             // CALL !abs
    CallPage,         // PCALL up: the destination is one byte into page $FF
    CallVector,       // TCALL n: the destination comes from the table below $FFDE
    Break,            // BRK: the same table, with the status byte pushed as well
    Return,           // RET
    ReturnInterrupt,  // RETI: the status byte comes back under the return address
    Push,             // PUSH A/X/Y/PSW
    Pop,              // POP A/X/Y/PSW
    Halt,             // SLEEP / STOP
  };

  // The shape of one control-flow instruction. A relative branch also carries the
  // status bit it tests and the state of that bit which takes it.
  struct CtrlForm {
    CtrlKind kind = CtrlKind::Branch;
    std::uint8_t flag = 0;  // the status bit the branch tests; 0 for BRA
    bool whenSet = false;   // whether the branch is taken with that bit set
  };

  // Fills in the shape of a control-flow instruction and answers whether the opcode
  // is one.
  [[nodiscard]] static constexpr bool controlForm(std::uint8_t opcode,
                                                  CtrlForm& form) noexcept;

  // Whether a relative branch's condition holds. BRA carries no flag and is always
  // taken.
  [[nodiscard]] bool branchTaken(const CtrlForm& form) const noexcept {
    return form.flag == 0 || (((state_.psw & form.flag) != 0) == form.whenSet);
  }

  // Whether one of the instructions that reads a byte before its displacement takes
  // its branch. The two compare forms branch when A differs from the byte; BBS and
  // BBC test one of its bits, the index in the opcode's top three bits and the
  // polarity in the fourth — an even high nibble branches on the bit set.
  [[nodiscard]] bool byteBranchTaken(std::uint8_t opcode,
                                     std::uint8_t value) const noexcept {
    if (opcode == 0x2E || opcode == 0xDE) return state_.a != value;
    const std::uint8_t high = static_cast<std::uint8_t>(opcode >> 4);
    const bool set = ((value >> (high >> 1)) & 1u) != 0;
    return set == ((high & 1u) == 0u);
  }

  // Adds the displacement in the data scratch to the program counter, which already
  // points past the whole instruction.
  void takeBranch() noexcept {
    state_.pc = static_cast<std::uint16_t>(state_.pc +
                                           static_cast<std::int8_t>(state_.tmp));
  }

  // The table entry a call through the top of memory reads its destination from. The
  // table ends at $FFDE and counts downwards, two bytes per entry; BRK takes the same
  // entry as TCALL 0.
  [[nodiscard]] std::uint16_t callVector() const noexcept {
    const unsigned entry = state_.ir == 0x0F ? 0u : unsigned{state_.ir} >> 4;
    return static_cast<std::uint16_t>(0xFFDEu - 2u * entry);
  }

  // The register a push writes.
  [[nodiscard]] std::uint8_t pushValue(std::uint8_t opcode) const noexcept {
    switch (opcode) {
      case 0x2D: return state_.a;
      case 0x4D: return state_.x;
      case 0x6D: return state_.y;
      default: return state_.psw;  // PUSH PSW
    }
  }

  // Lands a popped byte in its register. None of the three register forms sets a
  // flag; POP PSW replaces the whole status byte, the flags it carries included.
  void applyPop(std::uint8_t opcode, std::uint8_t value) noexcept {
    switch (opcode) {
      case 0xAE: state_.a = value; break;
      case 0xCE: state_.x = value; break;
      case 0xEE: state_.y = value; break;
      default: state_.psw = value; break;  // POP PSW
    }
  }

  // Executes one cycle of a control-flow instruction.
  template <ApuBus B>
  bool executeControlCycle(B& bus, const CtrlForm& form);

  // The 16-bit accumulator pair: Y is the high byte, A the low.
  [[nodiscard]] std::uint16_t ya() const noexcept {
    return static_cast<std::uint16_t>(state_.a | (state_.y << 8));
  }
  void setYa(std::uint16_t word) noexcept {
    state_.a = static_cast<std::uint8_t>(word);
    state_.y = static_cast<std::uint8_t>(word >> 8);
  }

  // A bit-addressing instruction's operand names an address in its low 13 bits and a
  // bit index in its top 3. The whole operand stays in the pointer scratch, so the
  // index is still there when the byte arrives.
  [[nodiscard]] std::uint8_t operandBitMask() const noexcept {
    return static_cast<std::uint8_t>(1u << (state_.ptr >> 13));
  }
  [[nodiscard]] bool operandBit(std::uint8_t value) const noexcept {
    return (value & operandBitMask()) != 0;
  }

  // Whether a mode carries a second operand byte, which lives in the pointer scratch
  // while the byte at the destination lives in the data scratch.
  [[nodiscard]] static constexpr bool twoOperand(AddrMode mode) noexcept {
    return mode == AddrMode::IndirectToIndirect || mode == AddrMode::DpToDp ||
           mode == AddrMode::ImmediateToDp;
  }

  // The direct-page address an offset names: the base plus the offset, which wraps
  // inside the page.
  [[nodiscard]] std::uint16_t dpAddr(std::uint8_t offset) const noexcept {
    return static_cast<std::uint16_t>(dpBase() + offset);
  }

  // Reads the byte after the opcode and discards it. A one-byte instruction still
  // issues that read — on real hardware it has the side effects any read has — but the
  // program counter does not step over it.
  template <ApuBus B>
  void discardNextByte(B& bus) {
    static_cast<void>(bus.read(state_.pc));
  }

  Spc700State state_{};
};

// The routing table for the cycle engine: every opcode it carries, with the mode whose
// cycle sequence it runs and what it does at the end of it.
constexpr bool Spc700::memoryForm(std::uint8_t opcode, MemForm& form) noexcept {
  switch (opcode) {
    // ---- 8-bit move: memory to register ----
    case 0xE8: case 0xCD: case 0x8D:  // MOV A/X/Y,#imm
      form = MemForm{.mode = AddrMode::Immediate, .access = MemAccess::Read};
      return true;
    case 0xE4: case 0xF8: case 0xEB:  // MOV A/X/Y,dp
      form = MemForm{.mode = AddrMode::Dp, .access = MemAccess::Read};
      return true;
    case 0xF4: case 0xFB:             // MOV A/Y,dp+X
      form = MemForm{.mode = AddrMode::DpX, .access = MemAccess::Read};
      return true;
    case 0xF9:                        // MOV X,dp+Y
      form = MemForm{.mode = AddrMode::DpY, .access = MemAccess::Read};
      return true;
    case 0xE5: case 0xE9: case 0xEC:  // MOV A/X/Y,!abs
      form = MemForm{.mode = AddrMode::Abs, .access = MemAccess::Read};
      return true;
    case 0xF5:                        // MOV A,!abs+X
      form = MemForm{.mode = AddrMode::AbsX, .access = MemAccess::Read};
      return true;
    case 0xF6:                        // MOV A,!abs+Y
      form = MemForm{.mode = AddrMode::AbsY, .access = MemAccess::Read};
      return true;
    case 0xE7:                        // MOV A,[dp+X]
      form = MemForm{.mode = AddrMode::IndexedIndirect, .access = MemAccess::Read};
      return true;
    case 0xF7:                        // MOV A,[dp]+Y
      form = MemForm{.mode = AddrMode::IndirectIndexed, .access = MemAccess::Read};
      return true;
    case 0xE6:                        // MOV A,(X)
      form = MemForm{.mode = AddrMode::Indirect, .access = MemAccess::Read};
      return true;
    case 0xBF:                        // MOV A,(X)+
      form = MemForm{.mode = AddrMode::IndirectIncrement, .access = MemAccess::Read};
      return true;

    // ---- 8-bit move: register to memory ----
    case 0xC4: case 0xCB: case 0xD8:  // MOV dp,A/Y/X
      form = MemForm{.mode = AddrMode::Dp, .access = MemAccess::Write};
      return true;
    case 0xD4: case 0xDB:             // MOV dp+X,A/Y
      form = MemForm{.mode = AddrMode::DpX, .access = MemAccess::Write};
      return true;
    case 0xD9:                        // MOV dp+Y,X
      form = MemForm{.mode = AddrMode::DpY, .access = MemAccess::Write};
      return true;
    case 0xC5: case 0xC9: case 0xCC:  // MOV !abs,A/X/Y
      form = MemForm{.mode = AddrMode::Abs, .access = MemAccess::Write};
      return true;
    case 0xD5:                        // MOV !abs+X,A
      form = MemForm{.mode = AddrMode::AbsX, .access = MemAccess::Write};
      return true;
    case 0xD6:                        // MOV !abs+Y,A
      form = MemForm{.mode = AddrMode::AbsY, .access = MemAccess::Write};
      return true;
    case 0xC7:                        // MOV [dp+X],A
      form = MemForm{.mode = AddrMode::IndexedIndirect, .access = MemAccess::Write};
      return true;
    case 0xD7:                        // MOV [dp]+Y,A
      form = MemForm{.mode = AddrMode::IndirectIndexed, .access = MemAccess::Write};
      return true;
    case 0xC6:                        // MOV (X),A
      form = MemForm{.mode = AddrMode::Indirect, .access = MemAccess::Write};
      return true;
    case 0xAF:                        // MOV (X)+,A — the one store that spends its
      form = MemForm{.mode = AddrMode::IndirectIncrement,  // destination cycle
                     .access = MemAccess::Write,           // internally
                     .destination = DestinationCycle::Internal};
      return true;

    // ---- 8-bit move: register to register, and the two-operand direct-page moves ----
    case 0x7D: case 0xDD: case 0x5D: case 0xFD: case 0x9D: case 0xBD:
      form = MemForm{.mode = AddrMode::Implied};
      return true;
    case 0xFA:                        // MOV dp,dp — the one two-operand form that
      form = MemForm{.mode = AddrMode::DpToDp,           // reaches its destination
                     .access = MemAccess::Write,         // only to write
                     .destination = DestinationCycle::None};
      return true;
    case 0x8F:                        // MOV dp,#imm
      form = MemForm{.mode = AddrMode::ImmediateToDp, .access = MemAccess::Write};
      return true;

    // ---- 8-bit arithmetic, logic and comparison against a register ----
    // Every one of these reads a byte and applies it to A, X or Y; they differ only in
    // the payload, so they share the read sequence of the move that fetches the same way.
    case 0x88: case 0xA8: case 0x68: case 0xC8: case 0xAD:  // ADC/SBC/CMP A,#imm,
    case 0x28: case 0x08: case 0x48:                        // CMP X/Y,#imm, AND/OR/EOR
      form = MemForm{.mode = AddrMode::Immediate, .access = MemAccess::Read};
      return true;
    case 0x84: case 0xA4: case 0x64: case 0x3E: case 0x7E:  // ...,dp
    case 0x24: case 0x04: case 0x44:
      form = MemForm{.mode = AddrMode::Dp, .access = MemAccess::Read};
      return true;
    case 0x94: case 0xB4: case 0x74: case 0x34: case 0x14: case 0x54:  // ...,dp+X
      form = MemForm{.mode = AddrMode::DpX, .access = MemAccess::Read};
      return true;
    case 0x85: case 0xA5: case 0x65: case 0x1E: case 0x5E:  // ...,!abs
    case 0x25: case 0x05: case 0x45:
      form = MemForm{.mode = AddrMode::Abs, .access = MemAccess::Read};
      return true;
    case 0x95: case 0xB5: case 0x75: case 0x35: case 0x15: case 0x55:  // ...,!abs+X
      form = MemForm{.mode = AddrMode::AbsX, .access = MemAccess::Read};
      return true;
    case 0x96: case 0xB6: case 0x76: case 0x36: case 0x16: case 0x56:  // ...,!abs+Y
      form = MemForm{.mode = AddrMode::AbsY, .access = MemAccess::Read};
      return true;
    case 0x87: case 0xA7: case 0x67: case 0x27: case 0x07: case 0x47:  // ...,[dp+X]
      form = MemForm{.mode = AddrMode::IndexedIndirect, .access = MemAccess::Read};
      return true;
    case 0x97: case 0xB7: case 0x77: case 0x37: case 0x17: case 0x57:  // ...,[dp]+Y
      form = MemForm{.mode = AddrMode::IndirectIndexed, .access = MemAccess::Read};
      return true;
    case 0x86: case 0xA6: case 0x66: case 0x26: case 0x06: case 0x46:  // ...,(X)
      form = MemForm{.mode = AddrMode::Indirect, .access = MemAccess::Read};
      return true;

    // ---- 8-bit increment, decrement, shift and rotation of a register ----
    case 0xBC: case 0x3D: case 0xFC: case 0x9C: case 0x1D: case 0xDC:  // INC/DEC A/X/Y
    case 0x1C: case 0x5C: case 0x3C: case 0x7C:                        // ASL/LSR/ROL/ROR A
      form = MemForm{.mode = AddrMode::Implied};
      return true;
    case 0x9F:                        // XCN A — the exchange runs on internal cycles
      form = MemForm{.mode = AddrMode::Implied, .internals = 3};
      return true;

    // ---- 8-bit increment, decrement, shift and rotation of a byte in memory ----
    // The read-modify-write seat: the byte is read, the result computed from it, and
    // the result written back to the same address.
    case 0xAB: case 0x8B: case 0x0B: case 0x4B: case 0x2B: case 0x6B:  // ...dp
      form = MemForm{.mode = AddrMode::Dp, .access = MemAccess::Modify};
      return true;
    case 0xBB: case 0x9B: case 0x1B: case 0x5B: case 0x3B: case 0x7B:  // ...dp+X
      form = MemForm{.mode = AddrMode::DpX, .access = MemAccess::Modify};
      return true;
    case 0xAC: case 0x8C: case 0x0C: case 0x4C: case 0x2C: case 0x6C:  // ...!abs
      form = MemForm{.mode = AddrMode::Abs, .access = MemAccess::Modify};
      return true;

    // ---- 8-bit arithmetic and logic between two bytes in memory ----
    // The Y side is the source and the X side both the other operand and the target.
    case 0x99: case 0xB9: case 0x39: case 0x19: case 0x59:  // ADC/SBC/AND/OR/EOR (X),(Y)
      form = MemForm{.mode = AddrMode::IndirectToIndirect, .access = MemAccess::Modify};
      return true;
    case 0x79:                        // CMP (X),(Y) — compares and writes nothing
      form = MemForm{.mode = AddrMode::IndirectToIndirect, .access = MemAccess::Internal};
      return true;
    case 0x89: case 0xA9: case 0x29: case 0x09: case 0x49:  // ...dp,dp
      form = MemForm{.mode = AddrMode::DpToDp, .access = MemAccess::Modify};
      return true;
    case 0x69:                        // CMP dp,dp
      form = MemForm{.mode = AddrMode::DpToDp, .access = MemAccess::Internal};
      return true;
    case 0x98: case 0xB8: case 0x38: case 0x18: case 0x58:  // ...dp,#imm
      form = MemForm{.mode = AddrMode::ImmediateToDp, .access = MemAccess::Modify};
      return true;
    case 0x78:                        // CMP dp,#imm
      form = MemForm{.mode = AddrMode::ImmediateToDp, .access = MemAccess::Internal};
      return true;

    // ---- multiply, divide and decimal adjust ----
    // Each reads the byte after the opcode, discards it, and spends every remaining
    // cycle computing inside the chip.
    case 0xCF:                        // MUL YA
      form = MemForm{.mode = AddrMode::Implied, .internals = 7};
      return true;
    case 0x9E:                        // DIV YA,X
      form = MemForm{.mode = AddrMode::Implied, .internals = 10};
      return true;
    case 0xDF: case 0xBE:             // DAA / DAS A
      form = MemForm{.mode = AddrMode::Implied, .internals = 1};
      return true;

    // ---- the status flags, and the instruction that does nothing at all ----
    // Each reads the byte after the opcode and discards it, and the three that settle
    // a cycle later spend that cycle inside the chip.
    case 0x00:                        // NOP
    case 0x20: case 0x40:             // CLRP / SETP
    case 0x60: case 0x80:             // CLRC / SETC
    case 0xE0:                        // CLRV
      form = MemForm{.mode = AddrMode::Implied};
      return true;
    case 0xED: case 0xA0: case 0xC0:  // NOTC / EI / DI
      form = MemForm{.mode = AddrMode::Implied, .internals = 1};
      return true;

    // ---- one bit of a direct-page byte, set or cleared ----
    // The read-modify-write seat once more: the byte is read and written back with a
    // single bit changed, and no flag moves.
    case 0x02: case 0x22: case 0x42: case 0x62:  // SET1 dp.0..7
    case 0x82: case 0xA2: case 0xC2: case 0xE2:
    case 0x12: case 0x32: case 0x52: case 0x72:  // CLR1 dp.0..7
    case 0x92: case 0xB2: case 0xD2: case 0xF2:
      form = MemForm{.mode = AddrMode::Dp, .access = MemAccess::Modify};
      return true;

    // ---- test and set or clear the bits of an absolute byte ----
    case 0x0E: case 0x4E:             // TSET1 / TCLR1 !abs — the byte is read, and
      form = MemForm{.mode = AddrMode::Abs,             // then read a second time
                     .access = MemAccess::Modify,
                     .destination = DestinationCycle::ReadTwice};
      return true;

    // ---- the carry flag against one bit of an absolute byte ----
    case 0x4A: case 0x6A: case 0xAA:  // AND1 C,m.b and C,/m.b; MOV1 C,m.b
      form = MemForm{.mode = AddrMode::AbsBit, .access = MemAccess::Read};
      return true;
    case 0x0A: case 0x2A: case 0x8A:  // OR1 C,m.b and C,/m.b; EOR1 C,m.b — each pays
      form = MemForm{.mode = AddrMode::AbsBit,            // a last cycle inside the chip
                     .access = MemAccess::Internal};
      return true;
    case 0xEA:                        // NOT1 m.b
      form = MemForm{.mode = AddrMode::AbsBit, .access = MemAccess::Modify};
      return true;
    case 0xCA:                        // MOV1 m.b,C — reads its byte, spends a cycle
      form = MemForm{.mode = AddrMode::AbsBit,            // inside the chip, then writes
                     .access = MemAccess::Modify,
                     .destination = DestinationCycle::ReadThenWait};
      return true;

    default:
      return false;
  }
}

// The routing table for the instructions that move the program counter. Every opcode
// the memory and word tables do not carry is one of these, so between the three every
// opcode has a cycle sequence.
constexpr bool Spc700::controlForm(std::uint8_t opcode, CtrlForm& form) noexcept {
  switch (opcode) {
    // ---- the relative branches. Two cycles when the condition fails, and two more
    //      inside the chip when it holds ----
    case 0x2F:  // BRA — no flag, so always taken
      form = CtrlForm{.kind = CtrlKind::Branch};
      return true;
    case 0xF0:  // BEQ
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagZ, .whenSet = true};
      return true;
    case 0xD0:  // BNE
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagZ};
      return true;
    case 0xB0:  // BCS
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagC, .whenSet = true};
      return true;
    case 0x90:  // BCC
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagC};
      return true;
    case 0x70:  // BVS
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagV, .whenSet = true};
      return true;
    case 0x50:  // BVC
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagV};
      return true;
    case 0x30:  // BMI
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagN, .whenSet = true};
      return true;
    case 0x10:  // BPL
      form = CtrlForm{.kind = CtrlKind::Branch, .flag = kFlagN};
      return true;

    // ---- the branches that read a byte first: one bit of a direct-page byte, or
    //      that byte against A. They share a cycle sequence and differ in the test ----
    case 0x03: case 0x23: case 0x43: case 0x63:  // BBS dp.0..7,rel
    case 0x83: case 0xA3: case 0xC3: case 0xE3:
    case 0x13: case 0x33: case 0x53: case 0x73:  // BBC dp.0..7,rel
    case 0x93: case 0xB3: case 0xD3: case 0xF3:
    case 0x2E:                                   // CBNE dp,rel
      form = CtrlForm{.kind = CtrlKind::BitBranch};
      return true;
    case 0xDE:                                   // CBNE dp+X,rel
      form = CtrlForm{.kind = CtrlKind::CompareIndexed};
      return true;
    case 0x6E:                                   // DBNZ dp,rel
      form = CtrlForm{.kind = CtrlKind::DecrementDp};
      return true;
    case 0xFE:                                   // DBNZ Y,rel
      form = CtrlForm{.kind = CtrlKind::DecrementY};
      return true;

    // ---- jumps, calls and returns ----
    case 0x5F: form = CtrlForm{.kind = CtrlKind::Jump}; return true;         // JMP !abs
    case 0x1F: form = CtrlForm{.kind = CtrlKind::JumpIndexed}; return true;  // JMP [!abs+X]
    case 0x3F: form = CtrlForm{.kind = CtrlKind::Call}; return true;         // CALL !abs
    case 0x4F: form = CtrlForm{.kind = CtrlKind::CallPage}; return true;     // PCALL up
    case 0x01: case 0x11: case 0x21: case 0x31:  // TCALL 0..7
    case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xA1: case 0xB1:  // TCALL 8..15
    case 0xC1: case 0xD1: case 0xE1: case 0xF1:
      form = CtrlForm{.kind = CtrlKind::CallVector};
      return true;
    case 0x0F: form = CtrlForm{.kind = CtrlKind::Break}; return true;            // BRK
    case 0x6F: form = CtrlForm{.kind = CtrlKind::Return}; return true;           // RET
    case 0x7F: form = CtrlForm{.kind = CtrlKind::ReturnInterrupt}; return true;  // RETI

    // ---- the stack ----
    case 0x2D: case 0x4D: case 0x6D: case 0x0D:  // PUSH A/X/Y/PSW
      form = CtrlForm{.kind = CtrlKind::Push};
      return true;
    case 0xAE: case 0xCE: case 0xEE: case 0x8E:  // POP A/X/Y/PSW
      form = CtrlForm{.kind = CtrlKind::Pop};
      return true;

    // ---- the halts ----
    case 0xEF: case 0xFF:                        // SLEEP / STOP
      form = CtrlForm{.kind = CtrlKind::Halt};
      return true;

    default:
      return false;
  }
}

// The direct-page word instructions, which the cycle engine carries beside the memory
// forms: one offset, two bytes, and a shape that says what happens between them.
constexpr Spc700::WordForm Spc700::wordForm(std::uint8_t opcode) noexcept {
  switch (opcode) {
    case 0x7A: case 0x9A: case 0xBA: return WordForm::Read;     // ADDW/SUBW/MOVW YA,dp
    case 0x5A: return WordForm::Compare;                        // CMPW YA,dp
    case 0x1A: case 0x3A: return WordForm::Modify;              // DECW / INCW dp
    case 0xDA: return WordForm::Store;                          // MOVW dp,YA
    default: return WordForm::None;
  }
}

constexpr std::uint8_t Spc700::setupCycles(AddrMode mode) noexcept {
  switch (mode) {
    case AddrMode::Immediate: return 0;      // the operand fetch is the access itself
    case AddrMode::Implied:
    case AddrMode::Dp:
    case AddrMode::Indirect:
    case AddrMode::IndirectIncrement: return 1;
    case AddrMode::DpX:
    case AddrMode::DpY:
    case AddrMode::Abs:
    case AddrMode::AbsBit:
    case AddrMode::IndirectToIndirect:
    case AddrMode::ImmediateToDp: return 2;
    case AddrMode::AbsX:
    case AddrMode::AbsY:
    case AddrMode::DpToDp: return 3;
    case AddrMode::IndexedIndirect:
    case AddrMode::IndirectIndexed: return 4;
  }
  return 0;
}

// An address settles over a mode's setup cycles: the operand bytes come off the
// program stream, an indexed mode spends a cycle on the addition, and an indirect one
// reads its pointer out of the direct page. Only an implied instruction finishes here
// — it has no address to settle.
template <ApuBus B>
bool Spc700::executeSetupCycle(B& bus, const MemForm& form) {
  switch (form.mode) {
    case AddrMode::Implied: {
      // The byte after the opcode is read and thrown away. Whatever the instruction
      // computes happens inside the chip, over as many cycles as it takes, and lands
      // on the last of them.
      const std::uint8_t last = static_cast<std::uint8_t>(1 + form.internals);
      if (state_.tcu == 1) discardNextByte(bus);
      if (state_.tcu != last) return false;
      applyImplied(state_.ir);
      return true;
    }

    case AddrMode::Dp:
      state_.ea = dpAddr(fetch(bus));
      return false;

    case AddrMode::DpX:
    case AddrMode::DpY:
      if (state_.tcu == 1) {
        state_.tmp = fetch(bus);
        return false;
      }
      // The indexing cycle: the addition happens inside the chip, so the cycle
      // reaches memory not at all.
      state_.ea = dpAddr(static_cast<std::uint8_t>(
          state_.tmp + (form.mode == AddrMode::DpX ? state_.x : state_.y)));
      return false;

    case AddrMode::Abs:
    case AddrMode::AbsX:
    case AddrMode::AbsY:
    case AddrMode::AbsBit:
      if (state_.tcu == 1) {
        state_.ptr = fetch(bus);
        return false;
      }
      if (state_.tcu == 2) {
        state_.ptr = static_cast<std::uint16_t>(state_.ptr | (fetch(bus) << 8));
        // A plain absolute operand is the address itself. A bit operand carries the
        // address in its low 13 bits and the bit index in the three above them, so the
        // whole operand stays in the pointer scratch and only the address reaches memory.
        if (form.mode == AddrMode::Abs) state_.ea = state_.ptr;
        if (form.mode == AddrMode::AbsBit) {
          state_.ea = static_cast<std::uint16_t>(state_.ptr & 0x1FFFu);
        }
        return false;
      }
      state_.ea = static_cast<std::uint16_t>(
          state_.ptr + (form.mode == AddrMode::AbsX ? state_.x : state_.y));
      return false;

    case AddrMode::Indirect:
    case AddrMode::IndirectIncrement:
      // X is the whole address here, so the byte after the opcode is read and thrown
      // away rather than being an operand.
      discardNextByte(bus);
      state_.ea = dpAddr(state_.x);
      return false;

    case AddrMode::IndexedIndirect:
    case AddrMode::IndirectIndexed: {
      if (state_.tcu == 1) {
        state_.tmp = fetch(bus);
        return false;
      }
      const std::uint8_t pointerCycle = waitsBeforePointer(form) ? 3 : 2;
      if (state_.tcu == pointerCycle) {
        state_.ptr = bus.read(pointerAddr(form, 0));
        return false;
      }
      if (state_.tcu == pointerCycle + 1) {
        state_.ptr = static_cast<std::uint16_t>(state_.ptr |
                                                (bus.read(pointerAddr(form, 1)) << 8));
        state_.ea = form.mode == AddrMode::IndexedIndirect
                        ? state_.ptr
                        : static_cast<std::uint16_t>(state_.ptr + state_.y);
        return false;
      }
      // The internal cycle, which sits before the pointer or after it by the mode.
      return false;
    }

    case AddrMode::IndirectToIndirect:
      // X names the target and Y the source. X addresses the target on its own, so the
      // byte after the opcode is read and thrown away as it is for every (X) form.
      if (state_.tcu == 1) {
        discardNextByte(bus);
        state_.ea = dpAddr(state_.x);
        return false;
      }
      state_.ptr = bus.read(dpAddr(state_.y));
      return false;

    case AddrMode::DpToDp:
      // The source offset, then the byte it names, then the destination offset. The
      // source byte takes the pointer scratch once the offset that found it is spent.
      if (state_.tcu == 1) {
        state_.ptr = fetch(bus);
        return false;
      }
      if (state_.tcu == 2) {
        state_.ptr = bus.read(dpAddr(static_cast<std::uint8_t>(state_.ptr)));
        return false;
      }
      state_.ea = dpAddr(fetch(bus));
      return false;

    case AddrMode::ImmediateToDp:
      // The source byte is in the instruction, so only the destination offset follows.
      if (state_.tcu == 1) {
        state_.ptr = fetch(bus);
        return false;
      }
      state_.ea = dpAddr(fetch(bus));
      return false;

    case AddrMode::Immediate:
      break;  // its operand fetch is the access itself, so it has no setup cycle
  }
  return false;
}

// Once the address is settled the instruction reaches memory: one cycle to read the
// byte, or two to reach the destination and settle there. Register work lands on the
// instruction's last cycle, so a snapshot taken mid-instruction never holds a register
// the hardware has not written yet.
template <ApuBus B>
bool Spc700::executeMemoryCycle(B& bus, const MemForm& form) {
  const std::uint8_t setup =
      static_cast<std::uint8_t>(setupCycles(form.mode) + form.internals);
  if (state_.tcu <= setup) return executeSetupCycle(bus, form);

  const std::uint8_t step = static_cast<std::uint8_t>(state_.tcu - setup);
  if (form.mode == AddrMode::Immediate) {
    applyMemoryRead(state_.ir, fetch(bus));
    return true;
  }

  if (form.access == MemAccess::Read) {
    if (step == 1) {
      const std::uint8_t value = bus.read(state_.ea);
      if (form.mode != AddrMode::IndirectIncrement) {
        applyMemoryRead(state_.ir, value);
        return true;
      }
      state_.tmp = value;
      return false;
    }
    // (X)+ spends a last cycle stepping X, and the load settles with it.
    applyMemoryRead(state_.ir, static_cast<std::uint8_t>(state_.tmp));
    ++state_.x;
    return true;
  }

  // The cycles between the settled address and the write. Most forms read the byte
  // there once — a modify needs it, a plain store discards it but still makes the
  // access. The test-and-set pair reads it twice, MOV1 m.b,C reads it and then spends a
  // cycle inside the chip, MOV (X)+,A spends its one such cycle inside the chip, and
  // the two-operand moves have no such cycle at all.
  if (step <= destinationCycles(form.destination)) {
    const bool reachesMemory =
        form.destination == DestinationCycle::Read ||
        form.destination == DestinationCycle::ReadTwice ||
        (form.destination == DestinationCycle::ReadThenWait && step == 1);
    if (reachesMemory) state_.tmp = bus.read(state_.ea);
    return false;
  }

  const std::uint8_t destination = static_cast<std::uint8_t>(state_.tmp);
  const std::uint8_t source = static_cast<std::uint8_t>(state_.ptr);
  switch (form.access) {
    case MemAccess::Write:
      // A two-operand move carries the byte it writes; every other store writes a
      // register.
      bus.write(state_.ea,
                twoOperand(form.mode) ? source : memoryStoreValue(state_.ir));
      if (form.mode == AddrMode::IndirectIncrement) ++state_.x;
      break;
    case MemAccess::Modify:
      bus.write(state_.ea, memoryModifyValue(state_.ir, destination, source));
      break;
    case MemAccess::Internal:
      // The result settles inside the chip, so this cycle reaches memory not at all.
      applyInternalResult(state_.ir, destination, source);
      break;
    case MemAccess::Read:
      break;  // a read settles on the cycle above, never here
  }
  return true;
}

inline void Spc700::applyMemoryRead(std::uint8_t opcode, std::uint8_t value) {
  switch (opcode) {
    case 0xE8: case 0xE6: case 0xBF: case 0xE4: case 0xF4:
    case 0xE5: case 0xF5: case 0xF6: case 0xE7: case 0xF7:
      state_.a = value;
      setNZ(value);
      break;
    case 0xCD: case 0xF8: case 0xF9: case 0xE9:
      state_.x = value;
      setNZ(value);
      break;
    case 0x8D: case 0xEB: case 0xFB: case 0xEC:
      state_.y = value;
      setNZ(value);
      break;

    // ---- the same reads, applied arithmetically to A ----
    case 0x88: case 0x86: case 0x84: case 0x94: case 0x85:
    case 0x95: case 0x96: case 0x87: case 0x97:              // ADC A,operand
      state_.a = adcOp(state_.a, value);
      break;
    case 0xA8: case 0xA6: case 0xA4: case 0xB4: case 0xA5:
    case 0xB5: case 0xB6: case 0xA7: case 0xB7:              // SBC A,operand
      state_.a = sbcOp(state_.a, value);
      break;
    case 0x68: case 0x66: case 0x64: case 0x74: case 0x65:
    case 0x75: case 0x76: case 0x67: case 0x77:              // CMP A,operand
      cmpOp(state_.a, value);
      break;
    case 0xC8: case 0x3E: case 0x1E:                         // CMP X,operand
      cmpOp(state_.x, value);
      break;
    case 0xAD: case 0x7E: case 0x5E:                         // CMP Y,operand
      cmpOp(state_.y, value);
      break;
    case 0x28: case 0x26: case 0x24: case 0x34: case 0x25:
    case 0x35: case 0x36: case 0x27: case 0x37:              // AND A,operand
      state_.a = static_cast<std::uint8_t>(state_.a & value);
      setNZ(state_.a);
      break;
    case 0x08: case 0x06: case 0x04: case 0x14: case 0x05:
    case 0x15: case 0x16: case 0x07: case 0x17:              // OR A,operand
      state_.a = static_cast<std::uint8_t>(state_.a | value);
      setNZ(state_.a);
      break;
    case 0x48: case 0x46: case 0x44: case 0x54: case 0x45:
    case 0x55: case 0x56: case 0x47: case 0x57:              // EOR A,operand
      state_.a = static_cast<std::uint8_t>(state_.a ^ value);
      setNZ(state_.a);
      break;

    // ---- one bit of that byte, into the carry flag ----
    case 0x4A:                                               // AND1 C,m.b
      setCarry((state_.psw & kFlagC) && operandBit(value));
      break;
    case 0x6A:                                               // AND1 C,/m.b
      setCarry((state_.psw & kFlagC) && !operandBit(value));
      break;
    case 0xAA:                                               // MOV1 C,m.b
      setCarry(operandBit(value));
      break;

    default:
      // Every form that reads a byte into a register or into the carry flag is
      // listed above; the tables send nothing else here.
      break;
  }
}

inline std::uint8_t Spc700::memoryStoreValue(std::uint8_t opcode) const noexcept {
  switch (opcode) {
    case 0xC6: case 0xAF: case 0xC4: case 0xD4: case 0xC5:
    case 0xD5: case 0xD6: case 0xC7: case 0xD7:
      return state_.a;
    case 0xD8: case 0xD9: case 0xC9:
      return state_.x;
    case 0xCB: case 0xDB: case 0xCC:
      return state_.y;
    default:
      // As for applyMemoryRead: every store form is listed above.
      return 0;
  }
}

inline void Spc700::applyImplied(std::uint8_t opcode) {
  switch (opcode) {
    case 0x7D: state_.a = state_.x;  setNZ(state_.a); break;  // MOV A,X
    case 0xDD: state_.a = state_.y;  setNZ(state_.a); break;  // MOV A,Y
    case 0x5D: state_.x = state_.a;  setNZ(state_.x); break;  // MOV X,A
    case 0xFD: state_.y = state_.a;  setNZ(state_.y); break;  // MOV Y,A
    case 0x9D: state_.x = state_.sp; setNZ(state_.x); break;  // MOV X,SP
    case 0xBD: state_.sp = state_.x; break;                   // MOV SP,X (no flags)

    // ---- a register's own increment, decrement, shift and rotation ----
    case 0xBC: ++state_.a; setNZ(state_.a); break;            // INC A
    case 0x3D: ++state_.x; setNZ(state_.x); break;            // INC X
    case 0xFC: ++state_.y; setNZ(state_.y); break;            // INC Y
    case 0x9C: --state_.a; setNZ(state_.a); break;            // DEC A
    case 0x1D: --state_.x; setNZ(state_.x); break;            // DEC X
    case 0xDC: --state_.y; setNZ(state_.y); break;            // DEC Y
    case 0x1C: state_.a = aslOp(state_.a); break;             // ASL A
    case 0x5C: state_.a = lsrOp(state_.a); break;             // LSR A
    case 0x3C: state_.a = rolOp(state_.a); break;             // ROL A
    case 0x7C: state_.a = rorOp(state_.a); break;             // ROR A
    case 0x9F:                                                // XCN A
      state_.a = static_cast<std::uint8_t>((state_.a << 4) | (state_.a >> 4));
      setNZ(state_.a);
      break;

    // ---- multiply and divide (YA is the pair A=low, Y=high) ----
    case 0xCF: {                                              // MUL YA (N,Z from Y only)
      const std::uint16_t product =
          static_cast<std::uint16_t>(unsigned{state_.y} * unsigned{state_.a});
      setYa(product);
      setNZ(state_.y);
      break;
    }
    case 0x9E: {                                              // DIV YA,X
      // The documented 9-iteration restoring division: the 17-bit accumulator ends as
      // YYYYYYYY V AAAAAAAA, so Y and A are the quotient/remainder and bit 8 is the
      // overflow flag. N,Z come from A; H is the nibble comparison X&$F <= Y&$F on the
      // entry values. The result past a quotient of 511 is hardware garbage the
      // algorithm still reproduces.
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
      break;
    }

    // ---- decimal adjust (N,Z,C; H is read but never changed) ----
    case 0xDF: {                                              // DAA A
      std::uint16_t a = state_.a;
      if ((state_.psw & kFlagC) || a > 0x99u) {
        a += 0x60u;
        state_.psw = static_cast<std::uint8_t>(state_.psw | kFlagC);
      }
      if ((state_.psw & kFlagH) || (state_.a & 0x0Fu) > 0x09u) a += 0x06u;
      state_.a = static_cast<std::uint8_t>(a);
      setNZ(state_.a);
      break;
    }
    case 0xBE: {                                              // DAS A
      std::uint16_t a = state_.a;
      if (!(state_.psw & kFlagC) || a > 0x99u) {
        a -= 0x60u;
        state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagC);
      }
      if (!(state_.psw & kFlagH) || (state_.a & 0x0Fu) > 0x09u) a -= 0x06u;
      state_.a = static_cast<std::uint8_t>(a);
      setNZ(state_.a);
      break;
    }

    // ---- the status flags ----
    // CLRV clears the half-carry with the overflow, and I is an enable rather than a
    // disable: EI sets it, DI clears it. The audio unit has no interrupt source, so
    // nothing is ever delivered through it.
    case 0x60: state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagC); break;
    case 0x80: state_.psw = static_cast<std::uint8_t>(state_.psw | kFlagC); break;
    case 0xED: state_.psw = static_cast<std::uint8_t>(state_.psw ^ kFlagC); break;
    case 0x20: state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagP); break;
    case 0x40: state_.psw = static_cast<std::uint8_t>(state_.psw | kFlagP); break;
    case 0xE0:
      state_.psw = static_cast<std::uint8_t>(state_.psw & ~(kFlagV | kFlagH));
      break;
    case 0xA0: state_.psw = static_cast<std::uint8_t>(state_.psw | kFlagI); break;
    case 0xC0: state_.psw = static_cast<std::uint8_t>(state_.psw & ~kFlagI); break;

    case 0x00: break;  // NOP, which reads the byte after the opcode and nothing else

    default: break;
  }
}

inline void Spc700::applyInternalResult(std::uint8_t opcode, std::uint8_t destination,
                                        std::uint8_t source) {
  switch (opcode) {
    case 0x69: case 0x78: case 0x79:  // CMP dp,dp / dp,#imm / (X),(Y)
      cmpOp(destination, source);
      break;

    // ---- the carry flag against one bit of a byte ----
    case 0x0A:                                                // OR1 C,m.b
      setCarry((state_.psw & kFlagC) || operandBit(destination));
      break;
    case 0x2A:                                                // OR1 C,/m.b
      setCarry((state_.psw & kFlagC) || !operandBit(destination));
      break;
    case 0x8A:                                                // EOR1 C,m.b
      setCarry(((state_.psw & kFlagC) != 0) != operandBit(destination));
      break;

    default:
      // As for applyMemoryRead: every form that settles inside the chip is listed above.
      break;
  }
}

inline void Spc700::applyWordRead(std::uint8_t opcode, std::uint16_t word) {
  switch (opcode) {
    case 0xBA:                        // MOVW YA,dp
      setYa(word);
      setNZ16(word);
      break;
    case 0x7A: setYa(addwOp(ya(), word)); break;  // ADDW YA,dp
    case 0x9A: setYa(subwOp(ya(), word)); break;  // SUBW YA,dp
    case 0x5A: cmpwOp(ya(), word); break;         // CMPW YA,dp (the difference is discarded)

    default:
      // As for applyMemoryRead: every word form that reads a whole word is listed above.
      break;
  }
}

inline std::uint8_t Spc700::memoryModifyValue(std::uint8_t opcode, std::uint8_t destination,
                                              std::uint8_t source) {
  switch (opcode) {
    // ---- one byte, changed in place ----
    case 0xAB: case 0xBB: case 0xAC: {                        // INC dp / dp+X / !abs
      const std::uint8_t v = static_cast<std::uint8_t>(destination + 1);
      setNZ(v);
      return v;
    }
    case 0x8B: case 0x9B: case 0x8C: {                        // DEC dp / dp+X / !abs
      const std::uint8_t v = static_cast<std::uint8_t>(destination - 1);
      setNZ(v);
      return v;
    }
    case 0x0B: case 0x1B: case 0x0C: return aslOp(destination);  // ASL dp / dp+X / !abs
    case 0x4B: case 0x5B: case 0x4C: return lsrOp(destination);  // LSR dp / dp+X / !abs
    case 0x2B: case 0x3B: case 0x2C: return rolOp(destination);  // ROL dp / dp+X / !abs
    case 0x6B: case 0x7B: case 0x6C: return rorOp(destination);  // ROR dp / dp+X / !abs

    // ---- two bytes, the result landing on the destination ----
    case 0x99: case 0x89: case 0x98:                          // ADC (X),(Y) / dp,dp / dp,#imm
      return adcOp(destination, source);
    case 0xB9: case 0xA9: case 0xB8:                          // SBC ...
      return sbcOp(destination, source);
    case 0x39: case 0x29: case 0x38: {                        // AND ...
      const std::uint8_t v = static_cast<std::uint8_t>(destination & source);
      setNZ(v);
      return v;
    }
    case 0x19: case 0x09: case 0x18: {                        // OR ...
      const std::uint8_t v = static_cast<std::uint8_t>(destination | source);
      setNZ(v);
      return v;
    }
    case 0x59: case 0x49: case 0x58: {                        // EOR ...
      const std::uint8_t v = static_cast<std::uint8_t>(destination ^ source);
      setNZ(v);
      return v;
    }

    // ---- one bit of a direct-page byte, set or cleared (no flags) ----
    // The bit index is in the opcode's top three bits, and the low nibble tells the two
    // instructions apart: SET1 is $x2 with an even high nibble, CLR1 with an odd one.
    case 0x02: case 0x22: case 0x42: case 0x62:               // SET1 dp.0..7
    case 0x82: case 0xA2: case 0xC2: case 0xE2:
      return static_cast<std::uint8_t>(destination | (1u << (opcode >> 5)));
    case 0x12: case 0x32: case 0x52: case 0x72:               // CLR1 dp.0..7
    case 0x92: case 0xB2: case 0xD2: case 0xF2:
      return static_cast<std::uint8_t>(destination & ~(1u << (opcode >> 5)));

    // ---- the byte tested against A, then its bits set or cleared by A ----
    // The flags are those of A - memory; A itself is untouched, and the bits it holds
    // are the ones the write turns on or off.
    case 0x0E: case 0x4E:                                     // TSET1 / TCLR1 !abs
      setNZ(static_cast<std::uint8_t>(state_.a - destination));
      return opcode == 0x0E ? static_cast<std::uint8_t>(destination | state_.a)
                            : static_cast<std::uint8_t>(destination & ~state_.a);

    // ---- one bit of a byte, flipped or written from the carry flag (no flags) ----
    case 0xEA:                                                // NOT1 m.b
      return static_cast<std::uint8_t>(destination ^ operandBitMask());
    case 0xCA:                                                // MOV1 m.b,C
      return (state_.psw & kFlagC)
                 ? static_cast<std::uint8_t>(destination | operandBitMask())
                 : static_cast<std::uint8_t>(destination & ~operandBitMask());

    default:
      // As for applyMemoryRead: every modify form is listed above.
      return destination;
  }
}

// A direct-page word instruction, one cycle at a time. One offset settles both
// addresses at once — the low byte's, and the high byte's one past it, wrapped inside
// the page — and from there the shapes differ only in what happens between the two
// bytes.
template <ApuBus B>
bool Spc700::executeWordCycle(B& bus, WordForm form) {
  if (state_.tcu == 1) {
    const std::uint8_t offset = fetch(bus);
    state_.ea = dpAddr(offset);
    state_.ptr = dpAddr(static_cast<std::uint8_t>(offset + 1));
    return false;
  }

  const std::uint8_t step = static_cast<std::uint8_t>(state_.tcu - 1);
  switch (form) {
    case WordForm::Read:
    case WordForm::Compare:
      // Both read the low byte and then the high one; the arithmetic and move forms
      // spend a cycle inside the chip between them, and the comparison does not.
      if (step == 1) {
        state_.tmp = bus.read(state_.ea);
        return false;
      }
      if (form == WordForm::Read && step == 2) return false;
      applyWordRead(state_.ir, static_cast<std::uint16_t>(
                                   state_.tmp | (bus.read(state_.ptr) << 8)));
      return true;

    case WordForm::Modify: {
      // Each half is written back before the next is read, so the two addresses are
      // reached in the order the chip reaches them. The high byte moves only when the
      // low one wrapped past its own end, and the flags are those of the whole word.
      const bool increment = state_.ir == 0x3A;  // INCW dp; DECW dp is 0x1A
      if (step == 1) {
        state_.tmp = bus.read(state_.ea);
        return false;
      }
      if (step == 2) {
        const std::uint8_t low =
            static_cast<std::uint8_t>(increment ? state_.tmp + 1 : state_.tmp - 1);
        bus.write(state_.ea, low);
        state_.tmp = low;
        return false;
      }
      if (step == 3) {
        state_.tmp = static_cast<std::uint16_t>(state_.tmp | (bus.read(state_.ptr) << 8));
        return false;
      }
      const std::uint8_t low = static_cast<std::uint8_t>(state_.tmp);
      const std::uint8_t read = static_cast<std::uint8_t>(state_.tmp >> 8);
      const bool carries = increment ? low == 0x00 : low == 0xFF;
      const std::uint8_t high =
          static_cast<std::uint8_t>(carries ? (increment ? read + 1 : read - 1) : read);
      bus.write(state_.ptr, high);
      setNZ16(static_cast<std::uint16_t>(low | (high << 8)));
      return true;
    }

    case WordForm::Store:
      // The low byte is read and discarded before either half is written, so a store
      // through this instruction clears a register that clears when read — but only the
      // one at the low address. Neither write sets a flag.
      if (step == 1) {
        static_cast<void>(bus.read(state_.ea));
        return false;
      }
      if (step == 2) {
        bus.write(state_.ea, state_.a);
        return false;
      }
      bus.write(state_.ptr, state_.y);
      return true;

    case WordForm::None:
      break;  // executeCycle reaches this path only for the shapes above
  }
  return true;
}

// A control-flow instruction, one cycle at a time. Two laws run through the family.
// A branch's condition is settled on the cycle that reads what it tests, so the
// cycles it prices afterwards are already known to be spent or not. And the program
// counter, like every other register, moves on the instruction's last cycle — which
// is why a stack address is measured from where the stack pointer began, and why a
// return address goes onto the stack before the destination reaches the core.
template <ApuBus B>
bool Spc700::executeControlCycle(B& bus, const CtrlForm& form) {
  switch (form.kind) {
    // ---- the relative branches. The displacement arrives on the second cycle and
    //      the condition settles with it; a taken branch spends two more cycles
    //      inside the chip before the program counter moves ----
    case CtrlKind::Branch:
      if (state_.tcu == 1) {
        state_.tmp = fetch(bus);
        state_.taken = branchTaken(form);
        return !state_.taken;
      }
      if (state_.tcu == 2) return false;
      takeBranch();
      return true;

    // ---- BBS, BBC and CBNE dp: the direct-page byte is read, a cycle passes inside
    //      the chip, and only then does the displacement follow at PC+2 ----
    case CtrlKind::BitBranch:
      if (state_.tcu == 1) {
        state_.ea = dpAddr(fetch(bus));
        return false;
      }
      if (state_.tcu == 2) {
        state_.taken = byteBranchTaken(state_.ir, bus.read(state_.ea));
        return false;
      }
      if (state_.tcu == 3) return false;
      if (state_.tcu == 4) {
        state_.tmp = fetch(bus);
        return !state_.taken;
      }
      if (state_.tcu == 5) return false;
      takeBranch();
      return true;

    // ---- CBNE dp+X: the indexing cycle comes before the byte, as it does in every
    //      indexed mode, which puts the whole instruction one cycle behind CBNE dp ----
    case CtrlKind::CompareIndexed:
      if (state_.tcu == 1) {
        state_.tmp = fetch(bus);
        return false;
      }
      if (state_.tcu == 2) {
        state_.ea = dpAddr(static_cast<std::uint8_t>(state_.tmp + state_.x));
        return false;
      }
      if (state_.tcu == 3) {
        state_.taken = byteBranchTaken(state_.ir, bus.read(state_.ea));
        return false;
      }
      if (state_.tcu == 4) return false;
      if (state_.tcu == 5) {
        state_.tmp = fetch(bus);
        return !state_.taken;
      }
      if (state_.tcu == 6) return false;
      takeBranch();
      return true;

    // ---- DBNZ dp: the byte goes back decremented before the displacement is read,
    //      so the store lands whether or not the branch is taken ----
    case CtrlKind::DecrementDp:
      if (state_.tcu == 1) {
        state_.ea = dpAddr(fetch(bus));
        return false;
      }
      if (state_.tcu == 2) {
        state_.tmp = bus.read(state_.ea);
        return false;
      }
      if (state_.tcu == 3) {
        const std::uint8_t value = static_cast<std::uint8_t>(state_.tmp - 1);
        bus.write(state_.ea, value);
        state_.taken = value != 0;
        return false;
      }
      if (state_.tcu == 4) {
        state_.tmp = fetch(bus);
        return !state_.taken;
      }
      if (state_.tcu == 5) return false;
      takeBranch();
      return true;

    // ---- DBNZ Y: the displacement is read, a cycle passes on the decrement, and the
    //      displacement's own address is read a second time. Y settles at the end ----
    case CtrlKind::DecrementY:
      if (state_.tcu == 1) {
        state_.ea = state_.pc;
        state_.tmp = fetch(bus);
        state_.taken = static_cast<std::uint8_t>(state_.y - 1) != 0;
        return false;
      }
      if (state_.tcu == 2) return false;
      if (state_.tcu == 3) {
        static_cast<void>(bus.read(state_.ea));
        if (state_.taken) return false;
        --state_.y;
        return true;
      }
      if (state_.tcu == 4) return false;
      --state_.y;
      takeBranch();
      return true;

    // ---- JMP !abs: the operand is the destination, and no cycle is spent inside the
    //      chip at all — the shortest instruction that moves the program counter ----
    case CtrlKind::Jump:
      if (state_.tcu == 1) {
        state_.ptr = fetch(bus);
        return false;
      }
      state_.pc = static_cast<std::uint16_t>(state_.ptr | (fetch(bus) << 8));
      return true;

    // ---- JMP [!abs+X]: the operand plus X addresses a pointer, and that pointer's
    //      two bytes are one LINEAR byte apart. It is the one address in the core
    //      that does not wrap inside a page ----
    case CtrlKind::JumpIndexed:
      if (state_.tcu == 1) {
        state_.ptr = fetch(bus);
        return false;
      }
      if (state_.tcu == 2) {
        state_.ptr = static_cast<std::uint16_t>(state_.ptr | (fetch(bus) << 8));
        return false;
      }
      if (state_.tcu == 3) {
        state_.ea = static_cast<std::uint16_t>(state_.ptr + state_.x);
        return false;
      }
      if (state_.tcu == 4) {
        state_.tmp = bus.read(state_.ea);
        return false;
      }
      state_.pc = static_cast<std::uint16_t>(
          state_.tmp | (bus.read(static_cast<std::uint16_t>(state_.ea + 1)) << 8));
      return true;

    // ---- CALL !abs: the destination arrives first, then the return address goes on
    //      the stack high byte first, and two cycles pass inside the chip ----
    case CtrlKind::Call:
      if (state_.tcu == 1) {
        state_.ptr = fetch(bus);
        return false;
      }
      if (state_.tcu == 2) {
        state_.ptr = static_cast<std::uint16_t>(state_.ptr | (fetch(bus) << 8));
        return false;
      }
      if (state_.tcu == 3) return false;
      if (state_.tcu == 4) {
        bus.write(stackAddr(0), static_cast<std::uint8_t>(state_.pc >> 8));
        return false;
      }
      if (state_.tcu == 5) {
        bus.write(stackAddr(-1), static_cast<std::uint8_t>(state_.pc));
        return false;
      }
      if (state_.tcu == 6) return false;
      state_.sp = static_cast<std::uint8_t>(state_.sp - 2);
      state_.pc = state_.ptr;
      return true;

    // ---- PCALL: the destination is one byte into page $FF, so the pushes begin a
    //      cycle earlier than CALL's and only one trails them ----
    case CtrlKind::CallPage:
      if (state_.tcu == 1) {
        state_.ptr = static_cast<std::uint16_t>(0xFF00u | fetch(bus));
        return false;
      }
      if (state_.tcu == 2) return false;
      if (state_.tcu == 3) {
        bus.write(stackAddr(0), static_cast<std::uint8_t>(state_.pc >> 8));
        return false;
      }
      if (state_.tcu == 4) {
        bus.write(stackAddr(-1), static_cast<std::uint8_t>(state_.pc));
        return false;
      }
      state_.sp = static_cast<std::uint8_t>(state_.sp - 2);
      state_.pc = state_.ptr;
      return true;

    // ---- TCALL: the byte after the opcode is read and discarded, a cycle passes,
    //      the return address goes on the stack, and only after another cycle is the
    //      destination read out of the table ----
    case CtrlKind::CallVector:
      if (state_.tcu == 1) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu == 2) return false;
      if (state_.tcu == 3) {
        bus.write(stackAddr(0), static_cast<std::uint8_t>(state_.pc >> 8));
        return false;
      }
      if (state_.tcu == 4) {
        bus.write(stackAddr(-1), static_cast<std::uint8_t>(state_.pc));
        return false;
      }
      if (state_.tcu == 5) return false;
      if (state_.tcu == 6) {
        state_.tmp = bus.read(callVector());
        return false;
      }
      state_.pc = static_cast<std::uint16_t>(
          state_.tmp | (bus.read(static_cast<std::uint16_t>(callVector() + 1)) << 8));
      state_.sp = static_cast<std::uint8_t>(state_.sp - 2);
      return true;

    // ---- BRK: the same table entry TCALL 0 reaches, with the status byte pushed
    //      under the return address — and the three pushes begin immediately, where
    //      TCALL spends a cycle inside the chip first ----
    case CtrlKind::Break:
      if (state_.tcu == 1) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu == 2) {
        bus.write(stackAddr(0), static_cast<std::uint8_t>(state_.pc >> 8));
        return false;
      }
      if (state_.tcu == 3) {
        bus.write(stackAddr(-1), static_cast<std::uint8_t>(state_.pc));
        return false;
      }
      if (state_.tcu == 4) {
        // The status byte as it stands now: the break and interrupt flags move at
        // the end of the instruction, after the byte is already on the stack.
        bus.write(stackAddr(-2), state_.psw);
        return false;
      }
      if (state_.tcu == 5) return false;
      if (state_.tcu == 6) {
        state_.tmp = bus.read(callVector());
        return false;
      }
      state_.pc = static_cast<std::uint16_t>(
          state_.tmp | (bus.read(static_cast<std::uint16_t>(callVector() + 1)) << 8));
      state_.sp = static_cast<std::uint8_t>(state_.sp - 3);
      state_.psw = static_cast<std::uint8_t>((state_.psw | kFlagB) & ~kFlagI);
      return true;

    // ---- RET: a cycle inside the chip, then the return address off the stack, low
    //      byte first — the order it was pushed in ----
    case CtrlKind::Return:
      if (state_.tcu == 1) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu == 2) return false;
      if (state_.tcu == 3) {
        state_.tmp = bus.read(stackAddr(1));
        return false;
      }
      state_.pc = static_cast<std::uint16_t>(state_.tmp | (bus.read(stackAddr(2)) << 8));
      state_.sp = static_cast<std::uint8_t>(state_.sp + 2);
      return true;

    // ---- RETI: the same, with the status byte coming back first ----
    case CtrlKind::ReturnInterrupt:
      if (state_.tcu == 1) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu == 2) return false;
      if (state_.tcu == 3) {
        state_.ptr = bus.read(stackAddr(1));
        return false;
      }
      if (state_.tcu == 4) {
        state_.tmp = bus.read(stackAddr(2));
        return false;
      }
      state_.pc = static_cast<std::uint16_t>(state_.tmp | (bus.read(stackAddr(3)) << 8));
      state_.psw = static_cast<std::uint8_t>(state_.ptr);
      state_.sp = static_cast<std::uint8_t>(state_.sp + 3);
      return true;

    // ---- the stack transfers, which are mirror images: a push writes and then
    //      spends a cycle inside the chip, a pop spends the cycle and then reads ----
    case CtrlKind::Push:
      if (state_.tcu == 1) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu == 2) {
        bus.write(stackAddr(0), pushValue(state_.ir));
        return false;
      }
      --state_.sp;
      return true;

    case CtrlKind::Pop:
      if (state_.tcu == 1) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu == 2) return false;
      applyPop(state_.ir, bus.read(stackAddr(1)));
      ++state_.sp;
      return true;

    // ---- SLEEP and STOP: the byte after the opcode is read three times over, each
    //      read followed by a cycle inside the chip, and the core halts as the last
    //      of them ends — on an instruction boundary, with the program counter left
    //      on the byte it kept reading ----
    case CtrlKind::Halt:
      if (state_.tcu == 1 || state_.tcu == 3 || state_.tcu == 5) {
        discardNextByte(bus);
        return false;
      }
      if (state_.tcu != 6) return false;
      state_.run = state_.ir == 0xEF ? RunState::Sleeping : RunState::Stopped;
      return true;
  }
  return true;
}

template <ApuBus B>
bool Spc700::executeCycle(B& bus) {
  if (const WordForm word = wordForm(state_.ir); word != WordForm::None) {
    return executeWordCycle(bus, word);
  }
  if (MemForm form{}; memoryForm(state_.ir, form)) {
    return executeMemoryCycle(bus, form);
  }
  CtrlForm control{};
  static_cast<void>(controlForm(state_.ir, control));
  return executeControlCycle(bus, control);
}

template <ApuBus B>
void Spc700::stepCycle(B& bus) {
  // A sleeping or stopped core reaches memory not at all; the cycle still passes, and
  // the machine around it keeps time.
  if (state_.run != RunState::Running) return;

  if (atInstructionBoundary()) {
    state_.ir = fetch(bus);
    state_.tcu = 1;
    return;
  }

  state_.tcu = executeCycle(bus) ? std::uint8_t{0}
                                 : static_cast<std::uint8_t>(state_.tcu + 1);
}

template <ApuBus B>
std::uint32_t Spc700::stepInstruction(B& bus) {
  if (state_.run != RunState::Running) {
    stepCycle(bus);  // halted: two cycles that change nothing and reach nothing
    stepCycle(bus);
    return 2;
  }

  std::uint32_t cycles = 0;
  if (atInstructionBoundary()) {
    stepCycle(bus);  // the opcode fetch
    cycles = 1;
  }
  while (!atInstructionBoundary()) {
    stepCycle(bus);
    ++cycles;
  }
  return cycles;
}

}  // namespace snaggletooth
