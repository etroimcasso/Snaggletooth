#include "assembler/assembler.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

namespace snaggletooth::assembler {
namespace {

// ---- characters ----------------------------------------------------------------
constexpr bool isSpace(char c) noexcept { return c == ' ' || c == '\t'; }
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isAlpha(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
constexpr bool isHexDigit(char c) noexcept {
  return isDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
constexpr bool isNameStart(char c) noexcept { return isAlpha(c) || c == '_' || c == '.'; }
constexpr bool isNameChar(char c) noexcept { return isNameStart(c) || isDigit(c); }
constexpr char toUpper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}
constexpr unsigned hexValue(char c) noexcept {
  if (isDigit(c)) return static_cast<unsigned>(c - '0');
  return static_cast<unsigned>(toUpper(c) - 'A' + 10);
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
  return text;
}

// The directives every dialect has.
bool coreDirective(std::string_view upperName) {
  return upperName == "ORG" || upperName == "DB" || upperName == "DW" || upperName == "DL" ||
         upperName == "DS" || upperName == "EQU";
}

// ---- literals --------------------------------------------------------------------
// Reads the character at `pos` inside a quoted literal, resolving an escape.
// Advances `pos` past what it read.
bool readEscaped(std::string_view text, std::size_t& pos, char& out, std::string& error) {
  if (pos >= text.size()) {
    error = "the literal is not closed";
    return false;
  }
  if (text[pos] != '\\') {
    out = text[pos++];
    return true;
  }
  if (pos + 1 >= text.size()) {
    error = "the literal ends inside an escape";
    return false;
  }
  const char code = text[pos + 1];
  pos += 2;
  switch (code) {
    case '\\': out = '\\'; return true;
    case '\'': out = '\''; return true;
    case '"': out = '"'; return true;
    case 'n': out = '\n'; return true;
    case 'r': out = '\r'; return true;
    case 't': out = '\t'; return true;
    case '0': out = '\0'; return true;
    default:
      error = std::string("`\\") + code + "` is not an escape; the escapes are \\\\ \\' \\\" \\n \\r \\t \\0";
      return false;
  }
}

// ---- expressions -----------------------------------------------------------------
// One term of an expression, read at `pos`.
std::optional<Value> readTerm(std::string_view text, std::size_t& pos,
                              const std::map<std::string, std::uint32_t>& symbols, Address here,
                              unsigned bits, bool firstPass, std::string& error) {
  if (pos >= text.size()) {
    error = "expected an expression";
    return std::nullopt;
  }
  const char c = text[pos];

  if (c == '$') {
    ++pos;
    const std::size_t start = pos;
    std::uint64_t value = 0;
    while (pos < text.size() && isHexDigit(text[pos])) {
      value = (value << 4) | hexValue(text[pos]);
      if (value > 0xFFFFFFFFu) {
        error = "the number is too large";
        return std::nullopt;
      }
      ++pos;
    }
    if (pos == start) {
      error = "`$` needs hexadecimal digits after it";
      return std::nullopt;
    }
    if (pos < text.size() && text[pos] == ':') {
      if (bits != 24) {
        error = "a bank separator belongs to the 24-bit dialect; addresses here are 16 bits";
        return std::nullopt;
      }
      ++pos;
      const std::size_t offsetStart = pos;
      std::uint64_t offset = 0;
      while (pos < text.size() && isHexDigit(text[pos])) {
        offset = (offset << 4) | hexValue(text[pos]);
        if (offset > 0xFFFFu) {
          error = "the offset after the bank separator must fit in 16 bits";
          return std::nullopt;
        }
        ++pos;
      }
      if (pos == offsetStart) {
        error = "the bank separator needs hexadecimal digits after it";
        return std::nullopt;
      }
      if (value > 0xFFu) {
        error = "the bank before the separator must fit in 8 bits";
        return std::nullopt;
      }
      return Value{.value = static_cast<std::uint32_t>((value << 16) | offset), .resolved = true};
    }
    return Value{.value = static_cast<std::uint32_t>(value), .resolved = true};
  }

  if (c == '%') {
    ++pos;
    const std::size_t start = pos;
    std::uint64_t value = 0;
    while (pos < text.size() && (text[pos] == '0' || text[pos] == '1')) {
      value = (value << 1) | static_cast<unsigned>(text[pos] - '0');
      if (value > 0xFFFFFFFFu) {
        error = "the number is too large";
        return std::nullopt;
      }
      ++pos;
    }
    if (pos == start) {
      error = "`%` needs binary digits after it";
      return std::nullopt;
    }
    return Value{.value = static_cast<std::uint32_t>(value), .resolved = true};
  }

  if (isDigit(c)) {
    if (c == '0' && pos + 1 < text.size() && (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
      error = "`0x` is not a number form; hexadecimal is written with `$`";
      return std::nullopt;
    }
    std::uint64_t value = 0;
    while (pos < text.size() && isDigit(text[pos])) {
      value = value * 10 + static_cast<unsigned>(text[pos] - '0');
      if (value > 0xFFFFFFFFu) {
        error = "the number is too large";
        return std::nullopt;
      }
      ++pos;
    }
    return Value{.value = static_cast<std::uint32_t>(value), .resolved = true};
  }

  if (c == '\'') {
    ++pos;
    char character = 0;
    if (!readEscaped(text, pos, character, error)) return std::nullopt;
    if (pos >= text.size() || text[pos] != '\'') {
      error = "a character literal is one character between single quotes";
      return std::nullopt;
    }
    ++pos;
    return Value{.value = static_cast<std::uint8_t>(character), .resolved = true};
  }

  if (c == '*') {
    ++pos;
    return Value{.value = here, .resolved = true};
  }

  if (isNameStart(c)) {
    const std::size_t start = pos;
    while (pos < text.size() && isNameChar(text[pos])) ++pos;
    const std::string name(text.substr(start, pos - start));
    const auto found = symbols.find(name);
    if (found != symbols.end()) return Value{.value = found->second, .resolved = true};
    if (firstPass) return Value{.value = 0, .resolved = false};
    error = "`" + name + "` is not defined";
    return std::nullopt;
  }

  error = std::string("expected a number, a symbol, a character or `*`, not `") + c + "`";
  return std::nullopt;
}

std::optional<Value> evaluateExpression(std::string_view text,
                                        const std::map<std::string, std::uint32_t>& symbols,
                                        Address here, unsigned bits, bool firstPass,
                                        std::string& error) {
  const std::uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
  std::size_t pos = 0;
  auto skipSpaces = [&]() {
    while (pos < text.size() && isSpace(text[pos])) ++pos;
  };
  skipSpaces();
  std::optional<Value> left = readTerm(text, pos, symbols, here, bits, firstPass, error);
  if (!left) return std::nullopt;
  Value result{.value = left->value & mask, .resolved = left->resolved};
  for (;;) {
    skipSpaces();
    if (pos >= text.size()) break;
    const char op = text[pos];
    if (op != '+' && op != '-') {
      error = std::string("unexpected `") + op + "` in the expression";
      return std::nullopt;
    }
    ++pos;
    skipSpaces();
    std::optional<Value> right = readTerm(text, pos, symbols, here, bits, firstPass, error);
    if (!right) return std::nullopt;
    result.value = (op == '+' ? result.value + right->value : result.value - right->value) & mask;
    result.resolved = result.resolved && right->resolved;
  }
  return result;
}

// ---- lines -----------------------------------------------------------------------
// `line` with its comment removed. A semicolon inside a literal is not a comment.
std::string_view stripComment(std::string_view line) {
  bool single = false;
  bool doubled = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\\' && (single || doubled)) {
      ++i;
      continue;
    }
    if (c == '"' && !single) doubled = !doubled;
    else if (c == '\'' && !doubled) single = !single;
    else if (c == ';' && !single && !doubled) return line.substr(0, i);
  }
  return line;
}

// One line, taken apart.
struct Statement {
  std::string label;     // the label defined on the line, if any
  bool equ = false;      // the line defines `label` with EQU; `operands` is the value
  std::string mnemonic;  // the instruction or directive, upper-cased; empty when none
  std::string operands;  // the rest of the line, trimmed, as written
  std::string error;     // what was wrong with the line's form, if anything
};

Statement parseStatement(std::string_view raw) {
  Statement out;
  std::string_view text = stripComment(raw);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
  if (text.empty()) return out;

  if (!isSpace(text.front())) {
    if (!isNameStart(text.front())) {
      out.error = std::string("a line begins with a label in column 1 or is indented; `") +
                  text.front() + "` is neither";
      return out;
    }
    std::size_t i = 0;
    while (i < text.size() && isNameChar(text[i])) ++i;
    const std::string name(text.substr(0, i));
    if (i < text.size() && text[i] == ':') {
      out.label = name;
      text = text.substr(i + 1);
    } else {
      std::string_view rest = trim(text.substr(i));
      std::size_t j = 0;
      while (j < rest.size() && !isSpace(rest[j])) ++j;
      if (upper(rest.substr(0, j)) == "EQU") {
        out.label = name;
        out.equ = true;
        out.operands = std::string(trim(rest.substr(j)));
        return out;
      }
      out.error = "`" + name + "` in column 1 needs a colon after it to be a label; an instruction "
                  "or a directive is indented";
      return out;
    }
  }

  text = trim(text);
  if (text.empty()) return out;
  std::size_t i = 0;
  while (i < text.size() && !isSpace(text[i])) ++i;
  if (text[i - 1] == ':') {
    out.error = "`" + std::string(text.substr(0, i)) + "` is indented; a label begins in column 1";
    return out;
  }
  out.mnemonic = upper(text.substr(0, i));
  out.operands = std::string(trim(text.substr(i)));
  return out;
}

// The items of a data directive's operand list, split at the commas outside
// literals and trimmed.
std::vector<std::string_view> splitItems(std::string_view text) {
  std::vector<std::string_view> items;
  bool single = false;
  bool doubled = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\' && (single || doubled)) {
      ++i;
      continue;
    }
    if (c == '"' && !single) doubled = !doubled;
    else if (c == '\'' && !doubled) single = !single;
    else if (c == ',' && !single && !doubled) {
      items.push_back(trim(text.substr(start, i - start)));
      start = i + 1;
    }
  }
  items.push_back(trim(text.substr(start)));
  return items;
}

// ---- the two passes ---------------------------------------------------------------
class Pass final : public Evaluator {
 public:
  Pass(const std::map<std::string, std::uint32_t>& symbols, unsigned bits, bool first)
      : symbols_(symbols), bits_(bits), first_(first) {}

  void moveTo(Address here) { here_ = here; }

  [[nodiscard]] std::optional<Value> evaluate(std::string_view text,
                                              std::string& error) const override {
    return evaluateExpression(text, symbols_, here_, bits_, first_, error);
  }
  [[nodiscard]] bool firstPass() const override { return first_; }

 private:
  const std::map<std::string, std::uint32_t>& symbols_;
  unsigned bits_;
  bool first_;
  Address here_ = 0;
};

}  // namespace

std::string upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = toUpper(c);
  return out;
}

std::string compact(std::string_view text) {
  std::string out;
  bool single = false;
  bool doubled = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\' && (single || doubled) && i + 1 < text.size()) {
      out += c;
      out += text[++i];
      continue;
    }
    if (c == '"' && !single) doubled = !doubled;
    else if (c == '\'' && !doubled) single = !single;
    if (isSpace(c) && !single && !doubled) continue;
    out += c;
  }
  return out;
}

std::size_t expressionEnd(std::string_view text, std::size_t from) {
  std::size_t pos = from;
  auto term = [&]() -> bool {
    if (pos >= text.size()) return false;
    const char c = text[pos];
    if (c == '$') {
      ++pos;
      while (pos < text.size() && isHexDigit(text[pos])) ++pos;
      if (pos < text.size() && text[pos] == ':') {
        ++pos;
        while (pos < text.size() && isHexDigit(text[pos])) ++pos;
      }
      return true;
    }
    if (c == '%') {
      ++pos;
      while (pos < text.size() && (text[pos] == '0' || text[pos] == '1')) ++pos;
      return true;
    }
    if (isDigit(c)) {
      while (pos < text.size() && isNameChar(text[pos])) ++pos;
      return true;
    }
    if (c == '\'') {
      ++pos;
      if (pos < text.size() && text[pos] == '\\') ++pos;
      if (pos < text.size()) ++pos;
      if (pos < text.size() && text[pos] == '\'') ++pos;
      return true;
    }
    if (c == '*') {
      ++pos;
      return true;
    }
    if (isNameStart(c)) {
      while (pos < text.size() && isNameChar(text[pos])) ++pos;
      return true;
    }
    return false;
  };
  if (!term()) return from;
  for (;;) {
    if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) break;
    if (text[pos] == '+' && pos + 1 < text.size()) {
      const char reg = toUpper(text[pos + 1]);
      if ((reg == 'X' || reg == 'Y') &&
          (pos + 2 >= text.size() || !isNameChar(text[pos + 2]))) {
        break;
      }
    }
    const std::size_t before = pos;
    ++pos;
    if (!term()) {
      pos = before;
      break;
    }
  }
  return pos;
}

std::string hex(std::uint32_t value, unsigned digits) {
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "$%0*X", static_cast<int>(digits), value);
  return buffer;
}

Assembly assemble(Dialect& dialect, std::string_view source, std::string_view file) {
  Assembly out;
  const unsigned bits = dialect.addressBits();
  const std::uint64_t spaceEnd = std::uint64_t{1} << bits;

  // The lines, taken apart once.
  std::vector<Statement> statements;
  {
    std::size_t start = 0;
    while (start <= source.size()) {
      std::size_t end = source.find('\n', start);
      if (end == std::string_view::npos) end = source.size();
      std::string_view line = source.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      statements.push_back(parseStatement(line));
      if (end == source.size()) break;
      start = end + 1;
    }
  }

  std::map<std::string, std::uint32_t> symbols;
  std::set<std::pair<unsigned, std::string>> seen;
  auto report = [&](std::size_t index, std::string message) {
    const unsigned line = static_cast<unsigned>(index + 1);
    if (!seen.insert({line, message}).second) return;
    out.errors.push_back(Diagnostic{.file = std::string(file), .line = line,
                                    .message = std::move(message)});
  };

  for (int pass = 1; pass <= 2; ++pass) {
    const bool first = pass == 1;
    Pass evaluator(symbols, bits, first);
    Address address = 0;
    dialect.beginRegion();
    std::vector<Range> ranges;
    bool open = false;  // whether `ranges.back()` is the range being appended to

    // Emits bytes at the address, on the second pass into the ranges.
    auto emit = [&](std::size_t index, const std::vector<std::uint8_t>& bytes) {
      if (bytes.empty()) return;
      if (address + bytes.size() > spaceEnd) {
        report(index, "the bytes run past the end of the " + std::to_string(bits) +
                          "-bit address space");
        address = static_cast<Address>(spaceEnd - 1);
        return;
      }
      if (!first) {
        const Address end = address + static_cast<Address>(bytes.size());
        for (std::size_t i = 0; i < ranges.size(); ++i) {
          if (open && i + 1 == ranges.size()) continue;
          const Range& other = ranges[i];
          const Address otherEnd = other.start + static_cast<Address>(other.bytes.size());
          if (address < otherEnd && other.start < end) {
            report(index, "overlaps bytes already emitted at " +
                              hex(std::max(address, other.start), bits / 4));
            break;
          }
        }
        if (!open) {
          ranges.push_back(Range{.start = address, .bytes = {}});
          open = true;
        }
        ranges.back().bytes.insert(ranges.back().bytes.end(), bytes.begin(), bytes.end());
      }
      address += static_cast<Address>(bytes.size());
    };

    // A value that must be known when its line is read, first pass included.
    auto known = [&](std::size_t index, std::string_view text, const char* what)
        -> std::optional<std::uint32_t> {
      std::string error;
      const std::optional<Value> value = evaluator.evaluate(text, error);
      if (!value) {
        report(index, std::string(what) + ": " + error);
        return std::nullopt;
      }
      if (!value->resolved) {
        report(index, std::string(what) + " must be known when it is read; `" +
                          std::string(trim(text)) + "` is defined later in the file");
        return std::nullopt;
      }
      return value->value;
    };

    for (std::size_t index = 0; index < statements.size(); ++index) {
      const Statement& statement = statements[index];
      if (!statement.error.empty()) {
        report(index, statement.error);
        continue;
      }
      evaluator.moveTo(address);

      if (statement.equ) {
        if (first) {
          if (coreDirective(upper(statement.label)) || dialect.reserved(upper(statement.label))) {
            report(index, "`" + statement.label + "` is a mnemonic, a register or a directive, "
                          "and cannot be a name");
          } else if (symbols.count(statement.label) != 0) {
            report(index, "`" + statement.label + "` is already defined");
          } else if (const std::optional<std::uint32_t> value =
                         known(index, statement.operands, "an EQU value")) {
            symbols[statement.label] = *value;
          }
        }
        continue;
      }

      if (!statement.label.empty()) {
        if (first) {
          if (coreDirective(upper(statement.label)) || dialect.reserved(upper(statement.label))) {
            report(index, "`" + statement.label + "` is a mnemonic, a register or a directive, "
                          "and cannot be a label");
          } else if (symbols.count(statement.label) != 0) {
            report(index, "`" + statement.label + "` is already defined");
          } else {
            symbols[statement.label] = address;
          }
        } else if (const auto found = symbols.find(statement.label);
                   found != symbols.end() && found->second != address) {
          report(index, "`" + statement.label + "` moved between passes: an instruction above "
                        "it changed length once a symbol was known");
        }
      }
      if (statement.mnemonic.empty()) continue;

      const std::string& mnemonic = statement.mnemonic;
      const std::string& operands = statement.operands;

      if (mnemonic == "ORG") {
        const std::optional<std::uint32_t> value = known(index, operands, "an ORG address");
        if (!value) continue;
        if (*value >= spaceEnd) {
          report(index, "ORG " + hex(*value, 6) + " lies outside the " + std::to_string(bits) +
                            "-bit address space");
          continue;
        }
        // An ORG to exactly the next address continues the range being written;
        // any other starts a new one.
        if (!(open && ranges.back().start + ranges.back().bytes.size() == *value)) open = false;
        address = *value;
        dialect.beginRegion();
        continue;
      }

      if (mnemonic == "DB" || mnemonic == "DW" || mnemonic == "DL") {
        if (mnemonic == "DL" && bits != 24) {
          report(index, "DL exists only in a dialect whose addresses are 24 bits wide");
          continue;
        }
        if (trim(operands).empty()) {
          report(index, mnemonic + " needs at least one value");
          continue;
        }
        const unsigned width = mnemonic == "DB" ? 8 : mnemonic == "DW" ? 16 : 24;
        std::vector<std::uint8_t> bytes;
        for (std::string_view item : splitItems(operands)) {
          if (item.empty()) {
            report(index, "an empty item in the list");
            continue;
          }
          if (item.front() == '"') {
            if (width != 8) {
              report(index, "a string is a run of bytes and belongs to DB");
              continue;
            }
            std::size_t pos = 1;
            std::string error;
            bool closed = false;
            while (pos < item.size()) {
              if (item[pos] == '"') {
                closed = true;
                ++pos;
                break;
              }
              char character = 0;
              if (!readEscaped(item, pos, character, error)) break;
              bytes.push_back(static_cast<std::uint8_t>(character));
            }
            if (!error.empty()) {
              report(index, error);
            } else if (!closed) {
              report(index, "the string is not closed");
            } else if (pos != item.size()) {
              report(index, "text after the closing quote of a string");
            }
            continue;
          }
          std::string error;
          const std::optional<Value> value = evaluator.evaluate(item, error);
          if (!value) {
            report(index, error);
            continue;
          }
          if (value->resolved && !fits(value->value, width)) {
            report(index, hex(value->value, bits / 4) + " does not fit in " +
                              (width == 8 ? "a byte" : width == 16 ? "a word" : "24 bits"));
          }
          for (unsigned shift = 0; shift < width; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>((value->value >> shift) & 0xFFu));
          }
        }
        emit(index, bytes);
        dialect.beginRegion();
        continue;
      }

      if (mnemonic == "DS") {
        const std::vector<std::string_view> items = splitItems(operands);
        if (items.empty() || items.front().empty() || items.size() > 2) {
          report(index, "DS takes a count and an optional fill byte");
          continue;
        }
        const std::optional<std::uint32_t> count = known(index, items.front(), "a DS count");
        if (!count) continue;
        std::uint8_t fill = 0;
        if (items.size() == 2) {
          std::string error;
          const std::optional<Value> value = evaluator.evaluate(items[1], error);
          if (!value) {
            report(index, error);
            continue;
          }
          if (value->resolved && !fits(value->value, 8)) {
            report(index, "the fill " + hex(value->value, bits / 4) + " does not fit in a byte");
          }
          fill = static_cast<std::uint8_t>(value->value & 0xFFu);
        }
        emit(index, std::vector<std::uint8_t>(*count, fill));
        dialect.beginRegion();
        continue;
      }

      if (mnemonic == "EQU") {
        report(index, "EQU defines a name written in column 1: `NAME EQU value`");
        continue;
      }

      std::string error;
      if (dialect.directive(mnemonic, operands, evaluator, error)) {
        if (!error.empty()) report(index, error);
        continue;
      }

      Encoded encoded = dialect.encode(mnemonic, operands, address, evaluator);
      if (!encoded.error.empty()) report(index, std::move(encoded.error));
      emit(index, encoded.bytes);
    }

    if (!first) out.ranges = std::move(ranges);
  }

  std::sort(out.errors.begin(), out.errors.end(),
            [](const Diagnostic& a, const Diagnostic& b) { return a.line < b.line; });
  if (!out.errors.empty()) out.ranges.clear();
  std::sort(out.ranges.begin(), out.ranges.end(),
            [](const Range& a, const Range& b) { return a.start < b.start; });
  out.symbols = std::move(symbols);
  return out;
}

std::optional<std::vector<std::uint8_t>> image(const Assembly& assembly, Address base,
                                               std::size_t size, std::uint8_t fill) {
  std::vector<std::uint8_t> out(size, fill);
  for (const Range& range : assembly.ranges) {
    if (range.start < base) return std::nullopt;
    const std::size_t offset = static_cast<std::size_t>(range.start - base);
    if (offset + range.bytes.size() > size) return std::nullopt;
    std::copy(range.bytes.begin(), range.bytes.end(),
              out.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return out;
}

}  // namespace snaggletooth::assembler
