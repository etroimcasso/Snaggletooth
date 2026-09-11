#include "palette.h"

#include <string>

#include "text.h"

namespace snaggletooth::formats {

namespace {

// A 5-bit channel value to 8 bits, spreading the top bits down so 0 stays 0 and
// 31 becomes 255.
unsigned to8(unsigned five) { return (five << 3) | (five >> 2); }

}  // namespace

Text encodePalette(std::span<const std::uint8_t> cgram) {
  Text out;
  std::string body;
  for (std::size_t i = 0; i < cgram.size(); i += 2) {
    if (i + 1 < cgram.size()) {
      const unsigned word = static_cast<unsigned>(cgram[i]) | (static_cast<unsigned>(cgram[i + 1]) << 8);
      const unsigned r = to8(word & 0x1Fu);
      const unsigned g = to8((word >> 5) & 0x1Fu);
      const unsigned b = to8((word >> 10) & 0x1Fu);
      body += '$';
      body += text::hex(word, 4);
      body += " ; " + std::to_string(r) + ' ' + std::to_string(g) + ' ' + std::to_string(b) + '\n';
    } else {
      body += '$';
      body += text::hex(cgram[i], 2);
      body += '\n';
    }
  }
  out.text = std::move(body);
  return out;
}

Bytes decodePalette(const std::string& text) {
  Bytes out;
  for (const std::string& token : text::tokens(text)) {
    const auto value = text::parseDollarHex(token);
    if (!value) {
      out.error = "palette line is not a $-prefixed hex value: " + token;
      out.bytes.clear();
      return out;
    }
    if (token.size() == 5) {  // $XXXX — a colour word, little-endian
      out.bytes.push_back(static_cast<std::uint8_t>(*value & 0xFFu));
      out.bytes.push_back(static_cast<std::uint8_t>((*value >> 8) & 0xFFu));
    } else if (token.size() == 3) {  // $XX — a lone trailing byte
      out.bytes.push_back(static_cast<std::uint8_t>(*value & 0xFFu));
    } else {
      out.error = "palette value is neither a word nor a byte: " + token;
      out.bytes.clear();
      return out;
    }
  }
  return out;
}

}  // namespace snaggletooth::formats
