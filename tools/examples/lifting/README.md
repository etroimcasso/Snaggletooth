# lifting

Two banks, in emulation mode. Reset sends bytes from the image to the hardware
in every way the asset pass has a rule for, then enables one HDMA table and
idles while the frames walk it.

Channel 0 carries a 64-byte tileset from `$9000` to VRAM, then sixteen bytes
from inside it and thirty-two bytes across its end — three ranges sharing
bytes, which the pass lifts as one file of eighty. Then a palette read
downward from `$920F` into CGRAM, a 544-byte sprite table from `$9300` to OAM,
and eight bytes from `$9600` to the audio port. The same sixteen bytes at
`$9800` go to VRAM and then to CGRAM, which the pass refuses; the reset
routine's own first sixteen bytes go to VRAM, which it refuses too. A copy from
`$9900` into work RAM through the port and a fill of VRAM from the one byte at
`$9A00` are not assets and are left where they are. A 32-byte tileset from
`$01:8000` is the second bank's; thirty-two bytes from `$01:FFF0` are read
past the bank's end, so the last sixteen come from `$01:0000`, which is not
the image. A read of VRAM back into `$9B00` writes into the image, which takes
nothing, and is not lifted; the same sixteen bytes at `$9C00` go to `VMDATAL`
and then to `VMDATAH`, one class and two registers, which the pass refuses.
Channel 1 walks an indirect HDMA table at `$9700` whose two entries point at
two-byte blocks at `$9712` and `$9710`.

What it shows: a file under each directory the pass writes; ranges that share
bytes as one file and ranges that touch as two; a range read downward lifted
in image order; each refusal and its note; a transfer that leaves the image
lifted as far as it was in it; and the tree still assembling to the image
through the `INCBIN` lines.

Read by `tests/rom/rom_disasm_test.cpp` and `tests/rom/verify_test.cpp`; the
source of the `asset` lines in
[`docs/project-manifest.md`](../../../docs/project-manifest.md) and of the
`INCBIN` lines in [`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md).
