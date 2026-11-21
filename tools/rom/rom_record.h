#pragma once

// What a run of the cartridge saw, kept as a file beside the tree it was made
// for. A run's observation (`rom/rom_observe.h`) is everything the tree takes
// from the machine: the targets the indirect jumps took, the ranges the engines
// moved and where they landed, the extents carried out of work RAM with the
// image bytes they were built from, the streams, the samples, and the outcomes
// of the routines the toolkit ran. A record holds one observation and the run's
// notes as text, kept in its file compressed with zstd, so a tree can hold what
// every run over it found, and records from several runs can be joined into one
// observation.
//
//   std::vector<std::string> notes;
//   const RunObservation seen = observeRun(rom, cycles, script, notes);
//   std::string error;
//   writeRecord(RunRecord{.observation = seen, .notes = notes, .enumerationNotes = {}}, rom, directory, error);
//   const std::optional<std::vector<RunRecord>> records = readRecords(directory, rom, error);
//   const RunRecord all = foldRecords(*records);
//
// The grammar is `docs/run-record.md`.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rom/progress.h"
#include "rom/rom_observe.h"

namespace snaggletooth::disasm {

// A run's record: what one run of the cartridge saw, as a file a tree keeps.
// The disassembler writes one when a run ends, under the tree's `runs/`, and a
// later disassembly over the tree reads every record there, so what a run found
// stays in the tree whether or not a later run finds it again. The grammar is
// `docs/run-record.md`.
//
// `notes` are what the run said as it played; `enumerationNotes` are what the
// toolkit's runs of the routines said, which a manifest says after the plan's
// notes, where the run's own stand before the trace's.
struct RunRecord {
  RunObservation observation;
  std::vector<std::string> notes;             // the run's notes, in the order made
  std::vector<std::string> enumerationNotes;  // the enumeration's notes, in the order made
  friend bool operator==(const RunRecord&, const RunRecord&) = default;
};

// The record as text, written for the image `rom`: its size and checksum head
// the file, as a manifest's do.
[[nodiscard]] std::string renderRecord(const RunRecord& record, std::span<const std::uint8_t> rom);

// The record as text, for an image known by its size and the checksum its
// header names — nothing for an image with no header — as a disassembly holds
// them (`CartridgeDisassembly::imageBytes`, `header.checksum`). The same text
// as the form above for that image.
[[nodiscard]] std::string renderRecord(const RunRecord& record, std::size_t imageBytes,
                                       std::optional<std::uint16_t> checksum);

// Reads a record. Nothing, with `error` naming the line and what was expected,
// when a line does not parse or names a kind the grammar does not hold. A
// register's name and class are those of its address, and a target's and a
// landing's names are empty, as a run reports them.
[[nodiscard]] std::optional<RunRecord> parseRecord(std::string_view text, std::string& error);

// Why a record cannot be read over `rom`, or an empty string when it can: a
// record names the size and checksum of the image its run played.
[[nodiscard]] std::string recordMismatch(std::string_view text, std::span<const std::uint8_t> rom);

// Joins what a later run saw to what earlier runs saw, in place. Each kind is
// joined by the identity the run itself keeps it under:
//   reached, ran       a sighting or a landing either holds, once;
//   moved, landed,     a range, a landing or a walk both hold, once, with the
//   walked, streamed,  counts added — a stream both hold keeps `into`'s source;
//   samples
//   palettes           each distinct palette once, and every landing of `from`
//                      names its palette's place in the joined list;
//   seen               one entry a site, holding every value either saw there;
//   staged             one entry an extent: the origins joined
//                      (`ir::Origins::merge`), a writer both hold with its
//                      bytes added, its origin joined and its sources united,
//                      and each distinct content once — `into`'s where both
//                      hold it, and `from`'s others after every content of
//                      `into`, in `from`'s own order of carries;
//   enumerated         outcomes that agree in everything but their values are
//                      one outcome holding the values of both; any other is an
//                      outcome of its own;
//   counts             the instruction, interrupt and divergence counts and
//                      the enumeration's are added; `nodes`, `spc700Nodes`,
//                      `originSets` and `originCap` take the larger.
// Every list stands afterward in the order `RunObservation` states for it.
// Joining an observation to an empty one gives that observation.
void joinObservation(RunObservation& into, const RunObservation& from);

// The records a tree holds, in the order of their numbers, read from
// `<directory>/runs/`: every file there named `<digits>.snagrun.zst`, each one
// zstd frame holding a record's text. Nothing, with `error` the file's path,
// `": "` and what is wrong, when one is not a whole zstd file, does not parse or
// was written for another image. A tree with no `runs/` holds none. `progress`,
// when given, is told `decompressing runs/<name> (<size>)` as each file begins,
// the file's size in decimal MB to a tenth, or whole kB under a megabyte
// (`rom/progress.h`).
[[nodiscard]] std::optional<std::vector<RunRecord>> readRecords(const std::filesystem::path& directory,
                                                                std::span<const std::uint8_t> rom, std::string& error,
                                                                const ProgressSink& progress = {});

// How many records the tree holds, each file read only as far as its first two
// lines: the format's line and the image the run played. Nothing, with `error`
// as `readRecords` sets it, when one is not a zstd file, does not begin as a
// record does, or was written for another image. What `readRecords` refuses in
// a record's body this does not look at; it costs a fraction of a second where
// reading the records costs seconds.
[[nodiscard]] std::optional<std::size_t> checkRecords(const std::filesystem::path& directory,
                                                      std::span<const std::uint8_t> rom, std::string& error);

// Every record joined into one, in order. Records whose observations agree in
// everything but their `enumerated` outcomes and the enumeration's counts are
// one run: its sightings count once, and its outcomes are joined. Notes, and
// the enumeration's notes apart from them, are joined each once, in the order
// first made.
[[nodiscard]] RunRecord foldRecords(std::span<const RunRecord> records);

// Writes `record` as the next file under `<directory>/runs/`, numbered one past
// the highest there, four digits or more, from `0001`, named `.snagrun.zst`:
// the text as one zstd frame, compressed at level 19 with long-distance
// matching over a 128 MB window and the frame's content size and checksum
// written, on one thread, so one text is one file's bytes on every platform. A
// record whose text a file there already holds is not written again. False,
// with `error` set, when the file cannot be written.
bool writeRecord(const RunRecord& record, std::span<const std::uint8_t> rom, const std::filesystem::path& directory,
                 std::string& error);

// `writeRecord` for an image known by its size and checksum, as
// `renderRecord`'s second form takes them.
bool writeRecord(const RunRecord& record, std::size_t imageBytes, std::optional<std::uint16_t> checksum,
                 const std::filesystem::path& directory, std::string& error);

}  // namespace snaggletooth::disasm
