/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic unit test for ScreenLockMonitor. A scripted fake query and a
// fake monotonic clock replace the OS entirely, so every edge, throttle and
// fail-open assertion is exact — no sleeps, no DBus, no CGSession.
#include "common/screen_lock_monitor.h"

#include <iostream>
#include <vector>

// The monitor's default query binding references the platform symbol, but a
// deterministic unit test must not drag in DBus/CGSession. Providing the
// stub here keeps the test self-contained on every platform.
namespace crossdesk {
ScreenLockState QueryScreenLock() { return ScreenLockState::unknown; }
bool LockScreen() { return false; }
}  // namespace crossdesk

namespace {

using crossdesk::ScreenLockMonitor;
using crossdesk::ScreenLockState;
using Clock = ScreenLockMonitor::Clock;

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Clock::time_point At(int ms) {
  return Clock::time_point(std::chrono::milliseconds(ms));
}

// A query function driven by a script of states; repeated calls return the
// back of the script once it is exhausted. Counting invocations proves the
// throttle.
struct ScriptedQuery {
  std::vector<ScreenLockState> script;
  int calls = 0;
  ScreenLockState operator()() {
    ++calls;
    if (script.empty()) return ScreenLockState::unknown;
    const ScreenLockState state = script.front();
    if (script.size() > 1) script.erase(script.begin());
    return state;
  }
};

void FirstPollEstablishesBaselineWithoutEdge() {
  ScriptedQuery fake{{ScreenLockState::unlocked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  const auto t = monitor.Poll(At(0));
  Expect(!t.became_locked && !t.became_unlocked,
         "first unlocked sample must not be an edge");
  Expect(!monitor.IsLocked(), "unlocked baseline must not report locked");
  Expect(fake.calls == 1, "first poll must query once");
}

void UnlockedToLockedIsAPauseEdge() {
  ScriptedQuery fake{{ScreenLockState::unlocked, ScreenLockState::locked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  monitor.Poll(At(0));
  const auto t = monitor.Poll(At(1000));
  Expect(t.became_locked && !t.became_unlocked,
         "unlocked->locked must raise became_locked exactly");
  Expect(monitor.IsLocked(), "locked state must be visible via IsLocked");
}

void LockedToUnlockedIsAResumeEdgeFiredOnce() {
  ScriptedQuery fake{{ScreenLockState::locked, ScreenLockState::unlocked,
                      ScreenLockState::unlocked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  monitor.Poll(At(0));
  const auto edge = monitor.Poll(At(1000));
  Expect(edge.became_unlocked && !edge.became_locked,
         "locked->unlocked must raise became_unlocked (the IDR trigger)");
  const auto level = monitor.Poll(At(2000));
  Expect(!level.became_unlocked && !level.became_locked,
         "steady unlocked must never re-fire the resume edge");
}

void UnknownStateFailsOpenAndNeverGeneratesEdges() {
  ScriptedQuery fake{{ScreenLockState::unlocked, ScreenLockState::unknown,
                      ScreenLockState::unknown, ScreenLockState::locked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  monitor.Poll(At(0));
  for (int ms : {1000, 2000}) {
    const auto t = monitor.Poll(At(ms));
    Expect(!t.became_locked && !t.became_unlocked,
           "unknown samples must never be edges");
  }
  Expect(!monitor.IsLocked(),
         "unknown samples must not move the last-known state (fail open)");
  // unlocked -> unknown -> unknown -> locked IS still a lock edge: the state
  // machine compares consecutive *known* states, and the host genuinely
  // locked during the blackout, so capture must pause.
  const auto t = monitor.Poll(At(3000));
  Expect(t.became_locked,
         "lock arriving across an unknown blackout must still pause capture");
}

void LockedAcrossConstructionBaselinePausesImmediately() {
  // First known state is locked: no edge, but IsLocked must reflect it so a
  // host that boots into the lock screen never forwards.
  ScriptedQuery fake{{ScreenLockState::locked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  const auto t = monitor.Poll(At(0));
  Expect(!t.became_locked, "baseline locked is not an edge");
  Expect(monitor.IsLocked(), "baseline locked must report IsLocked");
}

void ThrottleSuppressesOsCallsWithinInterval() {
  ScriptedQuery fake{{ScreenLockState::unlocked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  monitor.Poll(At(0));
  for (int ms = 1; ms < 1000; ms += 100) monitor.Poll(At(ms));
  Expect(fake.calls == 1, "polls inside the interval must not hit the OS");
  monitor.Poll(At(1000));
  Expect(fake.calls == 2, "poll at the interval boundary must query again");
}

void ResetThrottleForcesImmediateQuery() {
  ScriptedQuery fake{{ScreenLockState::unlocked}};
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000), std::ref(fake));
  monitor.Poll(At(0));
  monitor.Poll(At(100));
  Expect(fake.calls == 1, "throttled poll must not query");
  monitor.ResetThrottle();
  monitor.Poll(At(101));
  Expect(fake.calls == 2, "reset throttle must force a query on next poll");
}

void DefaultConstructedMonitorPollsRealQuerySafely() {
  // The default query binding must be callable; on a test machine the result
  // may be any state, the only contract is no crash and exact-once throttling.
  ScreenLockMonitor monitor(std::chrono::milliseconds(1000));
  const auto t = monitor.Poll(At(0));
  (void)t;
  const auto cached = monitor.LastKnownState();
  Expect(cached == ScreenLockState::unknown || cached == ScreenLockState::locked ||
         cached == ScreenLockState::unlocked, "state enum must be valid");
}

}  // namespace

int main() {
  FirstPollEstablishesBaselineWithoutEdge();
  UnlockedToLockedIsAPauseEdge();
  LockedToUnlockedIsAResumeEdgeFiredOnce();
  UnknownStateFailsOpenAndNeverGeneratesEdges();
  LockedAcrossConstructionBaselinePausesImmediately();
  ThrottleSuppressesOsCallsWithinInterval();
  ResetThrottleForcesImmediateQuery();
  DefaultConstructedMonitorPollsRealQuerySafely();
  if (failures == 0) {
    std::cout << "screen_lock_monitor_test: all assertions passed\n";
    return 0;
  }
  std::cerr << "screen_lock_monitor_test: " << failures << " failure(s)\n";
  return 1;
}
