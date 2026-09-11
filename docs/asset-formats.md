# Asset formats

A cartridge's graphics, palettes and tables are bytes the hardware reads directly — planar tiles,
15-bit colours, packed map and sprite entries, HDMA programs. This page defines the editable forms
the toolkit reads and writes for each of them: a tile sheet as an indexed PNG, and the palette,
tilemap, OAM and HDMA tables as text. Each form is exact both ways — the bytes encode to the form
and the form decodes back to the same bytes — so a form is a source the assembler can include in
place of a raw `.bin`.

The codecs live in `snaggletooth_formats` (`tools/formats/`). The assembler reads an included asset
back to its bytes through the encoding reader described at the end of this page.

## Contents

- [The tile sheet (`.png`)](#the-tile-sheet-png)
- [The palette (`.pal`)](#the-palette-pal)
- [The tilemap (`.map`)](#the-tilemap-map)
- [The OAM table (`.oam`)](#the-oam-table-oam)
- [The HDMA table (`.hdma`)](#the-hdma-table-hdma)
- [Including an asset](#including-an-asset)
- [Where to change it](#where-to-change-it)

## The tile sheet (`.png`)

Tiles are an indexed PNG. An 8×8 tile is 16, 32 or 64 bytes for 2, 4 or 8 bits a pixel, the colour
number of each pixel spread across the bit-planes the hardware reads: planes 0 and 1 interleave in
the first sixteen bytes, planes 2 and 3 in the next sixteen, planes 4 through 7 in the last
thirty-two, two bytes a row, and in each byte bit 7 is the left-most pixel. Plane 0 is the low bit
of the colour number.

The PNG is indexed at the tile depth — a 2bpp file is a 2-bit PNG, 4bpp a 4-bit, 8bpp an 8-bit — so
the file carries its own depth and a pixel value is a colour number, not a colour. The sheet is
sixteen tiles wide, `ceil(tiles ÷ 16) × 8` pixels tall, tiles in file order left to right and down.

Most tile files are not a whole number of tiles. The last tile is padded with zero pixels, and the
bank file that includes the sheet carries the byte length, so the padding past the file's end is
never assembled:

```
INCBIN "tiles/00_9000.png", 0, 1000
```

Decoding yields whole tiles up to the sheet's last row; the length clips the result back to the
file. The PNG's palette is a viewing aid — the index is the data, and a person may recolour the
palette freely without changing a pixel's colour number.

Only indexed PNGs are read. A truecolour, greyscale or 16-bit PNG is refused, naming its colour
type, because its pixels are colours rather than the colour numbers a tile is made of.

## The palette (`.pal`)

CGRAM holds one 15-bit colour a word: five bits each of blue, green and red, low bits to high, and a
top bit the PPU ignores. The text writes one word a line, the word verbatim as a four-digit hex
number, then the colour it names at eight bits a channel as a comment:

```
$0000 ; 0 0 0
$7FFF ; 255 255 255
$001F ; 255 0 0
```

The number is the data. The comment is not read back, so a person recolours by editing the word;
the eight-bit channels spread the five-bit value so `0` stays black and `31` reaches full `255`. A
run of an odd number of bytes ends with its lone byte as `$XX`.

## The tilemap (`.map`)

A BG map entry is a 16-bit word: a 10-bit tile number, a 3-bit palette, and priority, horizontal-flip
and vertical-flip bits. The text writes each entry as `tile:palette:flags` — the tile in hex, the
palette `0`–`7`, and the flags `P` (priority), `H` and `V` in that order, or `-` for none:

```
000:0:- 1A3:2:PH 055:7:V 3FF:0:PHV
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
$50 $48 155 2:1:HV
$60 $48 042 0:0:-
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
entry carries a single `$XXXX` pointer in place of the data. Bytes the file holds past the table's
end — a table a frame cut short — follow as `$XX` lines.

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
exactly. A file that does not decode — a `.png` that is not indexed, a line that does not parse —
reads as nothing, and the assembler reports the include naming the reason.

## Where to change it

The codecs are in `tools/formats/`: `png.{h,cpp}` and `tiles.{h,cpp}` for the tile sheet,
`palette`, `tilemap`, `oam` and `hdma` for the text forms, and `reader.{h,cpp}` for the encoding
reader. Each is covered by `tests/formats/`. PNG encoding and decoding is
[lodepng](../third_party/lodepng/README.md), vendored under `third_party/`. The assemblers that
include these files are described in [assemblers.md](assemblers.md); the lexicon's `INCBIN` is in
[assembly-lexicon.md §5.4](assembly-lexicon.md).
