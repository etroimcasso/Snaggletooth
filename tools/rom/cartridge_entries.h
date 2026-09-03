#pragma once

// Where a whole-cartridge disassembly starts, and which chip's disassembler owns
// each part of the bus.
//
// A cartridge's header names the handlers the CPU jumps to on reset and on every
// interrupt. Those are the addresses execution provably reaches before the
// program has run a single instruction, so they are the entry points a trace of
// the cartridge begins from. Everything else a trace reaches, it reaches from
// them.

#include <cstdint>
#include <string_view>
#include <vector>

#include "disasm/disasm.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {

// One entry point a cartridge header names: the handler's address in bank $00
// and the vector it came from.
struct VectorEntry {
  Address address = 0;
  std::string_view name;  // "reset", "nmi", "irq", "cop", "brk", "abort", with "_native" on the native set
};

// The entry points the header names that land in ROM under the header's map, in
// vector-table order: the emulation-mode set first, reset at its head, then the
// native set. A vector pointing outside ROM is left out — nothing at that address
// can be traced — so a cartridge that leaves a vector unused contributes no entry
// for it. Two vectors naming one handler give two entries, each under its own
// name.
[[nodiscard]] std::vector<VectorEntry> vectorEntries(const CartridgeHeader& header);

// The disassembler that owns the bytes at an address.
enum class CodeOwner : std::uint8_t {
  None,      // nothing to disassemble: RAM, registers, a save, or open bus
  Cpu65816,  // the main CPU's instruction set
};

// Which disassembler owns the bytes at `address` under `map`. Cartridge ROM is the
// main CPU's; everything else holds no code an image can be read for.
[[nodiscard]] CodeOwner codeOwner(CartridgeMap map, Address address) noexcept;

}  // namespace snaggletooth::disasm
