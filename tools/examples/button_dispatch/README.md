# button_dispatch

One bank. Reset enables the vertical-blank NMI and the controller auto-read and
idles. The NMI handler waits for the auto-read to finish, and if Start is down
in `$4219` jumps through the pointer at `$8100` to `$8200`; otherwise it
strobes the serial port, clocks nine bits out — the ninth is A — and if A is
down jumps through the pointer at `$8102` to `$8210`. Neither target is named
by any instruction, so the boot alone reaches neither.

What it shows: the recorded run — a script naming which buttons are held on
which port from which frame — replayed into the controller ports while the
cartridge runs, so the trace reaches what a player reaches; both ways a program
reads a pad, the auto-read and the serial port.

Read by `tests/rom/input_script_test.cpp`; shown in
[docs/input-script.md](../../../docs/input-script.md).
