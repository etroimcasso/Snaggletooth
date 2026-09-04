# unreadable_pointer

One bank whose one instruction is `JMP (!$2140)`: an indirect jump whose
pointer lies in the audio unit's communication port, which is not memory.

What it shows: the run reads a pointer only where it can see one — the image
and work RAM — and says so in a note rather than recording a target it cannot
read.

Read by `tests/rom/observe_test.cpp`.
