// A run's record: the text it is written as and read back from, the join of
// two runs' observations, and the records under a tree's `runs/`. The grammar
// is `docs/run-record.md`.

#include "rom/rom_record.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

#include "rom/rom_text.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {

using text::address24;
using text::hex;
using text::parseLongAddress;
using text::parseRegisterClass;
using text::tokens;

namespace {

constexpr std::string_view kFirstLine = "snagrun 1";
constexpr std::string_view kExtension = ".snagrun";

// ---------------------------------------------------------------------------
// The text of a record.

std::string widthText(bool known, bool eight) {
  if (!known) return "?";
  return eight ? "8" : "16";
}

// A mode as its three fields: `e=`, `m=` and `x=`, each width as the run held it.
std::string modeText(const Cpu65816Mode& mode) {
  return std::string("e=") + (mode.emulation ? "1" : "0") + " m=" +
         widthText(mode.accumulatorKnown, mode.accumulator8) + " x=" + widthText(mode.indexKnown, mode.index8);
}

std::string offsetText(std::size_t offset) { return "$" + hex(static_cast<std::uint32_t>(offset), 6); }

std::string intervalText(const ir::OriginInterval& interval) {
  return offsetText(interval.first) + "-" + offsetText(interval.last);
}

std::string intervalsText(const std::vector<ir::OriginInterval>& intervals) {
  if (intervals.empty()) return "none";
  std::string out;
  for (const ir::OriginInterval& interval : intervals) {
    if (!out.empty()) out += ",";
    out += intervalText(interval);
  }
  return out;
}

std::string originText(const ir::OriginSet& origin) {
  std::string out;
  const auto add = [&](const std::string& word) {
    if (!out.empty()) out += " ";
    out += word;
  };
  if (!origin.image.empty()) add("image " + intervalsText(origin.image));
  if (!origin.registers.empty()) {
    std::string registers;
    for (const std::uint32_t reg : origin.registers) {
      if (!registers.empty()) registers += ",";
      registers += "$" + hex(reg, 4);
    }
    add("registers " + registers);
  }
  if (origin.save) add("save");
  if (origin.approximate) add("approximate");
  return out.empty() ? "none" : out;
}

std::string bytesText(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) return "none";
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2u);
  for (const std::uint8_t byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0Fu]);
  }
  return out;
}

std::string_view memoryName(PortMemory memory) {
  switch (memory) {
    case PortMemory::Vram: return "vram";
    case PortMemory::Cgram: return "cgram";
    case PortMemory::Oam: return "oam";
  }
  return "vram";
}

std::string depthsText(std::uint8_t depths) {
  std::string out;
  for (const auto& [bit, name] : {std::pair{kDepth2, "2"}, std::pair{kDepth4, "4"}, std::pair{kDepth8, "8"}}) {
    if ((depths & bit) == 0u) continue;
    if (!out.empty()) out += "+";
    out += name;
  }
  return out.empty() ? "none" : out;
}

std::string landingText(const PortLanding& landing) {
  PortLanding named = landing;
  named.shown = true;  // `areaText` names the areas of a shown landing
  return std::string(memoryName(landing.memory)) + " " + portAddressText(landing.memory, landing.lowest) + "-" +
         portAddressText(landing.memory, landing.highest) + (landing.shown ? " shown" : " unshown") + " areas " +
         areaText(named) + " depths " + depthsText(landing.depths) + " palette " +
         (landing.palette ? std::to_string(*landing.palette) : std::string("none"));
}

std::string classText(RegisterClass cls) { return std::string(cpu65816RegisterClassName(cls)); }

std::string classesListText(const std::vector<RegisterClass>& classes) {
  return classes.empty() ? std::string("none") : text::classesText(classes, "+");
}

std::string valuesText(unsigned bits, const std::vector<std::uint32_t>& values) {
  return values.empty() ? std::string("none") : inputValuesText(bits, values);
}

std::string checksumText(std::span<const std::uint8_t> rom) {
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  return header ? "$" + hex(header->checksum, 4) : std::string("none");
}

// ---------------------------------------------------------------------------
// Reading a line, a field at a time. A field that does not read marks the line
// failed and answers a default, so a kind's reader takes its fields in order
// and asks once at the end whether every one read and nothing was left over.

std::optional<std::uint64_t> parseDecimal(std::string_view word) {
  if (word.empty() || word.size() > 20u) return std::nullopt;
  std::uint64_t value = 0;
  for (const char c : word) {
    if (c < '0' || c > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return std::nullopt;
    value = value * 10u + digit;
  }
  return value;
}

// `$` and one to eight hexadecimal digits.
std::optional<std::uint32_t> parseDollar(std::string_view word) {
  if (word.size() < 2u || word[0] != '$') return std::nullopt;
  return text::parseHex(word.substr(1));
}

// `$` and six to eight hexadecimal digits: an image offset.
std::optional<std::size_t> parseImageOffset(std::string_view word) {
  if (word.size() < 7u) return std::nullopt;
  const std::optional<std::uint32_t> value = parseDollar(word);
  if (!value) return std::nullopt;
  return static_cast<std::size_t>(*value);
}

// The pieces of `word` between `separator`s.
std::vector<std::string_view> split(std::string_view word, char separator) {
  std::vector<std::string_view> out;
  std::size_t from = 0;
  while (true) {
    const std::size_t at = word.find(separator, from);
    out.push_back(word.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
    if (at == std::string_view::npos) break;
    from = at + 1u;
  }
  return out;
}

std::optional<ir::OriginInterval> parseInterval(std::string_view word) {
  const std::vector<std::string_view> ends = split(word, '-');
  if (ends.size() != 2u) return std::nullopt;
  const std::optional<std::size_t> first = parseImageOffset(ends[0]);
  const std::optional<std::size_t> last = parseImageOffset(ends[1]);
  if (!first || !last || *last < *first) return std::nullopt;
  return ir::OriginInterval{.first = *first, .last = *last};
}

std::optional<std::vector<std::uint8_t>> parseBytes(std::string_view word) {
  std::vector<std::uint8_t> out;
  if (word == "none") return out;
  if (word.size() % 2u != 0u) return std::nullopt;
  out.reserve(word.size() / 2u);
  for (std::size_t i = 0; i < word.size(); i += 2u) {
    const std::optional<std::uint32_t> byte = text::parseHex(word.substr(i, 2));
    if (!byte) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>(*byte));
  }
  return out;
}

std::optional<MovedKind> parseKind(std::string_view word) {
  for (const MovedKind kind : {MovedKind::Dma, MovedKind::Table, MovedKind::Indirect, MovedKind::Stream,
                               MovedKind::Staged, MovedKind::Proven, MovedKind::Enumerated}) {
    if (movedKindName(kind) == word) return kind;
  }
  return std::nullopt;
}

std::optional<MovedStep> parseStep(std::string_view word) {
  for (const MovedStep step : {MovedStep::Increment, MovedStep::Decrement, MovedStep::Fixed}) {
    if (movedStepName(step) == word) return step;
  }
  return std::nullopt;
}

std::optional<InputRegister> parseInputRegister(std::string_view word) {
  for (const InputRegister reg : {InputRegister::A, InputRegister::X, InputRegister::Y}) {
    if (inputRegisterName(reg) == word) return reg;
  }
  return std::nullopt;
}

// The values `inputValuesText` writes: `$lo`, `$lo-$hi` and `$lo-$hi/d`, joined by `,`.
std::optional<std::vector<std::uint32_t>> parseValues(std::string_view word) {
  std::vector<std::uint32_t> out;
  if (word == "none") return out;
  for (const std::string_view piece : split(word, ',')) {
    const std::size_t slash = piece.find('/');
    const std::string_view span = piece.substr(0, slash);
    std::uint32_t step = 1;
    if (slash != std::string_view::npos) {
      const std::optional<std::uint64_t> d = parseDecimal(piece.substr(slash + 1u));
      if (!d || *d == 0u || *d > 0xFFFFu) return std::nullopt;
      step = static_cast<std::uint32_t>(*d);
    }
    const std::vector<std::string_view> ends = split(span, '-');
    if (ends.size() > 2u || (ends.size() == 1u && slash != std::string_view::npos)) return std::nullopt;
    const std::optional<std::uint32_t> low = parseDollar(ends.front());
    const std::optional<std::uint32_t> high = parseDollar(ends.back());
    if (!low || !high || *high < *low || (*high - *low) % step != 0u || *high > 0xFFFFu) return std::nullopt;
    for (std::uint32_t value = *low;; value += step) {
      out.push_back(value);
      if (value == *high) break;
    }
  }
  return out;
}

class Fields {
 public:
  Fields(const std::vector<std::string>& words, std::size_t palettes) : words_(words), palettes_(palettes) {}

  [[nodiscard]] bool complete() const { return !failed_ && at_ == words_.size(); }
  void fail() { failed_ = true; }
  // A reason more particular than the kind's shape, when a field gave one.
  [[nodiscard]] const std::string& problem() const { return problem_; }

  [[nodiscard]] bool peek(std::string_view word) const { return !failed_ && at_ < words_.size() && words_[at_] == word; }

  const std::string& take() {
    static const std::string kNothing;
    if (failed_ || at_ >= words_.size()) {
      failed_ = true;
      return kNothing;
    }
    return words_[at_++];
  }

  void expect(std::string_view word) {
    if (take() != word) failed_ = true;
  }

  // One of two words: true for the first.
  bool either(std::string_view yes, std::string_view no) {
    const std::string& word = take();
    if (word == yes) return true;
    if (word != no) failed_ = true;
    return false;
  }

  Address address() { return check(parseLongAddress(take())); }

  std::uint64_t count() { return check(parseDecimal(take())); }

  std::uint32_t count32() {
    const std::uint64_t value = count();
    if (value > std::numeric_limits<std::uint32_t>::max()) failed_ = true;
    return static_cast<std::uint32_t>(value);
  }

  std::size_t offset() { return check(parseImageOffset(take())); }

  Cpu65816Mode mode() {
    const std::string& e = take();
    if (e != "e=0" && e != "e=1") failed_ = true;
    Cpu65816Mode out{};
    out.emulation = e == "e=1";
    if (!text::parseWidth(take(), 'm', out.accumulatorKnown, out.accumulator8)) failed_ = true;
    if (!text::parseWidth(take(), 'x', out.indexKnown, out.index8)) failed_ = true;
    out.carryKnown = false;
    out.carry = false;
    return out;
  }

  RegisterClass registerClass() { return check(parseRegisterClass(take())); }
  MovedKind kind() { return check(parseKind(take())); }
  MovedStep step() { return check(parseStep(take())); }
  std::vector<std::uint8_t> bytes() { return check(parseBytes(take())); }

  std::vector<RegisterClass> classes() {
    std::vector<RegisterClass> out;
    const std::string& word = take();
    if (word == "none") return out;
    for (const std::string_view name : split(word, '+')) {
      out.push_back(check(parseRegisterClass(std::string(name))));
    }
    return out;
  }

  std::vector<ir::OriginInterval> intervals() {
    std::vector<ir::OriginInterval> out;
    const std::string& word = take();
    if (word == "none") return out;
    for (const std::string_view piece : split(word, ',')) out.push_back(check(parseInterval(piece)));
    return out;
  }

  // An origin set, which runs to the end of the line.
  ir::OriginSet origin() {
    ir::OriginSet set;
    if (peek("none")) {
      take();
      return set;
    }
    const std::size_t from = at_;
    if (peek("image")) {
      take();
      set.image = intervals();
      if (set.image.empty()) failed_ = true;
    }
    if (peek("registers")) {
      take();
      for (const std::string_view piece : split(take(), ',')) set.registers.push_back(check(parseDollar(piece)));
    }
    if (peek("save")) {
      take();
      set.save = true;
    }
    if (peek("approximate")) {
      take();
      set.approximate = true;
    }
    if (at_ == from) failed_ = true;
    return set;
  }

  PortLanding landing() {
    PortLanding out;
    const std::string& memory = take();
    if (memory == "vram") out.memory = PortMemory::Vram;
    else if (memory == "cgram") out.memory = PortMemory::Cgram;
    else if (memory == "oam") out.memory = PortMemory::Oam;
    else failed_ = true;
    const std::vector<std::string_view> ends = split(take(), '-');
    if (ends.size() != 2u) {
      failed_ = true;
    } else {
      out.lowest = portAddress(ends[0]);
      out.highest = portAddress(ends[1]);
    }
    out.shown = either("shown", "unshown");
    expect("areas");
    out.areas = areas(take());
    expect("depths");
    out.depths = depths(take());
    expect("palette");
    const std::string& palette = take();
    if (palette != "none") {
      const std::uint64_t index = check(parseDecimal(palette));
      if (!failed_ && index >= palettes_) {
        failed_ = true;
        problem_ = "palette " + palette + " is not among the " + std::to_string(palettes_) +
                   " the record's `palette` lines hold before it";
      }
      out.palette = static_cast<std::size_t>(index);
    }
    return out;
  }

  std::optional<PortLanding> optionalLanding() {
    if (peek("none")) {
      take();
      return std::nullopt;
    }
    return landing();
  }

 private:
  template <typename T>
  T check(std::optional<T> value) {
    if (!value) {
      failed_ = true;
      return T{};
    }
    return *value;
  }

  std::uint16_t portAddress(std::string_view word) {
    const std::optional<std::uint32_t> value = parseDollar(word);
    if (!value || *value > 0xFFFFu) {
      failed_ = true;
      return 0;
    }
    return static_cast<std::uint16_t>(*value);
  }

  std::uint16_t areas(std::string_view word) {
    if (word == "none") return 0;
    static constexpr std::pair<std::uint16_t, std::string_view> kNames[] = {
        {kAreaTilemap1, "tilemap1"}, {kAreaTilemap2, "tilemap2"}, {kAreaTilemap3, "tilemap3"},
        {kAreaTilemap4, "tilemap4"}, {kAreaTiles1, "tiles1"},     {kAreaTiles2, "tiles2"},
        {kAreaTiles3, "tiles3"},     {kAreaTiles4, "tiles4"},     {kAreaSprites, "sprites"},
        {kAreaMode7, "mode7"},       {kAreaPalette, "palette"},   {kAreaOam, "oam"}};
    std::uint16_t out = 0;
    for (const std::string_view name : split(word, '+')) {
      const auto found =
          std::find_if(std::begin(kNames), std::end(kNames), [&](const auto& entry) { return entry.second == name; });
      if (found == std::end(kNames)) {
        failed_ = true;
        return 0;
      }
      out = static_cast<std::uint16_t>(out | found->first);
    }
    return out;
  }

  std::uint8_t depths(std::string_view word) {
    if (word == "none") return 0;
    std::uint8_t out = 0;
    for (const std::string_view name : split(word, '+')) {
      if (name == "2") out = static_cast<std::uint8_t>(out | kDepth2);
      else if (name == "4") out = static_cast<std::uint8_t>(out | kDepth4);
      else if (name == "8") out = static_cast<std::uint8_t>(out | kDepth8);
      else failed_ = true;
    }
    return out;
  }

  const std::vector<std::string>& words_;
  std::size_t palettes_;
  std::size_t at_ = 1;
  bool failed_ = false;
  std::string problem_;
};

// The register a range or a stream names, with its name and class as its address gives them.
template <typename Range>
void nameRegister(Range& range) {
  if (const std::optional<Cpu65816Register> reg = cpu65816Register(range.registerAddress)) {
    range.registerName = reg->name;
    range.registerClass = reg->cls;
  } else {
    range.registerName = {};
    range.registerClass = std::nullopt;
  }
}

// The image's size and checksum, as the record's second line names them:
// nothing when the line does not read.
struct RecordImage {
  std::size_t bytes = 0;
  std::optional<std::uint16_t> checksum;
};

std::optional<RecordImage> parseImageLine(std::string_view line) {
  const std::vector<std::string> words = tokens(line);
  if (words.size() != 4u || words[0] != "image" || words[2] != "checksum") return std::nullopt;
  const std::optional<std::uint64_t> bytes = parseDecimal(words[1]);
  if (!bytes) return std::nullopt;
  RecordImage image{.bytes = static_cast<std::size_t>(*bytes), .checksum = std::nullopt};
  if (words[3] == "none") return image;
  const std::optional<std::uint32_t> checksum = parseDollar(words[3]);
  if (!checksum || words[3].size() != 5u) return std::nullopt;
  image.checksum = static_cast<std::uint16_t>(*checksum);
  return image;
}

// The record's lines, each without its line ending.
std::vector<std::string_view> linesOf(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t from = 0;
  while (from < text.size()) {
    std::size_t end = text.find('\n', from);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(from, end - from);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    out.push_back(line);
    from = end + 1u;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Joining.

using SightingKey = std::tuple<Address, Address, std::uint32_t>;

template <typename Sighting>
SightingKey sightingKey(const Sighting& s) {
  return {s.site, s.target, contextOf(s.mode).bits};
}

template <typename Sighting>
bool sightingBefore(const Sighting& a, const Sighting& b) {
  return sightingKey(a) < sightingKey(b);
}

using MovedKey = std::tuple<Address, std::uint8_t, bool, Address, Address, std::uint8_t, std::uint32_t, std::uint8_t>;
MovedKey keyOf(const MovedRange& r) {
  return {r.site,   r.channel, r.toRegister, r.registerAddress, r.memory, static_cast<std::uint8_t>(r.step),
          r.bytes, static_cast<std::uint8_t>(r.kind)};
}

// A landing's identity: every field but its palette, which is the first sighting's.
using PortKey = std::tuple<bool, std::uint8_t, std::uint16_t, std::uint16_t, bool, std::uint16_t, std::uint8_t>;
PortKey keyOf(const std::optional<PortLanding>& landing) {
  if (!landing) return PortKey{};
  return {true,           static_cast<std::uint8_t>(landing->memory), landing->lowest, landing->highest,
          landing->shown, landing->areas,                             landing->depths};
}

using LandedKey = std::tuple<Address, std::uint8_t, Address, std::uint32_t, std::uint8_t, PortKey>;
LandedKey keyOf(const LandedRange& l) {
  return {l.site, l.channel, l.memory, l.bytes, static_cast<std::uint8_t>(l.kind), keyOf(std::optional{l.landing})};
}

using WalkedKey = std::tuple<Address, std::uint8_t, Address, std::uint32_t, std::uint8_t, unsigned, bool>;
WalkedKey keyOf(const WalkedRange& w) {
  return {w.site, w.channel, w.memory, w.bytes, static_cast<std::uint8_t>(w.kind), w.unit, w.indirect};
}

using StreamKey = std::tuple<Address, Address, std::size_t, std::uint32_t, bool, Address, PortKey>;
StreamKey keyOf(const StreamedRange& s) {
  return {s.site, s.registerAddress, s.romOffset, s.bytes, s.memory.has_value(), s.memory.value_or(0),
          keyOf(s.landing)};
}

// The order the run reports streams in.
bool streamBefore(const StreamedRange& a, const StreamedRange& b) {
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
}

// The order the run reports samples in: by start, the shorter first, then by bytes.
bool sampleBefore(const KeyedSample& a, const KeyedSample& b) {
  if (a.start != b.start) return a.start < b.start;
  if (a.bytes.size() != b.bytes.size()) return a.bytes.size() < b.bytes.size();
  return a.bytes < b.bytes;
}

// The order the run reports a staged extent's writers in: most bytes first,
// then a writer before the unwritten bytes, then by the writer.
bool writerBefore(const StagedWriter& a, const StagedWriter& b) {
  if (a.bytes != b.bytes) return a.bytes > b.bytes;
  if (a.unwritten != b.unwritten) return !a.unwritten;
  return a.writer < b.writer;
}

// The order the run reports outcomes in: by routine, then register, then the first value.
bool outcomeBefore(const EnumeratedRun& a, const EnumeratedRun& b) {
  if (a.routine != b.routine) return a.routine < b.routine;
  if (a.reg != b.reg) return static_cast<int>(a.reg) < static_cast<int>(b.reg);
  const std::uint32_t first = a.values.empty() ? 0u : a.values.front();
  const std::uint32_t second = b.values.empty() ? 0u : b.values.front();
  return first < second;
}

// Two ascending lists of intervals as one, ascending, no two touching.
std::vector<ir::OriginInterval> unite(const std::vector<ir::OriginInterval>& a,
                                      const std::vector<ir::OriginInterval>& b) {
  std::vector<ir::OriginInterval> all;
  all.reserve(a.size() + b.size());
  std::merge(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(all));
  std::vector<ir::OriginInterval> out;
  for (const ir::OriginInterval& interval : all) {
    if (!out.empty() && interval.first <= out.back().last + 1u) {
      out.back().last = std::max(out.back().last, interval.last);
    } else {
      out.push_back(interval);
    }
  }
  return out;
}

template <typename Value>
std::vector<Value> uniteValues(const std::vector<Value>& a, const std::vector<Value>& b) {
  std::vector<Value> out;
  out.reserve(a.size() + b.size());
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

void joinEnumerated(std::vector<EnumeratedRun>& into, const std::vector<EnumeratedRun>& from) {
  for (const EnumeratedRun& outcome : from) {
    const auto same = std::find_if(into.begin(), into.end(), [&](const EnumeratedRun& held) {
      return held.routine == outcome.routine && held.reg == outcome.reg && held.bits == outcome.bits &&
             held.enteredFrom == outcome.enteredFrom && held.extents == outcome.extents;
    });
    if (same != into.end()) {
      same->values = uniteValues(same->values, outcome.values);
    } else {
      into.push_back(outcome);
    }
  }
  std::stable_sort(into.begin(), into.end(), outcomeBefore);
}

void addEnumerationCounts(RunObservation& into, const RunObservation& from) {
  into.variantsPlanned += from.variantsPlanned;
  into.variantsRun += from.variantsRun;
  into.variantsReturned += from.variantsReturned;
  into.variantsConfined += from.variantsConfined;
  into.enumerationCycles += from.enumerationCycles;
}

// Whether two observations agree in everything but their outcomes and the
// enumeration's counts: two records of one run.
bool sameRun(const RunObservation& a, const RunObservation& b) {
  return a.reached == b.reached && a.moved == b.moved && a.landed == b.landed && a.walked == b.walked &&
         a.palettes == b.palettes && a.ran == b.ran && a.seen == b.seen && a.staged == b.staged &&
         a.streamed == b.streamed && a.samples == b.samples && a.instructions == b.instructions &&
         a.interrupts == b.interrupts && a.nodes == b.nodes && a.divergences == b.divergences &&
         a.spc700Instructions == b.spc700Instructions && a.spc700Nodes == b.spc700Nodes &&
         a.spc700Divergences == b.spc700Divergences && a.originSets == b.originSets && a.originCap == b.originCap;
}

// The number a file under `runs/` carries, or nothing when its name is not
// `<digits>.snagrun`.
std::optional<std::uint64_t> recordNumber(const std::filesystem::path& file) {
  const std::string name = file.filename().string();
  if (name.size() <= kExtension.size() || name.substr(name.size() - kExtension.size()) != kExtension) {
    return std::nullopt;
  }
  return parseDecimal(std::string_view(name).substr(0, name.size() - kExtension.size()));
}

std::optional<std::string> readText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return text.str();
}

// The record files under `runs/`, in the order of their numbers, then by name.
std::vector<std::pair<std::uint64_t, std::filesystem::path>> recordFiles(const std::filesystem::path& runs,
                                                                         std::error_code& ec) {
  std::vector<std::pair<std::uint64_t, std::filesystem::path>> files;
  for (std::filesystem::directory_iterator it(runs, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    if (const std::optional<std::uint64_t> number = recordNumber(it->path())) files.emplace_back(*number, it->path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace

std::string renderRecord(const RunRecord& record, std::span<const std::uint8_t> rom) {
  const RunObservation& o = record.observation;
  std::string out;
  out += std::string(kFirstLine) + "\n";
  out += "image " + std::to_string(rom.size()) + " checksum " + checksumText(rom) + "\n";

  out += "\ncounts instructions " + std::to_string(o.instructions) + " interrupts " + std::to_string(o.interrupts) +
         " nodes " + std::to_string(o.nodes) + " divergences " + std::to_string(o.divergences) +
         " spc700-instructions " + std::to_string(o.spc700Instructions) + " spc700-nodes " +
         std::to_string(o.spc700Nodes) + " spc700-divergences " + std::to_string(o.spc700Divergences) +
         " origin-sets " + std::to_string(o.originSets) + " origin-cap " + std::to_string(o.originCap) +
         " variants-planned " + std::to_string(o.variantsPlanned) + " variants-run " + std::to_string(o.variantsRun) +
         " variants-returned " + std::to_string(o.variantsReturned) + " variants-confined " +
         std::to_string(o.variantsConfined) + " enumeration-cycles " + std::to_string(o.enumerationCycles) + "\n";

  if (!record.notes.empty()) out += "\n";
  for (const std::string& note : record.notes) out += "note " + note + "\n";

  if (!o.reached.empty()) out += "\n";
  for (const ReachedTarget& seen : o.reached) {
    out += "reached " + address24(seen.target) + " " + modeText(seen.mode) + " from " + address24(seen.site) +
           (seen.call ? " call" : " jump") + "\n";
  }
  if (!o.ran.empty()) out += "\n";
  for (const Landing& landing : o.ran) {
    out += "ran " + address24(landing.target) + " " + modeText(landing.mode) + " from " + address24(landing.site) +
           "\n";
  }
  if (!o.moved.empty()) out += "\n";
  for (const MovedRange& range : o.moved) {
    out += "moved " + address24(range.site) + " channel " + std::to_string(range.channel) +
           (range.toRegister ? " to-register " : " from-register ") + address24(range.registerAddress) +
           " memory " + address24(range.memory) + " " + std::string(movedStepName(range.step)) + " bytes " +
           std::to_string(range.bytes) + " as " + std::string(movedKindName(range.kind)) + " times " +
           std::to_string(range.times) + "\n";
  }
  if (!o.palettes.empty()) out += "\n";
  for (const std::vector<std::uint8_t>& palette : o.palettes) out += "palette " + bytesText(palette) + "\n";
  if (!o.landed.empty()) out += "\n";
  for (const LandedRange& landed : o.landed) {
    out += "landed " + address24(landed.site) + " channel " + std::to_string(landed.channel) + " memory " +
           address24(landed.memory) + " bytes " + std::to_string(landed.bytes) + " as " +
           std::string(movedKindName(landed.kind)) + " times " + std::to_string(landed.times) + " at " +
           landingText(landed.landing) + "\n";
  }
  if (!o.walked.empty()) out += "\n";
  for (const WalkedRange& walk : o.walked) {
    out += "walked " + address24(walk.site) + " channel " + std::to_string(walk.channel) + " memory " +
           address24(walk.memory) + " bytes " + std::to_string(walk.bytes) + " as " +
           std::string(movedKindName(walk.kind)) + " unit " + std::to_string(walk.unit) +
           (walk.indirect ? " indirect" : " direct") + " times " + std::to_string(walk.times) + "\n";
  }
  const auto seenList = [](const auto& values, int digits) {
    if (values.empty()) return std::string("none");
    std::string list;
    for (const auto value : values) {
      if (!list.empty()) list += "|";
      list += "$" + hex(value, digits);
    }
    return list;
  };
  if (!o.seen.empty()) out += "\n";
  for (const SeenState& seen : o.seen) {
    out += "seen " + address24(seen.address) + " D=" + seenList(seen.d, 4) + " DBR=" + seenList(seen.dbr, 2) + "\n";
  }
  if (!o.staged.empty()) out += "\n";
  for (const StagedRange& range : o.staged) {
    out += "staged " + address24(range.memory) + " bytes " + std::to_string(range.bytes) + " origin " +
           originText(range.origin) + "\n";
    for (const StagedWriter& writer : range.writers) {
      out += "  writer " + address24(writer.writer.site) + (writer.writer.engine ? " engine" : " instruction") +
             (writer.unwritten ? " unwritten" : " written") + " bytes " + std::to_string(writer.bytes) +
             " sources " + intervalsText(writer.sources) + " origin " + originText(writer.origin) + "\n";
    }
    for (const CarriedContent& content : range.contents) {
      out += "  content " + classText(content.cls) + " as " + std::string(movedKindName(content.kind)) + " unit " +
             std::to_string(content.unit) + (content.indirect ? " indirect" : " direct") + " order " +
             std::to_string(content.order) + " bytes " + bytesText(content.bytes) + " at " +
             (content.landing ? landingText(*content.landing) : std::string("none")) + " origin " +
             originText(content.origin) + "\n";
      for (const ContentOrigin& run : content.byteOrigins) {
        out += "    from " + std::to_string(run.bytes) + " origin " + originText(run.origin) + "\n";
      }
    }
  }
  if (!o.streamed.empty()) out += "\n";
  for (const StreamedRange& stream : o.streamed) {
    out += "streamed " + address24(stream.site) + " " + address24(stream.registerAddress) + " offset " +
           offsetText(stream.romOffset) + " bytes " + std::to_string(stream.bytes) + " times " +
           std::to_string(stream.times) + " source " + intervalText(stream.source) + " memory " +
           (stream.memory ? address24(*stream.memory) : std::string("none")) + " at " +
           (stream.landing ? landingText(*stream.landing) : std::string("none")) + "\n";
  }
  if (!o.samples.empty()) out += "\n";
  for (const KeyedSample& sample : o.samples) {
    out += "sample " + text::address16(sample.start) + " loop " + text::address16(sample.loop) + " times " +
           std::to_string(sample.times) + " bytes " + bytesText(sample.bytes) + "\n";
  }
  if (!o.enumerated.empty()) out += "\n";
  for (const EnumeratedRun& run : o.enumerated) {
    out += "enumerated " + address24(run.routine) + " " + std::string(inputRegisterName(run.reg)) + " bits " +
           std::to_string(run.bits) + " values " + valuesText(run.bits, run.values) + " entered-from " +
           address24(run.enteredFrom) + "\n";
    for (const EnumeratedExtent& extent : run.extents) {
      out += "  extent " + address24(extent.memory) + " bytes " + std::to_string(extent.bytes) + " classes " +
             classesListText(extent.classes) + " values " + bytesText(extent.values) + " sources " +
             intervalsText(extent.sources) + " origin " + originText(extent.origin) + "\n";
      for (const OwnCarry& carry : extent.carried) {
        out += "    carried " + classText(carry.cls) + " as " + std::string(movedKindName(carry.kind)) +
               " register " + address24(carry.registerAddress) + " from " + address24(carry.site) + "\n";
      }
    }
  }
  return out;
}

std::optional<RunRecord> parseRecord(std::string_view text, std::string& error) {
  const std::vector<std::string_view> lines = linesOf(text);
  const auto fail = [&](std::size_t number, const std::string& what) {
    error = "line " + std::to_string(number) + ": " + what;
    return std::nullopt;
  };
  if (lines.empty() || lines[0] != kFirstLine) {
    const std::string found = lines.empty() ? std::string("nothing") : "`" + std::string(lines[0]) + "`";
    return fail(1, "a record begins `" + std::string(kFirstLine) + "`; this one begins " + found);
  }
  if (lines.size() < 2u || !parseImageLine(lines[1])) {
    return fail(2, "the second line is `image <bytes> checksum $XXXX`, or `checksum none` for an image with no header");
  }

  RunRecord record;
  RunObservation& o = record.observation;
  bool counted = false;
  std::string_view block;  // the kind of the last line at the left margin
  for (std::size_t i = 2; i < lines.size(); ++i) {
    const std::size_t number = i + 1u;
    const std::string_view line = lines[i];
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) continue;
    const std::string_view body = line.substr(first);
    if (body == "note" || body.rfind("note ", 0) == 0) {
      record.notes.emplace_back(body.size() > 5u ? body.substr(5) : std::string_view{});
      block = "note";
      continue;
    }
    const std::vector<std::string> words = tokens(body);
    const std::string& kind = words[0];
    Fields f(words, o.palettes.size());
    std::string shape;

    if (kind == "counts") {
      shape = "a counts line names its fourteen counts in order: instructions, interrupts, nodes, divergences, "
              "spc700-instructions, spc700-nodes, spc700-divergences, origin-sets, origin-cap, variants-planned, "
              "variants-run, variants-returned, variants-confined, enumeration-cycles";
      if (counted) return fail(number, "a record has one counts line");
      counted = true;
      f.expect("instructions");
      o.instructions = f.count();
      f.expect("interrupts");
      o.interrupts = f.count();
      f.expect("nodes");
      o.nodes = static_cast<std::size_t>(f.count());
      f.expect("divergences");
      o.divergences = f.count();
      f.expect("spc700-instructions");
      o.spc700Instructions = f.count();
      f.expect("spc700-nodes");
      o.spc700Nodes = static_cast<std::size_t>(f.count());
      f.expect("spc700-divergences");
      o.spc700Divergences = f.count();
      f.expect("origin-sets");
      o.originSets = static_cast<std::size_t>(f.count());
      f.expect("origin-cap");
      o.originCap = static_cast<std::size_t>(f.count());
      f.expect("variants-planned");
      o.variantsPlanned = f.count();
      f.expect("variants-run");
      o.variantsRun = f.count();
      f.expect("variants-returned");
      o.variantsReturned = f.count();
      f.expect("variants-confined");
      o.variantsConfined = f.count();
      f.expect("enumeration-cycles");
      o.enumerationCycles = f.count();
    } else if (kind == "reached") {
      shape = "a reached line is the target, e=, m=, x=, `from` and the site, and `call` or `jump`";
      ReachedTarget seen;
      seen.target = f.address();
      seen.mode = f.mode();
      f.expect("from");
      seen.site = f.address();
      seen.call = f.either("call", "jump");
      o.reached.push_back(std::move(seen));
    } else if (kind == "ran") {
      shape = "a ran line is the target, e=, m=, x=, `from` and the site";
      Landing landing;
      landing.target = f.address();
      landing.mode = f.mode();
      f.expect("from");
      landing.site = f.address();
      o.ran.push_back(std::move(landing));
    } else if (kind == "moved") {
      shape = "a moved line is the site, `channel` and its number, `to-register` or `from-register`, the "
              "register, `memory` and its address, the step, `bytes`, `as` and the kind, and `times`";
      MovedRange range;
      range.site = f.address();
      f.expect("channel");
      range.channel = static_cast<std::uint8_t>(f.count32());
      range.toRegister = f.either("to-register", "from-register");
      range.registerAddress = f.address();
      nameRegister(range);
      f.expect("memory");
      range.memory = f.address();
      range.step = f.step();
      f.expect("bytes");
      range.bytes = f.count32();
      f.expect("as");
      range.kind = f.kind();
      f.expect("times");
      range.times = f.count32();
      o.moved.push_back(range);
    } else if (kind == "palette") {
      shape = "a palette line is its bytes, two hexadecimal digits a byte";
      o.palettes.push_back(f.bytes());
    } else if (kind == "landed") {
      shape = "a landed line is the site, `channel`, `memory`, `bytes`, `as`, `times` and `at` and a landing";
      LandedRange landed;
      landed.site = f.address();
      f.expect("channel");
      landed.channel = static_cast<std::uint8_t>(f.count32());
      f.expect("memory");
      landed.memory = f.address();
      f.expect("bytes");
      landed.bytes = f.count32();
      f.expect("as");
      landed.kind = f.kind();
      f.expect("times");
      landed.times = f.count32();
      f.expect("at");
      landed.landing = f.landing();
      o.landed.push_back(landed);
    } else if (kind == "walked") {
      shape = "a walked line is the site, `channel`, `memory`, `bytes`, `as`, `unit`, `direct` or `indirect`, and "
              "`times`";
      WalkedRange walk;
      walk.site = f.address();
      f.expect("channel");
      walk.channel = static_cast<std::uint8_t>(f.count32());
      f.expect("memory");
      walk.memory = f.address();
      f.expect("bytes");
      walk.bytes = f.count32();
      f.expect("as");
      walk.kind = f.kind();
      f.expect("unit");
      walk.unit = f.count32();
      walk.indirect = f.either("indirect", "direct");
      f.expect("times");
      walk.times = f.count32();
      o.walked.push_back(walk);
    } else if (kind == "seen") {
      shape = "a seen line is the site, `D=` and its values, and `DBR=` and its values";
      SeenState seen;
      seen.address = f.address();
      const auto values = [&](std::string_view lead, std::size_t digits) {
        std::vector<std::uint32_t> out;
        const std::string& field = f.take();
        if (field.rfind(lead, 0) != 0) return std::optional<std::vector<std::uint32_t>>{};
        const std::string_view list = std::string_view(field).substr(lead.size());
        if (list == "none") return std::optional{out};
        for (const std::string_view value : split(list, '|')) {
          const std::optional<std::uint32_t> parsed = parseDollar(value);
          if (!parsed || value.size() != digits + 1u) return std::optional<std::vector<std::uint32_t>>{};
          out.push_back(*parsed);
        }
        return std::optional{out};
      };
      const std::optional<std::vector<std::uint32_t>> d = values("D=", 4);
      const std::optional<std::vector<std::uint32_t>> dbr = values("DBR=", 2);
      if (!d || !dbr || !f.complete()) return fail(number, shape);
      for (const std::uint32_t value : *d) seen.d.push_back(static_cast<std::uint16_t>(value));
      for (const std::uint32_t value : *dbr) seen.dbr.push_back(static_cast<std::uint8_t>(value));
      o.seen.push_back(std::move(seen));
    } else if (kind == "staged") {
      shape = "a staged line is the extent's address, `bytes` and its count, and `origin` and an origin";
      StagedRange range;
      range.memory = f.address();
      f.expect("bytes");
      range.bytes = f.count32();
      f.expect("origin");
      range.origin = f.origin();
      o.staged.push_back(std::move(range));
    } else if (kind == "writer") {
      shape = "a writer line is the site, `instruction` or `engine`, `written` or `unwritten`, `bytes`, "
              "`sources` and `origin`";
      if (block != "staged") return fail(number, "a writer line follows a staged line");
      StagedWriter writer;
      writer.writer.site = f.address();
      writer.writer.engine = f.either("engine", "instruction");
      writer.unwritten = f.either("unwritten", "written");
      f.expect("bytes");
      writer.bytes = f.count();
      f.expect("sources");
      writer.sources = f.intervals();
      f.expect("origin");
      writer.origin = f.origin();
      if (f.complete()) o.staged.back().writers.push_back(std::move(writer));
    } else if (kind == "content") {
      shape = "a content line is the class, `as`, `unit`, `direct` or `indirect`, `order`, `bytes`, `at` and a "
              "landing or `none`, and `origin`";
      if (block != "staged") return fail(number, "a content line follows a staged line");
      CarriedContent content;
      content.cls = f.registerClass();
      f.expect("as");
      content.kind = f.kind();
      f.expect("unit");
      content.unit = f.count32();
      content.indirect = f.either("indirect", "direct");
      f.expect("order");
      content.order = f.count32();
      f.expect("bytes");
      content.bytes = f.bytes();
      f.expect("at");
      content.landing = f.optionalLanding();
      f.expect("origin");
      content.origin = f.origin();
      if (f.complete()) o.staged.back().contents.push_back(std::move(content));
    } else if (kind == "from") {
      shape = "a from line is the count of bytes and `origin` and an origin";
      if (block != "staged" || o.staged.back().contents.empty()) {
        return fail(number, "a from line follows a content line");
      }
      ContentOrigin run;
      run.bytes = f.count32();
      f.expect("origin");
      run.origin = f.origin();
      if (f.complete()) o.staged.back().contents.back().byteOrigins.push_back(std::move(run));
    } else if (kind == "streamed") {
      shape = "a streamed line is the site, the register, `offset`, `bytes`, `times`, `source`, `memory` and an "
              "address or `none`, and `at` and a landing or `none`";
      StreamedRange stream;
      stream.site = f.address();
      stream.registerAddress = f.address();
      nameRegister(stream);
      f.expect("offset");
      stream.romOffset = f.offset();
      f.expect("bytes");
      stream.bytes = f.count32();
      f.expect("times");
      stream.times = f.count32();
      f.expect("source");
      const std::vector<ir::OriginInterval> source = f.intervals();
      if (source.size() == 1u) stream.source = source.front();
      else f.fail();
      f.expect("memory");
      if (f.peek("none")) f.take();
      else stream.memory = f.address();
      f.expect("at");
      stream.landing = f.optionalLanding();
      o.streamed.push_back(std::move(stream));
    } else if (kind == "sample") {
      shape = "a sample line is the start, `loop`, `times` and `bytes`";
      KeyedSample sample;
      const std::optional<std::uint16_t> start = text::parseShortAddress(f.take());
      f.expect("loop");
      const std::optional<std::uint16_t> loop = text::parseShortAddress(f.take());
      f.expect("times");
      sample.times = f.count32();
      f.expect("bytes");
      sample.bytes = f.bytes();
      if (!start || !loop) return fail(number, shape);
      sample.start = *start;
      sample.loop = *loop;
      if (f.complete()) o.samples.push_back(std::move(sample));
    } else if (kind == "enumerated") {
      shape = "an enumerated line is the routine, the register, `bits`, `values` and `entered-from`";
      EnumeratedRun run;
      run.routine = f.address();
      const std::optional<InputRegister> reg = parseInputRegister(f.take());
      f.expect("bits");
      run.bits = f.count32();
      f.expect("values");
      const std::optional<std::vector<std::uint32_t>> values = parseValues(f.take());
      f.expect("entered-from");
      run.enteredFrom = f.address();
      if (!reg || !values || (run.bits != 8u && run.bits != 16u)) return fail(number, shape);
      run.reg = *reg;
      run.values = *values;
      if (f.complete()) o.enumerated.push_back(std::move(run));
    } else if (kind == "extent") {
      shape = "an extent line is the address, `bytes`, `classes`, `values`, `sources` and `origin`";
      if (block != "enumerated") return fail(number, "an extent line follows an enumerated line");
      EnumeratedExtent extent;
      extent.memory = f.address();
      f.expect("bytes");
      extent.bytes = f.count32();
      f.expect("classes");
      extent.classes = f.classes();
      f.expect("values");
      extent.values = f.bytes();
      f.expect("sources");
      extent.sources = f.intervals();
      f.expect("origin");
      extent.origin = f.origin();
      if (f.complete()) o.enumerated.back().extents.push_back(std::move(extent));
    } else if (kind == "carried") {
      shape = "a carried line is the class, `as`, `register` and `from`";
      if (block != "enumerated" || o.enumerated.back().extents.empty()) {
        return fail(number, "a carried line follows an extent line");
      }
      OwnCarry carry;
      carry.cls = f.registerClass();
      f.expect("as");
      carry.kind = f.kind();
      f.expect("register");
      carry.registerAddress = f.address();
      f.expect("from");
      carry.site = f.address();
      if (f.complete()) o.enumerated.back().extents.back().carried.push_back(carry);
    } else {
      return fail(number, "`" + kind + "` is not a kind of line a record holds");
    }
    if (!f.complete()) return fail(number, f.problem().empty() ? shape : f.problem());
    if (kind == "staged") block = "staged";
    else if (kind == "enumerated") block = "enumerated";
    else if (kind != "writer" && kind != "content" && kind != "from" && kind != "extent" && kind != "carried") {
      block = "other";
    }
  }
  if (!counted) return fail(lines.size(), "a record has a counts line");
  return record;
}

std::string recordMismatch(std::string_view text, std::span<const std::uint8_t> rom) {
  const std::vector<std::string_view> lines = linesOf(text);
  const std::optional<RecordImage> image = lines.size() >= 2u ? parseImageLine(lines[1]) : std::nullopt;
  if (!image) return "the record names no image on its second line";
  if (image->bytes != rom.size()) {
    return "the record was written for an image of " + std::to_string(image->bytes) + " bytes; this one is " +
           std::to_string(rom.size());
  }
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  const std::optional<std::uint16_t> checksum =
      header ? std::optional<std::uint16_t>(header->checksum) : std::nullopt;
  if (image->checksum != checksum) {
    const auto named = [](const std::optional<std::uint16_t>& value) {
      return value ? "checksum $" + hex(*value, 4) : std::string("no header");
    };
    return "the record was written for an image with " + named(image->checksum) + "; this one has " +
           named(checksum);
  }
  return {};
}

void joinObservation(RunObservation& into, const RunObservation& from) {
  // The palettes, and where each of `from`'s stands in the joined list.
  std::vector<std::size_t> place;
  place.reserve(from.palettes.size());
  for (const std::vector<std::uint8_t>& palette : from.palettes) {
    const auto found = std::find(into.palettes.begin(), into.palettes.end(), palette);
    if (found != into.palettes.end()) {
      place.push_back(static_cast<std::size_t>(found - into.palettes.begin()));
    } else {
      place.push_back(into.palettes.size());
      into.palettes.push_back(palette);
    }
  }
  const auto placed = [&](std::optional<PortLanding> landing) {
    if (landing && landing->palette && *landing->palette < place.size()) landing->palette = place[*landing->palette];
    return landing;
  };

  // Sightings and landings the CPU arrived at: each once.
  const auto joinSightings = [](auto& held, const auto& more) {
    std::set<SightingKey> keys;
    for (const auto& item : held) keys.insert(sightingKey(item));
    for (const auto& item : more) {
      if (keys.insert(sightingKey(item)).second) held.push_back(item);
    }
    std::stable_sort(held.begin(), held.end(), [](const auto& a, const auto& b) { return sightingBefore(a, b); });
  };
  joinSightings(into.reached, from.reached);
  joinSightings(into.ran, from.ran);

  // Ranges, landings, walks and streams: each once, the counts added.
  const auto joinCounted = [](auto& held, const auto& more, auto adjust, auto before) {
    std::map<decltype(keyOf(held.front())), std::size_t> index;
    for (std::size_t i = 0; i < held.size(); ++i) index.emplace(keyOf(held[i]), i);
    for (const auto& item : more) {
      const auto found = index.find(keyOf(item));
      if (found != index.end()) {
        held[found->second].times += item.times;
      } else {
        index.emplace(keyOf(item), held.size());
        held.push_back(adjust(item));
      }
    }
    std::stable_sort(held.begin(), held.end(), before);
  };
  const auto same = [](const auto& item) { return item; };
  joinCounted(into.moved, from.moved, same, rangeBefore);
  joinCounted(
      into.landed, from.landed,
      [&](LandedRange landed) {
        landed.landing = *placed(landed.landing);
        return landed;
      },
      landedBefore);
  joinCounted(into.walked, from.walked, same, walkedBefore);
  joinCounted(
      into.streamed, from.streamed,
      [&](StreamedRange stream) {
        stream.landing = placed(stream.landing);
        return stream;
      },
      streamBefore);

  // The samples: each once, the counts added.
  for (const KeyedSample& sample : from.samples) {
    const auto found = std::find_if(into.samples.begin(), into.samples.end(),
                                    [&](const KeyedSample& held) { return sameSample(held, sample); });
    if (found != into.samples.end()) {
      found->times += sample.times;
    } else {
      into.samples.push_back(sample);
    }
  }
  std::stable_sort(into.samples.begin(), into.samples.end(), sampleBefore);

  // The values seen: one entry a site.
  {
    std::map<Address, std::size_t> index;
    for (std::size_t i = 0; i < into.seen.size(); ++i) index.emplace(into.seen[i].address, i);
    for (const SeenState& seen : from.seen) {
      const auto found = index.find(seen.address);
      if (found == index.end()) {
        index.emplace(seen.address, into.seen.size());
        into.seen.push_back(seen);
        continue;
      }
      SeenState& held = into.seen[found->second];
      held.d = uniteValues(held.d, seen.d);
      held.dbr = uniteValues(held.dbr, seen.dbr);
    }
    std::stable_sort(into.seen.begin(), into.seen.end(),
                     [](const SeenState& a, const SeenState& b) { return a.address < b.address; });
  }

  // The staged extents: one entry an extent. `from`'s contents take their
  // places in the run's order of carries after every content `into` holds.
  {
    std::uint64_t base = 0;
    for (const StagedRange& range : into.staged) {
      for (const CarriedContent& content : range.contents) {
        base = std::max(base, static_cast<std::uint64_t>(content.order) + 1u);
      }
    }
    const auto carried = [&](CarriedContent content) {
      content.order = static_cast<std::uint32_t>(content.order + base);
      content.landing = placed(content.landing);
      return content;
    };
    std::map<std::pair<Address, std::uint32_t>, std::size_t> index;
    for (std::size_t i = 0; i < into.staged.size(); ++i) {
      index.emplace(std::pair{into.staged[i].memory, into.staged[i].bytes}, i);
    }
    for (const StagedRange& range : from.staged) {
      const auto found = index.find({range.memory, range.bytes});
      if (found == index.end()) {
        StagedRange added = range;
        for (CarriedContent& content : added.contents) content = carried(content);
        index.emplace(std::pair{range.memory, range.bytes}, into.staged.size());
        into.staged.push_back(std::move(added));
        continue;
      }
      StagedRange& held = into.staged[found->second];
      ir::Origins::merge(held.origin, range.origin);
      for (const StagedWriter& writer : range.writers) {
        const auto both = std::find_if(held.writers.begin(), held.writers.end(), [&](const StagedWriter& w) {
          return w.writer == writer.writer && w.unwritten == writer.unwritten;
        });
        if (both == held.writers.end()) {
          held.writers.push_back(writer);
          continue;
        }
        both->bytes += writer.bytes;
        ir::Origins::merge(both->origin, writer.origin);
        both->sources = unite(both->sources, writer.sources);
      }
      std::stable_sort(held.writers.begin(), held.writers.end(), writerBefore);
      std::vector<CarriedContent> added;
      for (const CarriedContent& content : range.contents) {
        const bool known = std::any_of(held.contents.begin(), held.contents.end(),
                                       [&](const CarriedContent& c) { return sameContent(c, content); });
        if (!known) added.push_back(carried(content));
      }
      std::stable_sort(added.begin(), added.end(),
                       [](const CarriedContent& a, const CarriedContent& b) { return a.order < b.order; });
      for (CarriedContent& content : added) held.contents.push_back(std::move(content));
    }
    std::stable_sort(into.staged.begin(), into.staged.end(), [](const StagedRange& a, const StagedRange& b) {
      return std::pair{a.memory, a.bytes} < std::pair{b.memory, b.bytes};
    });
  }

  joinEnumerated(into.enumerated, from.enumerated);

  into.instructions += from.instructions;
  into.interrupts += from.interrupts;
  into.divergences += from.divergences;
  into.spc700Instructions += from.spc700Instructions;
  into.spc700Divergences += from.spc700Divergences;
  into.nodes = std::max(into.nodes, from.nodes);
  into.spc700Nodes = std::max(into.spc700Nodes, from.spc700Nodes);
  into.originSets = std::max(into.originSets, from.originSets);
  into.originCap = std::max(into.originCap, from.originCap);
  addEnumerationCounts(into, from);
}

std::optional<std::vector<RunRecord>> readRecords(const std::filesystem::path& directory,
                                                  std::span<const std::uint8_t> rom, std::string& error) {
  std::vector<RunRecord> records;
  const std::filesystem::path runs = directory / "runs";
  std::error_code ec;
  if (!std::filesystem::is_directory(runs, ec)) return records;
  const auto files = recordFiles(runs, ec);
  if (ec) {
    error = runs.string() + ": " + ec.message();
    return std::nullopt;
  }
  for (const auto& file : files) {
    const std::filesystem::path& path = file.second;
    const std::optional<std::string> text = readText(path);
    if (!text) {
      error = path.string() + ": cannot be read";
      return std::nullopt;
    }
    std::string problem;
    std::optional<RunRecord> record = parseRecord(*text, problem);
    if (!record) {
      error = path.string() + ": " + problem;
      return std::nullopt;
    }
    if (const std::string mismatch = recordMismatch(*text, rom); !mismatch.empty()) {
      error = path.string() + ": " + mismatch;
      return std::nullopt;
    }
    records.push_back(std::move(*record));
  }
  return records;
}

RunRecord foldRecords(std::span<const RunRecord> records) {
  RunRecord out;
  std::vector<const RunObservation*> runs;
  std::set<std::string> notes;
  for (const RunRecord& record : records) {
    const bool again = std::any_of(runs.begin(), runs.end(),
                                   [&](const RunObservation* run) { return sameRun(*run, record.observation); });
    if (again) {
      joinEnumerated(out.observation.enumerated, record.observation.enumerated);
      addEnumerationCounts(out.observation, record.observation);
    } else {
      joinObservation(out.observation, record.observation);
      runs.push_back(&record.observation);
    }
    for (const std::string& note : record.notes) {
      if (notes.insert(note).second) out.notes.push_back(note);
    }
  }
  return out;
}

bool writeRecord(const RunRecord& record, std::span<const std::uint8_t> rom, const std::filesystem::path& directory,
                 std::string& error) {
  const std::string text = renderRecord(record, rom);
  const std::filesystem::path runs = directory / "runs";
  std::error_code ec;
  std::filesystem::create_directories(runs, ec);
  if (ec) {
    error = "cannot make " + runs.string() + ": " + ec.message();
    return false;
  }
  const auto files = recordFiles(runs, ec);
  if (ec) {
    error = runs.string() + ": " + ec.message();
    return false;
  }
  std::uint64_t highest = 0;
  for (const auto& [number, path] : files) {
    if (readText(path) == text) return true;
    highest = std::max(highest, number);
  }
  std::string name = std::to_string(highest + 1u);
  if (name.size() < 4u) name.insert(0, 4u - name.size(), '0');
  const std::filesystem::path path = runs / (name + std::string(kExtension));
  std::ofstream out(path, std::ios::binary);
  out << text;
  out.close();
  if (!out) {
    error = "cannot write " + path.string();
    return false;
  }
  return true;
}

}  // namespace snaggletooth::disasm
