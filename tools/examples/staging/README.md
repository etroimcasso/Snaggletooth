# staging

One bank, native mode, the data bank at `$7F`. Reset builds eight ranges in
work RAM, every way the shadow that follows values has a rule for, then sends
each to the hardware.

A run-length decoder at `$8100` unpacks eleven bytes at `$9000` — five runs,
each a count and a value, then a zero — into thirty-two bytes at `$7F:0000`;
the count goes to a counter in the direct page and only the value reaches the
output, so the output's origin is the five value bytes and its source is their
span. A copy at `$8140` moves thirty-two bytes from `$9100` to `$7F:0100`
unchanged. A loop at `$8180` carries sixteen bytes from `$9200` to `CGDATA` one
store at a time, then reads the end mark at `$9210`. A fill at `$81A0` writes
sixteen bytes of `$AA` to `$7F:0300` from a constant. A transfer at `$81C0`
moves thirty-two bytes from `$9300` into `$7E:0400` through the work-RAM port;
two stores at `$8300` put the first two bytes of `$9100` into `$7E:0500`
through the same port; a copy at `$8380` moves a three-byte HDMA table from
`$9400` to `$7F:0600`; a copy at `$83C0` moves eight bytes from `$9500` to
`$7F:0700`; a copy at `$8480` moves sixteen bytes from `$9600` to `$7F:0800`.
Then the transfers send `$7F:0000` and `$7F:0100` to VRAM, `$7F:0300` to OAM,
`$7E:0400` and `$7E:0500` to VRAM, and `$7F:0700` to VRAM and then to CGRAM; a
loop at `$84C0` carries `$7F:0800` to `VMDATAL` a word at a time; and channel 1
walks the table in `$7F:0600` to `INIDISP` every frame while the program idles.

What it shows: a decompressed range whose origin is a comb of the value bytes,
reported with its span and lifted from it; a copied range whose origin is
exact; a range built from constants, reported as computed and not lifted; a
range the engine wrote through the port, exact and by no routine; bytes the
CPU wrote through the port, by the routine that stored them, with the whole
block another routine read as their source; a stream the CPU carried from the
image, recorded as the bytes it carried and lifted as the run its loop read,
end mark included; a table HDMA walks out of work RAM, lifted from its source
under `hdma/`; a source built into data for two classes, lifted once under
`staged/` and named for both; and a buffer the CPU carries out itself, an
extent like an engine's, lifted as its source.

Read by `tests/rom/observe_test.cpp` and `tests/rom/rom_disasm_test.cpp`; the
source of the `origin`, `staged` and `streamed` lines in
[`docs/project-manifest.md`](../../../docs/project-manifest.md) and of the
staged lift in [`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
