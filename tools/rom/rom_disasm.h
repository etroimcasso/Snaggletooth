#pragma once

// A whole cartridge disassembled into a source tree that assembles back to the
// image: one file per region of the bus, the sound program the cartridge uploads
// at boot as a file of its own, and a manifest that says where every file's
// bytes land in the image, where the trace began, and where it stopped.
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "rom/rom_facts.h"
#include "rom/rom_observe.h"
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
[[nodiscard]] std::optional<UploadCapture> captureUpload(std::span<const std::uint8_t> rom,
                                                         std::uint64_t masterCycles,
                                                         std::string& reason);

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

// A whole cartridge, disassembled.
struct CartridgeDisassembly {
  CartridgeHeader header;
  std::size_t imageBytes = 0;
  std::vector<TraceEntry> entries;  // every entry the trace started from, the vectors first
  std::vector<RegionListing> regions;
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
  // The ranges a run saw the transfer engines move, this run's and every earlier
  // manifest's, each once with how many times it was seen — see `rom_observe.h`.
  // Nothing is traced from them; they say where the bytes the hardware received
  // came from.
  std::vector<MovedRange> moved;
  // The targets the bytes prove the indirect jumps take — a pointer in the image
  // selected by an index every path bounds — this run's and every earlier
  // manifest's, each traced from as an entry — see `rom_facts.h`. A jump every
  // one of whose destinations is derived is not among `stops`.
  std::vector<DerivedTarget> derived;
};

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
  std::vector<MovedRange> moved;       // what earlier runs saw move, read back the same way
  std::vector<DerivedTarget> derived;  // what earlier runs derived, read back the same way
  bool captureSound = true;
  std::uint64_t bootMasterCycles = 15u * 21'477'272u;
  // `observeRun` boots the machine and steps it for `runMasterCycles` — sixty
  // seconds of the master clock — recording the targets the indirect jumps take,
  // which the trace then starts from beside the vectors and entries, and the
  // ranges the transfer engines move. Off unless asked for: the run costs about
  // as long as it emulates, and a caller that wants the trace alone should not
  // pay it. `snes_disasm` asks for it unless told `--no-run`.
  bool observeRun = false;
  std::uint64_t runMasterCycles = 60u * 21'477'272u;
  // The recorded run replayed into the controller ports while the machine runs —
  // see `rom/input_script.h`. Empty, the ports stay empty and the run is the boot
  // alone. `snes_disasm --input <script>` supplies one.
  InputScript input;
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

// What a manifest gives the tools that read it. The next disassembly takes the
// entries, the reached and derived targets, the moved ranges, and the file
// split; a verification takes the map, the file split, the sound program and
// its blocks, which together say where every file's bytes land; both take the
// image identity. Everything else in a manifest is what the last run found, and
// is written fresh.
struct ManifestInput {
  std::vector<TraceEntry> entries;
  std::vector<SourceRegion> regions;
  std::vector<ReachedTarget> reached;
  std::vector<MovedRange> moved;
  std::vector<DerivedTarget> derived;
  std::optional<CartridgeMap> map;
  std::optional<ManifestSound> sound;
  std::optional<std::size_t> imageBytes;
  std::optional<std::uint16_t> checksum;
};

// Reads the entries, reached and derived targets, moved ranges, regions, map,
// sound program and image identity out of a manifest. Nothing, with `error` naming the line, when a line does not parse,
// or when a block names a file no `sound` line does.
[[nodiscard]] std::optional<ManifestInput> parseManifest(std::string_view text, std::string& error);

// Why `input` cannot direct a run over `rom`, or an empty string when it can: a
// manifest names the size and checksum of the image it was written for, and an
// entry or a file split meant for one image is meaningless over another.
[[nodiscard]] std::string manifestMismatch(const ManifestInput& input,
                                           std::span<const std::uint8_t> rom);

// A region's source file. The instructions are written from the representation
// the listing lifts to (`ir/ir_render.h`), in pieces with an `ORG` where a piece
// starts and a comment where a sound-program block's bytes are left out; the
// data runs and the labels are the listing's. The file opens with an `EQU` line
// for every hardware register its absolute operands address and every label
// another file defines that it refers to, an absolute operand that addresses a
// register is written as the register's name, a direct-page operand that every
// path proves lands on a register carries the register's name in its comment, a
// target with a label anywhere in the tree is written as the label, and each
// routine that begins in the file carries a comment with its role, what it
// calls and what calls it.
[[nodiscard]] std::string renderRegion(const RegionListing& region,
                                       const CartridgeDisassembly& disassembly);

// The sound program's source file: its blocks, each under its own `ORG`.
[[nodiscard]] std::string renderSoundProgram(const SoundProgram& sound);

// Writes the manifest and every source file under `directory`, creating it.
// False, with `error` set, when a file cannot be written.
bool writeProject(const CartridgeDisassembly& disassembly, const std::filesystem::path& directory,
                  std::string& error);

}  // namespace snaggletooth::disasm
