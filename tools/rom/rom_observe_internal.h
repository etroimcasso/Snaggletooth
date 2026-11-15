#pragma once

// The working parts of a run, shared by `rom_observe.cpp`, the buffered run
// (`rom_buffered_run.cpp`, `rom_buffer.h`) and the run's contract files: what
// the run reads of the machine, the observer the run sets on it, the check of
// each step on both CPUs and what the run does with a checked step, the run
// itself (`ObservedRun::Impl`), the reading of addresses they share, and what
// the run keeps to run a routine again for each value of its index. Not a
// library surface: nothing outside `tools/rom/` and `tests/rom/` includes it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "ir/cpu65816_lift.h"
#include "ir/ir_lockstep.h"
#include "ir/ir_provenance.h"
#include "ir/ir_text.h"
#include "ir/spc700_lift.h"
#include "rom/rom_observe.h"
#include "rom/rom_text.h"
#include "snaggletooth/apu/dsp.h"
#include "snaggletooth/cpu/cpu65816.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm::detail {

// The address the tree places the bytes the CPU holds at `address`: the one
// home of the image bytes it reads, so a bank that mirrors the image names the
// bank the image is written for. An address outside the image is its own.
inline Address placed(const CartridgeBoard& board, std::size_t imageBytes, Address address) {
  const std::optional<std::size_t> offset = romOffset(board, address, imageBytes);
  if (!offset) return address;
  const std::optional<std::uint32_t> home = romAddress(board.map, *offset);
  return home ? *home : address;
}

inline bool inImage(const CartridgeBoard& board, std::size_t imageBytes, Address address) {
  return romOffset(board, address, imageBytes).has_value();
}

// The byte the CPU would read at a bus address, from what the run can see: the
// image — through a LoROM save window with no save behind it as through any
// lower half — and work RAM — banks $7E-$7F whole, and the first 8 KB of every
// bank that mirrors them. A register, a save, a coprocessor's half, or open bus
// is nothing: the core reads it, the toolkit does not pretend to.
inline std::optional<std::uint8_t> readByte(const CartridgeBoard& board, std::span<const std::uint8_t> rom,
                                     const SnesState& state, Address address) {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  const std::uint32_t offset = address & 0xFFFFu;
  if (bank == 0x7Eu || bank == 0x7Fu) return state.wram[((bank - 0x7Eu) << 16) | offset];
  const bool mirrors = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
  if (mirrors && offset < 0x2000u) return state.wram[offset];
  if (const std::optional<std::size_t> at = romOffset(board, address, rom.size())) return rom[*at];
  return std::nullopt;
}

// The mode the CPU is in, as the trace carries one. The widths are known — the
// CPU has them — and the carry is not remembered, since nothing here follows an
// `XCE`.
inline Cpu65816Mode modeOf(const Cpu65816State& cpu) {
  return {.emulation = cpu.e,
          .accumulator8 = (cpu.p & kCpuFlagM) != 0,
          .index8 = (cpu.p & kCpuFlagX) != 0,
          .accumulatorKnown = true,
          .indexKnown = true,
          .carryKnown = false,
          .carry = false};
}

// Gives each port what the script holds for it at `frame`.
inline void presentPads(Snes& machine, const InputScript& input, std::uint32_t frame) {
  machine.setJoypad(JoypadPort::One, input.padAt(JoypadPort::One, frame));
  machine.setJoypad(JoypadPort::Two, input.padAt(JoypadPort::Two, frame));
}

// Whether a CPU access reaches the registers: offsets `$2100`-`$43FF` of the
// system banks. The two start registers live there, and nowhere else.
inline bool inSystemBank(std::uint32_t address) {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  return bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
}

constexpr std::uint16_t kMdmaen = 0x420Bu;
constexpr std::uint16_t kHdmaen = 0x420Cu;
constexpr std::uint16_t kWmdata = 0x2180u;

// Whether an address is the work-RAM data port.
inline bool isPort(std::uint32_t address) {
  return inSystemBank(address) && (address & 0xFFFFu) == kWmdata;
}


// The address the step says follows `address`.
inline Address stepped(Address address, MovedStep step) {
  const Address bank = address & 0xFF0000u;
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  switch (step) {
    case MovedStep::Increment: return bank | static_cast<std::uint16_t>(offset + 1u);
    case MovedStep::Decrement: return bank | static_cast<std::uint16_t>(offset - 1u);
    case MovedStep::Fixed: return address;
  }
  return address;
}

// Every field of a range but its count, as a key.
using RangeKey = std::tuple<Address, std::uint8_t, bool, Address, Address, std::uint8_t,
                            std::uint32_t, std::uint8_t>;
inline RangeKey keyOf(const MovedRange& r) {
  return {r.site,   r.channel, r.toRegister,                    r.registerAddress,
          r.memory, static_cast<std::uint8_t>(r.step), r.bytes, static_cast<std::uint8_t>(r.kind)};
}

// The memory a video data port reaches, by the B-bus address written: nothing
// for a register whose bytes land in no memory.
inline std::optional<PortMemory> portMemory(std::uint32_t address) {
  if (!inSystemBank(address)) return std::nullopt;
  switch (address & 0xFFFFu) {
    case 0x2118u:
    case 0x2119u: return PortMemory::Vram;
    case 0x2122u: return PortMemory::Cgram;
    case 0x2104u: return PortMemory::Oam;
    default: return std::nullopt;
  }
}

// Where the port put one more byte, folded into a landing's extent.
inline void foldLanding(std::optional<PortLanding>& landing, PortMemory memory, std::uint16_t at) {
  if (!landing || landing->memory != memory) {
    landing = PortLanding{.memory = memory,
                          .lowest = at,
                          .highest = at,
                          .shown = false,
                          .areas = 0,
                          .depths = 0,
                          .palette = std::nullopt};
    return;
  }
  landing->lowest = std::min(landing->lowest, at);
  landing->highest = std::max(landing->highest, at);
}

// Whether the words `lowest`-`highest` meet an area of `words` words at
// `base`, the area wrapping at the end of VRAM as the address does.
inline bool meets(std::uint16_t lowest, std::uint16_t highest, std::uint32_t base, std::uint32_t words) {
  constexpr std::uint32_t kVramWords = 0x8000u;
  const std::uint32_t start = base % kVramWords;
  const std::uint32_t end = start + std::min(words, kVramWords);  // exclusive
  const auto overlaps = [&](std::uint32_t from, std::uint32_t to) {
    return from < to && lowest < to && from <= highest;
  };
  if (overlaps(start, std::min(end, kVramWords))) return true;
  return end > kVramWords && overlaps(0, end - kVramWords);
}

// What the PPU used a stretch of VRAM as, and at what depths.
struct VramUse {
  std::uint16_t areas = 0;
  std::uint8_t depths = 0;
};

// The bit for a color depth in bits a pixel.
inline std::uint8_t depthBit(std::uint8_t depth) {
  return depth == 2u ? kDepth2 : depth == 4u ? kDepth4 : depth == 8u ? kDepth8 : 0u;
}

// What the PPU used the words `lowest`-`highest` as, from the screen mode and
// the bases as they stand: each layer the mode has, its screen and its name
// base at the layer's color depth in that mode; the sprite tiles; the whole
// of VRAM under Mode 7 — with the depth of every name base met.
inline VramUse vramAreas(const PpuState& state, std::uint16_t lowest, std::uint16_t highest) {
  const std::uint8_t mode = state.bgmode & 7u;
  if (mode == 7u) return VramUse{.areas = kAreaMode7, .depths = kDepth8};
  // Per mode, each layer's color depth in bits — zero for a layer the mode
  // does not have — and whether the layer has a screen without tiles of its
  // own, which is BG3 under the offset-per-tile modes.
  static constexpr std::uint8_t kDepth[8][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
                                                {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {0, 0, 0, 0}};
  const bool offsetPerTile = mode == 2u || mode == 4u || mode == 6u;
  const std::uint8_t screens[4] = {state.bg1sc, state.bg2sc, state.bg3sc, state.bg4sc};
  const std::uint8_t bases[4] = {static_cast<std::uint8_t>(state.bg12nba & 0x0Fu),
                                 static_cast<std::uint8_t>(state.bg12nba >> 4),
                                 static_cast<std::uint8_t>(state.bg34nba & 0x0Fu),
                                 static_cast<std::uint8_t>(state.bg34nba >> 4)};
  VramUse use;
  for (std::size_t layer = 0; layer < 4; ++layer) {
    const std::uint8_t depth = kDepth[mode][layer];
    const bool screenOnly = layer == 2 && offsetPerTile;
    if (depth == 0u && !screenOnly) continue;
    const std::uint32_t screenBase = static_cast<std::uint32_t>(screens[layer] & 0xFCu) << 8;  // 1K-word steps
    const std::uint8_t size = screens[layer] & 3u;
    const std::uint32_t screenWords = size == 0u ? 0x400u : size == 3u ? 0x1000u : 0x800u;
    if (meets(lowest, highest, screenBase, screenWords)) use.areas |= static_cast<std::uint16_t>(kAreaTilemap1 << layer);
    if (depth == 0u) continue;
    const std::uint32_t nameBase = static_cast<std::uint32_t>(bases[layer]) << 12;  // 4K-word steps
    const std::uint32_t nameWords = 1024u * 4u * depth;                             // a thousand tiles of 4 words per bit
    if (meets(lowest, highest, nameBase, nameWords)) {
      use.areas |= static_cast<std::uint16_t>(kAreaTiles1 << layer);
      use.depths |= depthBit(depth);
    }
  }
  const std::uint32_t spriteBase = static_cast<std::uint32_t>(state.objsel & 7u) << 13;  // 8K-word steps
  const std::uint32_t gap = static_cast<std::uint32_t>((state.objsel >> 3) & 3u) << 12;   // 4K-word steps
  if (meets(lowest, highest, spriteBase, 0x1000u) || meets(lowest, highest, spriteBase + 0x1000u + gap, 0x1000u)) {
    use.areas |= kAreaSprites;
    use.depths |= kDepth4;
  }
  return use;
}

// Every field of a landing but its palette, as a key: the palette is the
// first sighting's, and a later sighting under another palette is the same
// landing seen again.
using LandingKey = std::tuple<Address, std::uint8_t, Address, std::uint32_t, std::uint8_t, std::uint8_t,
                              std::uint16_t, std::uint16_t, bool, std::uint16_t, std::uint8_t>;
inline LandingKey keyOf(const LandedRange& l) {
  return {l.site,           l.channel,          l.memory,         l.bytes,        static_cast<std::uint8_t>(l.kind),
          static_cast<std::uint8_t>(l.landing.memory), l.landing.lowest, l.landing.highest, l.landing.shown,
          l.landing.areas, l.landing.depths};
}

// A stream's identity with its landing, as a key.
using StreamKey = std::tuple<Address, Address, std::size_t, std::uint32_t, bool, Address, bool, std::uint8_t,
                             std::uint16_t, std::uint16_t, bool, std::uint16_t, std::uint8_t>;
inline StreamKey keyOf(const StreamedRange& s) {
  const PortLanding landing = s.landing.value_or(PortLanding{});
  return {s.site,   s.registerAddress, s.romOffset, s.bytes, s.memory.has_value(), s.memory.value_or(0),
          s.landing.has_value(), static_cast<std::uint8_t>(landing.memory), landing.lowest, landing.highest,
          landing.shown, landing.areas, landing.depths};
}

// Every field of a walk but its count, as a key.
using WalkKey = std::tuple<Address, std::uint8_t, Address, std::uint32_t, std::uint8_t, unsigned, bool>;
inline WalkKey keyOf(const WalkedRange& w) {
  return {w.site, w.channel, w.memory, w.bytes, static_cast<std::uint8_t>(w.kind), w.unit, w.indirect};
}

// The work-RAM byte at a `$7E`/`$7F` address.
inline std::uint8_t workRamByte(const SnesState& state, Address address) {
  return state.wram[(((address >> 16) - 0x7Eu) << 16) | (address & 0xFFFFu)];
}

// What the run reads of the machine: the machine itself, for a run on the thread
// that runs it, or a state a worker keeps current from the buffer, with the bytes
// at the sound CPU's program counter as they stood at each boundary.
struct ApuWindow {
  std::uint16_t pc[2]{};  // the counter at the boundary before the step, and after it
  std::array<std::uint8_t, 3> bytes[2]{};
};
struct MachineView {
  const SnesState* state = nullptr;
  const Snes* live = nullptr;         // set for the machine itself
  const ApuWindow* window = nullptr;  // set for a kept state
  explicit MachineView(const Snes& machine) noexcept : state(&machine.state()), live(&machine) {}
  MachineView(const SnesState& kept, const ApuWindow& apuWindow) noexcept : state(&kept), window(&apuWindow) {}
  [[nodiscard]] const SnesState& held() const noexcept { return *state; }
  [[nodiscard]] std::span<const std::uint8_t> cgram() const noexcept { return state->ppu.cgram; }
  // The audio machine's byte at `address`: `Snes::peekApu` on the machine; on a
  // kept state, a byte of the window, and `std::logic_error` for any other.
  [[nodiscard]] std::uint8_t peekApu(std::uint16_t address) const {
    if (live != nullptr) return live->peekApu(address);
    for (const std::size_t side : {std::size_t{1}, std::size_t{0}}) {
      const auto offset = static_cast<std::uint16_t>(address - window->pc[side]);
      if (offset < 3u) return window->bytes[side][offset];
    }
    throw std::logic_error("a byte of the audio machine the buffer does not hold");
  }
};

// The observer the run sets on the machine: it follows every byte the two
// transfer engines move and groups them into ranges, and hands every CPU
// access and cycle to the step observer the lockstep reads. A range is open
// while the next byte lands where the channel's step says; it closes at a
// break in the step, at a new trigger for its channel, at the start of a frame
// for the HDMA engine's — a new walk of the table — and at the end of the run.
// A closed range that was seen before is counted, not repeated.
//
// The loop tells the recorder the instruction about to run before every step,
// so a CPU write to `MDMAEN` or `HDMAEN` — which the observer sees like any
// other access — names the site every byte the channel then moves belongs to.
// The recorder also keeps the shadow current for what the engines and the port
// move, which never passes through the interpreter: an engine's read carries
// the origin of the byte it read, its write lands that origin in work RAM
// under the trigger's name, and a byte the port reaches on the CPU's behalf is
// queued for the interpreter to pair with the CPU's own access. A byte an
// engine reads out of work RAM is folded into the origin of the range it
// belongs to, with the writer that put it there — and so is a byte the CPU
// carries out of work RAM to a data register, which the shadow tells the
// recorder about as the CPU goes, so the buffer the CPU carried is an extent
// exactly as one an engine carried.
//
// Every write through a video data port says where the port put the byte,
// and the recorder folds it into the landing of the range open on that
// channel — or, for the CPU's own store, holds it for the shadow's stream to
// take. A landing in VRAM is read at the first start of a frame with the
// screen on after its bytes landed, against the mode and the bases as they
// then stand, whether the range is still open or has closed; bytes that land
// after that reading are read at the next such frame, and the areas are the
// union. A landing none of whose bytes a frame was drawn after is unshown. A
// palette or OAM landing is read as it lands. Reading a VRAM landing also
// takes the palette RAM as it stands, once, and the depth of every name base
// the landing meets; a table's walk keeps the channel's transfer pattern; and
// a range or a carry read out of work RAM keeps the bytes it carried, which
// join the extent's contents once its landing is read.
struct Recorder final : BusObserver, ir::CarrySink {
  MachineView machine;
  ir::Provenance& shadow;
  Address site = 0;  // the instruction about to run
  ir::StepObserver cpu;  // what the CPU did this step: its fetches, its data accesses, its cycles

  // An open range, where its next byte is expected, who wrote its bytes and
  // where they came from — for a range read out of work RAM — and where its
  // bytes landed.
  struct Open {
    MovedRange range;
    Address next = 0;
    std::vector<StagedWriter> writers;
    std::optional<PortLanding> landing;
    bool unread = false;  // bytes landed in VRAM since the landing was last read at a drawn frame
    std::vector<std::uint8_t> values;  // every byte the engine read, in transfer order
    unsigned unit = 1;                 // a table's or a block's transfer unit, from the channel's DMAP
    bool indirect = false;             // and whether its entries carry pointers
    std::vector<ContentOrigin> byteOrigins;  // the origin of each byte read, as runs, in transfer order
  };
  std::array<std::optional<Open>, 8> dma;       // a general-purpose transfer per channel
  std::array<std::optional<Open>, 8> table;     // an HDMA table per channel
  std::array<std::optional<Open>, 8> indirect;  // an indirect block per channel
  std::array<Address, 8> dmaSite{};             // the last `MDMAEN` write naming each channel
  std::array<Address, 8> hdmaSite{};            // the `HDMAEN` write that enabled each channel
  std::array<MovedKind, 8> lastKind{};          // what the channel's last A-bus byte was, for its B-bus write
  std::uint8_t hdmaEnabled = 0;                 // the last `HDMAEN` value

  std::map<RangeKey, std::size_t> index;  // a closed range's place in `out`
  std::vector<MovedRange> out;

  // The landings: those waiting for a drawn frame, and those read, each
  // distinct one once with its count; the streams the same way.
  std::vector<LandedRange> pendingLandings;
  std::map<LandingKey, std::size_t> landedIndex;
  std::vector<LandedRange> landed;
  std::vector<StreamedRange> pendingStreams;
  std::map<StreamKey, std::size_t> streamIndex;
  std::vector<StreamedRange> streams;

  // The walks, each distinct one once with its count.
  std::map<WalkKey, std::size_t> walkIndex;
  std::vector<WalkedRange> walked;

  // The palette RAM at each drawn frame a VRAM landing was read at, each
  // distinct one once.
  std::vector<std::vector<std::uint8_t>> palettes;

  // A content carried out of an extent whose landing waits for a drawn frame.
  struct PendingContent {
    std::pair<Address, std::uint32_t> extent;
    CarriedContent content;
  };
  std::vector<PendingContent> pendingContents;

  // How many contents have been carried out so far: the next one's `order`.
  std::uint32_t carryCount = 0;

  // The last stream closed from a buffer for each register, with its landing
  // as the closing saw it: the carry that ends after it takes the landing.
  std::map<std::uint32_t, StreamedRange> closedFromBuffer;

  // Where the port put the bytes the CPU stored this step, in order, for the
  // shadow's streams to take by register.
  std::deque<std::pair<std::uint32_t, std::uint16_t>> stepLandings;

  // The origin of the byte an engine is carrying between its read and its
  // write, and the trigger it moves under.
  std::optional<ir::Origin> carried;
  Address carrier = 0;

  // The port's own access to work RAM, held until the access to `$2180` that
  // caused it says whose it was: the machine reports the port's first.
  std::optional<BusAccess> port;

  // Every address an engine wrote on the A bus this step, through the
  // work-RAM port included, in order.
  std::vector<Address> engineWrites;

  // The staged extents, each accumulating over every range with that extent.
  std::map<std::pair<Address, std::uint32_t>, StagedRange> staged;

  // A buffer the CPU is carrying out to a data register, by register: where
  // it begins, how many bytes so far, and who wrote them.
  struct Carry {
    Address memory = 0;
    std::uint32_t bytes = 0;
    std::vector<StagedWriter> writers;
    std::vector<std::uint8_t> values;        // every byte carried, in address order
    std::vector<ContentOrigin> byteOrigins;  // and the origin of each, as runs
  };
  std::map<std::uint32_t, Carry> carries;

  Recorder(MachineView m, ir::Provenance& p) : machine(m), shadow(p) {}

  // A step begins: what the CPU did last step is cleared, and so are the port
  // landings the shadow did not take and the engines' writes.
  void stepBegan(Address at) {
    site = at;
    cpu.clear();
    stepLandings.clear();
    engineWrites.clear();
  }

  // A landing read — or given up as unshown — joins the count of its kind.
  void record(LandedRange landing) {
    const LandingKey key = keyOf(landing);
    const auto found = landedIndex.find(key);
    if (found == landedIndex.end()) {
      landedIndex.emplace(key, landed.size());
      landed.push_back(landing);
    } else {
      ++landed[found->second].times;
    }
  }
  void record(StreamedRange stream) {
    const StreamKey key = keyOf(stream);
    const auto found = streamIndex.find(key);
    if (found == streamIndex.end()) {
      streamIndex.emplace(key, streams.size());
      streams.push_back(stream);
    } else {
      ++streams[found->second].times;
    }
  }

  // Reads a landing: a palette or OAM landing is what it is; a VRAM landing
  // takes the areas its extent lies in under the mode and the bases as they
  // stand, and their depths, joined to any it was read into before, and the
  // palette RAM as it stands the first time.
  void readLanding(PortLanding& landing) {
    landing.shown = true;
    switch (landing.memory) {
      case PortMemory::Cgram: landing.areas = kAreaPalette; return;
      case PortMemory::Oam: landing.areas = kAreaOam; return;
      case PortMemory::Vram: {
        const VramUse use = vramAreas(machine.held().ppu, landing.lowest, landing.highest);
        landing.areas |= use.areas;
        landing.depths |= use.depths;
        if (!landing.palette) landing.palette = paletteNow();
        return;
      }
    }
  }

  // The palette RAM as it stands, interned: the index of the copy equal to it.
  // A run holds few distinct copies, so they are searched, not indexed.
  std::size_t paletteNow() {
    const std::span<const std::uint8_t> cgram = machine.cgram();
    std::vector<std::uint8_t> copy(cgram.begin(), cgram.end());
    for (std::size_t i = 0; i < palettes.size(); ++i) {
      if (palettes[i] == copy) return i;
    }
    palettes.push_back(std::move(copy));
    return palettes.size() - 1u;
  }

  // The origin of one byte a carry takes, on its own, joined to the run before
  // it when it is the same: a byte read from the image through an engine, or
  // one with no origin — cleared, computed, read from a register — has none.
  void noteOrigin(std::vector<ContentOrigin>& runs, Address address, bool read) {
    ir::OriginSet set;
    if (read) {
      if (const std::optional<ir::Origin> origin = shadow.originOf(address)) set = shadow.origins().of(*origin);
    }
    if (!runs.empty() && runs.back().origin == set) {
      ++runs.back().bytes;
      return;
    }
    runs.push_back(ContentOrigin{.bytes = 1, .origin = std::move(set)});
  }

  // The origin of every byte the writers wrote, together.
  static ir::OriginSet originOf(const std::vector<StagedWriter>& writers) {
    ir::OriginSet origin;
    for (const StagedWriter& writer : writers) ir::Origins::merge(origin, writer.origin);
    return origin;
  }

  // A content carried out of an extent joins the extent's contents, once.
  void resolve(const PendingContent& pending) {
    StagedRange& range = staged[pending.extent];
    range.memory = pending.extent.first;
    range.bytes = pending.extent.second;
    const bool known = std::any_of(range.contents.begin(), range.contents.end(),
                                   [&](const CarriedContent& c) { return sameContent(c, pending.content); });
    if (!known) range.contents.push_back(pending.content);
  }

  // A content whose landing is read, or has none, is resolved now; one whose
  // VRAM landing waits for a drawn frame is resolved at that frame. Either way
  // it takes its place in the run's order of carries here, so a content
  // resolved late still sorts by when it was carried.
  void carryOut(std::pair<Address, std::uint32_t> extent, CarriedContent content, bool unread) {
    content.order = carryCount++;
    PendingContent pending{.extent = extent, .content = std::move(content)};
    if (unread && pending.content.landing && pending.content.landing->memory == PortMemory::Vram) {
      pendingContents.push_back(std::move(pending));
    } else {
      resolve(pending);
    }
  }

  void close(std::optional<Open>& open) {
    if (!open) return;
    const RangeKey key = keyOf(open->range);
    const auto found = index.find(key);
    if (found == index.end()) {
      index.emplace(key, out.size());
      out.push_back(open->range);
    } else {
      ++out[found->second].times;
    }
    if (open->range.kind != MovedKind::Dma) {
      const WalkedRange walk{.site = open->range.site,
                             .channel = open->range.channel,
                             .memory = open->range.memory,
                             .bytes = open->range.bytes,
                             .kind = open->range.kind,
                             .unit = open->unit,
                             .indirect = open->indirect,
                             .times = 1};
      const WalkKey walkKey = keyOf(walk);
      const auto known = walkIndex.find(walkKey);
      if (known == walkIndex.end()) {
        walkIndex.emplace(walkKey, walked.size());
        walked.push_back(walk);
      } else {
        ++walked[known->second].times;
      }
    }
    if (!open->writers.empty() && open->range.step != MovedStep::Fixed) {
      stage(extentStart(open->range), open->range.bytes, open->writers);
      // What the extent held, in address order: a range read downward was
      // read from its highest byte first.
      std::vector<std::uint8_t> values = open->values;
      std::vector<ContentOrigin> byteOrigins = open->byteOrigins;
      if (open->range.step == MovedStep::Decrement) {
        std::reverse(values.begin(), values.end());
        std::reverse(byteOrigins.begin(), byteOrigins.end());
      }
      if (open->range.registerClass) {
        carryOut({extentStart(open->range), open->range.bytes},
                CarriedContent{.bytes = std::move(values),
                               .origin = originOf(open->writers),
                               .cls = *open->range.registerClass,
                               .kind = open->range.kind,
                               .landing = open->landing,
                               .unit = open->unit,
                               .indirect = open->indirect,
                               .byteOrigins = std::move(byteOrigins),
                               .order = 0},
                open->unread);
      }
    }
    if (open->landing) {
      LandedRange landing{.site = open->range.site,
                          .channel = open->range.channel,
                          .memory = open->range.memory,
                          .bytes = open->range.bytes,
                          .kind = open->range.kind,
                          .landing = *open->landing,
                          .times = 1};
      if (open->unread) {
        pendingLandings.push_back(landing);
      } else {
        record(landing);
      }
    }
    open.reset();
  }

  // The start of a frame with the screen on reads every landing with bytes no
  // drawn frame has read — a range still open on a channel, and every range and
  // stream that closed since the last such frame — against the mode and the
  // bases as they stand.
  void frameDrawn() {
    if (machine.held().ppu.forcedBlank()) return;
    for (std::uint8_t c = 0; c < 8; ++c) {
      for (std::optional<Open>* open : {&dma[c], &table[c], &indirect[c]}) {
        if (!*open || !(*open)->landing || !(*open)->unread) continue;
        readLanding(*(*open)->landing);
        (*open)->unread = false;
      }
    }
    for (LandedRange& landing : pendingLandings) {
      readLanding(landing.landing);
      record(landing);
    }
    pendingLandings.clear();
    for (StreamedRange& stream : pendingStreams) {
      readLanding(*stream.landing);
      record(stream);
    }
    pendingStreams.clear();
    for (PendingContent& pending : pendingContents) {
      readLanding(*pending.content.landing);
      resolve(pending);
    }
    pendingContents.clear();
  }

  // Where the port put the byte the CPU stored to `registerAddress` — or to
  // the second of its pair — this step, the first such store the shadow has not taken.
  std::optional<std::uint16_t> landedAt(std::uint32_t registerAddress) override {
    for (auto it = stepLandings.begin(); it != stepLandings.end(); ++it) {
      if (it->first == registerAddress || it->first == registerAddress + 1u) {
        const std::uint16_t at = it->second;
        stepLandings.erase(it);
        return at;
      }
    }
    return std::nullopt;
  }

  // A stream closed: with its landing, it waits for a drawn frame as a range
  // does, or is read at once; without one, it is counted as it is.
  void streamClosed(const ir::Stream& stream) override {
    StreamedRange range{.site = stream.site,
                        .registerAddress = stream.registerAddress,
                        .registerName = {},
                        .registerClass = std::nullopt,
                        .romOffset = stream.first,
                        .bytes = static_cast<std::uint32_t>(stream.bytes),
                        .times = 1,
                        .source = stream.source,
                        .memory = stream.memory,
                        .landing = std::nullopt};
    if (const std::optional<Cpu65816Register> reg = cpu65816Register(range.registerAddress)) {
      range.registerName = reg->name;
      range.registerClass = reg->cls;
    }
    const std::optional<PortMemory> memory = portMemory(range.registerAddress);
    if (stream.landed && memory) {
      range.landing = PortLanding{.memory = *memory,
                                  .lowest = stream.lowest,
                                  .highest = stream.highest,
                                  .shown = false,
                                  .areas = 0,
                                  .depths = 0,
                                  .palette = std::nullopt};
      if (*memory == PortMemory::Vram) {
        if (stream.memory) closedFromBuffer[range.registerAddress] = range;
        pendingStreams.push_back(range);
        return;
      }
      readLanding(*range.landing);
    }
    if (stream.memory) closedFromBuffer[range.registerAddress] = range;
    record(range);
  }

  // Folds a closed range read out of work RAM into its extent.
  void stage(Address first, std::uint32_t bytes, const std::vector<StagedWriter>& writers) {
    const std::pair<Address, std::uint32_t> extent{first, bytes};
    StagedRange& range = staged[extent];
    range.memory = extent.first;
    range.bytes = extent.second;
    for (const StagedWriter& writer : writers) {
      ir::Origins::merge(range.origin, writer.origin);
      const auto same = std::find_if(range.writers.begin(), range.writers.end(), [&](const StagedWriter& w) {
        return w.writer == writer.writer && w.unwritten == writer.unwritten;
      });
      if (same == range.writers.end()) {
        range.writers.push_back(writer);
      } else {
        same->bytes += writer.bytes;
        ir::Origins::merge(same->origin, writer.origin);
        ir::OriginSet held;
        held.image = std::move(same->sources);
        ir::OriginSet more;
        more.image = writer.sources;
        ir::Origins::merge(held, more);
        same->sources = std::move(held.image);
      }
    }
  }

  // A byte read out of work RAM for the range or the carry that is open: its
  // origin and its writer join the writers.
  void fold(std::vector<StagedWriter>& writers, Address address) {
    const std::optional<ir::Origin> origin = shadow.originOf(address);
    if (!origin) return;
    const std::optional<ir::Writer> writer = shadow.writerOf(address);
    const StagedWriter key{.writer = writer.value_or(ir::Writer{}),
                           .unwritten = !writer.has_value(),
                           .bytes = 0,
                           .origin = {},
                           .sources = {}};
    auto same = std::find_if(writers.begin(), writers.end(), [&](const StagedWriter& w) {
      return w.writer == key.writer && w.unwritten == key.unwritten;
    });
    if (same == writers.end()) {
      writers.push_back(key);
      same = writers.end() - 1;
    }
    ++same->bytes;
    shadow.origins().accumulate(same->origin, *origin);
    ir::OriginSet sources;
    sources.image = shadow.sourcesOf(address);
    ir::OriginSet held;
    held.image = std::move(same->sources);
    ir::Origins::merge(held, sources);
    same->sources = std::move(held.image);
  }

  // The CPU carried a byte of work RAM out to a data register: the byte is
  // folded now, under the shadow as it stands, since the buffer may be rebuilt
  // before the carry ends.
  void carriedByte(std::uint32_t registerAddress, Address memory, bool continues) override {
    Carry& carry = carries[registerAddress];
    if (!continues) carry = Carry{.memory = memory, .bytes = 0, .writers = {}, .values = {}, .byteOrigins = {}};
    fold(carry.writers, memory);
    ++carry.bytes;
    carry.values.push_back(workRamByte(machine.held(), memory));
    noteOrigin(carry.byteOrigins, memory, true);
  }

  // The carry ended after the shadow closed its stream, so the stream's
  // landing — as that closing saw it — is the content's.
  void carryEnded(std::uint32_t registerAddress, bool recorded) override {
    const auto found = carries.find(registerAddress);
    if (found == carries.end()) return;
    Carry& carry = found->second;
    if (recorded && !carry.writers.empty()) {
      stage(carry.memory, carry.bytes, carry.writers);
      const auto closed = closedFromBuffer.find(registerAddress);
      const bool matches = closed != closedFromBuffer.end() && closed->second.memory == carry.memory &&
                           closed->second.bytes == carry.bytes;
      if (const std::optional<Cpu65816Register> reg = cpu65816Register(registerAddress)) {
        const std::optional<PortLanding> landing =
            matches ? closed->second.landing : std::optional<PortLanding>{};
        carryOut({carry.memory, carry.bytes},
                CarriedContent{.bytes = carry.values,
                               .origin = originOf(carry.writers),
                               .cls = reg->cls,
                               .kind = MovedKind::Stream,
                               .landing = landing,
                               .unit = 1,
                               .indirect = false,
                               .byteOrigins = carry.byteOrigins,
                               .order = 0},
                landing && !landing->shown);
      }
    }
    closedFromBuffer.erase(registerAddress);
    carries.erase(found);
  }

  void closeChannel(std::uint8_t channel) {
    close(dma[channel]);
    close(table[channel]);
    close(indirect[channel]);
  }

  // A new frame's HDMA walks every table from its start again; and, with the
  // screen on, the frame reads every landing waiting for it.
  void frameBegan() {
    for (std::uint8_t c = 0; c < 8; ++c) {
      close(table[c]);
      close(indirect[c]);
    }
    frameDrawn();
  }

  // The run's end: every range closes, and a landing still waiting for a
  // drawn frame is unshown.
  void finish() {
    for (std::uint8_t c = 0; c < 8; ++c) closeChannel(c);
    for (LandedRange& landing : pendingLandings) record(landing);
    pendingLandings.clear();
    for (StreamedRange& stream : pendingStreams) record(stream);
    pendingStreams.clear();
    for (const PendingContent& pending : pendingContents) resolve(pending);
    pendingContents.clear();
  }

  // A byte of `kind` the channel moved at `address` on the A bus; `read` when
  // the engine read it there, and `value` what crossed the bus.
  void moved(std::uint8_t channel, MovedKind kind, Address address, bool read, std::uint8_t value) {
    std::optional<Open>& open =
        kind == MovedKind::Dma ? dma[channel] : kind == MovedKind::Table ? table[channel] : indirect[channel];
    const DmaChannel& ch = machine.held().dma[channel];
    const Address trigger = kind == MovedKind::Dma ? dmaSite[channel] : hdmaSite[channel];
    const bool toRegister = (ch.dmap & 0x80u) == 0u;
    const Address registerAddress = 0x2100u | ch.bbad;
    // The engine walks a table and an indirect block upward whatever the step
    // bits say; only a general-purpose transfer follows them.
    MovedStep step = MovedStep::Increment;
    if (kind == MovedKind::Dma) {
      const std::uint8_t adjust = (ch.dmap >> 3) & 3u;
      step = adjust == 0u ? MovedStep::Increment : adjust == 2u ? MovedStep::Decrement : MovedStep::Fixed;
    }
    if (open && open->next == address && open->range.site == trigger &&
        open->range.toRegister == toRegister && open->range.registerAddress == registerAddress &&
        open->range.step == step) {
      ++open->range.bytes;
      open->next = stepped(address, step);
      if (read) fold(open->writers, address);
      open->values.push_back(value);
      noteOrigin(open->byteOrigins, address, read);
      return;
    }
    close(open);
    MovedRange range{.site = trigger,
                     .channel = channel,
                     .toRegister = toRegister,
                     .registerAddress = registerAddress,
                     .registerName = {},
                     .registerClass = std::nullopt,
                     .memory = address,
                     .step = step,
                     .bytes = 1,
                     .kind = kind,
                     .times = 1};
    if (const std::optional<Cpu65816Register> reg = cpu65816Register(registerAddress)) {
      range.registerName = reg->name;
      range.registerClass = reg->cls;
    }
    open = Open{.range = range,
                .next = stepped(address, step),
                .writers = {},
                .landing = std::nullopt,
                .unread = false,
                .values = {value},
                .unit = hdmaUnitOf(ch.dmap & 7u),
                .indirect = (ch.dmap & 0x40u) != 0u,
                .byteOrigins = {}};
    if (read) fold(open->writers, address);
    noteOrigin(open->byteOrigins, address, read);
  }

  // The port's access, now that the access that caused it is here. On an
  // engine's behalf a write lands the byte the engine is carrying and a read
  // is what the engine then carries; on the CPU's it is queued for the
  // interpreter, which pairs it with the CPU's own access to `$2180`.
  void settlePort(const BusAccess& cause) {
    const bool engine = (cause.source == AccessSource::Dma || cause.source == AccessSource::Hdma) &&
                        isPort(cause.address);
    if (!engine) {
      (port->write ? shadow.portWrites : shadow.portReads).push_back(port->address);
    } else if (port->write) {
      shadow.written(port->address, carried.value_or(ir::kNoOrigin),
                     ir::Writer{.site = carrier, .engine = true});
      engineWrites.push_back(port->address);
    } else {
      carried = shadow.originOf(port->address).value_or(ir::kNoOrigin);
    }
    port.reset();
  }

  void access(const BusAccess& a) override {
    if (port) settlePort(a);
    if (a.source == AccessSource::WramPort) {
      port = a;
      return;
    }
    if (a.source == AccessSource::Cpu) {
      cpu.access(a);
      if (!a.write) return;
      // A store through a video data port: where the port put the byte is
      // held for the shadow's stream.
      if (a.landed) stepLandings.emplace_back(a.address & 0xFFFFu, *a.landed);
      // Only the two start registers matter here: a write to `MDMAEN` names the
      // site of every byte the channels it selects then move, and closes what
      // those channels had open; a write to `HDMAEN` does the same for the
      // channels it newly enables.
      const std::uint16_t offset = static_cast<std::uint16_t>(a.address & 0xFFFFu);
      if ((offset != kMdmaen && offset != kHdmaen) || !inSystemBank(a.address)) return;
      for (std::uint8_t c = 0; c < 8; ++c) {
        const bool named = ((a.value >> c) & 1u) != 0u;
        if (offset == kMdmaen) {
          if (!named) continue;
          close(dma[c]);
          dmaSite[c] = site;
        } else {
          const bool was = ((hdmaEnabled >> c) & 1u) != 0u;
          if (named && !was) {
            close(table[c]);
            close(indirect[c]);
            hdmaSite[c] = site;
          }
        }
      }
      if (offset == kHdmaen) hdmaEnabled = a.value;
      return;
    }
    if (a.source != AccessSource::Dma && a.source != AccessSource::Hdma) return;
    // What the engine reads past a table's `$00` is not the table's, and belongs
    // to whatever the image holds there.
    if (a.pastTableEnd) return;
    // The A-bus side of a byte is the read when the byte goes to the register
    // and the write when it comes back from one; the other side is the register.
    const bool toRegister = (machine.held().dma[a.channel].dmap & 0x80u) == 0u;
    const MovedKind kind = a.source == AccessSource::Dma ? MovedKind::Dma
                           : a.table                     ? MovedKind::Table
                                                         : MovedKind::Indirect;
    carrier = kind == MovedKind::Dma ? dmaSite[a.channel] : hdmaSite[a.channel];
    if (a.write == toRegister) {
      // The register side. A read from a register is what the engine carries
      // — unless the register is the port, whose own read already said what;
      // a write ends the byte's journey, and where a video data port put the
      // byte joins the landing of the range the byte belongs to.
      if (a.write) {
        carried.reset();
        if (a.landed) {
          const MovedKind was = lastKind[a.channel];
          std::optional<Open>& open =
              was == MovedKind::Dma ? dma[a.channel] : was == MovedKind::Table ? table[a.channel] : indirect[a.channel];
          const std::optional<PortMemory> memory = portMemory(a.address);
          if (open && memory) {
            foldLanding(open->landing, *memory, *a.landed);
            if (*memory == PortMemory::Vram) {
              open->unread = true;
            } else {
              readLanding(*open->landing);
            }
          }
        }
      } else if (!isPort(a.address) || !carried) {
        carried = shadow.at(a.address);
      }
      return;
    }
    lastKind[a.channel] = kind;
    moved(a.channel, kind, a.address, !a.write, a.value);
    if (a.write) {
      shadow.written(a.address, carried.value_or(ir::kNoOrigin),
                     ir::Writer{.site = carrier, .engine = true});
      engineWrites.push_back(a.address);
      carried.reset();
    } else {
      carried = shadow.at(a.address);
    }
  }

  void internal(std::uint32_t address, std::optional<CycleKind> kind) override {
    cpu.internal(address, kind);
  }
};

// The machine and the shadow as they stood at the first instruction of a
// planned routine's first invocation, and that invocation's call: the address
// it returns to and the call's site, both as the tree places them, the stack
// pointer just after the call, and the frame and the master cycles the run had
// counted. The shadow's `carries` is null until a value's run binds it.
struct Snapshot {
  SnesState state;
  ir::Provenance shadow;
  Address returnTo = 0;
  Address enteredFrom = 0;
  std::uint16_t entryS = 0;
  std::uint32_t frame = 0;
  std::uint64_t spent = 0;
};

// What the second run keeps for one planned routine over the whole run: every
// byte of work RAM its frames wrote, as one bit an index; the offsets
// `$2100`-`$43FF` of every register its frames wrote; the master cycles of its
// longest invocation; and its snapshot, once its first invocation is entered.
struct Planned {
  const PlannedRoutine* routine = nullptr;
  std::vector<bool> footprint = std::vector<bool>(0x20000u, false);
  std::set<std::uint16_t> registers;
  std::uint64_t longest = 0;
  std::optional<Snapshot> snapshot;
};

// The second run's record of the planned routines, one entry per routine in
// the plan's order, and which of them are live: each live invocation,
// outermost first, with the shadow's id for it and the master cycles the
// run had counted when it was entered. What the CPU or an engine writes is a
// live routine's only while the running invocation is the routine's own or
// was called from it with no hardware interrupt taken between, so a handler
// that interrupts a routine writes nothing of the routine's, and a routine
// a handler calls has its writes all the same. `spent` is the run's master
// cycles as the loop last counted them; `dueSite` and `dueReturn` the call
// whose routine's snapshot the loop is to take.
struct Enumerating {
  struct Live {
    std::size_t planned;
    std::uint32_t invocation;
    std::uint64_t enteredAt;
  };
  std::vector<Planned> planned;
  std::vector<Live> live;
  std::uint64_t spent = 0;
  Address dueSite = 0;
  Address dueReturn = 0;

  explicit Enumerating(const EnumerationPlan& plan) {
    for (const PlannedRoutine& routine : plan.routines) {
      Planned entry;
      entry.routine = &routine;
      planned.push_back(std::move(entry));
    }
  }

  // A call landed on `landed`, opening `invocation`: when that is a planned
  // routine, its invocation is live, and its snapshot falls due when it has
  // none yet. Answers the routine whose snapshot is due.
  std::optional<std::size_t> entered(Address landed, Address site, Address returnTo, std::uint32_t invocation) {
    std::optional<std::size_t> due;
    for (std::size_t p = 0; p < planned.size(); ++p) {
      if (planned[p].routine->address != landed) continue;
      live.push_back(Live{.planned = p, .invocation = invocation, .enteredAt = spent});
      if (!planned[p].snapshot) {
        due = p;
        dueSite = site;
        dueReturn = returnTo;
      }
      break;
    }
    return due;
  }

  // Every live routine whose invocation has ended leaves, its cycles counting
  // toward its routine's longest.
  void ended(const ir::Provenance& shadow) {
    for (std::size_t i = live.size(); i-- > 0;) {
      if (shadow.live(live[i].invocation)) continue;
      Planned& entry = planned[live[i].planned];
      entry.longest = std::max(entry.longest, spent - live[i].enteredAt);
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }

  // What the CPU and the engines wrote this step, given to every live
  // routine the running invocation is within.
  void wrote(const Recorder& recorder, const ir::Provenance& shadow) {
    for (const Live& l : live) {
      if (!shadow.within(l.invocation)) continue;
      Planned& entry = planned[l.planned];
      for (const BusAccess& access : recorder.cpu.data) {
        if (!access.write) continue;
        if (const std::optional<std::size_t> index = ir::Provenance::workRamIndex(access.address)) {
          entry.footprint[*index] = true;
        } else if (const auto offset = static_cast<std::uint16_t>(access.address & 0xFFFFu);
                   inSystemBank(access.address) && offset >= 0x2100u && offset <= 0x43FFu) {
          entry.registers.insert(offset);
        }
      }
      for (const Address address : recorder.engineWrites) {
        if (const std::optional<std::size_t> index = ir::Provenance::workRamIndex(address)) {
          entry.footprint[*index] = true;
        }
      }
    }
  }
};

// The check of one step, as the rest of the run reads it: what the step was,
// the node's identity and the parts of it the run follows, and the last
// disagreement the check found, which is the one a note names.
struct Checked {
  enum class Kind : std::uint8_t { Halted, EngineHeld, Interrupt, Undecoded, Instruction };
  Kind kind = Kind::Halted;
  bool nmi = false;                     // Interrupt: NMI rather than IRQ
  bool fresh = false;                   // Instruction: the check lifted the node at this step
  Address site = 0;                     // placed
  Address rawSite = 0;
  std::uint32_t modeBits = 0;           // contextOf(mode).bits
  std::array<std::uint8_t, 4> bytes{};  // the instruction's bytes
  std::uint8_t length = 0;
  ir::Flow flow = ir::Flow::Continue;
  ir::Addressing addressing{};
  std::optional<Address> target;
  bool loadsStackPointer = false;
  bool brkOrCop = false;
  std::string_view mnemonic;            // the node's, which lives in the check's cache for the run
  std::uint32_t calls = 0;              // the shadow calls the step made, where a tape keeps them
  std::optional<ir::Divergence> divergence;
};

// Told by the check what the step is once it knows, before the interpreter
// tells the shadow anything: a run with the shadow under its own interpreter
// sets the shadow's site there.
struct CheckHook {
  virtual ~CheckHook() = default;
  virtual void beforeShadow(const Checked& step, const Cpu65816State& before, const Cpu65816State& after) = 0;
};

// The check of one step: the node from the fetched bytes, the interpreter run
// over the step's accesses against the file after, with the shadow told each
// move. Its one state between steps is the interpreter's file, which equals the
// machine's at every boundary, so a check begun at any boundary from
// `registersOf(before)` is the check begun at power-on.
struct StepCheck {
  CartridgeBoard board;
  std::size_t imageBytes;
  const Cpu65816Backend& backend = cpu65816Backend();
  ir::Interpreter interpreter;
  const std::vector<ir::Effect> nmi = ir::interruptSequence(ir::Interrupt::Nmi);
  const std::vector<ir::Effect> irq = ir::interruptSequence(ir::Interrupt::Irq);

  // The nodes lifted so far, one per address, mode and bytes — so bytes the
  // program rewrote at an address are a second node there, and a mirror bank
  // shares the node of the bank the tree places the bytes in.
  using NodeKey = std::tuple<Address, std::uint32_t, std::vector<std::uint8_t>>;
  std::map<NodeKey, ir::Node> nodes;

  StepCheck(const CartridgeBoard& b, std::size_t bytes, const Cpu65816State& start, ir::Shadow* shadow)
      : board(b), imageBytes(bytes) {
    interpreter.registers = ir::registersOf(start);
    interpreter.shadow = shadow;
  }

  // One step, from the file before it to the file after; `ordinal` is the step's
  // index for a divergence. Answers what the step was and what the check found;
  // `hook`, when given, is told before the shadow is.
  Checked step(std::uint64_t ordinal, const Cpu65816State& before, const Cpu65816State& after,
               const ir::StepObserver& observed, CheckHook* hook = nullptr) {
    Checked c;
    if (before.run != CpuRunState::Running) {
      // A halted cycle: nothing ran. If a line ended the wait, the interpreter's
      // wait ends with it.
      if (after.run == CpuRunState::Running) interpreter.release();
      return c;
    }
    // A transfer engine held the bus for the whole step; the instruction is
    // still ahead of the CPU and is seen on the step that runs it.
    if (!observed.cpuRan) {
      c.kind = Checked::Kind::EngineHeld;
      return c;
    }
    c.rawSite = (static_cast<Address>(before.pbr) << 16) | before.pc;
    c.site = placed(board, imageBytes, c.rawSite);
    ir::Divergence prototype;
    prototype.instruction = ordinal;
    prototype.site = c.site;
    std::vector<ir::Divergence> found;

    // What the machine took: a hardware request due at the boundary, or the
    // instruction at the program counter.
    const bool nmiTaken = before.nmiPending;
    const bool irqTaken = !nmiTaken && before.irqLine && (before.p & kCpuFlagI) == 0;
    if (nmiTaken || irqTaken) {
      c.kind = Checked::Kind::Interrupt;
      c.nmi = nmiTaken;
      prototype.name = nmiTaken ? "NMI" : "IRQ";
      if (hook != nullptr) hook->beforeShadow(c, before, after);
      ir::checkInterrupt(interpreter, nmiTaken ? nmi : irq, observed, after, prototype, found);
      if (!found.empty()) c.divergence = std::move(found.back());
      return c;
    }

    // The instruction, from the bytes the CPU fetched, decoded under the mode
    // the CPU was in and placed where the tree places it.
    std::vector<std::uint8_t> bytes;
    for (const BusAccess& fetch : observed.fetches) bytes.push_back(fetch.value);
    const Cpu65816Mode mode = modeOf(before);
    c.modeBits = contextOf(mode).bits;
    NodeKey key{c.site, c.modeBits, bytes};
    auto known = nodes.find(key);
    if (known == nodes.end()) {
      const std::optional<Decoded> decoded = backend.decode(bytes, c.site, c.site, contextOf(mode));
      if (!decoded || decoded->instruction.length != bytes.size()) {
        // The chip fetched bytes the decoder does not read as one instruction of
        // that length. Nothing is known to run, so nothing is checked.
        c.kind = Checked::Kind::Undecoded;
        interpreter.registers = ir::registersOf(after);
        return c;
      }
      known = nodes.emplace(std::move(key), ir::liftInstruction(decoded->instruction, mode)).first;
      c.fresh = true;
    }
    const ir::Node& node = known->second;
    const ir::Instruction& instruction = node.instruction;
    c.kind = Checked::Kind::Instruction;
    std::copy(bytes.begin(), bytes.end(), c.bytes.begin());
    c.length = instruction.length;
    c.flow = instruction.flow;
    c.addressing = instruction.addressing;
    c.target = instruction.target;
    c.loadsStackPointer = ir::loadsStackPointer(instruction);
    c.brkOrCop = instruction.mnemonic == std::string_view("BRK") || instruction.mnemonic == std::string_view("COP");
    c.mnemonic = instruction.mnemonic;
    if (hook != nullptr) hook->beforeShadow(c, before, after);
    ir::checkNode(interpreter, node, observed, after, prototype, found);
    if (!found.empty()) c.divergence = std::move(found.back());
    return c;
  }
};

// The four forms whose destination the bytes do not name and the run reads
// from the pointer instead: their landing is a reached target, never a
// landing the run has to record from where the CPU went.
inline bool indirectForm(ir::Addressing addressing, ir::Flow flow) {
  const bool indirect = addressing == ir::Addressing::AbsoluteIndirect ||
                        addressing == ir::Addressing::AbsoluteIndirectLong ||
                        addressing == ir::Addressing::AbsoluteIndexedIndirect;
  return indirect && (flow == ir::Flow::Jump || flow == ir::Flow::Call);
}

// The interpreter run beside the machine over the whole run: every executed
// instruction lifted from the bytes the CPU fetched, checked, and read for
// where the CPU went next and what its registers held. `check` is the check of
// each step; `follow` is everything the run does with a step once checked, in
// the order the run does it, so a step checked on another thread, its shadow
// calls kept on a tape, is followed here as the step checked here is.
struct Lockstep final : CheckHook {
  CartridgeBoard board;
  std::size_t imageBytes;
  ir::Provenance& shadow;
  const Cpu65816Backend& backend = cpu65816Backend();
  StepCheck check;

  // Every node the checks lifted, by address, mode and bytes: a step's node
  // joins when the check that met it lifted it then.
  using NodeKey = StepCheck::NodeKey;
  std::set<NodeKey> nodeKeys;

  // Where the run's own flow says execution may arrive without an instruction
  // naming it: the address after every call taken, for its return, and every
  // instruction a hardware interrupt interrupted, for the handler's `RTI`. All
  // as the tree places them.
  std::set<Address> expectedReturns;

  // What the run recorded.
  std::set<std::tuple<Address, Address, std::uint32_t>> landings;  // site, target, mode
  std::vector<Landing> ran;
  struct Values {
    std::set<std::uint16_t> d;
    std::set<std::uint8_t> dbr;
  };
  std::map<Address, Values> seen;
  std::set<Address> notedSites;  // a site already named in the notes
  Address expectedNext = 0;      // where falling through the last instruction leads, raw
  std::vector<ir::Divergence> divergences;  // each step's last, in order
  std::uint64_t instructions = 0;
  std::uint64_t interrupts = 0;
  std::uint64_t diverged = 0;
  std::uint64_t steps = 0;

  // The second run's record of the planned routines, told of every call and
  // of every invocation that ends; nothing in the first run. `snapshotDue` names the
  // planned routine whose snapshot the loop takes once this step is done.
  Enumerating* enumerating = nullptr;
  std::optional<std::size_t> snapshotDue;

  Lockstep(const CartridgeBoard& b, std::size_t bytes, const Cpu65816State& start, ir::Provenance& p)
      : board(b), imageBytes(bytes), shadow(p), check{b, bytes, start, &p} {}

  // What the port did on the CPU's behalf this step is paired with the CPU's
  // accesses as the interpreter runs them; whatever is left over is dropped
  // with the step.
  void clearPort() {
    shadow.portReads.clear();
    shadow.portWrites.clear();
  }

  std::string modeText(const Cpu65816Mode& mode) const {
    return backend.describe(contextOf(mode));
  }

  // One step the machine took, from the state before it to the state after,
  // checked and followed here, the shadow under the check's own interpreter.
  void step(const Cpu65816State& before, const Cpu65816State& after,
            const ir::StepObserver& observed, std::vector<std::string>& notes) {
    const Checked checked = check.step(steps, before, after, observed, this);
    follow(checked, before, after, notes);
  }

  // What the shadow is told about a step before the interpreter's moves: the
  // site it runs under, whether the CPU fell through to it, and for an
  // interrupt the stack it pushed; and the values seen at the site.
  void beforeShadow(const Checked& c, const Cpu65816State& before, const Cpu65816State& after) override {
    // The stack pointer wraps inside page one while the CPU is in emulation mode.
    const bool page = before.e && after.e;
    if (c.kind == Checked::Kind::Interrupt) {
      shadow.site = c.site;
      shadow.flowBroke();
      shadow.interrupted(before.s, after.s, page);
      if (enumerating != nullptr) enumerating->ended(shadow);
      return;
    }
    // What the run saw at the site, before the instruction ran.
    if (inImage(board, imageBytes, c.rawSite)) {
      Values& values = seen[c.site];
      values.d.insert(before.d);
      values.dbr.insert(before.dbr);
    }
    // The shadow follows the instruction under its site, told when the CPU did
    // not fall through to it — which is what a stream's straight run means.
    shadow.site = c.site;
    if (c.rawSite != expectedNext) shadow.flowBroke();
    expectedNext = backend.following(c.rawSite, c.length);
  }

  // Everything the run does with a checked step. A step checked with its
  // shadow calls kept on `tape` — `c.calls` of them from `from` — is told to
  // the shadow here first, the tape played where the check ran; a step checked
  // with the shadow under the check's interpreter was told already.
  void follow(const Checked& c, const Cpu65816State& before, const Cpu65816State& after,
              std::vector<std::string>& notes, const ir::Tape* tape = nullptr, std::size_t from = 0) {
    ++steps;
    const bool page = before.e && after.e;
    if (tape != nullptr && (c.kind == Checked::Kind::Interrupt || c.kind == Checked::Kind::Instruction)) {
      beforeShadow(c, before, after);
      tape->play(shadow, from, c.calls);
    }
    switch (c.kind) {
      case Checked::Kind::Halted:
      case Checked::Kind::EngineHeld:
        clearPort();
        return;
      case Checked::Kind::Interrupt:
        ++interrupts;
        expectedReturns.insert(c.site);
        if (c.divergence) {
          divergences.push_back(*c.divergence);
          noteDivergence(c.nmi ? "NMI" : "IRQ", c.site, before, notes);
          shadow.forgetPlaces();
        }
        clearPort();
        return;
      case Checked::Kind::Undecoded:
        // Said once: nothing is known to run, so nothing is checked.
        if (notedSites.insert(c.site).second) {
          notes.push_back("run: the bytes the CPU fetched at " + formatAddress(c.site, 24) +
                          " do not decode as one instruction under " + modeText(modeOf(before)) +
                          "; the step is not checked");
        }
        shadow.forgetPlaces();
        if (before.s != after.s) shadow.stackMoved(before.s, after.s, page);
        clearPort();
        return;
      case Checked::Kind::Instruction:
        break;
    }

    if (c.fresh) nodeKeys.insert(NodeKey{c.site, c.modeBits, std::vector<std::uint8_t>(c.bytes.begin(), c.bytes.begin() + c.length)});
    ++instructions;
    if (c.divergence) {
      divergences.push_back(*c.divergence);
      noteDivergence(describeNode(c), c.site, before, notes);
      shadow.forgetPlaces();
    }
    clearPort();
    // What the step did to the stack pointer: a call opens an invocation
    // marked where its pushes left the pointer; a push or a pull may pass a
    // mark and end one; a load of the pointer from a register is not told.
    if (c.flow == ir::Flow::Call) {
      shadow.called(before.s, after.s, page);
    } else if (before.s != after.s && !c.loadsStackPointer) {
      shadow.stackMoved(before.s, after.s, page);
    }
    if (enumerating != nullptr) followRoutines(c, after);

    // Where the CPU went, against what the instruction names: the address
    // after it, for a form that falls through; its constant target; the
    // pointer's target, for the four forms the run reads ahead; the vector, for
    // a software interrupt; and for a return, an address the run's own calls
    // and interrupts said to expect one at. A call's return is expected after
    // it, whichever instruction returns there.
    const Address following = backend.following(c.site, c.length);
    if (c.flow == ir::Flow::Call) expectedReturns.insert(following);
    if (after.run != CpuRunState::Running) return;  // a wait or a stop: no landing
    const Address rawLanded = (static_cast<Address>(after.pbr) << 16) | after.pc;
    const Address landed = placed(board, imageBytes, rawLanded);
    const bool fallsThrough = c.flow == ir::Flow::Continue || c.flow == ir::Flow::Branch || c.flow == ir::Flow::Call;
    const bool named =
        rawLanded == c.rawSite ||  // a block move with bytes left, run again
        (fallsThrough && landed == following) ||
        (c.target && landed == placed(board, imageBytes, *c.target)) ||
        indirectForm(c.addressing, c.flow) ||  // a reached target
        c.brkOrCop ||                          // the vector the header names
        (c.flow == ir::Flow::Return && expectedReturns.count(landed) != 0);
    if (named) return;
    if (!inImage(board, imageBytes, rawLanded)) {
      if (notedSites.insert(c.site).second) {
        notes.push_back("run: the CPU arrived at " + formatAddress(rawLanded, 24) + " from " +
                        formatAddress(c.site, 24) + ", which the tree does not hold; not recorded");
      }
      return;
    }
    // The landing is where the CPU arrived, in the bank it arrived in; the
    // disassembler places it to trace from it.
    const Cpu65816Mode arrived = modeOf(after);
    if (!landings.insert({c.site, rawLanded, contextOf(arrived).bits}).second) return;
    ran.push_back(Landing{.target = rawLanded, .mode = arrived, .site = c.site, .name = {}});
  }

  // The second run's record told what a step did to the planned routines'
  // frames: a call, not a software interrupt, that lands on a planned routine
  // enters it; every live routine whose invocation the step ended leaves.
  void followRoutines(const Checked& c, const Cpu65816State& after) {
    if (c.flow == ir::Flow::Call && !c.brkOrCop) {
      const Address landed = placed(board, imageBytes, (static_cast<Address>(after.pbr) << 16) | after.pc);
      const Address returnTo = placed(board, imageBytes, backend.following(c.site, c.length));
      const std::optional<std::size_t> due = enumerating->entered(landed, c.site, returnTo, shadow.running());
      if (due && !snapshotDue) snapshotDue = due;
    }
    enumerating->ended(shadow);
  }

  static std::string describeNode(const Checked& c) {
    return "`" + std::string(c.mnemonic) + " " + std::string(ir::addressingName(c.addressing)) + "`";
  }

  // A disagreement, said once per site with what disagreed: the last the
  // check found.
  void noteDivergence(const std::string& what, Address site, const Cpu65816State& before,
                      std::vector<std::string>& notes) {
    ++diverged;
    if (!notedSites.insert(site).second) return;
    const ir::Divergence& d = divergences.back();
    char values[64];
    std::snprintf(values, sizeof values, "machine $%X, the lift $%X", d.expected, d.actual);
    notes.push_back("run: the lift of " + what + " at " + formatAddress(site, 24) + " under " +
                    modeText(modeOf(before)) + " disagreed with the machine (" + d.what + ": " +
                    values + "); the interpreter was realigned");
  }
};

// One value to run a routine for: the register, its width and the value.
struct VariantSetting {
  InputRegister reg = InputRegister::X;
  unsigned bits = 8;
  std::uint32_t value = 0;
};

// A value as a note names it: the register, `=`, and `$XX` at eight bits or
// `$XXXX` at sixteen.
inline std::string variantText(const VariantSetting& setting) {
  return std::string(inputRegisterName(setting.reg)) + "=$" + text::hex(setting.value, setting.bits == 8u ? 2 : 4);
}

// The snapshot's state with the value in its register: the accumulator's low
// byte alone at eight bits, its high byte as the snapshot held it; an index
// whole, its high byte zero at eight bits, as the chip keeps an eight-bit index.
inline void setInput(SnesState& state, const VariantSetting& setting) {
  const auto value = static_cast<std::uint16_t>(setting.value);
  switch (setting.reg) {
    case InputRegister::A:
      state.cpu.a = setting.bits == 16u ? value : static_cast<std::uint16_t>((state.cpu.a & 0xFF00u) | (value & 0xFFu));
      return;
    case InputRegister::X: state.cpu.x = value; return;
    case InputRegister::Y: state.cpu.y = value; return;
  }
}

// One value: the sandbox restored from the snapshot with the input set, run
// beside `step` and `rec` until the routine returns, refuses or runs out of
// budget. Answers the run, or nothing with the refusing note in `notes`.
//
// The caller restores `sandbox`, binds `copy` — the snapshot's shadow — to
// `rec`, and builds `step` over `copy` from the restored registers, expecting
// the routine's first instruction next and its return; the routine's
// invocation is `copy`'s running one, and a write is its frames' while
// `copy.within` it; a `copy` reporting
// anywhere but `rec` throws `std::logic_error`. Each step is checked in
// this order: the lifted instruction disagreeing with the machine refuses the
// value; so does a byte of work RAM or a register `$2100`-`$43FF` the routine's
// frames write that `planned` does not hold, an interrupt handler's writes
// excepted; the program counter at the snapshot's return with the stack pointer
// above the entry's ends the run; and 64 times the routine's longest
// invocation, and no less than one second of the master clock, refuses it. The
// master cycles spent are added to `cycles` either way.
//
// A run that returns answers the work-RAM bytes the routine's frames wrote
// that a class covers. A written byte's classes are the bits `recordedClasses`
// holds at its work-RAM index — the classes of the recorded staged extents
// holding it, `1u << class`; an empty span holds none — and the classes of the
// value's own transfers that carried it: every range `rec` moved to a register
// with a class, stepping, from work RAM. The extents are the maximal runs of
// consecutive written bytes with the same non-empty class set, in address
// order, each with its bytes as work RAM holds them, its origin and its sources
// as `copy` reads them, its classes and the own transfers that carried any of
// its bytes. A written byte no class covers sets its index in `scratch` and
// sets `wroteScratch`; a refused value touches neither. A run that wrote no
// byte of work RAM answers no extent, noted.
[[nodiscard]] inline std::optional<EnumeratedRun> runVariant(const Snapshot& snapshot, const Planned& planned,
                                                             std::span<const std::uint16_t> recordedClasses,
                                                             std::vector<bool>& scratch, bool& wroteScratch,
                                                             const VariantSetting& setting, Snes& sandbox, Lockstep& step,
                                                             Recorder& rec, ir::Provenance& copy,
                                                             std::span<const std::uint8_t> rom, const CartridgeBoard& board,
                                                             const InputScript& input, std::string_view label,
                                                             std::vector<std::string>& notes, std::uint64_t& cycles) {
  constexpr std::uint64_t kOneSecond = 21'477'272u;
  constexpr std::uint64_t kFactor = 64u;
  if (copy.carries != &rec) throw std::logic_error("a value's shadow reports to the value's own recorder");
  const std::string head = "enumerate " + std::string(label) + " " + variantText(setting) + ": ";
  const std::uint64_t budget = std::max(kFactor * planned.longest, kOneSecond);
  std::vector<std::string> dropped;
  std::vector<bool> written(0x20000u, false);
  std::uint32_t frame = snapshot.frame;
  std::uint64_t spent = 0;
  const std::uint32_t routine = copy.running();
  const auto refuse = [&](const std::string& why) {
    notes.push_back(head + why);
    cycles += spent;
    return std::optional<EnumeratedRun>{};
  };
  for (;;) {
    const SnesState& before = sandbox.state();
    const Cpu65816State cpuBefore = before.cpu;
    const Address site = (static_cast<Address>(before.cpu.pbr) << 16) | before.cpu.pc;
    const std::uint16_t lineBefore = before.vpos;
    rec.stepBegan(site);
    spent += sandbox.step();
    if (sandbox.state().vpos < lineBefore) {
      ++frame;
      presentPads(sandbox, input, frame);
      rec.frameBegan();
    }
    const SnesState& after = sandbox.state();
    step.step(cpuBefore, after.cpu, rec.cpu, dropped);

    if (step.diverged > 0) {
      return refuse("the lift disagreed with the machine at " + text::address24(step.divergences.back().site) +
                    "; not lifted");
    }
    if (copy.within(routine)) {
      for (const BusAccess& access : rec.cpu.data) {
        if (!access.write) continue;
        if (const std::optional<std::size_t> index = ir::Provenance::workRamIndex(access.address)) {
          if (!planned.footprint[*index]) {
            return refuse("wrote " + text::address24(ir::Provenance::workRamAddress(*index)) +
                          ", outside what the run saw the routine write; not lifted");
          }
          written[*index] = true;
          continue;
        }
        const auto offset = static_cast<std::uint16_t>(access.address & 0xFFFFu);
        if (!inSystemBank(access.address) || offset < 0x2100u || offset > 0x43FFu) continue;
        if (planned.registers.count(offset) == 0) {
          const std::string_view name = cpu65816RegisterName(offset);
          return refuse("wrote " + (name.empty() ? text::address24(access.address) : std::string(name)) +
                        ", a register the run never saw the routine write; not lifted");
        }
      }
      for (const Address address : rec.engineWrites) {
        const std::optional<std::size_t> index = ir::Provenance::workRamIndex(address);
        if (!index) continue;
        if (!planned.footprint[*index]) {
          return refuse("wrote " + text::address24(ir::Provenance::workRamAddress(*index)) +
                        ", outside what the run saw the routine write; not lifted");
        }
        written[*index] = true;
      }
    }
    const Address at = placed(board, rom.size(), (static_cast<Address>(after.cpu.pbr) << 16) | after.cpu.pc);
    if (at == snapshot.returnTo && after.cpu.s > snapshot.entryS) break;
    if (spent >= budget) return refuse("did not return within " + std::to_string(budget) + " cycles; not lifted");
  }
  cycles += spent;

  // The value's own transfers from work RAM, each with the work-RAM indices it
  // read. A channel's last range is still open in the recorder, so every
  // channel is closed first.
  for (std::uint8_t c = 0; c < 8; ++c) rec.closeChannel(c);
  struct OwnSpan {
    OwnCarry carry;
    std::size_t first = 0;
    std::size_t end = 0;
  };
  std::vector<OwnSpan> own;
  for (const MovedRange& range : rec.out) {
    if (!range.toRegister || !range.registerClass || range.step == MovedStep::Fixed) continue;
    const std::optional<std::size_t> index = ir::Provenance::workRamIndex(extentStart(range));
    if (!index) continue;
    own.push_back(OwnSpan{.carry = OwnCarry{.cls = *range.registerClass,
                                            .kind = range.kind,
                                            .registerAddress = range.registerAddress,
                                            .site = placed(board, rom.size(), range.site)},
                          .first = *index,
                          .end = *index + range.bytes});
  }
  const auto bitsAt = [&](std::size_t index) {
    std::uint16_t bits = index < recordedClasses.size() ? recordedClasses[index] : std::uint16_t{0};
    for (const OwnSpan& span : own) {
      if (index >= span.first && index < span.end) bits |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(span.carry.cls));
    }
    return bits;
  };

  EnumeratedRun run{.routine = planned.routine->address,
                    .reg = setting.reg,
                    .bits = setting.bits,
                    .values = {setting.value},
                    .enteredFrom = snapshot.enteredFrom,
                    .extents = {}};
  const SnesState& state = sandbox.state();
  bool wroteAny = false;
  for (std::size_t i = 0; i < written.size();) {
    if (!written[i]) {
      ++i;
      continue;
    }
    wroteAny = true;
    const std::uint16_t bits = bitsAt(i);
    if (bits == 0u) {
      scratch[i] = true;
      wroteScratch = true;
      ++i;
      continue;
    }
    std::size_t end = i + 1;
    while (end < written.size() && written[end] && bitsAt(end) == bits) ++end;
    EnumeratedExtent extent{.memory = ir::Provenance::workRamAddress(i),
                            .bytes = static_cast<std::uint32_t>(end - i),
                            .values = {},
                            .origin = {},
                            .sources = {},
                            .classes = {},
                            .carried = {}};
    for (unsigned cls = 0; cls < 16u; ++cls) {
      if ((bits & (1u << cls)) != 0u) extent.classes.push_back(static_cast<RegisterClass>(cls));
    }
    for (const OwnSpan& span : own) {
      if (span.first < end && i < span.end) extent.carried.push_back(span.carry);
    }
    ir::OriginSet sources;
    for (std::size_t k = i; k < end; ++k) {
      const Address address = ir::Provenance::workRamAddress(k);
      extent.values.push_back(state.wram[k]);
      if (const std::optional<ir::Origin> origin = copy.originOf(address)) {
        copy.origins().accumulate(extent.origin, *origin);
      }
      ir::OriginSet more;
      more.image = copy.sourcesOf(address);
      ir::Origins::merge(sources, more);
    }
    extent.sources = std::move(sources.image);
    run.extents.push_back(std::move(extent));
    i = end;
  }
  if (!wroteAny) notes.push_back(head + "wrote no byte of work RAM; nothing to lift");
  return run;
}

inline std::optional<std::uint16_t> readWord(const CartridgeBoard& board, std::span<const std::uint8_t> rom,
                                             const SnesState& state, Address address) {
  const std::optional<std::uint8_t> low = readByte(board, rom, state, address);
  const std::optional<std::uint8_t> high = readByte(board, rom, state, address + 1u);
  if (!low || !high) return std::nullopt;
  return static_cast<std::uint16_t>(*low | (*high << 8));
}

// Where the instruction at the CPU's position is about to go, when it is one of
// the four forms whose pointer the bytes do not name — read the way the CPU is
// about to read it. Nothing for any other instruction; nothing, with `unreadable`
// set, when the pointer lies where the run cannot see.
struct Pending {
  Address target = 0;
  bool call = false;
};

inline std::optional<Pending> pendingTarget(const CartridgeBoard& board, std::span<const std::uint8_t> rom,
                                            const SnesState& state, bool& unreadable) {
  const Cpu65816State& cpu = state.cpu;
  const Address site = (static_cast<Address>(cpu.pbr) << 16) | cpu.pc;
  const std::optional<std::uint8_t> opcode = readByte(board, rom, state, site);
  if (!opcode) return std::nullopt;
  const Cpu65816Opcode& info = cpu65816Opcodes()[*opcode];
  const bool indirect = info.mode == Cpu65816Addressing::AbsoluteIndirect ||
                        info.mode == Cpu65816Addressing::AbsoluteIndirectLong ||
                        info.mode == Cpu65816Addressing::AbsoluteIndexedIndirect;
  if (!indirect || (info.flow != Flow::Jump && info.flow != Flow::Call)) return std::nullopt;

  // The operand follows the opcode within the program bank.
  const Address operandAt = (site & 0xFF0000u) | ((cpu.pc + 1u) & 0xFFFFu);
  const std::optional<std::uint16_t> operand = readWord(board, rom, state, operandAt);
  if (!operand) {
    unreadable = true;
    return std::nullopt;
  }

  const Address programBank = site & 0xFF0000u;
  std::optional<Address> target;
  switch (info.mode) {
    // `(!abs)`: a two-byte pointer in bank zero; the program bank is unchanged.
    case Cpu65816Addressing::AbsoluteIndirect:
      if (const std::optional<std::uint16_t> ptr = readWord(board, rom, state, *operand)) {
        target = programBank | *ptr;
      }
      break;
    // `[!abs]`: a three-byte pointer in bank zero; the third byte is the bank.
    case Cpu65816Addressing::AbsoluteIndirectLong: {
      const std::optional<std::uint16_t> low = readWord(board, rom, state, *operand);
      const std::optional<std::uint8_t> bank = readByte(board, rom, state, *operand + 2u);
      if (low && bank) target = (static_cast<Address>(*bank) << 16) | *low;
      break;
    }
    // `(!abs,X)`: the operand plus X addresses a two-byte pointer in the program
    // bank, wrapping within it; the program bank is unchanged.
    case Cpu65816Addressing::AbsoluteIndexedIndirect: {
      const Address pointerAt = programBank | ((*operand + cpu.x) & 0xFFFFu);
      if (const std::optional<std::uint16_t> ptr = readWord(board, rom, state, pointerAt)) {
        target = programBank | *ptr;
      }
      break;
    }
    default:
      break;
  }
  if (!target) {
    unreadable = true;
    return std::nullopt;
  }
  return Pending{.target = *target, .call = info.flow == Flow::Call};
}

// The cap on an origin's intervals, above which it is widened to its hull:
// set by exactness — raised until no staged range on a real cartridge is
// approximate — and never by cost.
constexpr std::size_t kOriginCap = 64;

// One sound-CPU step as the check saw it: what the step was, the node's text
// for a note, and the last disagreement the check found.
struct SoundChecked {
  enum class Kind : std::uint8_t { Halted, Undecoded, NotFetched, Instruction };
  Kind kind = Kind::Halted;
  std::uint16_t site = 0;
  std::string_view mnemonic;  // the node's, which lives in the check's cache for the run
  std::string_view form;
  std::optional<ir::Divergence> divergence;
};

// The check of the sound CPU's steps. At every boundary the instruction about
// to run is decoded from the bytes at the program counter as the machine holds
// them then — the image mapped over the boot-ROM window included — lifted, and
// held until the boundary after says what the machine did: the accesses
// between, with the instruction's own fetches set apart, the registers after
// and the cycles. The fetched bytes are held to the decoded ones too, so an
// instruction whose bytes changed between the boundary and its fetch is
// answered as not fetched rather than checked against the wrong node. Its
// state between steps is the interpreter's file and the instruction decoded at
// the last boundary, so a check begun from a boundary's file, with the bytes at
// its program counter as they stood there, is the check begun at power-on.
struct Spc700Check {
  MachineView machine;
  ir::Spc700Interpreter interpreter;
  std::vector<ir::Spc700Access> accesses;  // since the last boundary, in order

  // The nodes lifted so far, one per address and bytes — so bytes the program
  // rewrote at an address are a second node there — and the keys lifted since
  // the last step was answered.
  using NodeKey = std::pair<std::uint16_t, std::vector<std::uint8_t>>;
  std::map<NodeKey, ir::Node> nodes;
  std::vector<NodeKey> lifted;

  // The instruction about to run, decoded at the last boundary; nothing when
  // the core is halted or the bytes there do not decode.
  struct Upcoming {
    std::uint16_t pc = 0;
    std::vector<std::uint8_t> bytes;
    const ir::Node* node = nullptr;
  };
  std::optional<Upcoming> pending;
  std::uint64_t steps = 0;

  Spc700Check(MachineView m, const Spc700State& start) : machine(m) { restart(start); }

  // The check begins again from `cpu`: the interpreter takes its file, the
  // accesses so far are dropped, and the instruction at its program counter is
  // decoded.
  void restart(const Spc700State& cpu) {
    interpreter.registers = ir::registersOf(cpu);
    accesses.clear();
    prepare(cpu);
  }

  void access(std::uint16_t address, std::uint8_t value, bool write) {
    accesses.push_back(ir::Spc700Access{.address = address, .value = value, .write = write});
  }

  // One boundary the machine crossed, from the file before the step to the file
  // after it and the cycles between.
  SoundChecked step(const Spc700State& before, const Spc700State& after, std::uint32_t cycles) {
    const std::uint64_t ordinal = steps++;
    SoundChecked c;
    if (before.run == RunState::Running) {
      c = checkStep(ordinal, before, after, cycles);
    } else if (after.run == RunState::Running) {
      // Nothing ends a halt but a reset; the interpreter follows the machine.
      interpreter.registers = ir::registersOf(after);
    }
    accesses.clear();
    prepare(after);
    return c;
  }

  // Decodes and lifts the instruction at `state`'s program counter from the
  // bytes the machine holds there now, ahead of the step that runs it.
  void prepare(const Spc700State& state) {
    pending.reset();
    if (state.run != RunState::Running) return;
    std::array<std::uint8_t, 3> window{};
    for (std::size_t i = 0; i < window.size(); ++i) {
      window[i] = machine.peekApu(static_cast<std::uint16_t>(state.pc + i));
    }
    const std::optional<Instruction> decoded = decodeAt(window, state.pc, state.pc);
    if (!decoded) return;
    Upcoming next;
    next.pc = state.pc;
    next.bytes = decoded->bytes;
    NodeKey key{state.pc, next.bytes};
    auto found = nodes.find(key);
    if (found == nodes.end()) {
      lifted.push_back(key);
      found = nodes.emplace(std::move(key), ir::liftSpc700Instruction(*decoded)).first;
    }
    next.node = &found->second;
    pending = std::move(next);
  }

 private:
  SoundChecked checkStep(std::uint64_t ordinal, const Spc700State& before, const Spc700State& after,
                         std::uint32_t cycles) {
    SoundChecked c;
    const std::uint16_t site = before.pc;
    c.site = site;
    // A reload moved the CPU since the last boundary: the bytes now at the
    // program counter are the best reading there is.
    if (!pending || pending->pc != site) prepare(before);
    if (!pending) {
      c.kind = SoundChecked::Kind::Undecoded;
      interpreter.registers = ir::registersOf(after);
      return c;
    }
    const std::optional<ir::Spc700StepAccesses> split =
        ir::splitSpc700Step(accesses, site, pending->bytes.size());
    bool fetchedAsDecoded = split.has_value();
    if (split) {
      for (std::size_t i = 0; i < split->fetches.size(); ++i) {
        if (split->fetches[i].value != pending->bytes[i]) fetchedAsDecoded = false;
      }
    }
    if (!fetchedAsDecoded) {
      c.kind = SoundChecked::Kind::NotFetched;
      interpreter.registers = ir::registersOf(after);
      return c;
    }
    ir::Divergence prototype;
    prototype.instruction = ordinal;
    prototype.site = site;
    std::vector<ir::Divergence> found;
    ir::checkSpc700Node(interpreter, *pending->node, split->data, cycles, after, prototype, found);
    c.kind = SoundChecked::Kind::Instruction;
    c.mnemonic = pending->node->instruction.mnemonic;
    c.form = pending->node->instruction.form;
    if (!found.empty()) c.divergence = std::move(found.back());
    return c;
  }
};

// The sound interpreter run beside the audio machine over the whole run: each
// step checked (`Spc700Check`) and followed here.
//
// The same accesses keep a copy of the DSP's register file: a write to `$F2`
// selects a register and a write to `$F3` sets it, and a write to `KON` with a
// bit set is a key-on of each voice named. At each key-on the sample the DSP is
// about to play is read from the audio memory as the machine holds it then —
// the directory entry `DIR` and the voice's `SRCN` select, through the
// machine's own reading of it, and the blocks from its start to the first
// carrying the end flag, stopping at the memory's end — and kept once per
// distinct sample with a count.
//
// It keeps its own notes, which the run gives after the main CPU's: on a
// machine built with `apuThread` its reports arrive on the audio thread.
struct AudioLockstep final : ApuObserver {
  // The DSP's address register and its register file as the CPU last wrote
  // them; a write to `$F3` while the address is $80 or above sets nothing,
  // since those are the file's read-only mirrors.
  static constexpr std::uint8_t kDspRegisters = 0x80u;
  static constexpr std::uint16_t kDspAddressPort = 0x00F2u;
  static constexpr std::uint16_t kDspDataPort = 0x00F3u;
  static constexpr std::uint8_t kDspKon = 0x4Cu;
  static constexpr std::uint8_t kDspDir = 0x5Du;
  static constexpr std::uint8_t kDspSrcn = 0x04u;  // voice v's source number is at v * $10 + 4
  static constexpr std::size_t kAudioMemory = 65536;
  static constexpr std::size_t kBrrBlock = 9;

  MachineView machine;
  Spc700Check check;
  std::vector<std::string> notes;
  std::uint8_t dspAddress = 0;
  std::array<std::uint8_t, kDspRegisters> dsp{};
  std::vector<KeyedSample> samples;
  std::set<std::uint16_t> unendedStarts;      // a start already noted as having no end flag
  std::set<Spc700Check::NodeKey> nodeKeys;    // every node a check lifted
  std::set<std::uint16_t> notedSites;         // a site already named in the notes
  std::vector<ir::Divergence> divergences;    // each step's last, in order
  std::uint64_t instructions = 0;
  std::uint64_t diverged = 0;

  explicit AudioLockstep(MachineView m) : machine(m), check(m, m.held().apu.cpu) { took(check.lifted); }

  // The console's reset line went through the audio machine: the check takes
  // the sound CPU's registers as the reset left them and the instruction at its
  // program counter, the accesses before the reset are dropped, and the copy of
  // the DSP's address and register file is read again from the machine.
  void restart() {
    check.restart(machine.held().apu.cpu);
    took(check.lifted);
    restartDsp();
  }
  void restartDsp() {
    const ApuState& apu = machine.held().apu;
    dspAddress = apu.dspAddr;
    std::copy_n(apu.dsp.regs.begin(), kDspRegisters, dsp.begin());
  }

  void access(std::uint16_t address, std::uint8_t value, bool write) override {
    check.access(address, value, write);
    wrote(address, value, write);
  }

  // What an access does to the copy of the DSP's register file.
  void wrote(std::uint16_t address, std::uint8_t value, bool write) {
    if (!write) return;
    if (address == kDspAddressPort) {
      dspAddress = value;
    } else if (address == kDspDataPort && dspAddress < kDspRegisters) {
      dsp[dspAddress] = value;
      if (dspAddress == kDspKon && value != 0u) keyOn(value);
    }
  }

  // Each voice `mask` names keyed on: its sample read from the audio memory as
  // the machine holds it now — the memory itself, which is what the DSP reads,
  // not the CPU's view with the boot ROM over its window — and kept once with
  // its count.
  void keyOn(std::uint8_t mask) {
    const std::span<const std::uint8_t, kAudioMemory> ram(machine.held().apu.ram);
    for (unsigned voice = 0; voice < 8; ++voice) {
      if ((mask & (1u << voice)) == 0u) continue;
      const std::uint8_t srcn = dsp[(voice << 4) | kDspSrcn];
      const BrrSource source = readBrrSource(ram, dsp[kDspDir], srcn);
      KeyedSample sample{.start = source.start, .loop = source.loop, .bytes = {}, .times = 1};
      // The blocks from the start to the first carrying the end flag; a walk
      // that reaches the end of the audio memory without one names nothing.
      bool ended = false;
      std::size_t at = source.start;
      while (at + kBrrBlock <= kAudioMemory) {
        const std::uint8_t header = ram[at];
        for (std::size_t k = 0; k < kBrrBlock; ++k) sample.bytes.push_back(ram[at + k]);
        at += kBrrBlock;
        if ((header & 0x01u) != 0u) {
          ended = true;
          break;
        }
      }
      if (!ended) {
        if (unendedStarts.insert(source.start).second) {
          notes.push_back("run: the sample at " + formatAddress(source.start, 16) + " voice " +
                          std::to_string(voice) +
                          " keyed on reaches the end of the audio memory without an end flag; not recorded");
        }
        continue;
      }
      const auto known = std::find_if(samples.begin(), samples.end(),
                                      [&](const KeyedSample& s) { return sameSample(s, sample); });
      if (known == samples.end()) {
        samples.push_back(std::move(sample));
      } else {
        ++known->times;
      }
    }
  }

  void instruction(const Spc700State& before, const Spc700State& after, std::uint32_t cycles) override {
    const SoundChecked checked = check.step(before, after, cycles);
    took(check.lifted);
    follow(checked);
  }

  // The nodes a check lifted join the count, and its list is emptied.
  void took(std::vector<Spc700Check::NodeKey>& keys) {
    for (Spc700Check::NodeKey& key : keys) nodeKeys.insert(std::move(key));
    keys.clear();
  }

  // Everything the run does with a checked step: its notes and its counts.
  void follow(const SoundChecked& c) {
    switch (c.kind) {
      case SoundChecked::Kind::Halted: return;
      case SoundChecked::Kind::Undecoded:
        note(c.site, "run: the bytes at " + formatAddress(c.site, 16) +
                         " on the sound CPU do not decode as one instruction; the step is not checked");
        return;
      case SoundChecked::Kind::NotFetched:
        note(c.site, "run: the sound CPU's step at " + formatAddress(c.site, 16) +
                         " did not fetch the instruction decoded there; the step is not checked");
        return;
      case SoundChecked::Kind::Instruction: break;
    }
    ++instructions;
    if (!c.divergence) return;
    ++diverged;
    divergences.push_back(*c.divergence);
    const ir::Divergence& d = divergences.back();
    char values[64];
    std::snprintf(values, sizeof values, "machine $%X, the lift $%X", d.expected, d.actual);
    note(c.site, "run: the lift of `" + std::string(c.mnemonic) + " " + std::string(c.form) + "` at " +
                     formatAddress(c.site, 16) + " on the sound CPU disagreed with the machine (" + d.what + ": " +
                     values + "); the interpreter was realigned");
  }

  // A note about a site, said once.
  void note(std::uint16_t site, std::string text) {
    if (notedSites.insert(site).second) notes.push_back(std::move(text));
  }
};

}  // namespace snaggletooth::disasm::detail

namespace snaggletooth::disasm {

// The run, bound to a machine the host runs, or to a state a worker keeps
// current from a buffer. The machine reports every boundary `step()` returns
// at; `begin` reads, at a boundary, what the step about to run is going to do —
// the pointer an indirect form is about to read, the stack and the line before
// it — and `instruction` is the step seen from the boundary after it: the frame
// the beam began, the step checked against the lifted node, the pointer
// confirmed by the landing. Nothing here presents a pad or takes a snapshot; a
// host does both between its calls.
struct ObservedRun::Impl final : InstructionObserver {
  Snes* host = nullptr;  // the machine the observers are set on; none for a kept state
  detail::MachineView machine;
  std::span<const std::uint8_t> rom;
  CartridgeBoard board;
  ir::Provenance shadow;
  detail::Recorder recorder;
  detail::Lockstep lockstep;
  detail::AudioLockstep audio;
  std::optional<detail::Enumerating> enumerating;
  std::vector<ReachedTarget> reached;
  std::set<std::tuple<Address, Address, std::uint32_t>> seen;
  std::set<Address> unreadableSites, unconfirmedSites;
  std::vector<std::string> notes;  // the main CPU's, in the order made
  std::uint64_t spent = 0;
  std::uint32_t frame = 0;  // counted from power-on, the first being 0; a frame begins when the beam wraps to line 0
  // What the step about to run was read as, at the boundary before it.
  Address site = 0;
  std::uint16_t stackBefore = 0;
  std::uint16_t lineBefore = 0;
  std::optional<detail::Pending> pending;
  Cpu65816Mode mode{};

  Impl(Snes& m, std::span<const std::uint8_t> image) : Impl(detail::MachineView{m}, image) {
    host = &m;
    host->setObserver(&recorder);
    host->setApuObserver(&audio);
    host->setInstructionObserver(this);
  }

  // Over a state a worker keeps current from a buffer: no machine is told
  // anything, and the worker hands each boundary to `boundary`.
  Impl(detail::MachineView kept, std::span<const std::uint8_t> image)
      : machine(kept),
        rom(image),
        board(cartridgeBoard(image)),
        shadow{board, image.size(), detail::kOriginCap},
        recorder{kept, shadow},
        lockstep{board, image.size(), kept.held().cpu, shadow},
        audio{kept} {
    shadow.carries = &recorder;
    begin();
  }

  // The boundary the next step starts from. The state is the live machine:
  // everything read from it is copied out here, since the step rewrites it.
  void begin() {
    const SnesState& before = machine.held();
    site = (static_cast<Address>(before.cpu.pbr) << 16) | before.cpu.pc;
    stackBefore = before.cpu.s;
    lineBefore = before.vpos;
    bool unreadable = false;
    pending = detail::pendingTarget(board, rom, before, unreadable);
    mode = detail::modeOf(before.cpu);
    if (unreadable && unreadableSites.insert(site).second) {
      notes.push_back("run: the jump at " + formatAddress(site, 24) +
                      " reads its pointer from memory the run cannot see; not recorded");
    }
    recorder.stepBegan(site);
  }

  void instruction(const Cpu65816State& before, const Cpu65816State& after, std::uint32_t masterCycles) override {
    boundary(before, after, masterCycles, nullptr, nullptr, 0);
  }

  // One boundary: the step checked here, or, with `checked`, checked elsewhere
  // with its shadow calls on `tape` from `from`.
  void boundary(const Cpu65816State& before, const Cpu65816State& after, std::uint32_t masterCycles,
                const detail::Checked* checked, const ir::Tape* tape, std::size_t from) {
    spent += masterCycles;
    if (enumerating) enumerating->spent = spent;

    // A step runs one instruction, or one cycle of a transfer, never a whole
    // frame, so the beam wrapping to line 0 is a frame boundary seen exactly once.
    if (machine.held().vpos < lineBefore) {
      ++frame;
      recorder.frameBegan();
    }

    // The interpreter beside the machine: the step's instruction lifted from
    // its fetches and checked, the landing read, the registers recorded.
    if (checked != nullptr) {
      lockstep.follow(*checked, before, after, notes, tape, from);
    } else {
      lockstep.step(before, after, recorder.cpu, notes);
    }

    // The second run keeps what the planned routines' frames wrote.
    if (enumerating) enumerating->wrote(recorder, shadow);

    confirm(after);
    begin();
  }

  // The landing confirms the pointer. A step that serviced an interrupt instead
  // lands in the handler and the instruction has not run yet; it is seen when it
  // does. A landing elsewhere means the reading of the form is wrong, and that is
  // said rather than recorded.
  void confirm(const Cpu65816State& after) {
    if (!pending) return;
    const Address landed = (static_cast<Address>(after.pbr) << 16) | after.pc;
    // A step that left the program counter where it was ran no instruction: the
    // CPU was held off the bus — a DMA transfer, an HDMA event — and the jump is
    // still ahead of it. It is seen on the step that runs it.
    if (landed == site) return;
    if (landed != pending->target) {
      // An interrupt sequence lands in bank zero having pushed the return address
      // and the status byte: four bytes in native mode, three in emulation. Nothing
      // the four forms do moves the stack that way.
      const std::uint16_t pushed = static_cast<std::uint16_t>(stackBefore - after.s);
      const bool interrupted = after.pbr == 0 && (pushed == 4u || pushed == 3u);
      if (!interrupted && unconfirmedSites.insert(site).second) {
        notes.push_back("run: the jump at " + formatAddress(site, 24) + " read a pointer to " +
                        formatAddress(pending->target, 24) + " but the CPU went to " + formatAddress(landed, 24) +
                        "; not recorded");
      }
      return;
    }
    if (!seen.insert({site, pending->target, contextOf(mode).bits}).second) return;
    reached.push_back(ReachedTarget{.target = pending->target,
                                    .mode = mode,
                                    .site = site,
                                    .call = pending->call,
                                    .name = {}});
  }

  // The console's reset line, taken through every part of the run: the ranges
  // open close, the call stack the shadow held ends, and both interpreters
  // start again from the registers the reset left.
  void reset() {
    host->reset();
    afterReset();
    audio.restart();
  }

  // Everything but the sound CPU's part of a reset, once the state holds what
  // the reset left.
  void afterReset() {
    for (std::uint8_t c = 0; c < 8; ++c) recorder.closeChannel(c);
    recorder.hdmaEnabled = 0;
    recorder.port.reset();
    recorder.carried.reset();
    shadow.forgetPlaces();
    shadow.flowBroke();
    shadow.abandon();
    lockstep.check.interpreter.registers = ir::registersOf(machine.held().cpu);
    lockstep.check.interpreter.release();
    if (enumerating) enumerating->live.clear();
    begin();
  }

  // The run's notes: the main CPU's in the order made, then the sound CPU's.
  void notesInto(std::vector<std::string>& into) const {
    into.insert(into.end(), notes.begin(), notes.end());
    into.insert(into.end(), audio.notes.begin(), audio.notes.end());
  }

  // The shadow closes the stream still open — the recorder folds the last
  // carry with it and takes its landing — and then the recorder closes every
  // range, so what the run's end cut short is counted, and a landing no frame
  // drew is unshown.
  void finish(RunObservation& observation) {
    close();
    if (host != nullptr) {
      host->setObserver(nullptr);
      host->setApuObserver(nullptr);
      host->setInstructionObserver(nullptr);
    }
    assemble(observation);
  }
  void close() {
    shadow.finish();
    recorder.finish();
    shadow.carries = nullptr;
  }

  // What the run holds, as an observation: everything but what a plan's
  // values add.
  void assemble(RunObservation& observation) const {
    assembleCpu(observation);
    assembleSound(observation);
  }

  // The sound CPU's part of the observation: its counts and its samples.
  void assembleSound(RunObservation& observation) const {
    observation.spc700Instructions = audio.instructions;
    observation.spc700Nodes = audio.nodeKeys.size();
    observation.spc700Divergences = audio.diverged;
    // The samples the key-ons named: by start, the shorter first, then by bytes.
    observation.samples = audio.samples;
    std::sort(observation.samples.begin(), observation.samples.end(),
              [](const KeyedSample& a, const KeyedSample& b) {
                if (a.start != b.start) return a.start < b.start;
                if (a.bytes.size() != b.bytes.size()) return a.bytes.size() < b.bytes.size();
                return a.bytes < b.bytes;
              });
  }

  // Everything else.
  void assembleCpu(RunObservation& observation) const {
    observation.reached = reached;
    std::sort(observation.reached.begin(), observation.reached.end(),
              [](const ReachedTarget& a, const ReachedTarget& b) {
                if (a.site != b.site) return a.site < b.site;
                if (a.target != b.target) return a.target < b.target;
                return contextOf(a.mode).bits < contextOf(b.mode).bits;
              });
    observation.moved = recorder.out;
    std::sort(observation.moved.begin(), observation.moved.end(), rangeBefore);
    observation.landed = recorder.landed;
    std::sort(observation.landed.begin(), observation.landed.end(), landedBefore);
    observation.walked = recorder.walked;
    std::sort(observation.walked.begin(), observation.walked.end(), walkedBefore);
    observation.palettes = recorder.palettes;

    observation.ran = lockstep.ran;
    std::sort(observation.ran.begin(), observation.ran.end(), [](const Landing& a, const Landing& b) {
      if (a.site != b.site) return a.site < b.site;
      if (a.target != b.target) return a.target < b.target;
      return contextOf(a.mode).bits < contextOf(b.mode).bits;
    });
    for (const auto& [address, values] : lockstep.seen) {
      observation.seen.push_back(SeenState{.address = address,
                                           .d = {values.d.begin(), values.d.end()},
                                           .dbr = {values.dbr.begin(), values.dbr.end()}});
    }
    observation.instructions = lockstep.instructions;
    observation.interrupts = lockstep.interrupts;
    observation.nodes = lockstep.nodeKeys.size();
    observation.divergences = lockstep.diverged;

    // What was staged: every extent, in address order then by count, its
    // writers most bytes first.
    for (const auto& [extent, staged] : recorder.staged) {
      StagedRange range = staged;
      std::sort(range.writers.begin(), range.writers.end(), [](const StagedWriter& a, const StagedWriter& b) {
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        if (a.unwritten != b.unwritten) return !a.unwritten;
        return a.writer < b.writer;
      });
      observation.staged.push_back(std::move(range));
    }
    // What the CPU streamed: each distinct stream and landing once with its
    // count, as the recorder took them from the shadow's closings; the source
    // is the shadow's, which stands as the widest run it found.
    observation.streamed = recorder.streams;
    for (StreamedRange& range : observation.streamed) {
      for (const ir::Stream& stream : shadow.streams()) {
        if (stream.site == range.site && stream.registerAddress == range.registerAddress &&
            stream.memory == range.memory && stream.first == range.romOffset && stream.bytes == range.bytes) {
          range.source = stream.source;
          break;
        }
      }
    }
    std::sort(observation.streamed.begin(), observation.streamed.end(),
              [](const StreamedRange& a, const StreamedRange& b) {
                if (a.site != b.site) return a.site < b.site;
                if (a.registerAddress != b.registerAddress) return a.registerAddress < b.registerAddress;
                if (a.memory != b.memory) return a.memory < b.memory;
                if (a.romOffset != b.romOffset) return a.romOffset < b.romOffset;
                if (a.landing.has_value() != b.landing.has_value()) return !a.landing.has_value();
                if (!a.landing) return false;
                if (a.landing->lowest != b.landing->lowest) return a.landing->lowest < b.landing->lowest;
                if (a.landing->highest != b.landing->highest) return a.landing->highest < b.landing->highest;
                if (a.landing->shown != b.landing->shown) return a.landing->shown;
                if (a.landing->areas != b.landing->areas) return a.landing->areas < b.landing->areas;
                return a.landing->depths < b.landing->depths;
              });
    observation.originSets = shadow.origins().interned();
    observation.originCap = shadow.origins().cap();
  }
};

}  // namespace snaggletooth::disasm
