// The disassembly framework, exercised through a synthetic backend.
//
// The backend here is a toy instruction set built to have exactly the properties
// the framework exists to handle and nothing else: 24-bit addresses, an
// instruction whose length depends on the context the trace carries, and a
// register at a known address. Every case pins something the SPC700 backend
// cannot show, because that chip has 16-bit addresses and no context.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "disasm/disasm.h"
#include "spc700_disasm.h"

namespace snaggletooth::disasm {
namespace {

// The toy instruction set.
//
//   $00           NOP                 one byte, falls through
//   $01 nn        CTX #nn             falls through with the context set to nn
//   $02 ll hh bb  JMP $bb:hhll        never falls through
//   $03 ll hh bb  CALL $bb:hhll       comes back
//   $04 rr        BR rel              falls through, and may reach the target
//   $05           RET
//   $06 ll hh     LD $hhll            the operand names an address in the current bank
//   $07 ...       IMM                 two bytes under context 0, three under context 1
//   anything else HALT
//
// The register at offset $2140 of any bank is named PORT.
class ToyBackend final : public Backend {
 public:
  std::string_view name() const override { return "toy"; }
  unsigned addressBits() const override { return 24; }

  std::optional<Decoded> decode(std::span<const std::uint8_t> image, Address base, Address at,
                                Context context) const override {
    if (at < base) return std::nullopt;
    const std::size_t offset = static_cast<std::size_t>(at - base);
    if (offset >= image.size()) return std::nullopt;
    const std::uint8_t opcode = image[offset];

    Instruction out;
    out.address = at;
    out.opcode = opcode;
    Context next = context;
    auto operand = [&](std::size_t i) -> std::uint8_t {
      return offset + i < image.size() ? image[offset + i] : std::uint8_t{0};
    };
    const Address bank = at & 0xFF0000u;
    switch (opcode) {
      case 0x00: out.length = 1; out.text = "NOP"; break;
      case 0x01:
        out.length = 2;
        out.text = "CTX #" + std::to_string(operand(1));
        next.bits = operand(1);
        break;
      case 0x02:
      case 0x03: {
        out.length = 4;
        const Address target = static_cast<Address>(operand(1)) |
                               (static_cast<Address>(operand(2)) << 8) |
                               (static_cast<Address>(operand(3)) << 16);
        out.target = target;
        out.flow = opcode == 0x02 ? Flow::Jump : Flow::Call;
        out.text = std::string(opcode == 0x02 ? "JMP " : "CALL ") + formatAddress(target, 24);
        out.symbolic = SymbolicText{.before = opcode == 0x02 ? "JMP " : "CALL ", .after = ""};
        break;
      }
      case 0x04: {
        out.length = 2;
        const Address after = following(at, 2);
        const Address target = bank | ((after + static_cast<std::int8_t>(operand(1))) & 0xFFFFu);
        out.target = target;
        out.flow = Flow::Branch;
        out.text = "BR " + formatAddress(target, 24);
        // The toy writes a branch's symbol in brackets, so the two parts show.
        out.symbolic = SymbolicText{.before = "BR [", .after = "]"};
        break;
      }
      case 0x05: out.length = 1; out.flow = Flow::Return; out.text = "RET"; break;
      case 0x06: {
        out.length = 3;
        const Address named = bank | static_cast<Address>(operand(1)) |
                              (static_cast<Address>(operand(2)) << 8);
        out.operandAddress = named;
        out.text = "LD " + formatAddress(named, 24);
        break;
      }
      case 0x07:
        out.length = context.bits == 0 ? 2 : 3;
        out.text = context.bits == 0 ? "IMM #8" : "IMM #16";
        break;
      default: out.length = 1; out.flow = Flow::Halt; out.text = "HALT"; break;
    }
    if (offset + out.length > image.size()) return std::nullopt;
    out.bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(offset),
                     image.begin() + static_cast<std::ptrdiff_t>(offset + out.length));
    return Decoded{.instruction = std::move(out), .next = next};
  }

  std::string_view registerName(Address address) const override {
    return (address & 0xFFFFu) == 0x2140u ? "PORT" : std::string_view{};
  }
};

const ToyBackend& toy() {
  static const ToyBackend backend;
  return backend;
}

std::vector<const Line*> codeLines(const Listing& listing) {
  std::vector<const Line*> out;
  for (const Line& line : listing.lines) {
    if (line.isCode) out.push_back(&line);
  }
  return out;
}

}  // namespace

// Addresses carry a bank: a long call into the image's own bank is followed, the
// label it earns carries all six digits, and the listing prints the bank.
TEST(DisasmFramework, AddressesAreTwentyFourBitsThroughout) {
  const std::vector<std::uint8_t> image = {
      0x03, 0x05, 0x80, 0xC0,  // CALL $C0:8005
      0x00,                    // NOP — reached, because a call comes back
      0x05,                    // RET, at $C0:8005
  };
  Request request;
  request.image = image;
  request.base = 0xC08000;
  const Listing listing = trace(toy(), request);

  ASSERT_EQ(codeLines(listing).size(), 3u);
  EXPECT_EQ(codeLines(listing)[2]->address, 0xC08005u);
  ASSERT_TRUE(listing.labels.count(0xC08005u));
  EXPECT_EQ(listing.labels.at(0xC08005u), "sub_C08005");
  EXPECT_EQ(listing.addressBits, 24u);

  const std::string text = render(listing);
  EXPECT_NE(text.find("ORG $C0:8000"), std::string::npos);
  EXPECT_NE(text.find("; $C0:8005"), std::string::npos);
}

// The context a backend hands out of one instruction is what the next one is
// decoded under. Here that changes an instruction's length, which is the 65816's
// shape exactly.
TEST(DisasmFramework, ContextRidesOnTheWorkItemAndPropagates) {
  const std::vector<std::uint8_t> narrow = {
      0x07, 0xAA,  // IMM #8 under context 0
      0x00,        // NOP
      0xFF,        // HALT
  };
  Request request;
  request.image = narrow;
  request.base = 0x008000;
  const Listing withoutContext = trace(toy(), request);
  ASSERT_EQ(codeLines(withoutContext).size(), 3u);
  EXPECT_EQ(codeLines(withoutContext)[0]->instruction.length, 2);
  EXPECT_EQ(codeLines(withoutContext)[0]->instruction.text, "IMM #8");

  const std::vector<std::uint8_t> wide = {
      0x01, 0x01,        // CTX #1
      0x07, 0xAA, 0xBB,  // IMM #16 under context 1 — three bytes
      0xFF,              // HALT
  };
  request.image = wide;
  const Listing withContext = trace(toy(), request);
  ASSERT_EQ(codeLines(withContext).size(), 3u);
  EXPECT_EQ(codeLines(withContext)[1]->instruction.length, 3);
  EXPECT_EQ(codeLines(withContext)[1]->instruction.text, "IMM #16");
  EXPECT_EQ(codeLines(withContext)[2]->address, 0x008005u);
}

// The context every entry starts with comes from the request.
TEST(DisasmFramework, EntriesStartWithTheRequestsContext) {
  const std::vector<std::uint8_t> image = {0x07, 0xAA, 0xBB, 0xFF};
  Request request;
  request.image = image;
  request.base = 0x008000;
  request.context.bits = 1;
  const Listing fromBase = trace(toy(), request);
  ASSERT_FALSE(fromBase.lines.empty());
  EXPECT_EQ(fromBase.lines.front().instruction.text, "IMM #16");

  // The same through an explicit entry.
  request.entries = {0x008000};
  const Listing fromEntry = trace(toy(), request);
  ASSERT_FALSE(fromEntry.lines.empty());
  EXPECT_EQ(fromEntry.lines.front().instruction.text, "IMM #16");
}

// Two paths reaching one address under different contexts is reported, naming
// both, and the first reading is kept rather than either being guessed.
TEST(DisasmFramework, ConflictingContextsAreReportedNotGuessed) {
  const std::vector<std::uint8_t> image = {
      0x01, 0x01,              // $8000  CTX #1
      0x03, 0x08, 0x80, 0x00,  // $8002  CALL $00:8008 — reaches $8008 under context 1
      0x01, 0x00,              // $8006  CTX #0 — falls into $8008 under context 0
      0x07, 0xAA, 0xBB,        // $8008  IMM
      0x05,                    // $800B  RET
  };
  Request request;
  request.image = image;
  request.base = 0x008000;
  const Listing listing = trace(toy(), request);

  ASSERT_EQ(listing.warnings.size(), 1u);
  EXPECT_NE(listing.warnings[0].find("$00:8008"), std::string::npos);
  EXPECT_NE(listing.warnings[0].find("context 1"), std::string::npos);
  EXPECT_NE(listing.warnings[0].find("context 0"), std::string::npos);
  // The call's arrival came first, so the instruction reads as the wide form.
  bool found = false;
  for (const Line* line : codeLines(listing)) {
    if (line->address == 0x008008u) {
      found = true;
      EXPECT_EQ(line->instruction.text, "IMM #16");
    }
  }
  EXPECT_TRUE(found);
}

// Reaching an address twice under the same context is a loop, not a conflict.
TEST(DisasmFramework, ALoopUnderOneContextIsNotAConflict) {
  const std::vector<std::uint8_t> image = {
      0x00,        // NOP
      0x04, 0xFD,  // BR back to the NOP
      0xFF,        // HALT
  };
  Request request;
  request.image = image;
  request.base = 0x008000;
  const Listing listing = trace(toy(), request);
  EXPECT_TRUE(listing.warnings.empty());
  EXPECT_EQ(listing.labels.count(0x008000u), 1u);
}

// A register is named through the backend from the address the operand reports,
// and only when the request asks for annotations.
TEST(DisasmFramework, RegistersAreNamedThroughTheBackend) {
  const std::vector<std::uint8_t> image = {0x06, 0x40, 0x21, 0xFF};  // LD $2140 / HALT
  Request request;
  request.image = image;
  request.base = 0x7E8000;
  const Listing named = trace(toy(), request);
  ASSERT_TRUE(named.lines.front().isCode);
  EXPECT_EQ(named.lines.front().instruction.note, "PORT");

  request.annotateRegisters = false;
  const Listing bare = trace(toy(), request);
  EXPECT_TRUE(bare.lines.front().instruction.note.empty());
}

// The raw-bytes column is as wide as the longest instruction in the listing, so a
// four-byte call does not push its cycle cost out of line with the one-byte
// instruction beside it.
TEST(DisasmFramework, TheBytesColumnWidensToTheLongestInstruction) {
  const std::vector<std::uint8_t> image = {
      0x03, 0x05, 0x80, 0x00,  // CALL $00:8005
      0x00,                    // NOP
      0x05,                    // RET
  };
  Request request;
  request.image = image;
  request.base = 0x008000;
  const std::string text = render(trace(toy(), request));

  // Every code line ends in its cycle cost; with equal-width byte fields the cost
  // sits at the same column on every line.
  std::optional<std::size_t> column;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    const std::size_t comment = line.find("; $");
    if (comment == std::string::npos || line.find("|") != std::string::npos) continue;
    const std::size_t cost = line.find_last_of(' ');
    if (!column) column = cost;
    EXPECT_EQ(cost, *column) << line;
  }
  EXPECT_TRUE(column.has_value());
}

// A target that carries a label is written as the label, in the form the backend
// gave the instruction — and the label is one the listing defines, so the
// source names only lines it holds.
TEST(DisasmFramework, ATargetWithALabelIsWrittenAsTheLabel) {
  const std::vector<std::uint8_t> image = {
      0x03, 0x07, 0x80, 0x00,  // $8000  CALL $00:8007
      0x04, 0xFA,              // $8004  BR back to $8000
      0xFF,                    // $8006  HALT
      0x05,                    // $8007  RET
  };
  Request request;
  request.image = image;
  request.base = 0x008000;
  const std::string text = render(trace(toy(), request));
  EXPECT_NE(text.find("        CALL sub_008007 "), std::string::npos) << text;
  EXPECT_NE(text.find("        BR [entry] "), std::string::npos) << text;
  EXPECT_EQ(text.find("CALL $00:8007"), std::string::npos);
  EXPECT_EQ(text.find("BR $00:8000"), std::string::npos);
  // The comment still carries the address and the bytes.
  EXPECT_NE(text.find("; $00:8000  03 07 80 00"), std::string::npos) << text;
}

// A target with no line of its own — one that lands inside an instruction
// already decoded — keeps no label and is written as its address.
TEST(DisasmFramework, ATargetWithoutALineKeepsNoLabelAndIsWrittenAsAnAddress) {
  const std::vector<std::uint8_t> image = {
      0x03, 0x02, 0x80, 0x00,  // $8000  CALL $00:8002 — into its own operand bytes
      0x05,                    // $8004  RET
  };
  Request request;
  request.image = image;
  request.base = 0x008000;
  const Listing listing = trace(toy(), request);
  EXPECT_EQ(listing.labels.count(0x008002u), 0u);
  ASSERT_EQ(listing.labels.count(0x008000u), 1u);
  const std::string text = render(listing);
  EXPECT_NE(text.find("        CALL $00:8002 "), std::string::npos) << text;
}

// A backend that gives an instruction no symbolic form keeps the address even
// where the target carries a label.
TEST(DisasmFramework, AnInstructionWithoutASymbolicFormKeepsTheAddress) {
  Listing listing;
  listing.addressBits = 24;
  listing.labels[0x008000u] = "entry";
  Line line;
  line.isCode = true;
  line.address = 0x008000u;
  line.instruction.address = 0x008000u;
  line.instruction.length = 2;
  line.instruction.bytes = {0x04, 0xFE};
  line.instruction.text = "BR $00:8000";
  line.instruction.target = 0x008000u;
  listing.lines.push_back(line);
  EXPECT_NE(render(listing).find("        BR $00:8000 "), std::string::npos);
  listing.lines[0].instruction.symbolic = SymbolicText{.before = "BR [", .after = "]"};
  EXPECT_NE(render(listing).find("        BR [entry] "), std::string::npos);
}

// The SPC700 backend over the same framework: 16-bit addresses, no context.
TEST(DisasmFramework, TheSpc700BackendCarriesTheContextThroughUnchanged) {
  const Spc700Backend& backend = spc700Backend();
  EXPECT_EQ(backend.name(), "SPC700");
  EXPECT_EQ(backend.addressBits(), 16u);

  const std::vector<std::uint8_t> image = {0xE8, 0x01, 0x6F};  // MOV A,#$01 / RET
  Context context{.bits = 7};
  const std::optional<Decoded> decoded = backend.decode(image, 0x0400, 0x0400, context);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->next, context);
  EXPECT_EQ(decoded->instruction.text, "MOV A,#$01");
  // An address above the chip's space is outside every image.
  EXPECT_FALSE(backend.decode(image, 0x0400, 0x010400, context).has_value());
}

}  // namespace snaggletooth::disasm
