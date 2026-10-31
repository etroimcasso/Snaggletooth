# enumerating

One bank, native mode, the data bank at `$7F`, the direct register at `$0000`,
the stack at `$1FFF`, and the vertical-blank interrupt on, its handler at
`$8E00` counting frames in `$7E:0020` and calling the fifth loader. Five
loaders build what they send from tables of entries, and each is called for
only some of its entries.

The first loader, `sub_008100`, takes an index in X, sixteen bits, reads a
pointer from the table at `$8200`, and builds the blob it points at into
`$7F:0000`. A blob is a kind byte, a length byte and the bytes: kind 0 is raw,
copied a byte at a time; kind 1 is run-length, pairs of a count and a value;
kind 3 writes `$80` to `INIDISP` and then copies raw; any other kind is a loop
that never ends. The loader then sends the length to VRAM on channel 0. The
pointer goes to `$00` in the direct page, the length to `$04`, a run's count to
`$06`. The table's eight entries are raw at `$8300`, run-length at `$8340`, raw
at `$8380`, kind 3 at `$83C0`, raw at `$8400`, run-length at `$8440`, the
endless kind at `$8480`, and raw of 255 bytes at `$84C0`; every other entry
builds 32. Reset loads an index from `$7E:0010`, masks it to three bits and
doubles it, and calls the loader twice: for entry 2, then entry 5.

The second loader, `sub_008500`, is the same body over a table of four at
`$8600`, its index loaded from `$7F:0012` inside it; reset calls it for the
entry at `$8380`. Reset then jumps through the pointer at `$807C` to
`loc_008700`, which copies sixteen bytes from `$9000` to `$7F:0100`, sends them
to VRAM on channel 1 and jumps back to `$807E`.

The third loader, `sub_008800`, takes an index in A at sixteen bits, which reset
bounds with a compare against four and a branch on the carry. It copies the
sixteen bytes of the blob the table at `$8900` points at into `$7F:0200`, and
reset sends them to CGRAM on channel 2. The fourth, `sub_008B00`, takes an index
in A at eight bits, which reset masks to two bits. It copies the blob the table
at `$8C00` points at into `$7F:0300`, waits for the vertical blank, and sends
the bytes to OAM on channel 3. Both tables point at the four blobs at `$8A00`,
`$8A20`, `$8A40` and `$8A60`. Reset calls them for entries 1 and 2, then idles.

The fifth loader, `sub_008D00`, takes an index in A at sixteen bits and writes
X with `TAX` before anything reads it. It reads a pointer from the table at
`$8F00`, whose blobs are a build length, a send length and the bytes; it copies
the build length into `$7F:0400` and sends the send length to VRAM on channel
4. The handler loads an index from `$7E:0024` masked to three bits into X and
one from `$7E:0022` into A, and calls the loader when A is below five. Work RAM
powers on zero, so every frame calls entry 0. The five entries: `$9100` builds
48 bytes and sends 32; `$9140` builds 48 and sends them all; `$9180` builds 48
and sends 16; `$8A10` builds the 32 bytes from `$8A12`, across the blob at
`$8A20`, and sends them all; `$8A30` builds the 48 from `$8A32`, across the blob
at `$8A40`, and sends 16.

What it shows, with a run of one second:

- **The plan.** The four loaders whose index a caller bounds are planned:
  `sub_008100` with X over eight values at sixteen bits, `sub_008800` with A
  over four at sixteen, `sub_008B00` with A over four at eight, and `sub_008D00`
  with A over five at sixteen. The handler bounds `sub_008D00`'s X too, but the
  loader writes X before it reads it, so X is noted and not set. The loader that
  loads its own index, the reset routine, the routine reset jumps to and the
  handler are each named in a note and not run.
- **What is kept and lifted.** Five entries of the first loader are kept, each
  built from its own blob, header included. The values the run itself selected
  build their recorded files, which stay `staged`. The other three are files of
  their own, `enumerated`. Every entry of the third and fourth loaders is kept.
  The run sent the second blob to the palette and the third to the sprite
  table, each a file of its one class. Every entry that builds one of those
  blobs joins its file: the palette loader's entry for the third blob makes the
  sprite file's line say `also Cgram`, and the sprite loader's entry for the
  second makes the palette file's say `also Oam`. The first and fourth blobs,
  which no recorded file holds, are each one file under `staged/` with a
  preview of what each value built beside it. Every entry of the fifth loader
  is kept: entry 0's first 32 bytes are the run's file, `vram/00_9100.bin`;
  entry 1 sends all 48 on its own channel, a file of its own; entry 2's first
  32, which the recorded extent holds, are a file of their own; entry 3 reads
  from `$8A10`, its two header bytes and the 32 after them, so the palette file
  widens to `cgram/00_8A10.bin` and its line says `also Vram+Oam`.
- **What is refused.** Entry 3 of the first loader writes `INIDISP`, a register
  the run never saw the loader write. Entry 6 never returns within a second of
  the master clock. Entry 7 writes past `$7F:001F`, outside what the run saw.
  The fifth loader's entry 3 is kept, but its piece that carries the recorded
  VRAM extent's landings into the palette file would change that file's form,
  and is a note naming the file. Entry 4 reads from `$8A30`, across the widened
  palette file and the sprite file, and is a note naming both.
- **What is scratch.** The pointer, the length and the count each loader keeps
  in the direct page, and the 16 bytes the fifth loader builds past what the
  run carried, are noted once for each loader as extents the run never carried.

Read by `tests/rom/enumerate_test.cpp`, `tests/rom/rom_disasm_test.cpp` and
`tests/ir/render_test.cpp`; the source of the `enumerated` lines in
[`docs/project-manifest.md`](../../../docs/project-manifest.md) and of the
section "What the toolkit ran" in
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
