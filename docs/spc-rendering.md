# Rendering audio

Snaggletooth writes the [S-DSP](dsp.md)'s 32 kHz stereo output to a WAV file from
either of two sources. `spc_render` takes an SPC dump, a snapshot of the audio
machine on its own. `rom_render` takes a cartridge and boots the whole console, so
the game's own code drives its sound driver. Both write the same canonical PCM WAV.

An SPC dump (`.spc`) is a snapshot of the SNES audio machine at a moment during
playback: the SPC700's registers, the full 64KB of APU RAM, and the S-DSP's 128
registers. `spc_render` loads a dump into the machine, runs it forward, and writes
the output to a WAV file — the first way to hear the audio core end to end.

The dump format and the WAV output are both plain container formats; the loader and
writer bring in no third-party code. The machine being restored is Snaggletooth's
own — the loader maps the dump's bytes onto the [APU machine](apu-machine.md)'s
state and hands it to `Apu`.

## The `spc_render` tool

```
spc_render <in.spc> (--seconds N | --samples N) -o <out.wav>
```

Exactly one of `--seconds` or `--samples` gives the render length; `-o` names the
output file. The tool renders from sample zero, verbatim: no warm-up, no skip, no
fade — exactly the requested number of 32 kHz samples from the restored state.

```
spc_render song.spc --seconds 30 -o song.wav
```

The output is a canonical PCM WAV at 32000 Hz, 16-bit signed, stereo — the DSP's
digital output written sample for sample, with no resampling, dither, or gain.

## What a dump carries — and what it doesn't

A dump snapshots registers and RAM, not the DSP's moment-to-moment internal state.
The format carries the CPU registers, the 64KB RAM image, and the 128 DSP registers.
It does **not** carry:

- the timer stage counters and the shared divider,
- the communication-port latches on either side,
- the DSP's per-voice envelopes, pitch counters, and BRR sample windows,
- the shared noise level and the echo delay line's history,
- the CPU run state.

On load, the values that live inside the RAM image are recovered from it: `CONTROL`,
`DSPADDR`, the three timer targets and their readable outputs, and both faces of the
communication ports all sit in the `$F0`–`$FF` region and are read back from there.
Everything the format omits is seeded to the machine's power-on values — the timer
and DSP dividers to zero, the noise level to its power-on seed, the echo ring empty,
and every voice keyed off. A dumped `KON` register is a read-back value, not a
command: the loader never keys a voice on from it.

The extra-RAM field (64 bytes at the end of the file) overwrites `$FFC0`–`$FFFF`
only when the dumped `CONTROL` bit 7 was set — the case where the dump was taken
with the boot-ROM window mapped read-only, so the RAM image could not hold those
bytes. Snaggletooth has no console boot ROM, and the `.spc` loader maps no image
over the window: it restores those bytes into plain RAM and reads them back there
regardless of `CONTROL` bit 7. (A machine can map an original boot program over the
window to run the console's upload handshake — the APU machine's
[boot-ROM window](apu-machine.md#the-boot-rom-window) — but the `.spc` path never
does; see also the [README](../README.md).)

Because the DSP's internal state is not in the format, a dump taken **at track
start** reconstructs exactly — nothing is mid-decay, and the seeded power-on state
is the true state. A dump taken **mid-track** restarts the envelopes, echo, and
noise from power-on when it resumes; the sound converges as the driver re-keys its
voices, but the first moments differ from where the dump was taken. Dumps are
conventionally taken at track start for this reason.

## The `rom_render` tool

```
rom_render <in.smc> (--seconds N | --samples N) -o <out.wav> [--quiet]
```

`rom_render` boots a cartridge on the [SNES machine](snes-machine.md) and renders
what its sound driver plays. The arguments match `spc_render`'s: exactly one of
`--seconds` or `--samples` gives the length, `-o` names the output. Progress prints
a second at a time — the frames carrying sound and the voices sounding — which
`--quiet` suppresses.

```
rom_render game.smc --seconds 90 -o game.wav
```

This is the answer to the limits above: a cartridge supplies the command stream a
dump cannot carry. The 65816 runs the game, so song changes, sound effects and
per-frame parameter writes all reach the audio unit as they do on the console,
and a render can run as long as the game keeps playing.

Rendering starts at power-on and runs forward verbatim, so the opening seconds are
whatever the game does before it starts its driver — silence, usually, while it
uploads one. A dump copier's 512-byte header is dropped when the file length shows
one is present. The cartridge is mapped as LoROM, so a HiROM title does not yet
boot.

## Loading a dump in code

The loader is a pure function — dump bytes in, machine state out (or a reason it was
rejected):

```cpp
#include "snaggletooth/apu/apu.h"
#include "spc/spc_loader.h"

std::vector<std::uint8_t> bytes = /* the .spc file's contents */;

snaggletooth::spc::SpcLoad load = snaggletooth::spc::parseSpc(bytes);
if (!load.state) {
  // load.error names the offending field — bad magic, missing marker, or a file
  // shorter than the 66,048-byte machine-complete minimum.
  return;
}

snaggletooth::Apu apu{*load.state};
apu.run(32000 * 32);  // one second of audio time (32000 samples * 32 cycles)
std::vector<snaggletooth::StereoFrame> audio = apu.takeFrames();
```

`parseSpc` does no file I/O; the caller reads the bytes. It validates the magic (the
`"SNES-SPC700 Sound File Data v"` prefix; the version digits after it are not
checked), the `26,26` marker, and the minimum length, then maps the CPU registers,
RAM, and DSP registers and applies the recovery and seeding above.

## The WAV output

`writeWav` turns the frames into WAV bytes:

```cpp
#include "spc/wav_writer.h"

std::vector<std::uint8_t> wav = snaggletooth::spc::writeWav(audio, 32000);
// wav is a complete file: a 44-byte RIFF/fmt /data header followed by the frames.
```

The header is the canonical 44-byte layout — PCM, 2 channels, 16-bit, at the given
rate — and each frame is written as `left` then `right`, 16-bit little-endian. An
empty frame span yields the header alone with a zero-length data chunk. As with the
loader, `writeWav` returns the bytes; the caller writes the file.
