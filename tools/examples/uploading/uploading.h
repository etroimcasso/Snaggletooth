#pragma once

#include <algorithm>

#include "examples/common.h"

namespace snaggletooth::examples {

// The 24-byte program the uploading cartridge sends: three instructions, sixteen
// NOPs (whose opcode is zero, the value cleared audio memory already holds) and
// STOP. It sits in the image at $80A0.
inline const std::vector<std::uint8_t> kUploadedProgram = {
    0xE8u, 0x5Au,         // MOV A,#$5A
    0xC5u, 0x50u, 0x02u,  // MOV !$0250,A
    0xE8u, 0x00u,         // MOV A,#$00
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,  // NOP x16
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0xFFu,                // STOP
};

// The 20 bytes the same cartridge sends after it, to the address right after
// the program, from a table elsewhere in the image ($8100).
inline const std::vector<std::uint8_t> kUploadedTable = {
    0x9Cu, 0x3Du, 0x71u, 0xE2u, 0x58u, 0xA7u, 0x06u, 0xB4u, 0xC9u, 0x1Fu,
    0x63u, 0x8Eu, 0x2Bu, 0xF5u, 0x4Au, 0xD0u, 0x17u, 0x86u, 0xEBu, 0x39u,
};

// One bank whose reset code speaks the upload protocol: waits for the ready
// bytes, sends the program to $0200 one acknowledged byte at a time, sends the
// table to $0218 the same way, starts the program, and jumps past both tables
// to stop — so the bank file's last piece, after the ranges the sound program
// takes, holds a line the first piece names. The two uploads land end to end
// in audio memory but come from two places in the image.
inline std::vector<std::uint8_t> uploadingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x78u,                     // 8000 SEI
      0x18u,                     // 8001 CLC
      0xFBu,                     // 8002 XCE
      0xE2u, 0x30u,              // 8003 SEP #$30
      0xADu, 0x40u, 0x21u,       // 8005 LDA $2140      ; ready?
      0xC9u, 0xAAu,              // 8008 CMP #$AA
      0xD0u, 0xF9u,              // 800A BNE $8005
      0xADu, 0x41u, 0x21u,       // 800C LDA $2141
      0xC9u, 0xBBu,              // 800F CMP #$BB
      0xD0u, 0xF2u,              // 8011 BNE $8005
      0xA9u, 0x00u,              // 8013 LDA #$00       ; destination $0200
      0x8Du, 0x42u, 0x21u,       // 8015 STA $2142
      0xA9u, 0x02u,              // 8018 LDA #$02
      0x8Du, 0x43u, 0x21u,       // 801A STA $2143
      0xA9u, 0x01u,              // 801D LDA #$01       ; a transfer, not a start
      0x8Du, 0x41u, 0x21u,       // 801F STA $2141
      0xA9u, 0xCCu,              // 8022 LDA #$CC       ; the kick
      0x8Du, 0x40u, 0x21u,       // 8024 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8027 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 802A BNE $8027
      0xA2u, 0x00u,              // 802C LDX #$00
      0xBDu, 0xA0u, 0x80u,       // 802E LDA $80A0,X    ; the program's next byte
      0x8Du, 0x41u, 0x21u,       // 8031 STA $2141
      0x8Au,                     // 8034 TXA            ; its index
      0x8Du, 0x40u, 0x21u,       // 8035 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8038 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 803B BNE $8038
      0xE8u,                     // 803D INX
      0xE0u, 0x18u,              // 803E CPX #$18
      0xD0u, 0xECu,              // 8040 BNE $802E
      0xA9u, 0x18u,              // 8042 LDA #$18       ; destination $0218
      0x8Du, 0x42u, 0x21u,       // 8044 STA $2142
      0xA9u, 0x02u,              // 8047 LDA #$02
      0x8Du, 0x43u, 0x21u,       // 8049 STA $2143
      0xA9u, 0x01u,              // 804C LDA #$01       ; a transfer
      0x8Du, 0x41u, 0x21u,       // 804E STA $2141
      0xA9u, 0x19u,              // 8051 LDA #$19       ; two past the last index
      0x8Du, 0x40u, 0x21u,       // 8053 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8056 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 8059 BNE $8056
      0xA2u, 0x00u,              // 805B LDX #$00
      0xBDu, 0x00u, 0x81u,       // 805D LDA $8100,X    ; the table's next byte
      0x8Du, 0x41u, 0x21u,       // 8060 STA $2141
      0x8Au,                     // 8063 TXA
      0x8Du, 0x40u, 0x21u,       // 8064 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8067 CMP $2140
      0xD0u, 0xFBu,              // 806A BNE $8067
      0xE8u,                     // 806C INX
      0xE0u, 0x14u,              // 806D CPX #$14
      0xD0u, 0xECu,              // 806F BNE $805D
      0xA9u, 0x00u,              // 8071 LDA #$00       ; start at $0200
      0x8Du, 0x42u, 0x21u,       // 8073 STA $2142
      0xA9u, 0x02u,              // 8076 LDA #$02
      0x8Du, 0x43u, 0x21u,       // 8078 STA $2143
      0xA9u, 0x00u,              // 807B LDA #$00       ; zero starts the program
      0x8Du, 0x41u, 0x21u,       // 807D STA $2141
      0xA9u, 0x15u,              // 8080 LDA #$15       ; two past the last index
      0x8Du, 0x40u, 0x21u,       // 8082 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8085 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 8088 BNE $8085
      0x4Cu, 0x20u, 0x81u,       // 808A JMP $8120      ; past both tables
  });
  std::copy(kUploadedProgram.begin(), kUploadedProgram.end(), rom.begin() + 0xA0);
  std::copy(kUploadedTable.begin(), kUploadedTable.end(), rom.begin() + 0x100);
  put(rom, 0x0120u, {0xDBu});  // 8120 STP
  return rom;
}

}  // namespace snaggletooth::examples
