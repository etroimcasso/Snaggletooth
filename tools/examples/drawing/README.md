# drawing

One bank, in native mode with the data bank at `$7E`. Reset sends, in forced
blank, one of everything an editable form has a grammar for; the
vertical-blank handler counts frames and does one thing more on each of the
second and the third.

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
second frame the handler decompresses a run-length blob into thirty-two
bytes at `$7E:1000` and sends them to word `$1200`, inside BG1's name base,
then decompresses a second blob into the same thirty-two bytes and sends
them to the palette at entry sixteen. On the third it switches to Mode 7 and
sends a block of 128 bytes, a map in its even bytes and tiles in its odd, to
word `$0000`.

What it shows: a `landed` line's `depth` — 4 for the name base BG1 and BG2
share, 2 for BG3's, 4 for the sprite tiles, 8 under Mode 7, `none` for a
screen, the palette and the sprite table; the palette RAM a tile sheet
carries, as it stood at the frame that read the sheet, and a second copy
for a sheet read after the handler had written more of it; `walked` lines
for both tables and the indirect block, with the unit the channel's transfer
pattern set and the form; a tile sheet at each depth, a `.pal`, a `.map` with
every flag, an `.oam` of a whole table, a `.hdma` under each form, and the
indirect block left as bytes; two blobs that are the sources of what one
buffer held, each with a preview of what it became — a tile sheet, a
palette — and the Mode 7 block with a preview of its tiles and one of its
map; and the bank file including every encoded file with its length.

Read by `tests/rom/observe_test.cpp`, `tests/rom/rom_disasm_test.cpp` and
`tests/rom/verify_test.cpp`; the source of the `walked` and `preview` lines
in [`docs/project-manifest.md`](../../../docs/project-manifest.md), of the
forms' examples in [`docs/asset-formats.md`](../../../docs/asset-formats.md),
and of the encoded tree in
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
