#pragma once

#include "examples/common.h"
#include "examples/uploading/uploading.h"

namespace snaggletooth::examples {

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

}  // namespace snaggletooth::examples
