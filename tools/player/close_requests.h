#pragma once

// The requests to close that reach a process from outside its window, counted
// and refused: an interrupt, a termination, a hangup or a quit signal on macOS
// and Linux; Ctrl-C and Ctrl-Break in the console on Windows. While one of
// these stands, each such request adds one to a count and ends nothing, so the
// host can finish its work and answer the request with what is left. A forced
// kill — `kill -9`, Force Quit, Task Manager's End Task — is not a request and
// is never seen here.
//
//   player::CloseRequests requests;
//   while (working()) {
//     if (requests.take() != 0) say("still working");
//   }
//
// On Windows the console's own close, a logoff and a shutdown also arrive, and
// the system ends the process a few seconds after one whatever the process
// does: `systemIsClosing()` says one has, and the handler holds the system off
// for as long as it allows, until `done()`.

namespace snaggletooth::player {

class CloseRequests {
 public:
  // Installs the handlers. Throws `std::logic_error` while another stands.
  CloseRequests();
  // Puts back the handlers it replaced.
  ~CloseRequests();
  CloseRequests(const CloseRequests&) = delete;
  CloseRequests& operator=(const CloseRequests&) = delete;
  // How many requests arrived since the last call.
  [[nodiscard]] unsigned take() noexcept;
  [[nodiscard]] bool systemIsClosing() const noexcept;
  // The host's work is done: a Windows handler waiting on it returns.
  void done() noexcept;
  // What the platform's handler does on a request, for a caller that has one
  // of its own to pass on; `fromSystem` is a console close, a logoff or a
  // shutdown.
  static void deliver(bool fromSystem) noexcept;
};

}  // namespace snaggletooth::player
