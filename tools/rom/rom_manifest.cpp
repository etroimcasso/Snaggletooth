// The manifest read back — the grammar is `docs/project-manifest.md`. Its own
// translation unit, linked by the renderer as well as the disassembler, so a
// tool that reads a tree links nothing that can run one.

#include <algorithm>
#include <map>
#include <set>

#include "rom/rom_disasm.h"
#include "rom/rom_text.h"

namespace snaggletooth::disasm {

using text::hex;
using text::parseCount;
using text::parseLongAddress;
using text::parseMode;
using text::parseOffset;
using text::parseRegisterClass;
using text::parseShortAddress;
using text::tokens;

namespace {

// A kind as the manifest writes it. The words are the manifest's own
// (`docs/project-manifest.md`); the observer writes them from the same list.
std::optional<MovedKind> parseMovedKind(const std::string& word) {
  if (word == "dma") return MovedKind::Dma;
  if (word == "table") return MovedKind::Table;
  if (word == "indirect") return MovedKind::Indirect;
  if (word == "stream") return MovedKind::Stream;
  if (word == "staged") return MovedKind::Staged;
  if (word == "proven") return MovedKind::Proven;
  return std::nullopt;
}

// A list of register classes as a `routine` line writes one: names joined by
// commas, or `none`.
std::optional<std::vector<RegisterClass>> parseClassList(const std::string& word) {
  std::vector<RegisterClass> out;
  if (word == "none") return out;
  for (std::size_t from = 0; from <= word.size();) {
    const std::size_t comma = word.find(',', from);
    const std::string name = word.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
    const std::optional<RegisterClass> cls = parseRegisterClass(name);
    if (!cls) return std::nullopt;
    out.push_back(*cls);
    if (comma == std::string::npos) break;
    from = comma + 1u;
  }
  return out;
}

// The values of a `seen` field, `D=$a|$b`: each `$` and its digits, the field
// name and `=` ahead of them.
std::optional<std::vector<std::uint32_t>> parseSeenValues(const std::string& field, const char* name,
                                                          std::size_t digits) {
  const std::string lead = std::string(name) + "=";
  if (field.rfind(lead, 0) != 0) return std::nullopt;
  std::vector<std::uint32_t> out;
  for (std::size_t from = lead.size(); from <= field.size();) {
    const std::size_t bar = field.find('|', from);
    const std::string word = field.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
    if (word.size() != digits + 1 || word[0] != '$') return std::nullopt;
    const std::optional<std::uint32_t> value = text::parseHex(word.substr(1));
    if (!value) return std::nullopt;
    out.push_back(*value);
    if (bar == std::string::npos) break;
    from = bar + 1u;
  }
  return out;
}

}  // namespace

std::optional<ManifestInput> parseManifest(std::string_view text, std::string& error) {
  ManifestInput input;
  std::size_t number = 0;
  std::size_t position = 0;
  // A routine's calls are labels; they resolve to addresses once every routine
  // line is read.
  std::vector<std::vector<std::string>> callLabels;
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
    if (words[0] == "ran") {
      if (words.size() != 8 || words[6] != "from") {
        return fail("a landing is an address, a name, e=, m=, x=, `from` and the site");
      }
      const std::optional<Address> target = parseLongAddress(words[1]);
      const std::optional<Address> site = parseLongAddress(words[7]);
      if (!target || !site) return fail("addresses are written $BB:XXXX");
      const std::optional<Cpu65816Mode> mode = parseMode(words, 3);
      if (!mode) return fail("the mode is e=0|1 m=8|16|? x=8|16|?");
      input.ran.push_back(Landing{.target = *target, .mode = *mode, .site = *site, .name = words[2]});
      continue;
    }
    if (words[0] == "moved") {
      // moved <site> channel <n> <direction> <register> <name> <class> memory
      // <address> <step> bytes <n> as <kind> times <n>
      if (words.size() != 17 || words[2] != "channel" || words[8] != "memory" ||
          words[11] != "bytes" || words[13] != "as" || words[15] != "times") {
        return fail("a moved range is a site, `channel` n, a direction, a register with its name and "
                    "class, `memory` an address, a step, `bytes` n, `as` a kind and `times` n");
      }
      const std::optional<Address> site = parseLongAddress(words[1]);
      const std::optional<Address> registerAddress = parseLongAddress(words[5]);
      const std::optional<Address> memory = parseLongAddress(words[9]);
      if (!site || !registerAddress || !memory) return fail("addresses are written $BB:XXXX");
      const std::optional<std::size_t> channel = parseCount(words[3]);
      const std::optional<std::size_t> bytes = parseCount(words[12]);
      const std::optional<std::size_t> times = parseCount(words[16]);
      if (!channel || *channel > 7u) return fail(words[3] + " is not a channel 0-7");
      if (!bytes || !times) return fail("bytes and times are counts");
      MovedRange range{.site = *site,
                       .channel = static_cast<std::uint8_t>(*channel),
                       .toRegister = true,
                       .registerAddress = *registerAddress,
                       .registerName = {},
                       .registerClass = std::nullopt,
                       .memory = *memory,
                       .step = MovedStep::Increment,
                       .bytes = static_cast<std::uint32_t>(*bytes),
                       .kind = MovedKind::Dma,
                       .times = static_cast<std::uint32_t>(*times)};
      if (words[4] == "from-register") range.toRegister = false;
      else if (words[4] != "to-register") return fail(words[4] + " is not to-register or from-register");
      if (words[10] == "decrement") range.step = MovedStep::Decrement;
      else if (words[10] == "fixed") range.step = MovedStep::Fixed;
      else if (words[10] != "increment") return fail(words[10] + " is not increment, decrement or fixed");
      if (words[14] == "table") range.kind = MovedKind::Table;
      else if (words[14] == "indirect") range.kind = MovedKind::Indirect;
      else if (words[14] != "dma") return fail(words[14] + " is not dma, table or indirect");
      // The name and the class are the register table's, from the address; the
      // words on the line are what the last run wrote from the same table.
      if (const std::optional<Cpu65816Register> reg = cpu65816Register(*registerAddress)) {
        range.registerName = reg->name;
        range.registerClass = reg->cls;
      }
      input.moved.push_back(range);
      continue;
    }
    if (words[0] == "asset") {
      // asset <path> <class> as <kind> from <address> bytes <n>
      if (words.size() != 9 || words[3] != "as" || words[5] != "from" || words[7] != "bytes") {
        return fail("an asset is a path, a class, `as` a kind, `from` an address and `bytes` n");
      }
      // The class, or several joined by `+` for a file built into data for
      // two.
      std::vector<RegisterClass> classes;
      for (std::size_t from = 0; from <= words[2].size();) {
        const std::size_t joint = words[2].find('+', from);
        const std::string word = words[2].substr(from, joint == std::string::npos ? std::string::npos : joint - from);
        const std::optional<RegisterClass> cls = parseRegisterClass(word);
        if (!cls) return fail(word + " is not a register class");
        if (std::find(classes.begin(), classes.end(), *cls) == classes.end()) classes.push_back(*cls);
        if (joint == std::string::npos) break;
        from = joint + 1u;
      }
      std::sort(classes.begin(), classes.end());
      const std::optional<MovedKind> kind = parseMovedKind(words[4]);
      if (!kind) return fail(words[4] + " is not dma, table, indirect, stream, staged or proven");
      const std::optional<Address> first = parseLongAddress(words[6]);
      if (!first) return fail(words[6] + " is not a $BB:XXXX address");
      const std::optional<std::size_t> bytes = parseCount(words[8]);
      if (!bytes || *bytes == 0) return fail(words[8] + " is not a byte count");
      input.assets.push_back(
          ManifestAsset{.file = words[1], .first = *first, .bytes = *bytes, .classes = classes, .kind = *kind});
      continue;
    }
    if (words[0] == "derived") {
      if (words.size() != 10 || words[6] != "from" || words[8] != "via") {
        return fail("a derived target is an address, a name, e=, m=, x=, `from` the site and `via` the pointer");
      }
      const std::optional<Address> target = parseLongAddress(words[1]);
      const std::optional<Address> site = parseLongAddress(words[7]);
      const std::optional<Address> pointer = parseLongAddress(words[9]);
      if (!target || !site || !pointer) return fail("addresses are written $BB:XXXX");
      const std::optional<Cpu65816Mode> mode = parseMode(words, 3);
      if (!mode) return fail("the mode is e=0|1 m=8|16|? x=8|16|?");
      input.derived.push_back(DerivedTarget{.target = *target,
                                            .mode = *mode,
                                            .site = *site,
                                            .pointer = *pointer,
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
    if (words[0] == "access") {
      // access <site> <name> <class> <kind> <value|none>
      if (words.size() != 6) return fail("an access is a site, a register's name, its class, a kind and a value or `none`");
      const std::optional<Address> site = parseLongAddress(words[1]);
      if (!site) return fail(words[1] + " is not a $BB:XXXX address");
      if (!parseRegisterClass(words[3])) return fail(words[3] + " is not a register class");
      if (words[4] != "read" && words[4] != "write" && words[4] != "read-write") {
        return fail(words[4] + " is not read, write or read-write");
      }
      if (words[5] != "none" && (words[5].size() != 3 || words[5][0] != '$' || !text::parseHex(words[5].substr(1)))) {
        return fail(words[5] + " is not a $XX value or none");
      }
      // The name is the register table's own storage, found through the
      // address the renderer needs it at.
      Address registerAddress = 0;
      std::string_view name;
      for (Address offset = 0x2100u; offset <= 0x43FFu && name.empty(); ++offset) {
        if (offset == 0x2200u) offset = 0x4000u;
        const std::optional<Cpu65816Register> reg = cpu65816Register(offset);
        if (reg && reg->name == words[2]) {
          registerAddress = offset;
          name = reg->name;
        }
      }
      if (name.empty()) return fail(words[2] + " is not a register the table names");
      input.accesses.push_back(RenderAccess{.site = *site, .registerAddress = registerAddress, .name = name});
      continue;
    }
    if (words[0] == "dma") {
      // dma <site> channel <n> <direction> <dest> <name> <class> source <src> <step> bytes <n>
      //     start|start-hdma <mask> from <site>
      if (words.size() != 17 || words[2] != "channel" || words[8] != "source" || words[11] != "bytes" ||
          words[15] != "from") {
        return fail("a dma line is a site, `channel` n, a direction, a destination with its name and class, "
                    "`source` an address, a step, `bytes` n, a start, a mask and `from` a site");
      }
      const std::optional<Address> site = parseLongAddress(words[1]);
      if (!site) return fail(words[1] + " is not a $BB:XXXX address");
      ManifestDma dma{.site = *site,
                      .destination = std::nullopt,
                      .source = std::nullopt,
                      .step = MovedStep::Increment,
                      .bytes = std::nullopt};
      if (words[5] != "none") {
        dma.destination = parseLongAddress(words[5]);
        if (!dma.destination) return fail(words[5] + " is not a $BB:XXXX address or none");
      }
      if (words[9] != "none") {
        dma.source = parseLongAddress(words[9]);
        if (!dma.source) return fail(words[9] + " is not a $BB:XXXX address or none");
      }
      if (words[10] == "decrement") dma.step = MovedStep::Decrement;
      else if (words[10] == "fixed") dma.step = MovedStep::Fixed;
      else if (words[10] != "increment" && words[10] != "none") {
        return fail(words[10] + " is not increment, decrement, fixed or none");
      }
      if (words[12] != "none") {
        dma.bytes = parseCount(words[12]);
        if (!dma.bytes) return fail(words[12] + " is not a byte count or none");
      }
      input.dmas.push_back(dma);
      continue;
    }
    if (words[0] == "routine") {
      // routine <address> <label> lines <n> bytes <n> calls <list> reaches <list> through <list>
      if (words.size() != 13 || words[3] != "lines" || words[5] != "bytes" || words[7] != "calls" ||
          words[9] != "reaches" || words[11] != "through") {
        return fail("a routine is an address, a label, `lines` n, `bytes` n, `calls`, `reaches` and `through` lists");
      }
      const std::optional<Address> address = parseLongAddress(words[1]);
      if (!address) return fail(words[1] + " is not a $BB:XXXX address");
      const std::optional<std::size_t> lines = parseCount(words[4]);
      const std::optional<std::size_t> bytes = parseCount(words[6]);
      if (!lines || !bytes) return fail("lines and bytes are counts");
      const std::optional<std::vector<RegisterClass>> reaches = parseClassList(words[10]);
      const std::optional<std::vector<RegisterClass>> through = parseClassList(words[12]);
      if (!reaches || !through) return fail("reaches and through are register classes joined by commas, or none");
      std::vector<std::string> calls;
      if (words[8] != "none") {
        for (std::size_t from = 0; from <= words[8].size();) {
          const std::size_t comma = words[8].find(',', from);
          calls.push_back(words[8].substr(from, comma == std::string::npos ? std::string::npos : comma - from));
          if (comma == std::string::npos) break;
          from = comma + 1u;
        }
      }
      input.routines.push_back(RenderRoutine{.address = *address,
                                             .label = words[2],
                                             .lines = *lines,
                                             .bytes = *bytes,
                                             .calls = {},
                                             .reaches = *reaches,
                                             .through = *through});
      callLabels.push_back(std::move(calls));
      continue;
    }
    if (words[0] == "seen") {
      // seen <address> D=$a|$b DBR=$c
      if (words.size() != 4) return fail("a seen line is an address, D= and DBR=");
      const std::optional<Address> address = parseLongAddress(words[1]);
      if (!address) return fail(words[1] + " is not a $BB:XXXX address");
      const std::optional<std::vector<std::uint32_t>> d = parseSeenValues(words[2], "D", 4);
      const std::optional<std::vector<std::uint32_t>> dbr = parseSeenValues(words[3], "DBR", 2);
      if (!d || !dbr) return fail("the values are D=$XXXX and DBR=$XX, several joined by |");
      RenderSeen seen{.address = *address, .d = {}};
      for (const std::uint32_t value : *d) seen.d.push_back(static_cast<std::uint16_t>(value));
      input.seen.push_back(std::move(seen));
      continue;
    }
    static const std::set<std::string> kKnown = {"title", "stop", "warning", "note", "state",
                                                 "origin", "staged", "streamed", "landed"};
    if (kKnown.find(words[0]) == kKnown.end()) return fail(words[0] + " is not a manifest line");
  }
  std::map<std::string, Address> routineAddresses;
  for (const RenderRoutine& routine : input.routines) routineAddresses[routine.label] = routine.address;
  for (std::size_t i = 0; i < input.routines.size(); ++i) {
    for (const std::string& label : callLabels[i]) {
      const auto found = routineAddresses.find(label);
      if (found == routineAddresses.end()) {
        error = "routine " + input.routines[i].label + " calls " + label + ", which no routine line names";
        return std::nullopt;
      }
      input.routines[i].calls.push_back(found->second);
    }
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

}  // namespace snaggletooth::disasm
