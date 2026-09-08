# landing

One bank, in emulation mode. Reset uploads to every video memory while the
screen is in forced blank, writes the screen mode and the bases only
afterwards, turns the screen on and idles; the vertical-blank handler counts
frames and does one thing more on each of the first four.

In forced blank, channel 0 carries from the image to VRAM a 64-byte tileset to
word `$3000`, a 64-byte map to `$0000`, sixty-four bytes to `$0FF0`, thirty-two
to `$5000`, a 64-byte sprite sheet to `$6000`, thirty-two bytes to `$2100`
under the 8-bit address translation, and a fill of sixty-four bytes from the
one byte at `$9C00` to `$5C00`; to CGRAM a 32-byte palette at entry sixteen;
to OAM the 544-byte sprite table at `$A000` from byte zero; and thirty-two
bytes from `$9B00` are copied into work RAM through the port and sent from
there to word `$0020`. Then Mode 1, with
BG1's screen at `$0000`, BG2's at `$0400` and two screens wide, BG3's at
`$0C00`, BG1's and BG2's tiles at `$1000`, BG3's at `$1000` too, and the
sprite tiles at `$6000` with their second half past a gap that wraps it round
the end of VRAM to `$2000`. The handler sends the sprite table again
on the first three frames without writing the OAM address; on the second
frame uploads a map to `$7800` and another to `$0200`, and only then points
BG2 at a four-screen map from `$7400`, whose last screen wraps round the end
of VRAM and holds the second; on the
third sets Mode 7, uploads sixty-four bytes to `$0000` and moves the OAM
address to `$010` before that frame's sprite table; on the fourth turns forced
blank on and uploads sixty-four bytes to `$0100`.

What it shows: a `landed` line per range with the words, palette entries or
OAM bytes it reached; an upload made before any base was written, read at
the first drawn frame against the bases that were then in force — a tileset
in the name base two layers share, a map, a range that begins in one layer's
screen and ends in the name bases, a range nothing addresses, a sprite sheet;
a translation scattering sixteen words across a block that the sprite base's
wrapped second half also covers; a fill landing; the
sprite table at byte zero on the frames the OAM address is not written,
because it reloads at the start of vertical blank, and the same range landing
at `$010` on the frame it is, two landings of one range; two maps uploaded
behind a base and read as the map they became when the base was flipped, one
of them through the screen's wrap round the end of VRAM; a Mode 7 upload; and an upload no frame
drew. And the directories that follow: `tiles/`, `maps/` — the staged range's
source among them, placed by where the range it was built into landed — and
`vram/` for the rest.

Read by `tests/rom/observe_test.cpp` and `tests/rom/rom_disasm_test.cpp`; the
source of the `landed` lines in
[`docs/project-manifest.md`](../../../docs/project-manifest.md) and of the
directory rule's example in
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
