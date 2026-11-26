/*
 * Deterministic unit test for mac_virtual_display_policy.h.
 *
 * Pure C++ — no CoreGraphics, no Objective-C, no I/O. Exercises:
 *   1. Dimension clamping to the supported envelope.
 *   2. Even-dimension alignment.
 *   3. HiDPI logical-vs-backing constraints.
 *   4. Refresh rate snapping.
 *   5. Lifecycle state machine transitions.
 *
 * Compile & run standalone:
 *   clang++ -std=c++17 -Wall -Wextra -Werror \
 *     -I apps/desktop/src/platform/macos/virtual_display \
 *     -o /tmp/mac_vd_policy_test \
 *     apps/desktop/tests/mac_virtual_display_policy_test.cpp && \
 *     /tmp/mac_vd_policy_test
 *
 * Exit 0 on success, non-zero on first failure count (printed).
 */

#include <cstdio>
#include <cstdlib>

#include "mac_virtual_display_policy.h"

using crossdesk::mac_virtual_display_policy::AlignEvenDown;
using crossdesk::mac_virtual_display_policy::ClampResult;
using crossdesk::mac_virtual_display_policy::ClampSpec;
using crossdesk::mac_virtual_display_policy::kDefaultRefreshRate;
using crossdesk::mac_virtual_display_policy::kMaxHeight;
using crossdesk::mac_virtual_display_policy::kMaxWidth;
using crossdesk::mac_virtual_display_policy::kMinHeight;
using crossdesk::mac_virtual_display_policy::kMinWidth;
using crossdesk::mac_virtual_display_policy::LifecycleAction;
using crossdesk::mac_virtual_display_policy::LifecycleEvent;
using crossdesk::mac_virtual_display_policy::LifecycleInput;
using crossdesk::mac_virtual_display_policy::LifecycleOutput;
using crossdesk::mac_virtual_display_policy::LifecycleState;
using crossdesk::mac_virtual_display_policy::SpecForControllerViewport;
using crossdesk::mac_virtual_display_policy::Step;

namespace {

int failures = 0;

void ExpectEq(const char* what, long long got, long long want, int line) {
  if (got != want) {
    std::fprintf(stderr, "FAIL line %d: %s — got %lld, want %lld\n", line,
                 what, got, want);
    ++failures;
  }
}
void ExpectTrue(const char* what, bool cond, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL line %d: %s — expected true\n", line, what);
    ++failures;
  }
}
void ExpectFalse(const char* what, bool cond, int line) {
  if (cond) {
    std::fprintf(stderr, "FAIL line %d: %s — expected false\n", line, what);
    ++failures;
  }
}
void ExpectNear(const char* what, double got, double want, double tol,
                int line) {
  const double diff = got > want ? got - want : want - got;
  if (diff > tol) {
    std::fprintf(stderr, "FAIL line %d: %s — got %.4f, want %.4f\n", line,
                 what, got, want);
    ++failures;
  }
}

#define EXPECT_EQ(g, w) ExpectEq(#g, static_cast<long long>(g), \
                                 static_cast<long long>(w), __LINE__)
#define EXPECT_TRUE(c) ExpectTrue(#c, c, __LINE__)
#define EXPECT_FALSE(c) ExpectFalse(#c, c, __LINE__)
#define EXPECT_NEAR(g, w, t) ExpectNear(#g, g, w, t, __LINE__)

// ── AlignEvenDown ────────────────────────────────────────────────────────
void TestAlignEvenDown() {
  EXPECT_EQ(AlignEvenDown(1920), 1920);
  EXPECT_EQ(AlignEvenDown(1921), 1920);
  EXPECT_EQ(AlignEvenDown(1), 0);
  EXPECT_EQ(AlignEvenDown(0), 0);
  EXPECT_EQ(AlignEvenDown(2559), 2558);
}

// ── ClampSpec — basic envelope ──────────────────────────────────────────
void TestClampSpecEnvelop() {
  // Pass-through: 1920x1080@60 non-HiDPI is the canonical case.
  {
    const auto r = ClampSpec(1920, 1080, 60.0, false);
    EXPECT_EQ(r.width, 1920);
    EXPECT_EQ(r.height, 1080);
    EXPECT_NEAR(r.refresh_rate, 60.0, 1e-6);
    EXPECT_FALSE(r.adjusted);
  }
  // Under minimum: clamped up.
  {
    const auto r = ClampSpec(320, 200, 60.0, false);
    EXPECT_EQ(r.width, kMinWidth);
    EXPECT_EQ(r.height, kMinHeight);
    EXPECT_TRUE(r.adjusted);
  }
  // Over maximum non-HiDPI: clamped down.
  {
    const auto r = ClampSpec(9999, 9999, 60.0, false);
    EXPECT_EQ(r.width, kMaxWidth);
    EXPECT_EQ(r.height, kMaxHeight);
    EXPECT_TRUE(r.adjusted);
  }
  // Zero width/height: fallback to min.
  {
    const auto r = ClampSpec(0, 0, 60.0, false);
    EXPECT_EQ(r.width, kMinWidth);
    EXPECT_EQ(r.height, kMinHeight);
    EXPECT_TRUE(r.adjusted);
  }
}

// ── ClampSpec — even alignment ──────────────────────────────────────────
void TestClampSpecEvenAlignment() {
  // Odd input is rounded down to even.
  const auto r = ClampSpec(1921, 1081, 60.0, false);
  EXPECT_EQ(r.width, 1920);
  EXPECT_EQ(r.height, 1080);
  EXPECT_TRUE(r.adjusted);
}

// ── ClampSpec — HiDPI halves the maximum logical size ───────────────────
void TestClampSpecHiDpi() {
  // Under HiDPI, a 5120x2880 logical request must clamp to half-envelope
  // because the backing store would be 10240x5760 otherwise.
  const auto r = ClampSpec(5120, 2880, 60.0, true);
  EXPECT_EQ(r.width, kMaxWidth / 2);
  EXPECT_EQ(r.height, kMaxHeight / 2);
  EXPECT_TRUE(r.hi_dpi);
  EXPECT_TRUE(r.adjusted);

  // Reasonable HiDPI request passes through unmodified.
  const auto ok = ClampSpec(1920, 1080, 60.0, true);
  EXPECT_EQ(ok.width, 1920);
  EXPECT_EQ(ok.height, 1080);
  EXPECT_TRUE(ok.hi_dpi);
  EXPECT_FALSE(ok.adjusted);
}

// ── ClampSpec — refresh rate snapping ───────────────────────────────────
void TestClampSpecRefreshSnap() {
  // 60 stays 60.
  EXPECT_NEAR(ClampSpec(1920, 1080, 60.0, false).refresh_rate, 60.0, 1e-6);
  // 59.94 snaps to 60.
  EXPECT_NEAR(ClampSpec(1920, 1080, 59.94, false).refresh_rate, 60.0, 1e-6);
  // 30 stays 30.
  EXPECT_NEAR(ClampSpec(1920, 1080, 30.0, false).refresh_rate, 30.0, 1e-6);
  // 144 snaps to nearest in {30, 60, 120} = 120.
  EXPECT_NEAR(ClampSpec(1920, 1080, 144.0, false).refresh_rate, 120.0, 1e-6);
  // 90 is equidistant between 60 and 120 — either is acceptable, but must
  // be one of them (not 90).
  const double v = ClampSpec(1920, 1080, 90.0, false).refresh_rate;
  EXPECT_TRUE(v == 60.0 || v == 120.0);
}

// ── SpecForControllerViewport ───────────────────────────────────────────
void TestSpecForControllerViewport() {
  // Controller at 2560x1440, no HiDPI preference.
  const auto r = SpecForControllerViewport(2560, 1440, 60.0, false);
  EXPECT_EQ(r.width, 2560);
  EXPECT_EQ(r.height, 1440);
  EXPECT_FALSE(r.hi_dpi);
  EXPECT_FALSE(r.adjusted);

  // Controller at 3000x2000 with HiDPI preference — must fit in half
  // envelope (2560x1440 logical) because backing would be 6000x4000.
  const auto hi = SpecForControllerViewport(3000, 2000, 60.0, true);
  EXPECT_TRUE(hi.hi_dpi);
  EXPECT_EQ(hi.width, kMaxWidth / 2);
  EXPECT_EQ(hi.height, kMaxHeight / 2);
}

// ── Lifecycle state machine ─────────────────────────────────────────────
LifecycleInput MakeInput(LifecycleState state, LifecycleEvent event,
                         uint32_t active_sessions, uint32_t physical_displays,
                         bool explicit_request) {
  LifecycleInput in;
  in.state = state;
  in.event = event;
  in.active_sessions = active_sessions;
  in.physical_displays = physical_displays;
  in.explicit_request = explicit_request;
  return in;
}

void TestLifecycleHeadlessAutoCreate() {
  // Headless (0 physical displays) + capture starts -> create.
  const auto out = Step(MakeInput(LifecycleState::kIdle,
                                  LifecycleEvent::kSessionCaptureStarted,
                                  1, 0, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kActive));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kCreate));
}

void TestLifecycleNoCreateWhenPhysicalPresent() {
  // 1 physical display + capture starts + no explicit request -> no create.
  const auto out = Step(MakeInput(LifecycleState::kIdle,
                                  LifecycleEvent::kSessionCaptureStarted,
                                  1, 1, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kIdle));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kNone));
}

void TestLifecycleExplicitRequestOverridesPhysical() {
  // 1 physical display + capture starts + explicit request -> create.
  const auto out = Step(MakeInput(LifecycleState::kIdle,
                                  LifecycleEvent::kSessionCaptureStarted,
                                  1, 1, true));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kActive));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kCreate));
}

void TestLifecycleDestroyOnLastSessionStop() {
  // Active + last session stops -> destroy.
  const auto out = Step(MakeInput(LifecycleState::kActive,
                                  LifecycleEvent::kSessionCaptureStopped,
                                  0, 0, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kIdle));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kDestroy));
}

void TestLifecycleNoDestroyWhileSessionsRemain() {
  // Active + one session stops but others remain -> no destroy.
  const auto out = Step(MakeInput(LifecycleState::kActive,
                                  LifecycleEvent::kSessionCaptureStopped,
                                  1, 0, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kActive));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kNone));
}

void TestLifecycleProcessExitingAlwaysDestroys() {
  // Active + process exit -> destroy regardless of session count.
  const auto out = Step(MakeInput(LifecycleState::kActive,
                                  LifecycleEvent::kProcessExiting,
                                  5, 1, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kIdle));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kDestroy));
}

void TestLifecycleProcessExitingNoopWhenIdle() {
  // Idle + process exit -> nothing to do.
  const auto out = Step(MakeInput(LifecycleState::kIdle,
                                  LifecycleEvent::kProcessExiting,
                                  0, 0, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kIdle));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kNone));
}

void TestLifecycleNoDoubleCreate() {
  // Already active + another capture starts -> no re-create.
  const auto out = Step(MakeInput(LifecycleState::kActive,
                                  LifecycleEvent::kSessionCaptureStarted,
                                  2, 0, false));
  EXPECT_EQ(static_cast<int>(out.next_state),
            static_cast<int>(LifecycleState::kActive));
  EXPECT_EQ(static_cast<int>(out.action),
            static_cast<int>(LifecycleAction::kNone));
}

}  // namespace

int main() {
  TestAlignEvenDown();
  TestClampSpecEnvelop();
  TestClampSpecEvenAlignment();
  TestClampSpecHiDpi();
  TestClampSpecRefreshSnap();
  TestSpecForControllerViewport();
  TestLifecycleHeadlessAutoCreate();
  TestLifecycleNoCreateWhenPhysicalPresent();
  TestLifecycleExplicitRequestOverridesPhysical();
  TestLifecycleDestroyOnLastSessionStop();
  TestLifecycleNoDestroyWhileSessionsRemain();
  TestLifecycleProcessExitingAlwaysDestroys();
  TestLifecycleProcessExitingNoopWhenIdle();
  TestLifecycleNoDoubleCreate();

  if (failures == 0) {
    std::printf("mac_virtual_display_policy_test: PASS\n");
    return 0;
  }
  std::fprintf(stderr, "mac_virtual_display_policy_test: %d failure(s)\n",
               failures);
  return failures > 0 ? 1 : 0;
}
