#include "ir/ir_text.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "cpu65816_disasm.h"

namespace snaggletooth::ir {
namespace {

constexpr std::string_view kOps[] = {
    "Set", "SetNZ", "Add", "Sub", "And", "Or", "Xor", "Shr",
    "DirectAddress", "BankAddress", "LongAddress", "ProgramAddress", "StackAddress",
    "Load", "Store", "StoreRmw", "Push", "Pull", "SettleStack",
    "Adc", "Sbc", "Cmp", "Bit", "BitImm", "Asl", "Lsr", "Rol", "Ror", "Inc", "Dec",
    "Tsb", "Trb", "WriteP", "Xba", "Xce", "Halt", "Cycles",
};

constexpr std::string_view kPlaces[] = {
    "",   "imm", "A", "X",  "Y",  "S",  "D",  "PC", "PBR", "DBR", "P", "E",
    "T0", "T1",  "T2", "T3", "N", "V", "M", "X", "D", "I", "Z", "C",
};

// A flag as the file writes it: qualified by the register it is a bit of, so the
// register `X` and the flag `X` are two words.
constexpr std::string_view kFlagTexts[] = {"P.N", "P.V", "P.M", "P.X", "P.D", "P.I", "P.Z", "P.C"};

constexpr std::string_view kWidths[] = {"8", "16", "24", "byM", "byX"};
constexpr std::string_view kSteps[] = {"flat", "bank0", "bank", "direct", "pointer"};
constexpr std::string_view kAccesses[] = {"data", "rmw", "rmw-unmodified", "vector"};
constexpr std::string_view kWhens[] = {
    "", "if e", "if !e", "if set", "if clear", "if is", "if is not", "if D.lo", "if crossed",
};
constexpr std::string_view kFlows[] = {"continue", "branch", "jump", "call", "return", "halt"};

constexpr std::string_view kAddressings[] = {
    "",       "A",      "#imm(M)", "#imm(X)", "#byte",  "dp",      "dp,X",     "dp,Y",
    "(dp)",   "(dp,X)", "(dp),Y",  "[dp]",    "[dp],Y", "sr,S",    "(sr,S),Y", "abs",
    "abs,X",  "abs,Y",  "long",    "long,X",  "(abs)",  "[abs]",   "(abs,X)",  "rel",
    "rel16",  "src,dst", "#abs",   "rel16",
};

constexpr std::size_t kRegisterPlaceCount = static_cast<std::size_t>(Place::T3) + 1;

std::string hex(std::uint32_t value) {
  char b[16];
  std::snprintf(b, sizeof b, "$%X", value);
  return b;
}

std::string addressText(Address address) {
  char b[16];
  std::snprintf(b, sizeof b, "$%02X:%04X", (address >> 16) & 0xFFu, address & 0xFFFFu);
  return b;
}

std::string_view placeText(Place place) {
  if (place >= Place::FlagN) {
    return kFlagTexts[static_cast<std::size_t>(place) - static_cast<std::size_t>(Place::FlagN)];
  }
  return placeName(place);
}

std::string operandText(const Operand& o) {
  if (o.place == Place::Imm) return hex(o.value);
  return std::string(placeText(o.place));
}

bool carriesStep(Op op) { return op == Op::Load || op == Op::Store || op == Op::StoreRmw; }
bool carriesPin(Op op) { return op == Op::Push || op == Op::Pull; }
bool carriesValue(When when) { return when == When::PlaceIs || when == When::PlaceIsNot; }

// The representation's addressing mode for the backend's.
Addressing addressingOf(disasm::Cpu65816Addressing mode) {
  using M = disasm::Cpu65816Addressing;
  switch (mode) {
    case M::Implied: return Addressing::Implied;
    case M::Accumulator: return Addressing::Accumulator;
    case M::ImmediateM: return Addressing::ImmediateM;
    case M::ImmediateX: return Addressing::ImmediateX;
    case M::ImmediateByte: return Addressing::ImmediateByte;
    case M::Direct: return Addressing::Direct;
    case M::DirectX: return Addressing::DirectX;
    case M::DirectY: return Addressing::DirectY;
    case M::DirectIndirect: return Addressing::DirectIndirect;
    case M::DirectIndirectX: return Addressing::DirectIndirectX;
    case M::DirectIndirectY: return Addressing::DirectIndirectY;
    case M::DirectIndirectLong: return Addressing::DirectIndirectLong;
    case M::DirectIndirectLongY: return Addressing::DirectIndirectLongY;
    case M::StackRelative: return Addressing::StackRelative;
    case M::StackRelativeY: return Addressing::StackRelativeY;
    case M::Absolute: return Addressing::Absolute;
    case M::AbsoluteX: return Addressing::AbsoluteX;
    case M::AbsoluteY: return Addressing::AbsoluteY;
    case M::AbsoluteLong: return Addressing::AbsoluteLong;
    case M::AbsoluteLongX: return Addressing::AbsoluteLongX;
    case M::AbsoluteIndirect: return Addressing::AbsoluteIndirect;
    case M::AbsoluteIndirectLong: return Addressing::AbsoluteIndirectLong;
    case M::AbsoluteIndexedIndirect: return Addressing::AbsoluteIndexedIndirect;
    case M::Relative: return Addressing::Relative;
    case M::RelativeLong: return Addressing::RelativeLong;
    case M::BlockMove: return Addressing::BlockMove;
    case M::PushAbsolute: return Addressing::PushAbsolute;
    case M::PushRelative: return Addressing::PushRelative;
  }
  return Addressing::Implied;
}

// ---- the reader --------------------------------------------------------------

// The words of a line, split at runs of spaces and tabs.
std::vector<std::string_view> words(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

// `digits` as a hexadecimal number of at most eight digits, or nothing.
std::optional<std::uint32_t> hexValue(std::string_view digits) {
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

// A value as the file writes one: `$` and hexadecimal digits.
std::optional<std::uint32_t> value(std::string_view text) {
  if (text.size() < 2 || text[0] != '$') return std::nullopt;
  return hexValue(text.substr(1));
}

// An address as the file writes one: `$BB:XXXX`.
std::optional<Address> address(std::string_view text) {
  if (text.size() != 8 || text[0] != '$' || text[3] != ':') return std::nullopt;
  const std::optional<std::uint32_t> bank = hexValue(text.substr(1, 2));
  const std::optional<std::uint32_t> offset = hexValue(text.substr(4, 4));
  if (!bank || !offset) return std::nullopt;
  return (*bank << 16) | *offset;
}

// A decimal count no larger than `most`.
std::optional<std::uint32_t> decimal(std::string_view text, std::uint32_t most) {
  if (text.empty() || text.size() > 10) return std::nullopt;
  std::uint64_t out = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    out = out * 10u + static_cast<std::uint64_t>(c - '0');
  }
  if (out > most) return std::nullopt;
  return static_cast<std::uint32_t>(out);
}

template <typename Enum, std::size_t N>
std::optional<Enum> named(const std::string_view (&names)[N], std::string_view text,
                          std::size_t from = 0) {
  for (std::size_t i = from; i < N; ++i) {
    if (names[i] == text) return static_cast<Enum>(i);
  }
  return std::nullopt;
}

// A place as the file writes one: a register by name, a flag by `P.` and its
// letter. Neither `imm` nor an empty word is a place on the page.
std::optional<Place> place(std::string_view text) {
  for (std::size_t i = static_cast<std::size_t>(Place::A); i < kRegisterPlaceCount; ++i) {
    if (kPlaces[i] == text) return static_cast<Place>(i);
  }
  for (std::size_t i = 0; i < 8; ++i) {
    if (kFlagTexts[i] == text) {
      return static_cast<Place>(static_cast<std::size_t>(Place::FlagN) + i);
    }
  }
  return std::nullopt;
}

class Reader {
 public:
  explicit Reader(std::string& error) : error_(error) {}

  std::optional<Parsed> read(std::string_view text) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
      const std::size_t end = text.find('\n', pos);
      std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      ++lineNo_;
      if (!readLine(line)) return std::nullopt;
      if (end == std::string_view::npos) break;
      pos = end + 1;
    }
    if (phase_ != Phase::Irq) {
      fail(phase_ == Phase::Version ? "the first line is not `snagir 1`"
           : phase_ == Phase::Image ? "`image` must follow the version"
           : phase_ == Phase::Regions ? "the file ends before its nmi sequence"
                                      : "the file ends before its irq sequence");
      return std::nullopt;
    }
    std::stable_sort(parsed_.program.nodes.begin(), parsed_.program.nodes.end(),
                     [](const Node& a, const Node& b) {
                       return a.instruction.address < b.instruction.address;
                     });
    return std::move(parsed_);
  }

 private:
  enum class Phase { Version, Image, Regions, Nmi, Irq };
  enum class Kind { Label, Data, Node };

  bool fail(const std::string& what) {
    error_ = "line " + std::to_string(lineNo_) + ": " + what;
    return false;
  }

  static std::string quoted(std::string_view word) { return "`" + std::string(word) + "`"; }

  bool readLine(std::string_view line) {
    if (line.empty() || line[0] == '#') return true;
    if (line.starts_with("    ")) return readEffect(line.substr(4));
    if (line[0] == '$') return readNode(line);
    const std::vector<std::string_view> w = words(line);
    if (w.empty()) return true;  // spaces alone are a blank line
    effects_ = nullptr;
    const std::string_view kind = w.front();
    if (phase_ == Phase::Version) {
      if (kind != "snagir") return fail("the first line is not `snagir 1`");
      if (w.size() != 2) return fail("`snagir` takes one field, the version");
      if (w[1] != "1") return fail("version " + std::string(w[1]) + " is not one this reader knows");
      phase_ = Phase::Image;
      return true;
    }
    if (phase_ == Phase::Image) {
      if (kind != "image") return fail("`image` must follow the version");
      if (w.size() != 3) return fail("`image` takes two fields, the size in bytes and the map");
      const std::optional<std::uint32_t> bytes = decimal(w[1], 0xFFFFFFFFu);
      if (!bytes) return fail(quoted(w[1]) + " is not a byte count");
      parsed_.file.imageBytes = *bytes;
      parsed_.file.map = std::string(w[2]);
      phase_ = Phase::Regions;
      return true;
    }
    if (kind == "region") return readRegion(line, w);
    if (kind == "warning") return readWarning(line);
    if (kind == "label") return readLabel(w);
    if (kind == "data") return readData(w);
    if (kind == "nmi" || kind == "irq") return readSequence(w);
    if (kind == "image") return fail("`image` is written once, after the version");
    if (kind == "snagir") return fail("the version is written once, on the first line");
    return fail(quoted(kind) + " is not a record");
  }

  bool inRegion(std::string_view what) {
    if (phase_ != Phase::Regions) return fail(std::string(what) + " after the interrupt sequences");
    if (parsed_.file.regions.empty()) return fail(std::string(what) + " before any region");
    return true;
  }

  // A record's address must lie in the region and follow the record before it:
  // a label, then a data run, then the nodes, at one address.
  bool inOrder(Address at, Kind kind) {
    const ProgramRegion& region = parsed_.file.regions.back();
    if (at < region.first || at > region.last) {
      return fail(addressText(at) + " lies outside " + region.file + "'s range");
    }
    if (last_ && (at < last_->first || (at == last_->first && kind < last_->second))) {
      return fail(addressText(at) + " is out of address order");
    }
    last_ = std::pair{at, kind};
    return true;
  }

  bool readRegion(std::string_view line, const std::vector<std::string_view>& w) {
    if (phase_ != Phase::Regions) return fail("a region after the interrupt sequences");
    if (w.size() < 3) return fail("`region` takes a file and a range");
    const std::string_view range = w.back();
    const std::size_t dash = range.find('-');
    if (dash == std::string_view::npos) return fail(quoted(range) + " is not a range");
    const std::optional<Address> first = address(range.substr(0, dash));
    const std::optional<Address> last = address(range.substr(dash + 1));
    if (!first || !last || *first > *last) return fail(quoted(range) + " is not a range");
    ProgramRegion region;
    region.file = std::string(line.substr(7, line.rfind(' ') - 7));
    region.first = *first;
    region.last = *last;
    parsed_.file.regions.push_back(std::move(region));
    last_.reset();
    return true;
  }

  bool readWarning(std::string_view line) {
    if (!inRegion("a warning")) return false;
    ProgramRegion& region = parsed_.file.regions.back();
    const std::string lead = "warning " + region.file + " ";
    if (!line.starts_with(lead)) return fail("a warning must name its region's file");
    if (last_) return fail("a warning must follow its region's line");
    region.warnings.emplace_back(line.substr(lead.size()));
    return true;
  }

  bool readLabel(const std::vector<std::string_view>& w) {
    if (!inRegion("a label")) return false;
    if (w.size() != 3) return fail("`label` takes an address and a name");
    const std::optional<Address> at = address(w[1]);
    if (!at) return fail(quoted(w[1]) + " is not an address");
    if (!inOrder(*at, Kind::Label)) return false;
    parsed_.file.regions.back().labels.push_back({*at, std::string(w[2])});
    return true;
  }

  bool readData(const std::vector<std::string_view>& w) {
    if (!inRegion("a data run")) return false;
    if (w.size() != 3) return fail("`data` takes an address and the bytes");
    const std::optional<Address> at = address(w[1]);
    if (!at) return fail(quoted(w[1]) + " is not an address");
    if (!inOrder(*at, Kind::Data)) return false;
    const std::string_view digits = w[2];
    if (digits.size() % 2 != 0) return fail("the bytes of a data run come in pairs of digits");
    DataRun run;
    run.address = *at;
    run.bytes.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2) {
      const std::optional<std::uint32_t> byte = hexValue(digits.substr(i, 2));
      if (!byte) return fail(quoted(digits.substr(i, 2)) + " is not a byte");
      run.bytes.push_back(static_cast<std::uint8_t>(*byte));
    }
    parsed_.file.regions.back().data.push_back(std::move(run));
    return true;
  }

  bool readSequence(const std::vector<std::string_view>& w) {
    if (w.size() != 1) return fail(quoted(w[0]) + " takes no field");
    if (w[0] == "nmi") {
      if (phase_ != Phase::Regions) return fail("`nmi` is written once, after the last region");
      phase_ = Phase::Nmi;
      effects_ = &parsed_.program.nmi;
    } else {
      if (phase_ != Phase::Nmi) return fail("`irq` is written once, after `nmi`");
      phase_ = Phase::Irq;
      effects_ = &parsed_.program.irq;
    }
    return true;
  }

  bool readNode(std::string_view line) {
    if (!inRegion("a node")) return false;
    const std::vector<std::string_view> w = words(line);
    std::size_t i = 0;
    auto more = [&](const char* what) {
      if (i < w.size()) return true;
      return fail(std::string("the node lacks its ") + what);
    };
    auto expect = [&](std::string_view word) {
      if (!more(word.data())) return false;
      if (w[i] != word) return fail(quoted(w[i]) + " where `" + std::string(word) + "` belongs");
      ++i;
      return true;
    };

    Node node;
    Instruction& instruction = node.instruction;
    const std::optional<Address> at = address(w[i++]);
    if (!at) return fail(quoted(w[0]) + " is not an address");
    instruction.address = *at;
    if (!more("mnemonic")) return false;
    const std::string_view mnemonic = w[i++];
    std::string_view addressing;
    if (i < w.size() && w[i] != "operand") addressing = w[i++];
    if (!resolve(mnemonic, addressing, instruction)) return false;

    if (!expect("operand")) return false;
    if (!more("operand")) return false;
    const std::optional<std::uint32_t> operand = value(w[i]);
    if (!operand) return fail(quoted(w[i]) + " is not a value");
    instruction.operand = *operand;
    ++i;
    if (i < w.size() && w[i] == "operand2") {
      if (instruction.addressing != Addressing::BlockMove) {
        return fail("`operand2` belongs to a block move alone");
      }
      ++i;
      if (!more("operand2")) return false;
      const std::optional<std::uint32_t> operand2 = value(w[i]);
      if (!operand2 || *operand2 > 0xFFu) return fail(quoted(w[i]) + " is not a bank");
      instruction.operand2 = static_cast<std::uint8_t>(*operand2);
      ++i;
    } else if (instruction.addressing == Addressing::BlockMove) {
      return fail("a block move lacks its `operand2`");
    }

    if (!expect("length")) return false;
    if (!more("length")) return false;
    const std::optional<std::uint32_t> length = decimal(w[i], 4);
    if (!length || *length == 0) return fail(quoted(w[i]) + " is not a length");
    instruction.length = static_cast<std::uint8_t>(*length);
    ++i;

    if (!expect("flow")) return false;
    if (!more("flow")) return false;
    const std::optional<Flow> flow = named<Flow>(kFlows, w[i]);
    if (!flow) return fail(quoted(w[i]) + " is not a flow");
    instruction.flow = *flow;
    ++i;
    if (i < w.size() && w[i] == "target") {
      ++i;
      if (!more("target")) return false;
      const std::optional<Address> target = address(w[i]);
      if (!target) return fail(quoted(w[i]) + " is not an address");
      instruction.target = *target;
      ++i;
    }

    if (!more("mode")) return false;
    if (w[i] == "e=1") {
      node.mode = Mode{};
      ++i;
    } else if (w[i] == "e=0") {
      ++i;
      if (i + 1 >= w.size() || !width(w[i], 'm', node.mode.accumulatorKnown, node.mode.accumulator8) ||
          !width(w[i + 1], 'x', node.mode.indexKnown, node.mode.index8)) {
        return fail("a native mode is `e=0 m=<8|16|?> x=<8|16|?>`");
      }
      node.mode.emulation = false;
      i += 2;
    } else {
      return fail(quoted(w[i]) + " is not a mode");
    }

    if (!expect("base")) return false;
    if (!more("base")) return false;
    if (!costs(w[i], node.cost)) return fail(quoted(w[i]) + " is not four costs");
    ++i;

    if (i < w.size() && w[i] != "patched") {
      const std::string_view name = disasm::cpu65816RegisterName(instruction.operand);
      const bool longForm = instruction.addressing == Addressing::AbsoluteLong ||
                            instruction.addressing == Addressing::AbsoluteLongX;
      if (!longForm || name.empty() || name != w[i]) {
        return fail(quoted(w[i]) + " is not the register at " + hex(instruction.operand));
      }
      node.registerName = name;
      ++i;
    }
    if (i < w.size() && w[i] == "patched") {
      node.patched = true;
      ++i;
    }
    if (i < w.size()) return fail(quoted(w[i]) + " is not a field of a node");

    if (!inOrder(instruction.address, Kind::Node)) return false;
    parsed_.program.nodes.push_back(std::move(node));
    effects_ = &parsed_.program.nodes.back().effects;
    return true;
  }

  // The mnemonic and the addressing mode as the one opcode they name together;
  // the mnemonic the table's own, so the view outlives the text.
  bool resolve(std::string_view mnemonic, std::string_view addressing, Instruction& instruction) {
    for (const disasm::Cpu65816Opcode& row : disasm::cpu65816Opcodes()) {
      if (mnemonic != row.mnemonic) continue;
      const Addressing candidate = addressingOf(row.mode);
      if (addressingName(candidate) != addressing) continue;
      instruction.mnemonic = row.mnemonic;
      instruction.addressing = candidate;
      return true;
    }
    for (const disasm::Cpu65816Opcode& row : disasm::cpu65816Opcodes()) {
      if (mnemonic == row.mnemonic) {
        return fail(quoted(mnemonic) + " with " + quoted(addressing) + " names no opcode");
      }
    }
    return fail(quoted(mnemonic) + " is not a mnemonic");
  }

  static bool width(std::string_view text, char letter, bool& known, bool& eight) {
    if (text.size() < 3 || text[0] != letter || text[1] != '=') return false;
    const std::string_view w = text.substr(2);
    if (w == "8") { known = true; eight = true; return true; }
    if (w == "16") { known = true; eight = false; return true; }
    if (w == "?") { known = false; eight = false; return true; }  // the trace's own canonical bit
    return false;
  }

  static bool costs(std::string_view text, Cost& cost) {
    std::size_t start = 0;
    for (std::size_t n = 0; n < 4; ++n) {
      const std::size_t slash = text.find('/', start);
      const bool lastField = n == 3;
      if (lastField != (slash == std::string_view::npos)) return false;
      const std::optional<std::uint32_t> c =
          decimal(text.substr(start, lastField ? std::string_view::npos : slash - start), 255);
      if (!c) return false;
      cost.base[n] = static_cast<std::uint8_t>(*c);
      start = slash + 1;
    }
    return true;
  }

  bool readEffect(std::string_view body) {
    if (effects_ == nullptr) return fail("an effect with no node or sequence above it");
    // `<op> [<dst> <-] [<a>[, <b>]]  [<bracket>]  [<condition>]`
    const std::size_t open = body.find("  [");
    if (open == std::string_view::npos) return fail("an effect lacks its bracket");
    const std::size_t close = body.find(']', open);
    if (close == std::string_view::npos) return fail("an effect lacks its bracket");
    const std::string_view head = body.substr(0, open);
    const std::string_view bracket = body.substr(open + 3, close - open - 3);
    const std::string_view tail = body.substr(close + 1);

    Effect effect;
    const std::vector<std::string_view> h = words(head);
    if (h.empty()) return fail("an effect lacks its operation");
    const std::optional<Op> op = named<Op>(kOps, h[0]);
    if (!op) return fail(quoted(h[0]) + " is not an operation");
    effect.op = *op;
    std::size_t i = 1;
    if (i + 1 < h.size() && h[i + 1] == "<-") {
      if (!operand(h[i], effect.dst)) return false;
      i += 2;
    }
    std::vector<std::string_view> rest(h.begin() + static_cast<std::ptrdiff_t>(i), h.end());
    if (!rest.empty()) {
      if (rest.size() == 1) {
        if (rest[0].back() == ',') return fail("an effect's operands come in a pair or alone");
        if (!operand(rest[0], effect.a)) return false;
      } else if (rest.size() == 2 && rest[0].size() > 1 && rest[0].back() == ',') {
        if (!operand(rest[0].substr(0, rest[0].size() - 1), effect.a)) return false;
        if (!operand(rest[1], effect.b)) return false;
      } else {
        return fail("an effect's operands come in a pair or alone");
      }
    }

    const std::vector<std::string_view> b = words(bracket);
    if (b.empty()) return fail("an effect's bracket lacks its width");
    const std::optional<Width> width = named<Width>(kWidths, b[0]);
    if (!width) return fail(quoted(b[0]) + " is not a width");
    effect.width = *width;
    bool stepGiven = false;
    bool pinGiven = false;
    for (std::size_t k = 1; k < b.size(); ++k) {
      if (const std::optional<Step> step = named<Step>(kSteps, b[k])) {
        if (!carriesStep(effect.op) || stepGiven) return fail("a step on an operation that carries none");
        effect.step = *step;
        stepGiven = true;
      } else if (const std::optional<Access> access = named<Access>(kAccesses, b[k], 1)) {
        if (!stepGiven || effect.access != Access::Data) return fail("an access kind that follows no step");
        effect.access = *access;
      } else if (b[k] == "pinned" || b[k] == "unpinned") {
        if (!carriesPin(effect.op) || pinGiven) return fail("a pin on an operation that carries none");
        effect.pinned = b[k] == "pinned";
        pinGiven = true;
      } else {
        return fail(quoted(b[k]) + " is not a step, an access kind or a pin");
      }
    }
    if (carriesStep(effect.op) && !stepGiven) return fail("a load or store lacks its step");
    if (carriesPin(effect.op) && !pinGiven) return fail("a push or pull lacks its pin");

    if (!tail.empty()) {
      if (!tail.starts_with("  ")) return fail("a condition follows the bracket after two spaces");
      if (!condition(tail.substr(2), effect.when)) return false;
    }
    effects_->push_back(std::move(effect));
    return true;
  }

  bool operand(std::string_view text, Operand& out) {
    if (text.starts_with('$')) {
      const std::optional<std::uint32_t> v = value(text);
      if (!v) return fail(quoted(text) + " is not a value");
      out = Operand{Place::Imm, *v};
      return true;
    }
    const std::optional<Place> p = place(text);
    if (!p) return fail(quoted(text) + " is not a place");
    out = Operand{*p, 0};
    return true;
  }

  bool condition(std::string_view text, Cond& when) {
    // The longest condition word that opens the text, then its fields.
    std::size_t best = 0;
    for (std::size_t i = 1; i < sizeof kWhens / sizeof kWhens[0]; ++i) {
      const std::string_view name = kWhens[i];
      if (text.starts_with(name) && (text.size() == name.size() || text[name.size()] == ' ') &&
          name.size() > kWhens[best].size()) {
        best = i;
      }
    }
    if (best == 0) return fail(quoted(text) + " is not a condition");
    when.when = static_cast<When>(best);
    std::vector<std::string_view> f = words(text.substr(kWhens[best].size()));
    std::size_t i = 0;
    if (i < f.size() && f[i] != "and" && !f[i].starts_with('$')) {
      const std::optional<Place> p = place(f[i]);
      if (!p) return fail(quoted(f[i]) + " is not a place");
      when.place = *p;
      ++i;
    }
    if (carriesValue(when.when)) {
      if (i >= f.size() || !f[i].starts_with('$')) return fail("`if is` lacks its value");
      const std::optional<std::uint32_t> v = value(f[i]);
      if (!v) return fail(quoted(f[i]) + " is not a value");
      when.value = *v;
      ++i;
    } else if (i < f.size() && f[i].starts_with('$')) {
      return fail("a value on a condition that takes none");
    }
    if (i + 1 < f.size() && f[i] == "and" && f[i + 1] == "e") {
      when.andEmulation = true;
      i += 2;
    }
    if (i < f.size()) return fail(quoted(f[i]) + " is not a field of a condition");
    return true;
  }

  std::string& error_;
  std::size_t lineNo_ = 0;
  Phase phase_ = Phase::Version;
  Parsed parsed_;
  std::vector<Effect>* effects_ = nullptr;
  std::optional<std::pair<Address, Kind>> last_;
};

}  // namespace

std::string_view opName(Op op) noexcept { return kOps[static_cast<std::size_t>(op)]; }
std::string_view placeName(Place place) noexcept { return kPlaces[static_cast<std::size_t>(place)]; }
std::string_view widthName(Width width) noexcept { return kWidths[static_cast<std::size_t>(width)]; }
std::string_view stepName(Step step) noexcept { return kSteps[static_cast<std::size_t>(step)]; }
std::string_view accessName(Access access) noexcept {
  return kAccesses[static_cast<std::size_t>(access)];
}
std::string_view whenName(When when) noexcept { return kWhens[static_cast<std::size_t>(when)]; }
std::string_view flowName(Flow flow) noexcept { return kFlows[static_cast<std::size_t>(flow)]; }
std::string_view addressingName(Addressing addressing) noexcept {
  return kAddressings[static_cast<std::size_t>(addressing)];
}

std::string modeName(const Mode& mode) {
  if (mode.emulation) return "e=1";
  std::string out = "e=0 m=";
  out += mode.accumulatorKnown ? (mode.accumulator8 ? "8" : "16") : "?";
  out += " x=";
  out += mode.indexKnown ? (mode.index8 ? "8" : "16") : "?";
  return out;
}

std::string renderEffect(const Effect& e) {
  std::string line(opName(e.op));
  if (e.dst.place != Place::None) line += " " + operandText(e.dst) + " <-";
  if (e.a.place != Place::None) line += " " + operandText(e.a);
  if (e.b.place != Place::None) line += ", " + operandText(e.b);
  line += "  [";
  line += widthName(e.width);
  if (carriesStep(e.op)) {
    line += " ";
    line += stepName(e.step);
    if (e.access != Access::Data) {
      line += " ";
      line += accessName(e.access);
    }
  }
  if (carriesPin(e.op)) line += e.pinned ? " pinned" : " unpinned";
  line += "]";
  if (e.when.when == When::Always && e.when.andEmulation) {
    // An effect that always runs, and the emulation flag is set: `if e` is what
    // that is, and the interpreter reads the two alike.
    line += "  if e";
  } else if (e.when.when != When::Always) {
    line += "  ";
    line += whenName(e.when.when);
    if (e.when.place != Place::None) {
      line += " ";
      line += placeText(e.when.place);
    }
    if (carriesValue(e.when.when)) line += " " + hex(e.when.value);
    if (e.when.andEmulation) line += " and e";
  }
  return line;
}

std::string renderNode(const Node& node) {
  const Instruction& i = node.instruction;
  std::string out = addressText(i.address) + "  " + std::string(i.mnemonic) + " " +
                    std::string(addressingName(i.addressing)) + "  operand " + hex(i.operand);
  if (i.addressing == Addressing::BlockMove) out += " operand2 " + hex(i.operand2);
  out += "  length " + std::to_string(i.length) + "  flow " + std::string(flowName(i.flow));
  if (i.target) out += " target " + addressText(*i.target);
  out += "  " + modeName(node.mode) + "  base " + std::to_string(node.cost.base[0]) + "/" +
         std::to_string(node.cost.base[1]) + "/" + std::to_string(node.cost.base[2]) + "/" +
         std::to_string(node.cost.base[3]);
  if (!node.registerName.empty()) {
    out += "  ";
    out += node.registerName;
  }
  if (node.patched) out += "  patched";
  out += "\n";
  for (const Effect& e : node.effects) out += "    " + renderEffect(e) + "\n";
  return out;
}

std::string renderProgram(const Program& program, const ProgramFile& file) {
  std::string out = "snagir 1\nimage " + std::to_string(file.imageBytes) + " " + file.map + "\n";
  std::size_t written = 0;
  for (const ProgramRegion& region : file.regions) {
    out += "\nregion " + region.file + " " + addressText(region.first) + "-" +
           addressText(region.last) + "\n";
    for (const std::string& warning : region.warnings) {
      out += "warning " + region.file + " " + warning + "\n";
    }
    std::vector<const Node*> nodes;
    for (const Node& node : program.nodes) {
      const Address at = node.instruction.address;
      if (at >= region.first && at <= region.last) nodes.push_back(&node);
    }
    written += nodes.size();
    // The three sequences merged by address — a label, then a data run, then
    // the nodes, at one address — with a blank line before each data run and
    // before each node, except a node under its own label.
    std::size_t l = 0;
    std::size_t d = 0;
    std::size_t n = 0;
    bool labelled = false;
    while (l < region.labels.size() || d < region.data.size() || n < nodes.size()) {
      const Address la = l < region.labels.size() ? region.labels[l].address : 0xFFFFFFFFu;
      const Address da = d < region.data.size() ? region.data[d].address : 0xFFFFFFFFu;
      const Address na = n < nodes.size() ? nodes[n]->instruction.address : 0xFFFFFFFFu;
      if (la <= da && la <= na) {
        out += "\nlabel " + addressText(la) + " " + region.labels[l++].name + "\n";
        labelled = true;
      } else if (da <= na) {
        const DataRun& run = region.data[d++];
        out += "\ndata " + addressText(run.address) + " ";
        char b[4];
        for (const std::uint8_t byte : run.bytes) {
          std::snprintf(b, sizeof b, "%02X", byte);
          out += b;
        }
        out += "\n";
        labelled = false;
      } else {
        if (!labelled) out += "\n";
        out += renderNode(*nodes[n++]);
        labelled = false;
      }
    }
  }
  if (written != program.nodes.size()) {
    throw std::invalid_argument("a node lies in no region of the program file");
  }
  out += "\nnmi\n";
  for (const Effect& e : program.nmi) out += "    " + renderEffect(e) + "\n";
  out += "\nirq\n";
  for (const Effect& e : program.irq) out += "    " + renderEffect(e) + "\n";
  return out;
}

std::optional<Parsed> parseProgram(std::string_view text, std::string& error) {
  Reader reader(error);
  return reader.read(text);
}

bool equivalent(const Mode& a, const Mode& b) noexcept {
  return a.emulation == b.emulation && a.accumulatorKnown == b.accumulatorKnown &&
         a.indexKnown == b.indexKnown && (!a.accumulatorKnown || a.accumulator8 == b.accumulator8) &&
         (!a.indexKnown || a.index8 == b.index8);
}

bool equivalent(const Node& a, const Node& b) {
  return a.instruction == b.instruction && equivalent(a.mode, b.mode) && a.effects == b.effects &&
         a.cost == b.cost && a.registerName == b.registerName && a.patched == b.patched;
}

bool equivalent(const Program& a, const Program& b) {
  if (a.nodes.size() != b.nodes.size() || a.nmi != b.nmi || a.irq != b.irq) return false;
  for (std::size_t i = 0; i < a.nodes.size(); ++i) {
    if (!equivalent(a.nodes[i], b.nodes[i])) return false;
  }
  return true;
}

}  // namespace snaggletooth::ir
