#include "player/close_requests.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace snaggletooth::player {

namespace {

std::atomic<unsigned> requests{0};
std::atomic<bool> systemClosing{false};
std::atomic<bool> hostDone{false};
std::atomic<bool> standing{false};
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

#if defined(_WIN32)

BOOL WINAPI onConsoleEvent(DWORD event) {
  switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      CloseRequests::deliver(false);
      return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      CloseRequests::deliver(true);
      while (!hostDone.load(std::memory_order_acquire)) Sleep(50);
      return TRUE;
    default:
      return FALSE;
  }
}

#else

constexpr std::array<int, 4> kSignals = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
std::array<struct sigaction, kSignals.size()> replaced{};

void onSignal(int) { CloseRequests::deliver(false); }

#endif

}  // namespace

CloseRequests::CloseRequests() {
  if (standing.exchange(true, std::memory_order_acq_rel)) {
    throw std::logic_error("the requests to close are already being counted");
  }
  requests.store(0, std::memory_order_release);
  systemClosing.store(false, std::memory_order_release);
  hostDone.store(false, std::memory_order_release);
#if defined(_WIN32)
  SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
  struct sigaction action{};
  action.sa_handler = onSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  for (std::size_t i = 0; i < kSignals.size(); ++i) sigaction(kSignals[i], &action, &replaced[i]);
#endif
}

CloseRequests::~CloseRequests() {
#if defined(_WIN32)
  SetConsoleCtrlHandler(onConsoleEvent, FALSE);
#else
  for (std::size_t i = 0; i < kSignals.size(); ++i) sigaction(kSignals[i], &replaced[i], nullptr);
#endif
  standing.store(false, std::memory_order_release);
}

unsigned CloseRequests::take() noexcept { return requests.exchange(0, std::memory_order_acq_rel); }

bool CloseRequests::systemIsClosing() const noexcept { return systemClosing.load(std::memory_order_acquire); }

void CloseRequests::done() noexcept { hostDone.store(true, std::memory_order_release); }

void CloseRequests::deliver(bool fromSystem) noexcept {
  if (fromSystem) systemClosing.store(true, std::memory_order_release);
  requests.fetch_add(1u, std::memory_order_acq_rel);
}

}  // namespace snaggletooth::player
