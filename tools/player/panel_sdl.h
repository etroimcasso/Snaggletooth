#pragma once

// The Dear ImGui context the player's panel is drawn in, over the window's own
// present: SDL's renderer, or the GPU device's swapchain pass.
//
// The host feeds it every event it polls while the panel is up, begins a frame,
// draws the panel (`panel.h`), and presents the picture again with the frame's
// draw data over it:
//
//   player::PanelHost host{window, renderer, nullptr, SDL_GPU_TEXTUREFORMAT_INVALID};
//   host.event(polled);                     // for each event polled
//   host.begin();
//   const player::PanelActions did = player::drawPanel(model, edits);
//   SDL_RenderClear(renderer);
//   SDL_RenderTexture(renderer, picture, &source, nullptr);
//   host.end(renderer);                     // the panel over the picture
//   SDL_RenderPresent(renderer);
//
// Over the GPU device the present calls `prepare` after its blit and `draw`
// inside its pass on the swapchain texture (`SdlGpuDevice::setOverlay`). A
// frame begun and never presented, a present the window skipped, is ended by
// the next `begin`.
//
// Dear ImGui's settings file is off; the context takes the panel's style
// (`stylePanel`) and the platform's UI font (`usePlatformFont`), scaled by the
// window's pixel density (`SDL_GetWindowPixelDensity`) and by `scaleText`.
// One host lives at a time, and every call is made on the thread that made the
// window.

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_gpu.h"
#include "SDL3/SDL_render.h"
#include "SDL3/SDL_video.h"

struct ImGuiContext;

namespace snaggletooth::player {

class PanelHost {
 public:
  // Over a window shown through `renderer`; or, with `renderer` null, through
  // `device`, drawing into a swapchain of `swapchainFormat`. Throws
  // `std::runtime_error` when a backend does not start.
  PanelHost(SDL_Window* window, SDL_Renderer* renderer, SDL_GPUDevice* device, SDL_GPUTextureFormat swapchainFormat);
  ~PanelHost();
  PanelHost(const PanelHost&) = delete;
  PanelHost& operator=(const PanelHost&) = delete;

  // The panel's text and spacing drawn `factor` times Dear ImGui's own size
  // (`panelTextScale`); called once, before the first frame.
  void scaleText(float factor);
  void event(const SDL_Event& polled);  // every event the player polls while the panel is up
  void begin();                         // a frame: the caller draws the panel after it
  void end(SDL_Renderer* renderer);     // the renderer's present: the draw data over what was just drawn
  // The GPU present's: called with the command buffer before the swapchain's
  // pass and inside it.
  void prepare(SDL_GPUCommandBuffer* command);
  void draw(SDL_GPUCommandBuffer* command, SDL_GPURenderPass* pass);

 private:
  ImGuiContext* context_ = nullptr;
  bool gpu_ = false;
  bool open_ = false;  // a frame was begun and has not been rendered
};

}  // namespace snaggletooth::player
