#include "rom/rom_disasm.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "cpu65816/cpu65816_asm.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_render.h"
#include "ir/ir_text.h"
#include "ir/spc700_lift.h"
#include "rom/cartridge_entries.h"
#include "rom/rom_text.h"
#include "snaggletooth/snes/snes.h"
#include "spc700/spc700_disasm.h"

namespace snaggletooth::disasm {

using text::address16;
using text::address24;
using text::classesText;
using text::hex;
using text::mapName;

namespace {

// The upload stub's window in audio memory. The audio CPU runs there until the
// program it was sent starts, so leaving the window is the start of that program.
constexpr std::uint16_t kStubBase = 0xFFC0u;

// The audio CPU's register page. The memory beneath it is left as the machine
// seeds it, so a boot over filled memory changes nothing the registers read.
constexpr std::uint16_t kRegisterPage = 0x00F0u;
constexpr std::uint16_t kRegisterPageEnd = 0x0100u;

// About a millisecond of the master clock: the step the boot is watched at.
constexpr std::uint64_t kWatchStep = 21'477u;

// A mode as the manifest writes it: the backend's own words for a context.
std::string modeText(const Cpu65816Mode& mode) {
  return cpu65816Backend().describe(contextOf(mode));
}

// The address every byte of the image is placed at: the one `romAddress` reports
// for its offset. An address that reaches the image through a mirror is placed
// at the same bytes' one home.
std::optional<Address> canonical(CartridgeMap map, std::size_t imageBytes, Address address) {
  const std::optional<std::size_t> offset = romOffset(map, address, imageBytes);
  if (!offset) return std::nullopt;
  const std::optional<std::uint32_t> home = romAddress(map, *offset);
  if (!home) return std::nullopt;
  return *home;
}

bool within(const SourceRegion& region, Address address) {
  return address >= region.first && address <= region.last;
}

// Whether a region reads consecutive image bytes from `first` to `last`, which is
// what lets its file be one span of source under one `ORG`.
bool contiguous(CartridgeMap map, std::size_t imageBytes, const SourceRegion& region) {
  if (region.last < region.first) return false;
  const std::optional<std::size_t> start = romOffset(map, region.first, imageBytes);
  if (!start) return false;
  const std::size_t length = static_cast<std::size_t>(region.last - region.first) + 1u;
  if (*start + length > imageBytes) return false;
  for (std::size_t i = 0; i < length; ++i) {
    const std::optional<std::size_t> offset =
        romOffset(map, region.first + static_cast<Address>(i), imageBytes);
    if (!offset || *offset != *start + i) return false;
  }
  return true;
}

// `ranges` sorted, with every run of ranges that touch end to end joined into
// one, so a cut along them keeps an instruction that spans two of them.
std::vector<Range> joined(std::vector<Range> ranges) {
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.first < b.first; });
  std::vector<Range> out;
  for (const Range& range : ranges) {
    if (!out.empty() && out.back().last + 1u == range.first) {
      out.back().last = range.last;
    } else {
      out.push_back(range);
    }
  }
  return out;
}

// The stop an instruction leaves behind when its successors are not in the
// bytes, or none when they are.
std::optional<std::string> stopReason(const Instruction& instruction, CartridgeMap map,
                                      std::size_t imageBytes, const SourceRegion& region) {
  const bool leaves = instruction.flow == Flow::Jump || instruction.flow == Flow::Call;
  if (!leaves) return std::nullopt;
  // BRK and COP continue at their vectors' handlers, which are entries already.
  if (instruction.opcode == 0x00u || instruction.opcode == 0x02u) return std::nullopt;
  if (!instruction.target) {
    return "`" + instruction.text + "`: the target is computed at run time; add an entry for each destination";
  }
  const Address target = *instruction.target;
  if (within(region, target)) return std::nullopt;
  if (canonical(map, imageBytes, target)) return std::nullopt;
  std::string where;
  switch (cartridgeRegion(map, target)) {
    case CartridgeRegion::WorkRam: where = "work RAM"; break;
    case CartridgeRegion::System: where = "a system register or a work-RAM mirror"; break;
    case CartridgeRegion::SaveRam: where = "save RAM"; break;
    case CartridgeRegion::Rom: where = "the cartridge, beyond the image"; break;
    case CartridgeRegion::Unmapped: where = "nothing the bus maps"; break;
  }
  return "`" + instruction.text + "`: the target " + address24(target) + " is " + where +
         ", not in the image";
}

// The first place in `rom` holding exactly `bytes`, when there is exactly one.
std::optional<std::size_t> uniqueOffset(std::span<const std::uint8_t> rom,
                                        std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > rom.size()) return std::nullopt;
  const auto first = std::search(rom.begin(), rom.end(), bytes.begin(), bytes.end());
  if (first == rom.end()) return std::nullopt;
  const auto second = std::search(first + 1, rom.end(), bytes.begin(), bytes.end());
  if (second != rom.end()) return std::nullopt;
  return static_cast<std::size_t>(first - rom.begin());
}

// The shortest run worth matching to the image on its own. Below this a run of
// bytes is found by chance too often to say where it was read from.
constexpr std::size_t kShortestPiece = 16;

// `bytes` split into the pieces the image holds: the longest prefix found in it
// at exactly one place, then the rest the same way. Whole when it is found whole;
// unplaced from the first point on where no piece of at least `kShortestPiece`
// bytes is found at one place.
std::vector<UploadBlock> placeInImage(std::span<const std::uint8_t> rom, std::uint16_t apuAddress,
                                      std::span<const std::uint8_t> bytes) {
  std::vector<UploadBlock> pieces;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::span<const std::uint8_t> rest = bytes.subspan(at);
    std::optional<std::size_t> offset = uniqueOffset(rom, rest);
    std::size_t length = rest.size();
    auto found = [&](std::size_t n) {
      return std::search(rom.begin(), rom.end(), rest.begin(),
                         rest.begin() + static_cast<std::ptrdiff_t>(n)) != rom.end();
    };
    if (!offset && rest.size() > kShortestPiece && found(kShortestPiece)) {
      // The longest prefix the image holds anywhere: a prefix that occurs has
      // every shorter prefix occurring too, so the boundary is found by halving.
      // The piece is cut there whether or not that prefix is at one place.
      std::size_t low = kShortestPiece;  // found
      std::size_t high = rest.size();    // not found
      while (high - low > 1) {
        const std::size_t mid = low + (high - low) / 2;
        if (found(mid)) {
          low = mid;
        } else {
          high = mid;
        }
      }
      length = low;
      offset = uniqueOffset(rom, rest.first(low));
    }
    pieces.push_back(UploadBlock{
        .apuAddress = static_cast<std::uint16_t>(apuAddress + at),
        .bytes = std::vector<std::uint8_t>(rest.begin(),
                                           rest.begin() + static_cast<std::ptrdiff_t>(length)),
        .romOffset = offset});
    at += length;
  }
  return pieces;
}

// Boots the cartridge over audio memory filled with `fill` and runs until the
// audio CPU leaves the stub. True when it did, with the memory and the program
// counter at that moment in `state`.
bool bootUntilProgramStarts(std::span<const std::uint8_t> rom, std::uint8_t fill,
                            std::uint64_t masterCycles, SnesState& state,
                            const ProgressSink& progress) {
  constexpr std::string_view kStage = "booting the sound program";
  const auto report = [&](std::uint64_t at) {
    if (progress) progress(Progress{.stage = kStage, .spent = at, .budget = masterCycles});
  };
  Snes machine{SnesConfig{.rom = rom}};
  {
    SnesState seeded = machine.state();
    for (std::uint32_t a = 0; a < kStubBase; ++a) {
      if (a >= kRegisterPage && a < kRegisterPageEnd) continue;
      seeded.apu.ram[a] = fill;
    }
    machine.restore(seeded);
  }
  auto left = [&]() { return machine.state().apu.cpu.pc < kStubBase; };
  std::uint64_t spent = 0;
  std::uint64_t reported = 0;  // the tick the last report was made at
  report(0);
  while (spent < masterCycles) {
    if (spent / kProgressTick != reported) {
      reported = spent / kProgressTick;
      report(spent);
    }
    const SnesState before = machine.state();
    spent += machine.run(kWatchStep);
    if (!left()) continue;
    // Back to the step's start and forward a cycle at a time, to the first cycle
    // the program counter is out of the window: the jump has just landed and the
    // counter is the program's entry.
    machine.restore(before);
    for (std::uint64_t i = 0; i < kWatchStep * 4u && !left(); ++i) machine.run(1);
    state = machine.state();
    report(spent);
    return true;
  }
  report(spent);
  return false;
}

// The label an entry's name gives its handler. A name the 65816 dialect
// reserves — `cop` and `brk` are vectors and mnemonics both — cannot be a
// label, so it carries `_handler`; any other name is the label as it is.
std::string handlerLabel(std::string_view name) {
  static const assembler::Cpu65816Dialect dialect;
  const std::string label(name);
  return dialect.reserved(assembler::upper(label)) ? label + "_handler" : label;
}

// The vectors as trace entries, each in the mode the CPU takes it in. The
// emulation-mode set — reset among them — is taken only with the emulation flag
// set, which fixes both widths at eight; the native set is taken in native mode,
// with the widths whatever the interrupted code had, which the image cannot say.
std::vector<TraceEntry> vectorTraceEntries(const CartridgeHeader& header) {
  std::vector<TraceEntry> entries;
  for (const VectorEntry& vector : vectorEntries(header)) {
    const bool native = vector.name.ends_with("_native");
    entries.push_back(TraceEntry{
        .address = vector.address,
        .mode = native ? Cpu65816Mode::nativeUnknown() : Cpu65816Mode::reset(),
        .name = handlerLabel(vector.name)});
  }
  return entries;
}

}  // namespace

std::vector<SourceRegion> bankRegions(CartridgeMap map, std::size_t imageBytes) {
  std::vector<SourceRegion> regions;
  std::size_t offset = 0;
  while (offset < imageBytes) {
    const std::optional<std::uint32_t> start = romAddress(map, offset);
    if (!start) {
      ++offset;
      continue;
    }
    std::size_t length = 1;
    while (offset + length < imageBytes) {
      const std::optional<std::uint32_t> next = romAddress(map, offset + length);
      if (!next || *next != *start + length || (*next >> 16) != (*start >> 16)) break;
      ++length;
    }
    regions.push_back(SourceRegion{.file = "bank_" + hex(*start >> 16, 2) + ".asm",
                                   .first = *start,
                                   .last = *start + static_cast<Address>(length) - 1u});
    offset += length;
  }
  return regions;
}

namespace {

// ---- the assets -------------------------------------------------------------------

// The directory a lifted file lives under: the memory its bytes went to. A
// general-purpose transfer to any other register — a copy into work RAM, a
// register fill — carries bytes that could be anything, and is not an asset. A
// stream the CPU carried is placed as a transfer to the same register is; a
// staged source is placed by the use its range went as, which the caller
// names in `kind`.
std::optional<std::string_view> assetDirectory(RegisterClass cls, MovedKind kind) {
  if (kind == MovedKind::Table || kind == MovedKind::Indirect) return "hdma";
  switch (cls) {
    case RegisterClass::Vram: return "vram";
    case RegisterClass::Cgram: return "cgram";
    case RegisterClass::Oam: return "oam";
    case RegisterClass::Apu: return "apu";
    default: return std::nullopt;
  }
}

// One piece of a moved range that reads consecutive image offsets, in image
// order, with what the range was to the engine. For a staged source, `kind`
// is `Staged` and `use` is what the range built from it went as, which is
// what places the file; for a stream, both are `Stream`. `landings` is where
// the range — or the range built from the source, or the stream — landed on
// the other side of the port, every landing the run saw; empty for a transfer
// the code proves, a file kept from the manifest, and a range to no data port.
struct Piece {
  std::size_t offset = 0;
  std::size_t length = 0;
  RegisterClass cls = RegisterClass::Display;
  MovedKind kind = MovedKind::Dma;
  MovedKind use = MovedKind::Dma;
  Address registerAddress = 0;
  Address site = 0;
  std::vector<PortLanding> landings;
};

// The landings of one `moved` range, by the fields that identify it.
std::vector<PortLanding> landingsOf(const CartridgeDisassembly& out, const MovedRange& range) {
  std::vector<PortLanding> landings;
  for (const LandedRange& landed : out.landed) {
    if (landed.site == range.site && landed.channel == range.channel && landed.memory == range.memory &&
        landed.bytes == range.bytes && landed.kind == range.kind) {
      landings.push_back(landed.landing);
    }
  }
  return landings;
}

// The directory a VRAM file takes from where its bytes landed: `maps` when
// every landing the run saw drawn lies in a layer's screen, `tiles` when every
// one lies in a name base or the sprite tiles, and `vram` when a landing was
// never drawn (and so has no area), lies in no area, lies under Mode 7, mixes
// the two, or when there is none.
std::string_view vramDirectory(const std::vector<Piece>& group) {
  bool any = false;
  bool maps = true;
  bool tiles = true;
  for (const Piece& piece : group) {
    for (const PortLanding& landing : piece.landings) {
      if (landing.memory != PortMemory::Vram) continue;
      any = true;
      if (landing.areas == 0u) return "vram";  // shown in no area, or never shown, which has none
      if ((landing.areas & ~kAreaTilemaps) != 0u) maps = false;
      if ((landing.areas & ~kAreaTiles) != 0u) tiles = false;
    }
  }
  if (!any) return "vram";
  if (maps) return "maps";
  if (tiles) return "tiles";
  return "vram";
}

std::string movedText(const MovedRange& range) {
  return "moved " + address24(range.site) + " channel " + std::to_string(range.channel) + " memory " +
         address24(range.memory) + " bytes " + std::to_string(range.bytes);
}

// One way an extent of work RAM was carried out: the register it reached, as
// a transfer or a table walk (`Dma`, `Table`, `Indirect`) or as the CPU's own
// stores (`Stream`), the instruction that sent it, and where the bytes landed
// on the other side of the port, every time the extent went this way.
struct ExtentUse {
  RegisterClass cls = RegisterClass::Display;
  MovedKind kind = MovedKind::Dma;
  Address registerAddress = 0;
  Address site = 0;
  std::vector<PortLanding> landings;
};

// Every distinct way an extent went, as the `moved` and `streamed` lines say —
// only those a file can be named for.
std::vector<ExtentUse> extentUses(const CartridgeDisassembly& out, const StagedRange& range) {
  std::vector<ExtentUse> uses;
  const auto add = [&](ExtentUse use) {
    const auto known = std::find_if(uses.begin(), uses.end(), [&](const ExtentUse& u) {
      return u.cls == use.cls && u.kind == use.kind && u.registerAddress == use.registerAddress;
    });
    if (known == uses.end()) {
      uses.push_back(std::move(use));
    } else {
      known->landings.insert(known->landings.end(), use.landings.begin(), use.landings.end());
    }
  };
  for (const MovedRange& moved : out.moved) {
    if (!moved.toRegister || moved.step == MovedStep::Fixed || !moved.registerClass) continue;
    if (extentStart(moved) != range.memory || moved.bytes != range.bytes) continue;
    if (!assetDirectory(*moved.registerClass, moved.kind)) continue;
    add(ExtentUse{.cls = *moved.registerClass,
                  .kind = moved.kind,
                  .registerAddress = moved.registerAddress,
                  .site = moved.site,
                  .landings = landingsOf(out, moved)});
  }
  for (const StreamedRange& stream : out.streamed) {
    if (!stream.memory || *stream.memory != range.memory || stream.bytes != range.bytes) continue;
    if (!stream.registerClass || !assetDirectory(*stream.registerClass, MovedKind::Stream)) continue;
    add(ExtentUse{.cls = *stream.registerClass,
                  .kind = MovedKind::Stream,
                  .registerAddress = stream.registerAddress,
                  .site = stream.site,
                  .landings = stream.landing ? std::vector<PortLanding>{*stream.landing} : std::vector<PortLanding>{}});
  }
  return uses;
}

// ---- where a staged range came from ------------------------------------------------

// The label a writer's site carries: the routine it lies in, or the site
// itself where no routine holds it — which is a signal, not a name. An engine
// is `none`; bytes nothing wrote since power-on are `unwritten`.
std::string writerLabel(const StagedWriter& writer, const std::map<Address, std::string>& routineOf) {
  if (writer.unwritten) return "unwritten";
  if (writer.writer.engine) return "none";
  const auto found = routineOf.find(writer.writer.site);
  return found == routineOf.end() ? address24(writer.writer.site) : found->second;
}

// One source of a staged extent: the bytes one routine wrote — or the engines
// did — with their origin together and the runs of image bytes that origin
// was drawn from, which are what is lifted.
struct StagedSource {
  std::string label;
  std::uint64_t bytes = 0;
  ir::OriginSet origin;
  std::vector<ir::OriginInterval> sources;
};

// The mark an origin carries: `exact`, or `approximate` when the run widened
// it to its hull.
std::string_view originMark(const ir::OriginSet& origin) {
  return origin.approximate ? "approximate" : "exact";
}

// How many of an origin's image bytes lie within a run.
std::size_t usedWithin(const ir::OriginSet& origin, const ir::OriginInterval& run) {
  std::size_t used = 0;
  for (const ir::OriginInterval& interval : origin.image) {
    const std::size_t first = std::max(interval.first, run.first);
    const std::size_t last = std::min(interval.last, run.last);
    if (first <= last) used += last - first + 1u;
  }
  return used;
}

// The extent's writers grouped by the label they carry, most bytes first.
std::vector<StagedSource> stagedSources(const StagedRange& range,
                                        const std::map<Address, std::string>& routineOf) {
  std::vector<StagedSource> out;
  for (const StagedWriter& writer : range.writers) {
    const std::string label = writerLabel(writer, routineOf);
    auto same = std::find_if(out.begin(), out.end(),
                             [&](const StagedSource& s) { return s.label == label; });
    if (same == out.end()) {
      out.push_back(StagedSource{.label = label, .bytes = 0, .origin = {}, .sources = {}});
      same = out.end() - 1;
    }
    same->bytes += writer.bytes;
    ir::Origins::merge(same->origin, writer.origin);
    ir::OriginSet held;
    held.image = std::move(same->sources);
    ir::OriginSet more;
    more.image = writer.sources;
    ir::Origins::merge(held, more);
    same->sources = std::move(held.image);
  }
  std::stable_sort(out.begin(), out.end(), [](const StagedSource& a, const StagedSource& b) {
    return a.bytes > b.bytes;
  });
  return out;
}

// The routine every line of the tree belongs to, by address.
std::map<Address, std::string> routineByLine(const CartridgeDisassembly& disassembly) {
  std::map<Address, std::string> out;
  for (const Routine& routine : disassembly.routines) {
    for (const Address line : routine.lines) out[line] = routine.label;
  }
  return out;
}

std::string stagedText(const StagedRange& range, const StagedSource& source) {
  return "staged " + address24(range.memory) + " bytes " + std::to_string(range.bytes) + " by " +
         source.label;
}

std::string streamText(const StreamedRange& stream) {
  return "streamed " + address24(stream.site) + " " +
         (stream.registerName.empty() ? address24(stream.registerAddress)
                                      : std::string(stream.registerName)) +
         " bytes " + std::to_string(stream.bytes);
}

// Lifts every `moved` range the rules admit into `out.assets`. The rules are the
// page's (`docs/snes-disassembler.md` §The assets): a range is lifted when it goes
// to a register, steps up or down, its bytes are in the image, and it is a
// general-purpose transfer to VRAM, CGRAM, OAM or the audio port or an HDMA
// table or block to any register. A fill from one byte, a read back into memory,
// bytes outside the image and a transfer to any other register are left where
// they are, without a word; a range over an instruction the trace decoded, over
// a sound-program block, or sent two places is refused with a note. Ranges that
// share a byte are one file.
//
// A range in work RAM the run knows the source of is lifted as that source: for
// each routine that wrote it, every run of image bytes its bytes were drawn
// from, under the rules of the register the range went to — whether an engine
// carried it or the CPU did, a store at a time. A stream the CPU carried from
// the image is lifted as the run its carrier read, the same way. A source
// whose bytes went to two classes is one file under `staged/`, named for both.
void liftAssets(CartridgeDisassembly& out, const CartridgeRequest& request) {
  const CartridgeMap map = out.header.map;
  const std::size_t imageBytes = out.imageBytes;

  // Every instruction's bytes and every placed block's, as image offsets.
  std::vector<std::pair<std::size_t, std::size_t>> code;
  for (const RegionListing& region : out.regions) {
    for (const Line& line : region.listing.lines) {
      if (!line.isCode) continue;
      if (const std::optional<std::size_t> at = romOffset(map, line.address, imageBytes)) {
        code.emplace_back(*at, line.instruction.length);
      }
    }
  }
  std::sort(code.begin(), code.end());
  auto overlapsCode = [&](std::size_t offset, std::size_t length) {
    auto it = std::lower_bound(code.begin(), code.end(), std::make_pair(offset + length, std::size_t{0}));
    // Every instruction that starts before the piece ends, walked back to the
    // first that could still reach into it: an instruction is four bytes at most.
    while (it != code.begin()) {
      --it;
      if (it->first + it->second > offset) return true;
      if (it->first + 4u < offset) break;
    }
    return false;
  };
  std::vector<std::pair<std::size_t, std::size_t>> blocks;
  if (out.sound) {
    for (const UploadBlock& block : out.sound->capture.blocks) {
      if (block.romOffset) blocks.emplace_back(*block.romOffset, block.bytes.size());
    }
  }
  auto overlapsBlock = [&](std::size_t offset, std::size_t length) {
    for (const auto& [at, size] : blocks) {
      if (at < offset + length && offset < at + size) return true;
    }
    return false;
  };

  // The pieces: each range's bytes in transfer order, split where the next byte
  // is not the next image offset.
  std::vector<Piece> pieces;
  for (const MovedRange& range : out.moved) {
    if (!range.toRegister || range.step == MovedStep::Fixed || !range.registerClass) continue;
    if (!assetDirectory(*range.registerClass, range.kind)) continue;
    const bool up = range.step == MovedStep::Increment;
    const Address bank = range.memory & 0xFF0000u;
    const std::vector<PortLanding> landings = landingsOf(out, range);
    std::vector<Piece> mine;
    std::optional<std::size_t> previous;
    std::uint32_t outside = 0;
    for (std::uint32_t i = 0; i < range.bytes; ++i) {
      const std::uint16_t offset16 = static_cast<std::uint16_t>(up ? range.memory + i : range.memory - i);
      const std::optional<std::size_t> at = romOffset(map, bank | offset16, imageBytes);
      if (!at) {
        ++outside;
        previous.reset();
        continue;
      }
      if (previous && (up ? *at == *previous + 1u : *at + 1u == *previous)) {
        ++mine.back().length;
        if (!up) --mine.back().offset;
      } else {
        mine.push_back(Piece{.offset = *at,
                             .length = 1,
                             .cls = *range.registerClass,
                             .kind = range.kind,
                             .use = range.kind,
                             .registerAddress = range.registerAddress,
                             .site = range.site,
                             .landings = landings});
      }
      previous = at;
    }
    if (outside != 0 && !mine.empty()) {
      out.notes.push_back(movedText(range) + ": " + std::to_string(outside) +
                          " of its bytes are not the image and are not lifted");
    }
    for (const Piece& piece : mine) {
      const Address home = romAddress(map, piece.offset).value_or(0);
      const Address last = home + static_cast<Address>(piece.length) - 1u;
      if (overlapsCode(piece.offset, piece.length)) {
        out.notes.push_back(movedText(range) + ": " + address24(home) + "-" + address24(last) +
                            " overlaps an instruction the trace decoded; not lifted");
        continue;
      }
      if (overlapsBlock(piece.offset, piece.length)) {
        out.notes.push_back(movedText(range) + ": " + address24(home) + "-" + address24(last) +
                            " overlaps a block of the sound program; not lifted");
        continue;
      }
      pieces.push_back(piece);
    }
  }
  // A piece the run's shadow named — a staged range's source, a stream's
  // bytes — under the same two refusals. A refusal is said once: a stream
  // seen landing in several places is several ranges of the same bytes, and
  // one note names them.
  const auto refuse = [&](const std::string& note) {
    if (std::find(out.notes.begin(), out.notes.end(), note) == out.notes.end()) out.notes.push_back(note);
  };
  auto admit = [&](Piece piece, const std::string& what) {
    const Address home = romAddress(map, piece.offset).value_or(0);
    const Address last = home + static_cast<Address>(piece.length) - 1u;
    if (overlapsCode(piece.offset, piece.length)) {
      refuse(what + ": " + address24(home) + "-" + address24(last) +
             " overlaps an instruction the trace decoded; not lifted");
      return;
    }
    if (overlapsBlock(piece.offset, piece.length)) {
      refuse(what + ": " + address24(home) + "-" + address24(last) +
             " overlaps a block of the sound program; not lifted");
      return;
    }
    pieces.push_back(piece);
  };

  // The transfers the code proves whole: to a register a file can be named
  // for, stepping through the image, with a source, a count and a
  // general-purpose start every path settles. Where the run started the same
  // transfer — a range from the same start on the same channel — the run's
  // word stands: a range that agrees confirms the proof and the file is the
  // run's; one that differs is noted beside it and the proof lifts nothing.
  const auto dmaText = [](const DmaTransfer& dma) {
    return "dma " + address24(dma.site) + " channel " + std::to_string(dma.channel);
  };
  for (const DmaTransfer& dma : out.dmas) {
    const bool started = dma.startMask.has_value() && dma.startSite.has_value() && !dma.hdma;
    if (!started || !dma.source || !dma.bytes || !dma.step) continue;
    if (dma.direction != DmaDirection::ToBBus || *dma.step == MovedStep::Fixed || !dma.destinationClass) continue;
    if (!assetDirectory(*dma.destinationClass, MovedKind::Dma)) continue;
    // The start's site, which a `moved` range of the same transfer carries.
    const Address start = dma.startSite.value_or(dma.site);
    bool confirmed = false;
    const MovedRange* differing = nullptr;
    for (const MovedRange& range : out.moved) {
      if (range.channel != dma.channel) continue;
      if (canonical(map, imageBytes, range.site) != std::optional<Address>{start}) continue;
      if (range.toRegister && range.memory == *dma.source && range.step == *dma.step && range.bytes == *dma.bytes) {
        confirmed = true;
      } else if (differing == nullptr) {
        differing = &range;
      }
    }
    if (confirmed) continue;
    if (differing != nullptr) {
      out.notes.push_back(dmaText(dma) + ": the code proves " + address24(*dma.source) + " " +
                          std::string(movedStepName(*dma.step)) + " bytes " + std::to_string(*dma.bytes) +
                          "; the run moved " + address24(differing->memory) + " " +
                          std::string(movedStepName(differing->step)) + " bytes " +
                          std::to_string(differing->bytes));
      continue;
    }
    const bool up = *dma.step == MovedStep::Increment;
    const Address bank = *dma.source & 0xFF0000u;
    std::vector<Piece> mine;
    std::optional<std::size_t> previous;
    std::uint32_t outside = 0;
    for (std::uint32_t i = 0; i < *dma.bytes; ++i) {
      const std::uint16_t offset16 = static_cast<std::uint16_t>(up ? *dma.source + i : *dma.source - i);
      const std::optional<std::size_t> at = romOffset(map, bank | offset16, imageBytes);
      if (!at) {
        ++outside;
        previous.reset();
        continue;
      }
      if (previous && (up ? *at == *previous + 1u : *at + 1u == *previous)) {
        ++mine.back().length;
        if (!up) --mine.back().offset;
      } else {
        mine.push_back(Piece{.offset = *at,
                             .length = 1,
                             .cls = *dma.destinationClass,
                             .kind = MovedKind::Proven,
                             .use = MovedKind::Dma,
                             .registerAddress = dma.destination.value_or(0),
                             .site = dma.site,
                             .landings = {}});
      }
      previous = at;
    }
    if (outside != 0 && !mine.empty()) {
      out.notes.push_back(dmaText(dma) + " source " + address24(*dma.source) + " bytes " +
                          std::to_string(*dma.bytes) + ": " + std::to_string(outside) +
                          " of its bytes are not the image and are not lifted");
    }
    for (const Piece& piece : mine) {
      admit(piece, dmaText(dma) + " source " + address24(*dma.source) + " bytes " + std::to_string(*dma.bytes));
    }
  }

  // The staged ranges: for every extent in work RAM the run carried to a
  // register a file can be named for, each source's runs, once per use.
  const std::map<Address, std::string> routineOf = routineByLine(out);
  for (const StagedRange& range : out.staged) {
    const std::vector<ExtentUse> uses = extentUses(out, range);
    if (uses.empty()) continue;
    for (const StagedSource& source : stagedSources(range, routineOf)) {
      for (const ir::OriginInterval& run : source.sources) {
        if (run.last >= imageBytes) continue;
        for (const ExtentUse& use : uses) {
          admit(Piece{.offset = run.first,
                      .length = run.last - run.first + 1u,
                      .cls = use.cls,
                      .kind = MovedKind::Staged,
                      .use = use.kind == MovedKind::Stream ? MovedKind::Dma : use.kind,
                      .registerAddress = use.registerAddress,
                      .site = use.site,
                      .landings = use.landings},
                stagedText(range, source));
        }
      }
    }
  }

  // The streams from the image: the run the carrier read. A stream from work
  // RAM is its buffer's, lifted above as the buffer's source.
  for (const StreamedRange& stream : out.streamed) {
    if (stream.memory) continue;
    if (!stream.registerClass || !assetDirectory(*stream.registerClass, MovedKind::Stream)) continue;
    if (stream.source.last >= imageBytes) continue;
    admit(Piece{.offset = stream.source.first,
                .length = stream.source.last - stream.source.first + 1u,
                .cls = *stream.registerClass,
                .kind = MovedKind::Stream,
                .use = MovedKind::Stream,
                .registerAddress = stream.registerAddress,
                .site = stream.site,
                .landings = stream.landing ? std::vector<PortLanding>{*stream.landing} : std::vector<PortLanding>{}},
          streamText(stream));
  }

  // A file the shadow named in an earlier run, kept as its line records it
  // when nothing this pass lifted covers its bytes: its evidence is written
  // fresh by a run, and the line is what keeps the file.
  for (const ManifestAsset& kept : request.assets) {
    if (kept.kind != MovedKind::Staged && kept.kind != MovedKind::Stream) continue;
    const std::optional<std::size_t> offset = romOffset(map, kept.first, imageBytes);
    if (!offset || *offset + kept.bytes > imageBytes) continue;
    const bool covered = std::any_of(pieces.begin(), pieces.end(), [&](const Piece& piece) {
      return piece.offset < *offset + kept.bytes && *offset < piece.offset + piece.length;
    });
    if (covered) continue;
    // The line does not say what the range went as; a class only HDMA reaches
    // is placed as a table, the rest as a transfer. The path is the line's
    // own either way. A file named for two classes is one piece per class,
    // which group back into the one file.
    for (const RegisterClass cls : kept.classes) {
      const bool hdmaOnly = !assetDirectory(cls, MovedKind::Dma).has_value();
      admit(Piece{.offset = *offset,
                  .length = kept.bytes,
                  .cls = cls,
                  .kind = kept.kind,
                  .use = kept.kind == MovedKind::Stream ? MovedKind::Stream
                         : hdmaOnly                    ? MovedKind::Table
                                                       : MovedKind::Dma,
                  .registerAddress = 0,
                  .site = 0,
                  .landings = {}},
            "asset " + kept.file);
    }
  }

  // A total order, so two pieces of the same bytes sent two places stand in
  // register order whatever the library's sort does with equals.
  std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
    if (a.offset != b.offset) return a.offset < b.offset;
    if (a.length != b.length) return a.length > b.length;
    if (a.registerAddress != b.registerAddress) return a.registerAddress < b.registerAddress;
    if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    if (a.cls != b.cls) return static_cast<int>(a.cls) < static_cast<int>(b.cls);
    return a.site < b.site;
  });

  // The groups: pieces that share a byte are one file, if they agree on what
  // the bytes were for. A piece the shadow named agrees with any piece of its
  // class — the same bytes sent directly and sent after staging are one file —
  // and a group with an engine's piece in it is of that piece's kind; a piece
  // the code proves agrees with an engine's piece to the same register and
  // yields the kind to it, and a group of proven pieces alone is `proven`. A
  // group the shadow named whose pieces went to two classes is one file too,
  // under `staged/`, named for every class; only the engines' own pieces, and
  // the code's, sent two places, or two ways, are refused.
  out.assets.clear();
  for (std::size_t i = 0; i < pieces.size();) {
    std::size_t end = pieces[i].offset + pieces[i].length;
    std::size_t j = i + 1;
    while (j < pieces.size() && pieces[j].offset < end) {
      end = std::max(end, pieces[j].offset + pieces[j].length);
      ++j;
    }
    const Piece& first = pieces[i];
    const auto shadowed = [](const Piece& piece) {
      return piece.kind == MovedKind::Staged || piece.kind == MovedKind::Stream;
    };
    const auto moved = [](const Piece& piece) {
      return piece.kind == MovedKind::Dma || piece.kind == MovedKind::Table || piece.kind == MovedKind::Indirect;
    };
    const Piece* engine = nullptr;    // the first piece an engine moved
    const Piece* declared = nullptr;  // else the first the code proves
    bool staged = false;
    for (std::size_t k = i; k < j; ++k) {
      if (moved(pieces[k])) {
        if (engine == nullptr) engine = &pieces[k];
      } else if (pieces[k].kind == MovedKind::Proven) {
        if (declared == nullptr) declared = &pieces[k];
      } else if (pieces[k].kind == MovedKind::Staged) {
        staged = true;
      }
    }
    const Piece* named = engine != nullptr ? engine : declared;
    std::vector<RegisterClass> classes;
    bool agree = true;
    for (std::size_t k = i; k < j; ++k) {
      const Piece& other = pieces[k];
      if (std::find(classes.begin(), classes.end(), other.cls) == classes.end()) classes.push_back(other.cls);
      if (named != nullptr && !shadowed(other)) {
        if (other.registerAddress != named->registerAddress) agree = false;
        if (engine != nullptr && moved(other) && other.kind != engine->kind) agree = false;
      }
    }
    std::sort(classes.begin(), classes.end());
    const bool mixed = classes.size() > 1;
    if (mixed && named != nullptr && !staged) {
      // The engines sent the bytes two places, and nothing was built from
      // them that a stream's run would explain: refused.
      bool anyStream = false;
      for (std::size_t k = i; k < j; ++k) anyStream = anyStream || pieces[k].kind == MovedKind::Stream;
      if (!anyStream) agree = false;
    }
    const std::optional<Address> home = romAddress(map, first.offset);
    if (!agree) {
      std::string places;
      for (std::size_t k = i; k < j; ++k) {
        const std::string_view name = cpu65816RegisterName(pieces[k].registerAddress);
        const std::string place = name.empty() ? address24(pieces[k].registerAddress) : std::string(name);
        if (places.find(place) == std::string::npos) places += (places.empty() ? "" : " and ") + place;
      }
      out.notes.push_back("the bytes at " + address24(home.value_or(0)) + " were sent to " + places +
                          "; not lifted");
      i = j;
      continue;
    }
    if (home) {
      const Piece& lead = named != nullptr ? *named : first;
      std::string_view directory = mixed ? std::string_view("staged") : *assetDirectory(lead.cls, lead.use);
      // A VRAM file goes where the run saw its bytes drawn: a map, tiles, or
      // neither the run can say.
      if (directory == "vram") {
        directory = vramDirectory(std::vector<Piece>(pieces.begin() + static_cast<std::ptrdiff_t>(i),
                                                     pieces.begin() + static_cast<std::ptrdiff_t>(j)));
      }
      AssetFile asset{.file = std::string(directory) + "/" + hex(*home >> 16, 2) + "_" +
                              hex(*home & 0xFFFFu, 4) + ".bin",
                      .classes = classes,
                      .kind = mixed && named == nullptr ? (staged ? MovedKind::Staged : MovedKind::Stream)
                                                        : lead.kind,
                      .registerAddress = lead.registerAddress,
                      .first = *home,
                      .romOffset = first.offset,
                      .bytes = {}};
      asset.bytes.assign(request.rom.begin() + static_cast<std::ptrdiff_t>(first.offset),
                         request.rom.begin() + static_cast<std::ptrdiff_t>(end));
      out.assets.push_back(std::move(asset));
    }
    i = j;
  }

  // A person's path for a file lifted again. A file the shadow named that
  // nothing lifted again was re-lifted from its line above, or was covered by
  // a wider file this pass lifted, whose name is its own.
  for (const ManifestAsset& named : request.assets) {
    const auto found = std::find_if(out.assets.begin(), out.assets.end(), [&](const AssetFile& a) {
      return a.first == named.first && a.bytes.size() == named.bytes;
    });
    if (found == out.assets.end()) {
      if (named.kind == MovedKind::Staged || named.kind == MovedKind::Stream) continue;
      out.notes.push_back("asset " + named.file + " at " + address24(named.first) +
                          " names no range this run lifted; dropped");
      continue;
    }
    found->file = named.file;
  }
}

}  // namespace

std::optional<UploadCapture> captureUpload(std::span<const std::uint8_t> rom,
                                           std::uint64_t masterCycles, std::string& reason,
                                           const ProgressSink& progress) {
  SnesState cleared;
  SnesState filled;
  if (!bootUntilProgramStarts(rom, 0x00u, masterCycles, cleared, progress) ||
      !bootUntilProgramStarts(rom, 0xFFu, masterCycles, filled, progress)) {
    reason = "the audio CPU did not leave the upload stub within " + std::to_string(masterCycles) +
             " master cycles";
    return std::nullopt;
  }
  if (cleared.apu.cpu.pc != filled.apu.cpu.pc) {
    reason = "two boots started the sound program at different addresses, " +
             address16(cleared.apu.cpu.pc) + " and " + address16(filled.apu.cpu.pc);
    return std::nullopt;
  }

  UploadCapture capture;
  capture.entry = cleared.apu.cpu.pc;
  // A byte the upload wrote reads the same after both boots; one it never touched
  // reads as each boot's fill. The stub's own three direct-page bytes read the
  // same too, and are not the program's.
  std::vector<bool> written(kStubBase, false);
  for (std::uint32_t a = 0; a < kStubBase; ++a) {
    if (a >= kRegisterPage && a < kRegisterPageEnd) continue;
    if (a == 0x00u || a == 0x01u || a == 0x03u) continue;
    written[a] = cleared.apu.ram[a] == filled.apu.ram[a];
  }
  for (std::uint32_t a = 0; a < kStubBase;) {
    if (!written[a]) {
      ++a;
      continue;
    }
    const std::uint16_t start = static_cast<std::uint16_t>(a);
    std::vector<std::uint8_t> bytes;
    while (a < kStubBase && written[a]) bytes.push_back(cleared.apu.ram[a++]);
    for (UploadBlock& piece : placeInImage(rom, start, bytes)) {
      capture.blocks.push_back(std::move(piece));
    }
  }
  return capture;
}

namespace {

// The one lift: every region with its image bytes — both readings of an address
// two paths read two ways, the listing's first — into one program in address
// order, with the interrupt sequences. Run again whenever the regions change.
void liftProgram(CartridgeDisassembly& out, std::span<const std::uint8_t> rom) {
  const CartridgeMap map = out.header.map;
  ir::Program program;
  for (const RegionListing& region : out.regions) {
    const std::optional<std::size_t> start = romOffset(map, region.region.first, rom.size());
    if (!start) continue;
    const std::size_t length = static_cast<std::size_t>(region.region.last - region.region.first) + 1u;
    ir::Program lifted = ir::lift65816(region.listing, rom.subspan(*start, length), region.region.first);
    program.nodes.insert(program.nodes.end(), std::make_move_iterator(lifted.nodes.begin()),
                         std::make_move_iterator(lifted.nodes.end()));
    program.nmi = std::move(lifted.nmi);
    program.irq = std::move(lifted.irq);
  }
  std::stable_sort(program.nodes.begin(), program.nodes.end(), [](const ir::Node& a, const ir::Node& b) {
    return a.instruction.address < b.instruction.address;
  });
  out.program = std::move(program);
}

}  // namespace

CartridgeDisassembly disassembleCartridge(const CartridgeRequest& request) {
  CartridgeDisassembly out;
  out.imageBytes = request.rom.size();
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(request.rom);
  if (!header) {
    out.notes.push_back("the image is too small to hold a cartridge header at any site");
    return out;
  }
  out.header = *header;
  const CartridgeMap map = header->map;
  const std::size_t imageBytes = request.rom.size();

  // The regions, each checked to read consecutive image bytes.
  std::vector<SourceRegion> regions =
      request.regions.empty() ? bankRegions(map, imageBytes) : request.regions;
  for (auto it = regions.begin(); it != regions.end();) {
    if (contiguous(map, imageBytes, *it)) {
      ++it;
      continue;
    }
    out.notes.push_back("region " + it->file + " " + address24(it->first) + "-" +
                        address24(it->last) + " does not read consecutive image bytes; left out");
    it = regions.erase(it);
  }

  // Per region: the entries gathered so far, and whether a trace is owed.
  struct Pending {
    std::vector<Address> entries;
    std::vector<Context> contexts;
    std::set<std::pair<Address, std::uint32_t>> seen;
    std::map<Address, std::string> symbols;
    bool owed = false;
  };
  std::vector<Pending> pending(regions.size());
  auto regionOf = [&](Address address) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < regions.size(); ++i) {
      if (within(regions[i], address)) return i;
    }
    return std::nullopt;
  };
  // Adds an entry to the region that holds it: nothing when no region does, false
  // when the region already had that address under that context, true when it
  // is new and the region is owed a trace for it.
  auto add = [&](Address home, Context context, const std::string& name) -> std::optional<bool> {
    const std::optional<std::size_t> index = regionOf(home);
    if (!index) return std::nullopt;
    Pending& p = pending[*index];
    if (!name.empty() && p.symbols.find(home) == p.symbols.end()) p.symbols[home] = name;
    if (!p.seen.insert({home, context.bits}).second) return false;
    p.entries.push_back(home);
    p.contexts.push_back(context);
    p.owed = true;
    return true;
  };

  std::vector<TraceEntry> entries = vectorTraceEntries(*header);
  const std::size_t vectors = entries.size();
  for (TraceEntry entry : request.entries) {
    // A person's name that the dialect reserves is renamed the same way, and
    // the rename is said, since it was theirs.
    const std::string label = handlerLabel(entry.name);
    if (label != entry.name) {
      out.notes.push_back("entry " + entry.name + " at " + address24(entry.address) +
                          " is labelled " + label + ": `" + entry.name + "` cannot be a label");
      entry.name = label;
    }
    entries.push_back(std::move(entry));
  }
  // What a run saw the indirect jumps take: this run's, when asked for one, and
  // every earlier run's from the manifest. Each is an entry the trace starts
  // from, after the vectors and the person's entries, so a name a person gave a
  // target is the one it keeps.
  std::vector<ReachedTarget> reached = request.reached;
  // Where a run landed without an instruction naming it: this run's, when
  // asked for one, and every earlier run's from the manifest, each an entry the
  // trace starts from after the reached targets.
  std::vector<Landing> ran = request.ran;
  // What a run saw the engines move: every earlier run's from the manifest, and
  // this run's over them — a range this run saw again carries this run's count,
  // one it did not see is kept as it was.
  out.moved = request.moved;
  if (request.observeRun) {
    RunObservation observation =
        observeRun(request.rom, request.runMasterCycles, request.input, out.notes, request.progress);
    for (const ReachedTarget& seen : observation.reached) {
      const bool known = std::any_of(reached.begin(), reached.end(), [&](const ReachedTarget& r) {
        return sameSighting(r, seen);
      });
      if (!known) reached.push_back(seen);
    }
    for (const Landing& landing : observation.ran) {
      const bool known = std::any_of(ran.begin(), ran.end(), [&](const Landing& l) {
        return sameLanding(l, landing);
      });
      if (!known) ran.push_back(landing);
    }
    out.seen = std::move(observation.seen);
    out.staged = std::move(observation.staged);
    out.streamed = std::move(observation.streamed);
    out.landed = std::move(observation.landed);
    for (const MovedRange& range : observation.moved) {
      const auto known = std::find_if(out.moved.begin(), out.moved.end(),
                                      [&](const MovedRange& m) { return sameRange(m, range); });
      if (known == out.moved.end()) {
        out.moved.push_back(range);
      } else {
        known->times = range.times;
      }
    }
  }
  std::sort(reached.begin(), reached.end(), [](const ReachedTarget& a, const ReachedTarget& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return contextOf(a.mode).bits < contextOf(b.mode).bits;
  });
  std::sort(ran.begin(), ran.end(), [](const Landing& a, const Landing& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return contextOf(a.mode).bits < contextOf(b.mode).bits;
  });
  std::sort(out.moved.begin(), out.moved.end(), rangeBefore);

  // A vector's entry is written where the tree places its handler; a person's
  // is written as they gave it, in the bank they say the CPU enters in, and the
  // tree places it to trace it.
  for (std::size_t n = 0; n < entries.size(); ++n) {
    const TraceEntry& entry = entries[n];
    const std::optional<Address> home = canonical(map, imageBytes, entry.address);
    if (!home) {
      out.notes.push_back("entry " + entry.name + " at " + address24(entry.address) +
                          " is not in the image; not traced");
      continue;
    }
    const std::optional<bool> added = add(*home, contextOf(entry.mode), entry.name);
    if (!added) {
      out.notes.push_back("entry " + entry.name + " at " + address24(entry.address) +
                          " lies in no region; not traced");
      continue;
    }
    if (!*added) continue;  // the same address under the same mode, already an entry
    out.entries.push_back(
        TraceEntry{.address = n < vectors ? *home : entry.address, .mode = entry.mode, .name = entry.name});
  }
  // A target a run reached, a place it landed and a destination the bytes derive
  // are each kept as the CPU arrives there, in the bank it runs in; the tree
  // places each to trace it and to name it.
  for (const ReachedTarget& seen : reached) {
    const std::optional<Address> home = canonical(map, imageBytes, seen.target);
    if (!home) {
      out.notes.push_back("reached " + address24(seen.target) + " from " + address24(seen.site) +
                          " is not in the image; not traced");
      continue;
    }
    const std::string name = (seen.call ? "sub_" : "loc_") + hex(*home, 6);
    const std::optional<bool> added = add(*home, contextOf(seen.mode), name);
    if (!added) {
      out.notes.push_back("reached " + address24(seen.target) + " from " + address24(seen.site) +
                          " lies in no region; not traced");
      continue;
    }
    // The label the target carries: a person's, when their entry named it first.
    const std::string label = pending[*regionOf(*home)].symbols[*home];
    out.reached.push_back(ReachedTarget{
        .target = seen.target, .mode = seen.mode, .site = seen.site, .call = seen.call, .name = label});
  }
  for (const Landing& landing : ran) {
    const std::optional<Address> home = canonical(map, imageBytes, landing.target);
    if (!home) {
      out.notes.push_back("ran " + address24(landing.target) + " from " + address24(landing.site) +
                          " is not in the image; not traced");
      continue;
    }
    // The CPU arrived: a location, whatever instruction took it there.
    const std::optional<bool> added = add(*home, contextOf(landing.mode), "loc_" + hex(*home, 6));
    if (!added) {
      out.notes.push_back("ran " + address24(landing.target) + " from " + address24(landing.site) +
                          " lies in no region; not traced");
      continue;
    }
    const std::string label = pending[*regionOf(*home)].symbols[*home];
    out.ran.push_back(
        Landing{.target = landing.target, .mode = landing.mode, .site = landing.site, .name = label});
  }

  // Trace every region that is owed one, and carry each call or jump that leaves
  // a region into the region it lands in, under the mode it was made in — until
  // no region has a new entry.
  const Cpu65816Backend& backend = cpu65816Backend();
  std::vector<Listing> listings(regions.size());
  auto traceOwed = [&]() {
    bool owed = true;
    while (owed) {
      owed = false;
      for (std::size_t i = 0; i < regions.size(); ++i) {
        if (!pending[i].owed) continue;
        pending[i].owed = false;
        const SourceRegion& region = regions[i];
        const std::size_t start = *romOffset(map, region.first, imageBytes);
        const std::size_t length = static_cast<std::size_t>(region.last - region.first) + 1u;
        Request traceRequest;
        traceRequest.image = request.rom.subspan(start, length);
        traceRequest.base = region.first;
        traceRequest.entries = pending[i].entries;
        traceRequest.entryContexts = pending[i].contexts;
        traceRequest.symbols = pending[i].symbols;
        listings[i] = trace(backend, traceRequest);
        for (const Line& line : listings[i].lines) {
          if (!line.isCode) continue;
          const Instruction& instruction = line.instruction;
          const bool leaves = instruction.flow == Flow::Jump || instruction.flow == Flow::Call;
          if (!leaves || !instruction.target || within(region, *instruction.target)) continue;
          const std::optional<Address> home = canonical(map, imageBytes, *instruction.target);
          if (!home) continue;
          const std::optional<Decoded> again =
              backend.decode(traceRequest.image, region.first, line.address, line.context);
          const Context after = again ? again->next : line.context;
          const std::string prefix = instruction.flow == Flow::Call ? "sub_" : "loc_";
          const std::optional<bool> added = add(*home, after, prefix + hex(*home, 6));
          if (added) {
            if (*added) owed = true;
          } else {
            out.stops.push_back(TraceStop{
                .address = line.address,
                .reason = "`" + instruction.text + "`: the target " + address24(*instruction.target) +
                          " lies in no region"});
          }
        }
      }
    }
  };

  // The regions as they stand: every listing traced so far, and a region
  // nothing reached as all data — one run of every byte it holds.
  auto gatherRegions = [&]() {
    out.regions.clear();
    for (std::size_t i = 0; i < regions.size(); ++i) {
      Listing listing = listings[i];
      if (pending[i].entries.empty()) {
        const std::size_t start = *romOffset(map, regions[i].first, imageBytes);
        const std::size_t length = static_cast<std::size_t>(regions[i].last - regions[i].first) + 1u;
        Line line;
        line.isCode = false;
        line.address = regions[i].first;
        line.data.assign(request.rom.begin() + static_cast<std::ptrdiff_t>(start),
                         request.rom.begin() + static_cast<std::ptrdiff_t>(start + length));
        listing.addressBits = backend.addressBits();
        listing.lines.push_back(std::move(line));
      }
      out.regions.push_back(RegionListing{.region = regions[i], .listing = std::move(listing)});
    }
  };

  // What the bytes prove the indirect jumps take: every earlier run's, read back
  // from the manifest, and then this run's, derived over the traced program —
  // each an entry the trace starts from, after the vectors, the person's entries
  // and the run's sightings, so a name any of those gave a target is the one it
  // keeps. Deriving can reach code that derives more, so the trace and the
  // analysis take turns until neither adds anything.
  auto addDerived = [&](const std::vector<DerivedTarget>& found) {
    bool any = false;
    for (const DerivedTarget& derived : found) {
      const bool known = std::any_of(out.derived.begin(), out.derived.end(), [&](const DerivedTarget& d) {
        return sameDerivation(d, derived);
      });
      if (known) continue;
      const std::optional<Address> home = canonical(map, imageBytes, derived.target);
      if (!home) {
        out.notes.push_back("derived " + address24(derived.target) + " from " + address24(derived.site) +
                            " is not in the image; not traced");
        continue;
      }
      const std::string name = (derived.call ? "sub_" : "loc_") + hex(*home, 6);
      const std::optional<bool> added = add(*home, contextOf(derived.mode), name);
      if (!added) {
        out.notes.push_back("derived " + address24(derived.target) + " from " + address24(derived.site) +
                            " lies in no region; not traced");
        continue;
      }
      any = any || *added;
      const std::string label = pending[*regionOf(*home)].symbols[*home];
      out.derived.push_back(DerivedTarget{.target = derived.target,
                                          .mode = derived.mode,
                                          .site = derived.site,
                                          .pointer = derived.pointer,
                                          .call = derived.call,
                                          .name = label});
    }
    return any;
  };

  if (request.progress) request.progress(Progress{.stage = "tracing", .spent = 0, .budget = 0});
  traceOwed();
  gatherRegions();
  std::vector<DerivedTarget> readBack = request.derived;
  std::sort(readBack.begin(), readBack.end(), [](const DerivedTarget& a, const DerivedTarget& b) {
    if (a.site != b.site) return a.site < b.site;
    return a.target < b.target;
  });
  if (addDerived(readBack)) {
    traceOwed();
    gatherRegions();
  }
  if (request.progress) {
    request.progress(Progress{.stage = "proving what every path reaches", .spent = 0, .budget = 0});
  }
  std::optional<ProvenProgram> proven;
  for (;;) {
    liftProgram(out, request.rom);
    proven = proveProgram(out, request.rom);
    if (!addDerived(derivedTargets(out, *proven))) break;
    traceOwed();
    gatherRegions();
  }
  std::sort(out.derived.begin(), out.derived.end(), [](const DerivedTarget& a, const DerivedTarget& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return a.pointer < b.pointer;
  });

  // The stops: every jump or call whose successors the bytes do not name —
  // except one every destination of which the analysis derived, which is
  // answered.
  std::set<Address> derivedSites;
  for (const DerivedTarget& derived : out.derived) derivedSites.insert(derived.site);
  for (const RegionListing& region : out.regions) {
    for (const Line& line : region.listing.lines) {
      if (!line.isCode || derivedSites.count(line.address)) continue;
      if (const std::optional<std::string> reason =
              stopReason(line.instruction, map, imageBytes, region.region)) {
        out.stops.push_back(TraceStop{.address = line.address, .reason = *reason});
      }
    }
  }
  std::sort(out.stops.begin(), out.stops.end(),
            [](const TraceStop& a, const TraceStop& b) { return a.address < b.address; });

  if (request.captureSound) {
    std::string reason;
    std::optional<UploadCapture> capture =
        captureUpload(request.rom, request.bootMasterCycles, reason, request.progress);
    if (!capture) {
      out.notes.push_back("no sound program: " + reason);
    } else {
      SoundProgram sound;
      sound.file = "apu/driver.asm";
      sound.capture = std::move(*capture);
      std::vector<std::uint8_t> memory(65536u, 0u);
      std::vector<Range> blocks;
      for (const UploadBlock& block : sound.capture.blocks) {
        std::copy(block.bytes.begin(), block.bytes.end(), memory.begin() + block.apuAddress);
        blocks.push_back(Range{.first = block.apuAddress,
                               .last = block.apuAddress + static_cast<Address>(block.bytes.size()) - 1u});
      }
      Request traceRequest;
      traceRequest.image = memory;
      traceRequest.base = 0;
      traceRequest.entries = {sound.capture.entry};
      traceRequest.symbols[sound.capture.entry] = "entry";
      // Two blocks that landed end to end are one run of the program: an
      // instruction across their edge is kept whole.
      sound.listing = keepRanges(trace(spc700Backend(), traceRequest), joined(blocks));
      // The sound program lifted once, as every region's code was: one node
      // per code line, in the audio unit's address order.
      out.program.spc700 = ir::liftSpc700(sound.listing);
      out.sound = std::move(sound);
    }
  }

  // What the traced code reaches, the routines that reach it, and what every
  // path proves. Read off the finished listings and the program proven over
  // them, so a byte the trace never entered contributes nothing.
  out.accesses = hardwareAccesses(out, &*proven);
  out.dmas = dmaTransfers(out.accesses);
  out.routines = routines(out);
  out.states = stateFacts(out, *proven);

  // The files lifted out of the banks: what the run saw the engines carry from
  // the image, and the sources of what they carried out of work RAM, now that
  // the listings say where the instructions are, the sound program says where
  // its blocks are, and the routines say who wrote what.
  liftAssets(out, request);
  return out;
}

Placement placeBytes(const CartridgeDisassembly& disassembly) {
  Placement placement;
  placement.image.assign(disassembly.imageBytes, 0u);
  std::vector<std::uint8_t> count(disassembly.imageBytes, 0u);
  const CartridgeMap map = disassembly.header.map;
  auto place = [&](std::size_t offset, std::uint8_t byte) {
    if (offset >= placement.image.size()) return;
    placement.image[offset] = byte;
    if (count[offset] < 2) ++count[offset];
  };
  const RenderInput input = renderInputOf(disassembly);
  for (const RenderRegion& region : input.regions) {
    const Listing lines = regionLines(region, input);
    for (const Line& line : lines.lines) {
      const std::optional<std::size_t> start = romOffset(map, line.address, disassembly.imageBytes);
      if (!start) continue;
      const std::vector<std::uint8_t>& bytes = line.isCode ? line.instruction.bytes : line.data;
      for (std::size_t i = 0; i < bytes.size(); ++i) place(*start + i, bytes[i]);
    }
  }
  if (disassembly.sound) {
    for (const UploadBlock& block : disassembly.sound->capture.blocks) {
      if (!block.romOffset) continue;
      for (std::size_t i = 0; i < block.bytes.size(); ++i) place(*block.romOffset + i, block.bytes[i]);
    }
  }
  for (const AssetFile& asset : disassembly.assets) {
    for (std::size_t i = 0; i < asset.bytes.size(); ++i) place(asset.romOffset + i, asset.bytes[i]);
  }
  for (const std::uint8_t c : count) {
    if (c == 0) ++placement.unplaced;
    if (c > 1) ++placement.placedTwice;
  }
  return placement;
}

std::string renderManifest(const CartridgeDisassembly& disassembly) {
  std::string out;
  out += "; A Snaggletooth cartridge project. The next run reads the `entry`, `reached`,\n"
         "; `ran`, `derived`, `moved`, `asset` and `file` lines; snes_verify reads `map`,\n"
         "; `file`, `sound` and `block`; everything else is written fresh from what the run\n"
         "; found.\n";
  out += "image    " + std::to_string(disassembly.imageBytes) + "\n";
  out += "map      " + mapName(disassembly.header.map) + "\n";
  std::string title;
  for (const char c : disassembly.header.title) {
    if (c >= 0x20 && c < 0x7F && c != '"') title.push_back(c);
  }
  out += "title    \"" + title + "\"\n";
  out += "checksum $" + hex(disassembly.header.checksum, 4) + " $" +
         hex(disassembly.header.complement, 4) + "\n";

  out += "\n";
  for (const RegionListing& region : disassembly.regions) {
    out += "file     " + region.region.file + " 65816 " + address24(region.region.first) + " " +
           address24(region.region.last) + "\n";
  }
  if (disassembly.sound) {
    const SoundProgram& sound = *disassembly.sound;
    out += "sound    " + sound.file + " SPC700 entry " + address16(sound.capture.entry) + "\n";
    for (const UploadBlock& block : sound.capture.blocks) {
      out += "block    " + sound.file + " " + address16(block.apuAddress) + " " +
             std::to_string(block.bytes.size()) + " ";
      out += block.romOffset ? "at $" + hex(static_cast<std::uint32_t>(*block.romOffset), 6)
                             : std::string("unplaced");
      out += "\n";
    }
  }

  out += "\n";
  for (const TraceEntry& entry : disassembly.entries) {
    out += "entry    " + address24(entry.address) + " " + entry.name + " " + modeText(entry.mode) +
           "\n";
  }
  if (!disassembly.reached.empty()) out += "\n";
  for (const ReachedTarget& seen : disassembly.reached) {
    out += "reached  " + address24(seen.target) + " " + seen.name + " " + modeText(seen.mode) +
           " from " + address24(seen.site) + "\n";
  }
  if (!disassembly.ran.empty()) out += "\n";
  for (const Landing& landing : disassembly.ran) {
    out += "ran      " + address24(landing.target) + " " + landing.name + " " +
           modeText(landing.mode) + " from " + address24(landing.site) + "\n";
  }
  if (!disassembly.derived.empty()) out += "\n";
  for (const DerivedTarget& derived : disassembly.derived) {
    out += "derived  " + address24(derived.target) + " " + derived.name + " " + modeText(derived.mode) +
           " from " + address24(derived.site) + " via " + address24(derived.pointer) + "\n";
  }

  if (!disassembly.stops.empty()) out += "\n";
  for (const TraceStop& stop : disassembly.stops) {
    out += "stop     " + address24(stop.address) + " " + stop.reason + "\n";
  }
  bool anyWarning = false;
  for (const RegionListing& region : disassembly.regions) {
    for (const std::string& warning : region.listing.warnings) {
      if (!anyWarning) out += "\n";
      anyWarning = true;
      out += "warning  " + region.region.file + " " + warning + "\n";
    }
  }
  if (disassembly.sound) {
    for (const std::string& warning : disassembly.sound->listing.warnings) {
      if (!anyWarning) out += "\n";
      anyWarning = true;
      out += "warning  " + disassembly.sound->file + " " + warning + "\n";
    }
  }
  if (!disassembly.notes.empty()) out += "\n";
  for (const std::string& note : disassembly.notes) out += "note     " + note + "\n";

  // What the code reaches. Every field is present on every line; `none` is a
  // field the bytes did not say, which is a fact about the cartridge and not a
  // gap in the format.
  if (!disassembly.accesses.empty()) out += "\n";
  for (const HardwareAccess& access : disassembly.accesses) {
    out += "access   " + address24(access.site) + " " + std::string(access.name) + " " +
           std::string(cpu65816RegisterClassName(access.cls)) + " " +
           std::string(accessKindName(access.kind)) + " " +
           (access.value ? "$" + hex(*access.value, 2) : std::string("none")) + "\n";
  }
  if (!disassembly.dmas.empty()) out += "\n";
  for (const DmaTransfer& dma : disassembly.dmas) {
    out += "dma      " + address24(dma.site) + " channel " + std::to_string(dma.channel) + " " +
           std::string(dmaDirectionName(dma.direction)) + " " +
           (dma.destination ? address24(*dma.destination) : std::string("none")) + " " +
           (dma.destinationName.empty() ? std::string("none") : std::string(dma.destinationName)) +
           " " +
           (dma.destinationClass ? std::string(cpu65816RegisterClassName(*dma.destinationClass))
                                 : std::string("none")) +
           " source " + (dma.source ? address24(*dma.source) : std::string("none")) + " " +
           (dma.step ? std::string(movedStepName(*dma.step)) : std::string("none")) + " bytes " +
           (dma.bytes ? std::to_string(*dma.bytes) : std::string("none")) + " " +
           (dma.startMask ? (dma.hdma ? "start-hdma" : "start") : "start") + " " +
           (dma.startMask ? "$" + hex(*dma.startMask, 2) : std::string("none")) + " from " +
           (dma.startSite ? address24(*dma.startSite) : std::string("none")) + "\n";
  }

  // What a run saw move. Every field is present; a B-bus address no register
  // has writes `none` for the name and the class, as a `dma` line does.
  if (!disassembly.moved.empty()) out += "\n";
  for (const MovedRange& range : disassembly.moved) {
    out += "moved    " + address24(range.site) + " channel " + std::to_string(range.channel) + " " +
           (range.toRegister ? "to-register" : "from-register") + " " +
           address24(range.registerAddress) + " " +
           (range.registerName.empty() ? std::string("none") : std::string(range.registerName)) +
           " " +
           (range.registerClass ? std::string(cpu65816RegisterClassName(*range.registerClass))
                                : std::string("none")) +
           " memory " + address24(range.memory) + " " + std::string(movedStepName(range.step)) +
           " bytes " + std::to_string(range.bytes) + " as " + std::string(movedKindName(range.kind)) +
           " times " + std::to_string(range.times) + "\n";
  }

  // Where the ranges the run moved landed on the other side of the port, and
  // what the PPU used the memory as at the first frame drawn after.
  if (!disassembly.landed.empty()) out += "\n";
  for (const LandedRange& landed : disassembly.landed) {
    out += "landed   " + address24(landed.site) + " channel " + std::to_string(landed.channel) + " memory " +
           address24(landed.memory) + " bytes " + std::to_string(landed.bytes) + " as " +
           std::string(movedKindName(landed.kind)) + " at " +
           portAddressText(landed.landing.memory, landed.landing.lowest) + "-" +
           portAddressText(landed.landing.memory, landed.landing.highest) + " in " + areaText(landed.landing) +
           " times " + std::to_string(landed.times) + "\n";
  }

  // Where every range carried out of work RAM came from: one line per source
  // and image hull, one per register whose value entered, one for the save,
  // and one saying `computed` for a source built from constants alone.
  const std::map<Address, std::string> routineOf = routineByLine(disassembly);
  const CartridgeMap map = disassembly.header.map;
  std::string origins;
  for (const StagedRange& range : disassembly.staged) {
    const std::string head = "origin   " + address24(range.memory) + " bytes " +
                             std::to_string(range.bytes) + " ";
    for (const StagedSource& source : stagedSources(range, routineOf)) {
      const std::string by = " by " + source.label;
      if (source.label == "unwritten") {
        origins += head + "unwritten\n";
        continue;
      }
      if (source.origin.empty()) {
        origins += head + "computed" + by + "\n";
        continue;
      }
      for (const ir::OriginInterval& run : source.sources) {
        origins += head + "from " + address24(romAddress(map, run.first).value_or(0)) + " bytes " +
                   std::to_string(run.last - run.first + 1u) + " using " +
                   std::to_string(usedWithin(source.origin, run)) + by + " " +
                   std::string(originMark(source.origin)) + "\n";
      }
      for (const std::uint32_t reg : source.origin.registers) {
        const std::string_view name = cpu65816RegisterName(reg);
        origins += head + "from register " + address24(reg) + " " +
                   (name.empty() ? std::string("none") : std::string(name)) + by + "\n";
      }
      if (source.origin.save) origins += head + "from save" + by + "\n";
    }
  }
  if (!origins.empty()) out += "\n" + origins;

  // The files lifted out of the banks. The `moved` lines are their uses.
  if (!disassembly.assets.empty()) out += "\n";
  for (const AssetFile& asset : disassembly.assets) {
    out += "asset    " + asset.file + " " + classesText(asset.classes, "+") + " as " +
           std::string(movedKindName(asset.kind)) + " from " + address24(asset.first) + " bytes " +
           std::to_string(asset.bytes.size()) + "\n";
  }

  // What the run built from each file: one line per file, staged extent whose
  // source lies in it, class the extent went to, and writer.
  std::vector<std::string> stagedLines;
  for (const StagedRange& range : disassembly.staged) {
    std::vector<RegisterClass> classes;
    for (const ExtentUse& use : extentUses(disassembly, range)) {
      if (std::find(classes.begin(), classes.end(), use.cls) == classes.end()) classes.push_back(use.cls);
    }
    std::sort(classes.begin(), classes.end());
    for (const StagedSource& source : stagedSources(range, routineOf)) {
      for (const ir::OriginInterval& run : source.sources) {
        for (const AssetFile& asset : disassembly.assets) {
          if (run.first < asset.romOffset || run.last >= asset.romOffset + asset.bytes.size()) continue;
          for (const RegisterClass cls : classes) {
            const std::string line = "staged   " + asset.file + " at " + address24(range.memory) + " bytes " +
                                     std::to_string(range.bytes) + " to " +
                                     std::string(cpu65816RegisterClassName(cls)) + " by " + source.label + " " +
                                     std::string(originMark(source.origin)) + "\n";
            if (std::find(stagedLines.begin(), stagedLines.end(), line) == stagedLines.end()) {
              stagedLines.push_back(line);
            }
          }
        }
      }
    }
  }
  if (!stagedLines.empty()) out += "\n";
  for (const std::string& line : stagedLines) out += line;

  // What the CPU streamed a byte at a time: from the image, or from a buffer
  // in work RAM.
  if (!disassembly.streamed.empty()) out += "\n";
  for (const StreamedRange& stream : disassembly.streamed) {
    out += "streamed " + address24(stream.site) + " " + address24(stream.registerAddress) + " " +
           (stream.registerName.empty() ? std::string("none") : std::string(stream.registerName)) +
           " " +
           (stream.registerClass ? std::string(cpu65816RegisterClassName(*stream.registerClass))
                                 : std::string("none")) +
           " from " +
           address24(stream.memory ? *stream.memory : romAddress(map, stream.romOffset).value_or(0)) +
           " bytes " + std::to_string(stream.bytes) + " times " + std::to_string(stream.times) + " at " +
           (stream.landing ? portAddressText(stream.landing->memory, stream.landing->lowest) + "-" +
                                 portAddressText(stream.landing->memory, stream.landing->highest)
                           : std::string("none")) +
           " in " + (stream.landing ? areaText(*stream.landing) : std::string("none")) + "\n";
  }

  // The routines. A list is one field, its names joined by commas; an empty
  // list is `none`, which again is a fact about the routine.
  std::map<Address, std::string> routineLabels;
  for (const Routine& routine : disassembly.routines) {
    routineLabels[routine.address] = routine.label;
  }
  const auto classList = [](const std::vector<RegisterClass>& classes) {
    if (classes.empty()) return std::string("none");
    std::string text;
    for (const RegisterClass cls : classes) {
      if (!text.empty()) text += ",";
      text += std::string(cpu65816RegisterClassName(cls));
    }
    return text;
  };
  if (!disassembly.routines.empty()) out += "\n";
  for (const Routine& routine : disassembly.routines) {
    std::string calls;
    for (const Address callee : routine.calls) {
      if (!calls.empty()) calls += ",";
      calls += routineLabels.at(callee);
    }
    out += "routine  " + address24(routine.address) + " " + routine.label + " lines " +
           std::to_string(routine.lines.size()) + " bytes " + std::to_string(routine.bytes) +
           " calls " + (calls.empty() ? std::string("none") : calls) + " reaches " +
           classList(routine.reaches) + " through " + classList(routine.through) + "\n";
  }

  // What every path proves at each label. A field is the value, `?` where it is
  // not known, and the values joined by `|` where the paths disagree; a
  // disagreement of more values than a reader can hold at once is not known
  // either.
  constexpr std::size_t kMostShown = 8;
  const auto valueList = [](const std::vector<std::uint32_t>& values, int digits) {
    if (values.empty() || values.size() > kMostShown) return std::string("?");
    std::string text;
    for (const std::uint32_t value : values) {
      if (!text.empty()) text += "|";
      text += "$" + hex(value, digits);
    }
    return text;
  };
  if (!disassembly.states.empty()) out += "\n";
  for (const StateFact& state : disassembly.states) {
    out += "state    " + address24(state.address) + " D=" + valueList(state.d, 4) +
           " DBR=" + valueList(state.dbr, 2) + " S=" + valueList(state.s, 4) + "\n";
  }

  // What the run saw at each site: every value, joined by `|`, since the run
  // saw each of them and a person reading the line is owed all of it.
  const auto seenList = [](const auto& values, int digits) {
    std::string text;
    for (const auto value : values) {
      if (!text.empty()) text += "|";
      text += "$" + hex(value, digits);
    }
    return text;
  };
  if (!disassembly.seen.empty()) out += "\n";
  for (const SeenState& seen : disassembly.seen) {
    out += "seen     " + address24(seen.address) + " D=" + seenList(seen.d, 4) +
           " DBR=" + seenList(seen.dbr, 2) + "\n";
  }
  return out;
}

namespace {

// What the program file carries beside the program: the image line, and each
// region with its file, its range, its warnings, its labels and its data runs.
ir::ProgramFile programFileOf(const CartridgeDisassembly& disassembly) {
  ir::ProgramFile file;
  file.imageBytes = disassembly.imageBytes;
  file.map = mapName(disassembly.header.map);
  for (const RegionListing& region : disassembly.regions) {
    ir::ProgramRegion out;
    out.file = region.region.file;
    out.first = region.region.first;
    out.last = region.region.last;
    out.warnings = region.listing.warnings;
    for (const auto& [address, name] : region.listing.labels) out.labels.push_back({address, name});
    for (const Line& line : region.listing.lines) {
      if (!line.isCode && !line.data.empty()) out.data.push_back({line.address, line.data});
    }
    file.regions.push_back(std::move(out));
  }
  return file;
}

// What the sound program's file carries beside its nodes: the image line, and
// one region per run of uploaded addresses — two blocks that landed end to end
// are one run — with the listing's warnings on the first, and the labels and
// the data runs that lie within each.
ir::ProgramFile soundFileOf(const CartridgeDisassembly& disassembly) {
  if (!disassembly.sound) throw std::logic_error("no sound program was captured");
  const SoundProgram& sound = *disassembly.sound;
  ir::ProgramFile file;
  file.processor = ir::Processor::Spc700;
  file.imageBytes = disassembly.imageBytes;
  file.map = mapName(disassembly.header.map);
  std::vector<Range> ranges;
  for (const UploadBlock& block : sound.capture.blocks) {
    ranges.push_back(Range{.first = block.apuAddress,
                           .last = block.apuAddress + static_cast<Address>(block.bytes.size()) - 1u});
  }
  for (const Range& range : joined(ranges)) {
    ir::ProgramRegion out;
    out.file = sound.file;
    out.first = range.first;
    out.last = range.last;
    if (file.regions.empty()) out.warnings = sound.listing.warnings;
    for (const auto& [address, name] : sound.listing.labels) {
      if (address >= range.first && address <= range.last) out.labels.push_back({address, name});
    }
    for (const Line& line : sound.listing.lines) {
      if (line.isCode || line.data.empty()) continue;
      if (line.address >= range.first && line.address <= range.last) out.data.push_back({line.address, line.data});
    }
    file.regions.push_back(std::move(out));
  }
  return file;
}

bool writeFile(const std::filesystem::path& path, std::string_view text, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    error = "cannot write " + path.string();
    return false;
  }
  out << text;
  if (!out) {
    error = "cannot write " + path.string();
    return false;
  }
  return true;
}

}  // namespace

RenderInput renderInputOf(const CartridgeDisassembly& disassembly) {
  RenderInput input;
  input.map = disassembly.header.map;
  input.imageBytes = disassembly.imageBytes;
  for (const RegionListing& region : disassembly.regions) {
    input.regions.push_back(RenderRegion{.file = region.region.file,
                                         .first = region.region.first,
                                         .last = region.region.last,
                                         .listing = region.listing});
  }
  for (const HardwareAccess& access : disassembly.accesses) {
    input.accesses.push_back(
        RenderAccess{.site = access.site, .registerAddress = access.registerAddress, .name = access.name});
  }
  for (const SeenState& seen : disassembly.seen) {
    input.seen.push_back(RenderSeen{.address = seen.address, .d = seen.d});
  }
  for (const Routine& routine : disassembly.routines) {
    input.routines.push_back(RenderRoutine{.address = routine.address,
                                           .label = routine.label,
                                           .lines = routine.lines.size(),
                                           .bytes = routine.bytes,
                                           .calls = routine.calls,
                                           .reaches = routine.reaches,
                                           .through = routine.through});
  }
  for (const AssetFile& asset : disassembly.assets) {
    input.assets.push_back(RenderAsset{.file = asset.file,
                                       .classes = asset.classes,
                                       .kind = asset.kind,
                                       .registerAddress = asset.registerAddress,
                                       .first = asset.first,
                                       .bytes = asset.bytes.size()});
  }
  if (disassembly.sound) {
    RenderSound sound;
    sound.file = disassembly.sound->file;
    sound.entry = disassembly.sound->capture.entry;
    for (const UploadBlock& block : disassembly.sound->capture.blocks) {
      sound.blocks.push_back(
          RenderBlock{.apuAddress = block.apuAddress, .bytes = block.bytes.size(), .romOffset = block.romOffset});
    }
    sound.regions = soundFileOf(disassembly).regions;
    input.sound = std::move(sound);
  }
  return input;
}

std::string renderRegion(const RegionListing& region, const CartridgeDisassembly& disassembly) {
  const RenderInput input = renderInputOf(disassembly);
  for (const RenderRegion& candidate : input.regions) {
    if (candidate.file == region.region.file) return renderRegion(candidate, input, disassembly.program);
  }
  throw std::logic_error("the disassembly has no region " + region.region.file);
}

std::string renderSoundFile(const CartridgeDisassembly& disassembly) {
  if (!disassembly.sound) throw std::logic_error("no sound program was captured");
  return renderSoundFile(renderInputOf(disassembly), disassembly.program);
}

std::string renderProgramFile(const CartridgeDisassembly& disassembly) {
  return ir::renderProgram(disassembly.program, programFileOf(disassembly));
}

std::string renderSoundProgramFile(const CartridgeDisassembly& disassembly) {
  return ir::renderProgram(disassembly.program, soundFileOf(disassembly));
}

bool writeProject(const CartridgeDisassembly& disassembly, const std::filesystem::path& directory,
                  std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    error = "cannot create " + directory.string() + ": " + ec.message();
    return false;
  }
  // The program files are the first things under the directory; everything
  // after them is what the disassembly found beside the programs.
  if (!writeFile(directory / "program.snagir", renderProgramFile(disassembly), error)) return false;
  if (disassembly.sound && !writeFile(directory / "apu.snagir", renderSoundProgramFile(disassembly), error)) {
    return false;
  }
  if (!writeFile(directory / "project.snagifest", renderManifest(disassembly), error)) return false;
  for (const AssetFile& asset : disassembly.assets) {
    const std::string_view bytes(reinterpret_cast<const char*>(asset.bytes.data()), asset.bytes.size());
    if (!writeFile(directory / asset.file, bytes, error)) return false;
  }
  return true;
}

}  // namespace snaggletooth::disasm
