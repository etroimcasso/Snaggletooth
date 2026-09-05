# moving

One bank, in emulation mode. Reset moves bytes every way the transfer engines
can, then enables the vertical-blank interrupt and idles while the handler
sends a sprite table every frame.

Channel 0 carries a 32-byte tileset from `$9000` to VRAM, then fills 64 bytes
of VRAM from the one byte at `$9100`. Channel 1 reads 16 bytes of VRAM back
into work RAM at `$7E:0300`. Channels 2 and 3 are started by one write to
`MDMAEN`: 16 palette bytes read downward from `$920F` into CGRAM, and 8 bytes
to `$2150`, a B-bus address no register has. A write of zero to `MDMAEN`
follows, and starts nothing. Channel 4 runs a direct HDMA table at `$9500` to
the brightness register; channel 5 an indirect table at `$9510` to the palette
port, whose two blocks lie at `$9522` and `$9520` in the same bank — the second
entry's block ending exactly where the first entry's begins; channel 6 a direct
table of 129 bytes the program first writes into work RAM at `$7E:0400`,
enabled by a second write to `HDMAEN`. Channel 0 then carries 48 bytes from
`$9600` in three chunks of sixteen: the first two started by one instruction in
a subroutine, the third by a long store to `MDMAEN` through bank `$80`. Channel
7 sends 544 bytes from `$7E:0200` to OAM, started from the handler on every
frame.

What it shows: every field of a `moved` line — each kind, each direction, each
step, a register with a name and one without, two channels under one trigger, a
table and the blocks it points at, a range in work RAM, a range seen once a
frame — and where a range ends: at the same instruction starting the channel
again, at a new frame's walk of a table, and at a start register reached through
a mirror bank.

Read by `tests/rom/observe_test.cpp`; the source of the `moved` lines in
[`docs/project-manifest.md`](../../../docs/project-manifest.md) and
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
