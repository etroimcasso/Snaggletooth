# dispatching

Two banks that dispatch through every indirect form, each to a target nothing
else names. Bank `$00`'s reset switches to native mode, enables the
vertical-blank NMI, copies a `JML [!$8109]` into work RAM and runs it from
there, then dispatches through a table with `JMP (!$8100,X)`, calls through the
same table with `JSR (!$8100,X)`, jumps through a plain pointer with
`JMP (!$8104)`, and jumps long through a three-byte pointer with `JML [!$8106]`
into bank `$01`. Bank `$01` takes one indirect jump sixty-five thousand times —
long enough for NMIs to land on the jump itself — then stops. Its table at
`$01:8100` sits at the same offset as bank `$00`'s and names a different
target, so a pointer read in the wrong bank lands in the wrong place.

What it shows: the run as oracle — every destination an indirect jump or call
took, recorded with the mode it arrived in, confirmed by where the CPU landed,
and traced from as an entry; a pointer read in the program bank against one
read in bank zero; a step that services an interrupt instead of the jump
recording nothing.

Read by `tests/rom/observe_test.cpp`; shown in
[docs/snes-disassembler.md §Running the cartridge](../../../docs/snes-disassembler.md#running-the-cartridge).
