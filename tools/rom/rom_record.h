#pragma once

// What a run of the cartridge saw, kept as a file beside the tree it was made
// for. A run's observation (`rom/rom_observe.h`) is everything the tree takes
// from the machine: the targets the indirect jumps took, the ranges the engines
// moved and where they landed, the extents carried out of work RAM with the
// image bytes they were built from, the streams, the samples, and the outcomes
// of the routines the toolkit ran. A record holds one observation and the run's
// notes as text, so a tree can hold what every run over it found, and records
// from several runs can be joined into one observation.
//
//   std::vector<std::string> notes;
//   const RunObservation seen = observeRun(rom, cycles, script, notes);
//   std::string error;
//   writeRecord(RunRecord{.observation = seen, .notes = notes}, rom, directory, error);
//   const std::optional<std::vector<RunRecord>> records = readRecords(directory, rom, error);
//   const RunRecord all = foldRecords(*records);
//
// The grammar is `docs/run-record.md`.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rom/rom_observe.h"

namespace snaggletooth::disasm {

// A run's record: what one run of the cartridge saw, as a file a tree keeps.
// One is written when a run ends, as the next file under the tree's `runs/`
// (`writeRecord`), and every record there is read back together
// (`readRecords`), so what each run found can be joined into one observation
// (`foldRecords`). The grammar is `docs/run-record.md`.
struct RunRecord {
  RunObservation observation;
  std::vector<std::string> notes;  // the run's notes, in the order made
  friend bool operator==(const RunRecord&, const RunRecord&) = default;
};

// The record as text, written for the image `rom`: its size and checksum head
// the file, as a manifest's do.
[[nodiscard]] std::string renderRecord(const RunRecord& record, std::span<const std::uint8_t> rom);

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
// `<directory>/runs/`: every file there named `<digits>.snagrun`. Nothing, with `error` the file's
// path, `": "` and what is wrong, when one does not parse or was written for
// another image. A tree with no `runs/` holds none.
[[nodiscard]] std::optional<std::vector<RunRecord>> readRecords(const std::filesystem::path& directory,
                                                                std::span<const std::uint8_t> rom, std::string& error);

// Every record joined into one, in order. Records whose observations agree in
// everything but their `enumerated` outcomes and the enumeration's counts are
// one run: its sightings count once, and its outcomes are joined. Notes are
// joined each once, in the order first made.
[[nodiscard]] RunRecord foldRecords(std::span<const RunRecord> records);

// Writes `record` as the next file under `<directory>/runs/`, numbered one past
// the highest there, four digits or more, from `0001`. A record whose text a
// file there already holds is not written again. False, with `error` set, when
// the file cannot be written.
bool writeRecord(const RunRecord& record, std::span<const std::uint8_t> rom, const std::filesystem::path& directory,
                 std::string& error);

}  // namespace snaggletooth::disasm
