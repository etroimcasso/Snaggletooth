# Where your files are kept

`snes_player` keeps two kinds of file for you: the controller mappings you write,
and the save a cartridge with a battery writes for itself. Both live in one
directory, and the directory is the one your operating system sets aside for an
application's per-user files — the player asks the platform where that is rather
than deciding for itself, so it is the right place on every system it runs on.

Ask it:

```
snes_player --user-files
```

It prints the directory and exits, opening nothing. Everything below is relative
to what it printed.

## Contents

- [The layout](#the-layout)
- [Your controller mapping](#your-controller-mapping)
- [A cartridge's save](#a-cartridges-save)
- [Keeping it somewhere else](#keeping-it-somewhere-else)
- [See also](#see-also)

## The layout

| Directory | Holds | Written by |
|---|---|---|
| `config/input/` | `.snagpad` files — controller mappings. `default.snagpad` here is the mapping the player runs with when you give it no `--config` | you |
| `sram/` | `<cartridge>.srm` — a cartridge's battery-backed save | the player, while you play |

Neither directory is made until something is put in it, so a fresh install has
nothing in it at all and a cartridge without a battery leaves no `sram/` behind.

## Your controller mapping

The player takes its mapping from the first of these it finds:

1. `--config <file>`, whatever you name.
2. `config/input/default.snagpad`, where your files are kept.
3. The mapping built into the tool.

It says at the start which one it took. To begin from the shipped mapping and
change it:

```
snes_player --default-config > "$(snes_player --user-files)/config/input/default.snagpad"
```

Make `config/input/` yourself if it is not there. The file's form is
[pad-config.md](pad-config.md); a line it cannot read is refused by name, and the
run stops rather than starting with half a mapping.

## A cartridge's save

A cartridge whose header declares a battery keeps what it writes, in
`sram/<cartridge>.srm` — the cartridge's own file name with `.srm` for its
extension, wherever you opened the image from. That is the same file bsnes,
snes9x and Mesen read and write, so a save made here opens there and one made
there opens here.

The file is read into the cartridge before its first instruction and written
while you play, each time the machine reports that the save changed — which it
does at the end of a frame that changed it, and not at all in a frame that did
not. Closing the window writes whatever the last moments left.

Two things it will not do. A file whose size is not the size the cartridge
declares is **left exactly as it is** — the cartridge boots on its own power-on
memory and that run keeps nothing, so a save belonging to something else is never
overwritten on a guess; the player says so when it starts. And a save that would
be written with the bytes it already holds is not written at all.

If the disk refuses a write, the run carries on and the player tells you once.
Your game is not interrupted for it.

## Keeping it somewhere else

`SNAGGLETOOTH_USER_FILES` replaces the directory whole:

```
SNAGGLETOOTH_USER_FILES=/Volumes/stick/snaggletooth snes_player game.sfc
```

Everything above then happens under that directory instead, and
`--user-files` prints it. A name inside the store is always a relative path: one
that begins at a root, carries a drive, or climbs out with `..` is refused, so
nothing the player writes lands outside the directory it was given.

## See also

- [pad-config.md](pad-config.md) — the form of a `.snagpad` mapping.
- [input-script.md](input-script.md) — recording and replaying a run.
- [snes-machine.md](snes-machine.md) — the machine's own report that a save changed,
  which is what a program embedding the core watches.
