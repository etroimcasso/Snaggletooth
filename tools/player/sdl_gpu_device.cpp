// SdlGpuDevice: the GPU PPU's device over SDL's GPU API.

#include "player/sdl_gpu_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "SDL3/SDL_timer.h"

namespace snaggletooth::player {

namespace {

constexpr SDL_GPUTextureFormat kTextureFormat = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
constexpr std::size_t kTexelBytes = 16u;

[[nodiscard]] gpu::ShaderFormat toFormats(SDL_GPUShaderFormat sdl) noexcept {
  gpu::ShaderFormat out = gpu::ShaderFormat::None;
  if ((sdl & SDL_GPU_SHADERFORMAT_SPIRV) != 0u) out = out | gpu::ShaderFormat::Spirv;
  if ((sdl & SDL_GPU_SHADERFORMAT_DXIL) != 0u) out = out | gpu::ShaderFormat::Dxil;
  if ((sdl & SDL_GPU_SHADERFORMAT_METALLIB) != 0u) out = out | gpu::ShaderFormat::Metallib;
  return out;
}

[[nodiscard]] SDL_GPUShaderFormat toSdl(gpu::ShaderFormat format) noexcept {
  switch (format) {
    case gpu::ShaderFormat::Spirv: return SDL_GPU_SHADERFORMAT_SPIRV;
    case gpu::ShaderFormat::Dxil: return SDL_GPU_SHADERFORMAT_DXIL;
    case gpu::ShaderFormat::Metallib: return SDL_GPU_SHADERFORMAT_METALLIB;
    default: return SDL_GPU_SHADERFORMAT_INVALID;
  }
}

}  // namespace

SdlGpuDevice::~SdlGpuDevice() {
  if (device_ == nullptr) return;
  SDL_WaitForGPUIdle(device_);
  if (mapped_ != nullptr) SDL_UnmapGPUTransferBuffer(device_, upload_);
  if (download_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, download_);
  if (upload_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, upload_);
  if (buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, buffer_);
  if (texture_ != nullptr) SDL_ReleaseGPUTexture(device_, texture_);
  if (scratch_ != nullptr) SDL_ReleaseGPUBuffer(device_, scratch_);
  for (SDL_GPUComputePipeline* pipeline : pipelines_) {
    if (pipeline != nullptr) SDL_ReleaseGPUComputePipeline(device_, pipeline);
  }
  if (window_ != nullptr) SDL_ReleaseWindowFromGPUDevice(device_, window_);
  SDL_DestroyGPUDevice(device_);
}

bool SdlGpuDevice::open(std::string& error) {
  device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
                                false, nullptr);
  if (device_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  const SDL_GPUTextureCreateInfo info{
      .type = SDL_GPU_TEXTURETYPE_2D,
      .format = kTextureFormat,
      .usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER |
               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
      .width = gpu::kTextureWidth,
      .height = gpu::kTextureHeight,
      .layer_count_or_depth = 1u,
      .num_levels = 1u,
      .sample_count = SDL_GPU_SAMPLECOUNT_1,
      .props = 0u,
  };
  texture_ = SDL_CreateGPUTexture(device_, &info);
  if (texture_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  const SDL_GPUBufferCreateInfo scratchInfo{
      .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
      .size = static_cast<Uint32>(gpu::kScratchBytes),
      .props = 0u};
  scratch_ = SDL_CreateGPUBuffer(device_, &scratchInfo);
  if (scratch_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  return true;
}

bool SdlGpuDevice::claim(SDL_Window* window, bool waitForRefresh, std::string& error) {
  if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
    error = SDL_GetError();
    return false;
  }
  window_ = window;
  presentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
  if (!waitForRefresh) {
    if (SDL_WindowSupportsGPUPresentMode(device_, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
      presentMode_ = SDL_GPU_PRESENTMODE_IMMEDIATE;
    } else if (SDL_WindowSupportsGPUPresentMode(device_, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
      presentMode_ = SDL_GPU_PRESENTMODE_MAILBOX;
    }
  }
  if (!SDL_SetGPUSwapchainParameters(device_, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode_)) {
    error = SDL_GetError();
    return false;
  }
  return true;
}

void SdlGpuDevice::releaseWindow() {
  if (window_ == nullptr) return;
  SDL_WaitForGPUIdle(device_);
  SDL_ReleaseWindowFromGPUDevice(device_, window_);
  window_ = nullptr;
}

gpu::ShaderFormat SdlGpuDevice::formats() const {
  return device_ == nullptr ? gpu::ShaderFormat::None : toFormats(SDL_GetGPUShaderFormats(device_));
}

bool SdlGpuDevice::makePipeline(gpu::Pass pass, const gpu::ShaderBytecode& code, gpu::ShaderFormat format,
                                std::string& error) {
  const gpu::PassLayout layout = gpu::passLayout(pass);
  const SDL_GPUComputePipelineCreateInfo info{
      .code_size = code.size,
      .code = code.data,
      .entrypoint = code.entrypoint,
      .format = toSdl(format),
      .num_samplers = 0u,
      .num_readonly_storage_textures = 0u,
      .num_readonly_storage_buffers = layout.readOnlyBuffers,
      .num_readwrite_storage_textures = layout.readWriteTextures,
      .num_readwrite_storage_buffers = layout.readWriteBuffers,
      .num_uniform_buffers = layout.uniformBuffers,
      .threadcount_x = layout.threadsX,
      .threadcount_y = layout.threadsY,
      .threadcount_z = 1u,
      .props = 0u,
  };
  SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
  if (pipeline == nullptr) {
    error = SDL_GetError();
    return false;
  }
  pipelines_[static_cast<unsigned>(pass)] = pipeline;
  format_ = format;
  return true;
}

std::span<std::uint8_t> SdlGpuDevice::frameBytes(std::size_t bytes) {
  if (mapped_ != nullptr) {
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    mapped_ = nullptr;
  }
  // The buffers grow to the largest frame and never shrink.
  if (bytes > bufferBytes_) {
    if (upload_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, upload_);
    if (buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, buffer_);
    const SDL_GPUTransferBufferCreateInfo uploadInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = static_cast<Uint32>(bytes), .props = 0u};
    upload_ = SDL_CreateGPUTransferBuffer(device_, &uploadInfo);
    const SDL_GPUBufferCreateInfo bufferInfo{
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, .size = static_cast<Uint32>(bytes), .props = 0u};
    buffer_ = SDL_CreateGPUBuffer(device_, &bufferInfo);
    bufferBytes_ = bytes;
    ++growths_;
    if (upload_ == nullptr || buffer_ == nullptr) {
      bufferBytes_ = 0u;
      return {};
    }
  }
  mapped_ = SDL_MapGPUTransferBuffer(device_, upload_, true);
  if (mapped_ == nullptr) return {};
  return std::span<std::uint8_t>(static_cast<std::uint8_t*>(mapped_), bufferBytes_);
}

void SdlGpuDevice::draw(const gpu::Frame& frame) {
  const std::uint64_t began = SDL_GetTicksNS();
  lastDrawBegan_ = began;
  if (mapped_ == nullptr || pipelines_[0] == nullptr || pipelines_[1] == nullptr || pipelines_[2] == nullptr) return;
  SDL_UnmapGPUTransferBuffer(device_, upload_);
  mapped_ = nullptr;
  frame_ = frame;
  rows_ = gpu::rowMapFor(placement_, frame.interlaced, frame.field);
  const unsigned rows = gpu::rowsUnder(rows_, frame.height);
  // A picture of a new shape begins on a cleared texture: what the other parity
  // last wrote, or a wider frame's columns, is dropped.
  if (frame.width != shapeWidth_ || rows != shapeRows_) {
    clear();
    shapeWidth_ = frame.width;
    shapeRows_ = rows;
  }
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
  const SDL_GPUTransferBufferLocation from{.transfer_buffer = upload_, .offset = 0u};
  const SDL_GPUBufferRegion to{.buffer = buffer_, .offset = 0u, .size = static_cast<Uint32>(frame.bytes)};
  SDL_UploadToGPUBuffer(copy, &from, &to, true);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(command);
  dispatch(rows_);
  drawn_ = true;
  const std::uint64_t nanos = SDL_GetTicksNS() - began;
  lastDrawNanos_ = nanos;
  drawNanos_ += nanos;
  if (drawCount_ == 0u || nanos < drawNanosLeast_) drawNanosLeast_ = nanos;
  if (nanos > drawNanosMost_) drawNanosMost_ = nanos;
  ++drawCount_;
}

void SdlGpuDevice::dispatch(gpu::RowMap rows) {
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  const SDL_GPUStorageBufferReadWriteBinding scratch{
      .buffer = scratch_, .cycle = false, .padding1 = 0u, .padding2 = 0u, .padding3 = 0u};
  for (const gpu::Pass pass : {gpu::Pass::Sprites, gpu::Pass::Tiles, gpu::Pass::Pixels}) {
    const bool pixels = pass == gpu::Pass::Pixels;
    const SDL_GPUStorageTextureReadWriteBinding target{
        .texture = texture_, .mip_level = 0u, .layer = 0u, .cycle = false, .padding1 = 0u, .padding2 = 0u, .padding3 = 0u};
    SDL_GPUComputePass* run = SDL_BeginGPUComputePass(command, pixels ? &target : nullptr, pixels ? 1u : 0u, &scratch, 1u);
    SDL_BindGPUComputePipeline(run, pipelines_[static_cast<unsigned>(pass)]);
    SDL_BindGPUComputeStorageBuffers(run, 0u, &buffer_, 1u);
    if (pixels) {
      const gpu::Uniform uniform = gpu::uniformFor(rows);
      SDL_PushGPUComputeUniformData(command, 0u, &uniform, static_cast<Uint32>(gpu::kUniformBytes));
    }
    const gpu::Groups groups = gpu::groupsFor(pass, frame_);
    SDL_DispatchGPUCompute(run, groups.x, groups.y, groups.z);
    SDL_EndGPUComputePass(run);
  }
  SDL_SubmitGPUCommandBuffer(command);
}

void SdlGpuDevice::redraw(gpu::RowMap rows) {
  if (!drawn_) return;
  rows_ = rows;
  const unsigned textureRows = gpu::rowsUnder(rows, frame_.height);
  if (frame_.width != shapeWidth_ || textureRows != shapeRows_) {
    clear();
    shapeWidth_ = frame_.width;
    shapeRows_ = textureRows;
  }
  dispatch(rows);
}

void SdlGpuDevice::clear() {
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  const SDL_GPUColorTargetInfo target{
      .texture = texture_,
      .mip_level = 0u,
      .layer_or_depth_plane = 0u,
      .clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f},
      .load_op = SDL_GPU_LOADOP_CLEAR,
      .store_op = SDL_GPU_STOREOP_STORE,
      .resolve_texture = nullptr,
      .resolve_mip_level = 0u,
      .resolve_layer = 0u,
      .cycle = false,
      .cycle_resolve_texture = false,
      .padding1 = 0u,
      .padding2 = 0u,
  };
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1u, nullptr);
  SDL_EndGPURenderPass(pass);
  SDL_SubmitGPUCommandBuffer(command);
}

bool SdlGpuDevice::present(unsigned width, unsigned rows) {
  if (window_ == nullptr) return false;
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUTexture* swapchain = nullptr;
  Uint32 swapchainWidth = 0u;
  Uint32 swapchainHeight = 0u;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window_, &swapchain, &swapchainWidth, &swapchainHeight)) {
    SDL_SubmitGPUCommandBuffer(command);
    return false;
  }
  if (swapchain == nullptr) {
    SDL_SubmitGPUCommandBuffer(command);
    return true;
  }
  const SDL_GPUBlitInfo blit{
      .source = SDL_GPUBlitRegion{.texture = texture_, .mip_level = 0u, .layer_or_depth_plane = 0u,
                                  .x = 0u, .y = 0u, .w = width, .h = rows},
      .destination = SDL_GPUBlitRegion{.texture = swapchain, .mip_level = 0u, .layer_or_depth_plane = 0u,
                                       .x = 0u, .y = 0u, .w = swapchainWidth, .h = swapchainHeight},
      .load_op = SDL_GPU_LOADOP_DONT_CARE,
      .clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f},
      .flip_mode = SDL_FLIP_NONE,
      .filter = SDL_GPU_FILTER_NEAREST,
      .cycle = false,
      .padding1 = 0u,
      .padding2 = 0u,
      .padding3 = 0u,
  };
  SDL_BlitGPUTexture(command, &blit);
  return SDL_SubmitGPUCommandBuffer(command);
}

bool SdlGpuDevice::readBack(unsigned width, unsigned rows, std::vector<std::uint8_t>& rgba) {
  const std::size_t bytes = static_cast<std::size_t>(width) * rows * kTexelBytes;
  if (bytes > downloadBytes_) {
    if (download_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, download_);
    const SDL_GPUTransferBufferCreateInfo info{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = static_cast<Uint32>(bytes), .props = 0u};
    download_ = SDL_CreateGPUTransferBuffer(device_, &info);
    downloadBytes_ = download_ == nullptr ? 0u : bytes;
    if (download_ == nullptr) return false;
  }
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
  const SDL_GPUTextureRegion region{
      .texture = texture_, .mip_level = 0u, .layer = 0u, .x = 0u, .y = 0u, .z = 0u, .w = width, .h = rows, .d = 1u};
  const SDL_GPUTextureTransferInfo into{
      .transfer_buffer = download_, .offset = 0u, .pixels_per_row = width, .rows_per_layer = rows};
  SDL_DownloadFromGPUTexture(copy, &region, &into);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
  if (fence == nullptr) return false;
  SDL_WaitForGPUFences(device_, true, &fence, 1u);
  SDL_ReleaseGPUFence(device_, fence);
  const void* mapped = SDL_MapGPUTransferBuffer(device_, download_, false);
  if (mapped == nullptr) return false;
  rgba.resize(static_cast<std::size_t>(width) * rows * 4u);
  const auto* texels = static_cast<const float*>(mapped);
  for (std::size_t i = 0u; i < static_cast<std::size_t>(width) * rows * 4u; ++i) {
    rgba[i] = static_cast<std::uint8_t>(std::lround(std::clamp(texels[i], 0.0f, 1.0f) * 255.0f));
  }
  SDL_UnmapGPUTransferBuffer(device_, download_);
  return true;
}

std::uint64_t SdlGpuDevice::waitForDrawn() {
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
  if (fence != nullptr) {
    SDL_WaitForGPUFences(device_, true, &fence, 1u);
    SDL_ReleaseGPUFence(device_, fence);
  }
  return SDL_GetTicksNS() - lastDrawBegan_;
}

const char* SdlGpuDevice::formatName() const noexcept {
  switch (format_) {
    case gpu::ShaderFormat::Spirv: return "spirv";
    case gpu::ShaderFormat::Dxil: return "dxil";
    case gpu::ShaderFormat::Metallib: return "metallib";
    default: return "none";
  }
}

const char* SdlGpuDevice::presentModeName() const noexcept {
  switch (presentMode_) {
    case SDL_GPU_PRESENTMODE_IMMEDIATE: return "immediate";
    case SDL_GPU_PRESENTMODE_MAILBOX: return "mailbox";
    default: return "vsync";
  }
}

const char* SdlGpuDevice::driverName() const noexcept {
  const char* name = device_ == nullptr ? nullptr : SDL_GetGPUDeviceDriver(device_);
  return name == nullptr ? "none" : name;
}

}  // namespace snaggletooth::player
