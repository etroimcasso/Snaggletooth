# drawing

One bank, in native mode with the data bank at `$7E`. Reset sends, in forced
blank, one of everything an editable form has a grammar for, then uploads a
sound program that keys two voices on; the vertical-blank handler counts
frames and does one thing more on each of the second and the third.

From the image, channel 0 carries to VRAM a 4-bit tileset of three tiles and
a half — 112 bytes — to word `$1000`, a 2-bit tileset of four tiles to
`$5000`, and a sprite sheet of two tiles to `$7000`; to CGRAM a palette of
sixteen words, one with its top bit set, at entry zero; to VRAM a one-screen
tilemap of 2048 bytes that uses every combination of the priority and flip
flags, to word `$0000`; and to OAM the whole 544-byte sprite table from byte
zero. Then Mode 1 with BG1's screen at `$0000`, BG3's at `$0400`, BG1's and
BG2's tiles at `$1000`, BG3's at `$5000` and the sprite tiles at `$6000` with
their second half at `$7000`; the screen on; channel 1 enabled on a direct
HDMA table of unit 2 — one entry of two lines, one of three lines repeated,
then the end — to the window registers, and channel 2 on an indirect table
of unit 1 to the brightness register, whose two entries point at a block of
one byte and a block of three; and the vertical-blank interrupt on. On the
first and the second frames the handler fills thirty-two bytes at `$7E:1000`
from run-length blobs and sends them out ten times through that one buffer,
six on the first frame and four on the second, so each frame's sends fit its
vertical blank: a blob of two parts, each part unpacked and sent to word
`$1300` inside BG1's name base; twenty-four bytes unpacked from one blob and
eight from another at a lower address, sent there together; sixteen from
each of two blobs, sent there together; a second blob of two parts, its first
part sent to word `$5100` inside BG3's name base and its second to `$1300`;
then, on the second frame, twenty-four bytes the handler clears itself and
eight unpacked from a blob, sent to `$1300` together; thirty-two bytes each
of which adds two bytes of one blob to one of another, sent to `$1300`; a
blob sent to word `$1200`; and a blob sent to the palette at entry sixteen.
On the third it
switches to Mode 7 and sends a block of 128 bytes, a map in its even bytes
and tiles in its odd, to word `$0000`.

After the reset code, with the vertical-blank interrupt held off for the
upload's length, it speaks the audio upload protocol: a sound program of 90
bytes to `$0200` from `$A700`, then a sample directory and a two-block sample
to `$0300` from `$A780`, and starts the program. The program sets the
directory at `$0300`, voice 0's source to entry 0 — the uploaded sample at
`$0308`, looping at `$0311` — and voice 1's to entry 1, at `$0330`, where it
writes a first and a last block header over the zero bytes the boot left;
turns the volumes and the DSP on; and keys both voices on at once.

What it shows: a `landed` line's `depth` — 4 for the name base BG1 and BG2
share, 2 for BG3's, 4 for the sprite tiles, 8 under Mode 7, `none` for a
screen, the palette and the sprite table; the palette RAM a tile sheet
carries, as it stood at the frame that read the sheet, and a second copy
for a sheet read after the handler had written more of it; `walked` lines
for both tables and the indirect block, with the unit the channel's transfer
pattern set and the form; a tile sheet at each depth, a `.pal`, a `.map` with
every flag, an `.oam` of a whole table, a `.hdma` under each form, and the
indirect block left as bytes; eleven blobs that are the sources of what one
buffer held, and the previews beside them — a source whose two contents
went to the tiles has one sheet of both, a source that supplied most of a
content owns its preview while the source that supplied the rest has none,
two sources that supplied a content equally give it to the lower address, a
source whose contents went to tiles of two depths has one sheet at the
deeper, a source that supplied eight bytes of a buffer the handler cleared
has none since the handler made more of it, a source that supplied two of
every summed byte's three image bytes owns that buffer, and the blob sent to
the palette has a `.pal` — the Mode 7 block with
a preview of its tiles and one of its map; the bank file including every
encoded file with its length; and two `sample` lines, one for the uploaded
sample, whose bytes the sound program's file holds and whose WAV sits beside
it, and one for the sample the program built, which the image holds nowhere,
its WAV under `apu/samples/`.

Read by `tests/rom/observe_test.cpp`, `tests/rom/rom_disasm_test.cpp` and
`tests/rom/verify_test.cpp`; the source of the `walked`, `preview` and
`sample` lines in [`docs/project-manifest.md`](../../../docs/project-manifest.md),
of the forms' and the listening copy's examples in
[`docs/asset-formats.md`](../../../docs/asset-formats.md), and of the encoded
tree and the samples in
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
