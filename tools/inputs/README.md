# Input scripts

The recorded runs the tools replay. A script says which buttons are held on which
controller port from which frame; the tools that run a cartridge take one so that
what the machine does is what a player would make it do. The form is
[input-script.md](../../docs/input-script.md).

| File | What it plays |
|---|---|
| `default.snaginput` | The run a cartridge with no script of its own is played through: A every two seconds and Start every eight, ten frames each, to leave a title screen and a menu behind; Right held from twenty seconds in, so a level that has begun is walked; and B every three seconds or so from thirty-five, since the button a game jumps with is A on some and B on others. |

## Using it

`snes_disasm`, `snes_differential`, `snes_player` and `corpus.py` take
`--input-dir`. Point it here and every image is played, whether or not it has a
run of its own:

```
tools/corpus.py <images> <output> --build build --input-dir tools/inputs
snes_disasm <image> -o <tree> --input-dir tools/inputs
snes_differential <tree> <image> -o <report> --input-dir tools/inputs
snes_player <image> --input-dir tools/inputs
```

A command looks in that directory for the image's own script first —
`scriptPathFor` names it: the image's file name without its extension, spaces as
underscores, `.snaginput` — and falls back to `default.snaginput`. A single run
takes `--input <file>` instead and names the script directly.

## Adding one

A cartridge that needs a run of its own gets a file named for it beside this one,
written by hand to the grammar — or recorded: `snes_player <image> --out <dir>`
writes the run you just played as `<image>.snaginput`, which is the easy way to get
a script that reaches somewhere a hand-written one would take all day to describe. **A script named for a commercial cartridge stays
out of the repository**: the name identifies the image, and no title is named in a
tracked file. Keep those in a local directory of your own and point `--input-dir`
at it; a directory of your own that also holds a copy of `default.snaginput`
plays everything else the same way.

A script here earns its place by being about the machine rather than about one
cartridge: this file presses the two buttons a console game expects before it
starts, which is why one file plays a whole library.

## See also

- [input-script.md](../../docs/input-script.md) — the grammar, the refusals, and
  the library that parses and replays a script.
- [tools/README.md](../README.md) — the tools these scripts are handed to.
