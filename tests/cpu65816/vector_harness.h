#pragma once

// Test-only support for the 65816 vector suite: a recording bus over the 24-bit
// address space satisfying the SnesBus concept, the parsed shape of one
// SingleStepTests case, and a minimal JSON reader for the per-opcode vector files.
// No third-party JSON dependency enters the repository — the reader understands
// exactly this schema. This is a separate reader from the SPC700 harness; the two
// vector formats differ (24-bit addresses, wider registers, native/emulated files).

#include "snaggletooth/cpu/cpu65816.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snaggletooth::cpu_vectors {

// One cycle as the vectors record it: the address the chip drove, the byte that
// crossed the bus, and a string of pin states. A cycle with no valid access carries
// no value, so `value` is absent there; a halted cycle drives nothing at all, so
// `address` is absent too and every pin reads inactive.
struct CycleTrace {
  std::optional<std::uint32_t> address;
  std::optional<std::uint8_t> value;
  std::string signals;
};

// The pin string's eight characters, in the order the vectors write them. The
// first four say what the access is; the next three report the processor's live
// widths; the last marks a locked read-modify-write.
inline constexpr std::size_t kSignalVda = 0;   // a valid data address
inline constexpr std::size_t kSignalVpa = 1;   // a valid program address
inline constexpr std::size_t kSignalVpb = 2;   // an interrupt vector is being pulled
inline constexpr std::size_t kSignalRw = 3;    // 'r' reading, 'w' writing
inline constexpr std::size_t kSignalE = 4;     // emulation mode
inline constexpr std::size_t kSignalM = 5;     // 8-bit accumulator and memory
inline constexpr std::size_t kSignalX = 6;     // 8-bit index registers
inline constexpr std::size_t kSignalMlb = 7;   // the memory lock is asserted

// The pin string a halted cycle carries: the chip drives nothing.
inline constexpr const char* kHaltedSignals = "--------";

// The pin string for one access, given what the access is for and the widths the
// processor is running under at that moment.
std::string signalsFor(CycleKind kind, bool e, bool m, bool x);

// The pin string for a cycle that drives an address without a valid access: no
// address is valid and the direction reads, but the widths still report.
std::string internalSignals(bool e, bool m, bool x);

// A sparse view of the 16 MB address space. Unlisted addresses read zero; every
// write updates the map and is recorded, so a runner can prove no write landed
// outside the addresses a case accounts for. Every cycle the core narrates is
// appended to `trace`, with the widths read from the processor as the access
// happens — which is what makes a mid-instruction width change observable on the
// exact cycle it takes effect.
struct RecordingBus {
  std::unordered_map<std::uint32_t, std::uint8_t> mem;
  std::vector<std::uint32_t> writes;
  std::vector<CycleTrace> trace;
  const Cpu65816* cpu = nullptr;  // set to record pin states; optional otherwise

  [[nodiscard]] std::uint8_t read(std::uint32_t address) const {
    const auto it = mem.find(address & 0xFFFFFFu);
    return it == mem.end() ? std::uint8_t{0} : it->second;
  }

  std::uint8_t read(std::uint32_t address, CycleKind kind) {
    const std::uint8_t value = read(address);
    record(address & 0xFFFFFFu, value, kind);
    return value;
  }
  void write(std::uint32_t address, std::uint8_t value, CycleKind kind) {
    address &= 0xFFFFFFu;
    mem[address] = value;
    writes.push_back(address);
    record(address, value, kind);
  }
  // A cycle that drives an address without a valid access: no byte crosses the bus,
  // so the trace records the address alone.
  void internal(std::uint32_t address) {
    trace.push_back({.address = address & 0xFFFFFFu,
                     .value = std::nullopt,
                     .signals = liveSignals(std::nullopt)});
  }

 private:
  void record(std::uint32_t address, std::uint8_t value, CycleKind kind) {
    trace.push_back(
        {.address = address, .value = value, .signals = liveSignals(kind)});
  }
  // The pin string for the cycle now happening, with the widths read from the
  // processor as it happens. An absent kind is a cycle with no valid access.
  [[nodiscard]] std::string liveSignals(std::optional<CycleKind> kind) const;
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

// One test case: a before state, an after state, and the recorded activity of every
// cycle the instruction took. The cycle list is the per-cycle contract — its length
// is the instruction's cycle count and its entries are what the chip drove on each
// of them.
struct VectorCase {
  std::string name;
  RegState initial;
  RegState final_;
  std::vector<CycleTrace> cycles;
};

// Parses every case in one vector file's text. Throws std::runtime_error on
// malformed input.
std::vector<VectorCase> parseVectorFile(const std::string& text);

// Reads a whole file into a string; returns nullopt if it cannot be opened.
std::optional<std::string> readFile(const std::string& path);

}  // namespace snaggletooth::cpu_vectors
