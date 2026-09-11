#include "reader.h"

#include <cctype>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "hdma.h"
#include "oam.h"
#include "palette.h"
#include "png.h"
#include "tilemap.h"
#include "tiles.h"

namespace snaggletooth::formats {

namespace {

// The path's extension, lower-cased, without the dot; empty if it has none.
std::string extension(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

std::string asString(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

assembler::Reader encodingReader(assembler::Reader inner, ErrorSink onError) {
  return [inner = std::move(inner), onError = std::move(onError)](
             const std::string& path) -> std::optional<std::string> {
    std::optional<std::string> raw = inner ? inner(path) : std::nullopt;
    if (!raw) return std::nullopt;  // the inner reader could not find the file

    const std::string ext = extension(path);
    auto fail = [&](const std::string& reason) -> std::optional<std::string> {
      if (onError) onError(path + ": " + reason);
      return std::nullopt;
    };

    if (ext == "png") {
      const std::span<const std::uint8_t> bytes(
          reinterpret_cast<const std::uint8_t*>(raw->data()), raw->size());
      Bytes decoded = decodeTiles(bytes);
      return decoded.ok() ? std::optional<std::string>(asString(decoded.bytes)) : fail(decoded.error);
    }
    if (ext == "pal") {
      Bytes decoded = decodePalette(*raw);
      return decoded.ok() ? std::optional<std::string>(asString(decoded.bytes)) : fail(decoded.error);
    }
    if (ext == "map") {
      Bytes decoded = decodeTilemap(*raw);
      return decoded.ok() ? std::optional<std::string>(asString(decoded.bytes)) : fail(decoded.error);
    }
    if (ext == "oam") {
      Bytes decoded = decodeOam(*raw);
      return decoded.ok() ? std::optional<std::string>(asString(decoded.bytes)) : fail(decoded.error);
    }
    if (ext == "hdma") {
      Bytes decoded = decodeHdma(*raw);
      return decoded.ok() ? std::optional<std::string>(asString(decoded.bytes)) : fail(decoded.error);
    }
    return raw;  // every other extension is the bytes on disk
  };
}

}  // namespace snaggletooth::formats
