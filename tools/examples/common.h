#pragma once

// What every example cartridge is built from: a LoROM or a HiROM image with a
// header, a way to place bytes in it, and the vector layout the replay
// cartridges share.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace snaggletooth::examples {

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

// A HiROM image of `banks` 64 KB banks whose header names reset at $8000 and
// leaves every other vector unused. The header sits at $FFC0 of the first bank,
// where the HiROM map puts it, and its map-mode byte says so.
inline std::vector<std::uint8_t> hiRomImage(std::size_t banks) {
  std::vector<std::uint8_t> rom(banks * 0x10000u, 0u);
  const std::size_t site = 0xFFC0u;
  for (std::size_t i = 0; i < 21; ++i) rom[site + i] = 'A';
  rom[site + 0x15] = 0x21u;  // the map-mode byte: HiROM
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

// The vectors the five replay cartridges below share: reset at $8000, the
// native NMI at $8300 and the emulation NMI at $8310, each its own handler so
// the trace reads each under one mode. Both start as a bare RTI; a cartridge
// writes its handler over the one its mode takes.
inline std::vector<std::uint8_t> imageWithNmi() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x2A] = 0x00u;  // native NMI -> $8300
  rom[site + 0x2B] = 0x83u;
  rom[site + 0x3A] = 0x10u;  // emulation NMI -> $8310
  rom[site + 0x3B] = 0x83u;
  put(rom, 0x0300u, {0x40u});  // RTI
  put(rom, 0x0310u, {0x40u});  // RTI
  return rom;
}

}  // namespace snaggletooth::examples
