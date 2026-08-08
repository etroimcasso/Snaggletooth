#pragma once

// The .spc loader — maps an SPC700 Sound File image onto an ApuState the machine
// restores. An .spc snapshots the CPU registers, the 64KB RAM, and the 128 DSP
// registers. It does not carry the machine's free-running and internal state: the
// timer stage counters and shared divider, the comm-port latches (both sides), the
// DSP's per-voice envelopes, pitch counters, BRR windows, the noise level, and the
// echo ring, nor the CPU run state. Those are seeded to the deterministic power-on
// shape. The registers that ride inside the RAM image's $F0-$FF bytes (CONTROL,
// DSPADDR, the timer targets and outputs, and the comm ports) are recovered from
// it.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "snaggletooth/apu/apu.h"

namespace snaggletooth::spc {

// The result of parsing an .spc image: the restored machine state on success, or a
// reason naming the field that failed on rejection. Exactly one is set — a present
// `state` means the parse succeeded and `error` is empty; a non-empty `error` means
// it was rejected and `state` is empty.
struct SpcLoad {
  std::optional<ApuState> state;
  std::string error;
};

// Parses an .spc image into an ApuState. Validates the 33-byte magic (through the
// 'v'; the two version digits vary between revisions and are not checked), the
// 26,26 marker at offset 0x21, and the machine-complete minimum length of 66,048
// bytes; a shorter or malformed image is rejected with a reason. The CPU registers,
// the RAM, and the DSP registers map verbatim from their offsets; CONTROL, DSPADDR,
// the timer targets and outputs, and both comm-port faces are recovered from the
// RAM image; the extra-RAM field overwrites $FFC0-$FFFF only when the dumped CONTROL
// bit 7 was set. Everything the format does not carry is seeded to power-on values.
[[nodiscard]] SpcLoad parseSpc(std::span<const std::uint8_t> bytes);

}  // namespace snaggletooth::spc
