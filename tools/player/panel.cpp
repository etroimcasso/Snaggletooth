#include "player/panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "imgui.h"

namespace snaggletooth::player {

namespace {

constexpr std::uint64_t kGigabyte = 1'000'000'000u;
constexpr float kFontSize = 15.0f;  // the panel's text, in points, before the window's scale

[[nodiscard]] std::string rateText(double bytesPerSecond) {
  const double megabytes = bytesPerSecond / 1'000'000.0;
  if (megabytes >= 10.0) return std::to_string(static_cast<std::uint64_t>(megabytes)) + " MB a second";
  const std::uint64_t tenths = static_cast<std::uint64_t>(megabytes * 10.0);
  if (tenths == 0u) return "under 0.1 MB a second";
  return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u) + " MB a second";
}

// `part` over `whole`, 0 to 1; a whole of 0 is full where anything is held.
[[nodiscard]] float share(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0u) return part == 0u ? 0.0f : 1.0f;
  const double ratio = static_cast<double>(part) / static_cast<double>(whole);
  return static_cast<float>(std::clamp(ratio, 0.0, 1.0));
}

// The drain as one sentence: its rate, the time to empty, or both.
[[nodiscard]] std::optional<std::string> drainText(const LiveStatus& status) {
  const bool draining = status.drainBytesPerSecond && *status.drainBytesPerSecond > 0.0;
  const bool emptying = status.secondsToEmpty && status.buffer.held != 0u;
  if (draining && emptying) {
    return "Draining at " + rateText(*status.drainBytesPerSecond) + "; empty in about " +
           durationText(*status.secondsToEmpty) + ".";
  }
  if (draining) return "Draining at " + rateText(*status.drainBytesPerSecond) + ".";
  if (emptying) return "Empty in about " + durationText(*status.secondsToEmpty) + ".";
  return std::nullopt;
}

// `text` with `step` after a colon where there is one, and a full stop.
[[nodiscard]] std::string withStep(std::string text, const std::string& step) {
  if (!step.empty()) text += ": " + step;
  return text + ".";
}

// The close: its stage, and how far it has come where that is known.
void closeOf(const LiveStatus& status, std::uint64_t closeMost, PanelModel& model) {
  model.kind = PanelKind::Closing;
  model.heading = "Finishing the disassembly";
  model.mark = false;
  model.controls = false;
  model.parts.clear();
  model.notes.emplace_back("The application will exit when done.");
  const std::uint64_t held = status.buffer.held;
  // The last tree begins once the buffer is played, so a tree or a command
  // running while the buffer still holds anything is one from the middle of the
  // session, and the buffer is what the close has left.
  if (held != 0u || status.work == Work::Watching) {
    model.summary = "Playing what the buffer holds, then the last tree.";
    const std::uint64_t most = std::max(closeMost, held);
    if (most == 0u) {
      model.fill = 1.0f;
      model.meter = "nothing left to play";
      return;
    }
    model.fill = share(most - held, most);
    model.meter = sizeText(most - held) + " of " + sizeText(most) + " played";
    model.parts = drainText(status).value_or(std::string());
    return;
  }
  model.indeterminate = true;
  model.meter.clear();
  switch (status.work) {
    case Work::Records: model.summary = withStep("Reading the run records", status.step); break;
    case Work::Tree:
      model.summary = withStep("Writing the last tree", status.step);
      if (status.treeSecondsLeft && *status.treeSecondsLeft > 0.0) {
        model.meter = "about " + durationText(*status.treeSecondsLeft) + " left";
      }
      break;
    case Work::Render: model.summary = "Running snes_render over the tree."; break;
    case Work::Verify: model.summary = "Running snes_verify over the tree."; break;
    case Work::Done: model.summary = "Closing the buffer."; break;
    case Work::Watching: break;
  }
}

// The heading's color for each kind of panel.
[[nodiscard]] ImVec4 accentOf(PanelKind kind) noexcept {
  switch (kind) {
    case PanelKind::AtCeilings: return ImVec4(0.96f, 0.73f, 0.29f, 1.0f);      // amber
    case PanelKind::RamPastCeiling: return ImVec4(0.98f, 0.55f, 0.31f, 1.0f);  // orange
    case PanelKind::ByHand: return ImVec4(0.45f, 0.68f, 1.0f, 1.0f);           // blue
    case PanelKind::Closing: return ImVec4(0.42f, 0.82f, 0.62f, 1.0f);         // green
    case PanelKind::None: break;
  }
  return ImVec4(0.92f, 0.93f, 0.95f, 1.0f);
}

[[nodiscard]] ImVec4 scaled(const ImVec4& color, float by) noexcept {
  return ImVec4(std::min(color.x * by, 1.0f), std::min(color.y * by, 1.0f), std::min(color.z * by, 1.0f), color.w);
}

[[nodiscard]] PanelRect lastItem() {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  return PanelRect{.left = min.x, .top = min.y, .right = max.x, .bottom = max.y};
}

// A line of the panel's own text, wrapped at the sheet's edge, in `color` where given.
void line(const std::string& text, const ImVec4* color = nullptr) {
  if (color != nullptr) ImGui::PushStyleColor(ImGuiCol_Text, *color);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  if (color != nullptr) ImGui::PopStyleColor();
}

// The meter: a rounded track `width` wide, filled to `fill` in `accent`, with
// the resume level a tick across it where `mark` is given; or, `moving`, a
// stripe a third of the track that sweeps across it every 1.6 seconds of Dear
// ImGui's clock.
[[nodiscard]] PanelRect meter(float fill, std::optional<float> mark, bool moving, const ImVec4& accent, float width) {
  const float lineHeight = ImGui::GetTextLineHeight();
  const float thickness = lineHeight * 0.5f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, lineHeight));
  const PanelRect rect = lastItem();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float top = at.y + (lineHeight - thickness) / 2.0f;
  const float bottom = top + thickness;
  const float rounding = thickness / 2.0f;
  draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + width, bottom), ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
  if (moving) {
    const float phase = static_cast<float>(std::fmod(ImGui::GetTime(), 1.6) / 1.6);
    const float stripe = width / 3.0f;
    const float left = at.x - stripe + (width + stripe) * phase;
    draw->PushClipRect(ImVec2(at.x, top), ImVec2(at.x + width, bottom), true);
    draw->AddRectFilled(ImVec2(left, top), ImVec2(left + stripe, bottom), ImGui::GetColorU32(accent), rounding);
    draw->PopClipRect();
  } else if (fill > 0.0f) {
    const float right = at.x + std::max(width * fill, thickness);
    draw->AddRectFilled(ImVec2(at.x, top), ImVec2(right, bottom), ImGui::GetColorU32(accent), rounding);
  }
  if (mark) {
    const float x = at.x + width * *mark;
    draw->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + lineHeight), ImGui::GetColorU32(ImGuiCol_Text),
                  std::max(2.0f, thickness / 4.0f));
  }
  return rect;
}

// One size field: its name, the field `width` wide and "GB" after it, on the
// current line; the text the person types, kept in `text` between drawings, and
// what it reads as.
[[nodiscard]] std::optional<std::uint64_t> sizeField(const char* name, const char* id, std::string& text,
                                                     float width, PanelRect& rect) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(name);
  ImGui::SameLine();
  std::array<char, 32> buffer{};
  text.copy(buffer.data(), buffer.size() - 1u);
  ImGui::SetNextItemWidth(width);
  if (ImGui::InputText(id, buffer.data(), buffer.size())) text = buffer.data();
  rect = lastItem();
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::TextDisabled("GB");
  return parseGigabytes(text);
}

// `text` drawn as a key on a keyboard, level with a button on the same line.
void keycap(const std::string& text) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 size = ImGui::CalcTextSize(text.c_str());
  const ImVec2 padding(style.FramePadding.x * 0.6f, style.FramePadding.y * 0.5f);
  const float frame = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float top = at.y + (frame - size.y - padding.y * 2.0f) / 2.0f;
  const ImVec2 min(at.x, top);
  const ImVec2 max(at.x + size.x + padding.x * 2.0f, top + size.y + padding.y * 2.0f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding * 0.7f);
  draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border, 2.5f), style.FrameRounding * 0.7f);
  draw->AddText(ImVec2(min.x + padding.x, min.y + padding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str());
  ImGui::Dummy(ImVec2(max.x - min.x, frame));
}

// A label in the first column of a section's table, level with the control beside it.
void label(const char* text) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
  ImGui::TableNextColumn();
}

}  // namespace

PanelModel panelOf(const LiveStatus& status, std::string_view chord, std::uint64_t closeMost) {
  PanelModel model;
  const disasm::BufferedRun::Progress& buffer = status.buffer;
  model.pause = status.pause;
  model.memoryBytes = buffer.memoryBytes;
  model.diskBytes = buffer.diskBytes;
  model.chord = std::string(chord);
  model.fill = share(buffer.held, status.ceiling);
  model.resumeAt = share(status.resumeLevel, status.ceiling);
  model.meter = sizeText(buffer.held) + " of " + sizeText(status.ceiling);
  model.parts = "RAM " + sizeText(buffer.memoryUsed) + " of " + sizeText(buffer.memoryBytes) + " · files " +
                sizeText(buffer.diskUsed) + " of " + sizeText(buffer.diskBytes);

  if (status.closing) {
    closeOf(status, closeMost, model);
    return model;
  }
  if (status.stopped == Stopped::No) {
    model = PanelModel{};
    model.controls = false;
    return model;
  }

  const bool ramUnderCeiling = !status.atLimit && buffer.memoryUsed <= buffer.memoryBytes;
  if (status.stopped == Stopped::ByHand) {
    model.kind = PanelKind::ByHand;
    model.heading = "Stopped by hand";
    model.canResume = true;
  } else if (status.memoryStop) {
    model.kind = PanelKind::RamPastCeiling;
    model.heading = "Stopped: the buffer's RAM is past its ceiling";
    model.canResume = buffer.memoryUsed <= buffer.memoryLevel || buffer.held == 0u || ramUnderCeiling;
    model.fill = share(buffer.memoryUsed, buffer.memoryBytes);
    model.resumeAt = share(buffer.memoryLevel, buffer.memoryBytes);
    model.meter = "RAM " + sizeText(buffer.memoryUsed) + " of " + sizeText(buffer.memoryBytes);
    model.parts = "Its files are being written: " + sizeText(buffer.diskUsed) + " of " + sizeText(buffer.diskBytes) + ".";
  } else {
    model.kind = PanelKind::AtCeilings;
    model.heading = "Stopped: the buffer is at its ceilings";
    model.canResume = buffer.held <= status.resumeLevel || ramUnderCeiling;
  }

  if (status.stopped == Stopped::ByHand || status.pause.hold) {
    model.summary = "The game resumes when you resume it.";
  } else if (status.memoryStop) {
    model.summary = "The game resumes at " + sizeText(buffer.memoryLevel) + " of RAM.";
  } else if (status.secondsToResume) {
    model.summary = "The game resumes at " + sizeText(status.resumeLevel) + ", in about " +
                    durationText(*status.secondsToResume) + ".";
  } else {
    model.summary = "The game resumes at " + sizeText(status.resumeLevel) + ".";
  }
  if (std::optional<std::string> drain = drainText(status)) model.notes.push_back(std::move(*drain));
  if (!status.memoryStop && status.fillBytesPerSecond && *status.fillBytesPerSecond > 0.0 &&
      status.secondsOfPlayAfter) {
    model.notes.push_back("The last stretch of play filled it at " + rateText(*status.fillBytesPerSecond) +
                          ": from " + sizeText(status.resumeLevel) + " the game plays about " +
                          durationText(*status.secondsOfPlayAfter) + " before it stops again.");
  }
  return model;
}

void stylePanel() {
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  style.FontSizeBase = kFontSize;
  style.WindowPadding = ImVec2(22.0f, 20.0f);
  style.FramePadding = ImVec2(10.0f, 6.0f);
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  style.CellPadding = ImVec2(4.0f, 4.0f);
  style.WindowRounding = 12.0f;
  style.FrameRounding = 6.0f;
  style.GrabRounding = 6.0f;
  style.GrabMinSize = 14.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.SeparatorTextBorderSize = 1.0f;
  style.SeparatorTextPadding = ImVec2(0.0f, 6.0f);
  ImVec4* colors = style.Colors;
  colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.97f);
  colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
  colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1.0f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.61f, 0.66f, 1.0f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.21f, 1.0f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.22f, 0.26f, 1.0f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.26f, 0.30f, 1.0f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.62f, 0.66f, 0.74f, 1.0f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.82f, 0.85f, 0.92f, 1.0f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.92f, 0.93f, 0.95f, 1.0f);
  colors[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.30f, 0.35f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.33f, 0.35f, 0.41f, 1.0f);
  colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.45f, 0.68f, 1.0f, 0.35f);
}

bool usePlatformFont() {
  static constexpr const char* kFonts[] = {
#if defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/HelveticaNeue.ttc",
#elif defined(_WIN32)
      "C:\\Windows\\Fonts\\segoeui.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
#endif
  };
  ImGuiIO& io = ImGui::GetIO();
  for (const char* path : kFonts) {
    std::error_code missing;
    if (!std::filesystem::is_regular_file(path, missing)) continue;
    if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, kFontSize)) {
      io.FontDefault = font;
      return true;
    }
  }
  io.FontDefault = io.Fonts->AddFontDefaultVector();
  return false;
}

PanelActions drawPanel(const PanelModel& model, PanelEdits& edits, PanelLayout* layout) {
  PanelActions did;
  if (layout != nullptr) *layout = PanelLayout{};
  if (!edits.seeded) {
    edits.memory = gigabytesText(model.memoryBytes);
    edits.disk = gigabytesText(model.diskBytes);
    edits.seeded = true;
  }

  // The picture dimmed behind the sheet.
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::GetBackgroundDrawList()->AddRectFilled(
      viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
      IM_COL32(0, 0, 0, 150));

  const float em = ImGui::GetFontSize();
  const float width = std::min(viewport->Size.x * 0.92f, em * 36.0f);
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // The width is the sheet's; the height fits what it holds.
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
  if (!ImGui::Begin("##panel", nullptr, kFlags)) {
    ImGui::End();
    return did;
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec4 accent = accentOf(model.kind);
  const ImVec4 dim = style.Colors[ImGuiCol_TextDisabled];

  // The heading, a size up, and when the game resumes.
  ImGui::PushFont(nullptr, style.FontSizeBase * 1.3f);
  line(model.heading, &accent);
  ImGui::PopFont();
  line(model.summary);
  ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));

  // The meter, its figure beside it, its parts and the rates under it.
  const float figure = ImGui::CalcTextSize(model.meter.c_str()).x;
  const float track = std::max(ImGui::GetContentRegionAvail().x - figure - style.ItemSpacing.x * 2.0f, em * 4.0f);
  const PanelRect bar = meter(model.fill, model.mark ? std::optional<float>(model.resumeAt) : std::nullopt,
                              model.indeterminate, accent, track);
  if (layout != nullptr) layout->bar = bar;
  ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
  ImGui::TextUnformatted(model.meter.c_str());
  if (!model.parts.empty()) line(model.parts, &dim);
  for (const std::string& note : model.notes) line(note, &dim);

  if (model.controls) {
    const float labels = em * 6.0f;
    const ImVec4 red(1.0f, 0.47f, 0.47f, 1.0f);

    ImGui::SeparatorText("When the buffer is full");
    if (ImGui::BeginTable("##whenFull", 2, ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labels);
      ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch);
      label("Resume at");
      // A slim track with the level beside it, not under the grab.
      int percent = static_cast<int>(std::min(model.pause.resumePercent, 100u));
      const float figureWidth = ImGui::CalcTextSize("100 %").x;
      const float row = ImGui::GetCursorPosY();
      ImGui::SetCursorPosY(row + style.FramePadding.y * 0.7f);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, style.FramePadding.y * 0.3f));
      ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, em);
      ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - figureWidth - style.ItemSpacing.x, em * 4.0f));
      const bool moved = ImGui::SliderInt("##resumeAt", &percent, 0, 100, "", ImGuiSliderFlags_AlwaysClamp);
      ImGui::PopStyleVar(2);
      if (moved) {
        PauseSettings settings = model.pause;
        settings.resumePercent = static_cast<unsigned>(percent);
        did.pause = settings;
      }
      if (layout != nullptr) layout->resumeAt = lastItem();
      ImGui::SameLine();
      ImGui::SetCursorPosY(row);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%d %%", percent);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      bool hold = model.pause.hold;
      if (ImGui::Checkbox("Stay stopped until I resume", &hold)) {
        PauseSettings settings = did.pause.value_or(model.pause);
        settings.hold = hold;
        did.pause = settings;
      }
      if (layout != nullptr) layout->hold = lastItem();
      ImGui::EndTable();
    }

    // The two ceilings on one line, Apply at its end.
    ImGui::SeparatorText("Buffer ceilings");
    PanelRect memoryRect;
    PanelRect diskRect;
    const std::optional<std::uint64_t> memory = sizeField("RAM", "##memory", edits.memory, em * 4.5f, memoryRect);
    ImGui::SameLine(0.0f, em * 1.5f);
    const std::optional<std::uint64_t> disk = sizeField("Files", "##disk", edits.disk, em * 4.5f, diskRect);
    const float applyWidth = ImGui::CalcTextSize("Apply").x + style.FramePadding.x * 2.0f;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + style.ItemSpacing.x,
                                  ImGui::GetWindowContentRegionMax().x - applyWidth));
    const bool ready = memory && disk && (*memory != model.memoryBytes || *disk != model.diskBytes);
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Apply") && ready) {
      did.limits = PanelLimits{.memoryBytes = *memory, .diskBytes = *disk};
    }
    ImGui::EndDisabled();
    if (layout != nullptr) {
      layout->memory = memoryRect;
      layout->disk = diskRect;
      layout->apply = lastItem();
    }
    if (!memory) line("RAM: a size in GB, such as 1 or 0.5", &red);
    if (!disk) line("Files: a size in GB, such as 10 or 0.5", &red);

    // Resume, on the right, with the pause chord before it.
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.5f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.5f));
    const float button = ImGui::CalcTextSize("Resume").x + style.FramePadding.x * 2.0f + em * 2.0f;
    const float hint = model.chord.empty() ? 0.0f
                                           : ImGui::CalcTextSize(model.chord.c_str()).x + style.FramePadding.x * 1.2f +
                                                 style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - button - hint));
    if (!model.chord.empty()) {
      keycap(model.chord);
      ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scaled(accent, 1.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scaled(accent, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
    ImGui::BeginDisabled(!model.canResume);
    if (ImGui::Button("Resume", ImVec2(button, 0.0f))) did.toggle = true;
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    if (layout != nullptr) layout->resume = lastItem();
  }
  ImGui::End();
  return did;
}

float panelTextScale(unsigned windowScale) noexcept {
  return std::max(1.0f, static_cast<float>(windowScale) / 3.0f);
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
