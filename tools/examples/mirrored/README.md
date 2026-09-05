# mirrored

One bank. Reset switches to native mode and jumps long into bank `$80`, the
fast mirror of bank `$00` under LoROM, where the rest of the program runs: a
store to `$0100`, an increment of it, and a stop. The CPU executes those
instructions at `$80:8010`; the tree places the same bytes once, at `$00:8010`.

What it shows: a replay looks each instruction up at the address the tree
places it, so code the cartridge runs through a mirror bank is checked against
its nodes rather than counted as unlifted, and a divergence in it names the
address a reader finds in the tree.

Read by `tests/ir/differential_test.cpp`.
