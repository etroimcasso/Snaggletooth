#pragma once

// The 65816 disassembler — the 65816 backend over the disassembly framework
// (`disasm/disasm.h`), and the calls that use it without naming the backend.
//
// The instruction set has one property no other chip in the machine has: an
// instruction's length depends on state. The accumulator and the index registers
// are each 8 or 16 bits wide under two flags, and an immediate operand is as wide
// as the register it loads — so `LDA #` is two bytes or three depending on every
// `REP` and `SEP` on the path that reached it, and emulation mode forces both
// widths to eight. The backend carries those flags in the trace context, moves
// them through the instructions that change them, and where a path reaches an
// operand under a width it does not know, says so and stops rather than guess.
//
// Its cycle counts come from the interpreter, not from a table beside it. Each
// opcode's cost is measured by running the core over a synthetic bus under every
// combination of the mode flags, so the listing and the emulator cannot disagree
// about what an instruction costs under the flags at that address. The eight
// conditional branches are measured both ways and print as `base/taken`.
//
// It reads a raw image with a 24-bit load address, so it serves one bank of a
// cartridge, a block copied into work RAM, or a whole mapped image alike.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "disasm/disasm.h"

namespace snaggletooth::disasm {

// The state the 65816 backend carries beside every address: the mode flags that
// decide how the bytes there read, and one instruction's worth of memory about
// the carry, which is what `XCE` exchanges the emulation flag with.
//
// A width the trace does not know is marked unknown rather than assumed. The
// reset handler starts in emulation mode with both widths eight; an interrupt
// handler starts in native mode with whatever widths the interrupted code had,
// which is nothing the image can say.
struct Cpu65816Mode {
  bool emulation = true;         // e: emulation mode, which forces both widths to 8
  bool accumulator8 = true;      // m: an 8-bit accumulator and memory operand
  bool index8 = true;            // x: 8-bit index registers
  bool accumulatorKnown = true;  // whether `accumulator8` is known at this address
  bool indexKnown = true;        // whether `index8` is known at this address
  bool carryKnown = false;       // whether the carry was set by the instruction before
  bool carry = false;            // its value, when it was

  friend bool operator==(const Cpu65816Mode&, const Cpu65816Mode&) = default;

  // Where the reset vector starts: emulation mode, both widths eight.
  [[nodiscard]] static constexpr Cpu65816Mode reset() noexcept { return {}; }

  // Native mode with both widths known.
  [[nodiscard]] static constexpr Cpu65816Mode native(bool accumulator8, bool index8) noexcept {
    return {.emulation = false,
            .accumulator8 = accumulator8,
            .index8 = index8,
            .accumulatorKnown = true,
            .indexKnown = true,
            .carryKnown = false,
            .carry = false};
  }

  // Native mode with neither width known — where an interrupt handler starts.
  [[nodiscard]] static constexpr Cpu65816Mode nativeUnknown() noexcept {
    return {.emulation = false,
            .accumulator8 = true,
            .index8 = true,
            .accumulatorKnown = false,
            .indexKnown = false,
            .carryKnown = false,
            .carry = false};
  }
};

// A mode as the framework carries it, and back. Emulation mode packs with both
// widths eight and known, and an unknown width packs as eight, so two modes that
// read the bytes the same way pack to the same bits.
[[nodiscard]] Context contextOf(const Cpu65816Mode& mode) noexcept;
[[nodiscard]] Cpu65816Mode modeOf(Context context) noexcept;

// The mode execution carries out of an instruction, given the mode it ran under
// and its first operand byte (the mask of `REP` and `SEP`). Only a handful of
// instructions move the flags: `REP` and `SEP` move the widths they name and make
// them known; `PLP` and `RTI` make both unknown; `XCE` exchanges the carry with
// the emulation flag when `CLC` or `SEC` was the instruction before, and
// otherwise keeps the mode and says so in `note`. Every instruction but `CLC`
// and `SEC` forgets what was known about the carry. Under emulation both widths
// are eight and known after every instruction, `PLP` and `RTI` included, since
// the flag forces them and nothing but `XCE` moves the flag. The disassembler and
// the assembler both follow the widths through this one function.
[[nodiscard]] Cpu65816Mode cpu65816ModeAfter(std::uint8_t opcode, std::uint8_t operand,
                                             const Cpu65816Mode& mode, std::string& note);

// How an instruction's operand bytes are laid out and how they print. The
// mnemonic is separate, so every instruction sharing a shape shares a value here
// however differently it is named.
enum class Cpu65816Addressing : std::uint8_t {
  Implied,              // no operand
  Accumulator,          // `A`
  ImmediateM,           // `#imm`, one byte or two by the accumulator width
  ImmediateX,           // `#imm`, one byte or two by the index width
  ImmediateByte,        // `#imm`, always one byte: REP, SEP, WDM, and the BRK/COP signature
  Direct,               // `dp`
  DirectX,              // `dp,X`
  DirectY,              // `dp,Y`
  DirectIndirect,       // `(dp)`
  DirectIndirectX,      // `(dp,X)`
  DirectIndirectY,      // `(dp),Y`
  DirectIndirectLong,   // `[dp]`
  DirectIndirectLongY,  // `[dp],Y`
  StackRelative,        // `sr,S`
  StackRelativeY,       // `(sr,S),Y`
  Absolute,             // `!abs`
  AbsoluteX,            // `!abs,X`
  AbsoluteY,            // `!abs,Y`
  AbsoluteLong,         // `$bb:hhll`
  AbsoluteLongX,        // `$bb:hhll,X`
  AbsoluteIndirect,     // `(!abs)`, a pointer in bank zero
  AbsoluteIndirectLong, // `[!abs]`, a three-byte pointer in bank zero
  AbsoluteIndexedIndirect,  // `(!abs,X)`, a pointer in the program bank
  Relative,             // an 8-bit displacement, printed as the address it reaches
  RelativeLong,         // a 16-bit displacement, printed the same way
  BlockMove,            // two bank bytes: the destination first, then the source
  PushAbsolute,         // PEA: a 16-bit value pushed as it is
  PushRelative,         // PER: a 16-bit displacement, pushed as the address it names
};

// How many operand bytes an addressing mode carries. The two immediates whose
// width follows a flag answer for that flag's setting.
[[nodiscard]] constexpr std::uint8_t cpu65816OperandBytes(Cpu65816Addressing mode,
                                                          bool accumulator8,
                                                          bool index8) noexcept {
  switch (mode) {
    case Cpu65816Addressing::Implied:
    case Cpu65816Addressing::Accumulator:
      return 0;
    case Cpu65816Addressing::ImmediateM:
      return accumulator8 ? 1 : 2;
    case Cpu65816Addressing::ImmediateX:
      return index8 ? 1 : 2;
    case Cpu65816Addressing::ImmediateByte:
    case Cpu65816Addressing::Direct:
    case Cpu65816Addressing::DirectX:
    case Cpu65816Addressing::DirectY:
    case Cpu65816Addressing::DirectIndirect:
    case Cpu65816Addressing::DirectIndirectX:
    case Cpu65816Addressing::DirectIndirectY:
    case Cpu65816Addressing::DirectIndirectLong:
    case Cpu65816Addressing::DirectIndirectLongY:
    case Cpu65816Addressing::StackRelative:
    case Cpu65816Addressing::StackRelativeY:
    case Cpu65816Addressing::Relative:
      return 1;
    case Cpu65816Addressing::Absolute:
    case Cpu65816Addressing::AbsoluteX:
    case Cpu65816Addressing::AbsoluteY:
    case Cpu65816Addressing::AbsoluteIndirect:
    case Cpu65816Addressing::AbsoluteIndirectLong:
    case Cpu65816Addressing::AbsoluteIndexedIndirect:
    case Cpu65816Addressing::RelativeLong:
    case Cpu65816Addressing::BlockMove:
    case Cpu65816Addressing::PushAbsolute:
    case Cpu65816Addressing::PushRelative:
      return 2;
    case Cpu65816Addressing::AbsoluteLong:
    case Cpu65816Addressing::AbsoluteLongX:
      return 3;
  }
  return 0;
}

// One row of the instruction table: the opcode, its mnemonic, its addressing
// mode, and how execution leaves it. The same table decodes an instruction and
// encodes one, so the two cannot disagree.
struct Cpu65816Opcode {
  std::uint8_t opcode = 0;
  const char* mnemonic = "";
  Cpu65816Addressing mode = Cpu65816Addressing::Implied;
  Flow flow = Flow::Continue;
};

// The instruction table, indexed by opcode, in the order of the datasheet's
// opcode matrix.
[[nodiscard]] const std::array<Cpu65816Opcode, 256>& cpu65816Opcodes();

// The 65816 backend. Addresses are 24 bits; the context is a `Cpu65816Mode`.
class Cpu65816Backend final : public Backend {
 public:
  [[nodiscard]] std::string_view name() const override { return "65816"; }
  [[nodiscard]] unsigned addressBits() const override { return 24; }
  [[nodiscard]] std::optional<Decoded> decode(std::span<const std::uint8_t> image, Address base,
                                              Address at, Context context) const override;
  [[nodiscard]] std::string_view registerName(Address address) const override;

  // An immediate operand under a register width the context does not know
  // cannot be read: the reason names the instruction and the width. Everything
  // else reads.
  [[nodiscard]] std::string unreadable(std::span<const std::uint8_t> image, Address base,
                                       Address at, Context context) const override;

  // A mode as a warning names it: `e=0 m=16 x=8`, with `?` for a width the trace
  // does not know.
  [[nodiscard]] std::string describe(Context context) const override;

  // Two contexts conflict when they differ in a flag that decides a reading — the
  // emulation flag and the two widths. What is known about the carry is not one.
  [[nodiscard]] bool conflicts(Context first, Context second) const override;

  // The directives an instruction needs before it, and only those: `EMULATION`
  // where it reads in emulation mode and the instruction above did not leave the
  // chip there — or there is no instruction above, since a region begins native;
  // `NATIVE` where the instruction above left emulation mode and this one reads
  // native; and in native mode `A8` or `A16` and `X8` or `X16`, each only where
  // the width is known and the instruction above did not already leave it so.
  // Emulation mode forces both widths, so no width directive follows `EMULATION`.
  [[nodiscard]] std::vector<std::string> directives(std::optional<Context> before,
                                                    Context now) const override;
};

// The one 65816 backend; it holds no state.
[[nodiscard]] const Cpu65816Backend& cpu65816Backend();

// Decodes the single instruction at `address` under `mode`. Returns nothing when
// the address lies outside the image, when the instruction's operand bytes would
// run past its end, or when `mode` does not settle the operand's width.
[[nodiscard]] std::optional<Instruction> decodeAt(std::span<const std::uint8_t> image,
                                                  Address base, Address address,
                                                  const Cpu65816Mode& mode);

// The measured cost of every opcode under one setting of the mode flags, indexed
// by opcode. Measured once per setting, by running the interpreter — see the file
// comment. Emulation mode forces both widths, so `accumulator8` and `index8` are
// ignored when `emulation` is set. The measurement assumes what the datasheet's
// own table assumes: a direct register with a zero low byte and no page crossing.
[[nodiscard]] const std::array<CycleCost, 256>& cpu65816CycleTable(bool emulation,
                                                                    bool accumulator8,
                                                                    bool index8);

// What part of the machine a hardware register belongs to. The class is a fact
// about the register, so it is authored from the same staged tables the name is
// and lives beside it; a report says what a routine reaches without its reader
// having to know all two hundred names.
enum class RegisterClass : std::uint8_t {
  Display,     // brightness, forced blank, screen composition, the beam counters and status
  Background,  // the four background layers: mode, tilemaps, character bases, scroll, mosaic
  Vram,        // the video RAM port: address, increment mode, data
  Cgram,       // the palette port: address and data
  Oam,         // the sprite table port, and sprite size and character base
  Mode7,       // the Mode 7 matrix, centre and settings
  Window,      // the two windows: positions, per-layer enable, mask logic
  ColorMath,   // colour addition and subtraction, and the fixed colour
  Apu,         // the four ports the CPU talks to the audio unit through
  WramPort,    // the work-RAM port: address and data
  Joypad,      // the controller ports, both serial and auto-read
  Interrupt,   // NMI and IRQ enable, the timer targets, and the flags they raise
  Math,        // the multiplier and the divider, and the multiplication result
  DmaControl,  // the two registers that start a transfer
  DmaChannel,  // one of the eight channels' own registers
  Io,          // the general-purpose I/O bits of the controller ports
  Speed,       // the FastROM enable
};

// A class as a manifest and a report name one: the enumerator's own spelling.
[[nodiscard]] std::string_view cpu65816RegisterClassName(RegisterClass cls);

// A hardware register: its name, the part of the machine it belongs to, and
// whether the CPU reads it, writes it, or both. Authored from the staged MMIO
// register table — its Type column is what `reads` and `writes` say — and from
// the PPU registers page, whose sections are what `cls` says. Never from the
// core's own handling of an address, so the table and the machine stay two
// readings of one document rather than one copied from the other.
struct Cpu65816Register {
  std::string_view name;
  RegisterClass cls = RegisterClass::Display;
  bool reads = false;
  bool writes = false;
};

// The register at a 24-bit address, or nothing when the address is ordinary
// memory. The registers sit at `$2100`-`$21FF` and `$4000`-`$43FF` of banks
// `$00`-`$3F` and `$80`-`$BF`; the same offsets in any other bank are memory.
[[nodiscard]] std::optional<Cpu65816Register> cpu65816Register(Address address);

// The name of a hardware register at a 24-bit address, or an empty view when the
// address is ordinary memory. The same table `cpu65816Register` answers from, so
// a name and a class can never disagree about which addresses are registers.
[[nodiscard]] std::string_view cpu65816RegisterName(Address address);

}  // namespace snaggletooth::disasm
