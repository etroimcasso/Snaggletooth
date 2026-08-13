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

// One cycle as the vectors record it: what crossed the bus, if anything. A read or a
// write carries the address it reached and, where the recording captured one, the byte
// that moved; a wait carries neither, because the cycle reaches memory not at all.
struct CycleEvent {
  enum class Kind : std::uint8_t { Read, Write, Wait };

  std::optional<std::uint16_t> address;
  std::optional<std::uint8_t> value;
  Kind kind = Kind::Wait;
};

// The same 64KB space, with every access appended to a log. A cycle-stepped run
// compares that log against the recording one cycle at a time, so a bus this bus can
// see is a bus the vectors can pin.
struct RecordingFlatBus {
  std::array<std::uint8_t, 65536> ram{};
  std::vector<CycleEvent> events;

  std::uint8_t read(std::uint16_t address) {
    const std::uint8_t value = ram[address];
    events.push_back({.address = address,
                      .value = value,
                      .kind = CycleEvent::Kind::Read});
    return value;
  }
  void write(std::uint16_t address, std::uint8_t value) {
    ram[address] = value;
    events.push_back({.address = address,
                      .value = value,
                      .kind = CycleEvent::Kind::Write});
  }
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

// One test case: a before state, an after state, and the recorded activity of every
// cycle the instruction took. The cycle list is the per-cycle contract — its length is
// the instruction's cycle count and its entries are what the chip did on each of them.
struct VectorCase {
  std::string name;
  RegState initial;
  RegState final_;
  std::vector<CycleEvent> cycles;
};

// Parses every case in one vector file's text. Throws std::runtime_error on
// malformed input.
std::vector<VectorCase> parseVectorFile(const std::string& text);

// Reads a whole file into a string; returns nullopt if it cannot be opened.
std::optional<std::string> readFile(const std::string& path);

}  // namespace snaggletooth::test
