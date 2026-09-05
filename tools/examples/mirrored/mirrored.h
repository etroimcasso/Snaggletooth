#pragma once

#include "examples/common.h"

namespace snaggletooth::examples {

// One bank. Reset switches to native mode and jumps long into bank $80 — the
// fast mirror of bank $00 under LoROM — where the rest of the program runs: a
// store, a read-modify-write and a stop, executed at `$80:8010` while the tree
// places the same bytes at `$00:8010`. Every site the tests name is commented
// with its address.
inline std::vector<std::uint8_t> mirroredImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x18u, 0xFBu,                // $8000 CLC / XCE       -> native, A8, X8
      0x5Cu, 0x10u, 0x80u, 0x80u,  // $8002 JML $80:8010    into the mirror bank
  });
  put(rom, 0x0010u, {
      0xA9u, 0x12u,                // $8010 LDA #$12        run as $80:8010
      0x8Du, 0x00u, 0x01u,         // $8012 STA !$0100
      0xEEu, 0x00u, 0x01u,         // $8015 INC !$0100
      0xDBu,                       // $8018 STP
  });
  return rom;
}

}  // namespace snaggletooth::examples
