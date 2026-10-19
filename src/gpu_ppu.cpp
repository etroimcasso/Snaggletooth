// The GPU PPU's shader bytes, as the build compiled them, one set a pass.

#include "snaggletooth/gpu/gpu_ppu.h"

#if defined(SNAGGLETOOTH_GPU_PPU) && SNAGGLETOOTH_GPU_PPU
#include "snaggletooth/gpu/shaders/ppu_pixels.comp.h"
#include "snaggletooth/gpu/shaders/ppu_sprites.comp.h"
#include "snaggletooth/gpu/shaders/ppu_tiles.comp.h"
#endif

namespace snaggletooth::gpu {

const ShaderVariants& ppuShader(Pass pass) noexcept {
#if defined(SNAGGLETOOTH_GPU_PPU) && SNAGGLETOOTH_GPU_PPU
  switch (pass) {
    case Pass::Sprites: return shaders::ppu_sprites_comp;
    case Pass::Tiles: return shaders::ppu_tiles_comp;
    case Pass::Pixels: break;
  }
  return shaders::ppu_pixels_comp;
#else
  (void)pass;
  static constexpr ShaderVariants kNone{};
  return kNone;
#endif
}

}  // namespace snaggletooth::gpu
