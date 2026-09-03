#pragma once

// Cartridge images built by hand for the tests of the cartridge tools: a LoROM
// header, a few banks of 65816 code that call and jump across them, and one
// cartridge whose reset code speaks the audio upload protocol. Shared by the
// disassembly and the verification tests, so both read one tree.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace snaggletooth::disasm::fixtures {

// A LoROM image of `banks` 32 KB banks whose header names reset at $8000 and
// leaves every other vector unused.
inline std::vector<std::uint8_t> loRomImage(std::size_t banks) {
  std::vector<std::uint8_t> rom(banks * 0x8000u, 0u);
  const std::size_t site = 0x7FC0u;
  for (std::size_t i = 0; i < 21; ++i) rom[site + i] = 'A';
  rom[site + 0x15] = 0x20u;  // the map-mode byte: LoROM
  rom[site + 0x1C] = 0x34u;  // a complement and checksum that agree
  rom[site + 0x1D] = 0x12u;
  rom[site + 0x1E] = 0xCBu;
  rom[site + 0x1F] = 0xEDu;
  rom[site + 0x3C] = 0x00u;  // reset -> $8000
  rom[site + 0x3D] = 0x80u;
  return rom;
}

inline void put(std::vector<std::uint8_t>& rom, std::size_t offset,
                std::initializer_list<std::uint8_t> bytes) {
  std::size_t i = offset;
  for (const std::uint8_t b : bytes) rom[i++] = b;
}

// Three banks. Reset switches to native mode with both widths sixteen, calls a
// routine in bank $01, and jumps to bank $02 through its mirror at $82. The
// bank-$01 routine calls back into bank $00, at an address only it reaches. The
// bank-$02 code narrows the accumulator, calls into work RAM, and ends in a
// jump through a table; the table's one target sits at $02:8100.
inline std::vector<std::uint8_t> threeBankImage() {
  std::vector<std::uint8_t> rom = loRomImage(3);
  put(rom, 0x0000u, {0x18u,                       // CLC
                     0xFBu,                       // XCE
                     0xC2u, 0x30u,                // REP #$30
                     0x22u, 0x00u, 0x80u, 0x01u,  // JSL $01:8000
                     0x5Cu, 0x00u, 0x80u, 0x82u});  // JML $82:8000
  put(rom, 0x0100u, {0xEAu,    // $00:8100 NOP   (reached only from bank $01)
                     0x6Bu});  //          RTL
  put(rom, 0x8000u, {0xA9u, 0x34u, 0x12u,          // LDA #$1234
                     0x22u, 0x00u, 0x81u, 0x00u,   // JSL $00:8100
                     0x6Bu});                      // RTL
  put(rom, 0x10000u, {0xE2u, 0x20u,                // SEP #$20
                      0xA9u, 0x12u,                // LDA #$12
                      0x22u, 0x00u, 0x20u, 0x7Eu,  // JSL $7E:2000
                      0x7Cu, 0x00u, 0x81u});       // JMP (!$8100,X)
  put(rom, 0x10100u, {0xEAu,          // NOP        (reached only through the table)
                      0x00u, 0x01u,   // BRK #$01   (continues at the BRK vector's handler)
                      0x6Bu});        // RTL
  return rom;
}

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

// One bank whose header names the COP vector and the native BRK vector beside
// reset: `cop` and `brk` are mnemonics, so their handlers cannot carry the
// vector's name as a label. Reset stops at once; each handler returns.
inline std::vector<std::uint8_t> copVectorImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x34] = 0x10u;  // cop (emulation) -> $8010
  rom[site + 0x35] = 0x80u;
  rom[site + 0x26] = 0x12u;  // brk (native) -> $8012
  rom[site + 0x27] = 0x80u;
  put(rom, 0x0000u, {0xDBu});  // 8000 STP
  put(rom, 0x0010u, {0x40u});  // 8010 RTI
  put(rom, 0x0012u, {0x40u});  // 8012 RTI
  return rom;
}

// The uploading cartridge with two edits that put instructions across the
// edges of what was uploaded: the program's STOP becomes `MOV A,#`, whose
// operand is the table's first byte, so one instruction spans the two blocks;
// the table's second byte branches to its last, which is `MOV A,#` again, whose
// operand lies past the end of the upload.
inline std::vector<std::uint8_t> straddlingUploadImage() {
  std::vector<std::uint8_t> rom = uploadingImage();
  rom[0xA0u + 23u] = 0xE8u;   // $0217  MOV A,#<table[0]>
  rom[0x100u + 1u] = 0x2Fu;   // $0219  BRA $022B
  rom[0x100u + 2u] = 0x10u;
  rom[0x100u + 19u] = 0xE8u;  // $022B  MOV A,#<not uploaded>
  return rom;
}

}  // namespace snaggletooth::disasm::fixtures
