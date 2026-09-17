#pragma once

// The picture the machine hands to whoever is watching it.
//
// A host that wants to see what a cartridge draws, rather than read the video
// memories and work out the picture itself, sets a FrameObserver on the machine.
// It is told every frame the PPU finishes, as the beam wraps to the frame's first
// line: the raster the chip's converter drove, its size, and the frame's parity.
//
// The observer is the host's object and outlives every step it is set for. It is
// not part of the machine's state, so a snapshot does not carry it and restore()
// leaves it in place — the bus observer's terms exactly. The PPU draws pixels
// only while one is set; with none, a machine keeps its registers, its memories
// and what the chip carries from one position to the next exactly as a watched
// machine does, and draws nothing.

#include <cstdint>
#include <span>

namespace snaggletooth {

// One finished picture. The pixels are row-major from the top line, four bytes
// each — red, green, blue, then 255 — `width` across and `height` down. The span
// is the machine's own buffer: it is valid for the call and a host that keeps the
// picture copies it.
//
// A frame any line of which was drawn in half-pixels is 512 wide: the sub screen's
// half of each position on the even columns, the main screen's on the odd, and
// every line that was not split showing each pixel twice. An interlaced run hands
// over one field a frame, each with its own parity; a host that weaves them may.
struct VideoFrame {
  std::span<const std::uint8_t> pixels;
  unsigned width = 0;      // 256, or 512 when any line of the frame was drawn in half-pixels
  unsigned height = 0;     // 224, or 239 when SETINI asks for the taller picture
  std::uint8_t field = 0;  // the frame's parity, for a host that weaves two of them
};

class FrameObserver {
 public:
  virtual ~FrameObserver() = default;

  // A frame the PPU has finished. It arrives on the thread the machine is stepped
  // on, from inside step() or run(), as the beam reaches the next frame's first
  // line.
  virtual void frame(const VideoFrame& frame) = 0;
};

}  // namespace snaggletooth
