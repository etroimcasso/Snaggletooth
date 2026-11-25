#include "player/panel_sdl.h"

#include <stdexcept>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace snaggletooth::player {

PanelHost::PanelHost(SDL_Window* window, SDL_Renderer* renderer, SDL_GPUDevice* device,
                     SDL_GPUTextureFormat swapchainFormat)
    : gpu_(renderer == nullptr) {
  IMGUI_CHECKVERSION();
  context_ = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  const float density = SDL_GetWindowPixelDensity(window);
  if (density > 0.0f) ImGui::GetStyle().FontScaleDpi = density;

  const bool platform =
      gpu_ ? ImGui_ImplSDL3_InitForSDLGPU(window) : ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  if (!platform) {
    ImGui::DestroyContext(context_);
    throw std::runtime_error("the panel's events do not start over SDL");
  }
  bool drawing = false;
  if (gpu_) {
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = device;
    info.ColorTargetFormat = swapchainFormat;
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    drawing = ImGui_ImplSDLGPU3_Init(&info);
  } else {
    drawing = ImGui_ImplSDLRenderer3_Init(renderer);
  }
  if (!drawing) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context_);
    throw std::runtime_error(gpu_ ? "the panel cannot draw through the GPU device"
                                  : "the panel cannot draw through SDL's renderer");
  }
}

PanelHost::~PanelHost() {
  if (open_) ImGui::EndFrame();
  if (gpu_) {
    ImGui_ImplSDLGPU3_Shutdown();
  } else {
    ImGui_ImplSDLRenderer3_Shutdown();
  }
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext(context_);
}

void PanelHost::event(const SDL_Event& polled) { static_cast<void>(ImGui_ImplSDL3_ProcessEvent(&polled)); }

void PanelHost::begin() {
  if (open_) ImGui::EndFrame();
  if (gpu_) {
    ImGui_ImplSDLGPU3_NewFrame();
  } else {
    ImGui_ImplSDLRenderer3_NewFrame();
  }
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  open_ = true;
}

void PanelHost::end(SDL_Renderer* renderer) {
  if (!open_) return;
  ImGui::Render();
  open_ = false;
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void PanelHost::prepare(SDL_GPUCommandBuffer* command) {
  if (open_) {
    ImGui::Render();
    open_ = false;
  }
  ImDrawData* data = ImGui::GetDrawData();
  if (data != nullptr) ImGui_ImplSDLGPU3_PrepareDrawData(data, command);
}

void PanelHost::draw(SDL_GPUCommandBuffer* command, SDL_GPURenderPass* pass) {
  ImDrawData* data = ImGui::GetDrawData();
  if (data != nullptr) ImGui_ImplSDLGPU3_RenderDrawData(data, command, pass);
}

}  // namespace snaggletooth::player
