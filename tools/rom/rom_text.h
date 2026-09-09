#pragma once

// The text forms the cartridge tools share: addresses, hexadecimal values,
// counts, modes and register classes as the manifest writes and reads them.
// Inline, so the disassembler and the renderer read one definition and neither
// links the other.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm::text {

inline std::string hex(std::uint32_t value, int digits) {
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "%0*X", digits, static_cast<unsigned>(value));
  return buffer;
}

inline std::string address24(Address address) { return formatAddress(address, 24); }
inline std::string address16(Address address) { return formatAddress(address, 16); }

inline std::string mapName(CartridgeMap map) {
  switch (map) {
    case CartridgeMap::LoRom: return "LoROM";
    case CartridgeMap::HiRom: return "HiROM";
    case CartridgeMap::ExHiRom: return "ExHiROM";
  }
  return "LoROM";
}

// The words of a manifest line, split at spaces and tabs outside double quotes;
// the quotes themselves are dropped.
inline std::vector<std::string> tokens(std::string_view line) {
  std::vector<std::string> out;
  std::string current;
  bool quoted = false;
  for (const char c : line) {
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && (c == ' ' || c == '\t')) {
      if (!current.empty()) out.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// `digits` as a hexadecimal number, or nothing when a character is not a digit.
inline std::optional<std::uint32_t> parseHex(std::string_view digits) {
  if (digits.empty() || digits.size() > 8) return std::nullopt;
  std::uint32_t value = 0;
  for (const char c : digits) {
    value <<= 4;
    if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
    else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
    else return std::nullopt;
  }
  return value;
}

// A 24-bit address in the dialect's long form, `$BB:XXXX`.
inline std::optional<Address> parseLongAddress(const std::string& text) {
  if (text.size() != 8 || text[0] != '$' || text[3] != ':') return std::nullopt;
  return parseHex(text.substr(1, 2) + text.substr(4, 4));
}

// A 16-bit address in the SPC700 dialect's form, `$XXXX`.
inline std::optional<std::uint16_t> parseShortAddress(const std::string& text) {
  if (text.size() != 5 || text[0] != '$') return std::nullopt;
  const std::optional<std::uint32_t> value = parseHex(text.substr(1));
  if (!value) return std::nullopt;
  return static_cast<std::uint16_t>(*value);
}

// An image offset as the manifest writes one: `$` and six hexadecimal digits.
inline std::optional<std::size_t> parseOffset(const std::string& text) {
  if (text.size() != 7 || text[0] != '$') return std::nullopt;
  const std::optional<std::uint32_t> value = parseHex(text.substr(1));
  if (!value) return std::nullopt;
  return static_cast<std::size_t>(*value);
}

// A decimal count.
inline std::optional<std::size_t> parseCount(const std::string& text) {
  if (text.empty()) return std::nullopt;
  std::size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  return value;
}

// A width as the manifest writes it: `8`, `16`, or `?` for one the trace does
// not know. Returns whether it parsed; `known` and `eight` say what it said.
inline bool parseWidth(const std::string& token, char letter, bool& known, bool& eight) {
  if (token.size() < 3 || token[0] != letter || token[1] != '=') return false;
  const std::string value = token.substr(2);
  if (value == "?") {
    known = false;
    eight = true;
    return true;
  }
  if (value == "8") {
    known = true;
    eight = true;
    return true;
  }
  if (value == "16") {
    known = true;
    eight = false;
    return true;
  }
  return false;
}

// A register class from its name as a manifest writes it.
inline std::optional<RegisterClass> parseRegisterClass(const std::string& word) {
  for (int c = 0; c <= static_cast<int>(RegisterClass::Speed); ++c) {
    const RegisterClass cls = static_cast<RegisterClass>(c);
    if (cpu65816RegisterClassName(cls) == word) return cls;
  }
  return std::nullopt;
}

// A mode as the manifest writes it, from `words[from]` on: `e=`, `m=`, `x=`.
inline std::optional<Cpu65816Mode> parseMode(const std::vector<std::string>& words, std::size_t from) {
  if (words.size() < from + 3) return std::nullopt;
  const std::string& e = words[from];
  if (e != "e=0" && e != "e=1") return std::nullopt;
  bool accumulatorKnown = true;
  bool accumulator8 = true;
  bool indexKnown = true;
  bool index8 = true;
  if (!parseWidth(words[from + 1], 'm', accumulatorKnown, accumulator8)) return std::nullopt;
  if (!parseWidth(words[from + 2], 'x', indexKnown, index8)) return std::nullopt;
  if (e == "e=1") return Cpu65816Mode::reset();
  return Cpu65816Mode{.emulation = false,
                      .accumulator8 = accumulator8,
                      .index8 = index8,
                      .accumulatorKnown = accumulatorKnown,
                      .indexKnown = indexKnown,
                      .carryKnown = false,
                      .carry = false};
}

// The register class list a manifest writes joined by `joint`.
inline std::string classesText(const std::vector<RegisterClass>& classes, std::string_view joint) {
  std::string out;
  for (const RegisterClass cls : classes) {
    if (!out.empty()) out += joint;
    out += std::string(cpu65816RegisterClassName(cls));
  }
  return out;
}

}  // namespace snaggletooth::disasm::text
