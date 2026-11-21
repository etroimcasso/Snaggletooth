# zstd (vendored)

[Zstandard](https://facebook.github.io/zstd/) is the compressor and decompressor by Meta
Platforms. The toolkit uses it to write and read a tree's run records
(`runs/NNNN.snagrun.zst`, [docs/run-record.md](../../docs/run-record.md)).

- **Files:** `lib/common/`, `lib/compress/` and `lib/decompress/` without
  `huf_decompress_amd64.S`, `lib/zstd.h`, `lib/zstd_errors.h` and `LICENSE`, copied verbatim
  from the release tarball.
- **Release:** 1.5.7 (`ZSTD_VERSION_MAJOR`, `_MINOR`, `_RELEASE`), `zstd-1.5.7.tar.gz` from the
  project's GitHub release, SHA-256
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`.
- **License:** BSD (see `LICENSE`), the side of zstd's dual BSD and GPLv2 license this project
  takes. A source distribution keeps the notice, which this directory does; a binary
  distribution reproduces the notice, its conditions and its disclaimer in its documentation or
  other materials; and nothing built with it carries Meta's name as an endorsement.

The sources are unaltered. `snaggletooth_zstd` builds them as a static C library with a `SYSTEM`
include so their warnings are not the toolkit's, with `ZSTD_DISABLE_ASM` and without
`ZSTD_MULTITHREAD`, so every platform runs the same C on one thread; it is linked privately into
`snaggletooth_rom` so no zstd symbol reaches a toolkit header.
