#include "rom/rom_buffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace snaggletooth::disasm {

std::size_t payloadBytes(RecordKind kind, const std::uint8_t* payload) noexcept {
  switch (kind) {
    case RecordKind::Access: return sizeof(BusAccess);
    case RecordKind::EngineAccess: return sizeof(EngineAccessRecord);
    case RecordKind::Internal: return sizeof(InternalRecord);
    case RecordKind::Boundary: return sizeof(BoundaryRecord);
    case RecordKind::FrameStart: return sizeof(FrameStartRecord);
    case RecordKind::Keyframe: {
      KeyframeHeader header;
      std::memcpy(&header, payload, sizeof header);
      return sizeof header + header.bytes;
    }
    case RecordKind::Reset: return sizeof(ResetRecord);
    case RecordKind::End: return 0;
    case RecordKind::SoundAccess: return sizeof(SoundAccessRecord);
    case RecordKind::SoundStep: return sizeof(SoundStepRecord);
    case RecordKind::KeyOnMemory: return 65536u;
    case RecordKind::SoundReset: return sizeof(SoundResetRecord);
    case RecordKind::SoundEnd: return 0;
  }
  return 0;
}

bool putRecord(Stream& stream, RecordKind kind) noexcept {
  std::uint8_t* const at = stream.put(1u);
  if (at == nullptr) return false;
  *at = static_cast<std::uint8_t>(kind);
  return true;
}

std::uint8_t* putBytes(Stream& stream, RecordKind kind, std::size_t bytes) noexcept {
  std::uint8_t* const at = stream.put(1u + bytes);
  if (at == nullptr) return nullptr;
  *at = static_cast<std::uint8_t>(kind);
  return at + 1;
}

namespace {

thread_local ThreadClass threadClass = ThreadClass::Normal;

}  // namespace

void setThisThreadClass(ThreadClass which) noexcept {
  threadClass = which;
#if defined(__APPLE__)
  qos_class_t qos = QOS_CLASS_DEFAULT;
  if (which == ThreadClass::Lowered) qos = QOS_CLASS_BACKGROUND;
  if (which == ThreadClass::LoweredForFiles) qos = QOS_CLASS_UTILITY;
  pthread_set_qos_class_self_np(qos, 0);
#elif defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), which == ThreadClass::Normal ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
#elif defined(SCHED_IDLE)
  sched_param param{};
  param.sched_priority = 0;
  pthread_setschedparam(pthread_self(), which == ThreadClass::Normal ? SCHED_OTHER : SCHED_IDLE, &param);
#endif
}

ThreadClass thisThreadClass() noexcept { return threadClass; }

bool StreamBudget::atLimit() const noexcept {
  const std::uint64_t charged = held() + (open_.load(std::memory_order_acquire) + 1u) * Stream::kBlockBytes;
  return charged > memoryBytes() + diskBytes();
}

bool StreamBudget::memoryOver() const noexcept {
  return memoryUsed() > memoryBytes() + (open_.load(std::memory_order_acquire) + 1u) * Stream::kBlockBytes &&
         diskUsed() + Stream::kBlockBytes <= diskBytes();
}

ThreadClass ThreadClassKeeper::wanted() const noexcept {
  return budget_.hostStopped() ? ThreadClass::Normal : lowered_;
}

bool ThreadClassKeeper::due() const noexcept { return wanted() != thisThreadClass(); }

void ThreadClassKeeper::keep() noexcept {
  const ThreadClass which = wanted();
  if (which == thisThreadClass()) return;
  setThisThreadClass(which);
  const bool lowered = which != ThreadClass::Normal;
  if (lowered && !counted_) budget_.lowered_.fetch_add(1u, std::memory_order_acq_rel);
  if (!lowered && counted_) budget_.lowered_.fetch_sub(1u, std::memory_order_acq_rel);
  counted_ = lowered;
}

ThreadClassKeeper::~ThreadClassKeeper() {
  if (counted_) budget_.lowered_.fetch_sub(1u, std::memory_order_acq_rel);
}

namespace {

constexpr std::string_view kExtension = ".snagbuf";

// One block: the bytes written into it and its place in the stream.
struct Block {
  std::size_t used = 0;
  std::uint64_t serial = 0;
  std::uint8_t data[Stream::kBlockBytes];
};

// A ring of pointers one thread pushes and one other thread pops, neither ever
// waiting: a push to a full ring and a pop from an empty one answer false.
class PointerRing {
 public:
  explicit PointerRing(std::size_t capacity) : slots_(capacity) {}
  bool push(Block* block) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == slots_.size()) return false;
    slots_[tail % slots_.size()] = block;
    tail_.store(tail + 1u, std::memory_order_release);
    return true;
  }
  Block* pop() noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return nullptr;
    Block* const block = slots_[head % slots_.size()];
    head_.store(head + 1u, std::memory_order_release);
    return block;
  }

 private:
  std::vector<Block*> slots_;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

}  // namespace

struct Stream::Impl {
  // A block handed to the reader that it has not taken: in memory, or in its file.
  struct Entry {
    Block* block = nullptr;  // null while the block is in its file
    std::uint64_t serial = 0;
    std::size_t used = 0;
    bool moving = false;  // the stream's thread is writing or reading its file
    std::uint64_t file = 0;    // while the block is in a file
    std::uint64_t offset = 0;  // while the block is in a file
  };

  // A file of the stream's: what has been appended to it, and how many of its
  // blocks have not been read back.
  struct File {
    std::uint64_t number = 0;
    std::uint64_t bytes = 0;  // appended so far
    std::size_t blocks = 0;   // appended so far
    std::size_t unread = 0;   // of those, not read back
  };

  // Free blocks kept for the writer to reuse, at most, while RAM stands at or
  // under the level.
  static constexpr std::size_t kSpareBlocks = 8;

  std::filesystem::path directory;
  std::unique_ptr<StreamBudget> own;  // the stream's budget when it has one of its own
  StreamBudget& budget;
  std::size_t fileBlocks;

  // The writer's own.
  Block* current = nullptr;
  std::uint64_t nextSerial = 0;
  std::uint64_t handedBytes = 0;
  bool writerDone = false;
  // Under `AtLimit::Keep`, filled blocks the handed ring had no room for, in
  // order; each is charged as open until it is pushed.
  std::vector<Block*> waiting;

  // Between the writer and the stream's thread.
  PointerRing handedRing;
  PointerRing spareRing;
  std::atomic<std::uint64_t> allocated{0};      // this stream's blocks in RAM: written, queued, taken, free
  std::atomic<std::uint64_t> handedTotal{0};    // bytes, as the writer counts them
  std::atomic<std::uint64_t> handedBlocks{0};
  std::atomic<std::uint64_t> consumedTotal{0};  // bytes returned by the reader
  std::atomic<std::uint64_t> onDisk{0};         // this stream's bytes in files now
  std::atomic<std::uint64_t> diskWrittenTotal{0};
  std::atomic<bool> fullFlag{false};
  std::atomic<bool> ended{false};

  // Between the stream's thread and the reader.
  std::mutex mutex;
  std::condition_variable readable;  // the reader waits here
  std::condition_variable work;      // the stream's thread waits here
  std::deque<Entry> queue;
  std::deque<Block*> taken;
  std::vector<Block*> freed;  // returned by the reader, for the stream's thread to reuse or free
  bool drained = false;       // the writer ended and every handed block is queued
  bool stopping = false;
  std::string failure;        // a file the stream's thread could not write or read

  // The stream's thread's own, and the destructor's once that thread is joined.
  std::deque<File> files;  // every file on disk, in the order they were begun
  std::ofstream out;       // the newest of them, while it takes blocks
  bool taking = false;     // whether `files.back()` takes another block
  std::uint64_t nextFile = 0;

  std::thread thread;

  static std::size_t ringCapacity(std::uint64_t bytes) {
    const std::uint64_t blocks = bytes / kBlockBytes + 2u;
    return static_cast<std::size_t>(std::max<std::uint64_t>(blocks, 1024u));
  }

  Impl(std::filesystem::path where, std::unique_ptr<StreamBudget> ownBudget, StreamBudget& shared,
       std::size_t ringBlocks, std::size_t blocksAFile)
      : directory(std::move(where)),
        own(std::move(ownBudget)),
        budget(shared),
        fileBlocks(blocksAFile),
        handedRing(ringBlocks != 0u ? ringBlocks : ringCapacity(shared.memoryBytes() + shared.diskBytes())),
        spareRing(kSpareBlocks) {}

  [[nodiscard]] bool keeps() const noexcept { return budget.atLimit_ == AtLimit::Keep; }

  std::filesystem::path fileOf(std::uint64_t number) const {
    return directory / (std::to_string(number) + std::string(kExtension));
  }

  // The RAM every stream on the budget holds.
  [[nodiscard]] std::uint64_t ram() const noexcept { return budget.memoryUsed(); }

  Block* allocate() noexcept {
    Block* const block = new (std::nothrow) Block;
    if (block != nullptr) {
      allocated.fetch_add(1u, std::memory_order_relaxed);
      budget.blocks_.fetch_add(1u, std::memory_order_relaxed);
    }
    return block;
  }

  void release(Block* block) noexcept {
    if (block == nullptr) return;
    delete block;
    allocated.fetch_sub(1u, std::memory_order_relaxed);
    budget.blocks_.fetch_sub(1u, std::memory_order_relaxed);
  }

  // A free block goes to the writer while RAM stands at or under the level,
  // and is freed otherwise.
  void spare(Block* block) noexcept {
    if (ram() <= budget.memoryLevel() && spareRing.push(block)) return;
    release(block);
  }

  // The writer's side: a filled block onto the handed ring, counted as handed.
  bool push(Block* block) noexcept {
    if (!handedRing.push(block)) return false;
    handedBytes += block->used;
    budget.handed_.fetch_add(block->used, std::memory_order_release);
    budget.open_.fetch_sub(1u, std::memory_order_release);
    handedTotal.store(handedBytes, std::memory_order_release);
    handedBlocks.fetch_add(1u, std::memory_order_release);
    return true;
  }

  // The blocks waiting with the writer onto the ring, oldest first, as it has
  // room; answers whether none is left waiting.
  bool pushWaiting() noexcept {
    std::size_t pushed = 0;
    while (pushed < waiting.size() && push(waiting[pushed])) ++pushed;
    waiting.erase(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(pushed));
    return waiting.empty();
  }

  void hand() noexcept {
    if (current == nullptr || current->used == 0) return;
    if (pushWaiting() && push(current)) {
      current = nullptr;
      return;
    }
    // The stream's thread has fallen as many blocks behind as the ring holds.
    if (keeps()) {
      try {
        waiting.push_back(current);
        current = nullptr;
        return;
      } catch (...) {
        // No room to keep it waiting: as a block the system will not allocate.
      }
    }
    fullFlag.store(true, std::memory_order_relaxed);
  }

  // Every waiting block onto the ring, yielding while the ring is full.
  void handWaiting() noexcept {
    while (!pushWaiting()) std::this_thread::yield();
  }

  std::uint8_t* put(std::size_t bytes) noexcept {
    if (fullFlag.load(std::memory_order_relaxed) || writerDone) return nullptr;
    if (bytes > kBlockBytes) {
      fullFlag.store(true, std::memory_order_relaxed);
      return nullptr;
    }
    if (current != nullptr && current->used + bytes > kBlockBytes) {
      hand();
      if (fullFlag.load(std::memory_order_relaxed)) return nullptr;
    }
    if (current == nullptr) {
      // A block opened is charged to the budget whole, so what every stream on
      // it holds never passes the two ceilings together; a stream that keeps
      // opens it all the same.
      if (!keeps() && budget.atLimit()) {
        fullFlag.store(true, std::memory_order_relaxed);
        return nullptr;
      }
      current = spareRing.pop();
      if (current == nullptr) current = allocate();
      if (current == nullptr) {
        fullFlag.store(true, std::memory_order_relaxed);
        return nullptr;
      }
      current->used = 0;
      current->serial = nextSerial++;
      budget.open_.fetch_add(1u, std::memory_order_release);
    }
    std::uint8_t* const at = current->data + current->used;
    current->used += bytes;
    return at;
  }

  // The stream's thread.
  void body() {
    ThreadClassKeeper keeper{budget, ThreadClass::LoweredForFiles};
    std::unique_lock lock(mutex);
    for (;;) {
      lock.unlock();
      keeper.keep();
      // `ended` is read before the ring, so a writer that ended has handed its
      // last block by the time the ring is found empty.
      const bool writerEnded = ended.load(std::memory_order_acquire);
      bool moved = false;
      while (Block* block = handedRing.pop()) {
        lock.lock();
        queue.push_back(Entry{
            .block = block, .serial = block->serial, .used = block->used, .moving = false, .file = 0, .offset = 0});
        lock.unlock();
        moved = true;
      }
      lock.lock();
      for (Block* block : freed) spare(block);
      freed.clear();
      if (writerEnded && !drained) {
        drained = true;
        moved = true;
      }
      if (moved) readable.notify_all();
      if (failure.empty() && (writeOne(lock) || readOne(lock))) continue;
      if (stopping) return;
      if (!moved) work.wait_for(lock, std::chrono::microseconds(200));
    }
  }

  // Moves the newest block in RAM to its file while the budget says a write is
  // due and the disk has room for it; the free blocks go first.
  bool writeOne(std::unique_lock<std::mutex>& lock) {
    if (!budget.writeDue()) return false;
    // The oldest stays for the reader, which takes it next.
    for (std::size_t i = queue.size(); i-- > 1u;) {
      Entry& entry = queue[i];
      if (entry.block == nullptr || entry.moving) continue;
      if (budget.diskUsed() + entry.used > budget.diskBytes()) return false;
      entry.moving = true;
      Block* const block = entry.block;
      const std::uint64_t serial = entry.serial;
      const std::uint64_t target = taking ? files.back().number : nextFile;
      lock.unlock();
      std::uint64_t file = 0;
      std::uint64_t offset = 0;
      const bool written = writeFile(*block, file, offset);
      lock.lock();
      Entry* const now = find(serial);
      now->moving = false;
      if (!written) {
        failure = "the buffer could not write " + fileOf(target).string();
        readable.notify_all();
        return false;
      }
      now->block = nullptr;
      now->file = file;
      now->offset = offset;
      onDisk.fetch_add(block->used, std::memory_order_relaxed);
      budget.onDisk_.fetch_add(block->used, std::memory_order_relaxed);
      diskWrittenTotal.fetch_add(block->used, std::memory_order_relaxed);
      release(block);
      return true;
    }
    return false;
  }

  // Reads the oldest block in a file back while RAM stays at or under the level
  // with it, and the one the reader takes next whatever RAM holds.
  bool readOne(std::unique_lock<std::mutex>& lock) {
    for (std::size_t i = 0; i < queue.size(); ++i) {
      Entry& entry = queue[i];
      if (entry.moving) return false;
      if (entry.block != nullptr) continue;
      if (i != 0u && ram() + sizeof(Block) > budget.memoryLevel()) return false;
      entry.moving = true;
      const std::uint64_t serial = entry.serial;
      const std::size_t used = entry.used;
      const std::uint64_t file = entry.file;
      const std::uint64_t offset = entry.offset;
      lock.unlock();
      Block* const block = allocate();
      std::uint64_t removed = 0;
      const bool read = block != nullptr && readFile(file, offset, used, *block, removed);
      lock.lock();
      Entry* const now = find(serial);
      now->moving = false;
      if (!read) {
        release(block);
        failure = "the buffer could not read " + fileOf(file).string();
        readable.notify_all();
        return false;
      }
      block->used = used;
      block->serial = serial;
      now->block = block;
      onDisk.fetch_sub(removed, std::memory_order_relaxed);
      budget.onDisk_.fetch_sub(removed, std::memory_order_relaxed);
      readable.notify_all();
      return true;
    }
    return false;
  }

  Entry* find(std::uint64_t serial) {
    for (Entry& entry : queue) {
      if (entry.serial == serial) return &entry;
    }
    return nullptr;
  }

  // Appends a block to the newest file, beginning one when none takes blocks,
  // and says where the block lies. A file that holds `fileBlocks` is closed.
  bool writeFile(const Block& block, std::uint64_t& file, std::uint64_t& offset) {
    if (!taking) {
      // A file stream of its own for each file: libc++'s `close()` leaves a
      // stream unbuffered, and one opened again writes a byte at a time.
      out = std::ofstream(fileOf(nextFile), std::ios::binary | std::ios::trunc);
      if (!out) return false;
      files.push_back(File{.number = nextFile, .bytes = 0, .blocks = 0, .unread = 0});
      ++nextFile;
      taking = true;
    }
    File& last = files.back();
    out.write(reinterpret_cast<const char*>(block.data), static_cast<std::streamsize>(block.used));
    out.flush();
    if (out.fail()) return false;
    file = last.number;
    offset = last.bytes;
    last.bytes += block.used;
    ++last.blocks;
    ++last.unread;
    if (last.blocks == fileBlocks) {
      out.close();
      taking = false;
      if (out.fail()) return false;
    }
    return true;
  }

  // Reads a block back from where it lies, through a handle of this read's own
  // and a seek to the block's offset. A file whose last block this was is
  // removed, the newest one closed first, and `removed` is the bytes it held.
  bool readFile(std::uint64_t number, std::uint64_t offset, std::size_t used, Block& block, std::uint64_t& removed) {
    removed = 0;
    const std::filesystem::path path = fileOf(number);
    bool whole = false;
    {
      std::ifstream in(path, std::ios::binary);
      if (!in) return false;
      in.seekg(static_cast<std::streamoff>(offset));
      in.read(reinterpret_cast<char*>(block.data), static_cast<std::streamsize>(used));
      whole = static_cast<std::size_t>(in.gcount()) == used;
    }
    if (!whole) return false;
    const auto at = std::find_if(files.begin(), files.end(), [&](const File& f) { return f.number == number; });
    if (at == files.end()) return false;
    --at->unread;
    if (at->unread != 0u) return true;
    if (taking && &*at == &files.back()) {
      out.close();
      taking = false;
    }
    removed = at->bytes;
    files.erase(at);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return true;
  }

  // The reader's side: the front block, once it is in memory.
  std::optional<std::span<const std::uint8_t>> front() {
    if (!failure.empty()) throw std::runtime_error(failure);
    if (queue.empty() || queue.front().block == nullptr || queue.front().moving) return std::nullopt;
    Block* const block = queue.front().block;
    queue.pop_front();
    taken.push_back(block);
    work.notify_one();
    return std::span<const std::uint8_t>(block->data, block->used);
  }
};

Stream::Stream(std::filesystem::path directory, std::uint64_t memoryBytes, std::uint64_t diskBytes, AtLimit atLimit)
    : Stream(std::move(directory), std::make_unique<StreamBudget>(memoryBytes, diskBytes, atLimit), 0u, kFileBlocks) {
  start();
}

Stream::Stream(std::filesystem::path directory, StreamBudget& budget)
    : impl_(std::make_unique<Impl>(std::move(directory), nullptr, budget, 0u, kFileBlocks)) {
  start();
}

Stream::Stream(std::filesystem::path directory, std::unique_ptr<StreamBudget> own, std::size_t ringBlocks,
               std::size_t fileBlocks) {
  StreamBudget& shared = *own;
  impl_ = std::make_unique<Impl>(std::move(directory), std::move(own), shared, ringBlocks, fileBlocks);
}

Stream::Stream(std::filesystem::path directory, StreamBudget& budget, std::size_t ringBlocks, std::size_t fileBlocks)
    : impl_(std::make_unique<Impl>(std::move(directory), nullptr, budget, ringBlocks, fileBlocks)) {}

void Stream::start() {
  std::error_code error;
  std::filesystem::create_directories(impl_->directory, error);
  if (error || !std::filesystem::is_directory(impl_->directory)) {
    throw std::runtime_error("the buffer's directory " + impl_->directory.string() + " cannot be created");
  }
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(impl_->directory, error)) {
    if (entry.path().extension() == kExtension) std::filesystem::remove(entry.path(), error);
  }
  impl_->thread = std::thread([this] { impl_->body(); });
}

Stream::~Stream() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->work.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  std::error_code ignored;
  if (impl_->out.is_open()) impl_->out.close();
  for (const Impl::File& file : impl_->files) std::filesystem::remove(impl_->fileOf(file.number), ignored);
  for (const Impl::Entry& entry : impl_->queue) impl_->release(entry.block);
  for (Block* block : impl_->taken) impl_->release(block);
  for (Block* block : impl_->freed) impl_->release(block);
  while (Block* block = impl_->handedRing.pop()) impl_->release(block);
  while (Block* block = impl_->spareRing.pop()) impl_->release(block);
  for (Block* block : impl_->waiting) {
    impl_->budget.open_.fetch_sub(1u, std::memory_order_release);
    impl_->release(block);
  }
  if (impl_->current != nullptr) impl_->budget.open_.fetch_sub(1u, std::memory_order_release);
  impl_->release(impl_->current);
  if (std::filesystem::is_empty(impl_->directory, ignored)) std::filesystem::remove(impl_->directory, ignored);
}

std::uint8_t* Stream::put(std::size_t bytes) noexcept { return impl_->put(bytes); }

void Stream::flush() noexcept {
  impl_->hand();
  impl_->handWaiting();
}

void Stream::end() noexcept {
  if (impl_->writerDone) return;
  impl_->hand();
  impl_->handWaiting();
  if (impl_->current != nullptr) {
    // An empty block, or one the stream could not hand over: its charge ends.
    impl_->budget.open_.fetch_sub(1u, std::memory_order_release);
    impl_->release(impl_->current);
    impl_->current = nullptr;
  }
  impl_->writerDone = true;
  impl_->ended.store(true, std::memory_order_release);
}

std::optional<std::span<const std::uint8_t>> Stream::take() {
  std::unique_lock lock(impl_->mutex);
  for (;;) {
    if (std::optional<std::span<const std::uint8_t>> block = impl_->front()) return block;
    if (impl_->drained && impl_->queue.empty()) return std::nullopt;
    impl_->readable.wait(lock);
  }
}

std::optional<std::span<const std::uint8_t>> Stream::poll(bool& ended) {
  std::lock_guard lock(impl_->mutex);
  ended = false;
  if (std::optional<std::span<const std::uint8_t>> block = impl_->front()) return block;
  ended = impl_->drained && impl_->queue.empty();
  return std::nullopt;
}

void Stream::returned() noexcept {
  std::lock_guard lock(impl_->mutex);
  if (impl_->taken.empty()) return;
  Block* const block = impl_->taken.front();
  impl_->taken.pop_front();
  impl_->consumedTotal.fetch_add(block->used, std::memory_order_release);
  impl_->budget.consumed_.fetch_add(block->used, std::memory_order_release);
  impl_->freed.push_back(block);
  impl_->work.notify_one();
}

bool Stream::full() const noexcept { return impl_->fullFlag.load(std::memory_order_relaxed); }

std::uint64_t Stream::held() const noexcept {
  return impl_->handedTotal.load(std::memory_order_acquire) - impl_->consumedTotal.load(std::memory_order_acquire);
}

std::uint64_t Stream::memoryUsed() const noexcept {
  return impl_->allocated.load(std::memory_order_relaxed) * sizeof(Block);
}

std::uint64_t StreamBudget::memoryUsed() const noexcept {
  return blocks_.load(std::memory_order_relaxed) * sizeof(Block);
}

void StreamBudget::setMemoryLevel(unsigned percent) noexcept {
  levelPercent_.store(percent > 100u ? 100u : percent, std::memory_order_relaxed);
}

std::uint64_t StreamBudget::memoryLevel() const noexcept {
  const std::uint64_t ceiling = memoryBytes();
  const std::uint64_t percent = levelPercent_.load(std::memory_order_relaxed);
  // ceiling * percent / 100, exact and rounded down without the product's overflow.
  return ceiling / 100u * percent + ceiling % 100u * percent / 100u;
}

bool StreamBudget::memoryAtLevel() const noexcept { return memoryUsed() <= memoryLevel(); }

bool StreamBudget::writingDown() const noexcept { return writingDown_.load(std::memory_order_acquire); }

// Whether a stream's thread moves a block to a file now: always while RAM is
// past its ceiling, never while it stands at or under the level, and between
// the two only from RAM passing the ceiling until it comes down to the level.
bool StreamBudget::writeDue() noexcept {
  const std::uint64_t ram = memoryUsed();
  if (ram > memoryBytes()) {
    writingDown_.store(true, std::memory_order_release);
    return true;
  }
  if (ram <= memoryLevel()) {
    writingDown_.store(false, std::memory_order_release);
    return false;
  }
  return writingDown_.load(std::memory_order_acquire);
}

std::uint64_t Stream::diskUsed() const noexcept { return impl_->onDisk.load(std::memory_order_relaxed); }

std::uint64_t Stream::diskWritten() const noexcept {
  return impl_->diskWrittenTotal.load(std::memory_order_relaxed);
}

std::uint64_t Stream::handed() const noexcept { return impl_->handedBlocks.load(std::memory_order_acquire); }

}  // namespace snaggletooth::disasm
