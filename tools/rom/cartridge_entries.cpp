#include "rom/cartridge_entries.h"

namespace snaggletooth::disasm {

std::vector<VectorEntry> vectorEntries(const CartridgeHeader& header) {
  std::vector<VectorEntry> entries;
  auto add = [&](std::uint16_t vector, std::string_view name) {
    // A vector is fetched from bank $00 and names a handler in bank $00.
    const Address address = vector;
    if (cartridgeRegion(header.map, address) != CartridgeRegion::Rom) return;
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

CodeOwner codeOwner(CartridgeMap map, Address address) noexcept {
  return cartridgeRegion(map, address) == CartridgeRegion::Rom ? CodeOwner::Cpu65816
                                                              : CodeOwner::None;
}

}  // namespace snaggletooth::disasm
