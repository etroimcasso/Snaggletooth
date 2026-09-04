// The recorded run: the script's grammar and its refusals, what a port holds at
// a frame, and the replay — a cartridge whose two indirect jumps are taken only
// when a button is down, one read through the auto-read and one through the
// serial port, so a scripted run reaches code the boot alone never does.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cartridge_fixtures.h"
#include "gtest/gtest.h"
#include "rom/input_script.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"

namespace snaggletooth::disasm {
namespace {

using fixtures::loRomImage;
using fixtures::put;

constexpr std::uint64_t kFrame = 262u * 1364u;  // one NTSC frame of the master clock

// One bank. Reset enables the vertical-blank NMI and the auto-read and idles. The
// NMI handler waits for the auto-read to finish, and if Start is down in $4219
// jumps through the pointer at $8100 to $8200; otherwise it strobes the serial
// port, clocks nine bits out — the ninth is A — and if A is down jumps through the
// pointer at $8102 to $8210. Neither target is named by any instruction.
std::vector<std::uint8_t> buttonDispatchImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x3A] = 0x00u;  // emulation NMI -> $8300
  rom[site + 0x3B] = 0x83u;
  put(rom, 0x0100u, {0x00u, 0x82u,     // $8100: -> $8200
                     0x10u, 0x82u});   // $8102: -> $8210
  put(rom, 0x0000u, {
                        0xA9u, 0x81u,                // $8000 LDA #$81       NMI on, auto-read on
                        0x8Du, 0x00u, 0x42u,         // $8002 STA !$4200
                        0x80u, 0xFEu,                // $8005 BRA $8005      idle
                    });
  put(rom, 0x0300u, {
                        0xADu, 0x10u, 0x42u,         // $8300 LDA !$4210     acknowledge
                        0xADu, 0x12u, 0x42u,         // $8303 LDA !$4212
                        0x29u, 0x01u,                // $8306 AND #$01
                        0xD0u, 0xF9u,                // $8308 BNE $8303      until the auto-read is done
                        0xADu, 0x19u, 0x42u,         // $830A LDA !$4219
                        0x29u, 0x10u,                // $830D AND #$10       Start
                        0xF0u, 0x03u,                // $830F BEQ $8314
                        0x6Cu, 0x00u, 0x81u,         // $8311 JMP (!$8100)   -> $8200
                        0xA9u, 0x01u,                // $8314 LDA #$01
                        0x8Du, 0x16u, 0x40u,         // $8316 STA !$4016     strobe high
                        0x9Cu, 0x16u, 0x40u,         // $8319 STZ !$4016     strobe low
                        0xADu, 0x16u, 0x40u,         // $831C LDA !$4016     B
                        0xADu, 0x16u, 0x40u,         // $831F                Y
                        0xADu, 0x16u, 0x40u,         // $8322                Select
                        0xADu, 0x16u, 0x40u,         // $8325                Start
                        0xADu, 0x16u, 0x40u,         // $8328                Up
                        0xADu, 0x16u, 0x40u,         // $832B                Down
                        0xADu, 0x16u, 0x40u,         // $832E                Left
                        0xADu, 0x16u, 0x40u,         // $8331                Right
                        0xADu, 0x16u, 0x40u,         // $8334 LDA !$4016     A
                        0x29u, 0x01u,                // $8337 AND #$01
                        0xF0u, 0x03u,                // $8339 BEQ $833E
                        0x6Cu, 0x02u, 0x81u,         // $833B JMP (!$8102)   -> $8210
                        0x40u,                       // $833E RTI
                    });
  put(rom, 0x0200u, {0xA9u, 0x01u, 0x8Du, 0x20u, 0x00u, 0x40u});  // $8200 LDA #$01 / STA $0020 / RTI
  put(rom, 0x0210u, {0xA9u, 0x02u, 0x8Du, 0x21u, 0x00u, 0x40u});  // $8210 LDA #$02 / STA $0021 / RTI
  return rom;
}

constexpr Address kAutoReadSite = 0x008311u;
constexpr Address kSerialSite = 0x00833Bu;

InputScript script(const std::string& text) {
  std::string error;
  const std::optional<InputScript> parsed = parseInputScript(text, error);
  EXPECT_TRUE(parsed.has_value()) << error;
  return parsed.value_or(InputScript{});
}

std::string refusal(const std::string& text) {
  std::string error;
  const std::optional<InputScript> parsed = parseInputScript(text, error);
  EXPECT_FALSE(parsed.has_value()) << "accepted: " << text;
  return error;
}

const ReachedTarget* reachedFrom(const std::vector<ReachedTarget>& seen, Address site) {
  for (const ReachedTarget& r : seen) {
    if (r.site == site) return &r;
  }
  return nullptr;
}

std::vector<ReachedTarget> run(std::span<const std::uint8_t> rom, std::uint64_t cycles,
                               const InputScript& input) {
  std::vector<std::string> notes;
  const std::vector<ReachedTarget> seen = observeRun(rom, cycles, input, notes);
  EXPECT_TRUE(notes.empty()) << notes.front();
  return seen;
}

// ---- the grammar ---------------------------------------------------------------------

TEST(InputScript, LinesNameAFrameAPortAndTheButtonsHeld) {
  const InputScript s = script(
      "; a title screen left behind\n"
      "\n"
      "frame 120 1 start\n"
      "frame 130 1 none         ; released\n"
      "frame 200 1 Right B\n"
      "frame 200 2 up\n"
      "\tframe 300 1 l r select\n");
  ASSERT_EQ(s.events.size(), 5u);
  EXPECT_EQ(s.events[0].frame, 120u);
  EXPECT_EQ(s.events[0].port, JoypadPort::One);
  EXPECT_EQ(s.events[0].pad, (Joypad{.start = true}));
  EXPECT_EQ(s.events[1].frame, 130u);
  EXPECT_EQ(s.events[1].pad, Joypad{}) << "`none` is a pad with nothing pressed";
  EXPECT_EQ(s.events[2].pad, (Joypad{.b = true, .right = true})) << "button names in any case";
  EXPECT_EQ(s.events[3].port, JoypadPort::Two);
  EXPECT_EQ(s.events[3].pad, (Joypad{.up = true}));
  EXPECT_EQ(s.events[4].pad, (Joypad{.select = true, .l = true, .r = true}));
}

TEST(InputScript, AnEmptyScriptHasNoEvents) {
  EXPECT_TRUE(script("").events.empty());
  EXPECT_TRUE(script("; nothing\n\n").events.empty());
}

TEST(InputScript, RefusesWhatItCannotRead) {
  EXPECT_NE(refusal("frame 10 1 start\nframe 5 1 none\n").find("frame order"), std::string::npos);
  EXPECT_NE(refusal("frame 10 1 start\nframe 10 1 b\n").find("already given"), std::string::npos);
  EXPECT_NE(refusal("frame 10 1 start start\n").find("named twice"), std::string::npos);
  EXPECT_NE(refusal("frame 10 1 fire\n").find("not a button"), std::string::npos);
  EXPECT_NE(refusal("frame 10 1 start none\n").find("stands alone"), std::string::npos);
  EXPECT_NE(refusal("frame 10 3 start\n").find("not a port"), std::string::npos);
  EXPECT_NE(refusal("frame ten 1 start\n").find("not a frame number"), std::string::npos);
  EXPECT_NE(refusal("frame 10 1\n").find("needs a frame"), std::string::npos);
  EXPECT_NE(refusal("at 10 1 start\n").find("begins with `frame`"), std::string::npos);
  EXPECT_EQ(refusal("frame 1 1 a\nframe 2 1 b\nframe 3 1 c\n").find("line 3"), 0u)
      << "the error names the line";
}

TEST(InputScript, TheSameFrameMayNameBothPortsOnce) {
  EXPECT_EQ(script("frame 10 1 start\nframe 10 2 start\n").events.size(), 2u);
  EXPECT_NE(refusal("frame 10 1 start\nframe 10 2 start\nframe 10 1 none\n").find("already given"),
            std::string::npos);
}

// ---- what a port holds ---------------------------------------------------------------

TEST(InputScript, APortHoldsItsLastLineAtOrBeforeTheFrame) {
  const InputScript s = script("frame 10 1 start\nframe 20 1 a b\nframe 30 1 none\n");
  EXPECT_EQ(s.padAt(JoypadPort::One, 9), Joypad{}) << "a pad, nothing pressed, before the first line";
  EXPECT_EQ(s.padAt(JoypadPort::One, 10), (Joypad{.start = true}));
  EXPECT_EQ(s.padAt(JoypadPort::One, 19), (Joypad{.start = true}));
  EXPECT_EQ(s.padAt(JoypadPort::One, 20), (Joypad{.b = true, .a = true}));
  EXPECT_EQ(s.padAt(JoypadPort::One, 30), Joypad{});
  EXPECT_EQ(s.padAt(JoypadPort::One, 100000), Joypad{});
}

TEST(InputScript, APortTheScriptNeverNamesHasNoPad) {
  const InputScript s = script("frame 10 1 start\n");
  EXPECT_TRUE(s.names(JoypadPort::One));
  EXPECT_FALSE(s.names(JoypadPort::Two));
  EXPECT_FALSE(s.padAt(JoypadPort::Two, 0).has_value());
  EXPECT_FALSE(s.padAt(JoypadPort::Two, 10).has_value());
  EXPECT_FALSE(InputScript{}.padAt(JoypadPort::One, 0).has_value());
}

// ---- the replay ----------------------------------------------------------------------

TEST(InputScriptReplay, TheBootAloneReachesNeitherTarget) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  const std::vector<ReachedTarget> seen = run(rom, 12u * kFrame, InputScript{});
  EXPECT_TRUE(seen.empty());
}

TEST(InputScriptReplay, StartReachesTheTargetReadThroughTheAutoRead) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  const std::vector<ReachedTarget> seen = run(rom, 12u * kFrame, script("frame 5 1 start\n"));
  const ReachedTarget* reached = reachedFrom(seen, kAutoReadSite);
  ASSERT_NE(reached, nullptr);
  EXPECT_EQ(reached->target, 0x008200u);
  EXPECT_FALSE(reached->call);
  EXPECT_TRUE(reached->mode.emulation);
  EXPECT_EQ(reachedFrom(seen, kSerialSite), nullptr) << "A was never down";
}

TEST(InputScriptReplay, AReachesTheTargetReadThroughTheSerialPort) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  const std::vector<ReachedTarget> seen = run(rom, 12u * kFrame, script("frame 5 1 a\n"));
  const ReachedTarget* reached = reachedFrom(seen, kSerialSite);
  ASSERT_NE(reached, nullptr);
  EXPECT_EQ(reached->target, 0x008210u);
  EXPECT_EQ(reachedFrom(seen, kAutoReadSite), nullptr) << "Start was never down";
}

TEST(InputScriptReplay, AButtonIsNotSeenBeforeItsFrame) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  const InputScript late = script("frame 20 1 start\n");
  EXPECT_TRUE(run(rom, 10u * kFrame, late).empty()) << "the run ended before frame 20";
  const std::vector<ReachedTarget> seen = run(rom, 30u * kFrame, late);
  ASSERT_NE(reachedFrom(seen, kAutoReadSite), nullptr);
}

TEST(InputScriptReplay, AReleasedButtonStaysReleased) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  // Start down for frames 5-6, then a pad with nothing pressed; A down from 8.
  const std::vector<ReachedTarget> seen =
      run(rom, 20u * kFrame, script("frame 5 1 start\nframe 7 1 none\nframe 8 1 a\n"));
  ASSERT_NE(reachedFrom(seen, kAutoReadSite), nullptr);
  ASSERT_NE(reachedFrom(seen, kSerialSite), nullptr);
  EXPECT_EQ(seen.size(), 2u) << "one sighting per site, however many frames the button was down";
}

TEST(InputScriptReplay, ThePortsAreNotInterchangeable) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  const std::vector<ReachedTarget> seen = run(rom, 12u * kFrame, script("frame 5 2 start a\n"));
  EXPECT_TRUE(seen.empty()) << "the handler reads port 1";
}

TEST(InputScriptReplay, TwoScriptedRunsAgree) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  const InputScript s = script("frame 5 1 start\nframe 9 1 a\n");
  const std::vector<ReachedTarget> first = run(rom, 16u * kFrame, s);
  const std::vector<ReachedTarget> second = run(rom, 16u * kFrame, s);
  ASSERT_EQ(first.size(), 2u);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) EXPECT_TRUE(sameSighting(first[i], second[i])) << i;
}

TEST(InputScriptReplay, TheRequestCarriesTheScriptIntoTheTree) {
  const std::vector<std::uint8_t> rom = buttonDispatchImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 12u * kFrame;
  const CartridgeDisassembly unplayed = disassembleCartridge(request);
  // Start first, then A alone: the handler takes Start's jump before it reads
  // the serial port, so A is seen only on a frame Start is up.
  request.input = script("frame 5 1 start\nframe 9 1 a\n");
  const CartridgeDisassembly played = disassembleCartridge(request);

  const auto instructions = [](const CartridgeDisassembly& d) {
    std::size_t n = 0;
    for (const RegionListing& region : d.regions) {
      for (const Line& line : region.listing.lines) n += line.isCode ? 1u : 0u;
    }
    return n;
  };
  EXPECT_TRUE(unplayed.reached.empty());
  EXPECT_EQ(played.reached.size(), 2u);
  EXPECT_GT(instructions(played), instructions(unplayed)) << "both targets are code now";
  const std::string manifest = renderManifest(played);
  EXPECT_NE(manifest.find("reached  $00:8200 loc_008200 e=1 m=8 x=8 from $00:8311\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("reached  $00:8210 loc_008210 e=1 m=8 x=8 from $00:833B\n"), std::string::npos)
      << manifest;
}

}  // namespace
}  // namespace snaggletooth::disasm
