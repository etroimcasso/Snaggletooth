# Run record

A run record is a file holding what one run of a cartridge saw: the destinations its indirect jumps took, the places the CPU arrived that no instruction named, the ranges the transfer engines moved and where each landed, how each HDMA table was walked, the palettes a landing was read under, the registers seen at every site, every extent carried out of work RAM with the image bytes it was built from and every content it was carried out with, the runs of bytes the CPU carried itself, the samples the key-ons named, and the outcomes of the routines the toolkit ran. It is the [run's observation](snes-disassembler.md#running-the-cartridge) and the run's notes, whole, written so that it reads back to the same observation. A tree keeps its records as numbered zstd files under `runs/`, one a run, and the records of several runs join into one observation. The disassembler writes one when a run ends, and every later disassembly over the tree is made from every record there and its own run together, so what a run found stays in the tree whether or not a later run finds it again (§3).

The library is `rom/rom_record.h`, in the `snaggletooth_rom` target.

---

## Contents

- [1. Form](#1-form)
- [2. Lines](#2-lines)
  - [2.1 The head](#21-the-head)
  - [2.2 Counts](#22-counts)
  - [2.3 Notes](#23-notes)
  - [2.4 Where the run went](#24-where-the-run-went)
  - [2.5 What the engines moved](#25-what-the-engines-moved)
  - [2.6 What the run saw at each site](#26-what-the-run-saw-at-each-site)
  - [2.7 What was staged](#27-what-was-staged)
  - [2.8 What the CPU streamed](#28-what-the-cpu-streamed)
  - [2.9 Samples](#29-samples)
  - [2.10 What the toolkit ran](#210-what-the-toolkit-ran)
- [3. The records under a tree](#3-the-records-under-a-tree)
- [4. Joining records](#4-joining-records)
- [5. Library](#5-library)
- [6. Stability](#6-stability)
- [See also](#see-also)

## 1. Form

A record is UTF-8 text, kept in its file compressed with zstd (§3), one line per fact:

```
<kind> <field> <field> …
```

The first word names the kind of line; the fields follow, separated by spaces. Four kinds belong to the line above them and are written indented under it: `writer` and `content` under a `staged` line, `from` under a `content` line, `extent` under an `enumerated` line and `carried` under an `extent` line. The indentation is layout and carries no meaning: a nested line stands after its parent, among the lines nested under it, and belongs to the nearest line of its parent's kind above it. A blank line is nothing. A record has no comments: a `;` is a character like any other, and a note may hold one.

Addresses are written in the 65816 dialect's long form, `$BB:XXXX`, and the sound program's in the SPC700 dialect's, `$XXXX`. An image offset is `$` and six hexadecimal digits, and a run of image offsets is `<first>-<last>`, both inclusive. A count is decimal. Bytes are an unbroken run of hexadecimal digits, two a byte, in address order. A field with nothing in it is `none`.

The kinds stand in this order, each group behind a blank line: the head, `counts`, `note`, `enumeration-note`, `reached`, `ran`, `moved`, `palette`, `landed`, `walked`, `seen`, `staged`, `streamed`, `sample`, `enumerated`. Within a kind the lines stand in the order the observation states for it ([`rom/rom_observe.h`](../tools/rom/rom_observe.h)), so one observation has one text.

A record of twelve frames of the `drawing` example cartridge, which sends one of everything an editable form has a grammar for, begins:

```
snagrun 1
image 32768 checksum $EDCB

counts instructions 186265 interrupts 12 nodes 450 divergences 0 spc700-instructions 51488 spc700-nodes 60 spc700-divergences 0 origin-sets 3254 origin-cap 64 variants-planned 0 variants-run 0 variants-returned 0 variants-confined 0 enumeration-cycles 0
```

## 2. Lines

### 2.1 The head

```
snagrun 1
image <bytes> checksum $XXXX
```

The first line names the format and its version. The second names the image the run played: its size in bytes and the checksum its header holds, or `checksum none` for an image with no header. A reader refuses a record whose first line is anything else, and the records of a tree are refused when one names an image other than the one they are read over (§3).

### 2.2 Counts

```
counts instructions <n> interrupts <n> nodes <n> divergences <n> spc700-instructions <n> spc700-nodes <n> spc700-divergences <n> origin-sets <n> origin-cap <n> variants-planned <n> variants-run <n> variants-returned <n> variants-confined <n> enumeration-cycles <n>
```

One line, every count named in this order: the steps the interpreter ran a node for and checked, the hardware interrupt sequences checked, the distinct nodes lifted, the steps on which a lifted node disagreed with the machine; the same three for the sound CPU; the distinct origins the shadow interned and the cap above which one is widened; and the values the toolkit planned, ran, saw return, and saw return having written only where the run saw the routine write, with the master cycles those runs spent.

### 2.3 Notes

```
note <text>
enumeration-note <text>
```

A `note` line is one of the notes the run made as it played, the text running to the end of the line, in the order the run made them. Twelve frames of the `unreadable_pointer` example cartridge make one:

```
note run: the jump at $00:8000 reads its pointer from memory the run cannot see; not recorded
```

An `enumeration-note` line is one of the notes the toolkit's runs of the routines made (§2.10), in the order made. The two kinds are kept apart because a manifest says them in two places: the run's notes before the trace's, the enumeration's after the notes of the plan ([project-manifest.md §2.5](project-manifest.md#25-stops-warnings-and-notes)). A second of the `enumerating` example cartridge makes seven, among them:

```
enumeration-note enumerate sub_008100 X=$000C: did not return within 21477272 cycles; not lifted
enumeration-note enumerate sub_008800: 4 values wrote $7E:0000-$7E:0001, extents the run never carried; not lifted
```

### 2.4 Where the run went

```
reached <target> e=0|1 m=8|16|? x=8|16|? from <site> call|jump
ran     <target> e=0|1 m=8|16|? x=8|16|? from <site>
```

A `reached` line is a destination an indirect jump or call took: where it landed, the mode it arrived in, the instruction that took it, and whether that instruction was a call. A `ran` line is a place the CPU arrived that the instruction before it did not name. The mode's fields are the CPU's emulation flag and its two widths; `?` is a width the run did not know. From the `dispatching` and `ram_code` example cartridges:

```
reached $00:8200 e=0 m=8 x=8 from $00:8212 call
reached $00:8220 e=0 m=8 x=8 from $00:8215 jump
ran $00:804D e=0 m=16 x=16 from $00:804C
ran $00:802C e=1 m=8 x=8 from $7E:2001
```

### 2.5 What the engines moved

```
moved   <site> channel <c> to-register|from-register <register> memory <address> increment|decrement|fixed bytes <n> as <kind> times <n>
palette <bytes>
landed  <site> channel <c> memory <address> bytes <n> as <kind> times <n> at <landing>
walked  <site> channel <c> memory <address> bytes <n> as <kind> unit <n> direct|indirect times <n>
```

A `moved` line is one range one channel moved under one trigger: the instruction that started it, the channel, the direction, the B-bus register the channel named, the A-bus address the range began at and how it stepped, the count, what the range was to the engine (`dma`, `table`, `indirect`), and how many times the run saw exactly this range. A register's name and class are not written: they are the address's.

A `palette` line is the palette RAM as it stood at a drawn frame a landing was read at, all 512 bytes. The palettes are numbered from zero in the order their lines stand, and a landing names one by its number, so every `palette` line stands before the first landing that names it.

A `landed` line is where one range's bytes went on the other side of the port, with the fields of its `moved` line that identify the range and how many times the run saw exactly this landing. A landing is nine words:

```
<memory> <lowest>-<highest> shown|unshown areas <areas> depths <depths> palette <n>|none
```

The memory is `vram`, `cgram` or `oam`; the lowest and highest address the port put a byte at are written as the manifest writes them, four hexadecimal digits for a VRAM word, two for a palette word, three for an OAM byte; `unshown` is a landing no frame was drawn after. The areas are what the picture used the memory as, the manifest's names joined by `+`, or `none`; the depths are the color depths the landing's tile areas were read at, `2`, `4` and `8` joined by `+`, or `none`; and the palette is the number of the `palette` line the landing was read under, or `none`.

A `walked` line is how the HDMA engine read one table or indirect block: the fields that identify the range, the bytes one line of the table transfers, whether the entries carry pointers, and how many times the run saw exactly this walk. From the `drawing` example cartridge:

```
moved $00:8046 channel 0 to-register $00:2118 memory $00:9000 increment bytes 112 as dma times 1
moved $00:81CC channel 1 to-register $00:2126 memory $00:A400 increment bytes 11 as table times 12
landed $00:8046 channel 0 memory $00:9000 bytes 112 as dma times 1 at vram $1000-$1037 shown areas tiles1+tiles2 depths 4 palette 0
landed $00:80CA channel 0 memory $00:9300 bytes 32 as dma times 1 at cgram $00-$0F shown areas palette depths none palette none
walked $00:81CC channel 2 memory $00:A410 bytes 7 as table unit 1 indirect times 12
```

### 2.6 What the run saw at each site

```
seen <site> D=<values> DBR=<values>
```

Every direct register and every data bank the run saw at one site in the image before the instruction there ran, each `$` and four or two hexadecimal digits, ascending, joined by `|`, or `none`. From the `drawing` example cartridge:

```
seen $00:8008 D=$0000 DBR=$7E
```

### 2.7 What was staged

```
staged <address> bytes <n> origin <origin>
  writer <site> instruction|engine written|unwritten bytes <n> sources <runs>|none origin <origin>
  content <class> as <kind> unit <n> direct|indirect order <n> bytes <bytes> at <landing>|none origin <origin>
    from <n> origin <origin>
```

A `staged` line is one extent of work RAM carried to a register, by an engine or by the CPU a store at a time: its lowest address, its count, and the origin of every byte of it together. Under it stand its writers, most bytes first, and then its contents, in the order the run first carried each.

A `writer` line is one instruction, or the trigger of an engine, that wrote the extent's bytes; `unwritten` counts the bytes nothing wrote since power-on. It gives how many of the extent's bytes the writer wrote over every sighting, the runs of image bytes its invocations read that hold their origin, ascending and no two touching, and the origin of those bytes together.

A `content` line is one distinct set of bytes the extent was carried out with: the class of the register they went to, what the carry was to the engine, the unit and the form a table was walked under, the carry's place in the run's order of carries, the bytes, where they landed, or `none`, and their origin together. The `from` lines under it are the origin of each of its bytes on its own: runs of consecutive bytes with one origin, in address order, each its count and its origin.

An origin runs to the end of its line. It is `none`, or as many of these as hold, in this order: `image` and the runs of image offsets the value was computed from, joined by `,`; `registers` and the hardware registers whose value entered it, `$XXXX`, joined by `,`; `save`, where a byte of the save entered it; `approximate`, where the set was widened to its hull. From the `staging` example cartridge:

```
staged $7E:0400 bytes 32 origin image $001300-$00131F
  writer $00:81FC engine written bytes 32 sources $001300-$00131F origin image $001300-$00131F
  content Vram as dma unit 2 direct order 3 bytes 707172737475767778797A7B7C7D7E7F808182838485868788898A8B8C8D8E8F at vram $0020-$002F shown areas mode7 depths 8 palette 0 origin image $001300-$00131F
    from 1 origin image $001300-$001300
    from 1 origin image $001301-$001301
```

### 2.8 What the CPU streamed

```
streamed <site> <register> offset <offset> bytes <n> times <n> source <first>-<last> memory <address>|none at <landing>|none
```

A run of consecutive stores the CPU made to one data register: the first store's site, the register, the image offset the bytes were read from, the count, how many times the run saw exactly this stream, the run of image bytes its carrier read, the work-RAM buffer the bytes were loaded from, or `none` for bytes read from the image, and where they landed, or `none` for a stream to the audio ports. From the `staging` example cartridge:

```
streamed $00:818F $00:2122 offset $001200 bytes 16 times 1 source $001200-$001210 memory none at cgram $00-$07 shown areas palette depths none palette none
```

### 2.9 Samples

```
sample <start> loop <address> times <n> bytes <bytes>
```

One sample a key-on named: the address in the audio memory its first block lies at, the address the directory entry gives the DSP to continue from, how many key-ons named exactly this sample, and its bytes from its first block to the first carrying the end flag, nine a block. From the `drawing` example cartridge:

```
sample $0308 loop $0311 times 1 bytes B0123456789ABCDEF0B30FEDCBA987654321
```

### 2.10 What the toolkit ran

```
enumerated <routine> A|X|Y bits 8|16 values <values>|none entered-from <site>
  extent <address> bytes <n> classes <classes>|none values <bytes> sources <runs>|none origin <origin>
    carried <class> as <kind> register <register> from <site>
```

An `enumerated` line is one outcome of the toolkit's runs of a routine: the routine, the register it set and at what width, the values that built this outcome, written as the manifest writes them (`$00-$06/2,$09`), and the call the run entered the routine from. Under it stand the extents the values wrote, in address order: the lowest address, the count, the classes the bytes went to joined by `+`, the bytes as work RAM held them when the routine returned, the image bytes they were built from, and their origin. A `carried` line is one of the value's own transfers that carried a byte of the extent: the class, the kind, the register and the instruction that started it. From the outcomes the toolkit kept for the `enumerating` example cartridge:

```
enumerated $00:8100 X bits 16 values $0000 entered-from $00:804A
  extent $7F:0000 bytes 32 classes Vram values 000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F sources $000300-$000321 origin image $000302-$000321
    carried Vram as dma register $00:2118 from $00:817C
```

## 3. The records under a tree

A tree's records are the files under its `runs/` directory named `<digits>.snagrun.zst`: `runs/0001.snagrun.zst`, `runs/0002.snagrun.zst`, and on, numbered from `0001` in four digits or as many more as a number needs. They are read in the order of their numbers. A record is written once, as the number one past the highest there, and never rewritten; a record whose text a file there already holds is not written again. A file under `runs/` with any other name is not a record and is left alone.

Each file is one [zstd](../third_party/zstd/README.md) frame ([RFC 8878](https://www.rfc-editor.org/rfc/rfc8878)) holding one record's text, so `zstd -dc` shows the text: compressed at level 19 with long-distance matching over a 128 MB window, the frame carrying its content's size and a checksum, on one thread, so one text is one file's bytes on every platform. A reader takes any zstd frame of a record's text, whatever compressed it, within that window; a file that is not a whole frame, or has bytes after it, is refused with its path. A second of the `drawing` example cartridge is 26,027 bytes of text in a file of 2,040; a minute of a commercial cartridge's play, 5 to 54 MB of text, is a file of 0.1 to 0.6 MB, and a long session gains more, since its repeated lines lie within the window: about thirteen minutes of play, 416 MB of text, are a file of 2.6 MB.

```
$ zstd -dc --long=27 drawing-tree/runs/0001.snagrun.zst | head -2
snagrun 1
image 32768 checksum $EDCB
```

A record names the image its run played, and a record read over another image is refused with the file's path: a tree's records are of one cartridge.

A disassembly over a tree reads every record there, whether or not the tree holds a manifest, joins them in the order of their numbers (§4), joins its own run to them, and makes the tree from the whole: every `reached`, `ran`, `moved`, `seen`, `landed`, `walked`, `staged`, `origin`, `streamed`, `sample` and `enumerated` line of the manifest, every lifted file and every preview is what all the runs found together. A run that agrees with a record in everything but its outcomes and the enumeration's counts is that record's run again, and is not counted twice. A disassembly with no run of its own is made from the records alone, and runs no routine: its outcomes are the records'. When the disassembly made or was handed a run and asks for it to be kept, its run is written as the next record after the manifest; a host that makes several trees from one run as it goes keeps only the last (`CartridgeRequest::recordRun`), and [`snes_player --disasm`](../tools/player/README.md#disassembling-as-you-play) keeps the closing tree's.

A person may delete a record, a file at a time, to drop what that run found: the next disassembly is made from the records that are left. No tool removes a record.

## 4. Joining records

Two observations join into one by the identity the run itself keeps each kind under, so a joined observation is what one run would hold had it seen everything both saw:

- A destination a jump took, and a place the CPU arrived, is held once.
- A range, a landing, a walk, a stream and a sample that both hold is held once with the counts added. A landing keeps the palette its first sighting was read under, and a stream keeps its first source.
- Each distinct palette is held once, and every landing names its palette's place in the joined list.
- A site holds every value either run saw there.
- An extent holds the origins of both. A writer both hold has its bytes added, its origin joined and its sources united. A content both hold is held once; the later run's other contents follow every content of the earlier, in the later run's order of carries.
- Outcomes that agree in everything but their values are one outcome holding the values of both.
- The instruction, interrupt and divergence counts and the enumeration's are added; the node counts, the origin count and the cap take the larger.

Joining an observation to an empty one gives that observation. Records that agree in everything but their outcomes and the enumeration's counts are two records of one run: folded together, the run's sightings count once and the outcomes join. The notes of every record are joined each once, in the order first made, and the enumeration's notes apart from them in the same way.

## 5. Library

```cpp
#include "rom/rom_observe.h"
#include "rom/rom_record.h"

using namespace snaggletooth::disasm;

// `rom` is the image, `script` the InputScript to play, `tree` the tree's directory.
std::vector<std::string> notes;
const RunObservation seen = observeRun(rom, 60u * 21'477'272u, script, notes);

std::string error;
if (!writeRecord(RunRecord{.observation = seen, .notes = notes, .enumerationNotes = {}}, rom, tree, error)) {
  // `error` names the file that could not be written.
}
const std::optional<std::vector<RunRecord>> records = readRecords(tree, rom, error);
if (!records) {
  // `error` is the record's path, ": " and the line that did not read, or the image it was written for.
}
const RunRecord all = foldRecords(*records);  // every run the tree holds, as one
```

- `RunRecord` — `observation`, a `RunObservation`; `notes`, the run's notes in the order made; and `enumerationNotes`, the notes of the toolkit's runs of the routines in the order made. Two records compare equal field for field.
- `renderRecord(record, rom)` — the record's text, its head naming `rom`.
- `renderRecord(record, imageBytes, checksum)` — the same text for an image known by its size and the checksum its header names, `std::nullopt` for an image with no header, as a disassembly holds them (`CartridgeDisassembly::imageBytes`, `header.checksum`).
- `parseRecord(text, error)` — the record a text holds, or nothing with `error` set to `line <n>: ` and what the line was expected to be. A register's name and class are those of its address, and a destination's and a landing's names are empty, as a run reports them. Reading what `renderRecord` wrote gives the record back, and rendering that gives the same text.
- `recordMismatch(text, rom)` — why the record cannot be read over `rom`, its size or its checksum, or an empty string when it can.
- `joinObservation(into, from)` — `from` joined into `into` in place, by the rules of §4, every list standing afterward in the order the observation states for it.
- `readRecords(directory, rom, error, progress)` — the records under `<directory>/runs/`, in the order of their numbers, each file decompressed; none for a tree with no `runs/`; nothing, with `error` the file's path, `": "` and what is wrong, for one that is not a whole zstd file, does not read or was written for another image. `progress`, a `ProgressSink` from `rom/progress.h`, is told `decompressing runs/<name> (<size>)` as each file begins, the size in decimal MB to a tenth or whole kB under a megabyte.
- `checkRecords(directory, rom, error)` — how many records the tree holds, each file decompressed only as far as its first two lines; nothing, with `error` set as `readRecords` sets it, for one that is not a zstd file, does not begin `snagrun 1` or was written for another image. A fraction of a second where reading the records takes seconds; what a record's body holds it does not look at.
- `foldRecords(records)` — every record joined into one, two records of one run counted once.
- `writeRecord(record, rom, directory, error)` — the record written as the next file under `<directory>/runs/`, compressed with zstd (§3), the directory made where there is none; true without writing when a file there holds the same text; false, with `error` set, when the file cannot be written.
- `writeRecord(record, imageBytes, checksum, directory, error)` — the same, for an image known by its size and checksum; `writeProject` writes a disassembly's record this way.

The disassembler's part is in `rom/rom_disasm.h`: `treeRequest` reads a tree's records into `CartridgeRequest::records`, or with `TreeRecords::Checked` only checks them, `disassembleCartridge` makes the tree from them and its own run and holds its own run as `CartridgeDisassembly::record`, and `writeProject` writes that record under `runs/`.

Joining a record to itself with `joinObservation` doubles its counts: it is two runs to the join. `foldRecords` is the call that knows two records of one run.

The mode a sighting is written with keeps the emulation flag and the two widths. A run records no carry, so none is written.

## 6. Stability

This document defines a published surface, held to the same rule as the [assembly language](assembly-lexicon.md#8-stability): once a release reads a record, a later release reads it to the same effect. The fields of the kinds above do not change. New kinds may be added, and are added to this page before a release writes them; a reader treats a kind it does not know as an error, never as a line to pass over. A reader refuses a first line naming a version it does not read.

## See also

- [Project manifest](project-manifest.md) — the tree's manifest, whose run lines are written from the runs' observations, and the lexical conventions a record shares.
- [Cartridge disassembler](snes-disassembler.md) — the run on the machine and what it records.
- [Input script](input-script.md) — the recorded presses a run replays into the controller ports.
