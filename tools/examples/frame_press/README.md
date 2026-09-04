# frame_press

One bank, in emulation mode. Reset enables the vertical-blank NMI and the
controller auto-read and waits for twelve interrupts. The handler counts them,
and on exactly the tenth waits for the auto-read to finish and, if Start is
down, sets a flag. After the twelve the program stops if the flag is set, and
otherwise waits forever, one `WAI` per frame.

What it shows: a recorded run presents a frame's pad as that frame begins, so
a Start held on the one frame the tenth interrupt reads ends the run, and the
same press a frame earlier or later does not — which is what pins the replay to
the frame the disassembler's run used.

Read by `tests/ir/differential_test.cpp`.
