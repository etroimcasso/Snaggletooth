#include "player/panel.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "imgui.h"

namespace snaggletooth::player {

namespace {

constexpr std::uint64_t kGigabyte = 1'000'000'000u;

[[nodiscard]] std::string rateText(double bytesPerSecond) {
  return std::to_string(static_cast<std::uint64_t>(bytesPerSecond / 1'000'000.0)) + " MB a second";
}

// `part` over `whole`, 0 to 1; a whole of 0 is full where anything is held.
[[nodiscard]] float share(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0u) return part == 0u ? 0.0f : 1.0f;
  const double ratio = static_cast<double>(part) / static_cast<double>(whole);
  return static_cast<float>(std::clamp(ratio, 0.0, 1.0));
}

// What the close has left, as `LiveDisassembly::closeAsked` says it.
[[nodiscard]] std::string leftText(const LiveStatus& status) {
  switch (status.work) {
    case Work::Records: return "The run records are being read, then the last tree.";
    case Work::Tree: return "The last tree is being made.";
    case Work::Render: return "snes_render is running.";
    case Work::Verify: return "snes_verify is running.";
    case Work::Watching:
    case Work::Done: break;
  }
  return sizeText(status.buffer.held) + " to play, then the last tree.";
}

[[nodiscard]] PanelRect lastItem() {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  return PanelRect{.left = min.x, .top = min.y, .right = max.x, .bottom = max.y};
}

// One size field: the text the person types, kept in `text` between drawings.
void sizeField(const char* label, std::string& text) {
  std::array<char, 32> buffer{};
  text.copy(buffer.data(), buffer.size() - 1u);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
  if (ImGui::InputText(label, buffer.data(), buffer.size())) text = buffer.data();
}

}  // namespace

PanelModel panelOf(const LiveStatus& status, std::string_view chord) {
  PanelModel model;
  const disasm::BufferedRun::Progress& buffer = status.buffer;
  model.pause = status.pause;
  model.memoryBytes = buffer.memoryBytes;
  model.diskBytes = buffer.diskBytes;
  model.chord = std::string(chord);
  model.fill = share(buffer.held, status.ceiling);
  model.resumeAt = share(status.resumeLevel, status.ceiling);

  if (status.closing) {
    model.heading = "Finishing the disassembly";
    model.lines.push_back(leftText(status));
    model.lines.emplace_back("It ends by itself when that is done.");
    model.controls = false;
    return model;
  }
  if (status.stopped == Stopped::No) {
    model.controls = false;
    return model;
  }

  const bool ramUnderCeiling = !status.atLimit && buffer.memoryUsed <= buffer.memoryBytes;
  if (status.stopped == Stopped::ByHand) {
    model.heading = "Stopped by hand";
    model.canResume = true;
  } else if (status.memoryStop) {
    model.heading = "Stopped: the buffer's RAM is past its ceiling";
    model.canResume = buffer.memoryUsed <= buffer.memoryLevel || buffer.held == 0u || ramUnderCeiling;
    model.fill = share(buffer.memoryUsed, buffer.memoryBytes);
    model.resumeAt = share(buffer.memoryLevel, buffer.memoryBytes);
  } else {
    model.heading = "Stopped: the buffer is at its ceilings";
    model.canResume = buffer.held <= status.resumeLevel || ramUnderCeiling;
  }

  if (status.memoryStop) {
    model.lines.push_back("The buffer holds " + sizeText(buffer.memoryUsed) + " of RAM, past its " +
                          sizeText(buffer.memoryBytes) + " ceiling, while its files are written.");
  } else {
    model.lines.push_back("The buffer holds " + sizeText(buffer.held) + " of " + sizeText(status.ceiling) + ": " +
                          sizeText(buffer.memoryUsed) + " of " + sizeText(buffer.memoryBytes) + " in RAM, " +
                          sizeText(buffer.diskUsed) + " of " + sizeText(buffer.diskBytes) + " in files.");
  }
  if (status.drainBytesPerSecond && *status.drainBytesPerSecond > 0.0) {
    model.lines.push_back("Draining at " + rateText(*status.drainBytesPerSecond) + ".");
  }
  if (status.stopped == Stopped::ByHand || status.pause.hold) {
    model.lines.emplace_back("The game resumes when you resume it.");
  } else if (status.memoryStop) {
    model.lines.push_back("The game resumes at " + sizeText(buffer.memoryLevel) + " of RAM.");
  } else if (status.secondsToResume) {
    model.lines.push_back("The game resumes at " + sizeText(status.resumeLevel) + ", in about " +
                          durationText(*status.secondsToResume) + ".");
  } else {
    model.lines.push_back("The game resumes at " + sizeText(status.resumeLevel) + ".");
  }
  if (status.secondsToEmpty && buffer.held != 0u) {
    model.lines.push_back("Empty in about " + durationText(*status.secondsToEmpty) + ".");
  }
  if (!status.memoryStop && status.fillBytesPerSecond && *status.fillBytesPerSecond > 0.0 &&
      status.secondsOfPlayAfter) {
    model.lines.push_back("The last stretch of play filled the buffer at " + rateText(*status.fillBytesPerSecond) +
                          ": from " + sizeText(status.resumeLevel) + " the game plays about " +
                          durationText(*status.secondsOfPlayAfter) + " before it stops again.");
  }
  return model;
}

PanelActions drawPanel(const PanelModel& model, PanelEdits& edits, PanelLayout* layout) {
  PanelActions did;
  if (layout != nullptr) *layout = PanelLayout{};
  if (!edits.seeded) {
    edits.memory = gigabytesText(model.memoryBytes);
    edits.disk = gigabytesText(model.diskBytes);
    edits.seeded = true;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // A width of most of the display; the height fits what the panel holds.
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.8f, 0.0f), ImGuiCond_Always);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
  if (!ImGui::Begin("##panel", nullptr, kFlags)) {
    ImGui::End();
    return did;
  }
  ImGui::TextUnformatted(model.heading.c_str());
  ImGui::Separator();
  ImGui::PushTextWrapPos(0.0f);
  for (const std::string& line : model.lines) ImGui::TextUnformatted(line.c_str());
  ImGui::PopTextWrapPos();

  if (model.controls) {
    ImGui::Spacing();
    ImGui::ProgressBar(model.fill, ImVec2(-1.0f, 0.0f));
    const PanelRect bar = lastItem();
    const float markX = bar.left + (bar.right - bar.left) * model.resumeAt;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(markX, bar.top), ImVec2(markX, bar.bottom),
                                        ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
    if (layout != nullptr) layout->bar = bar;

    int percent = static_cast<int>(std::min(model.pause.resumePercent, 100u));
    if (ImGui::SliderInt("Resume at", &percent, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp)) {
      PauseSettings settings = model.pause;
      settings.resumePercent = static_cast<unsigned>(percent);
      did.pause = settings;
    }
    if (layout != nullptr) layout->resumeAt = lastItem();

    bool hold = model.pause.hold;
    if (ImGui::Checkbox("Stay stopped until I resume", &hold)) {
      PauseSettings settings = did.pause.value_or(model.pause);
      settings.hold = hold;
      did.pause = settings;
    }
    if (layout != nullptr) layout->hold = lastItem();

    ImGui::Spacing();
    sizeField("RAM (GB)", edits.memory);
    if (layout != nullptr) layout->memory = lastItem();
    const std::optional<std::uint64_t> memory = parseGigabytes(edits.memory);
    if (!memory) ImGui::TextDisabled("A size in GB, such as 1 or 0.5");
    sizeField("Files (GB)", edits.disk);
    if (layout != nullptr) layout->disk = lastItem();
    const std::optional<std::uint64_t> disk = parseGigabytes(edits.disk);
    if (!disk) ImGui::TextDisabled("A size in GB, such as 1 or 0.5");
    if (ImGui::Button("Apply") && memory && disk) {
      did.limits = PanelLimits{.memoryBytes = *memory, .diskBytes = *disk};
    }
    if (layout != nullptr) layout->apply = lastItem();

    ImGui::Spacing();
    ImGui::BeginDisabled(!model.canResume);
    if (ImGui::Button("Resume")) did.toggle = true;
    ImGui::EndDisabled();
    if (layout != nullptr) layout->resume = lastItem();
    if (!model.chord.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("or %s", model.chord.c_str());
    }
  }
  ImGui::End();
  return did;
}

std::string gigabytesText(std::uint64_t bytes) {
  std::string text = std::to_string(bytes / kGigabyte);
  std::uint64_t fraction = bytes % kGigabyte;
  if (fraction == 0u) return text;
  std::string digits = std::to_string(fraction);
  digits.insert(0, 9u - digits.size(), '0');
  digits.erase(digits.find_last_not_of('0') + 1u);
  return text + "." + digits;
}

}  // namespace snaggletooth::player
