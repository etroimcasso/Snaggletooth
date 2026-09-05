#!/usr/bin/env python3
"""Runs a directory of cartridges through the toolkit and reports one line per
image and a verdict.

    corpus.py <images> <output> --build <build-dir> [--no-run] [--seconds N]
              [--input-dir <scripts>] [--no-lift] [--no-differential] [--facts] [--routines]

For every `.smc` or `.sfc` under <images>, in name order, the tree is written
under <output>/<name>/ by `snes_disasm` (reading back the manifest already
there, so a directory an earlier run filled keeps its `reached` lines), lifted
by `snes_lift` into <output>/<name>.snagir unless `--no-lift`, assembled back by
`snes_verify` into <output>/<name>-rebuilt<.smc|.sfc> with the rebuilt image's
SHA-256 compared with the original's, and — unless `--no-differential` —
replayed beside the interpreter by `snes_differential`, whose report lands under
<output>/<name>/differential/. A script named <name>.txt under `--input-dir` is
the recorded run for that image, given to both the disassembler and the replay.
`--no-run` skips the machine run in the disassembler (the trace alone takes
seconds; the run takes about as long as it emulates), `--seconds` sets both the
run's and the replay's length (sixty by default).

`--facts` and `--routines` add the corpus-wide aggregates the manifests carry:
every hardware access by class and by register, every transfer the code set up
by destination, every range a run saw move by destination class and by kind,
every file lifted by its directory with the bytes it holds, and every routine
with what it calls and reaches.

Every image's line counts its manifest's `stop`, `reached`, `ran`, `derived`,
`moved`, `asset`, `state` and `seen` lines, so a corpus run says how far the
trace, the run, the analysis and the lift reached.

An image is OK only when every command exits 0 and the rebuilt image matches;
the script exits 0 only when every image is OK. A failure's whole output is
printed, never truncated. Nothing is written anywhere but under <output>.
"""
import argparse
import collections
import hashlib
import pathlib
import subprocess
import sys
import time


def sha(path):
    data = path.read_bytes()
    if len(data) % 1024 == 512:
        data = data[512:]
    return hashlib.sha256(data).hexdigest()


def manifestLines(tree, kind, length):
    manifest = tree / "project.manifest"
    if not manifest.exists():
        return []
    out = []
    for line in manifest.read_text(errors="replace").splitlines():
        words = line.split()
        if len(words) == length and words[0] == kind:
            out.append(words)
    return out


def facts(tree):
    """The `access`, `dma` and `moved` lines: counts by class and register, how
    many accesses carried a value, the transfers the code set up by destination
    class, and the ranges a run saw move — each as (class, kind, from the image)."""
    classes = collections.Counter()
    registers = collections.Counter()
    valued = 0
    accesses = manifestLines(tree, "access", 6)  # access <site> <name> <class> <kind> <value|none>
    for words in accesses:
        registers[words[2]] += 1
        classes[words[3]] += 1
        if words[5] != "none":
            valued += 1
    # dma <site> channel <n> <direction> <dest> <name> <class> source <src> start <mask>
    dmas = [(w[7], w[9] != "none", w[11] != "none") for w in manifestLines(tree, "dma", 12)]
    # moved <site> channel <n> <direction> <register> <name> <class> memory <address>
    #       <step> bytes <n> as <kind> times <n>
    moved = [(w[7], w[14], fromImage(w[9])) for w in manifestLines(tree, "moved", 17)]
    # asset <path> <class> as <kind> from <address> bytes <n>
    assets = [(w[1].split("/")[0], int(w[8])) for w in manifestLines(tree, "asset", 9)]
    return classes, registers, valued, len(accesses), dmas, moved, assets


def fromImage(address):
    """Whether a $BB:XXXX address lies in the image rather than in RAM: work
    RAM is banks $7E-$7F and the first 8 KB of the system banks; save RAM and
    the registers are not the image either, but a transfer from them is rare
    enough to count with the image here."""
    bank = int(address[1:3], 16)
    offset = int(address[4:8], 16)
    if bank in (0x7E, 0x7F):
        return False
    if (bank <= 0x3F or 0x80 <= bank <= 0xBF) and offset < 0x2000:
        return False
    return True


def routines(tree):
    """The `routine` lines: (label, lines, bytes, calls, reaches, through)."""
    out = []
    # routine <address> <label> lines <n> bytes <n> calls <list> reaches <list> through <list>
    for w in manifestLines(tree, "routine", 13):
        calls = [] if w[8] == "none" else w[8].split(",")
        reaches = [] if w[10] == "none" else w[10].split(",")
        through = [] if w[12] == "none" else w[12].split(",")
        out.append((w[2], int(w[4]), int(w[6]), calls, reaches, through))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, required=True, help="the build directory holding the commands")
    parser.add_argument("--no-run", action="store_true", help="trace without running the cartridge")
    parser.add_argument("--seconds", default="60", help="the run's and the replay's length in seconds of the master clock")
    parser.add_argument("--input-dir", type=pathlib.Path, help="recorded runs, one <name>.txt per image")
    parser.add_argument("--no-lift", action="store_true", help="skip writing the tree's program as <name>.snagir")
    parser.add_argument("--no-differential", action="store_true", help="skip the replay beside the interpreter")
    parser.add_argument("--facts", action="store_true", help="aggregate the hardware accesses, the transfers set up and the ranges moved")
    parser.add_argument("--routines", action="store_true", help="aggregate the routines")
    args = parser.parse_args()

    roms = sorted(p for p in args.images.iterdir() if p.suffix.lower() in (".smc", ".sfc"))
    if not roms:
        print(f"no .smc or .sfc under {args.images}")
        sys.exit(2)
    failures = 0
    corpusClasses = collections.Counter()
    corpusRegisters = collections.Counter()
    corpusDma = collections.Counter()
    corpusMoved = collections.Counter()
    corpusMovedKind = collections.Counter()
    corpusAssets = collections.Counter()
    corpusAssetBytes = collections.Counter()
    corpusReaches = collections.Counter()
    corpusThrough = collections.Counter()
    factTotals = collections.Counter()
    routineTotals = collections.Counter()
    largest = []

    for rom in roms:
        name = rom.stem.replace(" ", "_")
        tree = args.output / name
        tree.mkdir(parents=True, exist_ok=True)
        script = args.input_dir / (name + ".txt") if args.input_dir else None
        if script is not None and not script.exists():
            script = None
        started = time.time()
        results = []

        disasm = [args.build / "snes_disasm", rom, "-o", tree, "--run-seconds", args.seconds]
        if args.no_run:
            disasm = [args.build / "snes_disasm", rom, "-o", tree, "--no-run"]
        elif script is not None:
            disasm += ["--input", script]
        results.append(subprocess.run([str(c) for c in disasm], capture_output=True, text=True))

        if not args.no_lift:
            lifted = args.output / f"{name}.snagir"
            results.append(subprocess.run([str(c) for c in [args.build / "snes_lift", tree, rom, "-o", lifted]],
                                          capture_output=True, text=True))

        # The rebuilt image keeps the original's extension: it is the same kind of file.
        rebuilt = args.output / f"{name}-rebuilt{rom.suffix}"
        results.append(subprocess.run([str(c) for c in [args.build / "snes_verify", tree, rom, "-o", rebuilt]],
                                      capture_output=True, text=True))
        same = rebuilt.exists() and sha(rom) == sha(rebuilt)

        replayLine = ""
        if not args.no_differential:
            replay = [args.build / "snes_differential", tree, rom, "-o", tree / "differential",
                      "--seconds", args.seconds]
            if script is not None:
                replay += ["--input", script]
            results.append(subprocess.run([str(c) for c in replay], capture_output=True, text=True))
            replayLine = results[-1].stdout.strip()

        elapsed = time.time() - started
        ok = same and all(r.returncode == 0 for r in results)
        if not ok:
            failures += 1

        summary = f"{'OK ' if ok else 'BAD'} {rom.name}: {elapsed:.0f} s"
        manifest = tree / "project.manifest"
        if manifest.exists():
            kinds = collections.Counter(line.split()[0] for line in manifest.read_text(errors="replace").splitlines()
                                        if line.strip() and not line.startswith(";"))
            summary += (f"; {kinds['stop']} stops, {kinds['reached']} reached, {kinds['ran']} ran, "
                        f"{kinds['derived']} derived, {kinds['moved']} moved, {kinds['asset']} assets, "
                        f"{kinds['state']} state lines, {kinds['seen']} seen lines")
        if replayLine:
            summary += f"; {replayLine}"
        if args.facts:
            classes, registers, valued, accesses, dmas, movedLines, assets = facts(tree)
            corpusClasses.update(classes)
            corpusRegisters.update(registers)
            factTotals["accesses"] += accesses
            factTotals["valued"] += valued
            factTotals["dmas"] += len(dmas)
            for destination, sourced, startedFlag in dmas:
                corpusDma[destination] += 1
                factTotals["sourced"] += 1 if sourced else 0
                factTotals["started"] += 1 if startedFlag else 0
            factTotals["moved"] += len(movedLines)
            imageRanges = 0
            for destination, kind, image in movedLines:
                corpusMoved[destination] += 1
                corpusMovedKind[kind] += 1
                if image:
                    imageRanges += 1
            factTotals["movedFromImage"] += imageRanges
            factTotals["assets"] += len(assets)
            for directory, size in assets:
                corpusAssets[directory] += 1
                corpusAssetBytes[directory] += size
                factTotals["assetBytes"] += size
            top = ", ".join(f"{k} {v}" for k, v in classes.most_common(5))
            summary += (f"; {accesses} accesses ({valued} with a value), {len(dmas)} transfers, "
                        f"{len(movedLines)} moved ({imageRanges} from the image), "
                        f"{len(assets)} assets ({sum(s for _, s in assets)} bytes); {top}")
        if args.routines:
            found = routines(tree)
            leaf = sum(1 for r in found if not r[3])
            silent = sum(1 for r in found if not r[4])
            routineTotals["routines"] += len(found)
            routineTotals["leaf"] += leaf
            routineTotals["silent"] += silent
            routineTotals["lines"] += sum(r[1] for r in found)
            for r in found:
                corpusReaches.update(r[4])
                corpusThrough.update(r[5])
            biggest = max(found, key=lambda r: r[1], default=None)
            if biggest:
                largest.append((biggest[1], rom.name, biggest[0]))
            summary += (f"; {len(found)} routines ({leaf} call nothing, {silent} reach nothing themselves)"
                        f"; largest {biggest[0] if biggest else '-'} at {biggest[1] if biggest else 0} lines")
        print(summary, flush=True)
        if not ok:
            for r in results:
                print(r.stdout)
                print(r.stderr)
            print(f"      sha256 identical: {same}")

    print()
    print(f"{len(roms) - failures} of {len(roms)} images OK")
    if args.facts:
        print(f"{factTotals['accesses']} accesses, {factTotals['valued']} with a value; "
              f"{factTotals['dmas']} transfers, {factTotals['sourced']} with a source, "
              f"{factTotals['started']} with a start; "
              f"{factTotals['moved']} ranges moved, {factTotals['movedFromImage']} from the image; "
              f"{factTotals['assets']} files lifted, {factTotals['assetBytes']} bytes")
        print("\naccesses by class, whole corpus:")
        for k, v in corpusClasses.most_common():
            print(f"  {k:<12} {v}")
        print("\ntransfers by destination class, whole corpus:")
        for k, v in corpusDma.most_common():
            print(f"  {k:<12} {v}")
        print("\nranges moved by destination class, whole corpus:")
        for k, v in corpusMoved.most_common():
            print(f"  {k:<12} {v}")
        print("\nranges moved by kind, whole corpus:")
        for k, v in corpusMovedKind.most_common():
            print(f"  {k:<12} {v}")
        print("\nfiles lifted by directory, whole corpus:")
        for k, v in corpusAssets.most_common():
            print(f"  {k:<12} {v} files, {corpusAssetBytes[k]} bytes")
        print("\nthe thirty most-reached registers, whole corpus:")
        for k, v in corpusRegisters.most_common(30):
            print(f"  {k:<16} {v}")
    if args.routines:
        print(f"{routineTotals['routines']} routines, {routineTotals['leaf']} call nothing, "
              f"{routineTotals['silent']} reach nothing themselves; {routineTotals['lines']} routine-lines")
        print("\nroutines reaching each class themselves, whole corpus:")
        for k, v in corpusReaches.most_common():
            print(f"  {k:<12} {v}")
        print("\nroutines reaching each class through calls, whole corpus:")
        for k, v in corpusThrough.most_common():
            print(f"  {k:<12} {v}")
        print("\nthe ten largest routines by lines, whole corpus:")
        for lines, romName, label in sorted(largest, reverse=True)[:10]:
            print(f"  {lines:>6} {label:<16} {romName}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
