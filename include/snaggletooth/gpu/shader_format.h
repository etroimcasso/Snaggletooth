#pragma once

// A compiled shader's bytes in each format a GPU device may take, and the pick for
// the device at hand. The header `shaders/gen_shader.cmake` generates for a shader
// fills the one slot the build compiled for and leaves the others empty; a host
// asks its device which formats it takes, names them as a ShaderFormat set, and
// selectShader answers with the bytes to hand over and their format. Nothing here
// names a GPU API: the host maps its own API's format flags onto ShaderFormat.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace snaggletooth::gpu {

// The three formats a device may take precompiled, as bits a host combines.
enum class ShaderFormat : std::uint32_t {
  None = 0u,
  Spirv = 1u << 0,     // SPIR-V, for Vulkan
  Dxil = 1u << 1,      // DXIL, for Direct3D 12
  Metallib = 1u << 2,  // a Metal library, for Metal
};

[[nodiscard]] constexpr ShaderFormat operator|(ShaderFormat a, ShaderFormat b) noexcept {
  return static_cast<ShaderFormat>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has(ShaderFormat set, ShaderFormat one) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(one)) != 0u;
}

// One format's bytes: the code, its length, and the entry point's name as the
// format's compiler left it. A null `data` is a format the build did not make.
struct ShaderBytecode {
  const unsigned char* data = nullptr;
  std::size_t size = 0;
  const char* entrypoint = nullptr;
};

struct ShaderVariants {
  ShaderBytecode spirv;
  ShaderBytecode dxil;
  ShaderBytecode metallib;
};

// The bytes to hand a device that takes `supported`, and their format, or nothing
// where the build made no format the device takes.
[[nodiscard]] constexpr std::optional<std::pair<ShaderBytecode, ShaderFormat>> selectShader(
    ShaderFormat supported, const ShaderVariants& variants) noexcept {
  if (has(supported, ShaderFormat::Metallib) && variants.metallib.data != nullptr) {
    return std::pair{variants.metallib, ShaderFormat::Metallib};
  }
  if (has(supported, ShaderFormat::Spirv) && variants.spirv.data != nullptr) {
    return std::pair{variants.spirv, ShaderFormat::Spirv};
  }
  if (has(supported, ShaderFormat::Dxil) && variants.dxil.data != nullptr) {
    return std::pair{variants.dxil, ShaderFormat::Dxil};
  }
  return std::nullopt;
}

}  // namespace snaggletooth::gpu
