/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic verification of the exit-release ordering invariant:
// whatever interleaving the sender thread and the UI exit take, the final
// button state on the host must be UP for every button. The harness models
// the exact protocol pieces with explicit preemption points:
//
//   sender:    lock; validate active; tracker.Update(down/up); unlock;
//              --- preemptable ---; sink.send(edge)
//   exit:      lock; active=false; unlock; join sender; lock;
//              pressed=take(); unlock; for b in pressed: sink.send(up)
//
// The join is modelled as it really works: the exit cannot pass it until
// the sender's in-flight iteration completes. Enumerating every placement
// of the exit relative to the sender's preemption window must always end
// with the host state "up". Before the join-ordering fix the opposite was
// provable: exit releasing before the sender resumes yields down-after-up
// on the wire.

#include "features/input/game_mode_buttons.h"

#include <iostream>
#include <map>
#include <vector>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

// The wire: an ordered reliable channel feeding the host's button state.
struct FakeHost {
  // Edges in wire order: (button, down?).
  std::vector<std::pair<int, bool>> edges;
  std::map<int, bool> state;  // final host-side state per button

  void Send(int button, bool down) {
    edges.emplace_back(button, down);
    state[button] = down;
  }
};

// One sender iteration for a single button edge, split at the exact
// preemption point the defect lives in: tracker update done, send not yet
// issued. `issue_send` controls whether the send happens now (the caller
// uses it to place the send on either side of the exit's release).
struct SenderIteration {
  crossdesk::GameModePressedButtons* tracker;
  bool* active;
  int button;
  bool down;
  bool validated = false;

  // lock; validate; update tracker; unlock — then preempted.
  void ValidateAndTrack() {
    if (!*active) return;
    validated = true;
    tracker->Update(button, down);
  }

  // The send proper: only meaningful if validation passed.
  void Send(FakeHost& host) const {
    if (validated) host.Send(button, down);
  }
};

// The exit protocol under test: flip, JOIN, take, release. The join is
// modelled by the harness sequencing — when exit runs after a preempted
// sender iteration, the harness completes that iteration first, because
// the join cannot pass while it is in flight.
void ExitProtocol(bool& active, crossdesk::GameModePressedButtons& tracker,
                  FakeHost& host) {
  active = false;  // flip under the mutex, modelled atomically
  // join: harness guarantees no sender iteration is in flight here
  const auto pressed = tracker.TakePressed();
  for (int button : pressed) host.Send(button, false);
}

}  // namespace

int main() {
  bool ok = true;

  // Enumerate every ordering of: sender down-iteration, sender up-iteration
  // (optional), and the exit, with each sender iteration split at its
  // preemption point. For a button that is down when the chord lands, the
  // meaningful interleavings are:
  //
  //   A. down completes fully before exit            -> released at exit
  //   B. down validated+tracked, preempted, exit     -> join completes the
  //      down send first, then release: wire down, up
  //   C. exit flips before down validates            -> down never sent,
  //      nothing to release
  //   D. down+up both complete before exit           -> nothing pressed
  //   E. down completes, up validated but preempted  -> up send completes
  //      at join before the release pass: tracker already empty
  //
  // The pre-fix ordering (release before join) is modelled in case F to
  // prove the harness can distinguish the defect.

  // Case A: down fully sent, then exit.
  {
    FakeHost host;
    bool active = true;
    crossdesk::GameModePressedButtons tracker;
    SenderIteration down{&tracker, &active, 1, true};
    down.ValidateAndTrack();
    down.Send(host);
    ExitProtocol(active, tracker, host);
    ok &= Expect(host.state[1] == false,
                 "case A: completed down must be released at exit");
  }

  // Case B: down validated+tracked, preempted; exit joins -> down send
  // completes first, then release. Wire order must be down THEN up.
  {
    FakeHost host;
    bool active = true;
    crossdesk::GameModePressedButtons tracker;
    SenderIteration down{&tracker, &active, 1, true};
    down.ValidateAndTrack();
    // exit begins: flip happens, then the join — modelled by completing
    // the in-flight iteration before the release pass.
    active = false;
    down.Send(host);  // join completing the in-flight send
    const auto pressed = tracker.TakePressed();
    for (int button : pressed) host.Send(button, false);
    ok &= Expect(host.state[1] == false,
                 "case B: preempted down must still end released");
    ok &= Expect(host.edges.size() == 2 && host.edges[0].second &&
                     !host.edges[1].second,
                 "case B: wire order must be down then up");
  }

  // Case C: exit flips before the down validates — the down is never sent
  // and the tracker never records it.
  {
    FakeHost host;
    bool active = true;
    crossdesk::GameModePressedButtons tracker;
    SenderIteration down{&tracker, &active, 1, true};
    ExitProtocol(active, tracker, host);
    down.ValidateAndTrack();
    down.Send(host);
    ok &= Expect(!down.validated,
                 "case C: post-flip validation must reject the edge");
    ok &= Expect(host.edges.empty(),
                 "case C: nothing may reach the wire after the flip");
    ok &= Expect(host.state.find(1) == host.state.end() || !host.state[1],
                 "case C: host must not see a pressed button");
  }

  // Case D: full press cycle before exit — nothing to release.
  {
    FakeHost host;
    bool active = true;
    crossdesk::GameModePressedButtons tracker;
    SenderIteration down{&tracker, &active, 1, true};
    down.ValidateAndTrack();
    down.Send(host);
    SenderIteration up{&tracker, &active, 1, false};
    up.ValidateAndTrack();
    up.Send(host);
    ExitProtocol(active, tracker, host);
    ok &= Expect(host.state[1] == false,
                 "case D: clean press cycle ends up without release help");
    ok &= Expect(host.edges.size() == 2,
                 "case D: no extra release may be sent for clean input");
  }

  // Case E: down sent, up validated+tracked but preempted before its send.
  // The tracker no longer holds the button, so the exit releases nothing —
  // and the join completing the up send keeps the host correct.
  {
    FakeHost host;
    bool active = true;
    crossdesk::GameModePressedButtons tracker;
    SenderIteration down{&tracker, &active, 1, true};
    down.ValidateAndTrack();
    down.Send(host);
    SenderIteration up{&tracker, &active, 1, false};
    up.ValidateAndTrack();
    // exit: flip, join completes the up send, take finds nothing pressed.
    active = false;
    up.Send(host);
    const auto pressed = tracker.TakePressed();
    for (int button : pressed) host.Send(button, false);
    ok &= Expect(host.state[1] == false,
                 "case E: preempted up must leave the host up");
    ok &= Expect(pressed.empty(),
                 "case E: nothing may be released after a tracked up");
  }

  // Case F — the pre-fix ordering, included as a negative control: release
  // BEFORE the join completes the preempted down send. The harness must
  // show the defect it guards against, or it proves nothing.
  {
    FakeHost host;
    bool active = true;
    crossdesk::GameModePressedButtons tracker;
    SenderIteration down{&tracker, &active, 1, true};
    down.ValidateAndTrack();
    active = false;
    // Pre-fix: take and release first...
    const auto pressed = tracker.TakePressed();
    for (int button : pressed) host.Send(button, false);
    // ...then the preempted down send completes.
    down.Send(host);
    ok &= Expect(host.state[1] == true,
                 "case F (negative control): release-before-join must leave "
                 "the button stuck, proving the harness detects the defect");
  }

  return ok ? 0 : 1;
}
