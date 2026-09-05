# running_bank

One HiROM bank. The tree places every byte in bank `$C0`; the CPU enters at
`$00:8000` from the reset vector and runs through the mirror, where `$1000` is
work RAM — under `$C0` it is the image, and the byte there is `$5A`.

Reset pushes its bank with `PHK` and pulls it into the data bank, loads through
it from `$1000` and writes the display register; calls long into `$C0:8200`,
where the routine does the same and reads the image's `$5A`; both call
`$8A00`, which proves its bank too and jumps through a one-slot table, so the
label after it sees `$00` and `$C0` and the slot derives in both banks; reset
comes back and proves its own bank again; builds a pointer in work RAM and
jumps long through
it to `$00:8400`, which only a run can follow; that code jumps through a
one-slot table to `$00:8900`, which returns through a frame it built with `PEA`
to `$00:8500`, where the bank is proven once more. Two further blocks, at
`$8600` and `$8700`, are entered only when a person adds an entry — one written
in the mirror bank, one where the tree places it — and `$8800` is the native
NMI handler.

What it shows: the data bank a program proves is the bank the CPU runs in,
which reads work RAM where the bank the tree places the code in would have read
the image; a long call's routine runs in its operand's bank and the caller gets
its own back; two paths that run one routine in two banks are reported as both,
never one, and a table it jumps through derives a destination in each bank; a
destination a run saw, a landing, an entry a person gave and an
interrupt vector each begin in the bank the CPU arrives in; and the manifest
keeps every such address in that bank.

Read by `tests/ir/dataflow_test.cpp`, `tests/rom/observe_test.cpp` and
`tests/rom/routines_test.cpp`.
