#include "tilemap.h"

#include <string>

#include "text.h"

namespace snaggletooth::formats {

namespace {

constexpr unsigned kEntriesPerRow = 32;
constexpr unsigned kRowsPerScreen = 32;

constexpr unsigned kPriority = 1u << 13;
constexpr unsigned kXFlip = 1u << 14;
constexpr unsigned kYFlip = 1u << 15;

std::string entryText(unsigned word) {
  std::string out;
  out += text::hex(word & 0x3FFu, 3);
  out += ':';
  out += std::to_string((word >> 10) & 0x7u);
  out += ':';
  std::string flags;
  if (word & kPriority) flags += 'P';
  if (word & kXFlip) flags += 'H';
  if (word & kYFlip) flags += 'V';
  out += flags.empty() ? "-" : flags;
  return out;
}

}  // namespace

Text encodeTilemap(std::span<const std::uint8_t> map) {
  Text out;
  std::string body;
  const std::size_t words = map.size() / 2;
  for (std::size_t i = 0; i < words; ++i) {
    const unsigned word =
        static_cast<unsigned>(map[i * 2]) | (static_cast<unsigned>(map[i * 2 + 1]) << 8);
    body += entryText(word);
    const std::size_t next = i + 1;
    if (next % kEntriesPerRow == 0) {
      body += '\n';
      if (next % (kEntriesPerRow * kRowsPerScreen) == 0 && next < words) body += '\n';
    } else {
      body += ' ';
    }
  }
  if (words % kEntriesPerRow != 0) body += '\n';
  if (map.size() % 2 != 0) {
    body += '$';
    body += text::hex(map.back(), 2);
    body += '\n';
  }
  out.text = std::move(body);
  return out;
}

Bytes decodeTilemap(const std::string& text) {
  Bytes out;
  for (const std::string& token : text::tokens(text)) {
    if (!token.empty() && token.front() == '$') {  // a lone trailing byte
      const auto value = text::parseDollarHex(token);
      if (!value || token.size() != 3) {
        out.error = "tilemap trailing byte is not $XX: " + token;
        out.bytes.clear();
        return out;
      }
      out.bytes.push_back(static_cast<std::uint8_t>(*value & 0xFFu));
      continue;
    }

    const auto first = token.find(':');
    const auto second = token.find(':', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) {
      out.error = "tilemap entry is not tile:palette:flags: " + token;
      out.bytes.clear();
      return out;
    }
    const auto tile = text::parseHex(std::string_view(token).substr(0, first));
    const std::string paletteText = token.substr(first + 1, second - first - 1);
    const std::string flags = token.substr(second + 1);
    if (!tile || *tile > 0x3FFu) {
      out.error = "tilemap tile number out of range: " + token;
      out.bytes.clear();
      return out;
    }
    unsigned palette = 0;
    try {
      palette = static_cast<unsigned>(std::stoul(paletteText));
    } catch (...) {
      out.error = "tilemap palette is not a number: " + token;
      out.bytes.clear();
      return out;
    }
    if (palette > 7u) {
      out.error = "tilemap palette out of range: " + token;
      out.bytes.clear();
      return out;
    }
    unsigned word = *tile | (palette << 10);
    if (flags != "-") {
      for (char c : flags) {
        if (c == 'P') word |= kPriority;
        else if (c == 'H') word |= kXFlip;
        else if (c == 'V') word |= kYFlip;
        else {
          out.error = "tilemap flag is not one of P H V: " + token;
          out.bytes.clear();
          return out;
        }
      }
    }
    out.bytes.push_back(static_cast<std::uint8_t>(word & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFFu));
  }
  return out;
}

Text encodeMode7Map(std::span<const std::uint8_t> map) {
  Text out;
  for (std::size_t i = 0; i < map.size(); ++i) {
    out.text += (i % 32u == 0u ? "" : " ") + std::string("$") + text::hex(map[i], 2);
    if (i % 32u == 31u || i + 1 == map.size()) out.text += '\n';
  }
  return out;
}

}  // namespace snaggletooth::formats
