#pragma once

// The NEC µPD77C25 — the signal processor inside the DSP-1 family of SNES cartridges.
//
// The core runs one instruction a chip cycle: stepCycle() fetches the word the program
// counter names, advances the counter and executes the word whole. No instruction spans
// two cycles, so the state between any two cycles is the whole machine, and a snapshot
// is a plain copy of Upd77c25State.
//
// The program is a constant the host builds and hands over: 2048 instruction words and
// 1024 data words, held outside the state. The core keeps a pointer to it, so the
// program outlives the core.
//
// The chip has no external bus. A SNES cartridge reaches it through two ports: the data
// register and the status register, selected by the cartridge's A0 line. hostRead() and
// hostWrite() are those two ports, with the handshake fullsnes gives for them. The serial
// port, the interrupt input, the DMA request line and the P0/P1 outputs are unconnected
// on every SNES board: the serial inputs read zero, the serial outputs vanish, the serial
// acknowledges stay 0, and no interrupt is ever raised.
//
// The contract is fullsnes, "SNES Cart DSP-n/ST010/ST011 - NEC uPD77C25". Where it is
// the only source for a behavior, the code says "fullsnes alone" at the place the
// behavior is decided.

#include <array>
#include <cstdint>

namespace snaggletooth {

// The program the core runs: the instruction ROM and the data ROM, as values. Each
// instruction word is 24 bits, held in the low bits of 32; the top byte is ignored.
struct Upd77c25Program {
  std::array<std::uint32_t, 2048> code{};
  std::array<std::uint16_t, 1024> data{};
};

// The whole chip state as a value: snapshot by copy, restore by assignment. The default
// is all zero, the state a power-on starts from.
struct Upd77c25State {
  // One accumulator's flags, packed into a byte in fullsnes's order.
  static constexpr std::uint8_t kFlagS1 = 0x20;   // the sign of the last overflow
  static constexpr std::uint8_t kFlagS0 = 0x10;   // the result's bit 15
  static constexpr std::uint8_t kFlagC = 0x08;    // carry, or borrow on a subtraction
  static constexpr std::uint8_t kFlagZ = 0x04;    // the result is zero
  static constexpr std::uint8_t kFlagOv1 = 0x02;  // an odd number of overflows
  static constexpr std::uint8_t kFlagOv0 = 0x01;  // the last operation overflowed

  // The status register's bits. RQM and DRS are the chip's own: a program writing the
  // register leaves them as they are, and bits 6 to 2 always read zero.
  static constexpr std::uint16_t kSrRqm = 0x8000;   // the chip waits on the host's DR access
  static constexpr std::uint16_t kSrUsf1 = 0x4000;  // a flag for the program's own use
  static constexpr std::uint16_t kSrUsf0 = 0x2000;  // a flag for the program's own use
  static constexpr std::uint16_t kSrDrs = 0x1000;   // the host's 16-bit DR access is half done
  static constexpr std::uint16_t kSrDma = 0x0800;   // DMA mode; nothing on a SNES board answers
  static constexpr std::uint16_t kSrDrc = 0x0400;   // DR is 8 bits wide (clear: 16 bits)
  static constexpr std::uint16_t kSrSoc = 0x0200;   // the serial output's width
  static constexpr std::uint16_t kSrSic = 0x0100;   // the serial input's width
  static constexpr std::uint16_t kSrEi = 0x0080;    // interrupts enabled; none ever arrives
  static constexpr std::uint16_t kSrP1 = 0x0002;    // the P1 output pin
  static constexpr std::uint16_t kSrP0 = 0x0001;    // the P0 output pin

  std::uint32_t product = 0;  // K·L·2, as last computed
  std::uint16_t pc = 0;       // 11 bits: the next instruction word
  std::uint16_t rp = 0;       // 10 bits: the data ROM pointer
  std::array<std::uint16_t, 4> stack{};  // the return addresses, 11 bits each
  std::uint16_t k = 0;        // the multiplier's first factor
  std::uint16_t l = 0;        // the multiplier's second factor
  std::uint16_t accA = 0;
  std::uint16_t accB = 0;
  std::uint16_t tr = 0;       // temporary register A
  std::uint16_t trb = 0;      // temporary register B
  std::uint16_t sr = 0;       // the status register
  std::uint16_t dr = 0;       // the data register the host reads and writes
  std::array<std::uint16_t, 256> ram{};  // the data RAM
  std::uint8_t dp = 0;        // the data RAM pointer
  std::uint8_t sp = 0;        // 2 bits: the stack entry the next call writes
  std::uint8_t flagA = 0;     // AccA's flags
  std::uint8_t flagB = 0;     // AccB's flags

  [[nodiscard]] bool operator==(const Upd77c25State&) const noexcept = default;
};

// The ALU word's P field: the operand the operation takes besides the accumulator.
enum class Upd77c25Operand : std::uint8_t {
  Ram = 0,          // RAM[DP]
  Bus = 1,          // the value the SRC field put on the internal bus
  ProductHigh = 2,  // K·L·2's high word
  ProductLow = 3,   // K·L·2's low word
};

// The ALU word's operation field.
enum class Upd77c25AluOp : std::uint8_t {
  Nop = 0x0,
  Or = 0x1,
  And = 0x2,
  Xor = 0x3,
  Sub = 0x4,
  Add = 0x5,
  Sbb = 0x6,   // subtract the operand and the other accumulator's carry
  Adc = 0x7,   // add the operand and the other accumulator's carry
  Dec = 0x8,
  Inc = 0x9,
  Not = 0xA,
  Sar1 = 0xB,  // shift right one, the sign kept
  Rcl1 = 0xC,  // shift left one, the other accumulator's carry shifted in
  Sll2 = 0xD,  // shift left two, ones shifted in
  Sll4 = 0xE,  // shift left four, ones shifted in
  Xchg = 0xF,  // swap the two bytes
};

// The ALU word's SRC field: what the instruction moves, and puts on the internal bus.
enum class Upd77c25Source : std::uint8_t {
  Trb = 0x0,
  AccA = 0x1,
  AccB = 0x2,
  Tr = 0x3,
  Dp = 0x4,
  Rp = 0x5,
  Ro = 0x6,    // the data ROM word at RP
  Sgn = 0x7,   // 8000h less AccA's S1 flag
  Dr = 0x8,    // the data register; sets RQM
  Drnf = 0x9,  // the data register, RQM left alone
  Sr = 0xA,
  Sim = 0xB,   // the serial input; reads zero on a SNES board
  Sil = 0xC,   // the serial input; reads zero on a SNES board
  K = 0xD,
  L = 0xE,
  Mem = 0xF,   // RAM[DP]
};

// The DST field of an ALU or load word: where the moved value lands.
enum class Upd77c25Destination : std::uint8_t {
  None = 0x0,
  AccA = 0x1,
  AccB = 0x2,
  Tr = 0x3,
  Dp = 0x4,
  Rp = 0x5,
  Dr = 0x6,   // the data register; sets RQM
  Sr = 0x7,
  Sol = 0x8,  // the serial output; the value goes nowhere on a SNES board
  Som = 0x9,  // the serial output; the value goes nowhere on a SNES board
  K = 0xA,
  Klr = 0xB,  // K takes the value and L the data ROM word at RP
  Klm = 0xC,  // L takes the value and K takes RAM[DP | 40h]
  L = 0xD,
  Trb = 0xE,
  Mem = 0xF,  // RAM[DP]
};

// The jump word's BRCH field: every value the µPD77C25 defines, named by its mnemonic.
// A conditional jump tests one flag of AccA's or AccB's, the low nibble of DP, a serial
// acknowledge or RQM. Any other value, the three only the ST010 and ST011 define among
// them, is a jump that is never taken.
enum class Upd77c25Branch : std::uint16_t {
  Jmp = 0x100,
  Call = 0x140,
  Jnca = 0x080,  // AccA's carry clear
  Jca = 0x082,
  Jncb = 0x084,
  Jcb = 0x086,
  Jnza = 0x088,  // AccA's zero flag clear
  Jza = 0x08A,
  Jnzb = 0x08C,
  Jzb = 0x08E,
  Jnova0 = 0x090,  // AccA's OV0 clear
  Jova0 = 0x092,
  Jnovb0 = 0x094,
  Jovb0 = 0x096,
  Jnova1 = 0x098,  // AccA's OV1 clear
  Jova1 = 0x09A,
  Jnovb1 = 0x09C,
  Jovb1 = 0x09E,
  Jnsa0 = 0x0A0,  // AccA's S0 clear
  Jsa0 = 0x0A2,
  Jnsb0 = 0x0A4,
  Jsb0 = 0x0A6,
  Jnsa1 = 0x0A8,  // AccA's S1 clear
  Jsa1 = 0x0AA,
  Jnsb1 = 0x0AC,
  Jsb1 = 0x0AE,
  Jdpl0 = 0x0B0,   // DP's low nibble is 0
  Jdpln0 = 0x0B1,  // DP's low nibble is not 0
  Jdplf = 0x0B2,   // DP's low nibble is Fh
  Jdplnf = 0x0B3,  // DP's low nibble is not Fh
  Jnsiak = 0x0B4,  // the serial input's acknowledge clear: always, on a SNES board
  Jsiak = 0x0B6,
  Jnsoak = 0x0B8,  // the serial output's acknowledge clear: always, on a SNES board
  Jsoak = 0x0BA,
  Jnrqm = 0x0BC,  // RQM clear
  Jrqm = 0x0BE,
};

class Upd77c25 {
 public:
  // The core runs `program`, which it does not copy and which must outlive it.
  explicit Upd77c25(const Upd77c25Program& program, Upd77c25State state = {}) noexcept
      : program_(&program), state_(state) {}
  Upd77c25(const Upd77c25Program&& program, Upd77c25State state = {}) = delete;

  [[nodiscard]] const Upd77c25State& state() const noexcept { return state_; }
  void restore(const Upd77c25State& state) noexcept { state_ = state; }

  // The RST line: PC 0, both accumulators' flags 0, SR 0 (DR 16 bits wide, RQM and DRS
  // clear), RP 3FFh. Every other register and the data RAM keep their values.
  void reset() noexcept;

  // Executes one chip cycle, which is one whole instruction.
  void stepCycle() noexcept;

  // The host's read. With `status` set it reads SR's high byte, which changes nothing.
  // Otherwise it reads DR: in 8-bit mode its low byte, completing the access; in 16-bit
  // mode the low byte first and the high byte second, DRS set between the two, the
  // second completing the access. A completed access clears RQM.
  [[nodiscard]] std::uint8_t hostRead(bool status) noexcept;

  // The host's write to DR, by the same fragments and the same completion as a read.
  // SR is read-only to the host, so a write with A0 set reaches nothing on the chip.
  void hostWrite(std::uint8_t value) noexcept;

  // RQM: the program has read or written DR and waits on the host to complete its access.
  [[nodiscard]] bool requestingHost() const noexcept {
    return (state_.sr & Upd77c25State::kSrRqm) != 0;
  }

 private:
  void executeAlu(std::uint32_t word) noexcept;
  void executeJump(std::uint32_t word) noexcept;
  void executeLoad(std::uint32_t word) noexcept;

  [[nodiscard]] std::uint16_t readSource(Upd77c25Source source) noexcept;
  // Returns whether the write reached K or L.
  bool writeDestination(Upd77c25Destination destination, std::uint16_t value) noexcept;
  [[nodiscard]] bool branchTaken(std::uint16_t branch) const noexcept;
  void updateProduct() noexcept;
  void programTouchedDataRegister() noexcept;
  void push(std::uint16_t address) noexcept;
  [[nodiscard]] std::uint16_t pop() noexcept;

  const Upd77c25Program* program_;
  Upd77c25State state_;
};

}  // namespace snaggletooth
