#include "spc_loader.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace snaggletooth::spc {
namespace {

// Field offsets in the .spc image (the v0.30/v0.31 layout — the two revisions agree
// on every machine-state field).
constexpr std::size_t kMagicOffset = 0x0000;
constexpr std::string_view kMagicPrefix = "SNES-SPC700 Sound File Data v";  // through 'v'
constexpr std::size_t kMarkerOffset = 0x0021;  // two 26 bytes
constexpr std::size_t kPcOffset = 0x0025;
constexpr std::size_t kAOffset = 0x0027;
constexpr std::size_t kXOffset = 0x0028;
constexpr std::size_t kYOffset = 0x0029;
constexpr std::size_t kPswOffset = 0x002A;
constexpr std::size_t kSpOffset = 0x002B;
constexpr std::size_t kRamOffset = 0x0100;
constexpr std::size_t kDspOffset = 0x10100;
constexpr std::size_t kExtraRamOffset = 0x101C0;
constexpr std::size_t kExtraRamSize = 64;
constexpr std::size_t kMinSize = 0x10200;  // 66,048 — machine-complete

SpcLoad reject(std::string reason) {
  return SpcLoad{.state = std::nullopt, .error = std::move(reason)};
}

}  // namespace

SpcLoad parseSpc(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kMinSize) {
    return reject("file is " + std::to_string(bytes.size()) +
                  " bytes; a machine-complete .spc is at least " +
                  std::to_string(kMinSize));
  }

  // The magic through the 'v'; the two version digits vary between revisions.
  for (std::size_t i = 0; i < kMagicPrefix.size(); ++i) {
    if (bytes[kMagicOffset + i] != static_cast<std::uint8_t>(kMagicPrefix[i])) {
      return reject("bad magic at offset 0x00 (not an SPC700 sound file)");
    }
  }
  if (bytes[kMarkerOffset] != 26 || bytes[kMarkerOffset + 1] != 26) {
    return reject("missing 26,26 marker at offset 0x21");
  }

  // The member-initializers of ApuState / DspState / TimerState ARE the power-on
  // shape for everything the file does not carry; the loader overlays only what it
  // does.
  ApuState st{};

  // CPU registers. PC is little-endian; run state stays Running (the format has no
  // halt field).
  st.cpu.pc = static_cast<std::uint16_t>(bytes[kPcOffset] |
                                         (bytes[kPcOffset + 1] << 8));
  st.cpu.a = bytes[kAOffset];
  st.cpu.x = bytes[kXOffset];
  st.cpu.y = bytes[kYOffset];
  st.cpu.psw = bytes[kPswOffset];
  st.cpu.sp = bytes[kSpOffset];

  // RAM verbatim, including the $F0-$FF bytes — those are the underlying RAM, which
  // is what the host sees and what the recovered registers below are read from.
  std::copy_n(bytes.begin() + kRamOffset, st.ram.size(), st.ram.begin());

  // DSP registers verbatim.
  std::copy_n(bytes.begin() + kDspOffset, st.dsp.regs.size(), st.dsp.regs.begin());

  // The extra-RAM field overwrites $FFC0-$FFFF only when the dump was taken with
  // CONTROL bit 7 set (the IPL window mapped read-only, so the RAM image could not
  // hold those bytes). When clear, the RAM image already holds them and the field
  // is ignored. The machine treats $FFC0-$FFFF as plain RAM either way.
  if (st.ram[0xF1] & 0x80) {
    std::copy_n(bytes.begin() + kExtraRamOffset, kExtraRamSize,
                st.ram.begin() + 0xFFC0);
  }

  // Registers recovered from the RAM image, by direct field assignment — not a bus
  // write, so no CONTROL port-clear side effect fires at load.
  st.control = st.ram[0xF1];
  st.dspAddr = st.ram[0xF2];
  for (std::size_t i = 0; i < 3; ++i) {
    st.timers[i].target = st.ram[0xFA + i];
    st.timers[i].stage3 = static_cast<std::uint8_t>(st.ram[0xFD + i] & 0x0F);
  }
  for (std::size_t i = 0; i < 4; ++i) {
    // Both faces seed from the dumped bytes. The output latch is the last value the
    // CPU wrote (the overlay mirrors a port write into RAM), and the input latch
    // takes the same byte, so a driver polling a handshake value sees no spontaneous
    // change at sample zero.
    st.inputPorts[i] = st.ram[0xF4 + i];
    st.outputPorts[i] = st.ram[0xF4 + i];
  }

  return SpcLoad{.state = st, .error = {}};
}

}  // namespace snaggletooth::spc
