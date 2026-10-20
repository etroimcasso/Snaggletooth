#include "snaggletooth/snes/snes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#define SNAGGLETOOTH_NOINLINE __declspec(noinline)
#else
#define SNAGGLETOOTH_NOINLINE __attribute__((noinline))
#endif

#include "bytes.h"
#include "ppu_timeline_words.h"
#include "snes_ipl_stub.h"

namespace snaggletooth {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the threads' counters are read and written without a lock");

// One turn of a spin: tells the core the thread is waiting on another, which saves
// power and gives a sibling hardware thread the core.
inline void cpuRelax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
  __yield();
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
  __builtin_ia32_pause();
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
  asm volatile("yield");
#endif
}

// How long a thread of the machine's stays awake with nothing to run before it
// parks: long enough to cover the gap from one line's port reads, or one line's
// entry, to the next line's at the console's rate, and the gap between a host's
// calls inside one frame, so a thread the console is about to need is awake; short
// enough that a host's sleep inside a report, or between frames, leaves no core busy.
constexpr std::chrono::microseconds kHotWindow{100};
// Relaxed turns between two readings of the clock in a thread's wait loop.
constexpr std::uint32_t kClockEvery = 64u;
// Relaxed turns the console makes before it sleeps for the picture thread: enough
// for a thread a few entries behind to catch up without a wake.
constexpr std::uint32_t kSleepAfterTurns = 256u;
// Audio cycles the console pays before it publishes the count to a sleeping thread:
// a millisecond of the audio machine's time. The count is also published before
// every point that depends on it — a port write's stamp, a read's or a report's
// wait, a call's return — so what a sleeping thread has not run is never more than
// this, and what it runs is the same count in the same order.
constexpr std::uint64_t kApuBatch = 1024u;

// The APU advances by an exact rational share of the master cycles that elapse.
// The APU runs off a 24.576 MHz crystal divided by 24 — a 1,024,000 Hz cycle rate
// — in both regions; only the master clock differs. The NTSC master clock is
// 236,250,000 / 11 Hz and the PAL master clock is 21,281,370 Hz, so the ratio of
// APU cycles to master cycles reduces to lowest terms as:
//   NTSC: 1,024,000 / (236,250,000 / 11) = 5632 / 118125
//   PAL:  1,024,000 / 21,281,370         = 102400 / 2128137
// The machine accumulates the numerator per master cycle and takes an APU cycle
// for every denominator, so the count run after N master cycles is exactly
// floor(N * num / den) — no floating point, no rounding drift.
struct ApuRatio {
  std::uint32_t num;
  std::uint32_t den;
};
constexpr ApuRatio kNtscApu{.num = 5632u, .den = 118125u};
constexpr ApuRatio kPalApu{.num = 102400u, .den = 2128137u};

// The scanline structure. A line is 1364 master cycles and carries 340 dots: every
// dot is four cycles except 323 and 327, which are six. Two lines a frame are not
// 1364 — NTSC's line 240 on an odd field is four cycles short, and PAL's line 311 on
// an odd interlaced field four long — and an interlaced frame of even parity runs one
// line past the 262 NTSC and 312 PAL a frame otherwise has, which is where dot 340
// exists at all.
constexpr std::uint16_t kLineMaster = 1364u;
constexpr std::uint16_t kShortLineMaster = 1360u;
constexpr std::uint16_t kLongLineMaster = 1368u;
constexpr std::uint16_t kShortLineV = 240u;
constexpr std::uint16_t kLongLineV = 311u;
constexpr std::uint16_t kNtscLines = 262u;
constexpr std::uint16_t kPalLines = 312u;

// Where the dot map's two long dots begin and end, in master cycles into the line.
constexpr std::uint16_t kFirstLongDot = 1292u;
constexpr std::uint16_t kAfterFirstLongDot = 1298u;
constexpr std::uint16_t kSecondLongDot = 1310u;
constexpr std::uint16_t kAfterSecondLongDot = 1316u;

// The horizon of a line whose remaining spans hold no event.
constexpr std::uint64_t kNoLineEvent = ~std::uint64_t{0};

// The per-line events, as master cycles into the line they belong to. The blank flag
// is raised at H = 274 and lowered again at H = 1 of the line that follows, on every
// line of the frame; the frame's parity toggles at H = 1 of its first line; vertical
// blank's own line raises the NMI flag at H = 0.5 and hands the sprite table back at
// H = 10; and each visible line delivers its HDMA at dot 278.
constexpr std::uint16_t kHblankClear = 4u;
constexpr std::uint16_t kHblankSet = 1096u;
constexpr std::uint16_t kFieldToggle = 4u;
constexpr std::uint16_t kNmiFlagOffset = 2u;
constexpr std::uint16_t kOamReloadOffset = 40u;
constexpr std::uint16_t kHdmaDeliver = 1112u;

// The picture the chip draws, kFirstPictureDot to kLastPictureDot of every line the
// frame's own vertical blank leaves below it, the first line of a frame drawing
// nothing. Every one of those dots is four master cycles wide, the two long ones
// lying well past them.
static_assert(kPictureWidth == kLastPictureDot - kFirstPictureDot + 1u,
              "the picture is as wide as the dots it is drawn from");
constexpr std::uint16_t kTallestPicture = kOverscanVblankStartLine - 1u;
constexpr std::size_t kPixelBytes = 4u;
constexpr std::size_t kRasterBytes = kHiresWidth * kPixelBytes * kTallestPicture;

// The H/V timer's trigger points. With an H position to compare against, the flag is
// raised 14 master cycles past four times it; with none, 1374 master cycles after the
// previous line began — which is ten into a line following a normal one, fourteen
// after the short one and six after the long one.
constexpr std::uint16_t kTimerHOrigin = 14u;
constexpr std::uint16_t kTimerLineSpan = 1374u;
// The dot the timer raises nothing on, on the short line and on a frame's last line:
// anomie-timing.txt 121-123 measures both, and documents no mechanism for either.
constexpr std::uint16_t kTimerQuietDot = 153u;

// The memory refresh: the CPU is held off the bus for forty master cycles once a
// line, at the point on an eight-cycle grid nearest 536 into the line, spent a fast
// cycle at a time so a run can stop inside one.
constexpr std::uint16_t kRefreshMaster = 40u;
constexpr std::uint16_t kRefreshTarget = 536u;
constexpr std::uint64_t kFirstRefresh = 538u;  // the first line's pause, which SnesState starts `refreshAt` at
constexpr std::uint32_t kRefreshStep = 6u;

// The multiply/divide unit is clocked by the CPU, so its documented latencies are
// counted in CPU cycles — the same wait no matter the memory speed. Multiply is
// ready after 8, divide after 16.
constexpr std::uint8_t kMultiplyClocks = 8u;
constexpr std::uint8_t kDivideClocks = 16u;

// The auto-joypad read. It begins on vertical blank's first line: at H = 74.5 the
// first time the machine performs one, and thereafter at the first point on a
// 256-cycle grid carried from the previous read's start that lies at or past
// H = 32.5. The strobe pulse takes 128 cycles, then each of the sixteen bits a
// further 256, so the busy flag holds for 4224 cycles and the sixteenth bit lands
// as it clears.
constexpr std::uint16_t kAutoJoyFirstStart = 298u;   // H = 74.5, in master cycles into the line
constexpr std::uint16_t kAutoJoyEarliest = 130u;     // H = 32.5
constexpr std::uint16_t kAutoJoyGrid = 256u;
constexpr std::uint16_t kAutoJoyStrobe = 128u;
constexpr std::uint16_t kAutoJoyBit = 256u;
constexpr std::uint8_t kAutoJoyBits = 16u;
static_assert(kAutoJoyStrobe + kAutoJoyBits * kAutoJoyBit == 4224u,
              "the auto-read's busy window is the documented 4224 master cycles");

// The master-cycle cost of reaching an address, by the documented region map,
// with the second waitstate region ($80-$BF:$8000-$FFFF and $C0-$FF) at its
// MEMSEL rate: fast when `fast2` is set, slow otherwise. Every region but one
// is whole 8 KB pages, so a page's first address prices the page; the manual
// joypad ports, $4000-$41FF of a system bank, are the exception, priced inside
// their page at the access.
constexpr std::uint8_t regionCost(std::uint32_t address, bool fast2) noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);

  // Work RAM and the first waitstate region span whole banks and always run slow.
  if (bank >= 0x7E && bank <= 0x7F) return 8u;   // work RAM
  if (bank >= 0x40 && bank <= 0x7D) return 8u;   // the first LoROM region
  if (bank >= 0xC0) return fast2 ? 6u : 8u;      // the second LoROM region, under MEMSEL

  // System banks $00-$3F and $80-$BF share one layout below $8000, then LoROM.
  if (offset <= 0x1FFF) return 8u;               // work-RAM mirror
  if (offset <= 0x3FFF) return 6u;               // registers and unused pages, fast
  if (offset <= 0x41FF) return 12u;              // the manual joypad ports, extra slow
  if (offset <= 0x5FFF) return 6u;               // more registers, fast
  if (offset <= 0x7FFF) return 8u;               // the expansion region, slow
  return (bank >= 0x80) ? (fast2 ? 6u : 8u)      // $80-$BF LoROM follows MEMSEL
                        : 8u;                     // $00-$3F LoROM is always slow
}

// Whether the page starting at `address` is a system bank's $4000-$5FFF, where
// the first 512 bytes cost 12 and the rest 6: the one page a single cost cannot
// price. `address` is a page's first address.
constexpr bool joypadPage(std::uint32_t address) noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  return (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset == 0x4000u;
}

static_assert(regionCost(0x7E0000u, false) == 8u, "work RAM costs 8");
static_assert(regionCost(0x7F0000u, true) == 8u, "work RAM costs 8 under MEMSEL");
static_assert(regionCost(0x400000u, true) == 8u, "bank $40 costs 8 under MEMSEL");
static_assert(regionCost(0x7D8000u, true) == 8u, "bank $7D costs 8 under MEMSEL");
static_assert(regionCost(0xC00000u, false) == 8u, "bank $C0 costs 8 with MEMSEL clear");
static_assert(regionCost(0xC00000u, true) == 6u, "bank $C0 costs 6 under MEMSEL");
static_assert(regionCost(0xFF8000u, true) == 6u, "bank $FF costs 6 under MEMSEL");
static_assert(regionCost(0x000000u, true) == 8u, "the work-RAM mirror costs 8");
static_assert(regionCost(0x801FFFu, true) == 8u, "the mirror's last byte in bank $80 costs 8");
static_assert(regionCost(0x002000u, false) == 6u, "the registers at $2000 cost 6");
static_assert(regionCost(0x803FFFu, false) == 6u, "the registers' last byte below $4000 costs 6");
static_assert(regionCost(0x004000u, false) == 12u, "the manual joypad ports cost 12");
static_assert(regionCost(0x0041FFu, true) == 12u, "the joypad ports' last byte costs 12");
static_assert(regionCost(0xBF4000u, false) == 12u, "the joypad ports in bank $BF cost 12");
static_assert(regionCost(0x004200u, false) == 6u, "the registers at $4200 cost 6");
static_assert(regionCost(0x005FFFu, false) == 6u, "the registers' last byte below $6000 costs 6");
static_assert(regionCost(0x006000u, true) == 8u, "the expansion region costs 8");
static_assert(regionCost(0x807FFFu, true) == 8u, "the expansion region's last byte in bank $80 costs 8");
static_assert(regionCost(0x008000u, true) == 8u, "$00:8000 costs 8 under MEMSEL");
static_assert(regionCost(0x3FFFFFu, true) == 8u, "$3F:FFFF costs 8 under MEMSEL");
static_assert(regionCost(0x808000u, false) == 8u, "$80:8000 costs 8 with MEMSEL clear");
static_assert(regionCost(0x808000u, true) == 6u, "$80:8000 costs 6 under MEMSEL");
static_assert(regionCost(0xBFFFFFu, true) == 6u, "$BF:FFFF costs 6 under MEMSEL");
static_assert(joypadPage(0x004000u), "bank $00's $4000 page holds the joypad ports");
static_assert(joypadPage(0x804000u), "bank $80's $4000 page holds the joypad ports");
static_assert(joypadPage(0xBF4000u), "bank $BF's $4000 page holds the joypad ports");
static_assert(!joypadPage(0x404000u), "bank $40 is the cartridge's");
static_assert(!joypadPage(0x002000u), "the $2000 page is registers alone");
static_assert(!joypadPage(0x006000u), "the $6000 page is the expansion region");
static_assert(!joypadPage(0x004200u), "the predicate answers a page's first address");

}  // namespace

Snes::Snes(SnesConfig config)
    : apu_(&state_.apu),  // the audio machine runs in the snapshot's own storage, seeded at power-on
      rom_(config.rom.begin(), config.rom.end()),
      region_(config.region),
      apuThreaded_(config.apuThread),
      ppuThreaded_(config.ppuThread),
      ppuVerify_(config.ppuVerify) {
  gpuPpu_ = config.gpuPpu;
  // The board is the header's, with the map and the save the caller overrides.
  board_ = cartridgeBoard(rom_);
  if (config.map.has_value()) board_.map = *config.map;
  if (config.saveRamBytes.has_value()) board_.saveRamBytes = *config.saveRamBytes;
  if (board_.saveRamBytes > kMaxSaveRamBytes) board_.saveRamBytes = kMaxSaveRamBytes;
  buildPages();  // from the image and the board, both now fixed
  state_.sram.assign(board_.saveRamBytes, 0u);
  const ApuRatio ratio = region_ == Region::Pal ? kPalApu : kNtscApu;
  apuNum_ = ratio.num;
  apuDen_ = ratio.den;
  if (config.iplStub) {
    // Seed the upload stub over the ready state: the audio CPU runs the handshake
    // and posts its own ready bytes, the way it does when the console powers on.
    // Left off, the machine keeps the ready state, which is how a program loaded
    // straight into audio RAM skips the handshake.
    // A supplied boot ROM takes the stub's place, and the audio unit runs it.
    const std::span<const std::uint8_t, kIplWindowBytes> image =
        config.bootRom.has_value() ? std::span<const std::uint8_t, kIplWindowBytes>(*config.bootRom)
                                   : std::span<const std::uint8_t, kIplWindowBytes>(iplStubImage());
    seedIplStub(state_.apu, image);
    bootsAudio_ = true;
    // Map the same image over the $FFC0 window. The upload shell scratch-writes
    // that range in RAM and re-enters it expecting the boot code to read back
    // unchanged; the mapping serves the image to the CPU while CONTROL bit 7 is set,
    // so those reads survive the driver's writes to the RAM beneath.
    apu_.mapIplRom(image);
  }
  state_.cpu = powerOnCpu();  // emulation mode, the program counter at the reset vector
  // The picture thread's link, ahead of the load that sets its view: the default
  // machine holds none.
  if (ppuThreaded_) picture_ = std::make_unique<PictureLink>();
  // The record of what each picture line read and produced, which every machine
  // keeps and the load below empties.
  cache_ = std::make_unique<LineCache>();
  // The GPU PPU: the device is given the shader in a format it takes, and the
  // timeline is recorded for it from the first frame.
  if (gpuPpu_ != nullptr) {
    for (const gpu::Pass pass : {gpu::Pass::Sprites, gpu::Pass::Tiles, gpu::Pass::Pixels}) {
      const gpu::ShaderVariants& variants = gpu::ppuShader(pass);
      const std::optional<std::pair<gpu::ShaderBytecode, gpu::ShaderFormat>> shader =
          gpu::selectShader(gpuPpu_->formats(), variants);
      if (!shader.has_value()) {
        throw std::runtime_error(variants.spirv.data == nullptr && variants.dxil.data == nullptr &&
                                         variants.metallib.data == nullptr
                                     ? "the GPU PPU: this library was built without its shader"
                                     : "the GPU PPU: the device takes no format the shader was built in");
      }
      std::string error;
      if (!gpuPpu_->makePipeline(pass, shader->first, shader->second, error)) {
        throw std::runtime_error("the GPU PPU: the device did not make a pipeline: " + error);
      }
    }
    timeline_ = std::make_unique<TimelineRecorder>();
    recordsTimeline_ = true;
  }
  load();
}

Snes::Snes(Snes&& moved) noexcept
    // The moved machine's threads hold that machine's address, so they end before
    // anything leaves; this machine starts its own at its first call.
    : cpu_((moved.stopThreads(), std::move(moved.cpu_))),
      state_(std::move(moved.state_)),
      apu_(std::move(moved.apu_), &state_.apu),  // the audio machine follows its state here
      rom_(std::move(moved.rom_)),
      region_(moved.region_),
      board_(moved.board_),
      pages_(moved.pages_),
      bootsAudio_(moved.bootsAudio_),
      apuThreaded_(moved.apuThreaded_),
      ppuThreaded_(moved.ppuThreaded_),
      ppuVerify_(moved.ppuVerify_),
      apuNum_(moved.apuNum_),
      apuDen_(moved.apuDen_),
      lastCost_(moved.lastCost_),
      videoAdvanced_(moved.videoAdvanced_),
      timerHPoint_(moved.timerHPoint_),
      timerHPointOnVLine_(moved.timerHPointOnVLine_),
      timerZeroOnVLine_(moved.timerZeroOnVLine_),
      insideCycle_(moved.insideCycle_),
      observer_(moved.observer_),
      portLanding_(moved.portLanding_),
      frameObserver_(moved.frameObserver_),
      raster_(std::move(moved.raster_)),
      derived_(std::move(moved.derived_)),
      lineEventsFrom_(moved.lineEventsFrom_),
      frameFinished_(moved.frameFinished_),
      timelineFinished_(moved.timelineFinished_),
      timelineObserver_(moved.timelineObserver_),
      gpuPpu_(moved.gpuPpu_),
      recordsTimeline_(moved.recordsTimeline_),
      frameWide_(moved.frameWide_),
      frameWidth_(moved.frameWidth_),
      framePictureLines_(moved.framePictureLines_),
      frameField_(moved.frameField_),
      saveObserver_(moved.saveObserver_),
      saveChanged_(moved.saveChanged_),
      saveFinished_(moved.saveFinished_),
      accessWatcher_(moved.accessWatcher_),
      armed_(std::move(moved.armed_)),
      instructionWatcher_(moved.instructionWatcher_),
      standins_(std::move(moved.standins_)),
      inFlight_(false),
      inFlightPad_{},
      link_(),
      picture_(std::move(moved.picture_)),
      cache_(std::move(moved.cache_)),
      timeline_(std::move(moved.timeline_)) {
  moved.observer_ = nullptr;
  moved.frameObserver_ = nullptr;
  moved.timelineObserver_ = nullptr;
  moved.gpuPpu_ = nullptr;
  moved.recordsTimeline_ = false;
  moved.saveObserver_ = nullptr;
  moved.accessWatcher_ = nullptr;
  moved.instructionWatcher_ = nullptr;
}

Snes::~Snes() { stopThreads(); }

void Snes::setFrameObserver(FrameObserver* observer) noexcept {
  frameObserver_ = observer;
}

void Snes::setPpuTimelineObserver(PpuTimelineObserver* observer) {
  // The buffers are made the first time an observer is set and kept from then on;
  // the frame in progress, recorded or not, is dropped. The machine records while an
  // observer is set or the GPU PPU is on.
  timelineObserver_ = observer;
  if (observer != nullptr && !timeline_) timeline_ = std::make_unique<TimelineRecorder>();
  recordsTimeline_ = observer != nullptr || gpuPpu_ != nullptr;
  if (timeline_) dropTimeline();
}

bool Snes::restore(const SnesState& state) {
  // A state whose save is another size is refused before anything is touched.
  if (state.sram.size() != state_.sram.size()) return false;
  // The audio machine's state and the picture are replaced along with the rest.
  waitApu();
  waitPicture();
  state_ = state;
  // The caller replaced the save window along with everything else, which changes
  // it as surely as a store does — and a save left disagreeing with the machine is
  // worse than one written again.
  saveChanged_ = true;
  load();
  // The pages the watches reach are worked out again from the state now in place.
  if (armed_ || standins_) refreshWatchPages();
  return true;
}

void Snes::load() {
  // Every register the picture path works its answers out from has just been
  // replaced, so none of those answers stands: the first dot drawn after this works
  // them out from what the caller supplied.
  derived_.dropAll();
  // The picture thread's view is the chip's state as it now stands, its answers
  // dropped the same way, and its line the beam's: the thread, idle here, draws on
  // from the beam's position.
  if (picture_) {
    PictureLink& picture = *picture_;
    picture.view = state_.ppu;
    picture.derived.dropAll();
    picture.in = pictureInputs();
    picture.drawn = state_.hpos;
  }
  // Every record of a picture line was made from a state this one replaces: the
  // next frame draws every line and records it afresh. The timeline of the frame in
  // progress is dropped with them and recorded again from the next line drawn from
  // its start.
  dropLines();
  if (timeline_) dropTimeline();
  lineEventsFrom_ = 0u;
  // No host's call is on the stack of the machine that begins here, whatever a
  // throw from one left behind.
  insideCycle_ = false;
  cpu_.restore(state_.cpu);
  apu_.reload();  // its state is state_.apu, written in place by the restore or the seeding above
  // The NMI pin's remembered level is not part of the snapshot, so re-derive it from
  // the flags and enables and sync it WITHOUT minting an edge: the pending latch, if
  // one was in flight, rides the snapshot on its own. Driving it as a fresh assertion
  // would double a request the restore already carries. The IRQ line is level-only, so
  // setting it plainly is correct.
  cpu_.syncNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
}

void Snes::sync() {
  // The audio machine's state is already in state_.apu; only the live CPU's
  // register set has a copy to write back. A picture line pending on this thread
  // is drawn up to the beam, so the tile reads and the last decision a host reads
  // at the return are the beam's; on the threaded machine the flight's wait does
  // the same.
  state_.cpu = cpu_.state();
  if (!ppuThreaded_ && cache_->pending) {
    Ppu drawing{state_.ppu, derived_};
    settleLine(drawing, pictureInputs(), state_.hpos, false);
  }
}

std::uint16_t Snes::resetVector() const noexcept {
  // Bank $00's upper half is the cartridge under every map, so both bytes answer.
  return static_cast<std::uint16_t>(
      *peek(0x00FFFCu) | (static_cast<std::uint16_t>(*peek(0x00FFFDu)) << 8));
}

void Snes::reset() {
  // The audio machine is reset along with the rest, and the picture stands at the
  // beam before the beam moves.
  waitApu();
  waitPicture();
  // The CPU: the registers its reset leaves, at the cartridge's vector.
  state_.cpu = afterReset(state_.cpu, resetVector());

  // The 5A22's registers a reset initialises. The operands of the arithmetic unit,
  // its results, HTIME, VTIME and the eight channels' register files are not among
  // them and keep what they held.
  state_.nmitimen = 0u;
  state_.vblankNmi = false;
  state_.timeup = false;
  state_.wrio = 0xFFu;
  state_.memsel = 0u;
  state_.mdmaen = 0u;
  state_.hdmaen = 0u;
  state_.joyStrobe = false;
  state_.joy = {};
  state_.wmadd = 0u;  // the work-RAM chip's port address; its contents stand

  // Nothing the chip was part-way through survives it: a transfer, the HDMA frame,
  // an arithmetic job, an auto-read, a counter latch still owed.
  state_.dmaArm = 0u;
  state_.dmaRunning = false;
  state_.dmaOpened = false;
  state_.dmaChannelOpened = false;
  state_.dmaUnit = 0u;
  state_.dmaPauseMaster = 0u;
  state_.dmaResumePad = false;
  state_.hdmaActive = 0u;
  state_.hdmaEnded = 0u;
  state_.hdmaDoWrite = 0u;
  state_.hdmaInited = false;
  state_.hdmaLineFired = false;
  state_.hdmaRunPending = false;
  state_.hdmaIniting = false;
  state_.mathClocks = 0u;
  state_.mathOp = MathOp::None;
  state_.autoJoyStart = 0u;
  state_.autoJoyClocked = 0u;
  state_.counterLatchAt = 0u;

  // The machine starts again at H = 0, V = 0 with the parity clear, which is where
  // construction starts it, so the master counter — what every "since reset" grid
  // counts — begins again too, and the audio clock's share with it.
  state_.master = 0u;
  state_.consumed = 0u;
  state_.apuPhase = 0u;
  state_.hpos = 0u;
  state_.vpos = 0u;
  state_.field = 0u;
  state_.inVblank = false;
  state_.vblankBeginLine = 0u;
  state_.previousLineMaster = kLineMaster;
  state_.refreshAt = kFirstRefresh;
  state_.refreshLeft = 0u;

  // The PPU: the screen forced blank at the brightness it had, and the line begun
  // the way the machine begins any first line. Every other register and the three
  // memories keep what they held.
  state_.ppu.inidisp |= 0x80u;
  Ppu ppu{state_.ppu, derived_};
  ppu.beginRange(1u);
  ppu.beginLine(0u);
  frameWide_ = false;  // the picture the beam was part-way down is not finished

  // The audio machine, and the boot program again when the machine runs one.
  apu_.reset();
  if (bootsAudio_) enterIplStub(state_.apu);
  load();
}

Cpu65816State Snes::powerOnCpu() const {
  return Cpu65816State{
      .pc = resetVector(),
      .s = 0x01FF,
      .p = static_cast<std::uint8_t>(kCpuFlagM | kCpuFlagX | kCpuFlagI),
      .e = true,
      .run = CpuRunState::Running,
  };
}

void Snes::closeCycle() {
  // What every cycle ends with, whatever it was for: the timer's crossing settles
  // under the mode the cycle leaves behind, the interrupt lines take their levels from
  // the flags and enables as they now stand — so a register write this cycle is
  // settled for the next fetch to sample — and the master counter advances, paying the
  // audio machine the share of it that its own crystal owes. The share runs here, or,
  // on a machine built with the audio thread, joins the count the console publishes
  // to that thread, which runs it beside the console. The share is counted by
  // subtraction: a cycle owes at most one audio cycle, and an HDMA event, one cycle
  // costing the whole event, a handful.
  settleTimer();
  cpu_.setNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
  state_.master += lastCost_;
  state_.apuPhase += lastCost_ * apuNum_;
  std::uint64_t apuCycles = 0;
  while (state_.apuPhase >= apuDen_) {
    state_.apuPhase -= apuDen_;
    ++apuCycles;
  }
  if (apuCycles != 0) {
    if (apuThreaded_) [[unlikely]] {
      link_.pending += apuCycles;
      if (link_.pending >= kApuBatch) [[unlikely]] publishApu();
    } else {
      apu_.run(apuCycles);
    }
  }
}

void Snes::refreshCycle() {
  // The memory refresh, with the CPU held off the bus. Time passes as it does in any
  // cycle — the beam moves and the audio machine is paced — but no access is made and
  // the observer is told nothing: a pause is not a cycle the CPU spent, any more than
  // a transfer's overhead is. It is spent a fast cycle at a time, so a run may stop
  // inside one and carry the rest of it in the state.
  lastCost_ = state_.refreshLeft < kRefreshStep ? state_.refreshLeft : kRefreshStep;
  state_.refreshLeft = static_cast<std::uint8_t>(state_.refreshLeft - lastCost_);
  tickVideo(lastCost_);
  closeCycle();
}

void Snes::machineCycle() {
  // Every host callback the cycle reaches — a watcher's, the bus observer's, the
  // audio machine's — runs inside it, and a call into the guest made from one is
  // refused. On a machine built with the audio thread, the audio machine's observer
  // is told on that thread instead, and a call from it is refused by the thread's
  // identity. The instruction watch runs between
  // instructions and the frame, timeline and save reports after the cycle has closed, and both
  // are outside it.
  insideCycle_ = true;
  if (state_.refreshLeft != 0u) {
    refreshCycle();
    insideCycle_ = false;
    deliverFinished();
    return;
  }

  // A bus access overwrites this with its region's cost; a halted cycle makes no
  // access and keeps the fast rate, the same rate an internal cycle charges.
  lastCost_ = 6;
  videoAdvanced_ = false;

  // The bus has a priority order: HDMA outranks a general-purpose DMA, which
  // outranks the CPU. HDMA runs a whole event in this one cycle; a general-purpose
  // DMA runs a single byte or overhead cycle; otherwise the CPU steps, and a DMA
  // armed by a recent $420B write engages after this one more CPU cycle.
  if (state_.hdmaRunPending) {
    hdmaCycle();
    state_.hdmaRunPending = false;
  } else if (state_.dmaRunning) {
    dmaCycle();
  } else {
    const bool armedAtStart = state_.dmaArm != 0u;
    // The instruction watch, at a boundary the CPU takes as one; the pointer is
    // the "is anything armed" test, and an unarmed machine pays it alone.
    if (standins_ != nullptr) {
      // And the page's pointer, then the byte's code, are the "is this armed"
      // tests, made here so that only an armed instruction, or a page that
      // needs classifying, makes a call.
      const Cpu65816State& cpu = cpu_.state();
      const std::uint32_t at = (static_cast<std::uint32_t>(cpu.pbr) << 16) | cpu.pc;
      const std::uint8_t* codes = standins_->page[(at >> 13) & (kPages - 1u)];
      const std::uint32_t in = at & (kPageBytes - 1u);
      if (codes != nullptr &&
          (codes == &kSlowRun || ((codes[in >> 2] >> ((in & 3u) * 2u)) & 3u) != 0u)) {
        insideCycle_ = false;
        tellInstruction();
        insideCycle_ = true;
      }
    }
    Bus bus{*this};
    cpu_.stepCycle(bus);
    if (armedAtStart && --state_.dmaArm == 0u && state_.mdmaen != 0u) {
      state_.dmaRunning = true;
      state_.dmaOpened = false;
      state_.dmaChannelOpened = false;
      state_.dmaUnit = 0u;
      state_.dmaPauseMaster = state_.master + lastCost_;  // the pause is the end of this cycle
    }
  }

  // Every access ticks the machine's events before its value resolves; a halted
  // cycle makes no access, so tick it here — time still passes while the CPU sits.
  // A halted cycle can also be the first one after a transfer, so it too owes the
  // resume-rounding pad the bus callbacks apply on a live cycle.
  if (!videoAdvanced_) {
    if (state_.dmaResumePad) {
      lastCost_ += resumePad(lastCost_);
      state_.dmaResumePad = false;
    }
    tickVideo(lastCost_);
  }

  closeCycle();
  insideCycle_ = false;
  deliverFinished();
}

void Snes::deliverReports() {
  // A picture the beam finished this cycle, and the save window that frame
  // changed, go to whoever is watching once the cycle has closed: the master
  // counter advanced, the audio machine paid, the interrupt lines settled. The
  // register file is written back first, so state() inside a report — or after
  // one that threw — is the machine at this cycle's end. Each flag clears before
  // its report, so a report that throws is not made again, and the one after it
  // is made at the end of the next cycle.
  sync();
  if (frameFinished_) {
    frameFinished_ = false;
    deliverFrame();
  }
  if (timelineFinished_) {
    timelineFinished_ = false;
    deliverTimeline();
  }
  if (saveFinished_) {
    saveFinished_ = false;
    deliverSave();
  }
}

std::uint32_t Snes::step() {
  // A machine built with either thread runs the call inside a flight, entered here
  // and the call made again from inside it; the default machine runs it as it stands.
  if ((apuThreaded_ || ppuThreaded_) && !inFlight_.load(std::memory_order_relaxed)) [[unlikely]] {
    return stepInFlight();
  }
  const std::uint64_t before = state_.master;
  // A refresh the last call left part-way through is spent first: it is not an
  // instruction, and the instruction this call owes is the one after it.
  while (state_.refreshLeft != 0u) machineCycle();
  if (cpu_.state().run != CpuRunState::Running) {
    machineCycle();  // one idle cycle, which may be the one that ends a wait
  } else {
    do {
      machineCycle();
    } while (!cpu_.atInstructionBoundary());
  }
  state_.consumed = state_.master;  // an instruction lands on a cycle boundary, carrying nothing
  sync();
  return static_cast<std::uint32_t>(state_.master - before);
}

std::uint64_t Snes::run(std::uint64_t budget) {
  // A flight on a machine built with either thread, as step() enters one.
  if ((apuThreaded_ || ppuThreaded_) && !inFlight_.load(std::memory_order_relaxed)) [[unlikely]] {
    return runInFlight(budget);
  }
  state_.consumed += budget;
  while (state_.master < state_.consumed) machineCycle();
  sync();
  return budget;
}

std::vector<StereoFrame> Snes::takeFrames() {
  waitApu();
  return apu_.takeFrames();
}

std::uint32_t Snes::accessCost(std::uint32_t address) const noexcept {
  const Page& page = pages_[(address >> 13) & (kPages - 1u)];
  const std::uint32_t cost = page.cost[state_.memsel & 1u];  // MEMSEL's bit 0 alone
  if (cost != 0u) [[likely]] return cost;
  // The joypad page: the manual ports in its first 512 bytes, registers after.
  return (address & (kPageBytes - 1u)) < 0x200u ? 12u : 6u;
}

Snes::Page Snes::imagePage(const CartridgeBoard& board, std::uint32_t address) const noexcept {
  // The cost stays zero: buildPages copies a result's kind and base into the
  // table, never its cost.
  Page page{.kind = PageKind::RomEmpty,
            .fallback = PageKind::OpenBus,
            .cost = {std::uint8_t{0}, std::uint8_t{0}},
            .base = 0u,
            .fallbackBase = 0u};
  if (rom_.empty()) return page;
  if (rom_.size() % kPageBytes != 0u) {
    page.kind = PageKind::RomSlow;
    return page;
  }
  // The image repeats chip by chip, and every chip of an image this size is at
  // least a page, so no repeat begins inside a page: the offset of the page's
  // first byte carries the page.
  page.kind = PageKind::Rom;
  page.base = static_cast<std::uint32_t>(*romOffset(board, address, rom_.size()));
  return page;
}

void Snes::buildPages() {
  // The table does not say whether there is a save: resolve reads that at each
  // access. So the window's pages are built for the board with a save — any
  // size, the functions ask only whether there is one — and each carries as its
  // fallback what the board answers there with none.
  CartridgeBoard windowed = board_;
  windowed.saveRamBytes = kMaxSaveRamBytes;
  CartridgeBoard bare = board_;
  bare.saveRamBytes = 0;
  for (std::size_t p = 0; p < pages_.size(); ++p) {
    const std::uint32_t address = static_cast<std::uint32_t>(p * kPageBytes);
    const std::uint8_t bank = static_cast<std::uint8_t>(address >> 16);
    const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
    Page& page = pages_[p];
    page = Page{.kind = PageKind::OpenBus,
                .fallback = PageKind::OpenBus,
                .cost = {regionCost(address, false), regionCost(address, true)},
                .base = 0u,
                .fallbackBase = 0u};
    if (joypadPage(address)) page.cost = {std::uint8_t{0}, std::uint8_t{0}};
    if (bank >= 0x7E && bank <= 0x7F) {
      page.kind = PageKind::WorkRam;
      page.base = (static_cast<std::uint32_t>(bank - 0x7E) << 16) | offset;
      continue;
    }
    const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
    if (systemBank && offset <= 0x1FFFu) {
      page.kind = PageKind::WorkRam;  // the first 8 KB, mirrored into every system bank
      page.base = offset;
      continue;
    }
    if (systemBank && offset < 0x6000u) {
      page.kind = PageKind::System;  // the register windows, which the access dispatches on
      continue;
    }
    // The rest is the cartridge's, laid out as the cartridge functions say.
    switch (cartridgeRegion(windowed, address)) {
      case CartridgeRegion::SaveRam:
        page.kind = PageKind::SaveWindow;
        page.base = static_cast<std::uint32_t>(*saveRamOffset(windowed, address));
        // With no save the window is the board's: on a plain LoROM board the
        // lower half reads what the upper half reads, on a coprocessor's board
        // it is the chip's, and HiROM's and ExHiROM's windows sit in the
        // expansion area — the last two read open bus, the fallback's default.
        if (cartridgeRegion(bare, address) == CartridgeRegion::Rom) {
          const Page upper = imagePage(bare, address);
          page.fallback = upper.kind;
          page.fallbackBase = upper.base;
        }
        break;
      case CartridgeRegion::Rom: {
        const Page image = imagePage(board_, address);
        page.kind = image.kind;
        page.base = image.base;
        break;
      }
      case CartridgeRegion::Coprocessor:  // the chip's half; the machine carries no chip, so it answers as an absent chip's ports do, with open bus
      case CartridgeRegion::System:       // a system bank's expansion pages: nothing decodes them
      case CartridgeRegion::WorkRam:      // answered above
        break;
    }
  }
}

std::optional<std::uint8_t> Snes::peek(std::uint32_t address) const noexcept {
  const Resolved r = resolve(address);
  const std::uint32_t in = r.address & (kPageBytes - 1u);
  switch (r.kind) {
    case PageKind::WorkRam: return state_.wram[r.base + in];
    case PageKind::SaveWindow: return state_.sram[(r.base + in) % state_.sram.size()];
    case PageKind::Rom: return rom_[r.base + in];
    case PageKind::RomSlow: return rom_[*romOffset(board_, r.address, rom_.size())];
    case PageKind::RomEmpty: return std::uint8_t{0};
    case PageKind::System:   // a register
    case PageKind::OpenBus:  // an address the cartridge leaves open
      return std::nullopt;
  }
  return std::nullopt;
}

bool Snes::poke(std::uint32_t address, std::uint8_t value) noexcept {
  const Resolved r = resolve(address);
  const std::uint32_t in = r.address & (kPageBytes - 1u);
  switch (r.kind) {
    case PageKind::WorkRam:
      state_.wram[r.base + in] = value;
      return true;
    case PageKind::SaveWindow:
      // The host's own write to the save; not a program store, so it is not reported.
      state_.sram[(r.base + in) % state_.sram.size()] = value;
      return true;
    // A write to ROM changes the machine's copy of the image, mirrored the way a
    // read finds it, and never a file.
    case PageKind::Rom:
      rom_[r.base + in] = value;
      return true;
    case PageKind::RomSlow:
      rom_[*romOffset(board_, r.address, rom_.size())] = value;
      return true;
    case PageKind::RomEmpty:  // no byte to change
    case PageKind::System:
    case PageKind::OpenBus:
      return false;
  }
  return false;
}

bool Snes::addressable(std::uint32_t address, std::size_t bytes) const noexcept {
  for (std::size_t i = 0; i < bytes; ++i) {
    if (!peek((address + static_cast<std::uint32_t>(i)) & 0xFFFFFFu).has_value()) return false;
  }
  return true;
}

// The three video memories are written on the picture thread's view too, the thread
// idle for the write: between calls it has drawn to the beam already, and the wait
// is one look.
void Snes::writeVram(std::uint16_t address, std::uint8_t value) noexcept {
  waitPicture();
  // A page whose byte changes takes the count, as a write through the port would
  // give it: a line that read the page is drawn again in the next frame.
  if (state_.ppu.vram[address] != value) cache_->vramStamp[address >> 10] = ++cache_->writes;
  state_.ppu.vram[address] = value;  // 64 KB: every 16-bit address is in range
  if (picture_) picture_->view.vram[address] = value;
}

void Snes::writeCgram(std::uint16_t address, std::uint8_t value) noexcept {
  if (address >= state_.ppu.cgram.size()) return;
  waitPicture();
  if (state_.ppu.cgram[address] != value) {
    cache_->cgramStamp[address >> 5] = ++cache_->writes;  // the byte's word is address / 2, its group that / 16
  }
  state_.ppu.cgram[address] = value;
  if (picture_) picture_->view.cgram[address] = value;
}

void Snes::writeOam(std::uint16_t address, std::uint8_t value) noexcept {
  if (address >= state_.ppu.oam.size()) return;
  waitPicture();
  state_.ppu.oam[address] = value;
  if (picture_) picture_->view.oam[address] = value;
}

void Snes::writeApuRam(std::uint16_t address, std::uint8_t value) noexcept {
  waitApu();
  apu_.writeRam(address, value);
}

void Snes::writeApuPort(std::uint8_t index, std::uint8_t value) noexcept {
  waitApu();
  apu_.writePort(static_cast<std::uint8_t>(index & 3u), value);
}

void Snes::writeApuDspRegister(std::uint8_t index, std::uint8_t value) noexcept {
  waitApu();
  apu_.writeDspRegister(index, value);
}

void Snes::setCpuState(const Cpu65816State& state) {
  state_.cpu = state;
  // The CPU's part of load(): the live core reloaded, the interrupt lines
  // re-derived. Nothing else changed, so the picture's derived answers, the
  // audio machine's slot and its queued frames all stand.
  cpu_.restore(state_.cpu);
  cpu_.syncNmiLine((state_.nmitimen & 0x80u) != 0u && state_.vblankNmi);
  cpu_.setIrqLine(state_.timeup);
}

std::size_t Snes::takeFrames(std::span<StereoFrame> into) noexcept {
  waitApu();
  return apu_.takeFrames(into);
}

std::uint8_t Snes::busRead(std::uint32_t address, CycleKind kind) {
  const std::uint32_t cost = accessCost(address);
  lastCost_ = cost;
  if (state_.dmaResumePad) {  // the first cycle after a transfer pays the resume-rounding pad
    lastCost_ += resumePad(cost);
    state_.dmaResumePad = false;
  }
  tickVideo(lastCost_);  // tick-first: the read sees the event it shares the cycle with
  videoAdvanced_ = true;
  // The live core's cycle index is the access's cycle: the fetch runs at 0 and
  // each later cycle at its own index, the index moving on after the cycle.
  const std::uint8_t cycle = cpu_.state().tcu;
  // With nothing armed the access is the bus alone, and this test is all it
  // pays; the two pointers are the "is anything armed" tests.
  if (armed_ == nullptr && standins_ == nullptr) return routeReadRaw(address, cycle);
  return readWithHost(address, kind, cycle);
}

std::uint8_t Snes::readWithHost(std::uint32_t address, CycleKind kind, std::uint8_t cycle) {
  std::uint8_t value = routeReadRaw(address, cycle);
  // The return standing in for the instruction the watcher was told about
  // answers the fetch that begins it, on the data bus like any fetched byte;
  // any opcode fetch clears what is queued.
  if (kind == CycleKind::OpcodeFetch && standins_ != nullptr && standins_->pendingOpcode != 0u) {
    if (address == standins_->pendingAddress) value = latch(standins_->pendingOpcode);
    standins_->pendingOpcode = 0u;
  }
  // The watch's tests, made here so that only an armed byte, or a page that
  // needs classifying, makes a call: the page's pointer, then the byte's bit.
  if (armed_ == nullptr) return value;
  const std::uint8_t* bits = armed_->pageRead[(address >> 13) & (kPages - 1u)];
  if (bits == nullptr) return value;
  const std::uint32_t in = address & (kPageBytes - 1u);
  if (bits != &kSlowRun && (bits[in >> 3] & (1u << (in & 7u))) == 0u) return value;
  return watchRead(address, value, AccessSource::Cpu, kind, cycle);
}

void Snes::busWrite(std::uint32_t address, std::uint8_t value, CycleKind kind) {
  const std::uint32_t cost = accessCost(address);
  lastCost_ = cost;
  if (state_.dmaResumePad) {
    lastCost_ += resumePad(cost);
    state_.dmaResumePad = false;
  }
  const std::uint8_t busBefore = state_.mdr;  // the byte the bus held before this write
  tickVideo(lastCost_);  // tick-first, so a write lands after the event it shares the cycle with
  videoAdvanced_ = true;
  const std::uint8_t cycle = cpu_.state().tcu;
  // With nothing armed the access is the bus alone, and this test is all it
  // pays; with something armed, the page's pointer and then the byte's bit are
  // the tests, and a byte not armed for writes takes the bus alone too.
  bool watched = false;
  if (armed_ != nullptr) {
    const std::uint8_t* bits = armed_->pageWrite[(address >> 13) & (kPages - 1u)];
    const std::uint32_t in = address & (kPageBytes - 1u);
    watched = bits != nullptr && (bits == &kSlowRun || (bits[in >> 3] & (1u << (in & 7u))) != 0u);
  }
  if (watched) {
    routeWrite(address, value, AccessSource::Cpu, kind, cycle);
  } else {
    routeWriteRaw(address, value, cycle);
  }
  redrawInidispEarly(address, busBefore);
}

std::uint8_t Snes::routeRead(std::uint32_t address, AccessSource source, CycleKind kind,
                             std::uint8_t cycle) {
  return watchRead(address, routeReadRaw(address, cycle), source, kind, cycle);
}

void Snes::routeWrite(std::uint32_t address, std::uint8_t value, AccessSource source,
                      CycleKind kind, std::uint8_t cycle) {
  const std::optional<std::uint8_t> stored = watchWrite(address, value, source, kind, cycle);
  if (stored.has_value()) routeWriteRaw(address, *stored, cycle);  // a vetoed write stores nothing
}

std::uint8_t Snes::routeReadRaw(std::uint32_t address, std::uint8_t cycle) {
  const Resolved r = resolve(address);
  const std::uint32_t in = r.address & (kPageBytes - 1u);
  switch (r.kind) {
    case PageKind::WorkRam: return latch(state_.wram[r.base + in]);
    case PageKind::System: {
      const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
      if (offset >= 0x2100 && offset <= 0x213F) {
        // A byte the PPU drove goes onto the data bus; a register with nothing to
        // say leaves the bus as it was, which is what the read returns. The picture
        // thread is handed the read too: what a read moves, a write can land on.
        const PpuInputs pins = ppuInputs();
        const std::optional<std::uint8_t> v = Ppu{state_.ppu, derived_}.read(offset, pins);
        if (ppuThreaded_) [[unlikely]] pushAccess(PictureEventKind::Read, offset, 0u, pins);
        return v.has_value() ? latch(*v) : state_.mdr;
      }
      if (offset >= 0x2140 && offset <= 0x217F) {
        // The latch as the audio machine has it at this cycle: every cycle the
        // console released before this one run, every write before this one applied.
        if (apuThreaded_) [[unlikely]] waitApu();
        return latch(apu_.readPort(static_cast<std::uint8_t>(offset & 3u)));
      }
      if (offset >= 0x2180 && offset <= 0x2183) return readWramPort(offset, cycle);
      if (offset == 0x4016 || offset == 0x4017) return readJoypadPort(offset);
      if (offset >= 0x4200 && offset <= 0x421F) return readCpuReg(offset);
      if (offset >= 0x4300 && offset <= 0x437F) return readDmaReg(offset);
      return state_.mdr;  // between the windows nothing answers, and the bus keeps its byte
    }
    case PageKind::SaveWindow: return latch(state_.sram[(r.base + in) % state_.sram.size()]);
    case PageKind::Rom: return latch(rom_[r.base + in]);
    case PageKind::RomSlow: return latch(rom_[*romOffset(board_, r.address, rom_.size())]);
    case PageKind::RomEmpty: return latch(0u);
    case PageKind::OpenBus: return state_.mdr;  // an unmapped read returns the last value the data bus carried
  }
  return state_.mdr;
}

void Snes::routeWriteRaw(std::uint32_t address, std::uint8_t value, std::uint8_t cycle) {
  state_.mdr = value;  // a write drives the data bus
  // The page itself, not what a save window falls back to: with no save the
  // window is the image or open bus, and a write changes neither.
  const Page& page = pages_[(address >> 13) & (kPages - 1u)];
  const std::uint32_t in = address & (kPageBytes - 1u);
  switch (page.kind) {
    case PageKind::WorkRam:
      state_.wram[page.base + in] = value;
      return;
    case PageKind::System: {
      const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
      if (offset >= 0x2100 && offset <= 0x213F) {
        // $2133's interlace bit sets the line's length and the frame's, which the
        // timer's dot 153 is dated from. The picture thread is handed the write at
        // the position the beam holds here, every dot up to it drawn.
        if (offset == 0x2133u) lineEventsFrom_ = 0u;
        const PpuInputs pins = ppuInputs();
        // A write inside the line's picture dots to something they read is what the
        // line being drawn here depends on; the picture thread marks its own as it
        // replays the write.
        Ppu chip{state_.ppu, derived_};
        bool changes = false;
        if (!ppuThreaded_) {
          // A write the line's remaining dots read draws a pending line up to here
          // first, from the state the write has not yet changed.
          if (writeDisturbs(offset, state_.hpos, state_.ppu, pins)) {
            settleLine(chip, pictureInputs(), state_.hpos, true);
          }
          changes = videoWriteChanges(chip, offset, value);
          if (recordsTimeline_) [[unlikely]] recordBeforeWrite(offset, state_.ppu, pins);
        }
        portLanding_ = chip.write(offset, value, pins);
        // The drawing side counts the write where it landed, and records it in the
        // timeline: this thread when the picture is drawn here, the picture thread as
        // it replays the write.
        if (ppuThreaded_) [[unlikely]] {
          pushAccess(PictureEventKind::Write, offset, value, pins);
        } else {
          noteVideoWrite(offset, portLanding_, changes);
          if (recordsTimeline_) [[unlikely]] {
            recordWrite(offset, value, state_.hpos, state_.ppu, pins, portLanding_);
          }
        }
        return;
      }
      if (offset >= 0x2140 && offset <= 0x217F) {
        // The write lands before the audio machine runs this cycle's share, on the
        // thread as here.
        if (apuThreaded_) {
          pushPortWrite(static_cast<std::uint8_t>(offset & 3u), value);
        } else {
          apu_.writePort(static_cast<std::uint8_t>(offset & 3u), value);
        }
        return;
      }
      if (offset >= 0x2180 && offset <= 0x2183) {
        writeWramPort(offset, value, cycle);
        return;
      }
      if (offset == 0x4016) {
        writeJoypadStrobe(value);
        return;
      }
      if (offset >= 0x4200 && offset <= 0x421F) {
        writeCpuReg(offset, value);
        return;
      }
      if (offset >= 0x4300 && offset <= 0x437F) {
        writeDmaReg(offset, value);
        return;
      }
      return;  // between the windows nothing is written
    }
    case PageKind::SaveWindow:
      if (state_.sram.empty()) return;
      state_.sram[(page.base + in) % state_.sram.size()] = value;
      // Every store into the save window arrives here, which is what lets the machine
      // say a frame changed it without comparing anything (SaveObserver, `snes.h`).
      saveChanged_ = true;
      return;
    // A write to ROM or to an unmapped address changes nothing beyond the data bus.
    case PageKind::Rom:
    case PageKind::RomSlow:
    case PageKind::RomEmpty:
    case PageKind::OpenBus:
      return;
  }
}

namespace {

// Whether a system-bank offset is one the bus routes to a register: the PPU's
// file and the audio ports ($2100-$217F), the work-RAM port ($2180-$2183), the
// serial controller ports ($4016-$4017), the CPU's registers ($4200-$421F) and
// the transfer engines' ($4300-$437F) — the windows routeReadRaw and
// routeWriteRaw dispatch on, and no others.
[[nodiscard]] constexpr bool registerOffset(std::uint16_t offset) noexcept {
  return (offset >= 0x2100u && offset <= 0x2183u) || offset == 0x4016u || offset == 0x4017u ||
         (offset >= 0x4200u && offset <= 0x421Fu) || (offset >= 0x4300u && offset <= 0x437Fu);
}

[[nodiscard]] constexpr std::uint8_t spaceBit(Snes::Space space) noexcept {
  return static_cast<std::uint8_t>(1u << static_cast<unsigned>(space));
}
constexpr std::uint8_t kEverySpace = 0x1Fu;

}  // namespace

std::optional<Snes::Physical> Snes::classify(std::uint32_t address,
                                             std::uint8_t spaces) const noexcept {
  const auto want = [spaces](Space space) { return (spaces & spaceBit(space)) != 0u; };
  // The page says which memory the byte is in, as the bus reads it (routeReadRaw);
  // the address it lands on is open bus when no memory answers.
  const Resolved r = resolve(address);
  const std::uint32_t in = r.address & (kPageBytes - 1u);
  switch (r.kind) {
    case PageKind::WorkRam:
      if (!want(Space::WorkRam)) return std::nullopt;
      return Physical{Space::WorkRam, r.base + in};
    case PageKind::System: {
      const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
      if (registerOffset(offset)) {
        if (!want(Space::Register)) return std::nullopt;
        return Physical{Space::Register, offset};
      }
      break;  // between the windows: open bus
    }
    case PageKind::SaveWindow:
      if (!want(Space::SaveRam)) return std::nullopt;
      return Physical{Space::SaveRam, static_cast<std::uint32_t>((r.base + in) % state_.sram.size())};
    case PageKind::Rom:
      if (!want(Space::CartridgeRom)) return std::nullopt;
      return Physical{Space::CartridgeRom, r.base + in};
    case PageKind::RomSlow:
      if (!want(Space::CartridgeRom)) return std::nullopt;
      return Physical{Space::CartridgeRom,
                      static_cast<std::uint32_t>(*romOffset(board_, r.address, rom_.size()))};
    case PageKind::RomEmpty:  // an empty image reads as zero at offset 0
      if (!want(Space::CartridgeRom)) return std::nullopt;
      return Physical{Space::CartridgeRom, 0u};
    case PageKind::OpenBus:
      break;
  }
  if (!want(Space::OpenBus)) return std::nullopt;
  return Physical{Space::OpenBus, address & 0xFFFFFFu};
}

Snes::Physical Snes::physical(std::uint32_t address) const noexcept {
  return *classify(address, kEverySpace);  // every space wanted, so every address classifies
}

std::optional<std::uint8_t> Snes::peekRegister(std::uint32_t address) const noexcept {
  const Physical place = physical(address);
  if (place.space != Space::Register) return std::nullopt;
  // The windows routeReadRaw dispatches on, each answered by the value its read
  // returns; a register that answers with nothing is the data bus's byte.
  const auto offset = static_cast<std::uint16_t>(place.index);
  if (offset >= 0x2100 && offset <= 0x213F) {
    return Ppu::registerValue(state_.ppu, offset, ppuInputs()).value_or(state_.mdr);
  }
  if (offset >= 0x2140 && offset <= 0x217F) {
    waitApu();
    return apu_.readPort(static_cast<std::uint8_t>(offset & 3u));
  }
  if (offset >= 0x2180 && offset <= 0x2183) return wramPortValue(offset);
  if (offset == 0x4016 || offset == 0x4017) return joypadPortValue(offset);
  if (offset >= 0x4200 && offset <= 0x421F) return cpuRegValue(offset);
  if (offset >= 0x4300 && offset <= 0x437F) return dmaRegValue(offset);
  return state_.mdr;
}

std::optional<std::size_t> Snes::armedSlot(Physical place) const noexcept {
  const std::size_t space = static_cast<std::size_t>(place.space);
  const std::uint32_t chunk = place.index >> 16;
  if (chunk >= armed_->chunks[space]) return std::nullopt;
  return armed_->base[space] + chunk;
}

bool Snes::armedThrough(std::uint32_t address,
                        const std::vector<std::unique_ptr<ArmedChunk>>& table) const noexcept {
  const std::optional<Physical> place = classify(address, armed_->spaces);
  if (!place.has_value()) return false;
  const std::optional<std::size_t> slot = armedSlot(*place);
  if (!slot.has_value()) return false;
  const ArmedChunk* chunk = table[*slot].get();
  return chunk != nullptr && chunk->has(place->index);
}

std::uint8_t Snes::watchRead(std::uint32_t address, std::uint8_t value, AccessSource source,
                             CycleKind kind, std::uint8_t cycle) {
  // The table pointer is the "is anything armed" test: null means nothing is
  // watched, whether or not a sink is set, and the access pays this one test.
  if (armed_ == nullptr || accessWatcher_ == nullptr) return value;
  const std::uint8_t* bits = armed_->pageRead[(address >> 13) & (kPages - 1u)];
  if (bits == nullptr) return value;  // nothing armed in what this page reaches
  if (bits != &kSlowRun) {
    const std::uint32_t in = address & (kPageBytes - 1u);
    if ((bits[in >> 3] & (1u << (in & 7u))) == 0u) return value;
  } else if (!armedThrough(address, armed_->read)) {
    return value;
  }
  return accessWatcher_->read(address, value, source, kind, cycle).applyToRead(value);
}

std::optional<std::uint8_t> Snes::watchWrite(std::uint32_t address, std::uint8_t value,
                                             AccessSource source, CycleKind kind,
                                             std::uint8_t cycle) {
  if (armed_ == nullptr || accessWatcher_ == nullptr) return value;
  const std::uint8_t* bits = armed_->pageWrite[(address >> 13) & (kPages - 1u)];
  if (bits == nullptr) return value;
  if (bits != &kSlowRun) {
    const std::uint32_t in = address & (kPageBytes - 1u);
    if ((bits[in >> 3] & (1u << (in & 7u))) == 0u) return value;
  } else if (!armedThrough(address, armed_->write)) {
    return value;
  }
  const AccessAnswer answer = accessWatcher_->write(address, value, source, kind, cycle);
  if (!answer.storesWrite()) return std::nullopt;  // veto
  return answer.applyToWrite(value);
}

Snes::PageReach Snes::pageRun(std::size_t page, PageRun& run) const noexcept {
  const Page& entry = pages_[page];
  PageKind kind = entry.kind;
  std::uint32_t base = entry.base;
  if (kind == PageKind::SaveWindow) {
    if (!state_.sram.empty()) {
      // A save of whole pages puts each window page on one run of itself; any
      // other size folds a page across runs, so each access is classified.
      const std::size_t size = state_.sram.size();
      run.space = Space::SaveRam;
      if (size % kPageBytes != 0u) return PageReach::Slow;
      run.start = static_cast<std::uint32_t>(base % size);
      return PageReach::Run;
    }
    kind = entry.fallback;
    base = entry.fallbackBase;
  }
  switch (kind) {
    case PageKind::WorkRam:
      run = PageRun{Space::WorkRam, base};
      return PageReach::Run;
    case PageKind::System:
      run = PageRun{Space::Register, static_cast<std::uint32_t>((page & 7u) << 13)};
      return PageReach::Run;
    case PageKind::Rom:
      run = PageRun{Space::CartridgeRom, base};
      return PageReach::Run;
    case PageKind::RomSlow:
      run.space = Space::CartridgeRom;
      return PageReach::Slow;
    case PageKind::RomEmpty:  // one byte, at offset 0, which no chunk holds
      return PageReach::None;
    case PageKind::OpenBus:
      run = PageRun{Space::OpenBus, static_cast<std::uint32_t>(page << 13)};
      return PageReach::Run;
    case PageKind::SaveWindow:  // resolved above
      break;
  }
  return PageReach::None;
}

void Snes::refreshWatchPages() {
  for (std::size_t page = 0; page < kPages; ++page) {
    PageRun run{Space::OpenBus, 0u};
    const PageReach reach = pageRun(page, run);
    const std::size_t space = static_cast<std::size_t>(run.space);
    const std::uint32_t chunkIndex = run.start >> 16;
    const std::size_t runInChunk = (run.start >> 13) & 7u;
    const bool system = pages_[page].kind == PageKind::System;
    // Between a system page's registers lies open bus, keyed by the address:
    // an open-bus byte of the page armed means the classification decides.
    const std::size_t openBusChunk = page >> 3;  // one chunk per bank
    if (armed_) {
      const auto bitsIn = [&](const std::vector<std::unique_ptr<ArmedChunk>>& table)
          -> const std::uint8_t* {
        if (reach == PageReach::None) return nullptr;
        if (reach == PageReach::Slow) return armed_->perSpace[space] != 0u ? &kSlowRun : nullptr;
        if (system) {
          const ArmedChunk* open =
              table[armed_->base[static_cast<std::size_t>(Space::OpenBus)] + openBusChunk].get();
          if (open != nullptr && open->runs[page & 7u] != 0u) return &kSlowRun;
        }
        if (chunkIndex >= armed_->chunks[space]) return nullptr;
        const ArmedChunk* chunk = table[armed_->base[space] + chunkIndex].get();
        if (chunk == nullptr || chunk->runs[runInChunk] == 0u) return nullptr;
        return &chunk->bits[(run.start & 0xFFFFu) >> 3];
      };
      armed_->pageRead[page] = bitsIn(armed_->read);
      armed_->pageWrite[page] = bitsIn(armed_->write);
    }
    if (standins_) {
      const auto codesIn = [&]() -> const std::uint8_t* {
        if (reach == PageReach::None) return nullptr;
        if (reach == PageReach::Slow) return standins_->perSpace[space] != 0u ? &kSlowRun : nullptr;
        if (system) {
          const StandinChunk* open =
              standins_->slots[standins_->base[static_cast<std::size_t>(Space::OpenBus)] + openBusChunk]
                  .get();
          if (open != nullptr && open->runs[page & 7u] != 0u) return &kSlowRun;
        }
        if (chunkIndex >= standins_->chunks[space]) return nullptr;
        const StandinChunk* chunk = standins_->slots[standins_->base[space] + chunkIndex].get();
        if (chunk == nullptr || chunk->runs[runInChunk] == 0u) return nullptr;
        return &chunk->codes[(run.start & 0xFFFFu) >> 2];
      };
      standins_->page[page] = codesIn();
    }
  }
}

void Snes::setArmed(Physical place, bool onRead, bool onWrite, bool arm) {
  const std::optional<std::size_t> slot = armedSlot(place);
  if (!slot.has_value()) return;  // a byte past its space's size has no bit to hold
  const std::size_t space = static_cast<std::size_t>(place.space);
  const std::uint16_t byteIndex = static_cast<std::uint16_t>((place.index & 0xFFFFu) >> 3);
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << (place.index & 7u));
  const std::size_t run = (place.index >> 13) & 7u;
  const auto touch = [&](std::vector<std::unique_ptr<ArmedChunk>>& chunks) {
    std::unique_ptr<ArmedChunk>& chunk = chunks[*slot];
    if (arm) {
      if (!chunk) chunk = std::make_unique<ArmedChunk>();
      if ((chunk->bits[byteIndex] & mask) == 0u) {  // idempotent: count only a newly-set bit
        chunk->bits[byteIndex] |= mask;
        ++chunk->runs[run];
        ++chunk->armed;
        ++armed_->perSpace[space];
        ++armed_->armed;
      }
    } else if (chunk && (chunk->bits[byteIndex] & mask) != 0u) {
      chunk->bits[byteIndex] &= static_cast<std::uint8_t>(~mask);
      --chunk->runs[run];
      --chunk->armed;
      --armed_->perSpace[space];
      --armed_->armed;
      if (chunk->armed == 0u) chunk.reset();  // free the chunk's 8 KB when its last bit clears
    }
  };
  if (onRead) touch(armed_->read);
  if (onWrite) touch(armed_->write);
  // The spaces with anything armed, which is how far an access is classified.
  std::uint8_t spaces = 0;
  for (std::size_t s = 0; s < armed_->perSpace.size(); ++s) {
    if (armed_->perSpace[s] != 0u) spaces |= static_cast<std::uint8_t>(1u << s);
  }
  armed_->spaces = spaces;
}

void Snes::watchAccess(std::uint32_t address, std::size_t bytes, bool onRead, bool onWrite) {
  if (bytes == 0 || (!onRead && !onWrite)) return;
  if (!armed_) {
    armed_ = std::make_unique<AccessArmedSet>();
    const std::uint32_t total = chunkLayout(armed_->base, armed_->chunks);
    armed_->read.resize(total);
    armed_->write.resize(total);
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    const std::uint32_t a = (address + static_cast<std::uint32_t>(i)) & 0xFFFFFFu;
    setArmed(physical(a), onRead, onWrite, true);
  }
  if (armed_->armed == 0u) {
    armed_.reset();  // nothing could be armed: back to one null pointer
  } else {
    refreshWatchPages();
  }
}

void Snes::unwatchAccess(std::uint32_t address, std::size_t bytes, bool onRead, bool onWrite) {
  if (!armed_ || bytes == 0 || (!onRead && !onWrite)) return;
  for (std::size_t i = 0; i < bytes; ++i) {
    const std::uint32_t a = (address + static_cast<std::uint32_t>(i)) & 0xFFFFFFu;
    setArmed(physical(a), onRead, onWrite, false);
  }
  if (armed_->armed == 0u) {
    armed_.reset();  // the last place disarmed: back to one null pointer
  } else {
    refreshWatchPages();
  }
}

std::uint32_t Snes::chunkLayout(std::array<std::uint32_t, 5>& base,
                                std::array<std::uint32_t, 5>& chunks) const noexcept {
  // The save's count is the save's size at this moment, so the slots exist for
  // every byte it holds.
  const auto chunksFor = [](std::size_t bytesHeld) {
    return static_cast<std::uint32_t>((bytesHeld + 0xFFFFu) >> 16);
  };
  const std::array<std::uint32_t, 5> counts = {2u, 1u, chunksFor(state_.sram.size()),
                                               chunksFor(rom_.size()), 256u};
  std::uint32_t total = 0;
  for (std::size_t s = 0; s < counts.size(); ++s) {
    base[s] = total;
    chunks[s] = counts[s];
    total += counts[s];
  }
  return total;
}

std::uint8_t Snes::standinAt(std::uint32_t address) const noexcept {
  const std::optional<Physical> place = classify(address, standins_->spaces);
  if (!place.has_value()) return 0u;
  const std::size_t space = static_cast<std::size_t>(place->space);
  const std::uint32_t chunk = place->index >> 16;
  if (chunk >= standins_->chunks[space]) return 0u;  // past the space's size
  const StandinChunk* slot = standins_->slots[standins_->base[space] + chunk].get();
  return slot == nullptr ? std::uint8_t{0} : slot->code(place->index);
}

namespace {

// The opcode a stand-in answers the fetch with: RTS for Near, RTL for Long,
// nothing for None or for a byte not armed.
[[nodiscard]] constexpr std::uint8_t standinOpcode(std::uint8_t code) noexcept {
  switch (code) {
    case 1u + static_cast<std::uint8_t>(Standin::Near): return 0x60u;
    case 1u + static_cast<std::uint8_t>(Standin::Long): return 0x6Bu;
    default: return 0u;
  }
}

}  // namespace

void Snes::tellInstruction() {
  const Cpu65816State& cpu = cpu_.state();
  if (cpu.run != CpuRunState::Running || cpu.tcu != 0u || cpu_.takesRequestNext()) return;
  const std::uint32_t address = (static_cast<std::uint32_t>(cpu.pbr) << 16) | cpu.pc;
  // The page's pointer is the test: null means nothing armed in what the page
  // reaches, the run's codes answer at the address's offset, and a page that
  // needs classifying is looked up in its chunk.
  const std::uint8_t* codes = standins_->page[(address >> 13) & (kPages - 1u)];
  if (codes == nullptr) return;
  if (codes != &kSlowRun) {
    const std::uint32_t in = address & (kPageBytes - 1u);
    if (((codes[in >> 2] >> ((in & 3u) * 2u)) & 3u) == 0u) return;
  } else if (standinAt(address) == 0u) {
    return;
  }
  state_.cpu = cpu;  // live for cpuState() inside the call
  if (instructionWatcher_ != nullptr) instructionWatcher_->reached(address);
  // What stands there once the host has answered — the call may have disarmed
  // it, moved the program counter, or run the machine — queued for the fetch.
  if (standins_ == nullptr) return;
  standins_->pendingOpcode = standinOpcode(standinAt(address));
  standins_->pendingAddress = address;
}

void Snes::watchInstruction(std::uint32_t address, Standin standin) {
  if (!standins_) {
    standins_ = std::make_unique<StandinSet>();
    standins_->slots.resize(chunkLayout(standins_->base, standins_->chunks));
  }
  const Physical place = physical(address & 0xFFFFFFu);
  const std::size_t space = static_cast<std::size_t>(place.space);
  const std::uint32_t chunk = place.index >> 16;
  if (chunk < standins_->chunks[space]) {  // a byte past its space's size has no slot
    std::unique_ptr<StandinChunk>& slot = standins_->slots[standins_->base[space] + chunk];
    if (!slot) slot = std::make_unique<StandinChunk>();
    if (slot->code(place.index) == 0u) {  // a newly-armed byte; re-arming only replaces the stand-in
      ++slot->runs[(place.index >> 13) & 7u];
      ++slot->armed;
      ++standins_->perSpace[space];
      ++standins_->armed;
      standins_->spaces |= static_cast<std::uint8_t>(1u << space);
    }
    slot->set(place.index, static_cast<std::uint8_t>(1u + static_cast<std::uint8_t>(standin)));
  }
  if (standins_->armed == 0u) {
    standins_.reset();  // nothing could be armed: back to one null pointer
  } else {
    refreshWatchPages();
  }
}

void Snes::unwatchInstruction(std::uint32_t address) {
  if (!standins_) return;
  const Physical place = physical(address & 0xFFFFFFu);
  const std::size_t space = static_cast<std::size_t>(place.space);
  const std::uint32_t chunk = place.index >> 16;
  if (chunk < standins_->chunks[space]) {
    std::unique_ptr<StandinChunk>& slot = standins_->slots[standins_->base[space] + chunk];
    if (slot && slot->code(place.index) != 0u) {
      slot->set(place.index, 0u);
      --slot->runs[(place.index >> 13) & 7u];
      --slot->armed;
      --standins_->perSpace[space];
      --standins_->armed;
      if (standins_->perSpace[space] == 0u)
        standins_->spaces &= static_cast<std::uint8_t>(~(1u << space));
      if (slot->armed == 0u) slot.reset();  // free the chunk's 16 KB when its last byte clears
    }
  }
  if (standins_->armed == 0u) {
    standins_.reset();  // the last place disarmed: back to one null pointer
  } else {
    refreshWatchPages();
  }
}

bool Snes::runCall(std::uint32_t entry, Cpu65816State file, Standin returns, std::size_t guard) {
  const std::uint8_t entryBank = static_cast<std::uint8_t>((entry >> 16) & 0xFFu);
  const std::uint16_t entryPc = static_cast<std::uint16_t>(entry & 0xFFFFu);
  // The landing: the program counter as it stands, in the entry's bank for a
  // near return — RTS keeps the bank — and the current bank for a long one,
  // which RTL takes off the stack. A return pulls the address and steps past
  // it, as JSR and JSL push the address of their own last byte.
  const std::uint16_t landingPc = file.pc;
  const std::uint8_t landingBank = returns == Standin::Long ? file.pbr : entryBank;
  const std::uint16_t stackBefore = file.s;
  const std::uint16_t back = static_cast<std::uint16_t>(landingPc - 1u);

  // The push, as the call instruction's own would move the stack pointer: a
  // near push stays inside page one in emulation mode, a long one may leave it
  // and settles back afterwards. Every byte lands at the pointer's address in
  // bank $00 before the pointer moves; each address is checked before any byte
  // is written, so a refused call has written nothing.
  std::array<std::uint8_t, 3> bytes{};
  std::array<std::uint16_t, 3> at{};
  std::size_t count = 0;
  std::uint16_t s = file.s;
  const auto push = [&](std::uint8_t value, bool leavesPage) {
    at[count] = s;
    bytes[count] = value;
    ++count;
    s = (file.e && !leavesPage) ? static_cast<std::uint16_t>(0x0100u | ((s - 1u) & 0xFFu))
                                : static_cast<std::uint16_t>(s - 1u);
  };
  if (returns == Standin::Long) {
    push(file.pbr, true);
    push(static_cast<std::uint8_t>(back >> 8), true);
    push(static_cast<std::uint8_t>(back), true);
    if (file.e) s = static_cast<std::uint16_t>(0x0100u | (s & 0xFFu));
  } else {
    push(static_cast<std::uint8_t>(back >> 8), false);
    push(static_cast<std::uint8_t>(back), false);
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (!addressable(at[i], 1)) return false;
  }
  for (std::size_t i = 0; i < count; ++i) poke(at[i], bytes[i]);

  file.s = s;
  file.pc = entryPc;
  file.pbr = entryBank;
  file.run = CpuRunState::Running;
  setCpuState(file);

  // The machine runs as it runs from step(): every cycle its own, the engines
  // and the refresh taking the bus when they hold it. An instruction ends at a
  // boundary a CPU cycle lands on — an idle cycle of a halted core lands on one
  // too — and the guard counts those.
  std::size_t left = guard;
  for (;;) {
    if (left == 0u) {
      sync();
      return false;
    }
    const bool cpuCycle =
        state_.refreshLeft == 0u && !state_.hdmaRunPending && !state_.dmaRunning;
    machineCycle();
    if (!cpuCycle || !cpu_.atInstructionBoundary()) continue;
    --left;
    const Cpu65816State& cpu = cpu_.state();
    if (cpu.s == stackBefore && cpu.pc == landingPc && cpu.pbr == landingBank) {
      sync();
      return true;
    }
  }
}

bool Snes::callInContext(std::uint32_t entry, Standin returns, std::size_t guard) {
  // The refusals come first and spend nothing. A machine a budget stopped inside
  // an instruction — or inside a refresh, a transfer or an interrupt sequence —
  // then runs to the boundary exactly as step() would, those cycles the guest's
  // own, out of the budget the host runs next; the file taken there is the one
  // the guest resumes at. A call from the audio thread is refused by the thread it
  // comes on, before anything the console's own thread writes is read.
  if (returns == Standin::None || onApuThread() || insideCycle_ ||
      !addressable(entry & 0xFFFFFFu, 1)) {
    return false;
  }
  const Flight flight{*this};
  while (!cpu_.atInstructionBoundary()) machineCycle();
  sync();
  const Cpu65816State before = state_.cpu;
  const bool returned = runCall(entry, before, returns, guard);
  // The guest's file, back as it was — but the interrupt lines as they now
  // stand: an edge the routine's run took is not taken twice, and one that
  // arrived during it stays pending for the guest.
  Cpu65816State after = before;
  after.nmiPending = cpu_.state().nmiPending;
  after.irqLine = cpu_.state().irqLine;
  setCpuState(after);
  return returned;
}

bool Snes::callOnStack(std::uint32_t entry, std::uint16_t stackTop, Standin returns,
                       std::size_t guard) {
  // Refused, or run to the boundary first, as callInContext is.
  if (returns == Standin::None || onApuThread() || insideCycle_ ||
      !addressable(entry & 0xFFFFFFu, 1)) {
    return false;
  }
  const Flight flight{*this};
  while (!cpu_.atInstructionBoundary()) machineCycle();
  sync();
  Cpu65816State file = state_.cpu;
  file.s = stackTop;
  return runCall(entry, file, returns, guard);
}

std::uint8_t Snes::wramPortValue(std::uint16_t offset) const noexcept {
  if (offset == 0x2180) return state_.wram[state_.wmadd & 0x1FFFFu];
  return state_.mdr;  // $2181-$2183 are write-only
}

std::uint8_t Snes::readWramPort(std::uint16_t offset, std::uint8_t cycle) {
  if (offset == 0x2180) {
    const std::uint32_t at = state_.wmadd & 0x1FFFFu;
    std::uint8_t v = wramPortValue(offset);
    state_.wmadd = (at + 1u) & 0x1FFFFu;
    // The port's own read of work RAM, at the bank-$7E address it reached, is an
    // access a host can watch (source WramPort, the driving access's cycle); the
    // access to $2180 that asked for it is reported by whoever made it.
    v = watchRead(0x7E0000u | at, v, AccessSource::WramPort, CycleKind::DataRead, cycle);
    observe(0x7E0000u | at, v, false, CycleKind::DataRead, AccessSource::WramPort);
    return latch(v);
  }
  return wramPortValue(offset);
}

void Snes::writeWramPort(std::uint16_t offset, std::uint8_t value, std::uint8_t cycle) {
  switch (offset) {
    case 0x2180: {
      const std::uint32_t at = state_.wmadd & 0x1FFFFu;
      // The port's own write of work RAM is a watchable access (source
      // WramPort); a vetoed write stores nothing, and the port's address steps
      // regardless, as the hardware steps it on every $2180 access.
      const std::optional<std::uint8_t> stored =
          watchWrite(0x7E0000u | at, value, AccessSource::WramPort, CycleKind::DataWrite, cycle);
      if (stored.has_value()) state_.wram[at] = *stored;
      state_.wmadd = (at + 1u) & 0x1FFFFu;
      observe(0x7E0000u | at, value, true, CycleKind::DataWrite, AccessSource::WramPort);
      break;
    }
    case 0x2181:
      state_.wmadd = (state_.wmadd & 0x1FF00u) | value;
      break;
    case 0x2182:
      state_.wmadd = (state_.wmadd & 0x100FFu) |
                     (static_cast<std::uint32_t>(value) << 8);
      break;
    case 0x2183:
      state_.wmadd = (state_.wmadd & 0x0FFFFu) |
                     (static_cast<std::uint32_t>(value & 1u) << 16);
      break;
    default:
      break;
  }
}

std::uint16_t Snes::lineLength() const noexcept {
  // One line a frame is not 1364: NTSC drops a dot from line 240 on an odd field
  // unless the frame is interlaced, and PAL adds one to line 311 on an odd field when
  // it is. Each is decided by the state as its own line runs.
  const bool oddField = state_.field == 1u;
  if (region_ == Region::Ntsc) {
    if (oddField && state_.vpos == kShortLineV && !state_.ppu.interlace()) {
      return kShortLineMaster;
    }
    return kLineMaster;
  }
  if (oddField && state_.vpos == kLongLineV && state_.ppu.interlace()) return kLongLineMaster;
  return kLineMaster;
}

std::uint16_t Snes::frameLines() const noexcept {
  // An interlaced frame of even parity carries one line more than the frame otherwise
  // has, and that line is vertical blank's like the ones before it.
  const std::uint16_t base = region_ == Region::Ntsc ? kNtscLines : kPalLines;
  return state_.ppu.interlace() && state_.field == 0u
             ? static_cast<std::uint16_t>(base + 1u)
             : base;
}

namespace {

// Whether a position in the line falls in its dot's second half: the last two
// master cycles of a four-cycle dot, the last three of one of the two six-cycle
// dots. The short line keeps 340 even dots and has no six-cycle ones.
[[nodiscard]] bool lateHalfOf(std::uint16_t hpos, bool shortLine) noexcept {
  if (hpos < kFirstLongDot || shortLine) return (hpos & 3u) >= 2u;
  if (hpos < kAfterFirstLongDot) return static_cast<unsigned>(hpos - kFirstLongDot) >= 3u;
  if (hpos >= kSecondLongDot && hpos < kAfterSecondLongDot) {
    return static_cast<unsigned>(hpos - kSecondLongDot) >= 3u;
  }
  // Past a long dot the four-cycle grid is offset by the two cycles it added.
  const unsigned offset = hpos >= kAfterSecondLongDot ? 4u : 2u;
  return ((static_cast<unsigned>(hpos) - offset) & 3u) >= 2u;
}

}  // namespace

std::uint16_t Snes::hdot() const noexcept { return hdotAt(state_.hpos); }

std::uint16_t Snes::hdotAt(std::uint16_t h) const noexcept {
  // The dot the position is on. Dots 323 and 327 are six master cycles wide and every
  // other dot is four, so past dot 322 the count falls behind a plain quarter of the
  // position and dot 340 is reached only on a line that runs 1368. The short line
  // keeps 340 even dots and none of this applies to it.
  if (h < kFirstLongDot || lineLength() == kShortLineMaster) {
    return static_cast<std::uint16_t>(h >> 2);
  }
  if (h < kAfterFirstLongDot) return 323u;
  if (h < kSecondLongDot) return static_cast<std::uint16_t>(324u + (h - kAfterFirstLongDot) / 4u);
  if (h < kAfterSecondLongDot) return 327u;
  if (h < kLineMaster) return static_cast<std::uint16_t>(328u + (h - kAfterSecondLongDot) / 4u);
  return 340u;
}

bool Snes::inHblank() const noexcept {
  // The flag stands from H = 274 to H = 1 of the next line, so the line's first four
  // master cycles are still the previous line's blank.
  return state_.hpos < kHblankClear || state_.hpos >= kHblankSet;
}

std::uint64_t Snes::nextRefresh(std::uint64_t lineStart) const noexcept {
  // Each pause is the point on the previous pause's eight-cycle grid that lies
  // nearest the middle of its own line, so consecutive normal lines come up 538 and
  // 534 cycles in and a line of another length re-phases the pair.
  const std::uint64_t target = lineStart + kRefreshTarget;
  const std::uint64_t ahead = (target - state_.refreshAt) % 8u;
  return ahead <= 4u ? target - ahead : target + (8u - ahead);
}

bool Snes::timerCrossed() const noexcept {
  switch (static_cast<std::uint8_t>((state_.nmitimen >> 4) & 3u)) {
    case 1: return timerHPoint_;         // H = H on every line
    case 2: return timerZeroOnVLine_;    // V = V, with no H to compare
    case 3: return timerHPointOnVLine_;  // H = H and V = V
    default: return false;               // the timer is off
  }
}

void Snes::settleTimer() noexcept {
  // A crossing is noted as the cycle ticks and the flag is raised at the cycle's end
  // under the mode the cycle leaves behind, so a write that arms the timer in the very
  // cycle its point is crossed is in time and one that disarms it is too.
  if (timerCrossed()) state_.timeup = true;
}

Snes::TimerPoints Snes::timerPoints(std::uint64_t lineStart) const noexcept {
  const std::uint64_t zero = lineStart + kTimerLineSpan - state_.previousLineMaster;
  return TimerPoints{
      .h = state_.htime != 0u ? lineStart + kTimerHOrigin + 4ull * state_.htime : zero,
      .zero = zero,
      .onTimerLine = state_.vpos == state_.vtime,
      // Dot 153 raises nothing on the short scanline, nor on the last scanline of a
      // frame. The V-only point is a line's own and keeps its place: the exception is
      // a dot.
      .quietDot = state_.htime == kTimerQuietDot &&
                  (lineLength() == kShortLineMaster ||
                   state_.vpos == static_cast<std::uint16_t>(frameLines() - 1u)),
  };
}

std::uint64_t Snes::nextLineEvent(std::uint64_t lineStart, std::uint64_t to) const noexcept {
  // Every point crossLine tests, under the condition that lets it act, from the state
  // as it stands; the earliest past `to` is the first the beam can still reach.
  std::uint64_t next = kNoLineEvent;
  const auto consider = [to, &next](std::uint64_t point) noexcept {
    if (point > to && point < next) next = point;
  };
  if (state_.vpos == 0u) consider(lineStart + kFieldToggle);
  if (state_.inVblank && state_.vpos == state_.vblankBeginLine) {
    consider(lineStart + kNmiFlagOffset);
    consider(lineStart + kOamReloadOffset);
    if ((state_.nmitimen & 1u) != 0u && state_.autoJoyStart < lineStart) {
      consider(autoJoypadStart(lineStart));
    }
  }
  const TimerPoints timer = timerPoints(lineStart);
  if (!timer.quietDot) consider(timer.h);
  if (timer.onTimerLine) consider(timer.zero);
  consider(state_.refreshAt);
  if (state_.counterLatchAt != 0u) consider(state_.counterLatchAt);
  return next;
}

void Snes::crossLine(std::uint64_t lineStart, std::uint64_t from, std::uint64_t to) {
  // The events inside a line, each at its own master offset, for the span the cycle
  // just covered. A point is passed when the span reaches it, and a span never starts
  // before its own line — so an event at offset 0 can never be passed here and belongs
  // to advanceLine, which runs as the line begins. Everything below lies past it.
  // A span that ends short of the earliest event still ahead passes none of them.
  if (lineEventsFrom_ != 0u && to < lineEventsFrom_) return;

  const auto passed = [from, to](std::uint64_t point) noexcept {
    return from < point && point <= to;
  };

  if (state_.vpos == 0u && passed(lineStart + kFieldToggle)) {
    state_.field ^= 1u;  // the frame takes its parity as its first line begins
  }

  // Vertical blank's line raises the NMI flag two cycles after the signal itself, and
  // hands the sprite table's address back at dot 10. Neither re-fires: they are dated
  // from the line the blank began on, which comes once a frame.
  if (state_.inVblank && state_.vpos == state_.vblankBeginLine) {
    if (passed(lineStart + kNmiFlagOffset)) {
      // The flag's rise is the edge the CPU latches, here and now while NMIs are
      // enabled: a read of $4210 in this same cycle sees the flag and clears it,
      // and the NMI is taken all the same, at the end of the instruction.
      state_.vblankNmi = true;
      if ((state_.nmitimen & 0x80u) != 0u) cpu_.setNmiLine(true);
    }
    if (passed(lineStart + kOamReloadOffset)) Ppu{state_.ppu, derived_}.beginVblank();
    // The auto-read begins at its own point on this line, once, while $4200 asks
    // for it: the strobe pulse latches every pad and the clocks follow.
    if ((state_.nmitimen & 1u) != 0u && state_.autoJoyStart < lineStart &&
        passed(autoJoypadStart(lineStart))) {
      latchJoypads();
      state_.autoJoyStart = autoJoypadStart(lineStart);
      state_.autoJoyClocked = 0u;
    }
  }

  const TimerPoints timer = timerPoints(lineStart);
  if (!timer.quietDot && passed(timer.h)) {
    timerHPoint_ = true;
    if (timer.onTimerLine) timerHPointOnVLine_ = true;
  }
  if (timer.onTimerLine && passed(timer.zero)) timerZeroOnVLine_ = true;

  // The refresh: the cycle whose tick reaches the point finishes on its own time, and
  // the pause runs before the next one. The pause holds the core whatever it is doing,
  // so a core waiting on WAI samples the interrupt line it wakes on at the first idle
  // cycle past the pause rather than inside it.
  if (passed(state_.refreshAt)) {
    state_.refreshLeft = static_cast<std::uint8_t>(kRefreshMaster);
  }

  // A latch owed by a fall of the I/O port's top bit lands as the beam reaches its
  // point, capturing the dot and line there rather than at the write. The line is
  // this span's, and the dot is the point's own offset into it.
  if (state_.counterLatchAt != 0u && passed(state_.counterLatchAt)) {
    PpuInputs in = ppuInputs();
    in.hdot = hdotAt(static_cast<std::uint16_t>(state_.counterLatchAt - lineStart));
    Ppu{state_.ppu, derived_}.latchCounters(in);
    state_.counterLatchAt = 0u;
  }

  lineEventsFrom_ = nextLineEvent(lineStart, to);
}

void Snes::rangeSpan(std::uint64_t lineStart, std::uint64_t to) noexcept {
  // The pass belongs to a line the chip renders, and it is gathering for the line
  // after it — so the frame's first line, which draws nothing, is the one that
  // finds the picture's first line its sprites.
  if (state_.inVblank) return;
  // Forced blank walks nowhere, and a walk that has examined every sprite is done.
  if (state_.ppu.forcedBlank() || static_cast<unsigned>(state_.ppu.sprites.scanned) >= kSprites) {
    return;
  }

  // The sprites whose dots the span passed: sprite N is examined at the picture's
  // first dot plus twice its place in the walk, so the 128 of them fill the 256
  // dots of the visible span.
  const std::uint64_t last = (to - lineStart) / 4u;
  if (last < kFirstPictureDot) return;
  const std::uint64_t reached = (last - kFirstPictureDot) / 2u + 1u;

  Ppu ppu{state_.ppu, derived_};
  const std::uint16_t line = static_cast<std::uint16_t>(state_.vpos + 1u);
  while (static_cast<std::uint64_t>(state_.ppu.sprites.scanned) < reached &&
         static_cast<unsigned>(state_.ppu.sprites.scanned) < kSprites) {
    const std::uint8_t before = state_.ppu.sprites.scanned;
    ppu.rangeSprite(line);
    if (state_.ppu.sprites.scanned == before) return;  // forced blank walks nowhere
  }
}

void Snes::drawSpan(std::uint64_t lineStart, std::uint64_t from, std::uint64_t to) {
  // The first line of a frame draws nothing, and the lines from this frame's own
  // vertical blank on are past the picture: tested here before the inputs are built.
  if (state_.vpos == 0u || state_.inVblank) return;
  Ppu ppu{state_.ppu, derived_};
  drawDots(ppu, pictureInputs(), static_cast<std::uint16_t>(from - lineStart),
           static_cast<std::uint16_t>(to - lineStart));
}

void Snes::drawDots(Ppu& ppu, const PpuInputs& in, std::uint16_t fromHpos, std::uint16_t toHpos) {
  // Nothing is drawn on the frame's first line, in vertical blank, or on a line past
  // the tallest picture's last: the beam's blank begins there at the latest, and a
  // restored state that stands on such a line with its blank flag down has no raster
  // row and no line record to draw into.
  if (in.vpos == 0u || in.vblank || in.vpos > kTallestPicture) return;

  // The dots the span passed, by the same reckoning the line's events use: a dot is
  // reached when the span covers the master cycle it begins on. The tile reads of
  // those dots come first, in dot order, then each picture dot's pixel: a read
  // later in the span writes no column an earlier pixel of it reads, and a
  // register write lands between spans, so the order within one is not observable.
  const std::uint32_t first = fromHpos / 4u + 1u;
  const std::uint32_t last = toHpos / 4u;
  const std::uint32_t dot = first < kFirstTileReadDot ? kFirstTileReadDot : first;
  const std::uint32_t stop = last < kLastPictureDot ? last : kLastPictureDot;
  const std::uint32_t firstPixel = dot < kFirstPictureDot ? kFirstPictureDot : dot;

  // The span that reaches the line's first read dot compares what the line reads
  // with what the frame before recorded for it: a line the frame before can stand
  // in for is not drawn, and takes its outputs from the record at its end; a write
  // landing inside it draws it up to the beam first (settleLine).
  if (!cache_->decided && dot <= stop) decideLine(ppu, in, fromHpos);
  if (cache_->pending) return;

  // A watched picture line starts the frame's raster, black, whichever of its dots
  // the span covers.
  if (frameObserver_ != nullptr && raster_.empty()) startRaster();
  // The dots before the first read and past the last pixel hold neither.
  if (dot > stop) return;

  // The tile reads, and what the chip carries from one position to the next, are
  // made whether or not anyone is watching, since both are part of the state a
  // snapshot holds.
  ppu.tileReads(static_cast<std::uint16_t>(dot), static_cast<std::uint16_t>(stop), in);
  if (frameObserver_ == nullptr) {
    // What the chip carries to the next position is the span's last pixel's alone:
    // each decision replaces the one before it and touches nothing else.
    if (firstPixel <= stop) ppu.decide(static_cast<std::uint16_t>(stop - kFirstPictureDot), in);
    return;
  }

  const std::size_t line = static_cast<std::size_t>(in.vpos) - 1u;
  for (std::uint32_t at = firstPixel; at <= stop; ++at) {
    const std::uint16_t x = static_cast<std::uint16_t>(at - kFirstPictureDot);
    const Ppu::Dot out = ppu.dot(x, in);
    if (out.hires) {
      cache_->drawnInHalfPixels = true;
      if (!frameWide_) widenFrame(line, x);
    }

    // A frame drawn in half-pixels anywhere is 512 wide throughout, and a
    // position drawn whole fills both of its halves.
    if (frameWide_) {
      std::uint8_t* const pixel =
          raster_.data() + (line * kHiresWidth + static_cast<std::size_t>(x) * 2u) * kPixelBytes;
      std::copy(out.left.begin(), out.left.end(), pixel);
      std::copy(out.right.begin(), out.right.end(), pixel + kPixelBytes);
    } else {
      std::uint8_t* const pixel =
          raster_.data() + (line * kPictureWidth + static_cast<std::size_t>(x)) * kPixelBytes;
      std::copy(out.right.begin(), out.right.end(), pixel);
    }
  }
}

void Snes::startRaster() {
  raster_.assign(kRasterBytes, 0u);  // black, and opaque: a line nobody drew is black
  for (std::size_t alpha = 3u; alpha < kRasterBytes; alpha += kPixelBytes) raster_[alpha] = 255u;
}

void Snes::widenFrame(std::size_t line, std::uint16_t x) noexcept {
  // Every pixel already drawn this frame — the rows above `line` and the first `x`
  // of `line` itself — moves to twice its column in a row twice as wide, written
  // into both halves. Each row lands at or beyond where it stood, so working from
  // the last pixel back never overwrites one not yet moved.
  const std::uint8_t* const from = raster_.data();
  std::uint8_t* const to = raster_.data();
  for (std::size_t row = line + 1u; row-- > 0u;) {
    const std::size_t count = row == line ? x : kPictureWidth;
    for (std::size_t column = count; column-- > 0u;) {
      const std::size_t source = (row * kPictureWidth + column) * kPixelBytes;
      const std::size_t target = (row * kHiresWidth + column * 2u) * kPixelBytes;
      std::array<std::uint8_t, kPixelBytes> pixel{};
      std::copy(from + source, from + source + kPixelBytes, pixel.begin());
      std::copy(pixel.begin(), pixel.end(), to + target);
      std::copy(pixel.begin(), pixel.end(), to + target + kPixelBytes);
    }
  }
  frameWide_ = true;
}

void Snes::redrawInidispEarly(std::uint32_t address, std::uint8_t busBefore) {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
  if (!systemBank || offset != 0x2100u || state_.inVblank) return;
  // The cycle drew its dots under the register as it stood; redraw only the last of
  // them, under the byte the bus held before the write. The beam stands at the
  // cycle's end, the dot being the one that ends there.
  const std::uint16_t endHpos = state_.hpos;
  if (endHpos < lastCost_) return;  // the cycle crossed a line; its last dot is off the picture
  if (ppuThreaded_) [[unlikely]] {
    // The thread redraws it, after the write it has been handed.
    pushEvent(PictureEvent{.offset = 0u,
                           .hpos = endHpos,
                           .hdot = 0u,
                           .vpos = 0u,
                           .field = 0u,
                           .vblank = false,
                           .extLatch = false,
                           .lateHalf = false,
                           .kind = PictureEventKind::InidispEarly,
                           .value = 0u,
                           .byte0 = busBefore,
                           .byte1 = 0u});
    return;
  }
  const std::uint8_t written = state_.ppu.inidisp;  // the value the write applied, restored after
  state_.ppu.inidisp = busBefore;
  Ppu ppu{state_.ppu, derived_};
  drawDots(ppu, pictureInputs(), static_cast<std::uint16_t>(endHpos - 4u), endHpos);
  state_.ppu.inidisp = written;
  if (recordsTimeline_) [[unlikely]] recordInidispEarly(state_.vpos, endHpos, busBefore, written);
}

void Snes::deliverFrame() {
  if (frameObserver_ == nullptr) return;
  // The picture drawn to the beam first, since the thread that draws it is the one
  // that starts the raster; then the audio machine at this cycle's end too, so
  // state() inside the report is the whole machine there and the raster the
  // finished frame.
  if (ppuThreaded_) [[unlikely]] waitPicture();
  if (raster_.empty()) return;
  if (apuThreaded_) [[unlikely]] waitApu();
  const std::size_t bytes =
      static_cast<std::size_t>(framePictureLines_) * frameWidth_ * kPixelBytes;
  frameObserver_->frame(VideoFrame{
      .pixels = std::span<const std::uint8_t>(raster_.data(), bytes),
      .width = frameWidth_,
      .height = framePictureLines_,
      .field = frameField_,
  });
}

void Snes::deliverTimeline() {
  if (!recordsTimeline_ || !timeline_) return;
  // As deliverFrame waits: the picture thread finishes the frame's timeline as it
  // reaches the blank's line.
  if (ppuThreaded_) [[unlikely]] waitPicture();
  const TimelineFrame& frame = timeline_->finished;
  if (frame.lines.empty()) return;
  if (apuThreaded_) [[unlikely]] waitApu();
  PpuTimeline timeline;
  timeline.height = frame.height;
  timeline.width = frame.width;
  timeline.field = frame.field;
  timeline.firstLine = frame.lines.front().lineNumber();
  timeline.lines = frame.lines;
  timeline.events = frame.events;
  timeline.vramWrites = frame.vramWrites;
  timeline.vram = frame.vram;
  timeline.palettes = frame.palettes;
  timeline.oams = frame.oams;
  timeline.spriteBuffers = frame.spriteBuffers;
  // The GPU draws the frame before the observer is told of it, so a host that
  // presents the device's texture from its observer finds the frame drawn.
  if (gpuPpu_ != nullptr) drawOnGpu(timeline);
  if (timelineObserver_ != nullptr) timelineObserver_->frame(timeline);
}

void Snes::drawOnGpu(const PpuTimeline& timeline) {
  const std::size_t bytes = timeline.packedSize();
  const std::span<std::uint8_t> out = gpuPpu_->frameBytes(bytes);
  if (timeline.pack(out) != bytes) return;  // the device gave less than the frame needs
  gpuPpu_->draw(gpu::Frame{
      .width = timeline.width,
      .height = timeline.height,
      .firstLine = timeline.firstLine,
      .field = timeline.field,
      .interlaced = state_.ppu.interlace(),
      .bytes = bytes,
  });
}

void Snes::deliverSave() {
  if (saveObserver_ == nullptr) return;
  // As deliverFrame waits.
  if (apuThreaded_) [[unlikely]] waitApu();
  if (ppuThreaded_) [[unlikely]] waitPicture();
  saveObserver_->changed(std::span<const std::uint8_t>(state_.sram.data(), state_.sram.size()));
}

void Snes::finishPicture() noexcept {
  frameWidth_ = frameWide_ ? kHiresWidth : kPictureWidth;
  frameWide_ = false;
}

void Snes::advanceLine(std::uint64_t lineStart) noexcept {
  // The line ending is recorded for the next frame's same line, or counted, before
  // the beam leaves it — here when the picture is drawn on this thread, on the
  // picture thread as it takes the next line's facts when it is drawn there.
  if (!ppuThreaded_) {
    Ppu ending{state_.ppu, derived_};
    completeLine(ending, pictureInputs());
  }
  state_.vpos = static_cast<std::uint16_t>(state_.vpos + 1u);
  if (state_.vpos >= frameLines()) state_.vpos = 0u;
  state_.hdmaLineFired = false;  // each scanline may trigger its own HDMA delivery
  state_.refreshAt = nextRefresh(lineStart);

  // The horizontal blank the line just ended is where Time draws the sprites the
  // Range pass across that line found — for the line beginning now, which holds
  // H = 0 of it, hblank being lowered a dot later. Range then starts again, on the
  // line after this one, and the mosaic's vertical counter takes the new line.
  // What Time read is taken before Range begins again over it: by the picture
  // thread's copy of the line buffer, or by the timeline here.
  Ppu ppu{state_.ppu, derived_};
  ppu.timeSprites(state_.vpos, state_.field);
  if (ppuThreaded_) [[unlikely]] {
    stageLine();
  } else if (recordsTimeline_) [[unlikely]] {
    recordTime(state_.ppu, state_.vpos, state_.field, state_.ppu.objsel, state_.ppu.setini);
  }
  ppu.beginRange(static_cast<std::uint16_t>(state_.vpos + 1u));
  ppu.beginLine(state_.vpos);

  if (state_.vpos == 0u) {
    state_.inVblank = false;     // the frame begins in the picture
    state_.vblankNmi = false;    // and the NMI flag clears with it
    // The new frame re-initialises HDMA at line 0, and everything the last frame's
    // channels stood on is the last frame's. The clearing happens here rather than in
    // the initialisation itself because a frame that begins with $420C empty holds no
    // initialisation at all, and a channel the program brings in later must still find
    // a clean slate.
    state_.hdmaInited = false;
    state_.hdmaActive = 0u;
    state_.hdmaEnded = 0u;
    state_.hdmaDoWrite = 0u;
    Ppu{state_.ppu, derived_}.beginFrame();  // the overflow flags belong to the picture just drawn
  } else if (!state_.inVblank && state_.vpos >= kVblankStartLine &&
             (state_.vpos >= kOverscanVblankStartLine || !state_.ppu.overscan())) {
    // Vertical blank begins at the line SETINI asks for, and the machine asks again
    // at the start of every line until it has: the bit cleared between the two start
    // lines begins the blank at the next line, and the bit set after it has begun
    // changes nothing. Begun is begun; a later SETINI change re-fires nothing.
    state_.inVblank = true;
    state_.vblankBeginLine = state_.vpos;
    state_.hdmaActive = 0u;  // every HDMA channel deactivates for the rest of the frame

    // The picture is finished as the blank begins: no dot is drawn in vertical blank,
    // and the last picture line was completed above. It is as tall as the lines
    // before this one and carries the parity it ran under, which turns at the next
    // frame's first line. Its width is the drawing side's to settle: here when the
    // picture is drawn on this thread, on the picture thread at this line's entry
    // when it is drawn there. The frame's line counts are finished with it, by the
    // side that drew it.
    if (!ppuThreaded_) {
      cache_->last = cache_->frame;
      cache_->frame = LineReuse{};
    }
    if (frameObserver_ != nullptr) {
      frameFinished_ = true;
      frameField_ = state_.field;
      if (!ppuThreaded_) finishPicture();
      framePictureLines_ = static_cast<std::uint16_t>(state_.vpos - 1u);
    }
    // The frame's timeline is finished here by the side that draws, as its width is.
    if (recordsTimeline_) [[unlikely]] {
      timelineFinished_ = true;
      if (!ppuThreaded_) finishTimeline(state_.ppu, state_.vpos, state_.field);
    }
    // A frame that changed the save window is owed a report. The reports are made
    // at the end of this cycle, by deliverFinished, and not from here: the line's
    // own work cannot be left half done, and what a host does with a picture or a
    // save — writing a file, most plainly — may throw. The report lands before the
    // NMI handler runs and before the frame's controller auto-read begins, so a pad
    // a host sets inside it is what this frame's auto-read latches. The beam reaches
    // this line whether or not anyone is watching the picture, so a save is reported
    // to a host that asked for nothing else.
    if (saveObserver_ != nullptr && saveChanged_) {
      saveChanged_ = false;
      saveFinished_ = true;
    }
  }

  // The picture thread is handed the line as it now stands: its facts, the mosaic
  // counter and the line buffer Time just filled, which the whole line is drawn from.
  if (ppuThreaded_) [[unlikely]] pushLine();
}

void Snes::tickVideo(std::uint32_t cost) {
  // The arithmetic unit steps once per CPU cycle regardless of the cycle's master
  // cost; its result lands when the countdown reaches zero.
  if (state_.mathClocks != 0u) {
    --state_.mathClocks;
    if (state_.mathClocks == 0u) commitMath();
  }
  // Advance the beam by the cycle's master cost, one line's share at a time, so every
  // event the span passes is placed at its own master offset. The line's events come
  // first, then the line's end hands the beam to the next one.
  timerHPoint_ = false;
  timerHPointOnVLine_ = false;
  timerZeroOnVLine_ = false;

  std::uint64_t at = state_.master;
  const std::uint64_t end = at + cost;
  std::uint64_t lineStart = at - state_.hpos;
  while (at < end) {
    const std::uint64_t lineEnd = lineStart + lineLength();
    const std::uint64_t stop = end < lineEnd ? end : lineEnd;
    crossLine(lineStart, at, stop);
    // Finding the next line's sprites is the chip's own work and it does it for
    // nobody's benefit, because a program can read what the pass found.
    rangeSpan(lineStart, stop);
    // The picture is drawn a dot at a time: each background's tile reads at their
    // own dots, and each pixel from those reads and the registers as they stand at
    // its dot. A machine nobody is watching makes the reads and resolves only what
    // the chip carries from one position to the next. A machine drawing on its
    // picture thread draws nothing here: the thread draws the same dots from the
    // accesses and the lines this thread logs. A line the frame before stands in
    // for has nothing to draw until a write or a return settles it.
    if (!ppuThreaded_ && !cache_->pending) drawSpan(lineStart, at, stop);
    at = stop;
    state_.hpos = static_cast<std::uint16_t>(at - lineStart);
    if (at == lineEnd) {
      state_.previousLineMaster = static_cast<std::uint16_t>(lineEnd - lineStart);
      lineEventsFrom_ = 0u;  // the next line's events are its own
      lineStart = lineEnd;
      state_.hpos = 0u;
      advanceLine(lineStart);
    }
  }

  // The auto-read's clocks that fell inside this span land their bits, a read
  // begun inside it included.
  clockAutoJoypad(end);

  // HDMA triggers, each latched so it fires once: the frame's initialisation as the
  // beam passes dot 6 of line 0, and a delivery as it passes dot 278 of every line the
  // picture still owns. Triggering only marks the event pending; it runs on the next
  // machine cycle, so it preempts a general-purpose DMA at a whole byte.
  if (!state_.hdmaInited && state_.vpos == 0u && state_.hpos >= 24u &&
      state_.hdmaen != 0u) {
    state_.hdmaInited = true;
    state_.hdmaRunPending = true;
    state_.hdmaIniting = true;
  }
  if (!state_.hdmaLineFired && !state_.inVblank && state_.hpos >= kHdmaDeliver &&
      (state_.hdmaActive & state_.hdmaen) != 0u) {
    state_.hdmaLineFired = true;
    state_.hdmaRunPending = true;
    state_.hdmaIniting = false;
  }
}

void Snes::commitMath() noexcept {
  // The operands are the pair the job started with, not the registers as they
  // stand now: a program may load the next dividend while this division runs and
  // still read this division's quotient.
  switch (state_.mathOp) {
    case MathOp::Multiply:
      // The quotient register already took the multiplier when the multiply started;
      // now the product lands. (WRMPYA * WRMPYB fits sixteen bits.)
      state_.rdmpy = static_cast<std::uint16_t>(state_.mathLeft * state_.mathRight);
      break;
    case MathOp::Divide:
      if (state_.mathRight == 0u) {
        state_.rddiv = 0xFFFFu;          // dividing by zero yields an all-ones quotient
        state_.rdmpy = state_.mathLeft;  // and the dividend as the remainder
      } else {
        state_.rddiv = static_cast<std::uint16_t>(state_.mathLeft / state_.mathRight);
        state_.rdmpy = static_cast<std::uint16_t>(state_.mathLeft % state_.mathRight);
      }
      break;
    case MathOp::None:
      break;
  }
  state_.mathOp = MathOp::None;
}

PpuInputs Snes::ppuInputs() const noexcept {
  return PpuInputs{
      .hdot = hdot(),
      .vpos = state_.vpos,
      .field = state_.field,
      .vblank = state_.inVblank,
      .pal = region_ == Region::Pal,
      .extLatch = (state_.wrio & 0x80u) != 0u,
      // A position short of the first long dot is placed alike on every line length.
      .lateHalf = lateHalfOf(state_.hpos,
                             state_.hpos >= kFirstLongDot && lineLength() == kShortLineMaster),
  };
}

PpuInputs Snes::pictureInputs() const noexcept {
  // The dot, the latch line and the half of the dot are read by no dot drawn; the
  // pins the picture reads are these.
  return PpuInputs{
      .hdot = 0u,
      .vpos = state_.vpos,
      .field = state_.field,
      .vblank = state_.inVblank,
      .pal = region_ == Region::Pal,
      .extLatch = true,
      .lateHalf = false,
  };
}

std::uint8_t Snes::cpuRegValue(std::uint16_t offset) const noexcept {
  switch (offset) {
    case 0x4210:  // RDNMI: the vblank flag (bit 7), CPU version 2 (bits 3-0), open bus between
      return static_cast<std::uint8_t>((state_.vblankNmi ? 0x80u : 0x00u) | (state_.mdr & 0x70u) |
                                       0x02u);
    case 0x4211:  // TIMEUP: the H/V-timer IRQ flag (bit 7), open bus below
      // A read in the very cycle the flag rises receives it set and acknowledges
      // nothing (fullsnes.txt 1781-1784): the cycle has ticked, so the crossing is
      // known here, and the cycle's end raises the flag over this read's clear.
      return static_cast<std::uint8_t>(((state_.timeup || timerCrossed()) ? 0x80u : 0x00u) |
                                       (state_.mdr & 0x7Fu));
    case 0x4212:  // HVBJOY: vblank (bit 7), hblank (bit 6), auto-joypad busy (bit 0), open bus between
      return static_cast<std::uint8_t>((state_.inVblank ? 0x80u : 0x00u) |
                                       (inHblank() ? 0x40u : 0x00u) | (state_.mdr & 0x3Eu) |
                                       (autoJoypadBusy() ? 0x01u : 0x00u));
    case 0x4213: return state_.wrio;  // RDIO: the port's lines, which nothing on the console drives, so as written
    case 0x4214: return static_cast<std::uint8_t>(state_.rddiv & 0xFFu);
    case 0x4215: return static_cast<std::uint8_t>(state_.rddiv >> 8);
    case 0x4216: return static_cast<std::uint8_t>(state_.rdmpy & 0xFFu);
    case 0x4217: return static_cast<std::uint8_t>(state_.rdmpy >> 8);
    default:
      break;
  }
  if (offset >= 0x4218 && offset <= 0x421F) {
    return state_.joy[static_cast<std::size_t>(offset - 0x4218)];  // the auto-read's registers as they stand, part-shifted while it is busy
  }
  return state_.mdr;  // other CPU-register reads are open bus
}

std::uint8_t Snes::readCpuReg(std::uint16_t offset) {
  const std::uint8_t v = cpuRegValue(offset);
  // Reading RDNMI or TIMEUP acknowledges its flag.
  if (offset == 0x4210) state_.vblankNmi = false;
  if (offset == 0x4211) state_.timeup = false;
  return latch(v);
}

void Snes::writeCpuReg(std::uint16_t offset, std::uint8_t value) {
  // The H/V timer's points, the auto-read's start and an owed counter latch are dated
  // from these registers, so the line's earliest event is worked out again.
  if (offset == 0x4200u || offset == 0x4201u || (offset >= 0x4207u && offset <= 0x420Au)) {
    lineEventsFrom_ = 0u;
  }
  switch (offset) {
    case 0x4200:  // NMITIMEN: NMI enable, H/V IRQ mode, auto-joypad enable
      if (((value >> 4) & 3u) == 0u) state_.timeup = false;  // disabling the IRQ acknowledges it
      state_.nmitimen = value;
      return;
    case 0x4201: {  // WRIO: the I/O port; its top bit falling latches the PPU's counters
      const bool fell = (state_.wrio & 0x80u) != 0u && (value & 0x80u) == 0u;
      state_.wrio = value;
      // The latch through this line lands one dot after the point a $2137 read of
      // the same cycle would (fullsnes.txt 27073-27075). It is owed and
      // captured as the beam passes that point, so it lands from the next cycle's
      // tick, and a snapshot taken between the write and the landing carries it.
      if (fell) state_.counterLatchAt = state_.master + lastCost_ + 4u;
      return;
    }
    case 0x4202: state_.wrmpya = value; return;
    case 0x4203:  // WRMPYB: its write starts the multiply, on the operands as they stand
      state_.wrmpyb = value;
      state_.rddiv = value;  // the shared unit immediately loads the quotient register with the multiplier
      state_.mathLeft = state_.wrmpya;
      state_.mathRight = value;
      state_.mathOp = MathOp::Multiply;
      state_.mathClocks = kMultiplyClocks;
      return;
    case 0x4204:
      state_.wrdiv = static_cast<std::uint16_t>((state_.wrdiv & 0xFF00u) | value);
      return;
    case 0x4205:
      state_.wrdiv = static_cast<std::uint16_t>((state_.wrdiv & 0x00FFu) | (value << 8));
      return;
    case 0x4206:  // WRDIVB: its write starts the divide, on the operands as they stand
      state_.wrdivb = value;
      state_.mathLeft = state_.wrdiv;
      state_.mathRight = value;
      state_.mathOp = MathOp::Divide;
      state_.mathClocks = kDivideClocks;
      return;
    case 0x4207: state_.htime = static_cast<std::uint16_t>((state_.htime & 0x0100u) | value); return;
    case 0x4208: state_.htime = static_cast<std::uint16_t>((state_.htime & 0x00FFu) | ((value & 1u) << 8)); return;
    case 0x4209: state_.vtime = static_cast<std::uint16_t>((state_.vtime & 0x0100u) | value); return;
    case 0x420A: state_.vtime = static_cast<std::uint16_t>((state_.vtime & 0x00FFu) | ((value & 1u) << 8)); return;
    case 0x420B: triggerDma(value); return;             // start a general-purpose DMA on each selected channel
    case 0x420C: enableHdma(value); return;             // enable HDMA on the selected channels
    case 0x420D: state_.memsel = static_cast<std::uint8_t>(value & 1u); return;
    case 0x4211: state_.timeup = false; return;  // TIMEUP: a write acknowledges the flag as a read does
    default: return;  // the other read-only ports ignore writes
  }
}

// ---- the controller ports -----------------------------------------------------

namespace {

// The twelve, in the order the pad shifts them out, which is the order the Button
// values themselves run in — so a button indexes this table directly.
constexpr std::array<Button, kButtonCount> kButtonOrder{
    Button::B,  Button::Y,    Button::Select, Button::Start,
    Button::Up, Button::Down, Button::Left,   Button::Right,
    Button::A,  Button::X,    Button::L,      Button::R,
};

constexpr std::array<std::string_view, kButtonCount> kButtonNames{
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r",
};

constexpr char lowered(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::span<const Button> buttons() noexcept { return kButtonOrder; }

std::string_view buttonName(Button button) noexcept {
  return kButtonNames[static_cast<std::size_t>(button)];
}

std::optional<Button> buttonFromName(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kButtonCount; ++i) {
    const std::string_view candidate = kButtonNames[i];
    if (candidate.size() != name.size()) continue;
    bool same = true;
    for (std::size_t c = 0; c < name.size(); ++c) {
      if (lowered(name[c]) != candidate[c]) {
        same = false;
        break;
      }
    }
    if (same) return kButtonOrder[i];
  }
  return std::nullopt;
}

bool Joypad::holds(Button button) const noexcept {
  switch (button) {
    case Button::B: return b;
    case Button::Y: return y;
    case Button::Select: return select;
    case Button::Start: return start;
    case Button::Up: return up;
    case Button::Down: return down;
    case Button::Left: return left;
    case Button::Right: return right;
    case Button::A: return a;
    case Button::X: return x;
    case Button::L: return l;
    case Button::R: return r;
  }
  return false;
}

void Joypad::hold(Button button, bool pressed) noexcept {
  switch (button) {
    case Button::B: b = pressed; return;
    case Button::Y: y = pressed; return;
    case Button::Select: select = pressed; return;
    case Button::Start: start = pressed; return;
    case Button::Up: up = pressed; return;
    case Button::Down: down = pressed; return;
    case Button::Left: left = pressed; return;
    case Button::Right: right = pressed; return;
    case Button::A: a = pressed; return;
    case Button::X: x = pressed; return;
    case Button::L: l = pressed; return;
    case Button::R: r = pressed; return;
  }
}

std::uint16_t Joypad::bits() const noexcept {
  // The wire order, first bit highest: B Y Select Start Up Down Left Right A X L R,
  // then the four identity bits, zero for a standard pad.
  std::uint16_t word = 0;
  const bool order[12] = {b, y, select, start, up, down, left, right, a, x, l, r};
  for (std::size_t i = 0; i < 12; ++i) {
    if (order[i]) word = static_cast<std::uint16_t>(word | (0x8000u >> i));
  }
  return word;
}

// ---- the channel and the pad as bytes -------------------------------------
// Each is its fields in the order they are declared.

namespace bytes {

void putPart(std::uint8_t*& at, const DmaChannel& state) noexcept {
  put8(at, state.dmap);
  put8(at, state.bbad);
  put16(at, state.a1t);
  put8(at, state.a1b);
  put16(at, state.das);
  put8(at, state.dasb);
  put16(at, state.a2a);
  put8(at, state.nltr);
  put8(at, state.unused);
}

void readPart(Reader& in, DmaChannel& state) noexcept {
  in.u8(state.dmap);
  in.u8(state.bbad);
  in.u16(state.a1t);
  in.u8(state.a1b);
  in.u16(state.das);
  in.u8(state.dasb);
  in.u16(state.a2a);
  in.u8(state.nltr);
  in.u8(state.unused);
}

void putPart(std::uint8_t*& at, const Joypad& state) noexcept {
  putBool(at, state.b);
  putBool(at, state.y);
  putBool(at, state.select);
  putBool(at, state.start);
  putBool(at, state.up);
  putBool(at, state.down);
  putBool(at, state.left);
  putBool(at, state.right);
  putBool(at, state.a);
  putBool(at, state.x);
  putBool(at, state.l);
  putBool(at, state.r);
}

void readPart(Reader& in, Joypad& state) noexcept {
  in.boolean(state.b);
  in.boolean(state.y);
  in.boolean(state.select);
  in.boolean(state.start);
  in.boolean(state.up);
  in.boolean(state.down);
  in.boolean(state.left);
  in.boolean(state.right);
  in.boolean(state.a);
  in.boolean(state.x);
  in.boolean(state.l);
  in.boolean(state.r);
}

}  // namespace bytes

std::vector<std::uint8_t> DmaChannel::asBytes() const {
  return bytes::encode(*this);
}

std::size_t DmaChannel::asBytes(std::span<std::uint8_t> out) const noexcept {
  return bytes::encodeInto(*this, out);
}

std::optional<DmaChannel> DmaChannel::fromBytes(std::span<const std::uint8_t> bytes) {
  return bytes::decode<DmaChannel>(bytes);
}

bool DmaChannel::fromBytes(std::span<const std::uint8_t> bytes, DmaChannel& into) noexcept {
  return bytes::decodeInto(bytes, into);
}

std::vector<std::uint8_t> Joypad::asBytes() const {
  return bytes::encode(*this);
}

std::size_t Joypad::asBytes(std::span<std::uint8_t> out) const noexcept {
  return bytes::encodeInto(*this, out);
}

std::optional<Joypad> Joypad::fromBytes(std::span<const std::uint8_t> bytes) {
  return bytes::decode<Joypad>(bytes);
}

bool Joypad::fromBytes(std::span<const std::uint8_t> bytes, Joypad& into) noexcept {
  return bytes::decodeInto(bytes, into);
}

// ---- the machine as bytes -------------------------------------------------
// The version and the save's length, SnesState's own fields in the order they are
// declared, the parts — cpu, apu, the two ports, ppu, the eight channels — and the
// save last.

namespace bytes {
namespace {

// The version and the save's length.
constexpr std::size_t kHeaderBytes = 6u;
// Every byte before the save: the header, the state's own 131194 and its parts.
constexpr std::size_t kBytesBeforeSave = 268858u;

// A port: 1 and the pad's twelve bytes, or 0 and twelve zeros.
void putPort(std::uint8_t*& at, const std::optional<Joypad>& pad) noexcept {
  putBool(at, pad.has_value());
  if (pad.has_value()) {
    putPart(at, *pad);
    return;
  }
  for (std::size_t i = 0u; i < Joypad::byteSize(); ++i) put8(at, 0u);
}

void readPort(Reader& in, std::optional<Joypad>& pad) noexcept {
  const std::uint8_t present = in.take();
  if (!checkBool(present)) {
    in.refuse();
    in.zeros(Joypad::byteSize());
    return;
  }
  if (present == 0u) {
    in.zeros(Joypad::byteSize());
    if (in.fills()) pad.reset();
    return;
  }
  if (!in.fills()) {
    Joypad checked;
    readPart(in, checked);
    return;
  }
  if (!pad.has_value()) pad.emplace();
  readPart(in, *pad);
}

void putMachine(std::uint8_t*& at, const SnesState& state) noexcept {
  putBytes(at, state.wram);
  put32(at, state.wmadd);
  put8(at, state.memsel);
  put8(at, state.mdr);
  put64(at, state.master);
  put64(at, state.consumed);
  put32(at, state.apuPhase);
  put16(at, state.hpos);
  put16(at, state.vpos);
  put8(at, state.field);
  putBool(at, state.inVblank);
  put16(at, state.vblankBeginLine);
  put16(at, state.previousLineMaster);
  put64(at, state.refreshAt);
  put8(at, state.refreshLeft);
  put8(at, state.nmitimen);
  putBool(at, state.vblankNmi);
  putBool(at, state.timeup);
  put16(at, state.htime);
  put16(at, state.vtime);
  put8(at, state.wrmpya);
  put8(at, state.wrmpyb);
  put16(at, state.wrdiv);
  put8(at, state.wrdivb);
  put16(at, state.rddiv);
  put16(at, state.rdmpy);
  put8(at, state.mathClocks);
  putEnum(at, state.mathOp);
  put16(at, state.mathLeft);
  put8(at, state.mathRight);
  putBool(at, state.joyStrobe);
  for (const std::uint16_t latched : state.joyLatch) put16(at, latched);
  putBytes(at, state.joyClocks);
  put64(at, state.autoJoyStart);
  put8(at, state.autoJoyClocked);
  putBytes(at, state.joy);
  put8(at, state.wrio);
  put64(at, state.counterLatchAt);
  put8(at, state.mdmaen);
  put8(at, state.hdmaen);
  put8(at, state.dmaArm);
  putBool(at, state.dmaRunning);
  putBool(at, state.dmaOpened);
  putBool(at, state.dmaChannelOpened);
  put8(at, state.dmaUnit);
  put64(at, state.dmaPauseMaster);
  putBool(at, state.dmaResumePad);
  put8(at, state.hdmaActive);
  put8(at, state.hdmaEnded);
  put8(at, state.hdmaDoWrite);
  putBool(at, state.hdmaInited);
  putBool(at, state.hdmaLineFired);
  putBool(at, state.hdmaRunPending);
  putBool(at, state.hdmaIniting);
  putPart(at, state.cpu);
  putPart(at, state.apu);
  for (const std::optional<Joypad>& pad : state.pads) putPort(at, pad);
  putPart(at, state.ppu);
  for (const DmaChannel& channel : state.dma) putPart(at, channel);
}

void readMachine(Reader& in, SnesState& state) noexcept {
  in.bytes(state.wram);
  in.u32(state.wmadd);
  in.u8(state.memsel);
  in.u8(state.mdr);
  in.u64(state.master);
  in.u64(state.consumed);
  in.u32(state.apuPhase);
  in.u16(state.hpos);
  in.u16(state.vpos);
  in.u8(state.field);
  in.boolean(state.inVblank);
  in.u16(state.vblankBeginLine);
  in.u16(state.previousLineMaster);
  in.u64(state.refreshAt);
  in.u8(state.refreshLeft);
  in.u8(state.nmitimen);
  in.boolean(state.vblankNmi);
  in.boolean(state.timeup);
  in.u16(state.htime);
  in.u16(state.vtime);
  in.u8(state.wrmpya);
  in.u8(state.wrmpyb);
  in.u16(state.wrdiv);
  in.u8(state.wrdivb);
  in.u16(state.rddiv);
  in.u16(state.rdmpy);
  in.u8(state.mathClocks);
  in.enumeration(state.mathOp, MathOp::Divide);
  in.u16(state.mathLeft);
  in.u8(state.mathRight);
  in.boolean(state.joyStrobe);
  for (std::uint16_t& latched : state.joyLatch) in.u16(latched);
  in.bytes(state.joyClocks);
  in.u64(state.autoJoyStart);
  in.u8(state.autoJoyClocked);
  in.bytes(state.joy);
  in.u8(state.wrio);
  in.u64(state.counterLatchAt);
  in.u8(state.mdmaen);
  in.u8(state.hdmaen);
  in.u8(state.dmaArm);
  in.boolean(state.dmaRunning);
  in.boolean(state.dmaOpened);
  in.boolean(state.dmaChannelOpened);
  in.u8(state.dmaUnit);
  in.u64(state.dmaPauseMaster);
  in.boolean(state.dmaResumePad);
  in.u8(state.hdmaActive);
  in.u8(state.hdmaEnded);
  in.u8(state.hdmaDoWrite);
  in.boolean(state.hdmaInited);
  in.boolean(state.hdmaLineFired);
  in.boolean(state.hdmaRunPending);
  in.boolean(state.hdmaIniting);
  readPart(in, state.cpu);
  readPart(in, state.apu);
  for (std::optional<Joypad>& pad : state.pads) readPort(in, pad);
  readPart(in, state.ppu);
  for (DmaChannel& channel : state.dma) readPart(in, channel);
}

}  // namespace
}  // namespace bytes

std::size_t SnesState::byteSize() const noexcept {
  return bytes::kBytesBeforeSave + sram.size();
}

std::size_t SnesState::asBytes(std::span<std::uint8_t> out) const noexcept {
  if (out.size() < byteSize()) return 0u;
  std::uint8_t* at = out.data();
  bytes::put16(at, kBytesVersion);
  bytes::put32(at, static_cast<std::uint32_t>(sram.size()));
  bytes::putMachine(at, *this);
  at = std::copy(sram.begin(), sram.end(), at);
  return static_cast<std::size_t>(at - out.data());
}

std::vector<std::uint8_t> SnesState::asBytes() const {
  std::vector<std::uint8_t> out(byteSize());
  const std::size_t written = asBytes(std::span<std::uint8_t>(out));
  out.resize(written);
  return out;
}

bool SnesState::fromBytes(std::span<const std::uint8_t> bytes, SnesState& into) {
  if (bytes.size() < bytes::kHeaderBytes) return false;
  const std::uint8_t* at = bytes.data();
  if (bytes::get16(at) != kBytesVersion) return false;
  const std::uint32_t saveBytes = bytes::get32(at);
  if (saveBytes > kMaxSaveRamBytes) return false;
  if (bytes.size() != bytes::kBytesBeforeSave + saveBytes) return false;
  bytes::Reader check(at, bytes::Reader::Pass::Check);
  bytes::readMachine(check, into);
  if (!check.ok()) return false;
  bytes::Reader fill(at, bytes::Reader::Pass::Fill);
  bytes::readMachine(fill, into);
  into.sram.assign(fill.at(), fill.at() + saveBytes);
  return true;
}

std::optional<SnesState> SnesState::fromBytes(std::span<const std::uint8_t> bytes) {
  std::optional<SnesState> value{std::in_place};
  if (!fromBytes(bytes, *value)) value.reset();
  return value;
}

void Snes::setJoypad(JoypadPort port, std::optional<Joypad> pad) noexcept {
  state_.pads[static_cast<std::size_t>(port)] = pad;
}

const std::optional<Joypad>& Snes::joypad(JoypadPort port) const noexcept {
  return state_.pads[static_cast<std::size_t>(port)];
}

void Snes::latchJoypads() noexcept {
  // The strobe pulse: every port's pad loads its sixteen bits and the clock count
  // starts over. A port with nothing in it latches nothing.
  for (std::size_t p = 0; p < 2; ++p) {
    state_.joyLatch[p] = state_.pads[p] ? state_.pads[p]->bits() : std::uint16_t{0u};
    state_.joyClocks[p] = 0u;
  }
}

std::uint8_t Snes::joypadBit(std::size_t port) const noexcept {
  const std::optional<Joypad>& pad = state_.pads[port];
  if (!pad) return 0u;  // an empty port's data line stays high, which reads as zero
  // While the strobe is held high the pad keeps reloading its register, so every
  // clock returns the first bit — B.
  if (state_.joyStrobe) return static_cast<std::uint8_t>((pad->bits() >> 15) & 1u);
  if (state_.joyClocks[port] >= 16u) return 1u;  // past the sixteenth bit a pad returns its padding, low
  return static_cast<std::uint8_t>((state_.joyLatch[port] >> (15u - state_.joyClocks[port])) & 1u);
}

std::uint8_t Snes::clockJoypad(std::size_t port) noexcept {
  const std::uint8_t bit = joypadBit(port);
  // The count advances only while a pad is shifting out the bits it latched: an
  // empty port has none, a held strobe keeps reloading them, and past the
  // sixteenth the pad has none left.
  if (state_.pads[port] && !state_.joyStrobe && state_.joyClocks[port] < 16u) {
    ++state_.joyClocks[port];
  }
  return bit;
}

std::uint64_t Snes::autoJoypadStart(std::uint64_t lineStart) const noexcept {
  // The machine's first read begins at H = 74.5. Every later one begins at the
  // first point on the 256-cycle grid carried from the previous read's start that
  // lies at or past H = 32.5 of the line.
  if (state_.autoJoyStart == 0u) return lineStart + kAutoJoyFirstStart;
  const std::uint64_t earliest = lineStart + kAutoJoyEarliest;
  const std::uint64_t ahead = (earliest - state_.autoJoyStart) % kAutoJoyGrid;
  return ahead == 0u ? earliest : earliest + (kAutoJoyGrid - ahead);
}

bool Snes::autoJoypadBusy() const noexcept {
  return state_.autoJoyStart != 0u && state_.autoJoyClocked < kAutoJoyBits;
}

void Snes::clockAutoJoypad(std::uint64_t now) noexcept {
  if (!autoJoypadBusy()) return;
  // The bits whose clocks have completed by `now`: none through the strobe
  // pulse, then one every 256 cycles.
  const std::uint64_t elapsed = now > state_.autoJoyStart ? now - state_.autoJoyStart : 0u;
  const std::uint64_t landed =
      elapsed <= kAutoJoyStrobe ? 0u : (elapsed - kAutoJoyStrobe) / kAutoJoyBit;
  const auto reached = static_cast<std::uint8_t>(landed < kAutoJoyBits ? landed : kAutoJoyBits);
  // Each clock shifts every port's register up one and puts the bit it read at the
  // bottom, so the first bit read — B — is at bit 15 once all sixteen are in, and a
  // register read before then holds the previous read's bits shifted part-way out
  // above this one's shifted part-way in. The low byte is at $4218/$421A and the
  // high at $4219/$421B; the ports' second data lines — a multitap's — carry
  // nothing here, so $421C-$421F stay zero.
  while (state_.autoJoyClocked < reached) {
    for (std::size_t p = 0; p < 2; ++p) {
      const auto word = static_cast<std::uint16_t>(
          state_.joy[p * 2u] | (static_cast<std::uint16_t>(state_.joy[p * 2u + 1u]) << 8));
      const auto shifted = static_cast<std::uint16_t>((word << 1) | clockJoypad(p));
      state_.joy[p * 2u] = static_cast<std::uint8_t>(shifted & 0xFFu);
      state_.joy[p * 2u + 1u] = static_cast<std::uint8_t>(shifted >> 8);
    }
    ++state_.autoJoyClocked;
  }
}

std::uint8_t Snes::joypadPortValue(std::uint16_t offset) const noexcept {
  if (offset == 0x4016) {
    // JOYSER0: bit 0 is port 1's data line, bit 1 its second line (nothing is on
    // it); the rest is open bus.
    return static_cast<std::uint8_t>((state_.mdr & 0xFCu) | joypadBit(0));
  }
  // JOYSER1: bit 0 is port 2's data line, bit 1 its second line; bits 4-2 are
  // wired low and read as ones; the rest is open bus.
  return static_cast<std::uint8_t>((state_.mdr & 0xE0u) | 0x1Cu | joypadBit(1));
}

std::uint8_t Snes::readJoypadPort(std::uint16_t offset) {
  const std::uint8_t v = joypadPortValue(offset);
  // The read clocks the port's register; the bit it clocks out is v's bit 0.
  static_cast<void>(clockJoypad(offset == 0x4016 ? 0u : 1u));
  return latch(v);
}

void Snes::writeJoypadStrobe(std::uint8_t value) noexcept {
  // Bit 0 is the strobe line to both ports. Its fall latches the pads; while it
  // is high the pads reload continuously, which clockJoypad reads as B every time.
  const bool high = (value & 1u) != 0u;
  if (state_.joyStrobe && !high) latchJoypads();
  state_.joyStrobe = high;
}

// ---- the threads ----------------------------------------------------------------
// The flight, the worker both threads share, the audio thread, the picture thread
// and the waits. They sit after everything the console runs each cycle, which keeps
// that code together whether or not a machine has a thread.

// step() and run() inside a flight. Never inlined: step() and run() carry one test of
// the flags on a machine without a thread, and none of this.
SNAGGLETOOTH_NOINLINE std::uint32_t Snes::stepInFlight() {
  const Flight flight{*this};
  return step();
}

SNAGGLETOOTH_NOINLINE std::uint64_t Snes::runInFlight(std::uint64_t budget) {
  const Flight flight{*this};
  return run(budget);
}

Snes::Flight::Flight(Snes& machine) : m(machine) {
  if (!m.apuThreaded_ && !m.ppuThreaded_) return;
  // Only the calling thread writes the flag, so a call inside a call reads its own
  // store and leaves the flight to the call that began it.
  if (m.inFlight_.load(std::memory_order_relaxed)) return;
  outer = true;
  if (m.apuThreaded_ && !m.link_.worker.thread.joinable()) m.startApuThread();
  if (m.ppuThreaded_ && !m.picture_->worker.thread.joinable()) m.startPictureThread();
  // The flag goes up before the parked flags are read, and a thread sets parked
  // before it reads the flag under the mutex: one of the two sees the other's store,
  // so a thread parked between calls wakes as the call begins. A thread that parked
  // inside an earlier call is not woken by this one beginning but by the first thing
  // handed to it, so the nudge leaves parked up for that hand-over to see.
  m.inFlight_.store(true, std::memory_order_seq_cst);
  if (m.apuThreaded_) m.link_.worker.nudge();
  if (m.ppuThreaded_) m.picture_->worker.nudge();
}

Snes::Flight::~Flight() {
  if (!m.apuThreaded_ && !m.ppuThreaded_) return;
  m.waitApu();
  m.waitPicture();
  if (outer) m.inFlight_.store(false, std::memory_order_release);
}

// ---- the worker ----

void Snes::Worker::stopAndJoin() noexcept {
  if (!thread.joinable()) return;
  {
    // Under the mutex, so a thread between its look at `stop` and its wait cannot
    // miss it.
    const std::lock_guard<std::mutex> lock(mutex);
    stop.store(true, std::memory_order_seq_cst);
  }
  wake.notify_one();
  thread.join();
  stop.store(false, std::memory_order_relaxed);
  id = std::thread::id{};
}

bool Snes::Worker::onThread() const noexcept { return std::this_thread::get_id() == id; }

void Snes::Worker::release(std::uint64_t count) noexcept {
  released.store(released.load(std::memory_order_relaxed) + count, std::memory_order_release);
  if (parked.load(std::memory_order_relaxed)) [[unlikely]] wakeUnderMutex();
}

void Snes::Worker::need() noexcept {
  needed.store(true, std::memory_order_relaxed);
  // The console's stores come before its look at parked, and the thread sets parked
  // before its looks under the mutex: either the thread's looks see the work and the
  // need, or this look sees parked and the notify under the mutex finds it waiting.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (parked.load(std::memory_order_relaxed)) wakeUnderMutex();
}

void Snes::Worker::wakeUnderMutex() noexcept {
  const std::lock_guard<std::mutex> lock(mutex);
  // The caller made a release or a need before taking the mutex, so the thread's
  // looks see it and it wakes. Parked comes down here rather than when the thread
  // is next scheduled, so the work handed over meanwhile does not each wake it again.
  // A wake that arrives late, for work the thread has already run, finds the thread
  // parking again; it puts parked back up before it waits.
  parked.store(false, std::memory_order_relaxed);
  wake.notify_one();
}

void Snes::Worker::nudge() noexcept {
  if (parked.load(std::memory_order_seq_cst)) {
    const std::lock_guard<std::mutex> lock(mutex);
    wake.notify_one();
  }
}

void Snes::Worker::beginThread() noexcept {
  hotUntil = std::chrono::steady_clock::now() + kHotWindow;
  worked = false;
}

template <class HasWork>
bool Snes::Worker::awaitWork(const std::atomic<bool>& inFlight, HasWork hasWork) noexcept {
  using Clock = std::chrono::steady_clock;
  // The console waiting for the thread opens the window; work run while it is open
  // keeps it open; the thread reads the clock once every kClockEvery turns and parks
  // once the window has passed with nothing to run. Work that arrives with the
  // window shut is run and the thread parks again.
  std::uint32_t turns = 0;
  for (;;) {
    if (stop.load(std::memory_order_acquire)) return false;
    if (hasWork(std::memory_order_acquire)) return true;
    if (needed.load(std::memory_order_relaxed) && needed.exchange(false, std::memory_order_relaxed)) {
      hotUntil = Clock::now() + kHotWindow;
    }
    if (++turns == kClockEvery) {
      turns = 0;
      const Clock::time_point now = Clock::now();
      const bool keepOpen = worked && now < hotUntil;
      worked = false;
      if (keepOpen) {
        hotUntil = now + kHotWindow;
      } else if (now >= hotUntil) {
        park(inFlight, hasWork);
        hotUntil = Clock::time_point{};  // shut: a need opens it
        continue;
      }
    }
    cpuRelax();
  }
}

template <class HasWork>
void Snes::Worker::park(const std::atomic<bool>& inFlight, HasWork hasWork) {
  std::unique_lock<std::mutex> lock(mutex);
  // A call already in progress is no reason to wake; a call that begins once the
  // thread has parked is.
  const bool flying = inFlight.load(std::memory_order_seq_cst);
  for (;;) {
    // Parked goes up before the looks under the mutex, on every pass: work, a need
    // or a call that came before it is seen by the looks, and one that comes after
    // it sees parked and wakes the thread. A waker takes parked down, and a wake can
    // come late, for work this thread has already run; parked goes up again before
    // the thread waits again, so it never waits with parked down.
    parked.store(true, std::memory_order_seq_cst);
    if (stop.load(std::memory_order_seq_cst) ||
        (!flying && inFlight.load(std::memory_order_seq_cst)) ||
        needed.load(std::memory_order_seq_cst) || hasWork(std::memory_order_seq_cst)) {
      break;
    }
    wake.wait(lock);
  }
  parked.store(false, std::memory_order_seq_cst);
}

void Snes::Worker::notifyCaughtUp() noexcept {
  // The thread's store of `done` and this load, and the console's store of `awaited`
  // and its load of `done`, are all sequentially consistent, so one side sees the
  // other's store: either this look sees the console waiting and the notify under
  // the mutex reaches it, or the console's look sees the count and it does not wait.
  if (awaited.load(std::memory_order_seq_cst)) [[unlikely]] {
    const std::lock_guard<std::mutex> lock(mutex);
    caughtUp.notify_all();
  }
}

void Snes::Worker::waitSleeping(std::uint64_t target) noexcept {
  if (done.load(std::memory_order_acquire) >= target) return;
  need();
  for (std::uint32_t turn = 0; turn < kSleepAfterTurns; ++turn) {
    if (done.load(std::memory_order_acquire) >= target) return;
    cpuRelax();
  }
  std::unique_lock<std::mutex> lock(mutex);
  awaited.store(true, std::memory_order_seq_cst);
  while (done.load(std::memory_order_seq_cst) < target) caughtUp.wait(lock);
  awaited.store(false, std::memory_order_seq_cst);
}

// ---- the audio thread ----

void Snes::startApuThread() {
  // The id is set before the flight's flag goes up, so before the first audio cycle
  // is released and before any report of the audio machine's observer.
  link_.worker.start([this] { apuThreadBody(); });
}

void Snes::stopApuThread() noexcept { link_.worker.stopAndJoin(); }

bool Snes::onApuThread() const noexcept { return link_.worker.onThread(); }

void Snes::waitApu() const noexcept {
  if (!apuThreaded_) return;
  // The audio thread asking from inside its own run stands where it stands, and the
  // count it would publish is the console's alone.
  if (onApuThread()) return;
  publishApu();
  const std::uint64_t released = link_.worker.released.load(std::memory_order_relaxed);
  const std::uint32_t head = link_.head.load(std::memory_order_relaxed);
  if (link_.worker.done.load(std::memory_order_acquire) == released &&
      link_.tail.load(std::memory_order_acquire) == head) {
    return;
  }
  link_.worker.need();
  while (link_.worker.done.load(std::memory_order_acquire) != released ||
         link_.tail.load(std::memory_order_acquire) != head) {
    cpuRelax();
  }
}

void Snes::publishApu() const noexcept {
  if (link_.pending == 0u) return;
  link_.worker.release(link_.pending);
  link_.pending = 0;
}

void Snes::pushPortWrite(std::uint8_t index, std::uint8_t value) noexcept {
  // The count is published first, so the stamp below is the true count and the full
  // ring's wait gives the thread every cycle the writes in it are stamped with.
  publishApu();
  const std::uint32_t head = link_.head.load(std::memory_order_relaxed);
  // A full ring waits for the thread to apply the oldest write, so none is lost.
  if (head - link_.tail.load(std::memory_order_acquire) >= kPortRing) [[unlikely]] {
    link_.worker.need();
    while (head - link_.tail.load(std::memory_order_acquire) >= kPortRing) cpuRelax();
  }
  link_.ring[head % kPortRing] = PortWrite{
      .at = link_.worker.released.load(std::memory_order_relaxed),
      .index = index,
      .value = value,
  };
  // Published before the cycle's share is released, so the thread cannot run that
  // share without seeing the write.
  link_.head.store(head + 1u, std::memory_order_release);
}

void Snes::apuThreadBody() noexcept {
  Worker& worker = link_.worker;
  std::uint64_t done = worker.done.load(std::memory_order_relaxed);
  std::uint32_t tail = link_.tail.load(std::memory_order_relaxed);
  worker.beginThread();
  for (;;) {
    // Wait for work: released cycles, or a port write in the ring.
    if (!worker.awaitWork(inFlight_, [&](std::memory_order order) noexcept {
          return worker.released.load(order) != done || link_.head.load(order) != tail;
        })) {
      return;
    }
    // Run every released cycle, each port write applied when the count reaches its
    // stamp and never run past. `released` is read before `head`: a write stamped
    // below the count read is published before that count, so it is in the ring.
    for (;;) {
      const std::uint64_t released = worker.released.load(std::memory_order_acquire);
      const std::uint32_t head = link_.head.load(std::memory_order_acquire);
      while (tail != head && link_.ring[tail % kPortRing].at <= done) {
        const PortWrite& write = link_.ring[tail % kPortRing];
        apu_.writePort(write.index, write.value);
        ++tail;
        link_.tail.store(tail, std::memory_order_release);
      }
      if (done == released) break;
      std::uint64_t cycles = released - done;
      if (tail != head) cycles = std::min(cycles, link_.ring[tail % kPortRing].at - done);
      apu_.run(cycles);
      done += cycles;
      worker.done.store(done, std::memory_order_release);
      worker.notifyCaughtUp();
    }
    worker.ran();
  }
}

// ---- the picture thread ----

void Snes::startPictureThread() {
  picture_->worker.start([this] { pictureThreadBody(); });
}

void Snes::stopPictureThread() noexcept {
  if (picture_) picture_->worker.stopAndJoin();
}

void Snes::stopThreads() noexcept {
  stopApuThread();
  stopPictureThread();
}

void Snes::pushEvent(const PictureEvent& event) noexcept {
  // A watched machine waits for the thread at every frame report, so the thread is
  // never more than a frame of entries behind it. The most a frame can log is a
  // transfer writing the chip every eight master cycles of the longest frame, plus
  // its lines and the beam's entry, and the ring holds more: a watched machine never
  // waits on a slot. The wait below is for an unwatched machine whose thread the
  // host starves.
  static_assert(kEventRing > kLongLineMaster * (kPalLines + 1u) / 8u + kPalLines + 2u,
                "the event ring holds more entries than the longest frame can log");
  PictureLink& picture = *picture_;
  const std::uint64_t index = picture.worker.released.load(std::memory_order_relaxed);
  // A full ring waits, sleeping, for the thread to apply the entry this one takes
  // the slot of, so none is lost.
  if (index >= kEventRing) [[unlikely]] picture.worker.waitSleeping(index + 1u - kEventRing);
  picture.events[static_cast<std::size_t>(index % kEventRing)] = event;
  picture.worker.release(1u);
}

void Snes::pushAccess(PictureEventKind kind, std::uint16_t offset, std::uint8_t value,
                      const PpuInputs& in) noexcept {
  pushEvent(PictureEvent{.offset = offset,
                         .hpos = state_.hpos,
                         .hdot = in.hdot,
                         .vpos = in.vpos,
                         .field = in.field,
                         .vblank = in.vblank,
                         .extLatch = in.extLatch,
                         .lateHalf = in.lateHalf,
                         .kind = kind,
                         .value = value,
                         .byte0 = 0u,
                         .byte1 = 0u});
}

void Snes::stageLine() noexcept {
  // A watched machine waits for the thread at every frame report, so the thread is
  // never more than a frame of lines behind it, and the ring holds more than the
  // longest frame: a watched machine never waits on a slot.
  static_assert(kLineRing > kPalLines + 1u, "the line ring holds more lines than the longest frame");
  PictureLink& picture = *picture_;
  const std::size_t slot = static_cast<std::size_t>(picture.linesPushed % kLineRing);
  // A slot whose last line the thread has not taken yet waits, sleeping, for it.
  if (picture.lineEvent[slot] != 0u) picture.worker.waitSleeping(picture.lineEvent[slot]);
  picture.lines[slot] = state_.ppu.sprites;
}

void Snes::pushLine() noexcept {
  PictureLink& picture = *picture_;
  const std::size_t slot = static_cast<std::size_t>(picture.linesPushed % kLineRing);
  ++picture.linesPushed;
  picture.lineEvent[slot] = picture.worker.released.load(std::memory_order_relaxed) + 1u;
  pushEvent(PictureEvent{.offset = state_.ppu.mosaicBlockLine,
                         .hpos = static_cast<std::uint16_t>(slot),
                         .hdot = 0u,
                         .vpos = state_.vpos,
                         .field = state_.field,
                         .vblank = state_.inVblank,
                         .extLatch = false,
                         .lateHalf = false,
                         .kind = PictureEventKind::Line,
                         .value = state_.ppu.mosaicBlockSize,
                         .byte0 = state_.ppu.objsel,
                         .byte1 = state_.ppu.setini});
}

void Snes::waitPicture() noexcept {
  if (!ppuThreaded_) return;
  PictureLink& picture = *picture_;
  // Before the machine's first call, and after a move until the next, there is no
  // thread: nothing has been logged, the view is the state's, and there is nothing
  // to wait for.
  if (!picture.worker.thread.joinable()) return;
  // The thread draws to the beam before the console looks at the picture.
  pushEvent(PictureEvent{.offset = 0u,
                         .hpos = state_.hpos,
                         .hdot = 0u,
                         .vpos = 0u,
                         .field = 0u,
                         .vblank = false,
                         .extLatch = false,
                         .lateHalf = false,
                         .kind = PictureEventKind::Beam,
                         .value = 0u,
                         .byte0 = 0u,
                         .byte1 = 0u});
  picture.worker.waitSleeping(picture.worker.released.load(std::memory_order_relaxed));
  // The two fields the picture writes, which nothing on this thread writes.
  state_.ppu.tiles = picture.view.tiles;
  state_.ppu.lastMain = picture.view.lastMain;
}

void Snes::drawTo(std::uint16_t hpos) {
  PictureLink& picture = *picture_;
  Ppu ppu{picture.view, picture.derived};
  // A pending line is drawn from its first read dot to `hpos`, whichever of its
  // dots have gone by undrawn; any other line draws on from where it stands.
  if (cache_->pending) {
    settleLine(ppu, picture.in, hpos > picture.drawn ? hpos : picture.drawn, false);
  } else if (hpos > picture.drawn) {
    drawDots(ppu, picture.in, picture.drawn, hpos);
  }
  if (hpos > picture.drawn) picture.drawn = hpos;
}

void Snes::applyPictureEvent(const PictureEvent& event) {
  PictureLink& picture = *picture_;
  switch (event.kind) {
    case PictureEventKind::Line: {
      // The rest of the line the thread is on, unless the frame before stands in for
      // it, and the line's record or completion; then the new line's facts: the line
      // vertical blank begins on settles the finished picture's width and its line
      // counts.
      if (!cache_->pending) drawTo(kLineMaster);
      Ppu ending{picture.view, picture.derived};
      completeLine(ending, picture.in);
      const bool blankBegins = event.vblank && !picture.in.vblank;
      picture.in.vpos = event.vpos;
      picture.in.field = event.field;
      picture.in.vblank = event.vblank;
      picture.view.sprites = picture.lines[event.hpos];
      picture.view.mosaicBlockLine = event.offset;
      picture.view.mosaicBlockSize = event.value;
      picture.drawn = 0u;
      // The timeline takes Time's inputs for the new line, or finishes the frame as
      // its blank begins. The view holds every register as Time read it: each write
      // before the line's start is replayed on it before this entry.
      if (recordsTimeline_) [[unlikely]] {
        if (blankBegins) {
          finishTimeline(picture.view, event.vpos, event.field);
        } else if (!event.vblank) {
          recordTime(picture.view, event.vpos, event.field, event.byte0, event.byte1);
        }
      }
      if (blankBegins) {
        finishPicture();
        cache_->last = cache_->frame;
        cache_->frame = LineReuse{};
      }
      return;
    }
    case PictureEventKind::Write:
    case PictureEventKind::Read: {
      // The access replayed on the view with the pins it took, every dot up to its
      // position drawn first, as the console drew them — unless the line is pending
      // and the access changes nothing its dots read, where it stays pending.
      const PpuInputs in{.hdot = event.hdot,
                         .vpos = event.vpos,
                         .field = event.field,
                         .vblank = event.vblank,
                         .pal = picture.in.pal,
                         .extLatch = event.extLatch,
                         .lateHalf = event.lateHalf};
      Ppu ppu{picture.view, picture.derived};
      if (event.kind == PictureEventKind::Write) {
        const bool disturbs = writeDisturbs(event.offset, event.hpos, picture.view, in);
        if (disturbs || !cache_->pending) drawTo(event.hpos);
        if (disturbs) settleLine(ppu, picture.in, event.hpos, true);
        const bool changes = videoWriteChanges(ppu, event.offset, event.value);
        if (recordsTimeline_) [[unlikely]] recordBeforeWrite(event.offset, picture.view, in);
        const std::optional<std::uint16_t> landed = ppu.write(event.offset, event.value, in);
        noteVideoWrite(event.offset, landed, changes);
        if (recordsTimeline_) [[unlikely]] {
          recordWrite(event.offset, event.value, event.hpos, picture.view, in, landed);
        }
      } else {
        if (!cache_->pending) drawTo(event.hpos);
        static_cast<void>(ppu.read(event.offset, in));
      }
      return;
    }
    case PictureEventKind::InidispEarly: {
      // The one dot the write's cycle ended on, drawn again under the bus byte; the
      // write that owes it has drawn and marked the line where the dot is one of
      // the line's, and a line still pending has no dot of its own to draw again.
      if (!cache_->pending) drawTo(event.hpos);
      const std::uint8_t written = picture.view.inidisp;
      picture.view.inidisp = event.byte0;
      Ppu ppu{picture.view, picture.derived};
      drawDots(ppu, picture.in, static_cast<std::uint16_t>(event.hpos - 4u), event.hpos);
      picture.view.inidisp = written;
      if (recordsTimeline_) [[unlikely]] {
        recordInidispEarly(picture.in.vpos, event.hpos, event.byte0, written);
      }
      return;
    }
    case PictureEventKind::Beam: {
      // The console is about to read the picture's fields: the line stands drawn to
      // the beam, a line this draw found the frame before could stand in for
      // included.
      drawTo(event.hpos);
      if (cache_->pending) {
        Ppu drawing{picture.view, picture.derived};
        settleLine(drawing, picture.in, picture.drawn, false);
      }
      return;
    }
  }
}

void Snes::pictureThreadBody() noexcept {
  Worker& worker = picture_->worker;
  std::uint64_t done = worker.done.load(std::memory_order_relaxed);
  worker.beginThread();
  for (;;) {
    if (!worker.awaitWork(inFlight_, [&](std::memory_order order) noexcept {
          return worker.released.load(order) != done;
        })) {
      return;
    }
    // Every entry logged, in order, the count published after each so a console
    // waiting on a ring, or sleeping for the picture, sees it move.
    std::uint64_t released = worker.released.load(std::memory_order_acquire);
    while (done != released) {
      applyPictureEvent(picture_->events[static_cast<std::size_t>(done % kEventRing)]);
      ++done;
      worker.done.store(done, std::memory_order_seq_cst);
      worker.notifyCaughtUp();
      if (done == released) released = worker.released.load(std::memory_order_acquire);
    }
    worker.ran();
  }
}

void Snes::setApuObserver(ApuObserver* observer) noexcept {
  waitApu();
  apu_.setObserver(observer);
}

// ---- a line taken from the frame before --------------------------------------

Snes::LineKey Snes::lineKeyOf(const PpuState& s, const PpuInputs& in) noexcept {
  LineKey key;
  key.inidisp = s.inidisp;
  key.bgmode = s.bgmode;
  key.mosaic = s.mosaic;
  key.bg1sc = s.bg1sc;
  key.bg2sc = s.bg2sc;
  key.bg3sc = s.bg3sc;
  key.bg4sc = s.bg4sc;
  key.bg12nba = s.bg12nba;
  key.bg34nba = s.bg34nba;
  key.setini = s.setini;
  key.bg1hofs = s.bg1hofs;
  key.bg1vofs = s.bg1vofs;
  key.bg2hofs = s.bg2hofs;
  key.bg2vofs = s.bg2vofs;
  key.bg3hofs = s.bg3hofs;
  key.bg3vofs = s.bg3vofs;
  key.bg4hofs = s.bg4hofs;
  key.bg4vofs = s.bg4vofs;
  key.m7sel = s.m7sel;
  key.m7a = s.m7a;
  key.m7b = s.m7b;
  key.m7c = s.m7c;
  key.m7d = s.m7d;
  key.m7x = s.m7x;
  key.m7y = s.m7y;
  key.m7hofs = s.m7hofs;
  key.m7vofs = s.m7vofs;
  key.w12sel = s.w12sel;
  key.w34sel = s.w34sel;
  key.wobjsel = s.wobjsel;
  key.wh0 = s.wh0;
  key.wh1 = s.wh1;
  key.wh2 = s.wh2;
  key.wh3 = s.wh3;
  key.wbglog = s.wbglog;
  key.wobjlog = s.wobjlog;
  key.tm = s.tm;
  key.ts = s.ts;
  key.tmw = s.tmw;
  key.tsw = s.tsw;
  key.cgwsel = s.cgwsel;
  key.cgadsub = s.cgadsub;
  key.fixedRed = s.fixedRed;
  key.fixedGreen = s.fixedGreen;
  key.fixedBlue = s.fixedBlue;
  key.mosaicBlockLine = s.mosaicBlockLine;
  key.mosaicBlockSize = s.mosaicBlockSize;
  const std::uint8_t mode = s.bgmode & 0x07u;
  key.field = (mode == 5u || mode == 6u) && s.interlace() ? in.field : std::uint8_t{0};
  if (s.forcedBlank()) {
    key.lastMainIn = s.lastMain;
    key.lastMainIn.present = false;  // the blank dots clear it whatever it was
  }
  return key;
}

namespace {

// Whether a page or group in `mask` was written after `drawnAt`: its stamp is past
// the count the line was read under.
template <std::size_t N>
[[nodiscard]] bool writtenSince(std::uint64_t mask, const std::array<std::uint64_t, N>& stamps,
                                std::uint64_t drawnAt) noexcept {
  while (mask != 0u) {
    const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
    if (stamps[bit] > drawnAt) return true;
    mask &= mask - 1u;
  }
  return false;
}

}  // namespace

void Snes::decideLine(Ppu& ppu, const PpuInputs& in, std::uint16_t fromHpos) noexcept {
  LineCache& cache = *cache_;
  const PpuState& s = ppu.s_;
  cache.decided = true;
  cache.pending = false;
  cache.disturbed = false;
  cache.predicted = false;
  cache.scrollOnly = false;
  cache.drawnInHalfPixels = false;
  // A span that begins past the first read dot is a line this side did not draw
  // from its start — a restore put the beam part-way along it — so what it leaves
  // is not kept, and nothing stands in for it.
  cache.recordable = fromHpos / 4u + 1u <= kFirstTileReadDot;
  cache.key = lineKeyOf(s, in);
  // The timeline records the line as its first read dot finds it, when this side
  // draws it from its start.
  if (recordsTimeline_ && cache.recordable) [[unlikely]] recordLine(s, in);
  cache.drawnAt = cache.writes;
  const LineRecord& record = cache.lines[static_cast<std::size_t>(in.vpos) - 1u];

  bool hit = false;
  if (!cache.recordable || !record.valid) {
    cache.reason = LineReason::FirstDrawn;
  } else if (record.key != cache.key) {
    cache.reason = LineReason::Registers;
    // Whether the ten scroll words are the whole of the difference.
    LineKey moved = record.key;
    moved.bg1hofs = cache.key.bg1hofs;
    moved.bg1vofs = cache.key.bg1vofs;
    moved.bg2hofs = cache.key.bg2hofs;
    moved.bg2vofs = cache.key.bg2vofs;
    moved.bg3hofs = cache.key.bg3hofs;
    moved.bg3vofs = cache.key.bg3vofs;
    moved.bg4hofs = cache.key.bg4hofs;
    moved.bg4vofs = cache.key.bg4vofs;
    moved.m7hofs = cache.key.m7hofs;
    moved.m7vofs = cache.key.m7vofs;
    cache.scrollOnly = moved == cache.key;
  } else if (record.spriteLine != s.sprites.line || record.spriteWord != s.sprites.word ||
             record.spritePriority != s.sprites.priority) {
    cache.reason = LineReason::Sprites;
  } else if (writtenSince(record.vramPages, cache.vramStamp, record.drawnAt)) {
    cache.reason = LineReason::Vram;
  } else if (writtenSince(record.cgramGroups, cache.cgramStamp, record.drawnAt)) {
    cache.reason = LineReason::Cgram;
  } else if (frameObserver_ != nullptr && !record.watched) {
    cache.reason = LineReason::FirstDrawn;  // the record has no pixels to give a watched machine
  } else {
    hit = true;
    cache.reason = LineReason::Returned;
  }
  // The line's own reads are recorded afresh.
  ppu.d_.vramPages = 0u;
  ppu.d_.cgramGroups = 0u;
  if (!hit) return;
  // The record stands for the line. Under the verification mode the line is drawn
  // and the two compared; otherwise nothing is drawn, and the line takes its
  // outputs from the record at its end unless a write disturbs it first.
  if (ppuVerify_) {
    cache.predicted = true;
    return;
  }
  cache.pending = true;
  cache.pendingFrom = fromHpos;
}

bool Snes::writeDisturbs(std::uint16_t offset, std::uint16_t hpos, const PpuState& s,
                         const PpuInputs& pins) noexcept {
  // The first dot the write reaches, and whether a dot of the line's reads or pixels
  // lies at or after it.
  const unsigned reaches = hpos / 4u + (offset == 0x2100u ? 0u : 1u);
  if (reaches < kFirstTileReadDot || reaches > kLastPictureDot) return false;
  if (offset == 0x2100u || (offset >= 0x2105u && offset <= 0x2114u) ||
      (offset >= 0x211Au && offset <= 0x2120u) || (offset >= 0x2123u && offset <= 0x2133u)) {
    return true;
  }
  if (offset == 0x2122u) return !s.forcedBlank() && Ppu::cgramReachable(s, pins);
  return false;
}

void Snes::settleLine(Ppu& ppu, const PpuInputs& in, std::uint16_t hpos, bool disturbing) noexcept {
  LineCache& cache = *cache_;
  if (cache.pending) {
    // Past the last picture dot nothing of the line is left to draw, and a write
    // landing there may already have changed a register the line's dots read, so
    // the line is complete, from the record, as at its end. Before it, or for the
    // write that reaches its last dot, the line is drawn from its first read dot up
    // to the beam, from the state as it still stands, and draws on dot by dot.
    if (!disturbing && hpos >= 4u * kLastPictureDot) {
      completeLine(ppu, in);
      return;
    }
    cache.pending = false;
    drawDots(ppu, in, cache.pendingFrom, hpos);
  }
  if (disturbing && cache.decided) cache.disturbed = true;
}

void Snes::completeLine(Ppu& ppu, const PpuInputs& in) noexcept {
  LineCache& cache = *cache_;
  if (!cache.decided) return;
  PpuState& s = ppu.s_;
  Ppu::Derived& d = ppu.d_;
  LineRecord& record = cache.lines[static_cast<std::size_t>(in.vpos) - 1u];
  const std::size_t line = static_cast<std::size_t>(in.vpos) - 1u;
  LineReuse& counts = cache.frame;

  if (cache.pending) {
    // Taken whole from the frame before: the tile reads and the last decision as
    // that line left them, with the derived view's knowledge of the columns, and on
    // a watched machine its pixels, at the frame's current width, with the frame
    // widened where a picture dot of the record's line was drawn in half-pixels.
    s.tiles = record.tiles;
    d.nothing = record.nothing;
    d.allNothing = record.allNothing;
    d.nothingKnown = true;
    s.lastMain = record.lastMain;
    if (frameObserver_ != nullptr) {
      if (raster_.empty()) startRaster();
      if (record.hires && !frameWide_) widenFrame(line, 0u);
      if (frameWide_) {
        std::copy(record.pixels.begin(), record.pixels.end(),
                  raster_.data() + line * kHiresWidth * kPixelBytes);
      } else {
        for (std::size_t x = 0; x < kPictureWidth; ++x) {
          const std::uint8_t* const kept = record.pixels.data() + (x * 2u + 1u) * kPixelBytes;
          std::copy(kept, kept + kPixelBytes, raster_.data() + (line * kPictureWidth + x) * kPixelBytes);
        }
      }
    }
    ++counts.reused;
  } else if (cache.disturbed) {
    // Drawn to its end from a write that landed inside it: not what the line's key
    // alone produces, so not kept. The record of an earlier frame's undisturbed
    // line stands: what it read and produced is as true as it was, and the next
    // frame's comparison says whether it still applies.
    ++counts.drawn;
    ++counts.midLine;
  } else if (!cache.recordable) {
    ++counts.drawn;
    ++counts.firstDrawn;
  } else if (cache.reason == LineReason::Returned) {
    // The frame before stood in for the line and a call's return drew it: what it
    // drew is what the record holds, so the record stands as it is.
    ++counts.drawn;
    ++counts.returned;
  } else {
    if (cache.predicted) {
      // The frame before could have stood in for the line: what the line drew is held
      // to what that frame recorded.
      bool same = s.tiles == record.tiles && d.nothing == record.nothing &&
                  d.allNothing == record.allNothing && s.lastMain == record.lastMain;
      if (same && frameObserver_ != nullptr) {
        for (std::size_t x = 0; x < kPictureWidth && same; ++x) {
          const std::uint8_t* const kept = record.pixels.data() + x * 2u * kPixelBytes;
          if (frameWide_) {
            const std::uint8_t* const drawn = raster_.data() + (line * kHiresWidth + x * 2u) * kPixelBytes;
            same = std::equal(drawn, drawn + 2u * kPixelBytes, kept);
          } else {
            const std::uint8_t* const drawn = raster_.data() + (line * kPictureWidth + x) * kPixelBytes;
            same = std::equal(drawn, drawn + kPixelBytes, kept + kPixelBytes);
          }
        }
      }
      ++counts.reused;
      if (!same) ++counts.mismatches;
    } else {
      ++counts.drawn;
      switch (cache.reason) {
        case LineReason::FirstDrawn: ++counts.firstDrawn; break;
        case LineReason::Registers:
          ++counts.registers;
          if (cache.scrollOnly) ++counts.scrollOnly;
          break;
        case LineReason::Sprites: ++counts.sprites; break;
        case LineReason::Vram: ++counts.vram; break;
        case LineReason::Cgram: ++counts.cgram; break;
        case LineReason::Returned: break;  // counted above, the record kept
      }
    }
    // The line as drawn, for the next frame's same line.
    record.key = cache.key;
    record.spriteWord = s.sprites.word;
    record.spritePriority = s.sprites.priority;
    record.spriteLine = s.sprites.line;
    record.vramPages = d.vramPages;
    record.cgramGroups = d.cgramGroups;
    record.drawnAt = cache.drawnAt;
    record.tiles = s.tiles;
    record.nothing = d.nothing;
    record.allNothing = d.allNothing;
    record.lastMain = s.lastMain;
    record.hires = cache.drawnInHalfPixels;
    record.watched = frameObserver_ != nullptr;
    if (record.watched) {
      // Both halves of every position: a narrow line's pixel fills both.
      for (std::size_t x = 0; x < kPictureWidth; ++x) {
        std::uint8_t* const kept = record.pixels.data() + x * 2u * kPixelBytes;
        if (frameWide_) {
          const std::uint8_t* const drawn = raster_.data() + (line * kHiresWidth + x * 2u) * kPixelBytes;
          std::copy(drawn, drawn + 2u * kPixelBytes, kept);
        } else {
          const std::uint8_t* const drawn = raster_.data() + (line * kPictureWidth + x) * kPixelBytes;
          std::copy(drawn, drawn + kPixelBytes, kept);
          std::copy(drawn, drawn + kPixelBytes, kept + kPixelBytes);
        }
      }
    }
    record.valid = true;
  }
  cache.decided = false;
  cache.pending = false;
  cache.disturbed = false;
  cache.predicted = false;
}

bool Snes::videoWriteChanges(Ppu& ppu, std::uint16_t offset, std::uint8_t value) noexcept {
  const PpuState& s = ppu.s_;
  if (offset == 0x2118u || offset == 0x2119u) {
    const std::size_t byte = (static_cast<std::size_t>(ppu.vramWordAddress() & 0x7FFFu) << 1) +
                             (offset == 0x2119u ? 1u : 0u);
    return s.vram[byte] != value;
  }
  if (offset == 0x2122u && s.cgLatchHigh) {
    const std::size_t byte = static_cast<std::size_t>(s.cgadd) << 1;
    return s.cgram[byte] != s.cgLatch || s.cgram[byte + 1u] != (value & 0x7Fu);
  }
  return false;
}

void Snes::noteVideoWrite(std::uint16_t offset, std::optional<std::uint16_t> landed,
                          bool changes) noexcept {
  if (!landed.has_value() || !changes) return;
  LineCache& cache = *cache_;
  if (offset == 0x2118u || offset == 0x2119u) {
    cache.vramStamp[*landed >> 9] = ++cache.writes;  // the word's byte address is twice the word, its page that / 1024
  } else if (offset == 0x2122u) {
    cache.cgramStamp[*landed >> 4] = ++cache.writes;
  }
}

void Snes::dropLines() noexcept {
  LineCache& cache = *cache_;
  for (LineRecord& record : cache.lines) record.valid = false;
  cache.decided = false;
  cache.pending = false;
  cache.disturbed = false;
  cache.predicted = false;
}

Snes::LineReuse Snes::lineReuse() noexcept {
  waitPicture();
  return cache_->last;
}

// ---- the picture as a timeline -----------------------------------------------

namespace {

// Whether a position the chip was handed lies on a picture line, where the
// timeline records it.
[[nodiscard]] bool onPictureLine(const PpuInputs& pins) noexcept {
  return pins.vpos != 0u && pins.vpos <= kTallestPicture && !pins.vblank;
}

// A version of a memory: appended to `versions` when it differs from the last
// one there, and its index answered either way.
template <std::size_t N>
[[nodiscard]] std::uint32_t versionOf(std::vector<std::uint8_t>& versions,
                                      const std::array<std::uint8_t, N>& memory) {
  if (versions.size() < N ||
      !std::equal(memory.begin(), memory.end(), versions.end() - static_cast<std::ptrdiff_t>(N))) {
    versions.insert(versions.end(), memory.begin(), memory.end());
  }
  return static_cast<std::uint32_t>(versions.size() / N - 1u);
}

// Whether a picture position is drawn in half-pixels under a mode and SETINI.
[[nodiscard]] bool halfPixels(std::uint32_t bgmode, std::uint32_t setini) noexcept {
  const std::uint32_t mode = bgmode & 0x07u;
  return mode == 5u || mode == 6u || (setini & 0x08u) != 0u;
}

}  // namespace

void Snes::recordTime(const PpuState& s, std::uint16_t line, std::uint8_t field, std::uint8_t objsel,
                      std::uint8_t setini) noexcept {
  TimeInputs& time = timeline_->time;
  time.valid = true;
  time.line = line;
  time.gathered = !s.forcedBlank();
  time.field = field;
  time.objsel = objsel;
  time.setini = setini;
  time.found = s.sprites.found;
  time.first = s.sprites.first;
  time.inRange = s.sprites.inRange;
  time.oam = s.oam;
}

void Snes::recordLine(const PpuState& s, const PpuInputs& in) noexcept {
  TimelineRecorder& recorder = *timeline_;
  TimelineFrame& frame = recorder.recording;
  const TimeInputs& time = recorder.time;
  PpuTimeline::Line line{};
  std::array<std::uint32_t, 32>& words = line.words;
  timeline::packRegisters(s, words);

  // Time's run as the line began, when it was this side's to see: Time gathered
  // from its inputs, or gathered nothing and left a buffer that is another line's,
  // or one this line shows that no Time of this frame gathered, which is carried.
  const bool timed = time.valid && time.line == in.vpos;
  PpuTimeline::SpriteSource source = PpuTimeline::SpriteSource::None;
  if (timed && time.gathered) {
    source = PpuTimeline::SpriteSource::Gathered;
  } else if (s.sprites.line == in.vpos) {
    source = PpuTimeline::SpriteSource::Carried;
  }
  words[16] = in.field;
  if (source == PpuTimeline::SpriteSource::Gathered) {
    words[16] |= static_cast<std::uint32_t>(time.objsel) << 8 |
                 static_cast<std::uint32_t>(time.setini) << 16 |
                 static_cast<std::uint32_t>(time.found) << 24;
    words[17] = time.first;
    for (unsigned nth = 0u; nth < kSpritesPerLine; ++nth) {
      words[24u + nth / 4u] |= static_cast<std::uint32_t>(time.inRange[nth]) << (8u * (nth % 4u));
    }
  }
  words[17] |= versionOf(frame.palettes, s.cgram) << 16;
  words[18] = versionOf(frame.oams, timed ? time.oam : s.oam);
  words[21] = in.vpos;
  words[22] = static_cast<std::uint32_t>(source);
  if (source == PpuTimeline::SpriteSource::Carried) {
    words[23] = static_cast<std::uint32_t>(frame.spriteBuffers.size() / PpuTimeline::kSpriteBufferBytes);
    frame.spriteBuffers.insert(frame.spriteBuffers.end(), s.sprites.word.begin(), s.sprites.word.end());
    frame.spriteBuffers.insert(frame.spriteBuffers.end(), s.sprites.priority.begin(),
                               s.sprites.priority.end());
  }
  frame.lines.push_back(line);
}

void Snes::recordBeforeWrite(std::uint16_t offset, const PpuState& s, const PpuInputs& pins) noexcept {
  TimelineRecorder& recorder = *timeline_;
  recorder.paletteHalf = offset == 0x2122u && s.cgLatchHigh;
  // The frame's base is VRAM as the first write of its picture lines finds it.
  if ((offset == 0x2118u || offset == 0x2119u) && !recorder.baseTaken && onPictureLine(pins) &&
      Ppu::vramReachable(s, pins)) {
    recorder.recording.vram.assign(s.vram.begin(), s.vram.end());
    recorder.baseTaken = true;
  }
}

void Snes::recordWrite(std::uint16_t offset, std::uint8_t value, std::uint16_t hpos, const PpuState& s,
                       const PpuInputs& pins, std::optional<std::uint16_t> landed) noexcept {
  if (!onPictureLine(pins)) return;
  TimelineRecorder& recorder = *timeline_;
  TimelineFrame& frame = recorder.recording;
  // The first dot the write reaches: the one after the dot its cycle ends on.
  const std::uint32_t dot = hpos / 4u + 1u;
  const std::uint32_t at = timeline::at(pins.vpos, dot);
  if (offset == 0x2118u || offset == 0x2119u) {
    if (!landed.has_value()) return;
    const std::uint32_t address = (static_cast<std::uint32_t>(*landed) << 1) + (offset == 0x2119u ? 1u : 0u);
    frame.vramWrites.push_back(PpuTimeline::VramWrite{.at = at, .change = address | static_cast<std::uint32_t>(value) << 16});
    return;
  }
  if (dot < kFirstTileReadDot || dot > kLastPictureDot) return;
  if (offset == 0x2122u) {
    if (!recorder.paletteHalf || !landed.has_value()) return;
    const std::size_t byte = static_cast<std::size_t>(*landed) << 1;
    const std::uint32_t word = s.cgram[byte] | static_cast<std::uint32_t>(s.cgram[byte + 1u]) << 8;
    frame.events.push_back(PpuTimeline::Event{
        .at = at,
        .change = static_cast<std::uint32_t>(PpuTimeline::Field::PaletteWord) |
                  static_cast<std::uint32_t>(*landed) << 8 | word << 16});
    return;
  }
  const timeline::Changed changed = timeline::fieldsOf(offset, value);
  for (unsigned index = 0u; index < changed.count; ++index) {
    const PpuTimeline::Field field = changed.fields[index];
    frame.events.push_back(PpuTimeline::Event{
        .at = at,
        .change = static_cast<std::uint32_t>(field) | timeline::valueOf(s, field) << 16});
  }
}

void Snes::recordInidispEarly(std::uint16_t line, std::uint16_t hpos, std::uint8_t busBefore,
                              std::uint8_t inidisp) noexcept {
  if (line == 0u || line > kTallestPicture) return;
  // The write cycle's last dot is drawn under the bus byte, and the register as the
  // write left it stands from the dot after.
  const std::uint32_t dot = hpos / 4u;
  if (dot < kFirstTileReadDot || dot > kLastPictureDot) return;
  std::vector<PpuTimeline::Event>& events = timeline_->recording.events;
  const auto inidispAt = [](std::uint32_t at, std::uint8_t value) {
    return PpuTimeline::Event{.at = at,
                              .change = static_cast<std::uint32_t>(PpuTimeline::Field::Inidisp) |
                                        static_cast<std::uint32_t>(value) << 16};
  };
  const PpuTimeline::Event early = inidispAt(timeline::at(line, dot), busBefore);
  const std::uint32_t after = timeline::at(line, dot + 1u);
  if (!events.empty() && events.back().at == after && events.back().field() == PpuTimeline::Field::Inidisp) {
    // The write's own event, made as it landed, stands after this one.
    events.push_back(events.back());
    events[events.size() - 2u] = early;
    return;
  }
  // A write the watch stored nothing for, or one reaching past the last picture
  // dot: the register as it stands follows the early dot where a dot follows it.
  events.push_back(early);
  if (dot + 1u <= kLastPictureDot) events.push_back(inidispAt(after, inidisp));
}

void Snes::finishTimeline(const PpuState& s, std::uint16_t blankLine, std::uint8_t field) noexcept {
  TimelineRecorder& recorder = *timeline_;
  TimelineFrame& frame = recorder.recording;
  // Each line's events found, in order, and every event for a line that was not
  // recorded dropped; and whether a picture dot of a recorded line is drawn in
  // half-pixels, which makes the frame 512 wide.
  std::size_t read = 0u;
  std::size_t kept = 0u;
  bool wide = false;
  for (PpuTimeline::Line& line : frame.lines) {
    const std::uint32_t number = line.lineNumber();
    while (read < frame.events.size() && frame.events[read].line() < number) ++read;
    line.words[19] = static_cast<std::uint32_t>(kept);
    std::uint32_t bgmode = line.value(PpuTimeline::Field::Bgmode);
    std::uint32_t setini = line.value(PpuTimeline::Field::Setini);
    bool atFirstPixel = false;
    while (read < frame.events.size() && frame.events[read].line() == number) {
      const PpuTimeline::Event event = frame.events[read++];
      if (!atFirstPixel && event.dot() > kFirstPictureDot) {
        atFirstPixel = true;
        wide = wide || halfPixels(bgmode, setini);
      }
      if (event.field() == PpuTimeline::Field::Bgmode) bgmode = event.value();
      if (event.field() == PpuTimeline::Field::Setini) setini = event.value();
      if (atFirstPixel) wide = wide || halfPixels(bgmode, setini);
      frame.events[kept++] = event;
    }
    if (!atFirstPixel) wide = wide || halfPixels(bgmode, setini);
    line.words[20] = static_cast<std::uint32_t>(kept) - line.words[19];
  }
  frame.events.resize(kept);
  // VRAM as the picture lines left it, where no write of them took the base.
  if (!frame.lines.empty() && !recorder.baseTaken) frame.vram.assign(s.vram.begin(), s.vram.end());
  frame.height = static_cast<unsigned>(blankLine) - 1u;
  frame.width = wide ? kHiresWidth : kPictureWidth;
  frame.field = field;
  std::swap(recorder.recording, recorder.finished);
  recorder.recording.clear();
  recorder.baseTaken = false;
}

void Snes::dropTimeline() noexcept {
  timeline_->recording.clear();
  timeline_->time.valid = false;
  timeline_->baseTaken = false;
}

std::uint8_t Snes::peekApu(std::uint16_t address) const noexcept {
  waitApu();
  return apu_.peek(address);
}

std::optional<std::uint8_t> Snes::peekApuRegister(std::uint16_t address) const noexcept {
  waitApu();
  return apu_.peekRegister(address);
}

}  // namespace snaggletooth
