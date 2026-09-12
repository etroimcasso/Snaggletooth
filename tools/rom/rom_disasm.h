#pragma once

// A whole cartridge disassembled into a source tree that assembles back to the
// image: one file per region of the bus, the sound program the cartridge uploads
// at boot as a file of its own, and a manifest that says where every file's
// bytes land in the image, where the trace began, and where it stopped. The
// disassembler writes the two programs in the intermediate representation —
// `program.snagir` for the main CPU, `apu.snagir` for the sound program — and
// the manifest; every source file is rendered from those by `snes_render`.
//
// The trace starts at the handlers the header names and follows control flow
// across banks: a call or a jump into another region enters that region's trace
// under the mode it was made in, so a byte anywhere in the image is code only
// when execution can reach it from a vector — or from an entry a person adds to
// the manifest, which is how the trace gets past a jump table or a pointer the
// bytes cannot resolve.
//
// The sound program is found by running the machine rather than by reading the
// upload loop: the audio unit's memory after the boot says which bytes were sent
// and where, and the audio CPU's own program counter says where the program
// starts. Each uploaded block is matched back to the image bytes it was read
// from, so the block is written once, as SPC700 source, and the bank that
// carried it leaves that range to the sound program's file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/ir.h"
#include "rom/rom_facts.h"
#include "rom/rom_observe.h"
#include "rom/rom_render.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {

// An address range one source file is written for. A region lies within one
// bank and covers image bytes in order, so its file starts at `first` and runs
// without a gap to `last`.
struct SourceRegion {
  std::string file;
  Address first = 0;
  Address last = 0;  // inclusive
};

// One region per bank the image occupies under `map`, in image order, each named
// `bank_XX.asm` after its bank. Every image byte is placed once, at the address
// `romAddress` reports for its offset, so a mirror is never written twice. This is
// the split the tool writes when the manifest does not name one.
[[nodiscard]] std::vector<SourceRegion> bankRegions(CartridgeMap map, std::size_t imageBytes);

// An entry point the trace starts from: a handler the header names, or an address
// a person adds. `mode` is the CPU mode execution arrives in; `name` is the label
// the address takes.
struct TraceEntry {
  Address address = 0;
  Cpu65816Mode mode;
  std::string name;
};

// A run of bytes the cartridge sends to the audio unit at boot, and where in the
// image those bytes were read from — absent when no place in the image holds
// exactly those bytes, or more than one does. A run the image holds in pieces is
// reported as those pieces, each at its own place, so two programs uploaded end
// to end from two places in the image are two blocks.
struct UploadBlock {
  std::uint16_t apuAddress = 0;
  std::vector<std::uint8_t> bytes;
  std::optional<std::size_t> romOffset;
};

// What the boot upload sent: the blocks in address order, and the address the
// audio CPU started the program at.
struct UploadCapture {
  std::uint16_t entry = 0;
  std::vector<UploadBlock> blocks;
};

// Boots `rom` on the machine and watches the audio unit until its CPU leaves the
// upload stub for the program it was sent, or `masterCycles` elapse first. Returns
// what was uploaded, or nothing with `reason` set when no program started in time.
// The upload is read from the audio memory two boots leave behind, one over
// cleared memory and one over memory filled with $FF: a byte the upload wrote
// reads the same after both, and a byte it never touched reads differently.
// `progress`, when given, is told `booting the sound program` as each of the
// two boots begins and every tenth of a second of the master clock after, with
// the cycles spent against `masterCycles`, and once more as each boot ends —
// short of the budget when the program started early (`rom/progress.h`).
[[nodiscard]] std::optional<UploadCapture> captureUpload(std::span<const std::uint8_t> rom,
                                                         std::uint64_t masterCycles,
                                                         std::string& reason,
                                                         const ProgressSink& progress = {});

// Where the trace stopped and why: an address whose successors the bytes do not
// name, or that reads two ways. A person answers a stop with an entry.
struct TraceStop {
  Address address = 0;
  std::string reason;
};

// A region and the listing traced for it. The listing covers the region whole;
// the bytes a sound-program block was read from are left out when the file is
// rendered, not here.
struct RegionListing {
  SourceRegion region;
  Listing listing;
};

// The sound program: its file, what the boot sent, and the listing traced from
// the entry over the uploaded blocks.
struct SoundProgram {
  std::string file;
  UploadCapture capture;
  Listing listing;
};

// A file of bytes the tree lifts out of a bank file: a range a run saw a
// transfer engine carry from the image to the hardware — or the image source of
// a range it carried out of work RAM, or the run of image bytes a stream the
// CPU carried was read from, or a range the code proves a channel was set up
// to carry that no run started — written once, as the bytes are in the image,
// under a directory named for the memory they went to — `vram/`, `cgram/`,
// `oam/`, `apu/`, `hdma/` for a table and for a block an indirect entry pointed
// at, and `staged/` for a source whose bytes were built into data for two
// classes — and, for VRAM, for what the PPU used that memory as where the run
// saw every landing of the file's bytes drawn: `maps/` when every landing lies
// in a layer's screen, `tiles/` when every one lies in a name base or the
// sprite tiles, `vram/` otherwise — and named by the address of its first
// byte. The bank file refers to it with `INCBIN` where the bytes were. Ranges
// that share a byte are one file, the union; ranges that touch are not.
//
// The file is written in the form its path's extension names
// (`docs/asset-formats.md`): a tile sheet as `.png` at the depth its landings
// were read at, carrying the palette the run held at the frame that read
// them or a ramp; a palette as `.pal`; a tilemap as `.map`; a sprite table as
// `.oam`; an HDMA table as `.hdma` under the unit and the form the engine
// walked it; and `.bin` for bytes as they are — a source a routine built its
// data from, a file under `vram/`, `apu/` or `staged/`, an indirect block, a
// file shorter than one unit of its form, or one whose form does not give
// the bytes back. `bytes` are the image's; `written` is the file as it goes
// to disk, the form's encoding of them or the bytes themselves.
struct AssetFile {
  std::string file;  // relative to the manifest: `tiles/00_9000.png`
  std::vector<RegisterClass> classes;  // of the registers the bytes went to, ascending, one for every file but a `staged/` one
  MovedKind kind = MovedKind::Dma;
  Address registerAddress = 0;  // the register itself, for the comment beside the `INCBIN`
  Address first = 0;            // the address the tree places its first byte at
  std::size_t romOffset = 0;
  std::vector<std::uint8_t> bytes;
  unsigned depth = 0;                 // a tile sheet's bits a pixel, 2, 4 or 8; 0 for any other form
  std::vector<std::uint8_t> palette;  // a tile sheet's palette as RGBA quadruples, the run's or a ramp
  unsigned unit = 1;                  // an HDMA table's bytes a line
  bool indirect = false;              // and whether its entries carry pointers
  std::vector<std::uint8_t> written;
};

// A file written beside a lifted file to show what its bytes became, which
// nothing reads back and no bank file includes: its path, the lifted file it
// is of, the form it is in as the manifest's `preview` line names it —
// `tiles`, `palette`, `map`, `oam`, `hdma`, `mode7-tiles`, `mode7-map` — how
// many distinct contents it combines, for a source a routine built its data
// from (zero for a Mode 7 file's, whose bytes are their own picture), and the
// file's bytes.
struct PreviewFile {
  std::string file;
  std::string of;
  std::string form;
  std::uint32_t contents = 0;
  std::vector<std::uint8_t> written;
};

// A WAV written of a sample a run's key-on named
// (`docs/asset-formats.md` §The listening copy), which nothing includes and
// the verifier never reads: its path — `apu/<name>-<address>.wav` beside the
// file of the tree that holds the sample's bytes, `apu/samples/<address>.wav`
// for a sample the image holds nowhere whole — the sample's `start` and `loop`
// addresses in the audio memory, its `bytes` as the audio memory held them,
// the file the bytes are `in` and their `romOffset` where the image holds
// them at exactly one place, both absent otherwise, how many key-ons named
// it, and the WAV's bytes.
struct SampleFile {
  std::string file;
  std::uint16_t start = 0;
  std::uint16_t loop = 0;
  std::vector<std::uint8_t> bytes;
  std::string in;
  std::optional<std::size_t> romOffset;
  std::uint32_t times = 1;
  std::vector<std::uint8_t> written;
};

// An asset as the manifest records it, read back for its path — a person's
// rename survives a run when the file it names is lifted again with the same
// first byte and length — and, for a file the run's shadow named (`staged`,
// `stream`), for the file itself: its evidence is written fresh by a run, so
// the line is what keeps it from one disassembly to the next.
struct ManifestAsset {
  std::string file;
  Address first = 0;
  std::size_t bytes = 0;
  std::vector<RegisterClass> classes;
  MovedKind kind = MovedKind::Dma;
};

// A whole cartridge, disassembled.
struct CartridgeDisassembly {
  CartridgeHeader header;
  std::size_t imageBytes = 0;
  std::vector<TraceEntry> entries;  // every entry the trace started from, the vectors first
  std::vector<RegionListing> regions;
  // Every region's code lifted once into the intermediate representation
  // (`ir/ir.h`): the nodes in address order, an address two paths read two ways
  // as two nodes with the listing's reading first, and the two interrupt
  // sequences. The facts are proven over it, `program.snagir` is written from
  // it, and the bank files are rendered from that file read back. Its `spc700`
  // nodes are the sound program's listing lifted the same way, one node per
  // code line; `apu.snagir` is written from them and the sound file rendered
  // from that file read back.
  ir::Program program;
  std::optional<SoundProgram> sound;
  std::vector<TraceStop> stops;
  std::vector<std::string> notes;  // what the run could not do, in words
  // What the traced code reaches, attached to the addresses that reach it, the
  // routines those addresses belong to, and what every path proves about the
  // direct register, the data bank and the stack pointer at each label. All
  // four are written fresh on every run and read back by nothing — see
  // `rom_facts.h`.
  std::vector<HardwareAccess> accesses;
  std::vector<DmaTransfer> dmas;
  std::vector<Routine> routines;
  std::vector<StateFact> states;
  // The targets a run saw the indirect jumps take, this run's and every earlier
  // manifest's, each traced from as an entry — see `rom_observe.h`.
  std::vector<ReachedTarget> reached;
  // The landings a run saw the instructions not name — a return to an address
  // the code put on the stack, an `RTI` into flow the bytes do not carry — this
  // run's and every earlier manifest's, each traced from as a reached target is.
  std::vector<Landing> ran;
  // The direct register and the data bank the run saw at every site it executed
  // in the image. Written fresh on every run and read back by nothing: the next
  // run sees it again. Empty without a run.
  std::vector<SeenState> seen;
  // The ranges a run saw the transfer engines move, this run's and every earlier
  // manifest's, each once with how many times it was seen — see `rom_observe.h`.
  // Nothing is traced from them; they say where the bytes the hardware received
  // came from.
  std::vector<MovedRange> moved;
  // Where the ranges this run moved landed on the other side of the port, and
  // what the PPU used that memory as — see `rom_observe.h`. Written fresh on
  // every run and read back by nothing; the directory a VRAM file takes from
  // them is kept by its `asset` line. Empty without a run.
  std::vector<LandedRange> landed;
  // How every HDMA table and indirect block the run moved was walked — the
  // transfer unit and the form — see `rom_observe.h`; and the palette RAM as
  // it stood at each drawn frame a landing was read at, which the landings
  // name by index. Both written fresh on every run, `walked` as its line and
  // the palettes through the tile sheets that carry them; empty without a
  // run.
  std::vector<WalkedRange> walked;
  std::vector<std::vector<std::uint8_t>> palettes;
  // The files lifted out of the bank files, in address order: every `moved`
  // range that begins in the image and goes to a memory a file can be named for,
  // every source the shadow named, and every transfer the code proves whole
  // that no run started, by the rules `docs/snes-disassembler.md` states. A
  // range that is refused — over an instruction, over a sound-program block, or
  // sent two places — is named in `notes` and stays in its bank; so is a
  // proven transfer the run moved another way.
  std::vector<AssetFile> assets;
  // The previews written beside the lifted files, in the assets' order and,
  // within a file, in the order the run first carried each content. Written
  // fresh by a run and read back by nothing; empty without one.
  std::vector<PreviewFile> previews;
  // The samples the run's key-ons named, in the run's order (`rom_observe.h`),
  // each matched whole to the image and written as a WAV. Written fresh by a
  // run and read back by nothing; empty without one.
  std::vector<SampleFile> samples;
  // The targets the bytes prove the indirect jumps take — a pointer in the image
  // selected by an index every path bounds — this run's and every earlier
  // manifest's, each traced from as an entry — see `rom_facts.h`. A jump every
  // one of whose destinations is derived is not among `stops`.
  std::vector<DerivedTarget> derived;
  // Where every range the run saw carried out of work RAM came from, and the
  // streams the CPU carried a byte at a time — see `rom_observe.h`. Both are
  // written fresh on every run and read back by nothing; a staged range with
  // an image source is lifted as that source, and a stream as the run its
  // carrier read — or, for a buffer the CPU carried out, as the buffer's source.
  std::vector<StagedRange> staged;
  std::vector<StreamedRange> streamed;
};

// The text of one of the tree's files, by the path the manifest names, or
// nothing when it cannot be read.
using FileReader = std::function<std::optional<std::string>(const std::string& file)>;

// What to disassemble. `entries` are the entry points beyond the vectors, which
// are always traced. `regions` is the file split, or empty for one file per bank.
// `captureSound` boots the machine to find the sound program, within
// `bootMasterCycles` of the master clock — fifteen seconds of it by default, which
// is more than a cartridge that clears its memory, unpacks its program and streams
// tens of kilobytes of samples takes to start it.
struct CartridgeRequest {
  std::span<const std::uint8_t> rom;
  std::vector<TraceEntry> entries;
  std::vector<SourceRegion> regions;
  std::vector<ReachedTarget> reached;  // what earlier runs saw, read back from the manifest
  std::vector<Landing> ran;            // where earlier runs landed, read back the same way
  std::vector<MovedRange> moved;       // what earlier runs saw move, read back the same way
  std::vector<ManifestAsset> assets;   // the paths the manifest gives the lifted files
  std::vector<DerivedTarget> derived;  // what earlier runs derived, read back the same way
  // Reads a lifted file the manifest names, as it lies in the tree, for a
  // disassembly without a run: the depth and the palette of a tile sheet and
  // the unit of a table are the run's facts, and without a run they are read
  // from the file the path names before it is written again. Nothing, or a
  // reader that finds nothing, writes every such file as bytes.
  FileReader readFile;
  bool captureSound = true;
  std::uint64_t bootMasterCycles = 15u * 21'477'272u;
  // `observeRun` boots the machine and steps it for `runMasterCycles` — sixty
  // seconds of the master clock — recording the targets the indirect jumps take
  // and the landings the instructions do not name, which the trace then starts
  // from beside the vectors and entries, the ranges the transfer engines move,
  // and the registers seen at every site. Off unless asked for: the run costs
  // about as long as it emulates, and a caller that wants the trace alone
  // should not pay it. `snes_disasm` asks for it unless told `--no-run`.
  bool observeRun = false;
  std::uint64_t runMasterCycles = 60u * 21'477'272u;
  // The recorded run replayed into the controller ports while the machine runs —
  // see `rom/input_script.h`. Empty, the ports stay empty and the run is the boot
  // alone. `snes_disasm --input <script>` supplies one.
  InputScript input;
  // Told what the disassembly is doing as it goes, when set: `running the
  // cartridge` with the cycles spent against `runMasterCycles` every tenth of
  // a second of the master clock, then `tracing`, then `proving what every
  // path reaches`, then `booting the sound program` against `bootMasterCycles`
  // for each of the two boots (`rom/progress.h`). Nothing is printed by the
  // library; `snes_disasm` prints these to standard error unless `--quiet`.
  ProgressSink progress;
};

// Disassembles the cartridge: the header, the regions traced from every entry with
// control flow carried across them, the sound program when one is uploaded, and
// the stops.
[[nodiscard]] CartridgeDisassembly disassembleCartridge(const CartridgeRequest& request);

// The image the source tree assembles back to, built from the bytes each listing
// carries — an instruction's own bytes or a data run, placed at the image offset
// its address reads from — and from the sound-program blocks matched to the image.
// `unplaced` counts image bytes no file carries; `placedTwice` counts bytes two
// files carry. Both are zero for a complete tree.
struct Placement {
  std::vector<std::uint8_t> image;
  std::size_t unplaced = 0;
  std::size_t placedTwice = 0;
};
[[nodiscard]] Placement placeBytes(const CartridgeDisassembly& disassembly);

// The manifest as text — the grammar is `docs/project-manifest.md`.
[[nodiscard]] std::string renderManifest(const CartridgeDisassembly& disassembly);

// A sound-program block as the manifest records it: the audio address the
// cartridge sent the bytes to, how many, and the image offset they were read
// from — absent for a block the image does not hold at exactly one place.
struct ManifestBlock {
  std::uint16_t apuAddress = 0;
  std::size_t length = 0;
  std::optional<std::size_t> romOffset;
};

// The sound program as the manifest records it: its file, where the audio CPU
// starts it, and its blocks in address order.
struct ManifestSound {
  std::string file;
  std::uint16_t entry = 0;
  std::vector<ManifestBlock> blocks;
};

// A transfer the code set up, as the `dma` line records the part the renderer
// reads: the site, and the destination register, the source and the byte count
// where the bytes said them.
struct ManifestDma {
  Address site = 0;
  std::optional<Address> destination;
  std::optional<Address> source;
  MovedStep step = MovedStep::Increment;  // as the line says it; increment where it says `none`
  std::optional<std::size_t> bytes;
};

// What a manifest gives the tools that read it. The next disassembly takes the
// entries, the reached and derived targets, the landings, the moved ranges, the
// assets' paths and the file split; a verification takes the map, the file
// split, the sound program and its blocks, which together say where every
// file's bytes land; both take the image identity. Everything else in a
// manifest is what the last run found, and is written fresh.
struct ManifestInput {
  std::vector<TraceEntry> entries;
  std::vector<SourceRegion> regions;
  std::vector<ReachedTarget> reached;
  std::vector<Landing> ran;
  std::vector<MovedRange> moved;
  std::vector<ManifestAsset> assets;
  std::vector<DerivedTarget> derived;
  std::optional<CartridgeMap> map;
  std::optional<ManifestSound> sound;
  std::optional<std::size_t> imageBytes;
  std::optional<std::uint16_t> checksum;
  // What the renderer reads: the accesses, the routines with their calls
  // resolved to addresses, the direct registers a run saw, and the transfers
  // the code set up, each as its line says it (`rom/rom_render.h`).
  std::vector<RenderAccess> accesses;
  std::vector<RenderRoutine> routines;
  std::vector<RenderSeen> seen;
  std::vector<ManifestDma> dmas;
};

// Reads the entries, reached and derived targets, landings, moved ranges,
// assets, regions, map, sound program and image identity out of a manifest,
// and the accesses, routines, seen registers and transfers the renderer reads.
// Nothing, with `error` naming the line, when a line does not parse, when a
// block names a file no `sound` line does, or when a routine calls a label no
// routine line names.
[[nodiscard]] std::optional<ManifestInput> parseManifest(std::string_view text, std::string& error);

// Why `input` cannot direct a run over `rom`, or an empty string when it can: a
// manifest names the size and checksum of the image it was written for, and an
// entry or a file split meant for one image is meaningless over another.
[[nodiscard]] std::string manifestMismatch(const ManifestInput& input,
                                           std::span<const std::uint8_t> rom);

// The renderer's input as a disassembly holds it in memory: every region with
// its listing, and the facts. What `readRenderInput` (`rom/rom_render.h`)
// builds from a tree on disk, this builds from the disassembly, so a bank file
// rendered from the tree and one rendered from the disassembly can be held
// equal.
[[nodiscard]] RenderInput renderInputOf(const CartridgeDisassembly& disassembly);

// A region's source file rendered from the disassembly in memory:
// `renderRegion` of `rom/rom_render.h` over `renderInputOf(disassembly)` and
// the disassembly's program. The tree on disk is not written this way — the
// disassembler writes the program file and the manifest, and `snes_render`
// renders from them — so this is the check that the two paths agree.
[[nodiscard]] std::string renderRegion(const RegionListing& region,
                                       const CartridgeDisassembly& disassembly);

// The sound program's source file rendered from the disassembly in memory:
// `renderSoundFile` of `rom/rom_render.h` over `renderInputOf(disassembly)`
// and the disassembly's program — the check that a sound file rendered from
// the tree agrees with the disassembly that wrote it. `std::logic_error` when
// no sound program was captured.
[[nodiscard]] std::string renderSoundFile(const CartridgeDisassembly& disassembly);

// The program file as text, in the grammar `docs/snagir.md` gives: the
// disassembly's program, with the image's size and map and each region's file,
// range, warnings, labels and data runs beside it.
[[nodiscard]] std::string renderProgramFile(const CartridgeDisassembly& disassembly);

// The sound program's file as text, in the same grammar under `snagir 1 apu;`:
// the disassembly's `spc700` nodes, with one region per run of uploaded
// addresses — its range in the audio unit's space, the listing's warnings on
// the first, and the labels and data runs within it. `std::logic_error` when
// no sound program was captured.
[[nodiscard]] std::string renderSoundProgramFile(const CartridgeDisassembly& disassembly);

// Writes what the disassembly found under `directory`, creating it and its
// directories: `program.snagir` first, then `apu.snagir` where a sound program
// was captured, `project.snagifest`, every lifted file in its form, every
// preview, and every sample's WAV. No bank file and no sound file is written here; `snes_render`
// writes those from the program files and the manifest. False, with `error`
// set, when a file cannot be written.
bool writeProject(const CartridgeDisassembly& disassembly, const std::filesystem::path& directory,
                  std::string& error);

}  // namespace snaggletooth::disasm
