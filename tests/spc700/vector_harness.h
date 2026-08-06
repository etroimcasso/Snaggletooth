#pragma once

// Test-only support for the SPC700 vector suite: a flat-RAM bus satisfying the
// ApuBus concept, the parsed shape of one SingleStepTests case, and a minimal
// JSON reader for the per-opcode vector files. No third-party JSON dependency
// enters the repository — the reader understands exactly this schema.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace snaggletooth::test {

// A plain 64KB address space: every read and write hits RAM with no side effects.
struct FlatRamBus {
  std::array<std::uint8_t, 65536> ram{};

  [[nodiscard]] std::uint8_t read(std::uint16_t address) const {
    return ram[address];
  }
  void write(std::uint16_t address, std::uint8_t value) { ram[address] = value; }
};

// One side (initial or final) of a vector case: the registers plus the sparse
// list of non-zero RAM cells. Addresses absent from the list are zero.
struct RegState {
  std::uint16_t pc = 0;
  std::uint8_t a = 0;
  std::uint8_t x = 0;
  std::uint8_t y = 0;
  std::uint8_t sp = 0;
  std::uint8_t psw = 0;
  std::vector<std::pair<std::uint16_t, std::uint8_t>> ram;
};

// One test case: a before state, an after state, and the number of machine
// cycles the instruction took (the length of the case's cycle-activity list; its
// per-cycle contents are the contested sub-cycle frontier and are not asserted).
struct VectorCase {
  std::string name;
  RegState initial;
  RegState final_;
  std::size_t cycles = 0;
};

// Parses every case in one vector file's text. Throws std::runtime_error on
// malformed input.
std::vector<VectorCase> parseVectorFile(const std::string& text);

// Reads a whole file into a string; returns nullopt if it cannot be opened.
std::optional<std::string> readFile(const std::string& path);

}  // namespace snaggletooth::test
