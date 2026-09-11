# lodepng (vendored)

[lodepng](https://github.com/lvandeve/lodepng) is a single-file PNG encoder and
decoder by Lode Vandevenne. The toolkit uses it to read and write the indexed
PNG sheets its tile codec produces.

- **Files:** `lodepng.h`, `lodepng.cpp`, copied verbatim from upstream.
- **Upstream commit:** `ed6fe58`.
- **Version string:** `20260119` (`LODEPNG_VERSION_STRING`).
- **Licence:** zlib (see `LICENSE`). The three conditions — no misrepresentation
  of origin, altered versions marked as such, notice kept — are met by this file
  and the unmodified sources.

The sources are unaltered. `snaggletooth_lodepng` builds them as a static library
with a `SYSTEM` include so their warnings are not the toolkit's, linked into
`snaggletooth_formats` privately so no lodepng symbol reaches a toolkit header.
