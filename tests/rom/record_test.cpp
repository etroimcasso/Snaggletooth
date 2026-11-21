// A run's record: the text a tree keeps of what one run saw, read back to the
// same observation, refused where a line does not read, joined with what other
// runs saw, and kept as numbered files under a tree's `runs/`. Each case builds
// the observation it reads by hand, so every kind and every optional field is
// held to its text and to its join.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "rom/rom_record.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {
namespace {

using examples::liftingImage;

ir::OriginSet imageOrigin(std::size_t first, std::size_t last) {
  return ir::OriginSet{.image = {ir::OriginInterval{.first = first, .last = last}},
                       .registers = {},
                       .save = false,
                       .approximate = false};
}

// A range or a stream with its register's name and class, as a run reports them.
template <typename Range>
Range named(Range range) {
  if (const std::optional<Cpu65816Register> reg = cpu65816Register(range.registerAddress)) {
    range.registerName = reg->name;
    range.registerClass = reg->cls;
  }
  return range;
}

std::vector<std::uint8_t> palette(std::uint8_t seed) {
  std::vector<std::uint8_t> out(512);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(i ^ seed);
  return out;
}

MovedRange moved(Address site, std::uint32_t times) {
  return named(MovedRange{.site = site,
                          .channel = 0,
                          .toRegister = true,
                          .registerAddress = 0x002118,
                          .registerName = {},
                          .registerClass = std::nullopt,
                          .memory = 0x7E2000,
                          .step = MovedStep::Increment,
                          .bytes = 64,
                          .kind = MovedKind::Dma,
                          .times = times});
}

LandedRange landed(Address site, std::optional<std::size_t> paletteIndex, std::uint32_t times) {
  return LandedRange{.site = site,
                     .channel = 0,
                     .memory = 0x7E2000,
                     .bytes = 64,
                     .kind = MovedKind::Dma,
                     .landing = PortLanding{.memory = PortMemory::Vram,
                                            .lowest = 0x1000,
                                            .highest = 0x101F,
                                            .shown = true,
                                            .areas = kAreaTiles1 | kAreaTilemap2,
                                            .depths = kDepth2 | kDepth4,
                                            .palette = paletteIndex},
                     .times = times};
}

CarriedContent content(std::vector<std::uint8_t> bytes, std::uint32_t order) {
  return CarriedContent{.bytes = std::move(bytes),
                        .origin = {},
                        .cls = RegisterClass::Vram,
                        .kind = MovedKind::Dma,
                        .landing = std::nullopt,
                        .unit = 1,
                        .indirect = false,
                        .byteOrigins = {},
                        .order = order};
}

EnumeratedRun outcome(std::vector<std::uint32_t> values, Address extentMemory) {
  return EnumeratedRun{.routine = 0x0080C0,
                       .reg = InputRegister::X,
                       .bits = 8,
                       .values = std::move(values),
                       .enteredFrom = 0x008011,
                       .extents = {EnumeratedExtent{.memory = extentMemory,
                                                    .bytes = 2,
                                                    .values = {1, 2},
                                                    .origin = {},
                                                    .sources = {},
                                                    .classes = {RegisterClass::Vram},
                                                    .carried = {}}}};
}

// One of every kind, each optional field present in one entry and absent in
// another, every list in the order a run reports it.
RunRecord everyKind() {
  RunRecord r;
  RunObservation& o = r.observation;
  o.reached = {
      ReachedTarget{.target = 0x008100, .mode = Cpu65816Mode::reset(), .site = 0x008010, .call = true, .name = {}},
      ReachedTarget{
          .target = 0x008200, .mode = Cpu65816Mode::native(false, true), .site = 0x008020, .call = false, .name = {}},
  };
  o.ran = {Landing{.target = 0x008300, .mode = Cpu65816Mode::nativeUnknown(), .site = 0x008030, .name = {}}};
  MovedRange back = moved(0x008050, 1);
  back.channel = 1;
  back.toRegister = false;
  back.registerAddress = 0x0021FF;
  back.registerName = {};
  back.registerClass = std::nullopt;
  back = named(back);
  back.step = MovedStep::Decrement;
  back.kind = MovedKind::Table;
  o.moved = {moved(0x008040, 3), back};
  o.palettes = {palette(0x11), palette(0x22)};
  o.landed = {landed(0x008040, 1, 3),
              LandedRange{.site = 0x008060,
                          .channel = 2,
                          .memory = 0x7E3000,
                          .bytes = 32,
                          .kind = MovedKind::Dma,
                          .landing = PortLanding{.memory = PortMemory::Cgram,
                                                 .lowest = 0x00,
                                                 .highest = 0x0F,
                                                 .shown = true,
                                                 .areas = kAreaPalette,
                                                 .depths = 0,
                                                 .palette = std::nullopt},
                          .times = 1},
              LandedRange{.site = 0x008070,
                          .channel = 3,
                          .memory = 0x7E3100,
                          .bytes = 544,
                          .kind = MovedKind::Dma,
                          .landing = PortLanding{.memory = PortMemory::Oam,
                                                 .lowest = 0x000,
                                                 .highest = 0x21F,
                                                 .shown = false,
                                                 .areas = 0,
                                                 .depths = 0,
                                                 .palette = std::nullopt},
                          .times = 2}};
  o.walked = {WalkedRange{.site = 0x008080,
                          .channel = 3,
                          .memory = 0x7E4000,
                          .bytes = 12,
                          .kind = MovedKind::Table,
                          .unit = 2,
                          .indirect = false,
                          .times = 1},
              WalkedRange{.site = 0x008080,
                          .channel = 3,
                          .memory = 0x7E4100,
                          .bytes = 4,
                          .kind = MovedKind::Indirect,
                          .unit = 4,
                          .indirect = true,
                          .times = 2}};
  o.seen = {SeenState{.address = 0x008000, .d = {0x0000, 0x0100}, .dbr = {0x00, 0x7E}},
            SeenState{.address = 0x008001, .d = {}, .dbr = {}}};
  CarriedContent landedContent = content({1, 2, 3, 4}, 0);
  landedContent.origin = imageOrigin(0x100, 0x103);
  landedContent.landing = PortLanding{.memory = PortMemory::Vram,
                                      .lowest = 0x1000,
                                      .highest = 0x1001,
                                      .shown = true,
                                      .areas = kAreaTiles1,
                                      .depths = kDepth4,
                                      .palette = 0};
  landedContent.byteOrigins = {ContentOrigin{.bytes = 3, .origin = imageOrigin(0x100, 0x102)},
                               ContentOrigin{.bytes = 1, .origin = {}}};
  CarriedContent bare = content({}, 5);
  bare.cls = RegisterClass::Oam;
  bare.kind = MovedKind::Stream;
  bare.unit = 2;
  bare.indirect = true;
  o.staged = {StagedRange{.memory = 0x7E2000,
                          .bytes = 64,
                          .origin = ir::OriginSet{.image = {ir::OriginInterval{.first = 0x100, .last = 0x13B},
                                                            ir::OriginInterval{.first = 0x200, .last = 0x203}},
                                                  .registers = {0x2137, 0x4214},
                                                  .save = true,
                                                  .approximate = true},
                          .writers = {StagedWriter{.writer = ir::Writer{.site = 0x008090, .engine = false},
                                                   .unwritten = false,
                                                   .bytes = 60,
                                                   .origin = imageOrigin(0x100, 0x13B),
                                                   .sources = {ir::OriginInterval{.first = 0x100, .last = 0x13B}}},
                                      StagedWriter{.writer = ir::Writer{.site = 0x008040, .engine = true},
                                                   .unwritten = false,
                                                   .bytes = 4,
                                                   .origin = imageOrigin(0x200, 0x203),
                                                   .sources = {ir::OriginInterval{.first = 0x200, .last = 0x203}}},
                                      StagedWriter{.writer = ir::Writer{.site = 0, .engine = false},
                                                   .unwritten = true,
                                                   .bytes = 4,
                                                   .origin = {},
                                                   .sources = {}}},
                          .contents = {landedContent, bare}},
              StagedRange{.memory = 0x7E3000, .bytes = 32, .origin = {}, .writers = {}, .contents = {}}};
  o.streamed = {named(StreamedRange{.site = 0x0080A0,
                                    .registerAddress = 0x002118,
                                    .registerName = {},
                                    .registerClass = std::nullopt,
                                    .romOffset = 0x200,
                                    .bytes = 16,
                                    .times = 2,
                                    .source = ir::OriginInterval{.first = 0x1F0, .last = 0x20F},
                                    .memory = std::nullopt,
                                    .landing = PortLanding{.memory = PortMemory::Vram,
                                                           .lowest = 0x2000,
                                                           .highest = 0x2007,
                                                           .shown = true,
                                                           .areas = kAreaTilemap1,
                                                           .depths = 0,
                                                           .palette = 1}}),
                named(StreamedRange{.site = 0x0080B0,
                                    .registerAddress = 0x002140,
                                    .registerName = {},
                                    .registerClass = std::nullopt,
                                    .romOffset = 0x300,
                                    .bytes = 8,
                                    .times = 1,
                                    .source = ir::OriginInterval{.first = 0x300, .last = 0x307},
                                    .memory = 0x7E5000,
                                    .landing = std::nullopt})};
  o.samples = {KeyedSample{.start = 0x3000, .loop = 0x3009, .bytes = {0x03, 1, 2, 3, 4, 5, 6, 7, 8}, .times = 2}};
  o.enumerated = {EnumeratedRun{
      .routine = 0x0080C0,
      .reg = InputRegister::X,
      .bits = 8,
      .values = {0, 2, 4, 6, 9},
      .enteredFrom = 0x008011,
      .extents = {EnumeratedExtent{.memory = 0x7E6000,
                                   .bytes = 4,
                                   .values = {9, 8, 7, 6},
                                   .origin = imageOrigin(0x400, 0x403),
                                   .sources = {ir::OriginInterval{.first = 0x400, .last = 0x403}},
                                   .classes = {RegisterClass::Vram, RegisterClass::Oam},
                                   .carried = {OwnCarry{.cls = RegisterClass::Vram,
                                                        .kind = MovedKind::Dma,
                                                        .registerAddress = 0x002118,
                                                        .site = 0x0080C8}}},
                  EnumeratedExtent{.memory = 0x7E6100,
                                   .bytes = 2,
                                   .values = {1, 1},
                                   .origin = {},
                                   .sources = {},
                                   .classes = {},
                                   .carried = {}}}}};
  o.instructions = 1000;
  o.interrupts = 3;
  o.nodes = 40;
  o.divergences = 1;
  o.spc700Instructions = 500;
  o.spc700Nodes = 20;
  o.spc700Divergences = 2;
  o.originSets = 7;
  o.originCap = 64;
  o.variantsPlanned = 5;
  o.variantsRun = 4;
  o.variantsReturned = 3;
  o.variantsConfined = 2;
  o.enumerationCycles = 12345;
  r.notes = {"run: a note; with a semicolon", "a note with \"quotes\" in it"};
  r.enumerationNotes = {"enumerate sub_0080C0: 2 values wrote $7E:0000-$7E:0001, extents the run never carried; not lifted"};
  return r;
}

// A file's bytes, and a file written with `bytes`.
std::string fileText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void putFile(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}

// A zstd file of `text` written as another compressor might: one frame of raw
// blocks, no compression at all, no checksum.
std::string rawZstd(const std::string& text) {
  std::string out("\x28\xB5\x2F\xFD", 4);
  // The frame header: one segment, its content size in four bytes.
  out += static_cast<char>(0xA0);
  const std::uint32_t size = static_cast<std::uint32_t>(text.size());
  for (int i = 0; i < 4; ++i) out += static_cast<char>((size >> (8 * i)) & 0xFFu);
  std::size_t at = 0;
  do {
    const std::size_t take = std::min<std::size_t>(std::size_t{128} * 1024u, text.size() - at);
    const bool last = at + take == text.size();
    const std::uint32_t header = (static_cast<std::uint32_t>(take) << 3) | (last ? 1u : 0u);  // a raw block
    for (int i = 0; i < 3; ++i) out += static_cast<char>((header >> (8 * i)) & 0xFFu);
    out.append(text, at, take);
    at += take;
  } while (at < text.size());
  return out;
}

// A record's first four lines, which hold no finding, with `line` as its fifth.
std::string withLine(const std::vector<std::uint8_t>& rom, const std::string& line) {
  return renderRecord(RunRecord{}, rom) + line + "\n";
}

TEST(RomRecord, ARecordOfEveryKindReadsBackEqualAndRendersAgainToItsOwnText) {
  const std::vector<std::uint8_t> rom = liftingImage();
  const RunRecord record = everyKind();
  const std::string text = renderRecord(record, rom);
  std::string error;
  const std::optional<RunRecord> read = parseRecord(text, error);
  ASSERT_TRUE(read.has_value()) << error;
  EXPECT_TRUE(*read == record);
  EXPECT_EQ(renderRecord(*read, rom), text);

  // The fields a person reads: the head, a register's name taken from its
  // address, and a stream to the audio ports with no landing.
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  ASSERT_TRUE(header.has_value());
  char checksum[8];
  std::snprintf(checksum, sizeof checksum, "$%04X", header->checksum);
  EXPECT_EQ(text.rfind("snagrun 1\nimage " + std::to_string(rom.size()) + " checksum " + checksum + "\n", 0), 0u);
  EXPECT_EQ(read->observation.moved[0].registerName, "VMDATAL");
  EXPECT_EQ(read->observation.streamed[1].registerName, "APUIO0");
  EXPECT_NE(text.find("\nnote run: a note; with a semicolon\n"), std::string::npos);
  EXPECT_NE(text.find("\nenumeration-note enumerate sub_0080C0: 2 values wrote $7E:0000-$7E:0001, extents the run "
                      "never carried; not lifted\n"),
            std::string::npos);
  EXPECT_EQ(read->enumerationNotes.size(), 1u);
  EXPECT_EQ(read->notes.size(), 2u);

  // The image known by its size and checksum, as a disassembly holds it, gives
  // the same text; an image with no header names no checksum.
  EXPECT_EQ(renderRecord(record, rom.size(), header->checksum), text);
  EXPECT_EQ(renderRecord(RunRecord{}, 4096u, std::nullopt).rfind("snagrun 1\nimage 4096 checksum none\n", 0), 0u);
  EXPECT_NE(text.find("\nenumerated $00:80C0 X bits 8 values $00-$06/2,$09 entered-from $00:8011\n"),
            std::string::npos);
  EXPECT_NE(text.find(" at vram $1000-$101F shown areas tilemap2+tiles1 depths 2+4 palette 1\n"), std::string::npos);
  EXPECT_NE(text.find(" origin image $000100-$00013B,$000200-$000203 registers $2137,$4214 save approximate\n"),
            std::string::npos);
}

TEST(RomRecord, AReaderRefusesALineItCannotReadAndNamesIt) {
  const std::vector<std::uint8_t> rom = liftingImage();
  std::string error;
  const std::string valid = renderRecord(RunRecord{}, rom);
  ASSERT_TRUE(parseRecord(valid, error).has_value()) << error;

  EXPECT_FALSE(parseRecord("snagrun 2" + valid.substr(valid.find('\n')), error).has_value());
  EXPECT_EQ(error.rfind("line 1: ", 0), 0u) << error;

  EXPECT_FALSE(parseRecord(withLine(rom, "pallete 00"), error).has_value());
  EXPECT_EQ(error.rfind("line 5: ", 0), 0u) << error;
  EXPECT_NE(error.find("`pallete`"), std::string::npos) << error;

  EXPECT_FALSE(parseRecord(withLine(rom, "palette 0A1"), error).has_value());
  EXPECT_EQ(error.rfind("line 5: ", 0), 0u) << error;

  const std::string landing =
      "landed $00:8040 channel 0 memory $7E:2000 bytes 64 as dma times 1 at vram $1000-$101F shown areas tiles1 "
      "depths 4 palette ";
  ASSERT_TRUE(parseRecord(withLine(rom, landing + "none"), error).has_value()) << error;
  EXPECT_FALSE(parseRecord(withLine(rom, landing + "0"), error).has_value());
  EXPECT_EQ(error.rfind("line 5: palette 0 is not among the 0", 0), 0u) << error;

  // A word left over, an origin with nothing in it, and no counts line.
  EXPECT_FALSE(parseRecord(withLine(rom, landing + "none extra"), error).has_value());
  EXPECT_EQ(error.rfind("line 5: ", 0), 0u) << error;
  EXPECT_FALSE(parseRecord(withLine(rom, "staged $7E:0000 bytes 2 origin"), error).has_value());
  EXPECT_EQ(error.rfind("line 5: ", 0), 0u) << error;
  EXPECT_FALSE(parseRecord(valid.substr(0, valid.find("\ncounts")), error).has_value());
  EXPECT_EQ(error.rfind("line 2: ", 0), 0u) << error;

  // A writer belongs to the staged line it stands under, and to no other.
  const std::string staged = "staged $7E:0000 bytes 2 origin none\n";
  const std::string writer = "writer $00:8000 instruction written bytes 2 sources none origin none";
  const std::optional<RunRecord> under = parseRecord(withLine(rom, staged + writer), error);
  ASSERT_TRUE(under.has_value()) << error;
  EXPECT_EQ(under->observation.staged[0].writers.size(), 1u);
  EXPECT_FALSE(parseRecord(withLine(rom, staged + "seen $00:8000 D=$0000 DBR=$00\n" + writer), error).has_value());
  EXPECT_EQ(error.rfind("line 7: ", 0), 0u) << error;
}

TEST(RomRecord, AMismatchIsTheImageASizeOrAChecksumNamesAndNothingForItsOwn) {
  const std::vector<std::uint8_t> rom = liftingImage();
  const std::string text = renderRecord(everyKind(), rom);
  EXPECT_EQ(recordMismatch(text, rom), "");

  std::vector<std::uint8_t> longer = rom;
  longer.resize(rom.size() * 2u, 0xFF);
  EXPECT_EQ(recordMismatch(text, longer), "the record was written for an image of " + std::to_string(rom.size()) +
                                              " bytes; this one is " + std::to_string(longer.size()));

  std::vector<std::uint8_t> other = rom;
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  ASSERT_TRUE(header.has_value());
  const std::uint16_t checksum = static_cast<std::uint16_t>(header->checksum + 1u);
  const std::uint16_t complement = static_cast<std::uint16_t>(~checksum);
  other[header->offset + 0x1Cu] = static_cast<std::uint8_t>(complement);
  other[header->offset + 0x1Du] = static_cast<std::uint8_t>(complement >> 8);
  other[header->offset + 0x1Eu] = static_cast<std::uint8_t>(checksum);
  other[header->offset + 0x1Fu] = static_cast<std::uint8_t>(checksum >> 8);
  const std::string mismatch = recordMismatch(text, other);
  EXPECT_NE(mismatch.find("with checksum $"), std::string::npos) << mismatch;
  EXPECT_NE(mismatch.find("this one has checksum $"), std::string::npos) << mismatch;
}

TEST(RomRecord, AnObservationJoinedToAnEmptyOneIsThatObservation) {
  const RunObservation seen = everyKind().observation;
  RunObservation into;
  joinObservation(into, seen);
  EXPECT_TRUE(into == seen);
  EXPECT_EQ(into.staged[0].contents[1].order, 5u);
  EXPECT_EQ(into.landed[0].landing.palette, std::optional<std::size_t>(1));
}

TEST(RomRecord, EachKindJoinsByTheIdentityTheRunKeepsItUnder) {
  RunObservation into;
  RunObservation from;

  // A sighting both hold is once, and the list stands by site.
  const auto reached = [](Address site) {
    return ReachedTarget{.target = 0x009000, .mode = Cpu65816Mode::reset(), .site = site, .call = true, .name = {}};
  };
  into.reached = {reached(0x008001), reached(0x008003)};
  from.reached = {reached(0x008003), reached(0x008002)};
  const auto ran = [](Address site) {
    return Landing{.target = 0x009100, .mode = Cpu65816Mode::native(true, true), .site = site, .name = {}};
  };
  into.ran = {ran(0x008005)};
  from.ran = {ran(0x008004), ran(0x008005)};

  // A range both hold has its counts added.
  into.moved = {moved(0x008040, 3)};
  from.moved = {moved(0x008040, 2), moved(0x008030, 1)};

  // A landing of `from` names its palette's place in the joined list; a
  // landing both hold keeps `into`'s palette and adds the counts.
  into.palettes = {palette(0x10)};
  into.landed = {landed(0x008040, 0, 1)};
  from.palettes = {palette(0x20), palette(0x10)};
  from.landed = {landed(0x008050, 0, 1), landed(0x008040, 1, 2)};

  // A walk both hold has its counts added.
  const auto walked = [](std::uint32_t times) {
    return WalkedRange{.site = 0x008080,
                       .channel = 3,
                       .memory = 0x7E4000,
                       .bytes = 12,
                       .kind = MovedKind::Table,
                       .unit = 2,
                       .indirect = false,
                       .times = times};
  };
  into.walked = {walked(1)};
  from.walked = {walked(4)};

  // A stream both hold keeps `into`'s source and adds the counts.
  const auto stream = [](std::size_t sourceEnd, std::uint32_t times) {
    return named(StreamedRange{.site = 0x0080A0,
                               .registerAddress = 0x002118,
                               .registerName = {},
                               .registerClass = std::nullopt,
                               .romOffset = 0x200,
                               .bytes = 16,
                               .times = times,
                               .source = ir::OriginInterval{.first = 0x200, .last = sourceEnd},
                               .memory = std::nullopt,
                               .landing = std::nullopt});
  };
  into.streamed = {stream(0x20F, 2)};
  from.streamed = {stream(0x2FF, 3)};

  // A sample both hold has its counts added.
  const KeyedSample sample{.start = 0x3000, .loop = 0x3000, .bytes = {0x03, 0, 0, 0, 0, 0, 0, 0, 0}, .times = 1};
  into.samples = {sample};
  from.samples = {sample, sample};
  from.samples[1].start = 0x2000;

  // A site's values are united, ascending.
  into.seen = {SeenState{.address = 0x008000, .d = {0x0000, 0x0200}, .dbr = {0x7E}}};
  from.seen = {SeenState{.address = 0x008000, .d = {0x0000, 0x0100}, .dbr = {0x00}}};

  // An extent's origins are joined, a writer both hold has its bytes added and
  // its sources united, a content both hold is once and `from`'s others follow
  // `into`'s in their order of carries.
  CarriedContent same = content({1, 2}, 4);
  CarriedContent earlier = content({3, 4}, 2);
  into.staged = {StagedRange{.memory = 0x7E2000,
                             .bytes = 64,
                             .origin = imageOrigin(0x100, 0x10F),
                             .writers = {StagedWriter{.writer = ir::Writer{.site = 0x008090, .engine = false},
                                                      .unwritten = false,
                                                      .bytes = 10,
                                                      .origin = imageOrigin(0x100, 0x10F),
                                                      .sources = {ir::OriginInterval{.first = 0x100, .last = 0x10F}}}},
                             .contents = {same, earlier}}};
  CarriedContent sameAgain = content({1, 2}, 0);
  sameAgain.landing = PortLanding{};
  from.staged = {StagedRange{.memory = 0x7E2000,
                             .bytes = 64,
                             .origin = imageOrigin(0x110, 0x11F),
                             .writers = {StagedWriter{.writer = ir::Writer{.site = 0x008090, .engine = false},
                                                      .unwritten = false,
                                                      .bytes = 6,
                                                      .origin = imageOrigin(0x110, 0x11F),
                                                      .sources = {ir::OriginInterval{.first = 0x110, .last = 0x11F}}},
                                         StagedWriter{.writer = ir::Writer{.site = 0x008040, .engine = true},
                                                      .unwritten = false,
                                                      .bytes = 20,
                                                      .origin = {},
                                                      .sources = {}}},
                             .contents = {content({5}, 3), sameAgain, content({6}, 1)}},
                 StagedRange{.memory = 0x7E1000,
                             .bytes = 8,
                             .origin = {},
                             .writers = {},
                             .contents = {content({7}, 0)}}};

  // Outcomes that differ only in values are one; two that differ in an extent are two.
  into.enumerated = {outcome({0, 1}, 0x7E6000)};
  from.enumerated = {outcome({4}, 0x7E6000), outcome({2}, 0x7E6100)};

  // Every count: the added ones, and the four that take the larger.
  into.instructions = 100;
  from.instructions = 50;
  into.interrupts = 7;
  from.interrupts = 5;
  into.divergences = 1;
  from.divergences = 2;
  into.spc700Instructions = 300;
  from.spc700Instructions = 30;
  into.spc700Divergences = 3;
  from.spc700Divergences = 4;
  into.variantsPlanned = 9;
  from.variantsPlanned = 6;
  into.variantsRun = 2;
  from.variantsRun = 3;
  into.variantsReturned = 11;
  from.variantsReturned = 12;
  into.variantsConfined = 13;
  from.variantsConfined = 14;
  into.enumerationCycles = 1000;
  from.enumerationCycles = 2000;
  into.nodes = 40;
  from.nodes = 30;
  into.spc700Nodes = 9;
  from.spc700Nodes = 8;
  into.originSets = 70;
  from.originSets = 60;
  into.originCap = 64;
  from.originCap = 128;

  joinObservation(into, from);

  ASSERT_EQ(into.reached.size(), 3u);
  EXPECT_EQ(into.reached[0].site, 0x008001u);
  EXPECT_EQ(into.reached[1].site, 0x008002u);
  EXPECT_EQ(into.reached[2].site, 0x008003u);
  ASSERT_EQ(into.ran.size(), 2u);
  EXPECT_EQ(into.ran[0].site, 0x008004u);

  ASSERT_EQ(into.moved.size(), 2u);
  EXPECT_EQ(into.moved[0].site, 0x008030u);
  EXPECT_EQ(into.moved[1].times, 5u);

  ASSERT_EQ(into.palettes.size(), 2u);
  EXPECT_EQ(into.palettes[1], palette(0x20));
  ASSERT_EQ(into.landed.size(), 2u);
  EXPECT_EQ(into.landed[0].times, 3u);
  EXPECT_EQ(into.landed[0].landing.palette, std::optional<std::size_t>(0));
  EXPECT_EQ(into.landed[1].site, 0x008050u);
  EXPECT_EQ(into.landed[1].landing.palette, std::optional<std::size_t>(1));

  ASSERT_EQ(into.walked.size(), 1u);
  EXPECT_EQ(into.walked[0].times, 5u);

  ASSERT_EQ(into.streamed.size(), 1u);
  EXPECT_EQ(into.streamed[0].times, 5u);
  EXPECT_EQ(into.streamed[0].source.last, 0x20Fu);

  ASSERT_EQ(into.samples.size(), 2u);
  EXPECT_EQ(into.samples[0].start, 0x2000u);
  EXPECT_EQ(into.samples[1].times, 2u);

  ASSERT_EQ(into.seen.size(), 1u);
  EXPECT_EQ(into.seen[0].d, (std::vector<std::uint16_t>{0x0000, 0x0100, 0x0200}));
  EXPECT_EQ(into.seen[0].dbr, (std::vector<std::uint8_t>{0x00, 0x7E}));

  ASSERT_EQ(into.staged.size(), 2u);
  EXPECT_EQ(into.staged[0].memory, 0x7E1000u);
  EXPECT_EQ(into.staged[0].contents[0].order, 5u);
  const StagedRange& joined = into.staged[1];
  EXPECT_EQ(joined.origin.image, (std::vector<ir::OriginInterval>{{.first = 0x100, .last = 0x11F}}));
  ASSERT_EQ(joined.writers.size(), 2u);
  EXPECT_EQ(joined.writers[0].bytes, 20u);
  EXPECT_EQ(joined.writers[1].bytes, 16u);
  EXPECT_EQ(joined.writers[1].sources, (std::vector<ir::OriginInterval>{{.first = 0x100, .last = 0x11F}}));
  EXPECT_EQ(joined.writers[1].origin.image, (std::vector<ir::OriginInterval>{{.first = 0x100, .last = 0x11F}}));
  ASSERT_EQ(joined.contents.size(), 4u);
  EXPECT_EQ(joined.contents[0].order, 4u);
  EXPECT_FALSE(joined.contents[0].landing.has_value());
  EXPECT_EQ(joined.contents[1].order, 2u);
  EXPECT_EQ(joined.contents[2].bytes, (std::vector<std::uint8_t>{6}));
  EXPECT_EQ(joined.contents[2].order, 6u);
  EXPECT_EQ(joined.contents[3].bytes, (std::vector<std::uint8_t>{5}));
  EXPECT_EQ(joined.contents[3].order, 8u);

  ASSERT_EQ(into.enumerated.size(), 2u);
  EXPECT_EQ(into.enumerated[0].values, (std::vector<std::uint32_t>{0, 1, 4}));
  EXPECT_EQ(into.enumerated[1].values, (std::vector<std::uint32_t>{2}));

  EXPECT_EQ(into.instructions, 150u);
  EXPECT_EQ(into.interrupts, 12u);
  EXPECT_EQ(into.divergences, 3u);
  EXPECT_EQ(into.spc700Instructions, 330u);
  EXPECT_EQ(into.spc700Divergences, 7u);
  EXPECT_EQ(into.variantsPlanned, 15u);
  EXPECT_EQ(into.variantsRun, 5u);
  EXPECT_EQ(into.variantsReturned, 23u);
  EXPECT_EQ(into.variantsConfined, 27u);
  EXPECT_EQ(into.enumerationCycles, 3000u);
  EXPECT_EQ(into.nodes, 40u);
  EXPECT_EQ(into.spc700Nodes, 9u);
  EXPECT_EQ(into.originSets, 70u);
  EXPECT_EQ(into.originCap, 128u);
}

TEST(RomRecord, RecordsOfOneRunCountItsSightingsOnceAndJoinTheirOutcomes) {
  RunRecord first = everyKind();
  RunRecord second = first;
  second.observation.enumerated = {outcome({7}, 0x7E7000)};
  second.observation.variantsRun = 1;
  second.notes.push_back("enumerate sub_0080C0: a note only the second holds");
  second.enumerationNotes.push_back("enumerate sub_0080C0: the second run did not enter it; nothing run");

  const RunRecord one = foldRecords(std::vector<RunRecord>{first, second});
  EXPECT_EQ(one.observation.instructions, first.observation.instructions);
  EXPECT_EQ(one.observation.moved[0].times, first.observation.moved[0].times);
  ASSERT_EQ(one.observation.enumerated.size(), 2u);
  EXPECT_EQ(one.observation.enumerated[1].values, (std::vector<std::uint32_t>{7}));
  EXPECT_EQ(one.observation.variantsRun, 5u);
  EXPECT_EQ(one.notes.size(), 3u);
  EXPECT_EQ(one.enumerationNotes,
            (std::vector<std::string>{first.enumerationNotes[0], second.enumerationNotes[1]}));

  RunRecord other = first;
  other.observation.instructions += 1;
  const RunRecord two = foldRecords(std::vector<RunRecord>{first, other});
  EXPECT_EQ(two.observation.instructions, 2u * first.observation.instructions + 1u);
  EXPECT_EQ(two.observation.moved[0].times, 2u * first.observation.moved[0].times);
  EXPECT_EQ(two.notes, first.notes);
  EXPECT_EQ(two.enumerationNotes, first.enumerationNotes);

  // A run that differs only in one sighting is another run, counted as well.
  RunRecord sighted = first;
  sighted.observation.reached.push_back(ReachedTarget{
      .target = 0x008400, .mode = Cpu65816Mode::reset(), .site = 0x0080F0, .call = false, .name = {}});
  const RunRecord three = foldRecords(std::vector<RunRecord>{first, sighted});
  EXPECT_EQ(three.observation.instructions, 2u * first.observation.instructions);
  EXPECT_EQ(three.observation.reached.size(), first.observation.reached.size() + 1u);
}

TEST(RomRecord, ARecordOfARunOfEachExampleCartridgeReadsBackEqual) {
  // Twelve frames of each example: between them the runs hold every kind but
  // the outcomes, which a run given no plan has none of.
  constexpr std::uint64_t kFrame = 357'366u;
  bool reached = false;
  bool ran = false;
  bool noted = false;
  bool streamed = false;
  bool sampled = false;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "snaggletooth_record_examples_test";
  std::uint64_t files = 0xCBF29CE484222325u;  // FNV-1a over every record's file
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    RunRecord record;
    record.observation = observeRun(rom, 12u * kFrame, InputScript{}, record.notes);
    reached = reached || !record.observation.reached.empty();
    ran = ran || !record.observation.ran.empty();
    noted = noted || !record.notes.empty();
    streamed = streamed || !record.observation.streamed.empty();
    sampled = sampled || !record.observation.samples.empty();
    const std::string text = renderRecord(record, rom);
    std::string error;
    const std::optional<RunRecord> read = parseRecord(text, error);
    ASSERT_TRUE(read.has_value()) << example.name << ": " << error;
    EXPECT_TRUE(*read == record) << example.name;
    EXPECT_EQ(renderRecord(*read, rom), text) << example.name;
    // Each record's file, as `writeRecord` writes it, into one hash.
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(writeRecord(record, rom, dir, error)) << error;
    for (const char c : fileText(dir / "runs" / "0001.snagrun.zst")) {
      files = (files ^ static_cast<std::uint8_t>(c)) * 0x100000001B3u;
    }
    std::filesystem::remove_all(dir, ec);
  }
  EXPECT_TRUE(reached && ran && noted && streamed && sampled);
  // The parameters — level 19, the long window, one thread — give these files,
  // byte for byte, on every platform.
  EXPECT_EQ(files, 4680697128569359145u) << "the 28 records' files, byte for byte";
}

TEST(RomRecord, RecordsAreNumberedFilesUnderTheTreesRunsAndATextIsWrittenOnce) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "snaggletooth_record_test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  const std::vector<std::uint8_t> rom = liftingImage();
  std::string error;

  const std::optional<std::vector<RunRecord>> none = readRecords(dir, rom, error);
  ASSERT_TRUE(none.has_value()) << error;
  EXPECT_TRUE(none->empty());

  const RunRecord first = everyKind();
  RunRecord second = first;
  second.observation.instructions = 1;
  ASSERT_TRUE(writeRecord(first, rom, dir, error)) << error;
  ASSERT_TRUE(writeRecord(second, rom, dir, error)) << error;
  ASSERT_TRUE(writeRecord(first, rom, dir, error)) << error;
  const std::optional<CartridgeHeader> header = parseCartridgeHeader(rom);
  ASSERT_TRUE(header.has_value());
  ASSERT_TRUE(writeRecord(second, rom.size(), header->checksum, dir, error)) << error;
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "runs" / "0001.snagrun.zst"));
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "runs" / "0002.snagrun.zst"));
  EXPECT_FALSE(std::filesystem::exists(dir / "runs" / "0003.snagrun.zst"));

  // A record is one zstd frame that says its content's size and carries its
  // checksum: the magic 28 B5 2F FD, then a descriptor with the checksum flag.
  const std::string packed = fileText(dir / "runs" / "0001.snagrun.zst");
  ASSERT_GE(packed.size(), 9u);
  EXPECT_EQ(packed.substr(0, 4), std::string("\x28\xB5\x2F\xFD", 4));
  EXPECT_EQ(static_cast<std::uint8_t>(packed[4]) & 0x04u, 0x04u) << "the checksum flag";
  EXPECT_NE(static_cast<std::uint8_t>(packed[4]) & 0xC0u, 0u) << "the content size is written";
  EXPECT_LT(packed.size(), renderRecord(first, rom).size()) << "compressed";
  // The parameters — level 19, the long window, one thread — give this record
  // exactly this file, on every platform.
  EXPECT_EQ(packed.size(), 1679u) << "the record's file";
  std::uint64_t hash = 0xCBF29CE484222325u;  // FNV-1a over the file's bytes
  for (const char c : packed) hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001B3u;
  EXPECT_EQ(hash, 7592184046074743795u) << "the record's file, byte for byte";

  // Past four digits a number takes as many as it needs, and the records stand
  // in the order of their numbers.
  std::filesystem::copy_file(dir / "runs" / "0002.snagrun.zst", dir / "runs" / "9999.snagrun.zst", ec);
  ASSERT_FALSE(ec) << ec.message();
  RunRecord third = first;
  third.observation.instructions = 3;
  ASSERT_TRUE(writeRecord(third, rom, dir, error)) << error;
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "runs" / "10000.snagrun.zst"));
  const std::optional<std::vector<RunRecord>> read = readRecords(dir, rom, error);
  ASSERT_TRUE(read.has_value()) << error;
  ASSERT_EQ(read->size(), 4u);
  EXPECT_TRUE((*read)[0] == first);
  EXPECT_TRUE((*read)[1] == second);
  EXPECT_TRUE((*read)[2] == second);
  EXPECT_TRUE((*read)[3] == third);

  // Any zstd file of a record's text is the record: one another compressor
  // wrote is read, and the same text is not written again beside it.
  RunRecord fourth = first;
  fourth.observation.instructions = 4;
  putFile(dir / "runs" / "10001.snagrun.zst", rawZstd(renderRecord(fourth, rom)));
  const std::optional<std::vector<RunRecord>> stored = readRecords(dir, rom, error);
  ASSERT_TRUE(stored.has_value()) << error;
  ASSERT_EQ(stored->size(), 5u);
  EXPECT_TRUE(stored->back() == fourth);
  ASSERT_TRUE(writeRecord(fourth, rom, dir, error)) << error;
  EXPECT_FALSE(std::filesystem::exists(dir / "runs" / "10002.snagrun.zst"));
  // A file under `runs/` named otherwise is not a record, and is left alone.
  putFile(dir / "runs" / "0005.snagrun", renderRecord(fourth, rom));
  ASSERT_EQ(readRecords(dir, rom, error)->size(), 5u) << error;
  EXPECT_TRUE(std::filesystem::exists(dir / "runs" / "0005.snagrun"));

  // A zstd file of text that is not a record is refused by its head.
  putFile(dir / "runs" / "0099.snagrun.zst", rawZstd("a file of something else\n"));
  EXPECT_FALSE(checkRecords(dir, rom, error).has_value());
  EXPECT_EQ(error, (dir / "runs" / "0099.snagrun.zst").string() + ": a record begins `snagrun 1`") << error;
  std::filesystem::remove(dir / "runs" / "0099.snagrun.zst", ec);

  // Checking the records reads each as far as its head, and counts them.
  const std::optional<std::size_t> checked = checkRecords(dir, rom, error);
  ASSERT_TRUE(checked.has_value()) << error;
  EXPECT_EQ(*checked, 5u);

  // A file that is not one whole zstd frame, or holds bytes past it, is
  // refused by its path.
  const std::string bad = (dir / "runs" / "10002.snagrun.zst").string();
  putFile(dir / "runs" / "10002.snagrun.zst", renderRecord(fourth, rom));
  EXPECT_FALSE(readRecords(dir, rom, error).has_value());
  EXPECT_EQ(error, bad + ": not a whole zstd file") << error;
  EXPECT_FALSE(checkRecords(dir, rom, error).has_value());
  EXPECT_EQ(error, bad + ": not a whole zstd file") << error;
  // A frame cut short past its head reads as a record's head, so the check
  // passes it, and reading the record refuses it.
  const std::string whole = rawZstd(renderRecord(fourth, rom));
  putFile(dir / "runs" / "10002.snagrun.zst", whole.substr(0, whole.size() / 2u));
  EXPECT_EQ(checkRecords(dir, rom, error), std::optional<std::size_t>(6u)) << error;
  EXPECT_FALSE(readRecords(dir, rom, error).has_value());
  EXPECT_EQ(error, bad + ": not a whole zstd file") << error;
  putFile(dir / "runs" / "10002.snagrun.zst", rawZstd(renderRecord(fourth, rom)) + "x");
  EXPECT_FALSE(readRecords(dir, rom, error).has_value());
  EXPECT_EQ(error, bad + ": bytes follow the zstd frame") << error;
  std::filesystem::remove(dir / "runs" / "10002.snagrun.zst", ec);

  // A record written for another image is refused by its path.
  std::vector<std::uint8_t> longer = rom;
  longer.resize(rom.size() * 2u, 0xFF);
  ASSERT_TRUE(writeRecord(first, longer, dir, error)) << error;
  EXPECT_FALSE(readRecords(dir, rom, error).has_value());
  const std::string path = (dir / "runs" / "10002.snagrun.zst").string();
  EXPECT_EQ(error.rfind(path + ": the record was written for an image of", 0), 0u) << error;
  EXPECT_FALSE(checkRecords(dir, rom, error).has_value());
  EXPECT_EQ(error.rfind(path + ": the record was written for an image of", 0), 0u) << error;

  std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace snaggletooth::disasm
