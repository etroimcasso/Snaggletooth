#pragma once

// The APU upload stub — the small SPC700 program that sits in the top of audio
// RAM at power-on and loads a driver the main CPU sends across the communication
// ports.
//
// The real console keeps this program in a 64-byte boot ROM the audio unit maps
// over $FFC0-$FFFF. Snaggletooth has no ROM: the machine seeds the same 64 bytes
// into RAM at construction and points the audio CPU at them, so the documented
// upload handshake runs the same way. The image is an original program written to
// the published protocol, not a copy of the console's — it answers the handshake
// the main CPU speaks, and nothing more.
//
// seedIplStub writes the image into an audio-machine state, clears the ports (the
// stub posts its own ready bytes as it starts), and points the CPU at the entry.
// A machine built without the stub keeps whatever state it was seeded with, which
// is how a program loaded directly into RAM skips the handshake.

#include <array>
#include <cstdint>
#include <span>

namespace snaggletooth {

struct ApuState;

// The stub occupies $FFC0-$FFFF: the program, then its reset vector in the last
// two bytes pointing back at the entry. Sized to the console's boot-ROM window so
// an upload targeting any lower address cannot overwrite a running byte of it.
inline constexpr std::uint16_t kIplStubBase = 0xFFC0u;
inline constexpr std::size_t kIplStubSize = 64u;

// The stub image, address $FFC0 first. Exposed so a test can check the bytes the
// machine seeds; seedIplStub is the seam the machine uses.
[[nodiscard]] const std::array<std::uint8_t, kIplStubSize>& iplStubImage() noexcept;

// Seeds `apu` with a boot image: the 64 bytes at $FFC0, the input and output ports
// cleared, and the CPU pointed at the entry with a clear direct page and the
// post-boot stack pointer. Every other field is left as it was. The image is the
// stub unless one is given, which is how a caller runs a console's own boot ROM.
void seedIplStub(ApuState& apu) noexcept;
void seedIplStub(ApuState& apu, std::span<const std::uint8_t, kIplStubSize> image) noexcept;

}  // namespace snaggletooth
