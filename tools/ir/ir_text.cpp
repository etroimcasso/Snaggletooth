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

// A string as the file writes one: in double quotes, with `"` and `\` escaped.
std::string quotedText(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

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

// One token of the file: a word, one of the three delimiters `{` `}` `;`, or a
// string, with the line it begins on. A string's text is what the quotes held,
// unescaped, and `quoted` says it was one.
struct Token {
  std::string text;
  std::size_t line = 0;
  bool quoted = false;
};

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

bool isDelimiter(char c) { return c == '{' || c == '}' || c == ';'; }

class Reader {
 public:
  explicit Reader(std::string& error) : error_(error) {}

  std::optional<Parsed> read(std::string_view text) {
    if (!tokenize(text)) return std::nullopt;
    if (!readVersion() || !readImage()) return std::nullopt;
    while (peekIs("region")) {
      if (!readRegion()) return std::nullopt;
    }
    if (!readSequence("nmi", parsed_.program.nmi) || !readSequence("irq", parsed_.program.irq)) {
      return std::nullopt;
    }
    if (pos_ < tokens_.size()) {
      fail(quoted(tokens_[pos_].text) + " after the irq sequence");
      return std::nullopt;
    }
    std::stable_sort(parsed_.program.nodes.begin(), parsed_.program.nodes.end(),
                     [](const Node& a, const Node& b) {
                       return a.instruction.address < b.instruction.address;
                     });
    return std::move(parsed_);
  }

 private:
  enum class Kind { Label, Data, Node };

  // ---- tokens ------------------------------------------------------------------

  bool tokenize(std::string_view text) {
    std::size_t line = 1;
    std::size_t i = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (c == '\n') {
        ++line;
        ++i;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r') {
        ++i;
        continue;
      }
      if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
        while (i < text.size() && text[i] != '\n') ++i;
        continue;
      }
      if (isDelimiter(c)) {
        tokens_.push_back(Token{.text = std::string(1, c), .line = line, .quoted = false});
        ++i;
        continue;
      }
      if (c == '"') {
        std::string out;
        ++i;
        bool closed = false;
        while (i < text.size()) {
          const char d = text[i++];
          if (d == '\\' && i < text.size()) {
            out += text[i++];
          } else if (d == '"') {
            closed = true;
            break;
          } else if (d == '\n') {
            break;
          } else {
            out += d;
          }
        }
        if (!closed) return failAt(line, "a string that does not close on its line");
        tokens_.push_back(Token{.text = std::move(out), .line = line, .quoted = true});
        continue;
      }
      const std::size_t start = i;
      while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' &&
             !isDelimiter(text[i]) && text[i] != '"') {
        ++i;
      }
      tokens_.push_back(Token{.text = std::string(text.substr(start, i - start)), .line = line, .quoted = false});
    }
    lastLine_ = line;
    return true;
  }

  bool failAt(std::size_t line, const std::string& what) {
    error_ = "line " + std::to_string(line) + ": " + what;
    return false;
  }

  // The line of the token at hand — or the last line of the file when the
  // tokens have run out.
  std::size_t here() const { return pos_ < tokens_.size() ? tokens_[pos_].line : lastLine_; }

  bool fail(const std::string& what) { return failAt(here(), what); }

  static std::string quoted(std::string_view word) { return "`" + std::string(word) + "`"; }

  bool peekIs(std::string_view word) const {
    return pos_ < tokens_.size() && !tokens_[pos_].quoted && tokens_[pos_].text == word;
  }

  // The next token, a word or a delimiter, or nothing at the end of the file.
  const Token* next() {
    if (pos_ >= tokens_.size()) return nullptr;
    return &tokens_[pos_++];
  }

  // The next word, which must not be a delimiter or a string.
  const Token* word(const char* what) {
    const Token* t = next();
    if (t == nullptr) {
      fail(std::string("the file ends where ") + what + " belongs");
      return nullptr;
    }
    if (t->quoted || isDelimiter(t->text[0])) {
      --pos_;
      fail(quoted(t->quoted ? "\"" + t->text + "\"" : t->text) + " where " + what + " belongs");
      return nullptr;
    }
    return t;
  }

  bool expect(std::string_view text) {
    const Token* t = next();
    if (t == nullptr) return fail("the file ends where `" + std::string(text) + "` belongs");
    if (t->quoted || t->text != text) {
      --pos_;
      return fail(quoted(t->text) + " where `" + std::string(text) + "` belongs");
    }
    return true;
  }

  // ---- the head ------------------------------------------------------------------

  bool readVersion() {
    if (!peekIs("snagir")) return fail("the file does not open with `snagir 1;`");
    ++pos_;
    const Token* version = word("the version");
    if (version == nullptr) return false;
    if (version->text != "1") return failAt(version->line, "version " + version->text + " is not one this reader knows");
    return expect(";");
  }

  bool readImage() {
    if (!peekIs("image")) return fail("`image` must follow the version");
    ++pos_;
    const Token* bytes = word("the image's size");
    if (bytes == nullptr) return false;
    const std::optional<std::uint32_t> count = decimal(bytes->text, 0xFFFFFFFFu);
    if (!count) return failAt(bytes->line, quoted(bytes->text) + " is not a byte count");
    const Token* map = word("the image's map");
    if (map == nullptr) return false;
    parsed_.file.imageBytes = *count;
    parsed_.file.map = map->text;
    return expect(";");
  }

  // ---- a region ------------------------------------------------------------------

  bool readRegion() {
    ++pos_;  // `region`
    const Token* file = next();
    if (file == nullptr || (!file->quoted && isDelimiter(file->text[0]))) {
      if (file != nullptr) --pos_;
      return fail("`region` takes a file and a range");
    }
    const Token* range = word("the region's range");
    if (range == nullptr) return false;
    const std::size_t dash = range->text.find('-');
    if (dash == std::string::npos) return failAt(range->line, quoted(range->text) + " is not a range");
    const std::optional<Address> first = address(std::string_view(range->text).substr(0, dash));
    const std::optional<Address> last = address(std::string_view(range->text).substr(dash + 1));
    if (!first || !last || *first > *last) return failAt(range->line, quoted(range->text) + " is not a range");
    if (!expect("{")) return false;
    ProgramRegion region;
    region.file = file->text;
    region.first = *first;
    region.last = *last;
    parsed_.file.regions.push_back(std::move(region));
    last_.reset();
    bool opened = false;  // whether a label, a data run or a node has been read
    for (;;) {
      const Token* t = next();
      if (t == nullptr) return fail("the file ends inside " + parsed_.file.regions.back().file + "'s region");
      if (!t->quoted && t->text == "}") return true;
      if (t->quoted) return failAt(t->line, "a string where a record belongs");
      if (t->text == "warning") {
        if (opened) return failAt(t->line, "a warning must come before the region's labels, data and nodes");
        const Token* text = next();
        if (text == nullptr || !text->quoted) {
          if (text != nullptr) --pos_;
          return fail("`warning` takes its text in double quotes");
        }
        parsed_.file.regions.back().warnings.push_back(text->text);
        if (!expect(";")) return false;
        continue;
      }
      opened = true;
      if (t->text == "label") {
        if (!readLabel()) return false;
      } else if (t->text == "data") {
        if (!readData()) return false;
      } else if (t->text[0] == '$') {
        if (!readNode(*t)) return false;
      } else if (t->text == "region" || t->text == "nmi" || t->text == "irq" || t->text == "image" ||
                 t->text == "snagir") {
        return failAt(t->line, quoted(t->text) + " inside a region");
      } else {
        return failAt(t->line, quoted(t->text) + " is not a record");
      }
    }
  }

  // A record's address must lie in the region and follow the record before it:
  // a label, then a data run, then the nodes, at one address.
  bool inOrder(std::size_t line, Address at, Kind kind) {
    const ProgramRegion& region = parsed_.file.regions.back();
    if (at < region.first || at > region.last) {
      return failAt(line, addressText(at) + " lies outside " + region.file + "'s range");
    }
    if (last_ && (at < last_->first || (at == last_->first && kind < last_->second))) {
      return failAt(line, addressText(at) + " is out of address order");
    }
    last_ = std::pair{at, kind};
    return true;
  }

  bool readLabel() {
    const Token* at = word("the label's address");
    if (at == nullptr) return false;
    const std::optional<Address> a = address(at->text);
    if (!a) return failAt(at->line, quoted(at->text) + " is not an address");
    const Token* name = word("the label's name");
    if (name == nullptr) return false;
    if (!inOrder(at->line, *a, Kind::Label)) return false;
    parsed_.file.regions.back().labels.push_back({*a, name->text});
    return expect(";");
  }

  bool readData() {
    const Token* at = word("the data run's address");
    if (at == nullptr) return false;
    const std::optional<Address> a = address(at->text);
    if (!a) return failAt(at->line, quoted(at->text) + " is not an address");
    const Token* digits = word("the data run's bytes");
    if (digits == nullptr) return false;
    if (digits->text.size() % 2 != 0) return failAt(digits->line, "the bytes of a data run come in pairs of digits");
    if (!inOrder(at->line, *a, Kind::Data)) return false;
    DataRun run;
    run.address = *a;
    run.bytes.reserve(digits->text.size() / 2);
    for (std::size_t i = 0; i < digits->text.size(); i += 2) {
      const std::optional<std::uint32_t> byte = hexValue(std::string_view(digits->text).substr(i, 2));
      if (!byte) return failAt(digits->line, quoted(std::string_view(digits->text).substr(i, 2)) + " is not a byte");
      run.bytes.push_back(static_cast<std::uint8_t>(*byte));
    }
    parsed_.file.regions.back().data.push_back(std::move(run));
    return expect(";");
  }

  // ---- a node --------------------------------------------------------------------

  bool readNode(const Token& first) {
    Node node;
    Instruction& instruction = node.instruction;
    const std::optional<Address> at = address(first.text);
    if (!at) return failAt(first.line, quoted(first.text) + " is not an address");
    instruction.address = *at;
    const Token* mnemonic = word("the node's mnemonic");
    if (mnemonic == nullptr) return false;
    std::string addressing;
    if (!peekIs("operand")) {
      const Token* form = word("the node's addressing mode");
      if (form == nullptr) return false;
      addressing = form->text;
    }
    if (!resolve(*mnemonic, addressing, instruction)) return false;

    if (!expect("operand")) return false;
    const Token* operand = word("the operand");
    if (operand == nullptr) return false;
    const std::optional<std::uint32_t> operandValue = value(operand->text);
    if (!operandValue) return failAt(operand->line, quoted(operand->text) + " is not a value");
    instruction.operand = *operandValue;
    if (peekIs("operand2")) {
      const Token* key = next();
      if (instruction.addressing != Addressing::BlockMove) {
        return failAt(key->line, "`operand2` belongs to a block move alone");
      }
      const Token* bank = word("the block move's destination bank");
      if (bank == nullptr) return false;
      const std::optional<std::uint32_t> operand2 = value(bank->text);
      if (!operand2 || *operand2 > 0xFFu) return failAt(bank->line, quoted(bank->text) + " is not a bank");
      instruction.operand2 = static_cast<std::uint8_t>(*operand2);
    } else if (instruction.addressing == Addressing::BlockMove) {
      return fail("a block move lacks its `operand2`");
    }

    if (!expect("length")) return false;
    const Token* length = word("the length");
    if (length == nullptr) return false;
    const std::optional<std::uint32_t> lengthValue = decimal(length->text, 4);
    if (!lengthValue || *lengthValue == 0) return failAt(length->line, quoted(length->text) + " is not a length");
    instruction.length = static_cast<std::uint8_t>(*lengthValue);

    if (!expect("flow")) return false;
    const Token* flow = word("the flow");
    if (flow == nullptr) return false;
    const std::optional<Flow> flowValue = named<Flow>(kFlows, flow->text);
    if (!flowValue) return failAt(flow->line, quoted(flow->text) + " is not a flow");
    instruction.flow = *flowValue;
    if (peekIs("target")) {
      ++pos_;
      const Token* target = word("the target");
      if (target == nullptr) return false;
      const std::optional<Address> targetValue = address(target->text);
      if (!targetValue) return failAt(target->line, quoted(target->text) + " is not an address");
      instruction.target = *targetValue;
    }

    const Token* e = word("the mode");
    if (e == nullptr) return false;
    if (e->text == "e=1") {
      node.mode = Mode{};
    } else if (e->text == "e=0") {
      const Token* m = word("the accumulator width");
      const Token* x = m == nullptr ? nullptr : word("the index width");
      if (m == nullptr || x == nullptr) return false;
      if (!width(m->text, 'm', node.mode.accumulatorKnown, node.mode.accumulator8) ||
          !width(x->text, 'x', node.mode.indexKnown, node.mode.index8)) {
        return failAt(m->line, "a native mode is `e=0 m=<8|16|?> x=<8|16|?>`");
      }
      node.mode.emulation = false;
    } else {
      return failAt(e->line, quoted(e->text) + " is not a mode");
    }

    if (!expect("base")) return false;
    const Token* base = word("the costs");
    if (base == nullptr) return false;
    if (!costs(base->text, node.cost)) return failAt(base->line, quoted(base->text) + " is not four costs");

    if (!peekIs("{") && !peekIs("patched")) {
      const Token* name = word("the register name");
      if (name == nullptr) return false;
      const std::string_view tableName = disasm::cpu65816RegisterName(instruction.operand);
      const bool longForm = instruction.addressing == Addressing::AbsoluteLong ||
                            instruction.addressing == Addressing::AbsoluteLongX;
      if (!longForm || tableName.empty() || tableName != name->text) {
        return failAt(name->line, quoted(name->text) + " is not the register at " + hex(instruction.operand));
      }
      node.registerName = tableName;
    }
    if (peekIs("patched")) {
      ++pos_;
      node.patched = true;
    }
    if (!peekIs("{")) {
      const Token* stray = next();
      if (stray == nullptr) return fail("the file ends where the node's `{` belongs");
      return failAt(stray->line, quoted(stray->text) + " is not a field of a node");
    }
    ++pos_;
    if (!readEffects(node.effects)) return false;
    if (!inOrder(first.line, instruction.address, Kind::Node)) return false;
    parsed_.program.nodes.push_back(std::move(node));
    return true;
  }

  // The mnemonic and the addressing mode as the one opcode they name together;
  // the mnemonic the table's own, so the view outlives the text.
  bool resolve(const Token& mnemonic, std::string_view addressing, Instruction& instruction) {
    for (const disasm::Cpu65816Opcode& row : disasm::cpu65816Opcodes()) {
      if (mnemonic.text != row.mnemonic) continue;
      const Addressing candidate = addressingOf(row.mode);
      if (addressingName(candidate) != addressing) continue;
      instruction.mnemonic = row.mnemonic;
      instruction.addressing = candidate;
      return true;
    }
    for (const disasm::Cpu65816Opcode& row : disasm::cpu65816Opcodes()) {
      if (mnemonic.text == row.mnemonic) {
        return failAt(mnemonic.line, quoted(mnemonic.text) + " with " + quoted(addressing) + " names no opcode");
      }
    }
    return failAt(mnemonic.line, quoted(mnemonic.text) + " is not a mnemonic");
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

  // ---- effects ---------------------------------------------------------------------

  // The effects between a `{` already read and its `}`, each ended by `;`.
  bool readEffects(std::vector<Effect>& effects) {
    for (;;) {
      const Token* t = next();
      if (t == nullptr) return fail("the file ends where an effect or `}` belongs");
      if (!t->quoted && t->text == "}") return true;
      --pos_;
      if (!readEffect(effects)) return false;
    }
  }

  // `<op> [<dst> <-] [<a>[, <b>]] [<width> [<step> [<access>]] [pinned|unpinned]] [<condition>] ;`
  bool readEffect(std::vector<Effect>& effects) {
    // The words up to the `;`.
    std::vector<const Token*> w;
    for (;;) {
      const Token* t = next();
      if (t == nullptr) return fail("the file ends before an effect's `;`");
      if (t->quoted) return failAt(t->line, "a string inside an effect");
      if (t->text == ";") break;
      if (t->text == "{" || t->text == "}") return failAt(t->line, quoted(t->text) + " before an effect's `;`");
      w.push_back(t);
    }
    if (w.empty()) return fail("an effect with no operation");
    const std::size_t line = w.front()->line;
    Effect effect;
    const std::optional<Op> op = named<Op>(kOps, w[0]->text);
    if (!op) return failAt(line, quoted(w[0]->text) + " is not an operation");
    effect.op = *op;

    // The bracket: from the word that opens with `[` to the word that closes with `]`.
    std::size_t open = w.size();
    std::size_t close = w.size();
    for (std::size_t i = 1; i < w.size(); ++i) {
      if (open == w.size() && w[i]->text.front() == '[') open = i;
      if (open != w.size() && w[i]->text.back() == ']') {
        close = i;
        break;
      }
    }
    if (open == w.size() || close == w.size()) return failAt(line, "an effect lacks its bracket");

    std::size_t i = 1;
    if (i + 1 < open && w[i + 1]->text == "<-") {
      if (!operand(*w[i], effect.dst)) return false;
      i += 2;
    }
    std::vector<const Token*> rest(w.begin() + static_cast<std::ptrdiff_t>(i), w.begin() + static_cast<std::ptrdiff_t>(open));
    if (!rest.empty()) {
      if (rest.size() == 1) {
        if (rest[0]->text.back() == ',') return failAt(line, "an effect's operands come in a pair or alone");
        if (!operand(*rest[0], effect.a)) return false;
      } else if (rest.size() == 2 && rest[0]->text.size() > 1 && rest[0]->text.back() == ',') {
        Token a = *rest[0];
        a.text.pop_back();
        if (!operand(a, effect.a)) return false;
        if (!operand(*rest[1], effect.b)) return false;
      } else {
        return failAt(line, "an effect's operands come in a pair or alone");
      }
    }

    std::vector<std::string> b;
    for (std::size_t k = open; k <= close; ++k) {
      std::string text = w[k]->text;
      if (k == open) text.erase(0, 1);
      if (k == close && !text.empty() && text.back() == ']') text.pop_back();
      if (!text.empty()) b.push_back(text);
    }
    if (b.empty()) return failAt(line, "an effect's bracket lacks its width");
    const std::optional<Width> width = named<Width>(kWidths, b[0]);
    if (!width) return failAt(line, quoted(b[0]) + " is not a width");
    effect.width = *width;
    bool stepGiven = false;
    bool pinGiven = false;
    for (std::size_t k = 1; k < b.size(); ++k) {
      if (const std::optional<Step> step = named<Step>(kSteps, b[k])) {
        if (!carriesStep(effect.op) || stepGiven) return failAt(line, "a step on an operation that carries none");
        effect.step = *step;
        stepGiven = true;
      } else if (const std::optional<Access> access = named<Access>(kAccesses, b[k], 1)) {
        if (!stepGiven || effect.access != Access::Data) return failAt(line, "an access kind that follows no step");
        effect.access = *access;
      } else if (b[k] == "pinned" || b[k] == "unpinned") {
        if (!carriesPin(effect.op) || pinGiven) return failAt(line, "a pin on an operation that carries none");
        effect.pinned = b[k] == "pinned";
        pinGiven = true;
      } else {
        return failAt(line, quoted(b[k]) + " is not a step, an access kind or a pin");
      }
    }
    if (carriesStep(effect.op) && !stepGiven) return failAt(line, "a load or store lacks its step");
    if (carriesPin(effect.op) && !pinGiven) return failAt(line, "a push or pull lacks its pin");

    if (close + 1 < w.size()) {
      std::string tail;
      for (std::size_t k = close + 1; k < w.size(); ++k) {
        if (!tail.empty()) tail += ' ';
        tail += w[k]->text;
      }
      if (!condition(line, tail, effect.when)) return false;
    }
    effects.push_back(std::move(effect));
    return true;
  }

  bool operand(const Token& t, Operand& out) {
    if (t.text.starts_with('$')) {
      const std::optional<std::uint32_t> v = value(t.text);
      if (!v) return failAt(t.line, quoted(t.text) + " is not a value");
      out = Operand{Place::Imm, *v};
      return true;
    }
    const std::optional<Place> p = place(t.text);
    if (!p) return failAt(t.line, quoted(t.text) + " is not a place");
    out = Operand{*p, 0};
    return true;
  }

  bool condition(std::size_t line, std::string_view text, Cond& when) {
    // The longest condition word that opens the text, then its fields.
    std::size_t best = 0;
    for (std::size_t i = 1; i < sizeof kWhens / sizeof kWhens[0]; ++i) {
      const std::string_view name = kWhens[i];
      if (text.starts_with(name) && (text.size() == name.size() || text[name.size()] == ' ') &&
          name.size() > kWhens[best].size()) {
        best = i;
      }
    }
    if (best == 0) return failAt(line, quoted(text) + " is not a condition");
    when.when = static_cast<When>(best);
    std::vector<std::string_view> f;
    {
      std::string_view rest = text.substr(kWhens[best].size());
      std::size_t i = 0;
      while (i < rest.size()) {
        while (i < rest.size() && rest[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < rest.size() && rest[i] != ' ') ++i;
        if (i > start) f.push_back(rest.substr(start, i - start));
      }
    }
    std::size_t i = 0;
    if (i < f.size() && f[i] != "and" && !f[i].starts_with('$')) {
      const std::optional<Place> p = place(f[i]);
      if (!p) return failAt(line, quoted(f[i]) + " is not a place");
      when.place = *p;
      ++i;
    }
    if (carriesValue(when.when)) {
      if (i >= f.size() || !f[i].starts_with('$')) return failAt(line, "`if is` lacks its value");
      const std::optional<std::uint32_t> v = value(f[i]);
      if (!v) return failAt(line, quoted(f[i]) + " is not a value");
      when.value = *v;
      ++i;
    } else if (i < f.size() && f[i].starts_with('$')) {
      return failAt(line, "a value on a condition that takes none");
    }
    if (i + 1 < f.size() && f[i] == "and" && f[i + 1] == "e") {
      when.andEmulation = true;
      i += 2;
    }
    if (i < f.size()) return failAt(line, quoted(f[i]) + " is not a field of a condition");
    return true;
  }

  // ---- the sequences ---------------------------------------------------------------

  bool readSequence(const char* name, std::vector<Effect>& effects) {
    if (!peekIs(name)) {
      if (pos_ >= tokens_.size()) return fail(std::string("the file ends before its ") + name + " sequence");
      const Token& t = tokens_[pos_];
      if (!t.quoted && (t.text == "label" || t.text == "data" || t.text[0] == '$')) {
        return failAt(t.line, "a " + std::string(t.text == "label" ? "label" : t.text == "data" ? "data run" : "node") +
                                  " outside any region");
      }
      return failAt(t.line, quoted(t.text) + " where `" + name + "` belongs");
    }
    ++pos_;
    if (!expect("{")) return false;
    return readEffects(effects);
  }

  std::string& error_;
  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
  std::size_t lastLine_ = 1;
  Parsed parsed_;
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
  line += " [";
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
    line += " if e";
  } else if (e.when.when != When::Always) {
    line += " ";
    line += whenName(e.when.when);
    if (e.when.place != Place::None) {
      line += " ";
      line += placeText(e.when.place);
    }
    if (carriesValue(e.when.when)) line += " " + hex(e.when.value);
    if (e.when.andEmulation) line += " and e";
  }
  return line + ";";
}

std::string renderNode(const Node& node) {
  const Instruction& i = node.instruction;
  std::string out = "  " + addressText(i.address) + " " + std::string(i.mnemonic);
  if (i.addressing != Addressing::Implied) out += " " + std::string(addressingName(i.addressing));
  out += " operand " + hex(i.operand);
  if (i.addressing == Addressing::BlockMove) out += " operand2 " + hex(i.operand2);
  out += " length " + std::to_string(i.length) + " flow " + std::string(flowName(i.flow));
  if (i.target) out += " target " + addressText(*i.target);
  out += " " + modeName(node.mode) + " base " + std::to_string(node.cost.base[0]) + "/" +
         std::to_string(node.cost.base[1]) + "/" + std::to_string(node.cost.base[2]) + "/" +
         std::to_string(node.cost.base[3]);
  if (!node.registerName.empty()) {
    out += " ";
    out += node.registerName;
  }
  if (node.patched) out += " patched";
  out += " {\n";
  for (const Effect& e : node.effects) out += "    " + renderEffect(e) + "\n";
  out += "  }\n";
  return out;
}

std::string renderProgram(const Program& program, const ProgramFile& file) {
  std::string out = "snagir 1;\nimage " + std::to_string(file.imageBytes) + " " + file.map + ";\n";
  std::size_t written = 0;
  char b[4];
  for (const ProgramRegion& region : file.regions) {
    out += "\nregion " + region.file + " " + addressText(region.first) + "-" + addressText(region.last) + " {\n";
    for (const std::string& warning : region.warnings) out += "  warning " + quotedText(warning) + ";\n";
    std::vector<const Node*> nodes;
    for (const Node& node : program.nodes) {
      const Address at = node.instruction.address;
      if (at >= region.first && at <= region.last) nodes.push_back(&node);
    }
    written += nodes.size();
    // The three sequences merged by address — a label, then a data run, then
    // the nodes, at one address.
    std::size_t l = 0;
    std::size_t d = 0;
    std::size_t n = 0;
    while (l < region.labels.size() || d < region.data.size() || n < nodes.size()) {
      const Address la = l < region.labels.size() ? region.labels[l].address : 0xFFFFFFFFu;
      const Address da = d < region.data.size() ? region.data[d].address : 0xFFFFFFFFu;
      const Address na = n < nodes.size() ? nodes[n]->instruction.address : 0xFFFFFFFFu;
      if (la <= da && la <= na) {
        out += "  label " + addressText(la) + " " + region.labels[l++].name + ";\n";
      } else if (da <= na) {
        const DataRun& run = region.data[d++];
        out += "  data " + addressText(run.address) + " ";
        for (const std::uint8_t byte : run.bytes) {
          std::snprintf(b, sizeof b, "%02X", byte);
          out += b;
        }
        out += ";\n";
      } else {
        out += renderNode(*nodes[n++]);
      }
    }
    out += "}\n";
  }
  if (written != program.nodes.size()) {
    throw std::invalid_argument("a node lies in no region of the program file");
  }
  out += "\nnmi {\n";
  for (const Effect& e : program.nmi) out += "  " + renderEffect(e) + "\n";
  out += "}\nirq {\n";
  for (const Effect& e : program.irq) out += "  " + renderEffect(e) + "\n";
  out += "}\n";
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
