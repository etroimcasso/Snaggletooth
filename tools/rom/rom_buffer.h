#pragma once

// The buffer a run on another thread is fed through: the records a machine's
// observers copy at each report, and the streams that hold them, in RAM and
// then in files, so the thread that writes them never waits.
//
// A record is one byte naming its kind and a payload of fixed shape, copied
// with `std::memcpy` from what the report hands over and what the machine's
// state holds inside it. The CPU stream carries the console's records, written
// on the thread that runs the machine; the sound stream carries the sound CPU's,
// written on the thread the audio machine's cycles run on. A reader walks a
// block's records in order with `RecordReader`.
//
//   disasm::Stream stream{directory, 1'000'000'000ull, 64'000'000'000ull};  // 1 GB of RAM, 64 GB of files
//   disasm::putRecord(stream, disasm::RecordKind::Internal,
//                     disasm::InternalRecord{.address = 0x008000u, .has = false, .kind = {}});
//   stream.end();
//   while (const auto block = stream.take()) {
//     for (disasm::RecordReader r{*block}; r.more();) {
//       const disasm::RecordKind kind = r.kind();
//       if (kind == disasm::RecordKind::Internal) {
//         const auto internal = r.payload<disasm::InternalRecord>();
//         // ...
//       } else {
//         r.skip(kind);
//       }
//     }
//     stream.returned();
//   }
//
// Includes the run's working parts (`rom/rom_observe_internal.h`) for the
// sound CPU's window: like them, nothing outside `tools/rom/` and `tests/rom/`
// includes it.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

#include "rom/rom_observe_internal.h"
#include "snaggletooth/apu/spc700.h"
#include "snaggletooth/cpu/cpu65816.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {

// What a record holds. The first eight are the CPU stream's, the rest the
// sound stream's.
enum class RecordKind : std::uint8_t {
  Access = 1,        // BusAccess: a CPU access, or the work-RAM port's
  EngineAccess = 2,  // EngineAccessRecord: a transfer engine's access
  Internal = 3,      // InternalRecord: a CPU cycle with no access
  Boundary = 4,      // BoundaryRecord: an instruction boundary
  FrameStart = 5,    // FrameStartRecord: the beam wrapped to line 0, before that boundary's record
  Keyframe = 6,      // KeyframeHeader, then the whole machine as SnesState::asBytes writes it
  Reset = 7,         // ResetRecord: the console's reset line
  End = 8,           // nothing: the run is over
  SoundAccess = 17,  // SoundAccessRecord: a sound-CPU access
  SoundStep = 18,    // SoundStepRecord: a sound-CPU boundary
  KeyOnMemory = 19,  // the 65,536 bytes of audio RAM, before the access that keys a voice on
  SoundReset = 20,   // SoundResetRecord: the sound CPU after the reset line
  SoundEnd = 21,     // nothing: the run is over
};

// A transfer engine's access with its channel's DMAP and BBAD as the channel
// held them inside the report.
struct EngineAccessRecord {
  BusAccess access;
  std::uint8_t dmap = 0;
  std::uint8_t bbad = 0;
};

// A CPU cycle that drove `address` with no access; `kind` is read when `has`.
struct InternalRecord {
  std::uint32_t address = 0;
  bool has = false;
  CycleKind kind = CycleKind::DataRead;
};

// An instruction boundary: the register file after the step, the master cycles
// it took and the scanline the beam stood on. The file before is the boundary
// before's, or the one the run was bound or last reset at.
struct BoundaryRecord {
  Cpu65816State after;
  std::uint32_t masterCycles = 0;
  std::uint16_t vpos = 0;
};

// The PPU's registers the run reads at the start of a frame, and the palette RAM.
struct FrameStartRecord {
  std::uint8_t inidisp = 0;
  std::uint8_t objsel = 0;
  std::uint8_t bgmode = 0;
  std::uint8_t bg1sc = 0;
  std::uint8_t bg2sc = 0;
  std::uint8_t bg3sc = 0;
  std::uint8_t bg4sc = 0;
  std::uint8_t bg12nba = 0;
  std::uint8_t bg34nba = 0;
  std::array<std::uint8_t, 512> palette{};
};

// The whole machine once a frame: the master cycles and the frame it was taken
// at, and how many bytes of `SnesState::asBytes` follow.
struct KeyframeHeader {
  std::uint64_t master = 0;
  std::uint32_t frame = 0;
  std::uint32_t bytes = 0;
};

// The register file and the scanline the reset line left.
struct ResetRecord {
  Cpu65816State after;
  std::uint16_t vpos = 0;
};

struct SoundAccessRecord {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
};

// A sound-CPU boundary: the file before and after the step, its cycles, and the
// bytes at the program counter on each side of it as the audio machine held
// them inside the report.
struct SoundStepRecord {
  Spc700State before;
  Spc700State after;
  std::uint32_t cycles = 0;
  detail::ApuWindow window;
};

// The sound CPU's file after the reset line, the DSP's address register and
// register file, and the bytes at the program counter.
struct SoundResetRecord {
  Spc700State after;
  std::uint8_t dspAddr = 0;
  std::array<std::uint8_t, 128> registers{};
  detail::ApuWindow window;
};

// The bytes that follow a record's kind byte: a fixed size for every kind but
// `Keyframe`, whose header says how many follow it.
[[nodiscard]] std::size_t payloadBytes(RecordKind kind, const std::uint8_t* payload) noexcept;

// The RAM and the disk one or more streams hold together, and the limit on each:
// the streams of one run share one, so the limits are the run's whole buffer.
// Read from any thread.
class StreamBudget {
 public:
  StreamBudget(std::uint64_t memoryBytes, std::uint64_t diskBytes) noexcept
      : memoryBytes_(memoryBytes), diskBytes_(diskBytes) {}
  StreamBudget(const StreamBudget&) = delete;
  StreamBudget& operator=(const StreamBudget&) = delete;

  [[nodiscard]] std::uint64_t memoryBytes() const noexcept { return memoryBytes_; }
  [[nodiscard]] std::uint64_t diskBytes() const noexcept { return diskBytes_; }
  // Bytes of RAM in blocks, every block counted whole.
  [[nodiscard]] std::uint64_t memoryUsed() const noexcept;
  [[nodiscard]] std::uint64_t diskUsed() const noexcept { return onDisk_.load(std::memory_order_relaxed); }
  // Bytes handed to the readers that they have not returned, in RAM and files.
  [[nodiscard]] std::uint64_t held() const noexcept {
    return handed_.load(std::memory_order_acquire) - consumed_.load(std::memory_order_acquire);
  }

 private:
  friend class Stream;
  std::uint64_t memoryBytes_;
  std::uint64_t diskBytes_;
  std::atomic<std::uint64_t> blocks_{0};
  std::atomic<std::uint64_t> onDisk_{0};
  std::atomic<std::uint64_t> handed_{0};
  std::atomic<std::uint64_t> consumed_{0};
  std::atomic<std::uint64_t> open_{0};  // blocks being written, each charged whole
};

// A stream of records, written by one thread and read by another, held in RAM
// and then in files under a directory, so the writer never waits.
//
// The writer fills a block of `kBlockBytes` and hands it to the reader when the
// next record does not fit, at `flush()` and at `end()`; a record never spans
// two blocks. The stream's own thread keeps the stream's RAM within
// `memoryBytes`: while every block it holds — the one being written, those
// handed that the reader has not taken, those it holds, and the free ones
// kept for reuse — passes that, it frees the free ones and moves the newest
// handed blocks to files `<directory>/<n>.snagbuf`, as long as the files stay
// within `diskBytes`; it reads them back ahead of the reader, oldest first, as
// RAM has room, and the one the reader takes next whether or not it has; each
// file is removed as it is read. A block the writer needs is taken from the free
// ones or allocated, never waited for, so RAM passes `memoryBytes` by the blocks
// the writer takes before the stream's thread next runs, and always holds the
// block being written and the one being read. A block the writer opens is
// charged whole, and the stream is full when one more would bring what it holds
// past `memoryBytes` and `diskBytes` together, so what it holds never passes
// them and is full up to one block before. Neither has a floor or a ceiling.
// Streams built over one `StreamBudget` count their RAM, their files and what
// they hold against it together. The constructor creates `directory` and
// removes any `.snagbuf` file left in it, and throws `std::runtime_error` when
// it cannot; the destructor
// stops the stream's thread, removes every file it wrote and the directory when
// that leaves it empty.
class Stream {
 public:
  static constexpr std::size_t kBlockBytes = std::size_t{1} << 20;

  // A stream with a budget of its own.
  Stream(std::filesystem::path directory, std::uint64_t memoryBytes, std::uint64_t diskBytes);
  // A stream counted against `budget`, which outlives it.
  Stream(std::filesystem::path directory, StreamBudget& budget);
  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  // The writer's side: room for `bytes` in the block being written, which is
  // handed to the reader and replaced when full. Never waits, never touches a
  // file. Answers null once the stream is full, or for more than `kBlockBytes`:
  // the stream takes nothing more.
  [[nodiscard]] std::uint8_t* put(std::size_t bytes) noexcept;
  // The writer's side: the block being written is handed to the reader as it
  // stands, so the reader reaches every record written so far.
  void flush() noexcept;
  void end() noexcept;  // the writer is done: the last block is handed over

  // The reader's side: the next block in order, waiting for one; nothing once
  // the writer has ended and every block was taken. Throws `std::runtime_error`
  // once a file could not be written or read.
  [[nodiscard]] std::optional<std::span<const std::uint8_t>> take();
  // The next block in order if one is ready, without waiting; `ended` is set
  // when the writer has ended and every block was taken. Throws as `take` does.
  [[nodiscard]] std::optional<std::span<const std::uint8_t>> poll(bool& ended);
  // The oldest block the reader holds is free for the writer. A
  // reader may hold several blocks; it returns them in the order it took them.
  void returned() noexcept;

  [[nodiscard]] bool full() const noexcept;
  // This stream's own: bytes handed that the reader has not returned, in RAM and files;
  // bytes of RAM in blocks, every block counted whole; bytes in files now; bytes
  // written to files so far.
  [[nodiscard]] std::uint64_t held() const noexcept;
  [[nodiscard]] std::uint64_t memoryUsed() const noexcept;
  [[nodiscard]] std::uint64_t diskUsed() const noexcept;
  [[nodiscard]] std::uint64_t diskWritten() const noexcept;
  [[nodiscard]] std::uint64_t handed() const noexcept;       // blocks handed to the reader so far

 private:
  Stream(std::filesystem::path directory, std::unique_ptr<StreamBudget> own);
  void start();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Writes one record: its kind, then the payload's bytes. Answers false, writing
// nothing, once the stream is full.
template <class Payload>
bool putRecord(Stream& stream, RecordKind kind, const Payload& payload) noexcept {
  static_assert(std::is_trivially_copyable_v<Payload>);
  std::uint8_t* const at = stream.put(1u + sizeof(Payload));
  if (at == nullptr) return false;
  *at = static_cast<std::uint8_t>(kind);
  std::memcpy(at + 1, &payload, sizeof(Payload));
  return true;
}

// Writes a record of a kind with no payload.
bool putRecord(Stream& stream, RecordKind kind) noexcept;

// Writes a record of a kind whose payload is `bytes` the caller fills, the
// audio RAM at a key-on: answers the room for them, or null once the stream is
// full.
std::uint8_t* putBytes(Stream& stream, RecordKind kind, std::size_t bytes) noexcept;

// Writes a record of a kind whose payload is `bytes` after a fixed `header`, a
// keyframe's header and state: answers the room for `bytes`, which the caller
// fills, or null once the stream is full.
template <class Header>
std::uint8_t* putRecord(Stream& stream, RecordKind kind, const Header& header, std::size_t bytes) noexcept {
  static_assert(std::is_trivially_copyable_v<Header>);
  std::uint8_t* const at = stream.put(1u + sizeof(Header) + bytes);
  if (at == nullptr) return nullptr;
  *at = static_cast<std::uint8_t>(kind);
  std::memcpy(at + 1, &header, sizeof(Header));
  return at + 1 + sizeof(Header);
}

// Reads a block's records in order: `kind()` the next record's kind, then
// `payload<T>()` its payload, `bytes(n)` bytes of it, or `skip(kind)` all of it.
struct RecordReader {
  std::span<const std::uint8_t> block;
  std::size_t at = 0;

  [[nodiscard]] bool more() const noexcept { return at < block.size(); }
  [[nodiscard]] RecordKind kind() noexcept { return static_cast<RecordKind>(block[at++]); }
  template <class T>
  [[nodiscard]] T payload() noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T value;
    std::memcpy(&value, block.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
  }
  [[nodiscard]] std::span<const std::uint8_t> bytes(std::size_t count) noexcept {
    const std::span<const std::uint8_t> out = block.subspan(at, count);
    at += count;
    return out;
  }
  void skip(RecordKind kind) noexcept { at += payloadBytes(kind, block.data() + at); }
};

// Lowers the calling thread to the lowest scheduling class the platform gives a
// thread: `QOS_CLASS_BACKGROUND` on macOS, `SCHED_IDLE` on Linux,
// `THREAD_PRIORITY_LOWEST` on Windows. On a host with fewer cores than threads,
// every thread at a higher class is scheduled first.
void lowerThisThread() noexcept;

}  // namespace snaggletooth::disasm
