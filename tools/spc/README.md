# SPC dump tools

`spc_render` plays an SPC dump — a snapshot of the audio machine on its own — and
writes what it plays as a WAV. Beside it are the two file-format pieces both
renderers use: the `.spc` loader and the WAV writer.

## Contents

- [`spc_render`](#spc_render)
- [The loader](#the-loader)
- [The WAV writer](#the-wav-writer)
- [See also](#see-also)

## `spc_render`

```
spc_render <in.spc> (--seconds N | --samples N) -o <out.wav>
```

Loads the dump into the audio machine, runs it forward for exactly the requested
length, and writes the S-DSP's output as a canonical PCM WAV at 32000 Hz, 16-bit
signed, stereo — the digital output sample for sample, with no resampling, dither
or gain.

```
spc_render song.spc --seconds 30 -o song.wav
```

## The loader

An `.spc` carries the SPC700's registers, the 64 KB of APU RAM and the 128 DSP
registers, and nothing of the machine's internal state — the timer counters, the
communication-port latches, the voices' envelopes and pitch counters, the echo
ring. `parseSpc` maps what the file carries onto an `ApuState`, recovers the
registers that ride inside the RAM image's `$F0`–`$FF` bytes, and seeds everything
the format omits to the machine's power-on values.

```cpp
#include "spc/spc_loader.h"

const snaggletooth::spc::SpcLoad load = snaggletooth::spc::parseSpc(bytes);
if (!load.state) { /* load.error names the field that failed */ }
```

A malformed or short image is rejected with a reason rather than loaded partway.

## The WAV writer

```cpp
#include "spc/wav_writer.h"

std::vector<std::uint8_t> file = snaggletooth::spc::writeWav(frames, 32000);
```

Returns the whole file as bytes — a 44-byte header and the frames as 16-bit
little-endian signed samples, left and right interleaved. The caller writes them.

The library target is `snaggletooth_spc`; `tools/` is on its public include path.
`rom_render` links it for the writer.

## See also

- [docs/spc-rendering.md](../../docs/spc-rendering.md) — the full page: both
  renderers, what a dump does and does not carry, and the WAV output.
- [docs/apu-machine.md](../../docs/apu-machine.md) — the machine a dump is restored
  into.
