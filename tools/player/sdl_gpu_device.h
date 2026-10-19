#pragma once

// The GPU PPU's device over SDL's GPU API: what the player hands SnesConfig::gpuPpu.
// It opens a device in whichever of SDL's backends the platform has, makes the
// three passes' compute pipelines from the shader bytes the machine selects, maps
// its upload buffer for the machine to pack a frame into, and on draw() uploads
// the frame, dispatches the three passes over its scratch buffer into its texture
// under the field placement it was given, and submits. The texture persists between frames. A window claimed for the
// device is where present() blits the texture; readBack() copies the texture's
// rows out as RGBA bytes, each channel rounded from the texture's float, for a
// recording or a comparison with the machine's own frame. Every call the machine
// makes arrives on the thread the machine is stepped on; present() and readBack()
// are the player's own calls, made from the same thread.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "SDL3/SDL_gpu.h"
#include "SDL3/SDL_video.h"
#include "snaggletooth/gpu/gpu_ppu.h"

namespace snaggletooth::player {

class SdlGpuDevice final : public gpu::Device {
 public:
  SdlGpuDevice() = default;
  ~SdlGpuDevice() override;
  SdlGpuDevice(const SdlGpuDevice&) = delete;
  SdlGpuDevice& operator=(const SdlGpuDevice&) = delete;

  // Opens the device and makes the texture: false with SDL's reason when no device
  // opens. Nothing is claimed yet; a run with no window never calls claim().
  [[nodiscard]] bool open(std::string& error);
  // Claims a window for the present, with the swapchain waiting for a refresh where
  // `waitForRefresh`, and otherwise handed back at once where the window allows it,
  // or through a mailbox, or at the refresh after all. presentMode() names which.
  [[nodiscard]] bool claim(SDL_Window* window, bool waitForRefresh, std::string& error);
  // The claimed window given back, before whoever owns it destroys it.
  void releaseWindow();

  // gpu::Device, as the machine calls it.
  [[nodiscard]] gpu::ShaderFormat formats() const override;
  bool makePipeline(gpu::Pass pass, const gpu::ShaderBytecode& code, gpu::ShaderFormat format,
                    std::string& error) override;
  [[nodiscard]] std::span<std::uint8_t> frameBytes(std::size_t bytes) override;
  void draw(const gpu::Frame& frame) override;

  // Where an interlaced run's fields land in the texture (gpu::rowMapFor); Weave
  // unless set. A change of the picture's shape — width, rows, interlaced or not —
  // clears the texture before the next frame is drawn.
  void setPlacement(gpu::FieldPlacement placement) noexcept { placement_ = placement; }
  // The frame last uploaded dispatched again under another row map, which a
  // verifying run does to present the woven picture after reading the plain one.
  void redraw(gpu::RowMap rows);
  // The frame last drawn and the rows it was placed under.
  [[nodiscard]] const gpu::Frame& lastFrame() const noexcept { return frame_; }
  [[nodiscard]] gpu::RowMap lastRows() const noexcept { return rows_; }

  // The texture's top-left `width` by `rows` texels blitted to the whole of the
  // claimed window, nearest-neighbour. A window with no swapchain texture to give,
  // minimized say, is a present skipped and answered true.
  [[nodiscard]] bool present(unsigned width, unsigned rows);
  // The texture's top-left `width` by `rows` texels as RGBA bytes, row by row,
  // waited for.
  [[nodiscard]] bool readBack(unsigned width, unsigned rows, std::vector<std::uint8_t>& rgba);
  // Waits for the device to finish everything submitted so far and answers the
  // nanoseconds from the last draw()'s entry to that point: the frame's whole cost
  // on the GPU, measured by a run that proves the picture rather than shows it.
  [[nodiscard]] std::uint64_t waitForDrawn();

  // What the run reports: the format the pipeline was made in, the present mode,
  // how many times the buffers grew, and the nanoseconds draw() spent from its
  // entry to its submit's return, summed, least and most, over the frames drawn.
  [[nodiscard]] const char* formatName() const noexcept;
  [[nodiscard]] const char* presentModeName() const noexcept;
  [[nodiscard]] const char* driverName() const noexcept;
  [[nodiscard]] unsigned growths() const noexcept { return growths_; }
  [[nodiscard]] std::uint64_t drawCount() const noexcept { return drawCount_; }
  [[nodiscard]] std::uint64_t drawNanos() const noexcept { return drawNanos_; }
  [[nodiscard]] std::uint64_t drawNanosLeast() const noexcept { return drawCount_ == 0u ? 0u : drawNanosLeast_; }
  [[nodiscard]] std::uint64_t drawNanosMost() const noexcept { return drawNanosMost_; }
  [[nodiscard]] std::uint64_t lastDrawNanos() const noexcept { return lastDrawNanos_; }

 private:
  // The texture cleared to black, and the frame uploaded drawn under `rows`: the
  // three passes dispatched in order.
  void clear();
  void dispatch(gpu::RowMap rows);

  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUComputePipeline* pipelines_[3] = {nullptr, nullptr, nullptr};  // one a pass, in Pass's order
  SDL_GPUBuffer* scratch_ = nullptr;
  SDL_GPUTexture* texture_ = nullptr;
  SDL_GPUTransferBuffer* upload_ = nullptr;
  SDL_GPUBuffer* buffer_ = nullptr;
  std::size_t bufferBytes_ = 0;    // the upload and storage buffers' length
  void* mapped_ = nullptr;         // the upload buffer while frameBytes() has it mapped
  SDL_GPUTransferBuffer* download_ = nullptr;
  std::size_t downloadBytes_ = 0;
  SDL_Window* window_ = nullptr;
  SDL_GPUPresentMode presentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
  gpu::ShaderFormat format_ = gpu::ShaderFormat::None;
  gpu::FieldPlacement placement_ = gpu::FieldPlacement::Weave;
  gpu::Frame frame_{};
  gpu::RowMap rows_{};
  bool drawn_ = false;        // a frame has been drawn since the shape was last noted
  unsigned shapeWidth_ = 0;   // the shape the texture was last cleared for
  unsigned shapeRows_ = 0;
  unsigned growths_ = 0;
  std::uint64_t drawCount_ = 0;
  std::uint64_t drawNanos_ = 0;
  std::uint64_t drawNanosLeast_ = 0;
  std::uint64_t drawNanosMost_ = 0;
  std::uint64_t lastDrawNanos_ = 0;
  std::uint64_t lastDrawBegan_ = 0;
};

}  // namespace snaggletooth::player
