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

  // The width directives an instruction needs before it: `A8` or `A16` and `X8`
  // or `X16`, each only where the width is known and the instruction above did
  // not already leave it so.
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

// The name of a hardware register at a 24-bit address, or an empty view when the
// address is ordinary memory. The registers sit at `$2100`-`$21FF` and
// `$4000`-`$43FF` of banks `$00`-`$3F` and `$80`-`$BF`; the same offsets in any
// other bank are memory and have no name.
[[nodiscard]] std::string_view cpu65816RegisterName(Address address);

}  // namespace snaggletooth::disasm
