# Asset formats

A cartridge's graphics, palettes and tables are bytes the hardware reads directly — planar tiles,
15-bit colours, packed map and sprite entries, HDMA programs. This page defines the editable forms
the toolkit reads and writes for each of them: a tile sheet as an indexed PNG, and the palette,
tilemap, OAM and HDMA tables as text. Each form is exact both ways — the bytes encode to the form
and the form decodes back to the same bytes — so a form is a source the assembler can include in
place of a raw `.bin`.

The codecs live in `snaggletooth_formats` (`tools/formats/`). The [cartridge
disassembler](snes-disassembler.md#the-assets) writes a lifted file in its form where the run's facts
name one, and the assembler reads an included asset back to its bytes through the encoding reader
described at the end of this page. Every example below is the `drawing` cartridge's from
[`tools/examples/`](../tools/examples/README.md).

## Contents

- [The tile sheet (`.png`)](#the-tile-sheet-png)
- [The palette (`.pal`)](#the-palette-pal)
- [The tilemap (`.map`)](#the-tilemap-map)
- [The OAM table (`.oam`)](#the-oam-table-oam)
- [The HDMA table (`.hdma`)](#the-hdma-table-hdma)
- [Which form a lifted file takes](#which-form-a-lifted-file-takes)
- [Previews](#previews)
- [Including an asset](#including-an-asset)
- [Where to change it](#where-to-change-it)

## The tile sheet (`.png`)

Tiles are an indexed PNG. An 8×8 tile is 16, 32 or 64 bytes for 2, 4 or 8 bits a pixel, the colour
number of each pixel spread across the bit-planes the hardware reads: planes 0 and 1 interleave in
the first sixteen bytes, planes 2 and 3 in the next sixteen, planes 4 through 7 in the last
thirty-two, two bytes a row, and in each byte bit 7 is the left-most pixel. Plane 0 is the low bit
of the colour number.

The PNG is indexed at the tile depth — a 2bpp file is a 2-bit PNG, 4bpp a 4-bit, 8bpp an 8-bit — so
the file carries its own depth and a pixel value is a colour number, not a colour. The sheet is as
wide as its tiles up to sixteen — a one-tile file is 8 × 8, a five-tile file 40 × 8 — and a longer
file wraps at sixteen, `ceil(tiles ÷ 16) × 8` pixels tall, tiles in file order left to right and
down. Decoding accepts any width that is a whole number of tiles, so a sheet a person has widened
or narrowed in an editor still reads.

Most tile files are not a whole number of tiles. The last tile is padded with zero pixels, and the
bank file that includes the sheet carries the byte length, so the padding past the file's end is
never assembled:

```
INCBIN "tiles/00_9000.png", 0, 1000
```

Decoding yields whole tiles up to the sheet's last row; the length clips the result back to the
file. The PNG's palette is a viewing aid — the index is the data, and a person may recolour the
palette freely without changing a pixel's colour number. The disassembler writes the palette the
run held at the frame that read the sheet's landing: the first `2^depth` entries of palette RAM,
palette 0 of that depth, with each five-bit channel spread to eight bits; entries 128–143 for a
sheet the sprite tiles alone use; all 256 for a sheet at eight bits a pixel; and a ramp from black
to white where no frame drew the sheet. A tree written without a run keeps the palette the sheet on
disk carries.

Only indexed PNGs are read. A truecolour, greyscale or 16-bit PNG is refused, naming its colour
type, because its pixels are colours rather than the colour numbers a tile is made of.

## The palette (`.pal`)

CGRAM holds one 15-bit colour a word: five bits each of blue, green and red, low bits to high, and a
top bit the PPU ignores. The text writes one word a line, the word verbatim as a four-digit hex
number, then the colour it names at eight bits a channel as a comment:

```
$0000 ; 0 0 0
$0421 ; 8 8 8
$0842 ; 16 16 16
$0C63 ; 24 24 24
```

The number is the data. The comment is not read back, so a person recolours by editing the word;
the eight-bit channels spread the five-bit value so `0` stays black and `31` reaches full `255`. A
run of an odd number of bytes ends with its lone byte as `$XX`. The top bit of a word is the
image's — the PPU ignores it — and is written as it lies.

## The tilemap (`.map`)

A BG map entry is a 16-bit word: a 10-bit tile number, a 3-bit palette, and priority, horizontal-flip
and vertical-flip bits. The text writes each entry as `tile:palette:flags` — the tile in hex, the
palette `0`–`7`, and the flags `P` (priority), `H` and `V` in that order, or `-` for none:

```
000:0:- 001:1:P 002:2:H 003:3:PH 004:4:V 005:5:PV 006:6:HV 007:7:PHV 008:0:- 009:1:P 00A:2:H …
```

Thirty-two entries make one screen row, one row a line; a blank line follows every thirty-two rows,
one 32×32 screen. A length that is not a whole number of words ends with its lone byte as `$XX`.

## The OAM table (`.oam`)

The sprite table is 128 four-byte entries — the low table — followed by 32 bytes of two bits a
sprite — the high table. A file is usually part of one, sent to the hardware a few sprites at a time,
so the text writes the bytes as they lie.

Each whole sprite in the low table is a line `$XX $YY tile attr`: the X and Y bytes, the 9-bit tile
number in hex, and the attributes as `palette:priority:flags`, the flags `H` (horizontal flip) and
`V` (vertical flip) or `-`:

```
$00 $00 000 0:0:-
$02 $01 103 1:1:H
$04 $02 006 2:2:V
$06 $03 109 3:3:HV
```

A file that reaches into the high table writes each high-table byte, after a blank line, as its four
`size:x9` pairs — the object-size bit and the ninth X bit of the four sprites the byte covers:

```
1:0 0:1 0:0 1:1
```

A low-table byte left over before a whole sprite is written as `$XX`.

## The HDMA table (`.hdma`)

An HDMA table is a program the transfer engine runs down the frame. Each entry begins with a count
byte — how many scanlines it lasts, and whether it repeats — followed by the data itself in direct
mode, or a pointer to it in indirect mode. A zero count ends the table.

The transfer unit and the mode are the one thing the bytes do not carry, so the first line states
them, and each entry is a line of its own; `end` is the terminating zero:

```
unit 2 direct
lines 2 $AA $BB
lines 3 repeat $01 $02 $03 $04 $05 $06
end
```

`lines <n>` transfers one unit and holds for `n` scanlines; `lines <n> repeat` transfers one unit a
line for `n` lines, so a direct repeat entry carries `unit × n` data bytes. In indirect mode each
entry carries a single `$XXXX` pointer in place of the data:

```
unit 1 indirect
lines 3 $A420
lines 3 repeat $A421
end
```

Bytes the file holds past the table's end — a table a frame cut short — follow as `$XX` lines. The
unit and the mode are the channel's `DMAP` at the walk, which the run records as the manifest's
[`walked` line](project-manifest.md#218-how-a-table-was-walked); the block an indirect entry points
at is the data itself and stays bytes.

## Which form a lifted file takes

The disassembler names a file's form by what the run saw its bytes become, and writes the form only
where the facts allow it. A file under `tiles/` — every landing in a name base or the sprite tiles —
is a `.png` when every landing was read at one depth, at that depth, with the palette above; a file
under `maps/` is a `.map`, under `cgram/` a `.pal`, under `oam/` an `.oam`; an HDMA table under
`hdma/` is a `.hdma` when the engine walked it under one unit and one mode. Everything else stays
`.bin`: a file under `vram/`, `apu/` or `staged/`, a block an indirect entry points at, a source a
routine built its data from — compressed, packed, indexed — and a stream's file wider than the bytes
it carried. Before it writes a form the disassembler decodes it back and compares; a form that does
not give the bytes back is written as `.bin` with a note saying which file and why. A file shorter
than one unit of its form — one tile at its depth, one word, one map entry, one sprite, one HDMA
entry with its data — is `.bin` without a note.

The `drawing` cartridge's tree:

```
asset    tiles/00_9000.png Vram as dma from $00:9000 bytes 112
asset    tiles/00_9100.png Vram as dma from $00:9100 bytes 64
asset    tiles/00_9200.png Vram as dma from $00:9200 bytes 64
asset    cgram/00_9300.pal Cgram as dma from $00:9300 bytes 32
asset    maps/00_9400.map Vram as dma from $00:9400 bytes 2048
asset    oam/00_9C00.oam Oam as dma from $00:9C00 bytes 544
asset    hdma/00_A400.hdma Window as table from $00:A400 bytes 11
asset    hdma/00_A410.hdma Display as table from $00:A410 bytes 7
asset    hdma/00_A420.bin Display as indirect from $00:A420 bytes 4
asset    staged/00_A500.bin Vram+Cgram as staged from $00:A500 bytes 9
asset    staged/00_A520.bin Vram+Cgram as staged from $00:A520 bytes 17
asset    staged/00_A540.bin Vram+Cgram as staged from $00:A540 bytes 18
asset    staged/00_A560.bin Vram+Cgram as staged from $00:A560 bytes 3
asset    staged/00_A580.bin Vram+Cgram as staged from $00:A580 bytes 7
asset    staged/00_A5A0.bin Vram+Cgram as staged from $00:A5A0 bytes 5
asset    staged/00_A5B0.bin Vram+Cgram as staged from $00:A5B0 bytes 5
asset    staged/00_A5C0.bin Vram+Cgram as staged from $00:A5C0 bytes 18
asset    staged/00_A5E0.bin Vram+Cgram as staged from $00:A5E0 bytes 3
asset    staged/00_A5E8.bin Vram+Cgram as staged from $00:A5E8 bytes 8
asset    staged/00_A5F4.bin Vram+Cgram as staged from $00:A5F4 bytes 8
asset    vram/00_A600.bin Vram as dma from $00:A600 bytes 128
```

The three sheets are at four, two and four bits a pixel — BG1's name base, BG3's and the sprite
tiles under Mode 1; the eleven blobs are sources, and the Mode 7 block is a map and tiles in one
file.
A tree disassembled without a run writes each file in the form its manifest path names, reading a
sheet's depth and palette and a table's unit from the file already on disk.

## Previews

A file the disassembler cannot turn into a source gets a picture beside it, which nothing includes
and no tool reads back. A source a routine built its data from keeps its bytes, and beside it the
run writes one preview per form its contents took — `<name>-tiles.png`, `<name>-palette.pal`,
`<name>-map.map`, `<name>-oam.oam`, `<name>-hdma.hdma` — each holding every distinct content the
run built from the source and sent out in that form, in the order it first sent each: tiles as one
sheet, each content padded to whole tiles and laid end to end, as wide as its tiles up to sixteen,
at the deepest depth among the contents with the palette of the first content at that depth; a text
form as one file, a blank line between contents. A content belongs to the source that supplied the
most of its bytes — each byte counted for the source its own origin names — and to the lower address
when two supplied the same; the other sources show nothing of it. A content the routine made more of
itself than any source supplied — cleared, computed, assembled from constants — is the routine's own
work, and no source shows it. A Mode 7 file under `vram/`, whose even bytes are a map and whose
odd bytes are tiles, gets two: `<name>-tiles.png`, the odd bytes as an 8-bit sheet sixteen tiles
wide with one byte a pixel and no bit-planes, and `<name>-map.map`, the even bytes as tile numbers,
each `$XX`, thirty-two a line:

```
$00 $01 $02 $03 $04 $05 $06 $07 $08 $09 $0A $0B $0C $0D $0E $0F $10 $11 $12 …
```

The manifest's [`preview` line](project-manifest.md#219-previews) says what each is of, in what
form, and how many contents it holds. The `drawing` cartridge fills one buffer from eleven blobs
and sends it out ten times — a blob of two parts, a part at a time, to the tiles; twenty-four
bytes from one blob and eight from another to the tiles together; sixteen from each of two blobs
to the tiles together; a second two-part blob, its first part to tiles of two bits a pixel and its
second to tiles of four; twenty-four bytes it clears itself and eight from a blob, to the tiles
together; a buffer each byte of which adds two bytes of one blob to one of another, to the tiles;
one blob to the tiles; one to the palette — and sends a Mode 7 block:

```
preview  staged/00_A500-tiles.png of staged/00_A500.bin as tiles contents 1
preview  staged/00_A520-palette.pal of staged/00_A520.bin as palette contents 1
preview  staged/00_A540-tiles.png of staged/00_A540.bin as tiles contents 2
preview  staged/00_A580-tiles.png of staged/00_A580.bin as tiles contents 1
preview  staged/00_A5A0-tiles.png of staged/00_A5A0.bin as tiles contents 1
preview  staged/00_A5C0-tiles.png of staged/00_A5C0.bin as tiles contents 2
preview  staged/00_A5E8-tiles.png of staged/00_A5E8.bin as tiles contents 1
preview  vram/00_A600-tiles.png of vram/00_A600.bin as mode7-tiles
preview  vram/00_A600-map.map of vram/00_A600.bin as mode7-map
```

The first two-part blob's sheet is two tiles wide, its first part's tile then its second's. The
blob of twenty-four owns the content it shared with the blob of eight, whose file has no preview;
of the two blobs of sixteen, the lower address owns theirs. The second two-part blob's sheet is
three 4-bit tiles — the two 2-bit tiles of its first part, their upper planes zero, then the 4-bit
tile of its second — with the palette the 4-bit content carried. The blob that supplied eight bytes
of a buffer the routine cleared has no preview: the routine made more of that content than the blob
did. Of the two blobs summed into one buffer, the one that supplied two of every byte's three owns
it, and the other has no preview.

## Including an asset

An assembler reads an included file's bytes through a reader. The encoding reader wraps one so that,
by the path's extension, an asset is decoded back to its SNES bytes before the assembler places
them:

| Extension | Decoded to |
|---|---|
| `.png` | planar tile bytes |
| `.pal` | CGRAM words |
| `.map` | BG map words |
| `.oam` | OAM bytes |
| `.hdma` | an HDMA table |

Every other extension passes through unchanged, so a `.bin` include is the bytes on disk. An offset
and length in the `INCBIN` address the decoded bytes, which is what lets a padded tile sheet assemble
exactly; the disassembler writes both for every encoded file it includes. A file that does not
decode — a `.png` that is not indexed, a line that does not parse — reads as nothing, and the
assembler reports the include naming the reason. `snes_verify` and the two command-line assemblers
read through this reader, so an edited sheet or table assembles as its bytes.

## Where to change it

The codecs are in `tools/formats/`: `png.{h,cpp}` and `tiles.{h,cpp}` for the tile sheet and the
Mode 7 sheet, `palette`, `tilemap` (the Mode 7 map with it), `oam` and `hdma` for the text forms,
and `reader.{h,cpp}` for the encoding reader. Each is covered by `tests/formats/`. PNG encoding and
decoding is [lodepng](../third_party/lodepng/README.md), vendored under `third_party/`. Which form
a lifted file takes, and the previews, are the disassembler's in `tools/rom/rom_disasm.cpp`, under
the facts `tools/rom/rom_observe.cpp` records — the depth, the palette and the unit — and are
covered by `tests/rom/`. The assemblers that include these files are described in
[assemblers.md](assemblers.md); the lexicon's `INCBIN` is in
[assembly-lexicon.md §5.4](assembly-lexicon.md).
