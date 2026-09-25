#include "rom/cartridge_entries.h"

namespace snaggletooth::disasm {

std::vector<VectorEntry> vectorEntries(const CartridgeHeader& header) {
  std::vector<VectorEntry> entries;
  // The header declares the board, and a vector is fetched from bank $00 and
  // names a handler in bank $00 — whose upper half is ROM on every board and
  // whose lower half is the console's on every board.
  const CartridgeBoard board{.map = header.map, .coprocessor = header.coprocessor, .saveRamBytes = header.saveRamBytes};
  auto add = [&](std::uint16_t vector, std::string_view name) {
    const Address address = vector;
    if (cartridgeRegion(board, address) != CartridgeRegion::Rom) return;
    entries.push_back(VectorEntry{.address = address, .name = name});
  };
  add(header.emulation.reset, "reset");
  add(header.emulation.nmi, "nmi");
  add(header.emulation.irq, "irq");
  add(header.emulation.cop, "cop");
  add(header.emulation.abort, "abort");
  add(header.native.nmi, "nmi_native");
  add(header.native.irq, "irq_native");
  add(header.native.cop, "cop_native");
  add(header.native.brk, "brk_native");
  add(header.native.abort, "abort_native");
  return entries;
}

CodeOwner codeOwner(const CartridgeBoard& board, Address address) noexcept {
  return cartridgeRegion(board, address) == CartridgeRegion::Rom ? CodeOwner::Cpu65816
                                                                : CodeOwner::None;
}

}  // namespace snaggletooth::disasm
