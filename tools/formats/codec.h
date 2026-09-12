#pragma once

// The result types the asset codecs share. Every codec turns a run of raw SNES
// bytes into the file a person edits (encode) or that file back into the bytes
// (decode); it either produces its output or names, in one message, what was
// wrong with its input. That message is what a wrapping assembler reports for
// the file that included the asset.

#include <cstdint>
#include <string>
#include <vector>

namespace snaggletooth::formats {

// A run of bytes a codec produced, or the reason it could not. `error` is empty
// on success; on failure `bytes` is empty and `error` names what was wrong.
struct Bytes {
  std::vector<std::uint8_t> bytes;
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

// A text form a codec produced, or the reason it could not.
struct Text {
  std::string text;
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

}  // namespace snaggletooth::formats
