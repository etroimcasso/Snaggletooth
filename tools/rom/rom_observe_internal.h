#pragma once

// The working parts of a run, shared by `rom_observe.cpp` and the run's
// contract file: the observer the run sets on the machine, the interpreter run
// beside it, the reading of addresses they share, and what the run keeps to
// run a routine again for each value of its index. Not a library surface:
// nothing outside `tools/rom/` and `tests/rom/` includes it.

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
#include <tuple>
#include <vector>

#include "ir/cpu65816_lift.h"
#include "ir/ir_lockstep.h"
#include "ir/ir_provenance.h"
#include "ir/ir_text.h"
#include "rom/rom_observe.h"
#include "rom/rom_text.h"
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
  const Snes& machine;
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

  Recorder(const Snes& m, ir::Provenance& p) : machine(m), shadow(p) {}

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
        const VramUse use = vramAreas(machine.state().ppu, landing.lowest, landing.highest);
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
    if (machine.state().ppu.forcedBlank()) return;
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
    carry.values.push_back(workRamByte(machine.state(), memory));
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
    const DmaChannel& ch = machine.state().dma[channel];
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
    const bool toRegister = (machine.state().dma[a.channel].dmap & 0x80u) == 0u;
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
// the plan's order, and which of them have frames on the stack: each live
// invocation, outermost first, with the shadow's depth once it was entered, the
// master cycles the run had then counted, and the interrupt depth it was
// entered at. A hardware interrupt deepens `interruptDepth` and an `RTI`
// returns it; what the CPU or an engine writes is a live invocation's only at
// the interrupt depth it was entered at, so a handler that interrupts a
// routine writes nothing of the routine's, and a routine a handler calls has
// its writes all the same. `spent` is the run's master cycles as the loop last
// counted them; `dueSite` and `dueReturn` the call whose routine's snapshot the
// loop is to take.
struct Enumerating {
  struct Live {
    std::size_t planned;
    std::size_t depth;
    std::uint64_t enteredAt;
    unsigned interrupts;
  };
  std::vector<Planned> planned;
  std::vector<Live> live;
  unsigned interruptDepth = 0;
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

  // A call landed on `landed`, the shadow `depth` deep after it: when that is a
  // planned routine, its invocation is live, and its snapshot falls due when
  // it has none yet. Answers the routine whose snapshot is due.
  std::optional<std::size_t> entered(Address landed, Address site, Address returnTo, std::size_t depth) {
    std::optional<std::size_t> due;
    for (std::size_t p = 0; p < planned.size(); ++p) {
      if (planned[p].routine->address != landed) continue;
      live.push_back(Live{.planned = p, .depth = depth, .enteredAt = spent, .interrupts = interruptDepth});
      if (!planned[p].snapshot) {
        due = p;
        dueSite = site;
        dueReturn = returnTo;
      }
      break;
    }
    return due;
  }

  // A return left the shadow `depth` deep: every invocation deeper than that
  // has returned, and its cycles count toward its routine's longest.
  void returned(std::size_t depth) {
    while (!live.empty() && live.back().depth > depth) {
      Planned& entry = planned[live.back().planned];
      entry.longest = std::max(entry.longest, spent - live.back().enteredAt);
      live.pop_back();
    }
  }

  // What the CPU and the engines wrote this step, given to every live
  // invocation's routine entered at the interrupt depth the step ran at.
  void wrote(const Recorder& recorder) {
    for (const Live& l : live) {
      if (l.interrupts != interruptDepth) continue;
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

// The four forms whose destination the bytes do not name and the run reads
// from the pointer instead: their landing is a reached target, never a
// landing the run has to record from where the CPU went.
inline bool indirectForm(const ir::Instruction& instruction) {
  const bool indirect = instruction.addressing == ir::Addressing::AbsoluteIndirect ||
                        instruction.addressing == ir::Addressing::AbsoluteIndirectLong ||
                        instruction.addressing == ir::Addressing::AbsoluteIndexedIndirect;
  return indirect && (instruction.flow == ir::Flow::Jump || instruction.flow == ir::Flow::Call);
}

// The interpreter run beside the machine over the whole run: every executed
// instruction lifted from the bytes the CPU fetched, checked, and read for
// where the CPU went next and what its registers held.
struct Lockstep {
  CartridgeBoard board;
  std::size_t imageBytes;
  ir::Provenance& shadow;
  const Cpu65816Backend& backend = cpu65816Backend();
  ir::Interpreter interpreter;
  const std::vector<ir::Effect> nmi = ir::interruptSequence(ir::Interrupt::Nmi);
  const std::vector<ir::Effect> irq = ir::interruptSequence(ir::Interrupt::Irq);

  // The nodes lifted so far, one per address, mode and bytes — so bytes the
  // program rewrote at an address are a second node there, and a mirror bank
  // shares the node of the bank the tree places the bytes in.
  using NodeKey = std::tuple<Address, std::uint32_t, std::vector<std::uint8_t>>;
  std::map<NodeKey, ir::Node> nodes;

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
  std::vector<ir::Divergence> divergences;
  std::uint64_t instructions = 0;
  std::uint64_t interrupts = 0;
  std::uint64_t diverged = 0;
  std::uint64_t steps = 0;

  // The second run's record of the planned routines, told of every call,
  // return and interrupt; nothing in the first run. `snapshotDue` names the
  // planned routine whose snapshot the loop takes once this step is done.
  Enumerating* enumerating = nullptr;
  std::optional<std::size_t> snapshotDue;

  Lockstep(const CartridgeBoard& b, std::size_t bytes, const Cpu65816State& start, ir::Provenance& p)
      : board(b), imageBytes(bytes), shadow(p) {
    interpreter.registers = ir::registersOf(start);
    interpreter.shadow = &shadow;
  }

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

  // One step the machine took, from the state before it to the state after.
  void step(const Cpu65816State& before, const Cpu65816State& after,
            const ir::StepObserver& observed, std::vector<std::string>& notes) {
    const std::uint64_t ordinal = steps++;
    if (before.run != CpuRunState::Running) {
      // A halted cycle: nothing ran. If a line ended the wait, the interpreter's
      // wait ends with it.
      if (after.run == CpuRunState::Running) interpreter.release();
      clearPort();
      return;
    }
    // A transfer engine held the bus for the whole step; the instruction is
    // still ahead of the CPU and is seen on the step that runs it.
    if (!observed.cpuRan) {
      clearPort();
      return;
    }

    const Address rawSite = (static_cast<Address>(before.pbr) << 16) | before.pc;
    const Address site = placed(board, imageBytes, rawSite);
    ir::Divergence prototype;
    prototype.instruction = ordinal;
    prototype.site = site;
    const std::size_t divergencesBefore = divergences.size();

    // What the machine took: a hardware request due at the boundary, or the
    // instruction at the program counter.
    const bool nmiTaken = before.nmiPending;
    const bool irqTaken = !nmiTaken && before.irqLine && (before.p & kCpuFlagI) == 0;
    if (nmiTaken || irqTaken) {
      prototype.name = nmiTaken ? "NMI" : "IRQ";
      shadow.site = site;
      shadow.flowBroke();
      shadow.called();
      if (enumerating != nullptr) ++enumerating->interruptDepth;
      ir::checkInterrupt(interpreter, nmiTaken ? nmi : irq, observed, after, prototype, divergences);
      ++interrupts;
      expectedReturns.insert(site);
      if (divergences.size() != divergencesBefore) {
        noteDivergence(prototype.name, site, before, notes);
        shadow.forgetPlaces();
      }
      clearPort();
      return;
    }

    // The instruction, from the bytes the CPU fetched, decoded under the mode
    // the CPU was in and placed where the tree places it.
    std::vector<std::uint8_t> bytes;
    for (const BusAccess& fetch : observed.fetches) bytes.push_back(fetch.value);
    const Cpu65816Mode mode = modeOf(before);
    const NodeKey key{site, contextOf(mode).bits, bytes};
    auto found = nodes.find(key);
    if (found == nodes.end()) {
      const std::optional<Decoded> decoded = backend.decode(bytes, site, site, contextOf(mode));
      if (!decoded || decoded->instruction.length != bytes.size()) {
        // The chip fetched bytes the decoder does not read as one instruction of
        // that length. Nothing is known to run, so nothing is checked; said once.
        if (notedSites.insert(site).second) {
          notes.push_back("run: the bytes the CPU fetched at " + formatAddress(site, 24) +
                          " do not decode as one instruction under " + modeText(mode) +
                          "; the step is not checked");
        }
        interpreter.registers = ir::registersOf(after);
        shadow.forgetPlaces();
        clearPort();
        return;
      }
      found = nodes.emplace(key, ir::liftInstruction(decoded->instruction, mode)).first;
    }
    const ir::Node& node = found->second;

    // What the run saw at the site, before the instruction ran.
    if (inImage(board, imageBytes, rawSite)) {
      Values& values = seen[site];
      values.d.insert(before.d);
      values.dbr.insert(before.dbr);
    }

    // The shadow follows the instruction under its site, told when the CPU did
    // not fall through to it — which is what a stream's straight run means.
    shadow.site = site;
    if (rawSite != expectedNext) shadow.flowBroke();
    expectedNext = backend.following(rawSite, node.instruction.length);
    ir::checkNode(interpreter, node, observed, after, prototype, divergences);
    ++instructions;
    if (divergences.size() != divergencesBefore) {
      noteDivergence(describeNode(node), site, before, notes);
      shadow.forgetPlaces();
    }
    clearPort();
    // An invocation begins at a call and ends at a return, by the chip's own
    // rules: a `PEA`/`RTS` jump ends one too, and a tail jump does not.
    if (node.instruction.flow == ir::Flow::Call) shadow.called();
    if (node.instruction.flow == ir::Flow::Return) shadow.returned();
    if (enumerating != nullptr) followRoutines(node, site, after);

    // Where the CPU went, against what the instruction names: the address
    // after it, for a form that falls through; its constant target; the
    // pointer's target, for the four forms the run reads ahead; the vector, for
    // a software interrupt; and for a return, an address the run's own calls
    // and interrupts said to expect one at. A call's return is expected after
    // it, whichever instruction returns there.
    const ir::Instruction& instruction = node.instruction;
    const Address following = backend.following(site, instruction.length);
    if (instruction.flow == ir::Flow::Call) expectedReturns.insert(following);
    if (after.run != CpuRunState::Running) return;  // a wait or a stop: no landing
    const Address rawLanded = (static_cast<Address>(after.pbr) << 16) | after.pc;
    const Address landed = placed(board, imageBytes, rawLanded);
    const bool fallsThrough = instruction.flow == ir::Flow::Continue ||
                              instruction.flow == ir::Flow::Branch ||
                              instruction.flow == ir::Flow::Call;
    const bool named =
        rawLanded == rawSite ||  // a block move with bytes left, run again
        (fallsThrough && landed == following) ||
        (instruction.target && landed == placed(board, imageBytes, *instruction.target)) ||
        indirectForm(instruction) ||  // a reached target
        instruction.mnemonic == std::string_view("BRK") ||  // the vector the header names
        instruction.mnemonic == std::string_view("COP") ||
        (instruction.flow == ir::Flow::Return && expectedReturns.count(landed) != 0);
    if (named) return;
    if (!inImage(board, imageBytes, rawLanded)) {
      if (notedSites.insert(site).second) {
        notes.push_back("run: the CPU arrived at " + formatAddress(rawLanded, 24) + " from " +
                        formatAddress(site, 24) + ", which the tree does not hold; not recorded");
      }
      return;
    }
    // The landing is where the CPU arrived, in the bank it arrived in; the
    // disassembler places it to trace from it.
    const Cpu65816Mode arrived = modeOf(after);
    if (!landings.insert({site, rawLanded, contextOf(arrived).bits}).second) return;
    ran.push_back(Landing{.target = rawLanded, .mode = arrived, .site = site, .name = {}});
  }

  // The second run's record told what a node did to the planned routines'
  // frames: a call, not a software interrupt, that lands on a planned routine
  // enters it; every return leaves the frames deeper than the shadow now is;
  // an `RTI` ends an interrupt handler.
  void followRoutines(const ir::Node& node, Address site, const Cpu65816State& after) {
    const ir::Instruction& instruction = node.instruction;
    const std::string_view mnemonic = instruction.mnemonic;
    if (instruction.flow == ir::Flow::Call && mnemonic != "BRK" && mnemonic != "COP") {
      const Address landed = placed(board, imageBytes, (static_cast<Address>(after.pbr) << 16) | after.pc);
      const Address returnTo = placed(board, imageBytes, backend.following(site, instruction.length));
      const std::optional<std::size_t> due = enumerating->entered(landed, site, returnTo, shadow.depth());
      if (due && !snapshotDue) snapshotDue = due;
    }
    if (instruction.flow == ir::Flow::Return) {
      enumerating->returned(shadow.depth());
      if (mnemonic == "RTI" && enumerating->interruptDepth > 0) --enumerating->interruptDepth;
    }
  }

  // The mode the CPU is in, as the trace carries one: the widths are known —
  // the CPU has them — and the carry is not remembered, since nothing here
  // follows an `XCE`.
  static Cpu65816Mode modeOf(const Cpu65816State& cpu) {
    return {.emulation = cpu.e,
            .accumulator8 = (cpu.p & kCpuFlagM) != 0,
            .index8 = (cpu.p & kCpuFlagX) != 0,
            .accumulatorKnown = true,
            .indexKnown = true,
            .carryKnown = false,
            .carry = false};
  }

  static std::string describeNode(const ir::Node& node) {
    return "`" + std::string(node.instruction.mnemonic) + " " +
           std::string(ir::addressingName(node.instruction.addressing)) + "`";
  }

  // A disagreement, said once per site with what disagreed first.
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
// the routine's first instruction next and its return; a `copy` reporting
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
  unsigned interruptDepth = 0;
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
    const std::uint64_t interruptsBefore = step.interrupts;
    const std::uint64_t instructionsBefore = step.instructions;
    rec.stepBegan(site);
    spent += sandbox.step();
    if (sandbox.state().vpos < lineBefore) {
      ++frame;
      presentPads(sandbox, input, frame);
      rec.frameBegan();
    }
    const SnesState& after = sandbox.state();
    step.step(cpuBefore, after.cpu, rec.cpu, dropped);
    if (step.interrupts != interruptsBefore) {
      ++interruptDepth;
    } else if (step.instructions != instructionsBefore && !rec.cpu.fetches.empty() &&
               rec.cpu.fetches.front().value == 0x40u && interruptDepth > 0) {
      --interruptDepth;  // RTI
    }

    if (step.diverged > 0) {
      return refuse("the lift disagreed with the machine at " + text::address24(step.divergences.back().site) +
                    "; not lifted");
    }
    if (interruptDepth == 0) {
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

}  // namespace snaggletooth::disasm::detail
