#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// A cartridge that starts a DMA transfer and takes an indirect jump on the very
// next instruction: the CPU is held off the bus while the transfer runs, so the
// step that meets the jump may run nothing at all before the jump does.
inline std::vector<std::uint8_t> stalledJumpImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0100u, {0x20u, 0x82u});          // $8100: -> $8220
  put(rom, 0x0000u, {
                        0x18u, 0xFBu,                // $8000 CLC / XCE
                        0xE2u, 0x30u,                // $8002 SEP #$30
                        0xA9u, 0x00u,                // $8004 LDA #$00
                        0x8Du, 0x00u, 0x43u,         // $8006 STA !$4300   DMAP0: A to B, one register
                        0xA9u, 0x80u,                // $8009 LDA #$80
                        0x8Du, 0x01u, 0x43u,         // $800B STA !$4301   BBAD0: WMDATA
                        0x9Cu, 0x02u, 0x43u,         // $800E STZ !$4302   A1T0L
                        0xA9u, 0x80u,                // $8011 LDA #$80
                        0x8Du, 0x03u, 0x43u,         // $8013 STA !$4303   A1T0H: source $00:8000
                        0x9Cu, 0x04u, 0x43u,         // $8016 STZ !$4304   A1B0
                        0x9Cu, 0x05u, 0x43u,         // $8019 STZ !$4305   DAS0L
                        0xA9u, 0x10u,                // $801C LDA #$10
                        0x8Du, 0x06u, 0x43u,         // $801E STA !$4306   DAS0H: 4096 bytes
                        0xA9u, 0x01u,                // $8021 LDA #$01
                        0x8Du, 0x0Bu, 0x42u,         // $8023 STA !$420B   MDMAEN: go
                        0x6Cu, 0x00u, 0x81u,         // $8026 JMP (!$8100) -> $8220, the CPU stalled
                    });
  put(rom, 0x0220u, {0xDBu});                        // $8220 STP
  return rom;
}

}  // namespace snaggletooth::examples
