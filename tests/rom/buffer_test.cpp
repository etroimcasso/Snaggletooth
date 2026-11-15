// The buffer a run on another thread is fed through: every record kind written
// and read back as it was, and the stream that holds them — blocks handed over
// in order, held in files once memory holds its share, taken back in order, a
// writer that never waits, a bound past which it takes nothing, and a directory
// left as it was found.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "rom/rom_buffer.h"

namespace snaggletooth::disasm {
namespace {

constexpr std::uint64_t kMB = 1'000'000;

// A directory of this case's own, empty at the start of it.
class RomBuffer : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* const which = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("snaggletooth-buffer-" + std::string(which == nullptr ? "case" : which->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  [[nodiscard]] std::size_t files() const {
    std::size_t count = 0;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ignored)) {
      if (entry.path().extension() == ".snagbuf") ++count;
    }
    return count;
  }

  std::filesystem::path dir_;
};

// Writes `count` Internal records numbered from 0; answers how many the stream took.
std::uint32_t writeNumbered(Stream& stream, std::uint32_t count) {
  for (std::uint32_t i = 0; i < count; ++i) {
    const InternalRecord record{.address = i, .has = false, .kind = CycleKind::DataRead};
    if (!putRecord(stream, RecordKind::Internal, record)) return i;
  }
  return count;
}

// What reading every block to the end found: how many records, and how many
// were not the Internal record numbered next.
struct Numbered {
  std::uint32_t records = 0;
  std::uint32_t outOfOrder = 0;
};
Numbered readNumbered(Stream& stream) {
  Numbered read;
  while (const std::optional<std::span<const std::uint8_t>> block = stream.take()) {
    for (RecordReader r{*block}; r.more();) {
      const RecordKind kind = r.kind();
      if (kind != RecordKind::Internal) {
        ++read.outOfOrder;
        r.skip(kind);
      } else if (r.payload<InternalRecord>().address != read.records) {
        ++read.outOfOrder;
      }
      ++read.records;
    }
    stream.returned();
  }
  return read;
}

constexpr std::size_t kRecord = 1u + sizeof(InternalRecord);

}  // namespace

TEST_F(RomBuffer, EveryRecordKindReadsBackAsItWasWritten) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  const BusAccess access{.address = 0x7E1234u,
                         .value = 0x5Au,
                         .write = true,
                         .kind = CycleKind::DataWrite,
                         .source = AccessSource::Cpu,
                         .channel = 0,
                         .table = false,
                         .pastTableEnd = false,
                         .landed = std::uint16_t{0x3001u}};
  const EngineAccessRecord engine{.access = BusAccess{.address = 0x002118u,
                                                      .value = 0x11u,
                                                      .write = true,
                                                      .kind = CycleKind::DataWrite,
                                                      .source = AccessSource::Hdma,
                                                      .channel = 5,
                                                      .table = true,
                                                      .pastTableEnd = true,
                                                      .landed = std::nullopt},
                                  .dmap = 0x41u,
                                  .bbad = 0x18u};
  const InternalRecord internal{.address = 0x00ABCDu, .has = true, .kind = CycleKind::OperandFetch};
  Cpu65816State cpu;
  cpu.pc = 0x8123u;
  cpu.a = 0xBEEFu;
  cpu.e = true;
  const BoundaryRecord boundary{.after = cpu, .masterCycles = 42u, .vpos = 225u};
  FrameStartRecord frame{.inidisp = 0x0Fu,
                         .objsel = 1,
                         .bgmode = 2,
                         .bg1sc = 3,
                         .bg2sc = 4,
                         .bg3sc = 5,
                         .bg4sc = 6,
                         .bg12nba = 7,
                         .bg34nba = 8,
                         .palette = {}};
  for (std::size_t i = 0; i < frame.palette.size(); ++i) frame.palette[i] = static_cast<std::uint8_t>(i * 7u);
  const ResetRecord reset{.after = cpu, .vpos = 0};
  const SoundAccessRecord sound{.address = 0x00F3u, .value = 0x01u, .write = true};
  Spc700State before;
  before.pc = 0xFFC0u;
  Spc700State after = before;
  after.pc = 0xFFC2u;
  detail::ApuWindow window;
  window.pc[0] = before.pc;
  window.pc[1] = after.pc;
  window.bytes[0] = {0xCDu, 0xEFu, 0xBDu};
  window.bytes[1] = {0xE8u, 0x00u, 0xC6u};
  const SoundStepRecord step{.before = before, .after = after, .cycles = 4u, .window = window};
  SoundResetRecord soundReset{.after = before, .dspAddr = 0x4Cu, .registers = {}, .window = window};
  soundReset.registers[0x5Du] = 0x02u;
  const KeyframeHeader header{.master = 123'456u, .frame = 9u, .bytes = 5u};

  ASSERT_TRUE(putRecord(stream, RecordKind::Access, access));
  ASSERT_TRUE(putRecord(stream, RecordKind::EngineAccess, engine));
  ASSERT_TRUE(putRecord(stream, RecordKind::Internal, internal));
  ASSERT_TRUE(putRecord(stream, RecordKind::FrameStart, frame));
  ASSERT_TRUE(putRecord(stream, RecordKind::Boundary, boundary));
  std::uint8_t* const state = putRecord(stream, RecordKind::Keyframe, header, header.bytes);
  ASSERT_NE(state, nullptr);
  std::copy_n(std::vector<std::uint8_t>{1, 2, 3, 4, 5}.begin(), 5, state);
  ASSERT_TRUE(putRecord(stream, RecordKind::Reset, reset));
  std::uint8_t* const ram = putBytes(stream, RecordKind::KeyOnMemory, 65536u);
  ASSERT_NE(ram, nullptr);
  for (std::size_t i = 0; i < 65536u; ++i) ram[i] = static_cast<std::uint8_t>(i ^ (i >> 8));
  ASSERT_TRUE(putRecord(stream, RecordKind::SoundAccess, sound));
  ASSERT_TRUE(putRecord(stream, RecordKind::SoundStep, step));
  ASSERT_TRUE(putRecord(stream, RecordKind::SoundReset, soundReset));
  ASSERT_TRUE(putRecord(stream, RecordKind::End));
  ASSERT_TRUE(putRecord(stream, RecordKind::SoundEnd));
  stream.end();

  const std::optional<std::span<const std::uint8_t>> block = stream.take();
  ASSERT_TRUE(block.has_value());
  RecordReader r{*block};
  ASSERT_EQ(r.kind(), RecordKind::Access);
  const BusAccess a = r.payload<BusAccess>();
  EXPECT_EQ(a.address, access.address);
  EXPECT_EQ(a.value, access.value);
  EXPECT_EQ(a.write, access.write);
  EXPECT_EQ(a.kind, access.kind);
  EXPECT_EQ(a.source, access.source);
  EXPECT_EQ(a.landed, access.landed);
  ASSERT_EQ(r.kind(), RecordKind::EngineAccess);
  const EngineAccessRecord e = r.payload<EngineAccessRecord>();
  EXPECT_EQ(e.access.address, engine.access.address);
  EXPECT_EQ(e.access.source, AccessSource::Hdma);
  EXPECT_EQ(e.access.channel, 5u);
  EXPECT_TRUE(e.access.table);
  EXPECT_TRUE(e.access.pastTableEnd);
  EXPECT_FALSE(e.access.landed.has_value());
  EXPECT_EQ(e.dmap, 0x41u);
  EXPECT_EQ(e.bbad, 0x18u);
  ASSERT_EQ(r.kind(), RecordKind::Internal);
  const InternalRecord i = r.payload<InternalRecord>();
  EXPECT_EQ(i.address, internal.address);
  EXPECT_TRUE(i.has);
  EXPECT_EQ(i.kind, CycleKind::OperandFetch);
  ASSERT_EQ(r.kind(), RecordKind::FrameStart);
  const FrameStartRecord f = r.payload<FrameStartRecord>();
  EXPECT_EQ(f.inidisp, 0x0Fu);
  EXPECT_EQ(f.bg34nba, 8u);
  EXPECT_EQ(f.palette, frame.palette);
  ASSERT_EQ(r.kind(), RecordKind::Boundary);
  const BoundaryRecord b = r.payload<BoundaryRecord>();
  EXPECT_EQ(b.after.pc, 0x8123u);
  EXPECT_EQ(b.after.a, 0xBEEFu);
  EXPECT_TRUE(b.after.e);
  EXPECT_EQ(b.masterCycles, 42u);
  EXPECT_EQ(b.vpos, 225u);
  ASSERT_EQ(r.kind(), RecordKind::Keyframe);
  const KeyframeHeader h = r.payload<KeyframeHeader>();
  EXPECT_EQ(h.master, header.master);
  EXPECT_EQ(h.frame, 9u);
  ASSERT_EQ(h.bytes, 5u);
  const std::span<const std::uint8_t> keyframe = r.bytes(h.bytes);
  EXPECT_EQ(std::vector<std::uint8_t>(keyframe.begin(), keyframe.end()), (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));
  ASSERT_EQ(r.kind(), RecordKind::Reset);
  EXPECT_EQ(r.payload<ResetRecord>().after.a, 0xBEEFu);
  ASSERT_EQ(r.kind(), RecordKind::KeyOnMemory);
  const std::span<const std::uint8_t> memory = r.bytes(65536u);
  EXPECT_EQ(memory[0x1234u], static_cast<std::uint8_t>(0x1234u ^ 0x12u));
  EXPECT_EQ(memory[0xFFFFu], static_cast<std::uint8_t>(0xFFFFu ^ 0xFFu));
  ASSERT_EQ(r.kind(), RecordKind::SoundAccess);
  const SoundAccessRecord s = r.payload<SoundAccessRecord>();
  EXPECT_EQ(s.address, 0x00F3u);
  EXPECT_EQ(s.value, 0x01u);
  EXPECT_TRUE(s.write);
  ASSERT_EQ(r.kind(), RecordKind::SoundStep);
  const SoundStepRecord st = r.payload<SoundStepRecord>();
  EXPECT_EQ(st.before.pc, 0xFFC0u);
  EXPECT_EQ(st.after.pc, 0xFFC2u);
  EXPECT_EQ(st.cycles, 4u);
  EXPECT_EQ(st.window.pc[1], 0xFFC2u);
  EXPECT_EQ(st.window.bytes[0], window.bytes[0]);
  EXPECT_EQ(st.window.bytes[1], window.bytes[1]);
  ASSERT_EQ(r.kind(), RecordKind::SoundReset);
  const SoundResetRecord sr = r.payload<SoundResetRecord>();
  EXPECT_EQ(sr.dspAddr, 0x4Cu);
  EXPECT_EQ(sr.registers[0x5Du], 0x02u);
  EXPECT_EQ(sr.window.bytes[0], window.bytes[0]);
  EXPECT_EQ(r.kind(), RecordKind::End);
  EXPECT_EQ(r.kind(), RecordKind::SoundEnd);
  EXPECT_FALSE(r.more()) << "nothing after the last record";
  stream.returned();
  EXPECT_FALSE(stream.take().has_value());
}

TEST_F(RomBuffer, SkipStepsOverEveryKind) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  const KeyframeHeader header{.master = 1u, .frame = 2u, .bytes = 300u};
  ASSERT_NE(putRecord(stream, RecordKind::Keyframe, header, header.bytes), nullptr);
  ASSERT_NE(putBytes(stream, RecordKind::KeyOnMemory, 65536u), nullptr);
  ASSERT_TRUE(putRecord(stream, RecordKind::SoundStep, SoundStepRecord{}));
  ASSERT_TRUE(putRecord(stream, RecordKind::End));
  ASSERT_TRUE(putRecord(stream, RecordKind::Internal, InternalRecord{.address = 77u, .has = false, .kind = {}}));
  stream.end();
  const std::optional<std::span<const std::uint8_t>> block = stream.take();
  ASSERT_TRUE(block.has_value());
  RecordReader r{*block};
  for (int skipped = 0; skipped < 4; ++skipped) r.skip(r.kind());
  ASSERT_EQ(r.kind(), RecordKind::Internal);
  EXPECT_EQ(r.payload<InternalRecord>().address, 77u);
  EXPECT_FALSE(r.more());
  stream.returned();
}

TEST_F(RomBuffer, TenMegabytesGoThroughFilesWhenRamHoldsThree) {
  const std::uint32_t count = static_cast<std::uint32_t>((10u * kMB) / kRecord);
  {
    Stream stream{dir_, 3u * kMB, 64u * kMB};
    ASSERT_EQ(writeNumbered(stream, count), count);
    stream.end();
    // The stream's thread moves blocks to files while the reader has not begun,
    // until the RAM its blocks take is within the limit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (stream.memoryUsed() > 3u * kMB && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_LE(stream.memoryUsed(), 3u * kMB) << "every block the stream holds counted, within the limit";
    EXPECT_GE(stream.diskUsed(), 6u * kMB) << "what RAM does not hold goes to files";
    EXPECT_EQ(stream.diskWritten(), stream.diskUsed());
    EXPECT_GT(files(), 0u);
    EXPECT_EQ(stream.held(), static_cast<std::uint64_t>(count) * kRecord);
    const Numbered read = readNumbered(stream);
    EXPECT_EQ(read.records, count);
    EXPECT_EQ(read.outOfOrder, 0u) << "every record in order";
    EXPECT_GE(stream.diskWritten(), 6u * kMB);
    EXPECT_LT(stream.diskWritten(), 10u * kMB) << "the block the reader takes first stays in RAM";
    EXPECT_EQ(stream.diskUsed(), 0u);
    EXPECT_EQ(stream.held(), 0u);
    EXPECT_EQ(files(), 0u) << "each file is removed as it is read back";
    // The blocks returned are kept for reuse only while the limit has room.
    const auto settled = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (stream.memoryUsed() > 3u * kMB && std::chrono::steady_clock::now() < settled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_LE(stream.memoryUsed(), 3u * kMB) << "the free blocks kept are within the RAM limit too";
  }
  EXPECT_FALSE(std::filesystem::exists(dir_)) << "the directory goes with the stream when it is empty";
}

TEST_F(RomBuffer, TheFilesStayWithinTheirLimitAndTheRestStaysInRam) {
  // No RAM of its own and 3 MB of files: past the files' limit, what the
  // stream holds stays in RAM until the stream is full at the two together.
  Stream stream{dir_, 0u, 3u * kMB};
  const std::uint32_t taken = writeNumbered(stream, static_cast<std::uint32_t>((6u * kMB) / kRecord));
  EXPECT_TRUE(stream.full());
  EXPECT_LE(static_cast<std::uint64_t>(taken) * kRecord, 3u * kMB) << "full at the two limits together";
  EXPECT_GT(static_cast<std::uint64_t>(taken) * kRecord, 3u * kMB - 2u * Stream::kBlockBytes)
      << "full up to a block before";
  stream.end();
  // The stream's thread moves what it can to files, at the lowest scheduling
  // class with its disk access throttled: the files are read a millisecond
  // apart until they hold two blocks, the most the limit has room for.
  std::uint64_t most = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline && most < 2u * Stream::kBlockBytes - 2u * kRecord) {
    most = std::max(most, stream.diskUsed());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_GT(most, 0u) << "blocks went to files";
  EXPECT_LE(most, 3u * kMB);
  const Numbered read = readNumbered(stream);
  EXPECT_EQ(read.records, taken);
  EXPECT_EQ(read.outOfOrder, 0u);
}

TEST_F(RomBuffer, WhatTheFilesHaveNoRoomForStaysInRam) {
  // 2 MB of RAM and 1.5 MB of files: of the three blocks held, RAM has room for
  // one; one goes to a file, and the third has no room there and stays.
  StreamBudget budget{2u * kMB, 1'500'000u};
  Stream stream{dir_, budget};
  const std::uint32_t taken = writeNumbered(stream, static_cast<std::uint32_t>((4u * kMB) / kRecord));
  EXPECT_TRUE(stream.full());
  stream.end();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (stream.diskUsed() == 0u && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::uint64_t most = 0;
  for (int sample = 0; sample < 300; ++sample) {
    most = std::max(most, stream.diskUsed());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(budget.diskUsed(), stream.diskUsed()) << "the budget counts the stream's files";
  EXPECT_GT(most, 0u) << "a block went to a file";
  EXPECT_LE(most, 1'500'000u) << "and no more than the files' limit holds";
  const Numbered read = readNumbered(stream);
  EXPECT_EQ(read.records, taken);
  EXPECT_EQ(read.outOfOrder, 0u);
}

TEST_F(RomBuffer, TwoStreamsOnOneBudgetShareItsLimits) {
  StreamBudget budget{1u * kMB, 2u * kMB};
  Stream first{dir_ / "first", budget};
  Stream second{dir_ / "second", budget};
  const std::uint32_t intoFirst = writeNumbered(first, static_cast<std::uint32_t>((2u * kMB) / kRecord));
  EXPECT_EQ(intoFirst, static_cast<std::uint32_t>((2u * kMB) / kRecord));
  const std::uint32_t intoSecond = writeNumbered(second, static_cast<std::uint32_t>((2u * kMB) / kRecord));
  EXPECT_TRUE(second.full()) << "the first's handed block and its open one, charged whole, leave no block's room";
  EXPECT_LE(static_cast<std::uint64_t>(intoFirst + intoSecond) * kRecord, 3u * kMB);
  EXPECT_GT(static_cast<std::uint64_t>(intoFirst + intoSecond) * kRecord + 2u * Stream::kBlockBytes, 3u * kMB)
      << "full up to a block before on each";
  first.end();
  second.end();
  EXPECT_EQ(budget.held(), first.held() + second.held());
  const Numbered a = readNumbered(first);
  const Numbered b = readNumbered(second);
  EXPECT_EQ(a.records, intoFirst);
  EXPECT_EQ(b.records, intoSecond);
  EXPECT_EQ(budget.held(), 0u);
}

TEST_F(RomBuffer, TheRamFigureCountsEveryBlockWhole) {
  Stream stream{dir_, 64u * kMB, 64u * kMB};
  EXPECT_EQ(stream.memoryUsed(), 0u);
  ASSERT_EQ(writeNumbered(stream, 1u), 1u);
  EXPECT_GE(stream.memoryUsed(), Stream::kBlockBytes) << "one record takes a whole block";
  EXPECT_LT(stream.memoryUsed(), 2u * Stream::kBlockBytes);
  stream.end();
  const Numbered read = readNumbered(stream);
  EXPECT_EQ(read.records, 1u);
  EXPECT_GE(stream.memoryUsed(), Stream::kBlockBytes) << "a returned block is kept to be written again";
}

TEST_F(RomBuffer, AReaderWaitsForTheWriter) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  Numbered read;
  std::atomic<bool> done{false};
  std::thread reader([&] {
    read = readNumbered(stream);
    done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(done) << "the reader waits while nothing is written";
  // EXPECT, not ASSERT, while the reader runs: it is joined whatever the writer saw.
  EXPECT_EQ(writeNumbered(stream, 1000u), 1000u);
  stream.end();
  reader.join();
  EXPECT_EQ(read.records, 1000u);
  EXPECT_EQ(read.outOfOrder, 0u);
}

TEST_F(RomBuffer, PollAnswersWithoutWaiting) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  bool ended = true;
  EXPECT_FALSE(stream.poll(ended).has_value());
  EXPECT_FALSE(ended) << "the writer goes on";
  ASSERT_EQ(writeNumbered(stream, 10u), 10u);
  stream.flush();
  EXPECT_EQ(stream.handed(), 1u);
  std::optional<std::span<const std::uint8_t>> block;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!(block = stream.poll(ended)) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  ASSERT_TRUE(block.has_value()) << "a flushed block reaches the reader";
  EXPECT_EQ(block->size(), 10u * kRecord);
  stream.returned();
  stream.end();
  while (!stream.poll(ended) && !ended && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  EXPECT_TRUE(ended) << "the writer ended and every block was taken";
}

TEST_F(RomBuffer, PutAnswersNullWhenFullAndEverythingBeforeIsRead) {
  // 1 MB of RAM and 2 MB of files: full at 3 MB held.
  Stream stream{dir_, 1u * kMB, 2u * kMB};
  const std::uint32_t taken = writeNumbered(stream, static_cast<std::uint32_t>((5u * kMB) / kRecord));
  EXPECT_TRUE(stream.full());
  EXPECT_LE(static_cast<std::uint64_t>(taken) * kRecord, 3u * kMB);
  EXPECT_GT(static_cast<std::uint64_t>(taken) * kRecord, 3u * kMB - 2u * Stream::kBlockBytes)
      << "full up to a block before";
  EXPECT_EQ(stream.put(kRecord), nullptr) << "a full stream takes nothing more";
  stream.end();
  const Numbered read = readNumbered(stream);
  EXPECT_EQ(read.records, taken);
  EXPECT_EQ(read.outOfOrder, 0u);
  EXPECT_EQ(stream.put(kRecord), nullptr) << "and stays full";
}

TEST_F(RomBuffer, ARecordLargerThanABlockIsRefused) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  EXPECT_EQ(stream.put(Stream::kBlockBytes + 1u), nullptr);
  EXPECT_TRUE(stream.full());
}

TEST_F(RomBuffer, EndHandsThePartialBlockOver) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  const std::uint32_t count = static_cast<std::uint32_t>(Stream::kBlockBytes / kRecord) + 3u;
  ASSERT_EQ(writeNumbered(stream, count), count);
  EXPECT_EQ(stream.handed(), 1u) << "the first block filled and went";
  stream.end();
  EXPECT_EQ(stream.handed(), 2u) << "the partial one goes at the end";
  const Numbered read = readNumbered(stream);
  EXPECT_EQ(read.records, count);
  EXPECT_EQ(read.outOfOrder, 0u);
}

TEST_F(RomBuffer, BlocksReturnedAsTheyAreReadKeepTheStreamInMemory) {
  Stream stream{dir_, 4u * kMB, 64u * kMB};
  Numbered read;
  std::thread reader([&] { read = readNumbered(stream); });
  // Twenty blocks, never more than one handed ahead of the reader. The writes
  // are counted, not asserted, while the reader runs: it is joined whatever
  // the writer saw.
  const std::uint32_t perBlock = static_cast<std::uint32_t>(Stream::kBlockBytes / kRecord);
  std::uint32_t next = 0;
  std::uint32_t refused = 0;
  for (int block = 0; block < 20; ++block) {
    for (std::uint32_t i = 0; i < perBlock; ++i, ++next) {
      if (!putRecord(stream, RecordKind::Internal, InternalRecord{.address = next, .has = false, .kind = {}})) ++refused;
    }
    stream.flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (stream.held() > 0u && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  stream.end();
  reader.join();
  EXPECT_EQ(refused, 0u);
  EXPECT_EQ(read.records, next);
  EXPECT_EQ(read.outOfOrder, 0u);
  EXPECT_EQ(stream.diskWritten(), 0u) << "a reader that keeps up keeps every block in RAM";
}

TEST_F(RomBuffer, ALeftoverFileIsSweptAndOtherFilesKept) {
  std::filesystem::create_directories(dir_);
  {
    std::ofstream(dir_ / "7.snagbuf") << "left over";
    std::ofstream(dir_ / "keep.txt") << "the host's";
  }
  {
    Stream stream{dir_, 4u * kMB, 64u * kMB};
    EXPECT_FALSE(std::filesystem::exists(dir_ / "7.snagbuf"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "keep.txt"));
  }
  EXPECT_TRUE(std::filesystem::exists(dir_)) << "a directory that holds another file stays";
}

TEST_F(RomBuffer, ADirectoryThatCannotBeMadeIsRefused) {
  std::filesystem::create_directories(dir_);
  { std::ofstream(dir_ / "file") << "a file, not a directory"; }
  EXPECT_THROW(Stream(dir_ / "file" / "under", 4u * kMB, 64u * kMB), std::runtime_error);
}

}  // namespace snaggletooth::disasm
