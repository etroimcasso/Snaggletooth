#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A HiROM cartridge whose reset code sends thirty-two bytes from `$C1:FFF0` to
// VRAM and stops. A transfer's address steps within its bank, so the last
// sixteen bytes are read from `$C1:0000` — which under HiROM is the image, the
// start of the same bank — and the run records one range of thirty-two whose
// image offsets are two runs of sixteen. The asset pass lifts each run as a
// file of its own.
//
// Two banks. The code is at `$C0:8000`, which the CPU reaches as `$00:8000`
// from the reset vector.
inline std::vector<std::uint8_t> wrappingImage() {
  std::vector<std::uint8_t> rom = hiRomImage(2);
  put(rom, 0x8000u, {
      // ---- channel 0: the last sixteen bytes of bank $C1 and then its first sixteen ----
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x43u,       // $8000 DMAP0 = $01: A->B, increment, pattern 1
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,       // $8005 BBAD0 = $18: VMDATAL
      0xA9u, 0xF0u, 0x8Du, 0x02u, 0x43u,       // $800A A1T0 low
      0xA9u, 0xFFu, 0x8Du, 0x03u, 0x43u,       // $800F A1T0 high: $FFF0
      0xA9u, 0xC1u, 0x8Du, 0x04u, 0x43u,       // $8014 A1B0 = $C1
      0xA9u, 0x20u, 0x8Du, 0x05u, 0x43u,       // $8019 DAS0 low = 32
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,       // $801E DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,       // $8023 MDMAEN = $01 (the write at $8025)
      0xDBu,                                   // $8028 STP
  });
  for (std::size_t i = 0; i < 16; ++i) rom[0x1FFF0u + i] = static_cast<std::uint8_t>(0xA0u + i);  // $C1:FFF0: the bytes before the wrap
  for (std::size_t i = 0; i < 16; ++i) rom[0x10000u + i] = static_cast<std::uint8_t>(0xB0u + i);  // $C1:0000: the bytes after it
  return rom;
}

}  // namespace snaggletooth::examples
