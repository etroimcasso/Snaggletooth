#include "hdma.h"

#include <sstream>
#include <string>
#include <vector>

#include "text.h"

namespace snaggletooth::formats {

namespace {

std::vector<std::string> lineTokens(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream words{std::string(text::stripComment(line))};
  std::string word;
  while (words >> word) out.push_back(word);
  return out;
}

}  // namespace

Text encodeHdma(std::span<const std::uint8_t> table, unsigned unit, bool indirect) {
  Text out;
  if (unit != 1 && unit != 2 && unit != 4) {
    out.error = "HDMA unit " + std::to_string(unit) + " is not 1, 2 or 4";
    return out;
  }

  std::string body = "unit " + std::to_string(unit) + (indirect ? " indirect\n" : " direct\n");
  std::size_t pos = 0;
  while (pos < table.size()) {
    const unsigned count = table[pos];
    if (count == 0) {
      body += "end\n";
      ++pos;
      break;
    }
    const bool repeat = count >= 0x81u;
    const unsigned lineCount = repeat ? count - 0x80u : count;
    const std::size_t dataLen = indirect ? 2u : (repeat ? unit * lineCount : unit);
    if (pos + 1 + dataLen > table.size()) {
      out.error = "HDMA entry at byte " + std::to_string(pos) +
                  " runs past the table under unit " + std::to_string(unit);
      return out;
    }
    std::string entry = "lines " + std::to_string(lineCount);
    if (repeat) entry += " repeat";
    if (indirect) {
      const unsigned ptr =
          static_cast<unsigned>(table[pos + 1]) | (static_cast<unsigned>(table[pos + 2]) << 8);
      entry += " $" + text::hex(ptr, 4);
    } else {
      for (std::size_t b = 0; b < dataLen; ++b) entry += " $" + text::hex(table[pos + 1 + b], 2);
    }
    body += entry + '\n';
    pos += 1 + dataLen;
  }
  while (pos < table.size()) {
    body += '$' + text::hex(table[pos], 2) + '\n';
    ++pos;
  }

  out.text = std::move(body);
  return out;
}

Bytes decodeHdma(const std::string& text) {
  Bytes out;
  std::istringstream lines(text);
  std::string line;
  bool header = false;
  while (std::getline(lines, line)) {
    const std::vector<std::string> tok = lineTokens(line);
    if (tok.empty()) continue;

    if (!header) {
      if (tok.size() != 3 || tok[0] != "unit" || (tok[2] != "direct" && tok[2] != "indirect")) {
        out.error = "HDMA first line is not `unit <1|2|4> <direct|indirect>`: " + line;
        return out;
      }
      if (tok[1] != "1" && tok[1] != "2" && tok[1] != "4") {
        out.error = "HDMA unit is not 1, 2 or 4: " + line;
        return out;
      }
      header = true;
      continue;
    }

    if (tok.size() == 1 && tok[0] == "end") {
      out.bytes.push_back(0);
      continue;
    }
    if (tok.size() == 1 && tok[0].front() == '$') {
      const auto value = text::parseDollarHex(tok[0]);
      if (!value || tok[0].size() != 3) {
        out.error = "HDMA trailing byte is not $XX: " + tok[0];
        out.bytes.clear();
        return out;
      }
      out.bytes.push_back(static_cast<std::uint8_t>(*value & 0xFFu));
      continue;
    }

    if (tok[0] == "lines" && tok.size() >= 2) {
      unsigned n = 0;
      try {
        n = static_cast<unsigned>(std::stoul(tok[1]));
      } catch (...) {
        out.error = "HDMA line count is not a number: " + line;
        out.bytes.clear();
        return out;
      }
      std::size_t idx = 2;
      bool repeat = false;
      if (idx < tok.size() && tok[idx] == "repeat") {
        repeat = true;
        ++idx;
      }
      if (n == 0 || (repeat && n > 0x7Fu) || (!repeat && n > 0x80u)) {
        out.error = "HDMA line count out of range: " + line;
        out.bytes.clear();
        return out;
      }
      out.bytes.push_back(static_cast<std::uint8_t>(repeat ? n + 0x80u : n));
      for (; idx < tok.size(); ++idx) {
        const auto datum = text::parseDollarHex(tok[idx]);
        if (!datum) {
          out.error = "HDMA entry data is not a $-prefixed value: " + tok[idx];
          out.bytes.clear();
          return out;
        }
        if (tok[idx].size() == 5) {  // $XXXX — an indirect pointer
          out.bytes.push_back(static_cast<std::uint8_t>(*datum & 0xFFu));
          out.bytes.push_back(static_cast<std::uint8_t>((*datum >> 8) & 0xFFu));
        } else if (tok[idx].size() == 3) {  // $XX — a direct data byte
          out.bytes.push_back(static_cast<std::uint8_t>(*datum & 0xFFu));
        } else {
          out.error = "HDMA entry value is neither a byte nor a pointer: " + tok[idx];
          out.bytes.clear();
          return out;
        }
      }
      continue;
    }

    out.error = "HDMA line is not an entry, `end`, or a lone byte: " + line;
    out.bytes.clear();
    return out;
  }
  return out;
}

}  // namespace snaggletooth::formats
