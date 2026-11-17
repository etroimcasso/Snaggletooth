// The requests to close a process gets from outside its window: counted and
// taken, the system's own close said, one counter at a time, and on macOS and
// Linux the four signals counted with the process still running and the
// handlers they replaced put back.

#include <stdexcept>

#if !defined(_WIN32)
#include <csignal>
#endif

#include "gtest/gtest.h"
#include "player/close_requests.h"

namespace snaggletooth::player {

TEST(PlayerCloseRequests, ARequestIsCountedAndTaken) {
  CloseRequests requests;
  EXPECT_EQ(requests.take(), 0u);
  CloseRequests::deliver(false);
  CloseRequests::deliver(false);
  EXPECT_EQ(requests.take(), 2u);
  EXPECT_EQ(requests.take(), 0u) << "taken once";
  EXPECT_FALSE(requests.systemIsClosing());
}

TEST(PlayerCloseRequests, TheSystemsOwnCloseIsSaid) {
  CloseRequests requests;
  CloseRequests::deliver(true);
  EXPECT_TRUE(requests.systemIsClosing());
  EXPECT_EQ(requests.take(), 1u) << "and counted as a request";
  requests.done();
}

TEST(PlayerCloseRequests, OneCounterStandsAtATime) {
  {
    CloseRequests first;
    EXPECT_THROW(CloseRequests{}, std::logic_error);
  }
  CloseRequests after;
  EXPECT_FALSE(after.systemIsClosing()) << "a new counter starts clear";
}

TEST(PlayerCloseRequests, TheFourSignalsAreCountedAndTheProcessRuns) {
#if defined(_WIN32)
  GTEST_SKIP() << "the four signals are macOS's and Linux's; Windows has its console events, which deliver() "
                  "stands in for";
#else
  CloseRequests requests;
  for (const int which : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
    ASSERT_EQ(std::raise(which), 0);
    EXPECT_EQ(requests.take(), 1u) << "signal " << which;
  }
#endif
}

TEST(PlayerCloseRequests, TheReplacedHandlersArePutBack) {
#if defined(_WIN32)
  GTEST_SKIP() << "the four signals are macOS's and Linux's; Windows has its console events, which deliver() "
                  "stands in for";
#else
  for (const int which : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
    struct sigaction before{};
    ASSERT_EQ(sigaction(which, nullptr, &before), 0);
    {
      CloseRequests requests;
      struct sigaction standing{};
      ASSERT_EQ(sigaction(which, nullptr, &standing), 0);
      EXPECT_NE(standing.sa_handler, before.sa_handler) << "signal " << which << ": replaced while it stands";
    }
    struct sigaction after{};
    ASSERT_EQ(sigaction(which, nullptr, &after), 0);
    EXPECT_EQ(after.sa_handler, before.sa_handler) << "signal " << which << ": put back";
  }
#endif
}

}  // namespace snaggletooth::player
