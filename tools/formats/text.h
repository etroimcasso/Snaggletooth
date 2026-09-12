#pragma once

// Small shared helpers for the text codecs: formatting a value as fixed-width
// upper-case hexadecimal, parsing a `$`-prefixed hex number, dropping a `;`
// comment, and splitting a line into whitespace-separated tokens.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snaggletooth::formats::text {

// `value` as `width` upper-case hex digits (no `$`).
[[nodiscard]] std::string hex(unsigned value, int width);

// The number a `$`-prefixed hex token names, or nothing if it is not one.
[[nodiscard]] std::optional<unsigned> parseDollarHex(std::string_view token);

// A plain hex token (no `$`) as a number, or nothing.
[[nodiscard]] std::optional<unsigned> parseHex(std::string_view token);

// `line` with everything from the first `;` removed.
[[nodiscard]] std::string_view stripComment(std::string_view line);

// `text` split into whitespace-separated tokens, comments dropped.
[[nodiscard]] std::vector<std::string> tokens(const std::string& text);

}  // namespace snaggletooth::formats::text
