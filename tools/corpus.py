#!/usr/bin/env python3
"""Runs a directory of cartridges through the toolkit and reports one line per
image and a verdict.

    corpus.py <images> <output> --build <build-dir> [--no-run] [--seconds N]
              [--input-dir <scripts>] [--no-differential] [--facts] [--routines]
              [--jobs N]

For every `.smc` or `.sfc` under <images>, in name order, four commands run and
nothing else happens: `snes_disasm` writes what the disassembly found under
<output>/<name>/ — `program.snagir`, the manifest, the lifted files and the
sound program's file — reading back the manifest already there so a directory
an earlier run filled keeps its `reached` lines; `snes_render` writes the bank
files from the program file and the manifest; `snes_verify` assembles the tree
back into <output>/<name>-rebuilt<.smc|.sfc> and compares it with the image,
its exit status the verdict; and — unless `--no-differential` —
`snes_differential` replays the recorded run beside the interpreter, its report
under <output>/<name>/differential/. `--input-dir` is handed to the two commands
that run the cartridge; each finds the script named for the image itself, or the
directory's `default.snaginput` when the image has none, so every image is played.
`--no-run` skips the machine run in the disassembler (the trace alone takes
seconds; the run takes about as long as it emulates), `--seconds` sets both the
run's and the replay's length (sixty by default). `--jobs` runs that many
images at once, each image's four commands still one after another; the lines
and the aggregates come out in name order whatever finished first.

`--facts` and `--routines` add the corpus-wide aggregates the manifests carry:
every hardware access by class and by register, every transfer the code set up
by destination, every range a run saw move by destination class and by kind,
every file lifted by its directory and by the form its path names with the
bytes it holds, every staged extent by what the shadow found of its source —
exact, approximate, computed, from a register, unwritten — every landing by
what the PPU used the memory as and by its depth, every walk by its unit and
form, every preview by its form, and every routine with what it calls and
reaches.

Every image's line counts its manifest's `stop`, `reached`, `ran`, `derived`,
`moved`, `landed`, `walked`, `asset`, `preview`, `origin`, `staged`,
`streamed`, `state` and `seen` lines, so a corpus run says how far the trace,
the run, the analysis and the shadow reached.

An image is OK only when every command exits 0; the script exits 0 only when
every image is OK. A failure's whole output is printed, never truncated. The
script creates nothing itself and writes nothing anywhere but under <output>.
"""
import argparse
import collections
import concurrent.futures
import pathlib
import subprocess
import sys
import time


def manifestLines(tree, kind, length):
    manifest = tree / "project.snagifest"
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
    # dma <site> channel <n> <direction> <dest> <name> <class> source <src> <step> bytes <n>
    #     start|start-hdma <mask> from <site>
    dmas = [(w[7], w[9] != "none", w[14] != "none", w[12] != "none") for w in manifestLines(tree, "dma", 17)]
    # moved <site> channel <n> <direction> <register> <name> <class> memory <address>
    #       <step> bytes <n> as <kind> times <n>
    moved = [(w[7], w[14], fromImage(w[9])) for w in manifestLines(tree, "moved", 17)]
    # asset <path> <class> as <kind> from <address> bytes <n>
    assets = [(w[1].split("/")[0], int(w[8]), w[4], w[1].rsplit(".", 1)[-1]) for w in manifestLines(tree, "asset", 9)]
    # landed <site> channel <n> memory <address> bytes <n> as <kind> at <lowest>-<highest>
    #        in <area> times <n> depth <2|4|8|none>
    landed = [(w[13], w[17]) for w in manifestLines(tree, "landed", 18)]
    # walked <site> channel <n> memory <address> bytes <n> as <kind> unit <n> direct|indirect times <n>
    walked = [f"unit {w[11]} {w[12]}" for w in manifestLines(tree, "walked", 15)]
    # preview <path> of <path> as <form> [contents <n>]
    previews = [w[5] for w in manifestLines(tree, "preview", 6) + manifestLines(tree, "preview", 8)]
    return classes, registers, valued, len(accesses), dmas, moved, assets, landed, walked, previews


def staged(tree):
    """The `origin` lines by what they say of a source: `exact` or `approximate`
    with the bytes the source spans and the bytes the origin uses within it,
    `computed`, `register`, `save`, `unwritten`; and the `staged` and `streamed`
    lines counted."""
    kinds = collections.Counter()
    spanned = 0
    used = 0
    manifest = tree / "project.snagifest"
    if not manifest.exists():
        return kinds, spanned, used, 0, 0
    stagedLines = 0
    streamedLines = 0
    for line in manifest.read_text(errors="replace").splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "staged":
            stagedLines += 1
        elif w[0] == "streamed":
            streamedLines += 1
        elif w[0] == "origin":
            # origin <address> bytes <n> from <address> bytes <n> using <n> by <label> exact|approximate
            # origin <address> bytes <n> from register <address> <name> by <label>
            # origin <address> bytes <n> from save by <label>
            # origin <address> bytes <n> computed by <label>
            # origin <address> bytes <n> unwritten
            if w[4] == "computed" or w[4] == "unwritten":
                kinds[w[4]] += 1
            elif w[5] in ("register", "save"):
                kinds[w[5]] += 1
            else:
                kinds[w[-1]] += 1
                spanned += int(w[7])
                used += int(w[9])
    return kinds, spanned, used, stagedLines, streamedLines


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


def runImage(args, rom):
    """The four commands over one image, one after another: the results in
    order, the seconds they took, and the replay's summary line."""
    name = rom.stem.replace(" ", "_")
    tree = args.output / name
    started = time.time()
    results = []

    # The commands' progress is for a terminal; here their output is kept for a failure's report.
    disasm = [args.build / "snes_disasm", rom, "-o", tree, "--run-seconds", args.seconds, "--quiet"]
    if args.no_run:
        disasm = [args.build / "snes_disasm", rom, "-o", tree, "--no-run", "--quiet"]
    elif args.input_dir is not None:
        disasm += ["--input-dir", args.input_dir]
    results.append(subprocess.run([str(c) for c in disasm], capture_output=True, text=True))

    results.append(subprocess.run([str(c) for c in [args.build / "snes_render", tree]],
                                  capture_output=True, text=True))

    # The rebuilt image keeps the original's extension: it is the same kind of file.
    rebuilt = args.output / f"{name}-rebuilt{rom.suffix}"
    results.append(subprocess.run([str(c) for c in [args.build / "snes_verify", tree, rom, "-o", rebuilt]],
                                  capture_output=True, text=True))

    replayLine = ""
    if not args.no_differential:
        replay = [args.build / "snes_differential", tree, rom, "-o", tree / "differential",
                  "--seconds", args.seconds, "--quiet"]
        if args.input_dir is not None:
            replay += ["--input-dir", args.input_dir]
        results.append(subprocess.run([str(c) for c in replay], capture_output=True, text=True))
        # The replay's last line sums it up; a dropped copier header is reported ahead of it.
        replayLine = results[-1].stdout.strip().splitlines()[-1] if results[-1].stdout.strip() else ""

    return results, time.time() - started, replayLine


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, required=True, help="the build directory holding the commands")
    parser.add_argument("--no-run", action="store_true", help="trace without running the cartridge")
    parser.add_argument("--seconds", default="60", help="the run's and the replay's length in seconds of the master clock")
    parser.add_argument("--input-dir", type=pathlib.Path, help="recorded runs, <name>.snaginput per image or default.snaginput for the rest, found by the commands")
    parser.add_argument("--no-differential", action="store_true", help="skip the replay beside the interpreter")
    parser.add_argument("--facts", action="store_true", help="aggregate the hardware accesses, the transfers set up and the ranges moved")
    parser.add_argument("--routines", action="store_true", help="aggregate the routines")
    parser.add_argument("--jobs", type=int, default=1, help="how many images to run at once")
    args = parser.parse_args()

    roms = sorted(p for p in args.images.iterdir() if p.suffix.lower() in (".smc", ".sfc"))
    if not roms:
        print(f"no .smc or .sfc under {args.images}")
        sys.exit(2)
    if args.jobs < 1:
        print("--jobs needs a positive number")
        sys.exit(2)
    failures = 0
    corpusClasses = collections.Counter()
    corpusRegisters = collections.Counter()
    corpusDma = collections.Counter()
    corpusMoved = collections.Counter()
    corpusMovedKind = collections.Counter()
    corpusAssets = collections.Counter()
    corpusAssetBytes = collections.Counter()
    corpusAssetKinds = collections.Counter()
    corpusAssetKindBytes = collections.Counter()
    corpusSources = collections.Counter()
    corpusAreas = collections.Counter()
    corpusDepths = collections.Counter()
    corpusForms = collections.Counter()
    corpusFormBytes = collections.Counter()
    corpusWalks = collections.Counter()
    corpusPreviews = collections.Counter()
    corpusReaches = collections.Counter()
    corpusThrough = collections.Counter()
    factTotals = collections.Counter()
    routineTotals = collections.Counter()
    largest = []

    # The images run `--jobs` at a time; each one's line and aggregates follow
    # in name order, so the report reads the same however they finished.
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
    runs = [pool.submit(runImage, args, rom) for rom in roms]
    for rom, run in zip(roms, runs):
        name = rom.stem.replace(" ", "_")
        tree = args.output / name
        results, elapsed, replayLine = run.result()
        ok = all(r.returncode == 0 for r in results)
        if not ok:
            failures += 1

        summary = f"{'OK ' if ok else 'BAD'} {rom.name}: {elapsed:.0f} s"
        manifest = tree / "project.snagifest"
        if manifest.exists():
            kinds = collections.Counter(line.split()[0] for line in manifest.read_text(errors="replace").splitlines()
                                        if line.strip() and not line.startswith(";"))
            summary += (f"; {kinds['stop']} stops, {kinds['reached']} reached, {kinds['ran']} ran, "
                        f"{kinds['derived']} derived, {kinds['moved']} moved, {kinds['landed']} landed, "
                        f"{kinds['walked']} walked, {kinds['asset']} assets, {kinds['preview']} previews, "
                        f"{kinds['origin']} origin, {kinds['staged']} staged, {kinds['streamed']} streamed, "
                        f"{kinds['state']} state lines, {kinds['seen']} seen lines")
        if replayLine:
            summary += f"; {replayLine}"
        if args.facts:
            classes, registers, valued, accesses, dmas, movedLines, assets, landedLines, walkedLines, previewLines = facts(tree)
            corpusClasses.update(classes)
            corpusRegisters.update(registers)
            factTotals["accesses"] += accesses
            factTotals["valued"] += valued
            factTotals["dmas"] += len(dmas)
            for destination, sourced, startedFlag, counted in dmas:
                corpusDma[destination] += 1
                factTotals["sourced"] += 1 if sourced else 0
                factTotals["started"] += 1 if startedFlag else 0
                factTotals["counted"] += 1 if counted else 0
            # A transfer the code proves that the run moved another way is a
            # note beside the two; the count is what the check found.
            factTotals["contradicted"] += sum(
                1 for line in manifest.read_text(errors="replace").splitlines()
                if line.startswith("note") and "the code proves" in line) if manifest.exists() else 0
            factTotals["moved"] += len(movedLines)
            imageRanges = 0
            for destination, kind, image in movedLines:
                corpusMoved[destination] += 1
                corpusMovedKind[kind] += 1
                if image:
                    imageRanges += 1
            factTotals["movedFromImage"] += imageRanges
            factTotals["assets"] += len(assets)
            for directory, size, kind, form in assets:
                corpusAssets[directory] += 1
                corpusAssetBytes[directory] += size
                corpusAssetKinds[kind] += 1
                corpusAssetKindBytes[kind] += size
                corpusForms[form] += 1
                corpusFormBytes[form] += size
                factTotals["assetBytes"] += size
            factTotals["landed"] += len(landedLines)
            corpusAreas.update(area for area, _ in landedLines)
            corpusDepths.update(depth for _, depth in landedLines)
            factTotals["walked"] += len(walkedLines)
            corpusWalks.update(walkedLines)
            factTotals["previews"] += len(previewLines)
            corpusPreviews.update(previewLines)
            sourceKinds, spanned, used, stagedLines, streamedLines = staged(tree)
            corpusSources.update(sourceKinds)
            factTotals["sourceSpanned"] += spanned
            factTotals["sourceUsed"] += used
            factTotals["staged"] += stagedLines
            factTotals["streamed"] += streamedLines
            top = ", ".join(f"{k} {v}" for k, v in classes.most_common(5))
            sources = ", ".join(f"{k} {v}" for k, v in sourceKinds.most_common())
            forms = ", ".join(f"{k} {v}" for k, v in collections.Counter(f for _, _, _, f in assets).most_common())
            summary += (f"; {accesses} accesses ({valued} with a value), {len(dmas)} transfers, "
                        f"{len(movedLines)} moved ({imageRanges} from the image), "
                        f"{len(assets)} assets ({sum(s for _, s, _, _ in assets)} bytes; {forms or 'none'}), "
                        f"{len(previewLines)} previews; {top}"
                        f"; sources: {sources or 'none'}, {spanned} bytes spanned, {used} used")
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
    pool.shutdown()

    print()
    print(f"{len(roms) - failures} of {len(roms)} images OK")
    if args.facts:
        print(f"{factTotals['accesses']} accesses, {factTotals['valued']} with a value; "
              f"{factTotals['dmas']} transfers, {factTotals['sourced']} with a source, "
              f"{factTotals['counted']} with a count, {factTotals['started']} with a start, "
              f"{factTotals['contradicted']} the run contradicted; "
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
        print("\nfiles lifted by kind, whole corpus:")
        for k, v in corpusAssetKinds.most_common():
            print(f"  {k:<12} {v} files, {corpusAssetKindBytes[k]} bytes")
        print("\nfiles lifted by form, whole corpus:")
        for k, v in corpusForms.most_common():
            print(f"  {k:<12} {v} files, {corpusFormBytes[k]} bytes")
        print(f"\nlandings by what the memory was used as, whole corpus ({factTotals['landed']} landed lines):")
        for k, v in corpusAreas.most_common():
            print(f"  {k:<32} {v}")
        print("\nlandings by depth, whole corpus:")
        for k, v in corpusDepths.most_common():
            print(f"  {k:<12} {v}")
        print(f"\nwalks by unit and form, whole corpus ({factTotals['walked']} walked lines):")
        for k, v in corpusWalks.most_common():
            print(f"  {k:<16} {v}")
        print(f"\npreviews by form, whole corpus ({factTotals['previews']} preview lines):")
        for k, v in corpusPreviews.most_common():
            print(f"  {k:<12} {v}")
        print(f"\nstaged sources, whole corpus ({factTotals['staged']} staged lines, "
              f"{factTotals['streamed']} streams; {factTotals['sourceSpanned']} bytes spanned, "
              f"{factTotals['sourceUsed']} used):")
        for k, v in corpusSources.most_common():
            print(f"  {k:<12} {v}")
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
