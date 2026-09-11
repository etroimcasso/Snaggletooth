#include "text.h"

#include <cctype>
#include <sstream>

namespace snaggletooth::formats::text {

namespace {

std::optional<unsigned> hexValue(std::string_view digits) {
  if (digits.empty()) return std::nullopt;
  unsigned value = 0;
  for (char c : digits) {
    unsigned d;
    if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
    else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
    else return std::nullopt;
    value = value * 16u + d;
  }
  return value;
}

}  // namespace

std::string hex(unsigned value, int width) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out(static_cast<std::size_t>(width), '0');
  for (int i = width - 1; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
    value >>= 4;
  }
  return out;
}

std::optional<unsigned> parseDollarHex(std::string_view token) {
  if (token.size() < 2 || token.front() != '$') return std::nullopt;
  return hexValue(token.substr(1));
}

std::optional<unsigned> parseHex(std::string_view token) { return hexValue(token); }

std::string_view stripComment(std::string_view line) {
  const auto pos = line.find(';');
  return pos == std::string_view::npos ? line : line.substr(0, pos);
}

std::vector<std::string> tokens(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words{std::string(stripComment(line))};
    std::string word;
    while (words >> word) out.push_back(word);
  }
  return out;
}

}  // namespace snaggletooth::formats::text
