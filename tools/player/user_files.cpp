#include "player/user_files.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace snaggletooth::player {
namespace {

// A name is refused outright when it could name something outside the root. The
// checks are spelt out rather than left to the path type, because what counts as
// absolute is not the same on every platform: a leading separator is not what
// makes a path absolute on Windows, and a drive designator is not a separator at
// all on the others, where `C:\x` would otherwise pass as an ordinary file name.
[[noreturn]] void refuse(const std::string& name, const char* why) {
  throw std::invalid_argument("a file kept here cannot be named \"" + name + "\": " + why);
}

void checkName(const std::string& name) {
  if (name.empty()) refuse(name, "it names nothing");
  if (name.front() == '/' || name.front() == '\\') refuse(name, "it begins at a root");
  if (name.find('\\') != std::string::npos) refuse(name, "a separator here is /");
  if (name.find(':') != std::string::npos) refuse(name, "it carries a drive");
  const std::filesystem::path path(name);
  if (path.is_absolute() || path.has_root_name()) refuse(name, "it is an absolute path");
  for (const std::filesystem::path& part : path) {
    if (part == "..") refuse(name, "it climbs out of the store");
  }
}

}  // namespace

std::filesystem::path UserFiles::pathFor(const std::string& name) const {
  checkName(name);
  return root_ / std::filesystem::path(name);
}

void UserFiles::write(const std::string& name, std::span<const std::uint8_t> bytes) const {
  const std::filesystem::path target = pathFor(name);
  std::error_code code;
  std::filesystem::create_directories(target.parent_path(), code);
  if (code) {
    throw std::runtime_error("cannot make " + target.parent_path().string() + ": " + code.message());
  }

  // The bytes go beside the target and arrive under its name in one step, so a
  // reader sees the whole of one version or the whole of the other, and a run that
  // dies here leaves what was already kept.
  std::filesystem::path temporary = target;
  temporary += ".writing";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + temporary.string());
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) throw std::runtime_error("cannot write " + temporary.string());
  }  // closed before the rename: a file still open cannot be moved on every platform

  std::filesystem::rename(temporary, target, code);
  if (code) {
    std::filesystem::remove(temporary, code);
    throw std::runtime_error("cannot put " + target.string() + " in place: " + code.message());
  }
}

std::optional<std::vector<std::uint8_t>> UserFiles::read(const std::string& name) const {
  const std::filesystem::path path = pathFor(name);
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

bool UserFiles::exists(const std::string& name) const {
  std::error_code code;
  return std::filesystem::is_regular_file(pathFor(name), code);
}

bool UserFiles::remove(const std::string& name) const {
  std::error_code code;
  return std::filesystem::remove(pathFor(name), code);
}

std::string UserFiles::saveNameFor(const std::filesystem::path& image) {
  // Only the image's own file name is taken. Where a cartridge was opened from is
  // the person's business and changes between runs; where its save goes does not.
  std::filesystem::path name = image.filename();
  name.replace_extension(kSaveExtension);
  return std::string(kSramDir) + "/" + name.string();
}

std::string chooseConfigPath(const std::string& flag, const UserFiles* files) {
  if (!flag.empty()) return flag;
  if (files == nullptr) return {};
  const std::filesystem::path kept = files->pathFor(kDefaultPadConfigName);
  std::error_code code;
  if (std::filesystem::is_regular_file(kept, code)) return kept.string();
  return {};  // the built-in text, which is what a tool handed nothing runs with
}

CartridgeSave::Load CartridgeSave::loadInto(std::vector<std::uint8_t>& window, std::size_t& found,
                                            std::size_t& kept) {
  found = 0;
  kept = window.size();
  const std::optional<std::vector<std::uint8_t>> file = files_.read(name_);
  if (!file) return Load::NoFile;
  found = file->size();
  if (found != kept) return Load::WrongSize;

  window = *file;
  // What the file holds is known now, so the report that follows putting this into
  // the machine does not write it straight back.
  last_ = *file;
  return Load::Loaded;
}

void CartridgeSave::changed(std::span<const std::uint8_t> save) {
  if (save.empty()) return;  // a cartridge with no save window keeps no file
  if (last_.size() == save.size() &&
      std::equal(save.begin(), save.end(), last_.begin())) {
    return;  // the file would say exactly what it says already
  }
  try {
    files_.write(name_, save);
  } catch (const std::exception& refused) {
    // Called from inside a running machine: a person mid-game is told once that
    // their save is not reaching the disk, rather than dropped out of the run.
    if (error_.empty()) error_ = refused.what();
    return;
  }
  last_.assign(save.begin(), save.end());
  wrote_ = true;
}

}  // namespace snaggletooth::player
