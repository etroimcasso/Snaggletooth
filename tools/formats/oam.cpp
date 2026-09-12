#include "oam.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "text.h"

namespace snaggletooth::formats {

namespace {

constexpr std::size_t kLowTableBytes = 512;

std::vector<std::string> lineTokens(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream words{std::string(text::stripComment(line))};
  std::string word;
  while (words >> word) out.push_back(word);
  return out;
}

// The three colon-separated fields of an attribute token, or empty on a malformed
// token.
bool splitColons(const std::string& token, std::vector<std::string>& parts) {
  parts.clear();
  std::string field;
  std::istringstream in(token);
  while (std::getline(in, field, ':')) parts.push_back(field);
  return true;
}

}  // namespace

Text encodeOam(std::span<const std::uint8_t> oam) {
  Text out;
  std::string body;

  const std::size_t low = std::min(oam.size(), kLowTableBytes);
  const std::size_t sprites = low / 4;
  for (std::size_t i = 0; i < sprites; ++i) {
    const unsigned b0 = oam[i * 4 + 0];
    const unsigned b1 = oam[i * 4 + 1];
    const unsigned b2 = oam[i * 4 + 2];
    const unsigned b3 = oam[i * 4 + 3];
    const unsigned tile = b2 | ((b3 & 1u) << 8);
    const unsigned palette = (b3 >> 1) & 0x7u;
    const unsigned priority = (b3 >> 4) & 0x3u;
    std::string flags;
    if (b3 & 0x40u) flags += 'H';
    if (b3 & 0x80u) flags += 'V';
    body += '$' + text::hex(b0, 2) + " $" + text::hex(b1, 2) + ' ' + text::hex(tile, 3) + ' ' +
            std::to_string(palette) + ':' + std::to_string(priority) + ':' +
            (flags.empty() ? "-" : flags) + '\n';
  }
  for (std::size_t j = sprites * 4; j < low; ++j) {
    body += '$' + text::hex(oam[j], 2) + '\n';
  }

  if (oam.size() > kLowTableBytes) {
    body += '\n';
    for (std::size_t k = kLowTableBytes; k < oam.size(); ++k) {
      const unsigned byte = oam[k];
      std::string line;
      for (unsigned j = 0; j < 4; ++j) {
        const unsigned size = (byte >> (2 * j + 1)) & 1u;
        const unsigned x9 = (byte >> (2 * j)) & 1u;
        if (j) line += ' ';
        line += std::to_string(size) + ':' + std::to_string(x9);
      }
      body += line + '\n';
    }
  }

  out.text = std::move(body);
  return out;
}

Bytes decodeOam(const std::string& text) {
  Bytes out;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::vector<std::string> tok = lineTokens(line);
    if (tok.empty()) continue;

    if (tok.size() == 1 && tok[0].front() == '$') {  // a lone low-table byte
      const auto value = text::parseDollarHex(tok[0]);
      if (!value || tok[0].size() != 3) {
        out.error = "OAM trailing byte is not $XX: " + tok[0];
        out.bytes.clear();
        return out;
      }
      out.bytes.push_back(static_cast<std::uint8_t>(*value & 0xFFu));
      continue;
    }

    if (tok.size() == 4 && tok[0].front() == '$') {  // a sprite
      const auto x = text::parseDollarHex(tok[0]);
      const auto y = text::parseDollarHex(tok[1]);
      const auto tile = text::parseHex(tok[2]);
      std::vector<std::string> attr;
      splitColons(tok[3], attr);
      if (!x || !y || !tile || *tile > 0x1FFu || attr.size() != 3) {
        out.error = "OAM sprite line is malformed: " + line;
        out.bytes.clear();
        return out;
      }
      unsigned palette = 0;
      unsigned priority = 0;
      try {
        palette = static_cast<unsigned>(std::stoul(attr[0]));
        priority = static_cast<unsigned>(std::stoul(attr[1]));
      } catch (...) {
        out.error = "OAM sprite attributes are not numbers: " + line;
        out.bytes.clear();
        return out;
      }
      if (palette > 7u || priority > 3u) {
        out.error = "OAM sprite palette or priority out of range: " + line;
        out.bytes.clear();
        return out;
      }
      unsigned b3 = ((*tile >> 8) & 1u) | (palette << 1) | (priority << 4);
      if (attr[2] != "-") {
        for (char c : attr[2]) {
          if (c == 'H') b3 |= 0x40u;
          else if (c == 'V') b3 |= 0x80u;
          else {
            out.error = "OAM sprite flag is not H or V: " + line;
            out.bytes.clear();
            return out;
          }
        }
      }
      out.bytes.push_back(static_cast<std::uint8_t>(*x & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>(*y & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>(*tile & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>(b3 & 0xFFu));
      continue;
    }

    if (tok.size() == 4) {  // a high-table byte: four size:x9 pairs
      unsigned byte = 0;
      bool bad = false;
      for (unsigned j = 0; j < 4; ++j) {
        std::vector<std::string> pair;
        splitColons(tok[j], pair);
        if (pair.size() != 2 || (pair[0] != "0" && pair[0] != "1") ||
            (pair[1] != "0" && pair[1] != "1")) {
          bad = true;
          break;
        }
        if (pair[0] == "1") byte |= 1u << (2 * j + 1);
        if (pair[1] == "1") byte |= 1u << (2 * j);
      }
      if (bad) {
        out.error = "OAM high-table line is not four size:x9 pairs: " + line;
        out.bytes.clear();
        return out;
      }
      out.bytes.push_back(static_cast<std::uint8_t>(byte));
      continue;
    }

    out.error = "OAM line is not a sprite, a high-table byte, or a lone byte: " + line;
    out.bytes.clear();
    return out;
  }
  return out;
}

}  // namespace snaggletooth::formats
