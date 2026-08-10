#pragma once

// Test-only support for the 65816 vector suite: a recording bus over the 24-bit
// address space satisfying the SnesBus concept, the parsed shape of one
// SingleStepTests case, and a minimal JSON reader for the per-opcode vector files.
// No third-party JSON dependency enters the repository — the reader understands
// exactly this schema. This is a separate reader from the SPC700 harness; the two
// vector formats differ (24-bit addresses, wider registers, native/emulated files).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snaggletooth::cpu_vectors {

// A sparse view of the 16 MB address space. Unlisted addresses read zero; every
// write updates the map and is recorded, so a runner can prove no write landed
// outside the addresses a case accounts for.
struct RecordingBus {
  std::unordered_map<std::uint32_t, std::uint8_t> mem;
  std::vector<std::uint32_t> writes;

  [[nodiscard]] std::uint8_t read(std::uint32_t address) const {
    const auto it = mem.find(address & 0xFFFFFFu);
    return it == mem.end() ? std::uint8_t{0} : it->second;
  }
  void write(std::uint32_t address, std::uint8_t value) {
    address &= 0xFFFFFFu;
    mem[address] = value;
    writes.push_back(address);
  }
};

// One side (initial or final) of a vector case: the registers plus the sparse list
// of RAM cells the case accounts for. Addresses absent from the list are zero.
struct RegState {
  std::uint16_t pc = 0;
  std::uint16_t s = 0;
  std::uint16_t a = 0;
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  std::uint16_t d = 0;
  std::uint8_t p = 0;
  std::uint8_t dbr = 0;
  std::uint8_t pbr = 0;
  bool e = false;
  std::vector<std::pair<std::uint32_t, std::uint8_t>> ram;
};

// One test case: a before state, an after state, and the number of machine cycles
// the instruction took (the length of the case's cycle-activity list; the per-cycle
// contents are the contested sub-cycle frontier and are not asserted).
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

}  // namespace snaggletooth::cpu_vectors
