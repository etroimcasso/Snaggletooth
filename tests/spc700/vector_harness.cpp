#include "vector_harness.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace snaggletooth::test {
namespace {

// A recursive-descent reader over the fixed vector schema: an array of objects,
// each { name, initial, final, cycles }. It understands objects, arrays, strings,
// non-negative integers, and the null literal — the subset the vectors use.
struct Parser {
  const char* p;
  const char* end;

  [[noreturn]] void fail(const char* message) const {
    throw std::runtime_error(std::string("vector JSON: ") + message);
  }

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  char cur() {
    ws();
    if (p >= end) fail("unexpected end of input");
    return *p;
  }
  void want(char c) {
    if (cur() != c) fail("unexpected character");
    ++p;
  }
  bool accept(char c) {
    if (p < end && cur() == c) {
      ++p;
      return true;
    }
    return false;
  }

  std::string str() {
    want('"');
    std::string s;
    while (true) {
      if (p >= end) fail("unterminated string");
      char c = *p++;
      if (c == '"') break;
      if (c == '\\') {
        if (p >= end) fail("dangling escape");
        char e = *p++;
        switch (e) {
          case '"': s += '"'; break;
          case '\\': s += '\\'; break;
          case '/': s += '/'; break;
          case 'n': s += '\n'; break;
          case 't': s += '\t'; break;
          case 'r': s += '\r'; break;
          case 'b': s += '\b'; break;
          case 'f': s += '\f'; break;
          case 'u':
            for (int i = 0; i < 4; ++i) {
              if (p >= end) fail("truncated \\u escape");
              ++p;
            }
            s += '?';
            break;
          default: fail("invalid escape");
        }
      } else {
        s += c;
      }
    }
    return s;
  }

  long integer() {
    ws();
    bool neg = false;
    if (p < end && *p == '-') {
      neg = true;
      ++p;
    }
    if (p >= end || std::isdigit(static_cast<unsigned char>(*p)) == 0) {
      fail("expected a digit");
    }
    long v = 0;
    while (p < end && std::isdigit(static_cast<unsigned char>(*p)) != 0) {
      v = v * 10 + (*p - '0');
      ++p;
    }
    return neg ? -v : v;
  }

  // Skips any value — used for the keys the reader does not map.
  void skipValue() {
    char c = cur();
    if (c == '{') {
      want('{');
      if (!accept('}')) {
        do {
          str();
          want(':');
          skipValue();
        } while (accept(','));
        want('}');
      }
    } else if (c == '[') {
      want('[');
      if (!accept(']')) {
        do {
          skipValue();
        } while (accept(','));
        want(']');
      }
    } else if (c == '"') {
      str();
    } else if (c == 'n') {
      if (end - p < 4 || std::strncmp(p, "null", 4) != 0) fail("bad literal");
      p += 4;
    } else if (c == 't') {
      if (end - p < 4 || std::strncmp(p, "true", 4) != 0) fail("bad literal");
      p += 4;
    } else if (c == 'f') {
      if (end - p < 5 || std::strncmp(p, "false", 5) != 0) fail("bad literal");
      p += 5;
    } else {
      integer();
    }
  }

  std::vector<std::pair<std::uint16_t, std::uint8_t>> ramPairs() {
    std::vector<std::pair<std::uint16_t, std::uint8_t>> pairs;
    want('[');
    if (!accept(']')) {
      do {
        want('[');
        long address = integer();
        want(',');
        long value = integer();
        want(']');
        pairs.emplace_back(static_cast<std::uint16_t>(address),
                           static_cast<std::uint8_t>(value));
      } while (accept(','));
      want(']');
    }
    return pairs;
  }

  // A number or the null literal: a cycle that reaches memory not at all leaves both
  // its address and its value out, and a read the recording did not capture a byte for
  // leaves out the value alone.
  std::optional<long> integerOrNull() {
    if (cur() == 'n') {
      if (end - p < 4 || std::strncmp(p, "null", 4) != 0) fail("bad literal");
      p += 4;
      return std::nullopt;
    }
    return integer();
  }

  std::vector<CycleEvent> cycleEvents() {
    std::vector<CycleEvent> events;
    want('[');
    if (!accept(']')) {
      do {
        CycleEvent event;
        want('[');
        if (const auto address = integerOrNull(); address.has_value()) {
          event.address = static_cast<std::uint16_t>(*address);
        }
        want(',');
        if (const auto value = integerOrNull(); value.has_value()) {
          event.value = static_cast<std::uint8_t>(*value);
        }
        want(',');
        const std::string kind = str();
        if (kind == "read") {
          event.kind = CycleEvent::Kind::Read;
        } else if (kind == "write") {
          event.kind = CycleEvent::Kind::Write;
        } else if (kind == "wait") {
          event.kind = CycleEvent::Kind::Wait;
        } else {
          fail("unknown cycle kind");
        }
        want(']');
        events.push_back(event);
      } while (accept(','));
      want(']');
    }
    return events;
  }

  RegState regs() {
    RegState r;
    want('{');
    if (!accept('}')) {
      do {
        std::string key = str();
        want(':');
        if (key == "pc") {
          r.pc = static_cast<std::uint16_t>(integer());
        } else if (key == "a") {
          r.a = static_cast<std::uint8_t>(integer());
        } else if (key == "x") {
          r.x = static_cast<std::uint8_t>(integer());
        } else if (key == "y") {
          r.y = static_cast<std::uint8_t>(integer());
        } else if (key == "sp") {
          r.sp = static_cast<std::uint8_t>(integer());
        } else if (key == "psw") {
          r.psw = static_cast<std::uint8_t>(integer());
        } else if (key == "ram") {
          r.ram = ramPairs();
        } else {
          skipValue();
        }
      } while (accept(','));
      want('}');
    }
    return r;
  }

  VectorCase testCase() {
    VectorCase c;
    want('{');
    if (!accept('}')) {
      do {
        std::string key = str();
        want(':');
        if (key == "name") {
          c.name = str();
        } else if (key == "initial") {
          c.initial = regs();
        } else if (key == "final") {
          c.final_ = regs();
        } else if (key == "cycles") {
          c.cycles = cycleEvents();
        } else {
          skipValue();
        }
      } while (accept(','));
      want('}');
    }
    return c;
  }

  std::vector<VectorCase> file() {
    std::vector<VectorCase> cases;
    want('[');
    if (!accept(']')) {
      do {
        cases.push_back(testCase());
      } while (accept(','));
      want(']');
    }
    ws();
    if (p != end) fail("trailing content after top-level array");
    return cases;
  }
};

}  // namespace

std::vector<VectorCase> parseVectorFile(const std::string& text) {
  Parser parser{text.data(), text.data() + text.size()};
  return parser.file();
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

}  // namespace snaggletooth::test
