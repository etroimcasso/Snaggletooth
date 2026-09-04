#include "rom/rom_disasm.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "cpu65816/cpu65816_asm.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_render.h"
#include "rom/cartridge_entries.h"
#include "snaggletooth/snes/snes.h"
#include "spc700/spc700_disasm.h"

namespace snaggletooth::disasm {
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

std::string hex(std::uint32_t value, int digits) {
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "%0*X", digits, static_cast<unsigned>(value));
  return buffer;
}

std::string address24(Address address) { return formatAddress(address, 24); }
std::string address16(Address address) { return formatAddress(address, 16); }

std::string mapName(CartridgeMap map) {
  switch (map) {
    case CartridgeMap::LoRom: return "LoROM";
    case CartridgeMap::HiRom: return "HiROM";
    case CartridgeMap::ExHiRom: return "ExHiROM";
  }
  return "LoROM";
}

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

// An inclusive address range.
struct Range {
  Address first = 0;
  Address last = 0;
};

// The lines of `listing` that fall inside `keep`, in order. A data run is cut to
// the parts inside. An instruction is kept when the whole of it is inside one
// range, and a label only when its instruction is, so the cut listing defines
// every label it holds; an instruction that runs past a range's edge is not an
// instruction of the cut listing, and the bytes of it that are inside are kept
// as data, so no byte inside a range goes unwritten.
Listing keepRanges(const Listing& listing, const std::vector<Range>& keep) {
  Listing kept;
  kept.warnings = listing.warnings;
  kept.addressBits = listing.addressBits;
  auto keepData = [&](Address address, const std::vector<std::uint8_t>& data) {
    const Address end = address + static_cast<Address>(data.size()) - 1u;
    for (const Range& range : keep) {
      const Address from = std::max(address, range.first);
      const Address to = std::min(end, range.last);
      if (from > to) continue;
      Line piece;
      piece.isCode = false;
      piece.address = from;
      piece.data.assign(data.begin() + static_cast<std::ptrdiff_t>(from - address),
                        data.begin() + static_cast<std::ptrdiff_t>(to - address) + 1);
      kept.lines.push_back(std::move(piece));
    }
  };
  for (const Line& line : listing.lines) {
    if (line.isCode) {
      const Address end = line.address + line.instruction.length - 1u;
      bool whole = false;
      for (const Range& range : keep) {
        if (line.address >= range.first && end <= range.last) {
          kept.lines.push_back(line);
          if (const auto label = listing.labels.find(line.address); label != listing.labels.end()) {
            kept.labels.insert(*label);
          }
          whole = true;
          break;
        }
      }
      if (!whole) keepData(line.address, line.instruction.bytes);
      continue;
    }
    if (!line.data.empty()) keepData(line.address, line.data);
  }
  return kept;
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

Address lineEnd(const Line& line) {
  return line.isCode ? line.address + line.instruction.length
                     : line.address + static_cast<Address>(line.data.size());
}

// The listing as text, one piece per run of consecutive lines, each piece under
// its own `ORG`. `between` names what lies in the gap before a piece. Every
// piece is a region to an assembler, so the first instruction of a piece carries
// the directives a region's start needs, whatever the line above the cut left.
template <typename Between>
std::string renderPieces(const Listing& listing, const Backend& backend, Between between) {
  std::string out;
  Listing piece;
  piece.labels = listing.labels;
  piece.warnings = listing.warnings;
  piece.addressBits = listing.addressBits;
  std::optional<Address> previousEnd;
  auto flush = [&]() {
    if (piece.lines.empty()) return;
    out += render(piece);
    piece.lines.clear();
    piece.warnings.clear();
  };
  for (const Line& line : listing.lines) {
    if (previousEnd && line.address != *previousEnd) {
      flush();
      out += "\n" + between(*previousEnd, line.address - 1u) + "\n";
    }
    piece.lines.push_back(line);
    if (piece.lines.size() == 1 && line.isCode) {
      piece.lines.back().directives = backend.directives(std::nullopt, line.context);
    }
    previousEnd = lineEnd(line);
  }
  flush();
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
                            std::uint64_t masterCycles, SnesState& state) {
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
  while (spent < masterCycles) {
    const SnesState before = machine.state();
    spent += machine.run(kWatchStep);
    if (!left()) continue;
    // Back to the step's start and forward a cycle at a time, to the first cycle
    // the program counter is out of the window: the jump has just landed and the
    // counter is the program's entry.
    machine.restore(before);
    for (std::uint64_t i = 0; i < kWatchStep * 4u && !left(); ++i) machine.run(1);
    state = machine.state();
    return true;
  }
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

// The uploaded blocks' bytes as address ranges within a region, for the bytes
// the region's file leaves to the sound program.
std::vector<Range> placedBlockRanges(const CartridgeDisassembly& disassembly,
                                     const SourceRegion& region) {
  std::vector<Range> ranges;
  if (!disassembly.sound) return ranges;
  const CartridgeMap map = disassembly.header.map;
  const std::optional<std::size_t> start = romOffset(map, region.first, disassembly.imageBytes);
  if (!start) return ranges;
  const std::size_t length = static_cast<std::size_t>(region.last - region.first) + 1u;
  for (const UploadBlock& block : disassembly.sound->capture.blocks) {
    if (!block.romOffset) continue;
    const std::size_t from = std::max(*block.romOffset, *start);
    const std::size_t to = std::min(*block.romOffset + block.bytes.size(), *start + length);
    if (from >= to) continue;
    ranges.push_back(Range{.first = region.first + static_cast<Address>(from - *start),
                           .last = region.first + static_cast<Address>(to - *start) - 1u});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.first < b.first; });
  return ranges;
}

// The region's range with `cut` taken out.
std::vector<Range> without(const SourceRegion& region, const std::vector<Range>& cut) {
  std::vector<Range> keep;
  Address next = region.first;
  for (const Range& range : cut) {
    if (range.first > next) keep.push_back(Range{.first = next, .last = range.first - 1u});
    next = std::max(next, range.last + 1u);
  }
  if (next <= region.last) keep.push_back(Range{.first = next, .last = region.last});
  return keep;
}

// The region's listing with the sound program's bytes left out.
Listing regionLines(const RegionListing& region, const CartridgeDisassembly& disassembly) {
  return keepRanges(region.listing, without(region.region, placedBlockRanges(disassembly, region.region)));
}

std::vector<std::string> tokens(std::string_view line) {
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
std::optional<std::uint32_t> parseHex(std::string_view digits) {
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
std::optional<Address> parseLongAddress(const std::string& text) {
  if (text.size() != 8 || text[0] != '$' || text[3] != ':') return std::nullopt;
  return parseHex(text.substr(1, 2) + text.substr(4, 4));
}

// A 16-bit address in the SPC700 dialect's form, `$XXXX`.
std::optional<std::uint16_t> parseShortAddress(const std::string& text) {
  if (text.size() != 5 || text[0] != '$') return std::nullopt;
  const std::optional<std::uint32_t> value = parseHex(text.substr(1));
  if (!value) return std::nullopt;
  return static_cast<std::uint16_t>(*value);
}

// An image offset as the manifest writes one: `$` and six hexadecimal digits.
std::optional<std::size_t> parseOffset(const std::string& text) {
  if (text.size() != 7 || text[0] != '$') return std::nullopt;
  const std::optional<std::uint32_t> value = parseHex(text.substr(1));
  if (!value) return std::nullopt;
  return static_cast<std::size_t>(*value);
}

// A decimal count.
std::optional<std::size_t> parseCount(const std::string& text) {
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
bool parseWidth(const std::string& token, char letter, bool& known, bool& eight) {
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

std::optional<Cpu65816Mode> parseMode(const std::vector<std::string>& words, std::size_t from) {
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

std::optional<UploadCapture> captureUpload(std::span<const std::uint8_t> rom,
                                           std::uint64_t masterCycles, std::string& reason) {
  SnesState cleared;
  SnesState filled;
  if (!bootUntilProgramStarts(rom, 0x00u, masterCycles, cleared) ||
      !bootUntilProgramStarts(rom, 0xFFu, masterCycles, filled)) {
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
  if (request.observeRun) {
    for (const ReachedTarget& seen :
         observeRun(request.rom, request.runMasterCycles, request.input, out.notes)) {
      const bool known = std::any_of(reached.begin(), reached.end(), [&](const ReachedTarget& r) {
        return sameSighting(r, seen);
      });
      if (!known) reached.push_back(seen);
    }
  }
  std::sort(reached.begin(), reached.end(), [](const ReachedTarget& a, const ReachedTarget& b) {
    if (a.site != b.site) return a.site < b.site;
    if (a.target != b.target) return a.target < b.target;
    return contextOf(a.mode).bits < contextOf(b.mode).bits;
  });

  for (const TraceEntry& entry : entries) {
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
    out.entries.push_back(TraceEntry{.address = *home, .mode = entry.mode, .name = entry.name});
  }
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
        .target = *home, .mode = seen.mode, .site = seen.site, .call = seen.call, .name = label});
  }

  // Trace every region that is owed one, and carry each call or jump that leaves
  // a region into the region it lands in, under the mode it was made in — until
  // no region has a new entry.
  const Cpu65816Backend& backend = cpu65816Backend();
  std::vector<Listing> listings(regions.size());
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

  for (std::size_t i = 0; i < regions.size(); ++i) {
    // A region nothing reached is all data: one run of every byte it holds.
    if (pending[i].entries.empty()) {
      const std::size_t start = *romOffset(map, regions[i].first, imageBytes);
      const std::size_t length = static_cast<std::size_t>(regions[i].last - regions[i].first) + 1u;
      Line line;
      line.isCode = false;
      line.address = regions[i].first;
      line.data.assign(request.rom.begin() + static_cast<std::ptrdiff_t>(start),
                       request.rom.begin() + static_cast<std::ptrdiff_t>(start + length));
      listings[i].addressBits = backend.addressBits();
      listings[i].lines.push_back(std::move(line));
    }
    out.regions.push_back(RegionListing{.region = regions[i], .listing = std::move(listings[i])});
    for (const Line& line : out.regions.back().listing.lines) {
      if (!line.isCode) continue;
      if (const std::optional<std::string> reason =
              stopReason(line.instruction, map, imageBytes, regions[i])) {
        out.stops.push_back(TraceStop{.address = line.address, .reason = *reason});
      }
    }
  }
  std::sort(out.stops.begin(), out.stops.end(),
            [](const TraceStop& a, const TraceStop& b) { return a.address < b.address; });

  if (request.captureSound) {
    std::string reason;
    std::optional<UploadCapture> capture =
        captureUpload(request.rom, request.bootMasterCycles, reason);
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
      out.sound = std::move(sound);
    }
  }

  // What the traced code reaches, and the routines that reach it. Read off the
  // finished listings, so a byte the trace never entered contributes nothing.
  out.accesses = hardwareAccesses(out);
  out.dmas = dmaTransfers(out.accesses);
  out.routines = routines(out);
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
  for (const RegionListing& region : disassembly.regions) {
    const Listing lines = regionLines(region, disassembly);
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
  for (const std::uint8_t c : count) {
    if (c == 0) ++placement.unplaced;
    if (c > 1) ++placement.placedTwice;
  }
  return placement;
}

std::string renderManifest(const CartridgeDisassembly& disassembly) {
  std::string out;
  out += "; A Snaggletooth cartridge project. The next run reads the `entry` and `file`\n"
         "; lines; snes_verify reads `map`, `file`, `sound` and `block`; everything else\n"
         "; is written fresh from what the run found.\n";
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
           (dma.startMask ? (dma.hdma ? "start-hdma" : "start") : "start") + " " +
           (dma.startMask ? "$" + hex(*dma.startMask, 2) : std::string("none")) + "\n";
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
  return out;
}

std::optional<ManifestInput> parseManifest(std::string_view text, std::string& error) {
  ManifestInput input;
  std::size_t number = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t end = text.find('\n', position);
    std::string_view line = text.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                                                : end - position);
    position = end == std::string_view::npos ? text.size() + 1 : end + 1;
    ++number;
    if (const std::size_t comment = line.find(';'); comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    const std::vector<std::string> words = tokens(line);
    if (words.empty()) continue;
    auto fail = [&](const std::string& what) {
      error = "line " + std::to_string(number) + ": " + what;
      return std::nullopt;
    };
    if (words[0] == "entry") {
      if (words.size() != 6) return fail("an entry is an address, a name, and e=, m=, x=");
      const std::optional<Address> address = parseLongAddress(words[1]);
      if (!address) return fail(words[1] + " is not a $BB:XXXX address");
      const std::optional<Cpu65816Mode> mode = parseMode(words, 3);
      if (!mode) return fail("the mode is e=0|1 m=8|16|? x=8|16|?");
      input.entries.push_back(TraceEntry{.address = *address, .mode = *mode, .name = words[2]});
      continue;
    }
    if (words[0] == "reached") {
      if (words.size() != 8 || words[6] != "from") {
        return fail("a reached target is an address, a name, e=, m=, x=, `from` and the site");
      }
      const std::optional<Address> target = parseLongAddress(words[1]);
      const std::optional<Address> site = parseLongAddress(words[7]);
      if (!target || !site) return fail("addresses are written $BB:XXXX");
      const std::optional<Cpu65816Mode> mode = parseMode(words, 3);
      if (!mode) return fail("the mode is e=0|1 m=8|16|? x=8|16|?");
      input.reached.push_back(ReachedTarget{.target = *target,
                                            .mode = *mode,
                                            .site = *site,
                                            .call = words[2].rfind("sub_", 0) == 0,
                                            .name = words[2]});
      continue;
    }
    if (words[0] == "file") {
      if (words.size() != 5) return fail("a file is a path, 65816, and its first and last address");
      if (words[2] != "65816") return fail(words[2] + " is not a chip a file can be written for");
      const std::optional<Address> first = parseLongAddress(words[3]);
      const std::optional<Address> last = parseLongAddress(words[4]);
      if (!first || !last) return fail("addresses are written $BB:XXXX");
      if (*last < *first) return fail("the last address is before the first");
      if ((*first >> 16) != (*last >> 16)) return fail("a file lies within one bank");
      input.regions.push_back(SourceRegion{.file = words[1], .first = *first, .last = *last});
      continue;
    }
    if (words[0] == "image") {
      if (words.size() != 2) return fail("image is a byte count");
      const std::optional<std::size_t> count = parseCount(words[1]);
      if (!count) return fail(words[1] + " is not a byte count");
      input.imageBytes = *count;
      continue;
    }
    if (words[0] == "checksum") {
      if (words.size() != 3) return fail("checksum is $XXXX $XXXX");
      const std::optional<std::uint16_t> value = parseShortAddress(words[1]);
      if (!value) return fail(words[1] + " is not a $XXXX value");
      input.checksum = *value;
      continue;
    }
    if (words[0] == "map") {
      if (words.size() != 2) return fail("map is LoROM, HiROM or ExHiROM");
      if (words[1] == "LoROM") input.map = CartridgeMap::LoRom;
      else if (words[1] == "HiROM") input.map = CartridgeMap::HiRom;
      else if (words[1] == "ExHiROM") input.map = CartridgeMap::ExHiRom;
      else return fail(words[1] + " is not a map");
      continue;
    }
    if (words[0] == "sound") {
      if (words.size() != 5 || words[2] != "SPC700" || words[3] != "entry") {
        return fail("sound is a path, SPC700, entry, and the entry address");
      }
      const std::optional<std::uint16_t> entry = parseShortAddress(words[4]);
      if (!entry) return fail(words[4] + " is not a $XXXX address");
      std::vector<ManifestBlock> kept = input.sound ? input.sound->blocks : std::vector<ManifestBlock>{};
      input.sound = ManifestSound{.file = words[1], .entry = *entry, .blocks = std::move(kept)};
      continue;
    }
    if (words[0] == "block") {
      const bool placed = words.size() == 6 && words[4] == "at";
      const bool unplaced = words.size() == 5 && words[4] == "unplaced";
      if (!placed && !unplaced) {
        return fail("a block is a path, its address, its length, and `at` an offset or `unplaced`");
      }
      const std::optional<std::uint16_t> address = parseShortAddress(words[2]);
      if (!address) return fail(words[2] + " is not a $XXXX address");
      const std::optional<std::size_t> length = parseCount(words[3]);
      if (!length) return fail(words[3] + " is not a length");
      ManifestBlock block{.apuAddress = *address, .length = *length, .romOffset = std::nullopt};
      if (placed) {
        block.romOffset = parseOffset(words[5]);
        if (!block.romOffset) return fail(words[5] + " is not a $XXXXXX offset");
      }
      if (!input.sound) input.sound = ManifestSound{.file = words[1], .entry = 0, .blocks = {}};
      if (input.sound->file != words[1]) {
        return fail("block " + words[1] + " names a file the sound line does not");
      }
      input.sound->blocks.push_back(block);
      continue;
    }
    static const std::set<std::string> kKnown = {"title", "stop",   "warning", "note",
                                                 "access", "dma",   "routine"};
    if (kKnown.find(words[0]) == kKnown.end()) return fail(words[0] + " is not a manifest line");
  }
  return input;
}

std::string manifestMismatch(const ManifestInput& input, std::span<const std::uint8_t> rom) {
  if (input.imageBytes && *input.imageBytes != rom.size()) {
    return "the manifest was written for an image of " + std::to_string(*input.imageBytes) +
           " bytes; this one is " + std::to_string(rom.size());
  }
  if (input.checksum) {
    const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
    if (!header || header->checksum != *input.checksum) {
      return "the manifest was written for an image with checksum $" + hex(*input.checksum, 4) +
             "; this one has " + (header ? "$" + hex(header->checksum, 4) : std::string("no header"));
    }
  }
  return {};
}

namespace {

// ---- a bank file, from the representation -------------------------------------

// Whether `name` can be a symbol of the 65816 dialect: the lexicon's name form,
// and not a mnemonic, a register or a directive.
bool symbolName(std::string_view name) {
  static const assembler::Cpu65816Dialect dialect;
  static const std::set<std::string> kCoreDirectives = {"ORG", "DB", "DW", "DL", "DS", "EQU"};
  if (name.empty()) return false;
  auto letter = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.';
  };
  if (!letter(name.front())) return false;
  for (const char c : name) {
    if (!letter(c) && !(c >= '0' && c <= '9')) return false;
  }
  const std::string upper = assembler::upper(name);
  return !dialect.reserved(upper) && kCoreDirectives.find(upper) == kCoreDirectives.end();
}

// The register the absolute operand at `site` addresses, from the access facts:
// the fact at the site whose register is the operand's own address. Empty where
// no fact names one.
std::string_view operandRegister(const CartridgeDisassembly& disassembly, Address site,
                                 std::uint32_t operand) {
  const std::vector<HardwareAccess>& accesses = disassembly.accesses;
  auto it = std::lower_bound(accesses.begin(), accesses.end(), site,
                             [](const HardwareAccess& a, Address wanted) { return a.site < wanted; });
  for (; it != accesses.end() && it->site == site; ++it) {
    if (it->registerAddress == operand) return it->name;
  }
  return {};
}

// The label an address carries in any of the tree's listings, or nothing.
std::optional<std::string> labelAnywhere(const CartridgeDisassembly& disassembly, Address address) {
  for (const RegionListing& region : disassembly.regions) {
    const auto found = region.listing.labels.find(address);
    if (found != region.listing.labels.end()) return found->second;
  }
  return std::nullopt;
}

bool absoluteData(const ir::Instruction& instruction) {
  const bool absolute = instruction.addressing == ir::Addressing::Absolute ||
                        instruction.addressing == ir::Addressing::AbsoluteX ||
                        instruction.addressing == ir::Addressing::AbsoluteY;
  return absolute && !instruction.target;
}

// A run of bytes execution never reached, as `DB` rows of eight with the bytes
// as text beside them — the framework's own form, so a bank file and a listing
// read alike.
std::string renderDataRun(const Line& line) {
  constexpr std::size_t kCommentColumn = 40;
  constexpr std::size_t kPerRow = 8;
  std::string out = "\n; ---- " + std::to_string(line.data.size()) +
                    " bytes execution did not reach\n";
  for (std::size_t i = 0; i < line.data.size(); i += kPerRow) {
    const std::size_t end = std::min(i + kPerRow, line.data.size());
    std::string row = "        DB ";
    std::string ascii;
    for (std::size_t j = i; j < end; ++j) {
      if (j != i) row += ",";
      row += "$" + hex(line.data[j], 2);
      const std::uint8_t byte = line.data[j];
      ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
    }
    if (row.size() < kCommentColumn) {
      row.append(kCommentColumn - row.size(), ' ');
    } else {
      row += "  ";
    }
    row += "; " + address24(line.address + static_cast<Address>(i)) + "  |" + ascii + "|";
    out += row + "\n";
  }
  return out;
}

std::string classList(const std::vector<RegisterClass>& classes) {
  if (classes.empty()) return "none";
  std::string text;
  for (const RegisterClass cls : classes) {
    if (!text.empty()) text += ", ";
    text += std::string(cpu65816RegisterClassName(cls));
  }
  return text;
}

std::string nameList(const std::vector<std::string>& names) {
  if (names.empty()) return "none";
  std::string text;
  for (const std::string& name : names) {
    if (!text.empty()) text += ", ";
    text += name;
  }
  return text;
}

std::string counted(std::size_t count, const char* noun) {
  return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

}  // namespace

std::string renderRegion(const RegionListing& region, const CartridgeDisassembly& disassembly) {
  const Listing lines = regionLines(region, disassembly);
  const std::string soundFile = disassembly.sound ? disassembly.sound->file : std::string();

  // The instructions as the representation holds them: one node per code line,
  // the first reading where an address was read two ways.
  const ir::Program program = ir::lift65816(lines);
  std::map<Address, const ir::Node*> nodes;
  for (const ir::Node& node : program.nodes) nodes.emplace(node.instruction.address, &node);
  auto nodeAt = [&](Address address) -> const ir::Node& {
    const auto found = nodes.find(address);
    if (found == nodes.end()) {
      throw std::logic_error("no node was lifted for the instruction at " + address24(address));
    }
    return *found->second;
  };

  // What the file names: the labels it defines, the registers its absolute
  // operands address, and the labels other files define that it refers to. A
  // register is named only where its name can be a symbol and the file defines
  // no label of that name.
  std::set<std::string> defined;
  for (const auto& [address, label] : lines.labels) defined.insert(label);
  std::map<std::uint32_t, std::string_view> registers;
  std::map<Address, std::string> foreign;
  for (const Line& line : lines.lines) {
    if (!line.isCode) continue;
    const ir::Instruction& instruction = nodeAt(line.address).instruction;
    // A target is looked up as the bytes name it: a jump through a mirror bank
    // finds no label there, and keeps its address, since a symbol would carry
    // the bank the bytes are placed in rather than the one they name.
    if (instruction.target) {
      if (lines.labels.find(*instruction.target) != lines.labels.end()) continue;
      const std::optional<std::string> label = labelAnywhere(disassembly, *instruction.target);
      if (label && defined.find(*label) == defined.end()) foreign[*instruction.target] = *label;
      continue;
    }
    if (!absoluteData(instruction)) continue;
    const std::string_view name = operandRegister(disassembly, line.address, instruction.operand);
    if (!name.empty() && symbolName(name) && defined.find(std::string(name)) == defined.end()) {
      registers[instruction.operand] = name;
    }
  }

  // The routines that begin in this file, and who calls each.
  std::map<Address, const Routine*> routines;
  std::map<Address, std::string> routineLabels;
  std::map<Address, std::vector<std::string>> callers;
  for (const Routine& routine : disassembly.routines) routineLabels[routine.address] = routine.label;
  for (const Routine& routine : disassembly.routines) {
    if (lines.labels.find(routine.address) != lines.labels.end()) {
      routines[routine.address] = &routine;
    }
    for (const Address callee : routine.calls) callers[callee].push_back(routine.label);
  }

  std::string out;
  for (const std::string& warning : lines.warnings) out += "; warning: " + warning + "\n";
  if (!lines.warnings.empty()) out += "\n";

  if (!registers.empty() || !foreign.empty()) {
    std::size_t width = 0;
    for (const auto& [address, name] : registers) width = std::max(width, name.size());
    for (const auto& [address, name] : foreign) width = std::max(width, name.size());
    auto equ = [&](std::string_view name, const std::string& value) {
      std::string row(name);
      row.append(width + 2 - name.size(), ' ');
      out += row + "EQU " + value + "\n";
    };
    out += "; The hardware registers this file names, and the labels other files\n"
           "; define that it refers to.\n";
    for (const auto& [address, name] : registers) equ(name, "$" + hex(address, 4));
    for (const auto& [address, name] : foreign) equ(name, "$" + hex(address, 6));
    out += "\n";
  }

  // The raw-bytes field is as wide as the longest instruction in the file, and
  // never narrower than three bytes, so the cycle costs stay aligned.
  std::size_t longest = 3;
  for (const Line& line : lines.lines) {
    if (line.isCode) longest = std::max<std::size_t>(longest, line.instruction.length);
  }
  const std::size_t bytesWidth = longest * 3;

  // One piece per run of consecutive lines, each under its own `ORG`; a gap is
  // the sound program's bytes. Every piece is a region to an assembler, and so is
  // whatever follows a run of data.
  std::optional<Address> previousEnd;
  bool open = false;
  ir::SourceMode mode;
  for (const Line& line : lines.lines) {
    if (previousEnd && line.address != *previousEnd) {
      out += "\n; ---- " + address24(*previousEnd) + "-" + address24(line.address - 1u) +
             ": the sound program, see " + soundFile + "\n";
      open = false;
    }
    if (!open) {
      out += "        ORG " + address24(line.address) + "\n";
      open = true;
      mode.reset();
    }
    previousEnd = lineEnd(line);

    if (!line.isCode) {
      if (!line.data.empty()) out += renderDataRun(line);
      mode.reset();
      continue;
    }

    const ir::Node& node = nodeAt(line.address);
    // A routine's header sits directly above its label: its size, its role, and
    // the call graph either way.
    bool headed = false;
    if (const auto routine = routines.find(line.address); routine != routines.end()) {
      const Routine& r = *routine->second;
      std::vector<std::string> calls;
      for (const Address callee : r.calls) calls.push_back(routineLabels.at(callee));
      const auto called = callers.find(r.address);
      out += "\n; routine " + r.label + ": " + counted(r.lines.size(), "line") + ", " +
             counted(r.bytes, "byte") + "\n";
      out += ";   reaches " + classList(r.reaches) + "; through " + classList(r.through) + "\n";
      out += ";   calls " + nameList(calls) + "; called by " +
             nameList(called == callers.end() ? std::vector<std::string>{} : called->second) +
             "\n";
      headed = true;
    }
    if (const auto label = lines.labels.find(line.address); label != lines.labels.end()) {
      out += (headed ? "" : "\n") + label->second + ":\n";
    }
    for (const std::string& directive : mode.directives(node)) {
      out += "        " + directive + "\n";
    }

    ir::SourceNames names;
    const ir::Instruction& instruction = node.instruction;
    if (instruction.target) {
      if (const auto own = lines.labels.find(*instruction.target); own != lines.labels.end()) {
        names.target = own->second;
      } else if (const auto other = foreign.find(*instruction.target); other != foreign.end()) {
        names.target = other->second;
      }
    } else if (absoluteData(instruction)) {
      const std::string_view name = operandRegister(disassembly, line.address, instruction.operand);
      if (registers.find(instruction.operand) != registers.end()) {
        names.operand = name;
      } else {
        names.annotation = name;
      }
    }
    out += ir::renderLine(node, names, bytesWidth);
  }
  return out;
}

std::string renderSoundProgram(const SoundProgram& sound) {
  std::string out;
  out += "; The sound program the cartridge uploads at boot, traced from " +
         address16(sound.capture.entry) + ".\n";
  for (const UploadBlock& block : sound.capture.blocks) {
    out += "; " + address16(block.apuAddress) + ": " + std::to_string(block.bytes.size()) + " bytes";
    out += block.romOffset ? ", read from image offset $" +
                                 hex(static_cast<std::uint32_t>(*block.romOffset), 6)
                           : std::string(", not read from the image as they are");
    out += "\n";
  }
  out += "\n";
  out += renderPieces(sound.listing, spc700Backend(), [&](Address first, Address last) {
    return "; ---- " + address16(first) + "-" + address16(last) + ": not uploaded";
  });
  return out;
}

bool writeProject(const CartridgeDisassembly& disassembly, const std::filesystem::path& directory,
                  std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    error = "cannot create " + directory.string() + ": " + ec.message();
    return false;
  }
  auto write = [&](const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      error = "cannot write " + path.string();
      return false;
    }
    out << text;
    return static_cast<bool>(out);
  };
  if (!write(directory / "project.manifest", renderManifest(disassembly))) return false;
  for (const RegionListing& region : disassembly.regions) {
    if (!write(directory / region.region.file, renderRegion(region, disassembly))) return false;
  }
  if (disassembly.sound) {
    if (!write(directory / disassembly.sound->file, renderSoundProgram(*disassembly.sound))) {
      return false;
    }
  }
  return true;
}

}  // namespace snaggletooth::disasm
