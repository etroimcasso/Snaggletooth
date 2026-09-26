# chip_half

One bank whose chipset byte names a DSP, and no save, so the board gives the
lower halves of its cartridge banks to the chip. Reset reads a byte at
`$60:1000` — the chip's half, where no image byte is — into `$0200`, then strobes
the first controller and clocks one bit out: with B down it calls `$60:1000` and
jumps long to `$20:6000`, the expansion area; with B up it stops.

What it shows: the tracer stops at both targets and says what each is — the call's
is the coprocessor's, the jump's is the expansion area — and neither is in the
image, so no entry lifts them. A run with no button down never takes the call or
the jump, and the machine models no chip in the half, so the read returns open
bus. The manifest's board lines read `save 0` and `chip DSP`.

Read by `tests/rom/rom_disasm_test.cpp`; the source of the coprocessor and
expansion-area `stop` lines in
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md), of the `chip`
line in [`docs/project-manifest.md`](../../../docs/project-manifest.md) and of the
coprocessor's board in [`docs/snes-cartridge.md`](../../../docs/snes-cartridge.md).
