# declaring

One bank, in emulation mode. Reset sets channel 0 up and starts it four times
in one stretch of straight-line code, sets channel 6 up whole and never starts
it, sends sixteen bytes on channel 7 from inside a block a later routine
declares whole, and then reads the controller: only when a button is down
does it call five routines that each set a channel up whole and start it. The
run's ports are empty, so the trace reaches the five routines and the run
never does.

Channel 0 carries 32 bytes from `$9000` to VRAM; a second start that rewrites
only the source and the count carries sixteen from `$9040` to the same place;
a palette is read downward from `$920F` into CGRAM; and VRAM is filled with
sixty-four copies of the byte at `$9600`. Channel 6 is set up to carry sixteen
bytes from `$9080` and never started; channel 7 carries the sixteen at `$9120`
to VRAM. Behind the button, channel 1 carries 48 bytes
from `$9100` to VRAM and channel 2 the 544-byte sprite table at `$9300` to
OAM; channel 3 fills VRAM from `$9600` with a count of zero, which is 65536;
channel 4 has the low byte of its source rewritten from a variable after it
was written with a value; channel 5 is enabled as an HDMA table at `$9700` to
`CGDATA`, with a count written that the engine overwrites from the table.

What it shows: one `dma` line per start, each with the registers as they
stood when the start was written — four lines for channel 0 from one stretch
of code, the second to the destination the first left; the step from `DMAP`
and the count from `DAS`, zero standing for 65536; the write that started each
transfer; a channel set up and never started; a register written from a
variable leaving the source `none`; an HDMA channel's count reported as the
code wrote it and not read as a length. And what the code proves is lifted:
the sprite table at `$9300` is a file of kind `proven` though no run moved it;
the 48 bytes at `$9100` are proven too, and since the run sent sixteen of them
from inside the block, the file is the run's kind; the fill, the unproven
source, the table and the transfer never started lift nothing; and every
transfer the run did start agrees with the `moved` line from the same start,
field for field.

Read by `tests/rom/facts_test.cpp` and `tests/rom/rom_disasm_test.cpp`; the
source of the `dma` lines in
[`docs/snes-disassembler.md`](../../../docs/snes-disassembler.md) §What the
code reaches and of the `proven` lines in §The assets.
