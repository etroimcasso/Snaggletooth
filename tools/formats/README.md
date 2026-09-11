# The asset codecs

`snaggletooth_formats` turns the bytes a cartridge feeds the hardware — planar
tiles, 15-bit palettes, packed map and sprite entries, HDMA programs — into
editable files and back, each exact both ways. The forms are defined in
[docs/asset-formats.md](../../docs/asset-formats.md): a tile sheet as an indexed
PNG, and palettes, tilemaps, OAM and HDMA tables as text.

Each codec encodes SNES bytes to its form and decodes the form back to the same
bytes, so a form is a source an assembly can include in place of a raw `.bin`.

## Contents

- [Surface](#surface)
- [Using it](#using-it)
- [See also](#see-also)

## Surface

Everything lives in `snaggletooth::formats`.

| Symbol | Purpose |
|---|---|
| `Bytes`, `Text` | A codec's result — the output, an `error` empty on success, and `ok()`. |
| `IndexedImage` | An indexed image: per-pixel indexes, a palette, and the bit depth. |
| `decodePng`, `encodePng` | The indexed PNG face over lodepng; refuses a non-indexed image. |
| `encodeTiles`, `decodeTiles` | Planar tile bytes ↔ an indexed PNG sheet at the tile's depth. |
| `encodePalette`, `decodePalette` | CGRAM words ↔ `.pal` text. |
| `encodeTilemap`, `decodeTilemap` | BG map words ↔ `.map` text. |
| `encodeOam`, `decodeOam` | OAM bytes ↔ `.oam` text. |
| `encodeHdma`, `decodeHdma` | An HDMA table ↔ `.hdma` text. |
| `encodingReader` | Wraps an `assembler::Reader` so an included asset is decoded by extension. |

## Using it

```cpp
#include "formats/tiles.h"

const snaggletooth::formats::Bytes png =
    snaggletooth::formats::encodeTiles(planarBytes, /*depth=*/4, palette);
if (!png.ok()) { /* png.error names what was wrong */ }
```

The library target is `snaggletooth_formats`; `tools/` is on its public include
path. PNG encoding and decoding is [lodepng](../../third_party/lodepng/README.md),
built as `snaggletooth_lodepng` and linked privately so no lodepng symbol reaches
a header here. The library has no command line of its own.

## See also

- [docs/asset-formats.md](../../docs/asset-formats.md) — the full page: every
  grammar, worked examples, and the encoding reader.
- [`../assembler/`](../assembler/README.md) — the assembler whose `Reader` the
  encoding reader wraps.
