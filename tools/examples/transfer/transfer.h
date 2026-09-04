#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// Reset sets up a transfer of sixteen bytes from the image into work RAM
// through the data port, triggers it, runs a few instructions while and after
// the engine holds the bus, and stops.
inline std::vector<std::uint8_t> transferImage() {
  std::vector<std::uint8_t> rom = imageWithNmi();
  put(rom, 0x0000u, {
      0xA9u, 0x00u, 0x8Du, 0x00u, 0x43u,   // $8000 DMAP0 = $00: A->B, increment, one register
      0xA9u, 0x80u, 0x8Du, 0x01u, 0x43u,   // $8005 BBAD0 = $80: $2180
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,   // $800A A1T0 low
      0xA9u, 0x81u, 0x8Du, 0x03u, 0x43u,   // $800F A1T0 high: $8100
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,   // $8014 A1B0 = $00
      0xA9u, 0x10u, 0x8Du, 0x05u, 0x43u,   // $8019 DAS0 low = 16
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,   // $801E DAS0 high
      0xA9u, 0x00u, 0x8Du, 0x81u, 0x21u,   // $8023 WMADDL = 0
      0xA9u, 0x10u, 0x8Du, 0x82u, 0x21u,   // $8028 WMADDM = $10: the port at $7E:1000
      0xA9u, 0x00u, 0x8Du, 0x83u, 0x21u,   // $802D WMADDH = 0
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,   // $8032 MDMAEN = 1: go
      0xEAu, 0xEAu,                        // $8037 NOP ; NOP  (the engine engages inside the first)
      0xADu, 0x00u, 0x10u,                 // $8039 LDA !$1000  the first byte, now in work RAM
      0xDBu,                               // $803C STP
  });
  for (std::size_t i = 0; i < 16; ++i) rom[0x0100u + i] = static_cast<std::uint8_t>(0xC0u + i);
  return rom;
}

}  // namespace snaggletooth::examples
