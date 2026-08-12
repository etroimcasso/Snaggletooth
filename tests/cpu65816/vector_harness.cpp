#include "vector_harness.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace snaggletooth::cpu_vectors {
namespace {

// A recursive-descent reader over the fixed vector schema: an array of objects,
// each { name, initial, final, cycles }. It understands objects, arrays, strings,
// signed integers, and the null literal — the subset the vectors use.
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

  // Skips any value, so the cycle array can be counted without reading its entries.
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

  std::vector<std::pair<std::uint32_t, std::uint8_t>> ramPairs() {
    std::vector<std::pair<std::uint32_t, std::uint8_t>> pairs;
    want('[');
    if (!accept(']')) {
      do {
        want('[');
        long address = integer();
        want(',');
        long value = integer();
        want(']');
        pairs.emplace_back(static_cast<std::uint32_t>(address),
                           static_cast<std::uint8_t>(value));
      } while (accept(','));
      want(']');
    }
    return pairs;
  }

  // A cycle entry is [address, value, signals]; either of the first two is null on
  // a cycle that drove no address or moved no byte.
  std::vector<CycleTrace> cycleTraces() {
    std::vector<CycleTrace> traces;
    want('[');
    if (!accept(']')) {
      do {
        CycleTrace trace;
        want('[');
        if (isNull()) {
          takeNull();
        } else {
          trace.address = static_cast<std::uint32_t>(integer());
        }
        want(',');
        if (isNull()) {
          takeNull();
        } else {
          trace.value = static_cast<std::uint8_t>(integer());
        }
        want(',');
        trace.signals = str();
        want(']');
        traces.push_back(std::move(trace));
      } while (accept(','));
      want(']');
    }
    return traces;
  }

  bool isNull() { return cur() == 'n'; }
  void takeNull() {
    if (end - p < 4 || std::strncmp(p, "null", 4) != 0) fail("bad literal");
    p += 4;
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
        } else if (key == "s") {
          r.s = static_cast<std::uint16_t>(integer());
        } else if (key == "a") {
          r.a = static_cast<std::uint16_t>(integer());
        } else if (key == "x") {
          r.x = static_cast<std::uint16_t>(integer());
        } else if (key == "y") {
          r.y = static_cast<std::uint16_t>(integer());
        } else if (key == "d") {
          r.d = static_cast<std::uint16_t>(integer());
        } else if (key == "p") {
          r.p = static_cast<std::uint8_t>(integer());
        } else if (key == "dbr") {
          r.dbr = static_cast<std::uint8_t>(integer());
        } else if (key == "pbr") {
          r.pbr = static_cast<std::uint8_t>(integer());
        } else if (key == "e") {
          r.e = integer() != 0;
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
          c.cycles = cycleTraces();
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

std::string signalsFor(CycleKind kind, bool e, bool m, bool x) {
  std::string s(8, '-');
  switch (kind) {
    case CycleKind::OpcodeFetch:  // both program and data addresses are valid
      s[kSignalVda] = 'd';
      s[kSignalVpa] = 'p';
      break;
    case CycleKind::OperandFetch:  // a program address only
      s[kSignalVpa] = 'p';
      break;
    case CycleKind::DataRead:
    case CycleKind::DataWrite:
      s[kSignalVda] = 'd';
      break;
    case CycleKind::RmwRead:  // a data address, held under the memory lock
    case CycleKind::RmwWrite:
      s[kSignalVda] = 'd';
      s[kSignalMlb] = 'l';
      break;
    case CycleKind::VectorRead:  // a data address, with the vector pull asserted
      s[kSignalVda] = 'd';
      s[kSignalVpb] = 'v';
      break;
  }
  s[kSignalRw] = (kind == CycleKind::DataWrite || kind == CycleKind::RmwWrite) ? 'w' : 'r';
  if (e) s[kSignalE] = 'e';
  if (m) s[kSignalM] = 'm';
  if (x) s[kSignalX] = 'x';
  return s;
}

std::string internalSignals(bool e, bool m, bool x) {
  std::string s(8, '-');
  s[kSignalRw] = 'r';
  if (e) s[kSignalE] = 'e';
  if (m) s[kSignalM] = 'm';
  if (x) s[kSignalX] = 'x';
  return s;
}

std::string RecordingBus::liveSignals(std::optional<CycleKind> kind) const {
  // Without a processor to read, the widths are unknown and only the access kind is
  // reported — enough for a test that does not compare against a recorded trace.
  const bool e = cpu != nullptr && cpu->state().e;
  const bool m = cpu != nullptr && (e || (cpu->state().p & kCpuFlagM) != 0);
  const bool x = cpu != nullptr && (e || (cpu->state().p & kCpuFlagX) != 0);
  return kind.has_value() ? signalsFor(*kind, e, m, x) : internalSignals(e, m, x);
}

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

}  // namespace snaggletooth::cpu_vectors
