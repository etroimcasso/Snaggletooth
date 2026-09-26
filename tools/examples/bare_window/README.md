# bare_window

One bank and no save, so the save window's lower halves — `$70-$7D` and
`$F0-$FF` below `$8000` — repeat the image as every other cartridge bank's do.
Reset reads a byte through the window at `$70:1300` into `$0200`, sends
thirty-two bytes from `$70:1300` to VRAM by DMA, then strobes the first
controller and clocks one bit out: with B down it calls a routine at `$70:1340`
that increments `$0200` and returns, and it stops. With one bank every cartridge
bank repeats it, so the window reads offset `$1300` of the image, which the tree
places at `$00:9300`, and the routine the call names sits at `$00:9340`.

What it shows: on a board with no save the window is the image, and every reader
says so. The tracer stops at the call, naming the half the target repeats —
`$70:9340` — rather than save RAM. The run's transfer is lifted as the image
range the window reads, `vram/00_9300.bin` from `$00:9300`, the file included in
`bank_00.asm` where the bytes are. A run with no button down never takes the
call. The manifest's board lines read `save 0` and `chip none`.

Read by `tests/rom/rom_disasm_test.cpp`; the source of the bare-window `stop`
line in [`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md) and of
the plain board in [`docs/snes-cartridge.md`](../../../docs/snes-cartridge.md).
