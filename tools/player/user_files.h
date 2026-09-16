#pragma once

// Where the player keeps a person's files. The platform decides the directory —
// the player asks it once and hands the answer here — and this store does the rest:
// it holds a root, names files under it, and writes them so a file is either the
// bytes it held before or the bytes being written and never half of either.
//
// Nothing here knows what a SNES is, and nothing in the machine knows this exists.
// The machine says when a cartridge's save changed (SaveObserver, `snes/snes.h`);
// where that goes is this side's business entirely.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "snaggletooth/snes/snes.h"

namespace snaggletooth::player {

// The layout every file the player keeps lives in, relative to the root:
//
//   config/input/   the mappings a person writes, including the default.snagpad
//                   the player runs with when it is handed no configuration
//   sram/           <rom name>.srm, a cartridge's battery-backed save, in the
//                   form other emulators read and write
//
// Both are made by the first write into them, not by starting the player.
inline constexpr const char* kConfigInputDir = "config/input";
inline constexpr const char* kSramDir = "sram";

// The file a person's own mapping goes in for the player to find with no flag.
inline constexpr const char* kDefaultPadConfigName = "config/input/default.snagpad";

// The extension a cartridge's save takes: what bsnes, snes9x and Mesen read and
// write for the same cartridge, so a save made here is a save they can open.
inline constexpr const char* kSaveExtension = ".srm";

class UserFiles {
 public:
  // Rooted at a directory handed in. Nothing is created here — not the root, not a
  // directory under it — so constructing a store touches no disk at all.
  explicit UserFiles(std::filesystem::path root) : root_(std::move(root)) {}

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  // Where `name` lands. A name is a relative path under the root: an absolute
  // path, a drive designator or a `..` component throws std::invalid_argument, so
  // no file this store writes can land outside root(). Touches no disk.
  [[nodiscard]] std::filesystem::path pathFor(const std::string& name) const;

  // Writes `bytes` to `name`, whole or not at all: the bytes go to a sibling
  // temporary file which is closed and then renamed over the target, so a reader
  // sees one or the other and a run that dies mid-write leaves what was there.
  // Directories on the way are made. Throws std::invalid_argument for a name that
  // could leave the store, std::runtime_error when the disk refuses.
  void write(const std::string& name, std::span<const std::uint8_t> bytes) const;

  // The bytes of `name`, or nothing when there is no such file.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(const std::string& name) const;

  [[nodiscard]] bool exists(const std::string& name) const;

  // Removes `name`, answering whether there was a file to remove.
  bool remove(const std::string& name) const;

  // The two directories of the layout, named without being made.
  [[nodiscard]] std::filesystem::path configInput() const { return pathFor(kConfigInputDir); }
  [[nodiscard]] std::filesystem::path sram() const { return pathFor(kSramDir); }

  // The name a cartridge's save takes: the image's own file name with its
  // extension replaced, under the save directory — `game.sfc` becomes
  // `sram/game.srm`. The name passes the same containment check as any other, so
  // an image called `../x.sfc` is refused rather than writing above the root.
  [[nodiscard]] static std::string saveNameFor(const std::filesystem::path& image);

 private:
  std::filesystem::path root_;
};

// Which mapping the tool runs with, in the order a person expects: what they named
// on the command line, else one of their own kept with their files, else the one
// built in. `flag` is what --config was given, empty when it was not; `files` is
// the store, or nothing when the platform would not say where a person's files go.
// Answers the path to read, or empty for the built-in text.
[[nodiscard]] std::string chooseConfigPath(const std::string& flag, const UserFiles* files);

// A cartridge's save, kept in the store as the file other emulators read. It is
// what the machine's save observer talks to: the machine says the window changed
// and hands over the bytes, and this writes them where they belong — which is the
// whole of what either side knows about the other.
class CartridgeSave final : public SaveObserver {
 public:
  // What loading found, which the tool says once at start.
  enum class Load {
    NoFile,     // nothing kept yet; the cartridge boots on its power-on RAM
    Loaded,     // the file went into the window
    WrongSize,  // a file is there and is not the size this cartridge declares
  };

  CartridgeSave(UserFiles files, std::string name)
      : files_(std::move(files)), name_(std::move(name)) {}

  // Puts a kept save into `window` before the machine runs, when there is one and
  // it is exactly as large as the cartridge declares. A file of any other size is
  // left alone and reported: the cartridge boots on what it had, and nothing of a
  // person's is overwritten on a guess. `found` and `kept` carry the two sizes for
  // the report.
  Load loadInto(std::vector<std::uint8_t>& window, std::size_t& found, std::size_t& kept);

  // The machine's report. Writes the bytes when they differ from what was last
  // written or loaded, and does nothing when they do not — one comparison per
  // report, which is a handful in a session, rather than the per-frame comparison
  // the observer exists to make unnecessary.
  //
  // A disk that refuses is kept here rather than thrown: this is called from inside
  // a running machine, and a person mid-game is better told their save is not
  // reaching the disk than dropped out of the run. error() carries the first such
  // refusal, and the tool says it once.
  void changed(std::span<const std::uint8_t> save) override;

  // Whether anything has been written to the file this run.
  [[nodiscard]] bool wrote() const noexcept { return wrote_; }

  // The first refusal the disk gave, or empty when it has given none.
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  UserFiles files_;
  std::string name_;
  std::vector<std::uint8_t> last_;  // what the file holds, as far as this run knows
  std::string error_;               // the first refusal the disk gave
  bool wrote_ = false;
};

}  // namespace snaggletooth::player
