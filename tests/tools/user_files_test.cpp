// Where the player keeps a person's files: the store rooted at a directory handed
// in, the layout under it, and a cartridge's save kept as the file other emulators
// read. The store's cases hold it to naming files under the root and refusing
// anything that could leave it, and to writing whole or not at all; the save's
// cases run a real machine and hold the file to what the machine reported — loaded
// before the first instruction, written when the window changed, and not rewritten
// when it would say the same thing. Nothing here needs a device, a window, or the
// directory the platform would really answer with.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "player/user_files.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::player {
namespace {

// A directory of this CASE's own, empty at the start of it. The name carries the
// case and the process, because the suite is run with its cases spread across
// processes: a directory shared between them is one case deleting another's files
// half way through, which is a failure that comes and goes with the scheduling.
class UserFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* const which =
        ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("snaggletooth-user-files-" + std::string(which == nullptr ? "case" : which->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  [[nodiscard]] UserFiles store() const { return UserFiles(dir_); }

  std::filesystem::path dir_;
};

std::vector<std::uint8_t> bytesOf(std::initializer_list<int> values) {
  std::vector<std::uint8_t> out;
  for (const int v : values) out.push_back(static_cast<std::uint8_t>(v));
  return out;
}

// ---- the store ------------------------------------------------------------------

TEST_F(UserFilesTest, TheRootIsTheDirectoryHandedIn) {
  const UserFiles files = store();
  EXPECT_EQ(files.root(), dir_);
  EXPECT_FALSE(std::filesystem::exists(dir_)) << "constructing a store makes nothing";
}

TEST_F(UserFilesTest, ANameIsARelativePathUnderTheRoot) {
  const UserFiles files = store();
  EXPECT_EQ(files.pathFor("config/input/x.snagpad"), dir_ / "config" / "input" / "x.snagpad");
  EXPECT_FALSE(std::filesystem::exists(dir_)) << "naming a file touches no disk";
}

TEST_F(UserFilesTest, ANameThatCouldLeaveTheStoreIsRefused) {
  const UserFiles files = store();
  EXPECT_THROW((void)files.pathFor("/etc/passwd"), std::invalid_argument);
  EXPECT_THROW((void)files.pathFor("../escaped.srm"), std::invalid_argument);
  EXPECT_THROW((void)files.pathFor("sram/../../escaped.srm"), std::invalid_argument);
  EXPECT_THROW((void)files.pathFor("C:\\windows\\system32\\x"), std::invalid_argument);
  EXPECT_THROW((void)files.pathFor(""), std::invalid_argument);
}

TEST_F(UserFilesTest, WriteCreatesTheDirectoriesOnTheWay) {
  const UserFiles files = store();
  files.write("sram/a.srm", bytesOf({1, 2, 3}));
  EXPECT_TRUE(std::filesystem::is_directory(dir_ / "sram"));
  EXPECT_EQ(files.read("sram/a.srm"), bytesOf({1, 2, 3}));
}

TEST_F(UserFilesTest, WriteIsAtomic) {
  const UserFiles files = store();
  files.write("sram/a.srm", bytesOf({1, 2, 3}));

  unsigned strays = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir_ / "sram")) {
    if (entry.path().filename() != "a.srm") ++strays;
  }
  EXPECT_EQ(strays, 0u) << "the temporary file the write went through is gone";

  files.write("sram/a.srm", bytesOf({9}));
  EXPECT_EQ(files.read("sram/a.srm"), bytesOf({9})) << "a second write replaces whole";
}

// What makes the write atomic is that the bytes go somewhere else first and arrive
// under the target's name in one step. Block that staging and a kept file must come
// through untouched — which a write straight into the target could not manage, since
// opening it is what destroys what it held.
TEST_F(UserFilesTest, AWriteThatCannotBeStagedLeavesTheKeptFileAlone) {
  const UserFiles files = store();
  files.write("sram/a.srm", bytesOf({1, 2, 3}));

  // Something standing exactly where the bytes would be staged.
  std::filesystem::create_directories(files.pathFor("sram/a.srm.writing"));
  std::filesystem::create_directories(files.pathFor("sram/a.srm.writing/held"));

  EXPECT_ANY_THROW(files.write("sram/a.srm", bytesOf({9, 9, 9, 9})));
  EXPECT_EQ(files.read("sram/a.srm"), bytesOf({1, 2, 3})) << "what was kept is still kept";
}

TEST_F(UserFilesTest, ReadAnswersTheBytesOrNothing) {
  const UserFiles files = store();
  files.write("sram/a.srm", bytesOf({0xDE, 0xAD, 0xBE, 0xEF}));
  EXPECT_EQ(files.read("sram/a.srm"), bytesOf({0xDE, 0xAD, 0xBE, 0xEF}));
  EXPECT_EQ(files.read("sram/absent.srm"), std::nullopt);
}

TEST_F(UserFilesTest, RemoveSaysWhetherAFileWent) {
  const UserFiles files = store();
  files.write("sram/a.srm", bytesOf({1}));
  EXPECT_TRUE(files.exists("sram/a.srm"));
  EXPECT_TRUE(files.remove("sram/a.srm"));
  EXPECT_FALSE(files.exists("sram/a.srm"));
  EXPECT_FALSE(files.remove("sram/a.srm"));
}

TEST_F(UserFilesTest, TheLayoutNamesItsDirectoriesWithoutMakingThem) {
  const UserFiles files = store();
  EXPECT_EQ(files.configInput(), dir_ / "config" / "input");
  EXPECT_EQ(files.sram(), dir_ / "sram");
  EXPECT_EQ(std::string(kSramDir), "sram") << "lower case, as the layout was given";
  EXPECT_FALSE(std::filesystem::exists(files.sram()));
  EXPECT_FALSE(std::filesystem::exists(files.configInput()));
}

TEST_F(UserFilesTest, TheSaveNameIsTheImageNameWithSrmForItsExtension) {
  EXPECT_EQ(UserFiles::saveNameFor("game.sfc"), "sram/game.srm");
  EXPECT_EQ(UserFiles::saveNameFor("/somewhere/else/game.smc"), "sram/game.srm")
      << "the image's own directory is not where its save goes";
  EXPECT_EQ(UserFiles::saveNameFor("a game. with dots.sfc"), "sram/a game. with dots.srm");
}

// The image's path cannot reach the save's: only its file name is taken, so a
// cartridge opened through a path of any shape keeps its save in the one place.
// This is why nothing a person can type as an image name escapes the store.
TEST_F(UserFilesTest, AnImagePathCannotReachOutOfTheSaveDirectory) {
  const UserFiles files = store();
  EXPECT_EQ(UserFiles::saveNameFor("../../../etc/game.sfc"), "sram/game.srm");
  EXPECT_EQ(UserFiles::saveNameFor("/absolute/game.sfc"), "sram/game.srm");
  EXPECT_NO_THROW((void)files.pathFor(UserFiles::saveNameFor("../../../etc/game.sfc")));
  EXPECT_EQ(files.pathFor(UserFiles::saveNameFor("../../../etc/game.sfc")),
            dir_ / "sram" / "game.srm");
}

// ---- which mapping the tool runs with -------------------------------------------

TEST_F(UserFilesTest, TheConfigOrderIsFlagThenUserFilesThenEmbedded) {
  const UserFiles files = store();

  EXPECT_EQ(chooseConfigPath("", &files), "") << "nothing kept: the built-in text";

  files.write(kDefaultPadConfigName, bytesOf({'#', '\n'}));
  EXPECT_EQ(chooseConfigPath("", &files), files.pathFor(kDefaultPadConfigName).string())
      << "a mapping of their own, picked up with no flag";

  EXPECT_EQ(chooseConfigPath("/given/on/the/line.snagpad", &files), "/given/on/the/line.snagpad")
      << "and the flag stands over both";
}

TEST_F(UserFilesTest, WithNowhereToKeepFilesTheFlagStillWorks) {
  EXPECT_EQ(chooseConfigPath("", nullptr), "");
  EXPECT_EQ(chooseConfigPath("/given.snagpad", nullptr), "/given.snagpad");
}

TEST_F(UserFilesTest, ADirectoryWhereTheMappingWouldBeIsNotAMapping) {
  const UserFiles files = store();
  std::filesystem::create_directories(files.pathFor(kDefaultPadConfigName));
  EXPECT_EQ(chooseConfigPath("", &files), "") << "only a file is a file";
}

// ---- a cartridge's save ---------------------------------------------------------

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kStaLong = 0x8Fu;
constexpr std::uint8_t kStp = 0xDBu;

constexpr std::uint64_t kFrameMaster = 357364u;
constexpr std::uint64_t kOneFrame = kFrameMaster + 20000u;
constexpr std::size_t kSaveBytes = 2048u;

std::vector<std::uint8_t> cartridge(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// Stores one byte into the save window at $700000, then stops.
std::vector<std::uint8_t> storesOnce(std::uint8_t value) {
  return cartridge({kLdaImm, value, kStaLong, 0x00u, 0x00u, 0x70u, kStp});
}

TEST_F(UserFilesTest, ASaveIsLoadedIntoTheWindowBeforeTheFirstInstruction) {
  const UserFiles files = store();
  std::vector<std::uint8_t> kept(kSaveBytes, 0x11u);
  kept[7] = 0x42u;
  files.write("sram/game.srm", kept);

  CartridgeSave save(files, "sram/game.srm");
  const std::vector<std::uint8_t> rom = storesOnce(0x5Au);
  Snes machine(SnesConfig{.rom = rom, .saveRamBytes = kSaveBytes});
  SnesState state = machine.state();
  std::size_t found = 0;
  std::size_t declared = 0;
  EXPECT_EQ(save.loadInto(state.sram, found, declared), CartridgeSave::Load::Loaded);
  machine.restore(state);

  EXPECT_EQ(machine.state().sram[7], 0x42u) << "before anything ran";
  machine.run(kOneFrame);
  EXPECT_EQ(machine.state().sram[7], 0x42u) << "and the cartridge left the rest of it alone";
}

TEST_F(UserFilesTest, ASaveOfTheWrongSizeIsLeftAloneAndReported) {
  const UserFiles files = store();
  files.write("sram/game.srm", std::vector<std::uint8_t>(16u, 0x33u));

  CartridgeSave save(files, "sram/game.srm");
  std::vector<std::uint8_t> window(kSaveBytes, 0x00u);
  std::size_t found = 0;
  std::size_t declared = 0;
  EXPECT_EQ(save.loadInto(window, found, declared), CartridgeSave::Load::WrongSize);
  EXPECT_EQ(found, 16u);
  EXPECT_EQ(declared, kSaveBytes);
  EXPECT_EQ(window, std::vector<std::uint8_t>(kSaveBytes, 0x00u)) << "the window is untouched";
  EXPECT_EQ(files.read("sram/game.srm")->size(), 16u) << "and so is the file";
}

TEST_F(UserFilesTest, NoFileIsNotAnError) {
  const UserFiles files = store();
  CartridgeSave save(files, "sram/game.srm");
  std::vector<std::uint8_t> window(kSaveBytes, 0x00u);
  std::size_t found = 0;
  std::size_t declared = 0;
  EXPECT_EQ(save.loadInto(window, found, declared), CartridgeSave::Load::NoFile);
  EXPECT_FALSE(std::filesystem::exists(files.sram())) << "and nothing was made looking";
}

TEST_F(UserFilesTest, TheFileIsWrittenWhenTheMachineSaysSo) {
  const UserFiles files = store();
  CartridgeSave save(files, "sram/game.srm");

  const std::vector<std::uint8_t> rom = storesOnce(0x5Au);
  Snes machine(SnesConfig{.rom = rom, .saveRamBytes = kSaveBytes});
  machine.setSaveObserver(&save);
  machine.run(kOneFrame);

  const std::optional<std::vector<std::uint8_t>> written = files.read("sram/game.srm");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->size(), kSaveBytes);
  EXPECT_EQ((*written)[0], 0x5Au);
  EXPECT_EQ(*written, machine.state().sram);
}

TEST_F(UserFilesTest, ACartridgeThatKeepsNothingLeavesNoSaveDirectory) {
  const UserFiles files = store();
  CartridgeSave save(files, "sram/game.srm");

  const std::vector<std::uint8_t> rom = storesOnce(0x5Au);
  Snes machine(SnesConfig{.rom = rom, .saveRamBytes = 0u});  // the header declares no battery
  machine.setSaveObserver(&save);
  machine.run(kOneFrame);

  EXPECT_FALSE(save.wrote());
  EXPECT_FALSE(std::filesystem::exists(files.sram()));
}

TEST_F(UserFilesTest, TheSameBytesReportedAgainAreNotWrittenAgain) {
  const UserFiles files = store();
  CartridgeSave save(files, "sram/game.srm");

  const std::vector<std::uint8_t> window(kSaveBytes, 0x77u);
  save.changed(window);
  const std::filesystem::file_time_type first =
      std::filesystem::last_write_time(files.pathFor("sram/game.srm"));

  save.changed(window);
  EXPECT_EQ(std::filesystem::last_write_time(files.pathFor("sram/game.srm")), first)
      << "the file would have said the same thing";

  std::vector<std::uint8_t> moved = window;
  moved[3] = 0x01u;
  save.changed(moved);
  EXPECT_EQ(files.read("sram/game.srm"), moved) << "and a real change does reach it";
}

// A save is written from inside a running machine, so a disk that refuses must not
// come out as an exception through the machine's own step — the run goes on and the
// person is told.
TEST_F(UserFilesTest, ADiskThatRefusesIsReportedRatherThanThrown) {
  const UserFiles files = store();
  // Something already standing where the file must go, which no rename can replace.
  std::filesystem::create_directories(files.pathFor("sram/game.srm"));
  std::filesystem::create_directories(files.pathFor("sram/game.srm/held"));

  CartridgeSave save(files, "sram/game.srm");
  EXPECT_NO_THROW(save.changed(std::vector<std::uint8_t>(kSaveBytes, 0x77u)));
  EXPECT_FALSE(save.wrote());
  EXPECT_FALSE(save.error().empty()) << "and it says what the disk said";
}

TEST_F(UserFilesTest, ASaveJustLoadedIsNotWrittenStraightBack) {
  const UserFiles files = store();
  const std::vector<std::uint8_t> kept(kSaveBytes, 0x11u);
  files.write("sram/game.srm", kept);
  const std::filesystem::file_time_type first =
      std::filesystem::last_write_time(files.pathFor("sram/game.srm"));

  CartridgeSave save(files, "sram/game.srm");
  std::vector<std::uint8_t> window(kSaveBytes, 0x00u);
  std::size_t found = 0;
  std::size_t declared = 0;
  ASSERT_EQ(save.loadInto(window, found, declared), CartridgeSave::Load::Loaded);

  // Putting a save into the window is a restore, and a restore is reported — so
  // without loading counting as knowing the file, every run would rewrite the file
  // it had just read.
  save.changed(window);
  EXPECT_FALSE(save.wrote());
  EXPECT_EQ(std::filesystem::last_write_time(files.pathFor("sram/game.srm")), first);
}

}  // namespace
}  // namespace snaggletooth::player
