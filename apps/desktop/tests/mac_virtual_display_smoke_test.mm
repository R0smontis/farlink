/*
 * Real-hardware smoke test for the macOS virtual display RAII wrapper.
 *
 * This test exercises the create-verify-reconfigure-destroy lifecycle
 * against the live WindowServer. It is intended to be run manually on a
 * real macOS host, NOT in CI.
 *
 * IMPORTANT — macOS 27.x teardown defect:
 *   On macOS 27.x, destroying a CGVirtualDisplay object does NOT remove
 *   the display from the system. Even process exit leaves the display
 *   attached. The test detects this condition via TeardownReliable() and
 *   adjusts its assertions accordingly:
 *
 *     - If TeardownReliable() is true (macOS 11-15 expected): the test
 *       verifies full destroy semantics — display count returns to
 *       baseline after Destroy().
 *     - If TeardownReliable() is false (macOS 27.x reality): the test
 *       verifies creation and reconfiguration but expects teardown to
 *       leave the display attached. It reports this honestly so the
 *       operator understands the system state after the test.
 *
 *   After running on macOS 27.x, a logout/login or reboot is required
 *   to clear the displays created by the test.
 *
 * Build & run (from the worktree root):
 *   clang++ -std=c++17 -fobjc-arc -Wall -Wextra \
 *     -framework Foundation -framework CoreGraphics \
 *     -I apps/desktop/src/platform/macos/virtual_display \
 *     -I apps/desktop/src \
 *     -DCROSSDESK_MAC_VIRTUAL_DISPLAY_NO_RD_LOG \
 *     -o /tmp/mac_vd_smoke_test \
 *     apps/desktop/tests/mac_virtual_display_smoke_test.mm \
 *     apps/desktop/src/platform/macos/virtual_display/mac_virtual_display.mm
 *
 * Exit 0 on success; non-zero on failure. Prints a step-by-step log.
 */

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstring>
#include <set>

#include "mac_virtual_display.h"

namespace {

int failures = 0;

void Fail(const char* what, int line) {
  std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
  ++failures;
}
#define CHECK(cond, msg) do { if (!(cond)) Fail(msg, __LINE__); } while (0)

std::set<CGDirectDisplayID> SnapshotActiveDisplays() {
  CGDirectDisplayID ids[32] = {0};
  uint32_t n = 0;
  CGGetActiveDisplayList(32, ids, &n);
  std::set<CGDirectDisplayID> out;
  for (uint32_t i = 0; i < n; ++i) out.insert(ids[i]);
  return out;
}

void PrintSet(const char* tag, const std::set<CGDirectDisplayID>& s) {
  std::printf("%s (%zu):", tag, s.size());
  for (auto id : s) {
    std::printf(" 0x%x(%zux%zu)", id, CGDisplayPixelsWide(id),
                CGDisplayPixelsHigh(id));
  }
  std::printf("\n");
}

void DrainRunloop(int iterations) {
  for (int i = 0; i < iterations; ++i) {
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
  }
}

}  // namespace

int main() {
  @autoreleasepool {
    std::printf("=== mac_virtual_display smoke test ===\n");

    // ── 0. Runtime support gate ─────────────────────────────────────────
    if (!crossdesk::MacVirtualDisplay::IsRuntimeSupported()) {
      std::printf(
          "SKIP: CGVirtualDisplay is not supported on this macOS build\n");
      return 0;
    }
    std::printf("[0] runtime supported: yes\n");

    const bool teardown_reliable =
        crossdesk::MacVirtualDisplay::TeardownReliable();
    std::printf("[0] teardown reliable: %s\n",
                teardown_reliable ? "yes (pre-macOS-27)" :
                                    "NO — macOS 27.x defect, displays leak");
    if (!teardown_reliable) {
      std::printf(
          "    WARNING: this test will create displays that persist after\n"
          "    exit. A logout/login or reboot is required to clear them.\n");
    }

    // ── 1. Baseline snapshot ────────────────────────────────────────────
    const auto baseline = SnapshotActiveDisplays();
    PrintSet("[1] baseline active displays", baseline);

    // ── 2. Create 1920x1080 @ 60 Hz non-HiDPI ──────────────────────────
    std::printf("[2] creating 1920x1080@60 hi_dpi=false\n");
    crossdesk::MacVirtualDisplay vd;
    crossdesk::MacVirtualDisplaySpec spec;
    spec.width = 1920;
    spec.height = 1080;
    spec.refresh_rate = 60.0;
    spec.hi_dpi = false;
    spec.name = "FarlinkSmokeTest";

    std::string error;
    if (!vd.Create(spec, &error)) {
      std::fprintf(stderr, "FAIL create: %s\n", error.c_str());
      return 1;
    }
    CHECK(vd.active(), "vd.active() after create");
    CHECK(vd.display_id() != 0, "display_id non-zero");
    std::printf("    displayID = 0x%x\n", vd.display_id());

    DrainRunloop(20);

    const auto after_create = SnapshotActiveDisplays();
    PrintSet("    after create", after_create);
    CHECK(after_create.size() == baseline.size() + 1,
          "active display count grew by exactly one");
    // ── 3. Reconfigure behavior depends on platform ─────────────────────
    std::printf("[3] reconfigure attempt: 2560x1440@60\n");
    const uint32_t original_id = vd.display_id();
    spec.width = 2560;
    spec.height = 1440;
    const bool reconf_ok = vd.Reconfigure(spec, &error);

    if (teardown_reliable) {
      // Pre-macOS-27: reconfigure should succeed and the active mode
      // should actually change.
      if (!reconf_ok) {
        std::fprintf(stderr, "FAIL reconfigure: %s\n", error.c_str());
        return 1;
      }
      CHECK(vd.active(), "vd.active() after reconfigure");
      CHECK(vd.display_id() == original_id,
            "reconfigure reuses the same displayID");
      DrainRunloop(20);
      const auto after_reconf = SnapshotActiveDisplays();
      PrintSet("    after reconfigure", after_reconf);
      CHECK(CGDisplayPixelsWide(vd.display_id()) == 2560,
            "width is 2560 after reconfigure");
      CHECK(CGDisplayPixelsHigh(vd.display_id()) == 1440,
            "height is 1440 after reconfigure");
      CHECK(after_reconf.size() == after_create.size(),
            "reconfigure did not increase display count");
    } else {
      // macOS 27.x: reconfigure must FAIL honestly because the platform
      // cannot actually switch the active mode.
      std::printf("    reconfigure returned: %s\n",
                  reconf_ok ? "success (UNEXPECTED)" : "false (expected)");
      std::printf("    error: %s\n", error.c_str());
      CHECK(!reconf_ok,
            "reconfigure must fail on macOS 27.x because mode switching "
            "is broken");
      // The original display must still be active at its original spec.
      CHECK(vd.active(), "vd still active after failed reconfigure");
      CHECK(vd.display_id() == original_id,
            "displayID unchanged after failed reconfigure");
      DrainRunloop(10);
      CHECK(CGDisplayPixelsWide(vd.display_id()) == 1920,
            "width still 1920 after failed reconfigure");
      CHECK(CGDisplayPixelsHigh(vd.display_id()) == 1080,
            "height still 1080 after failed reconfigure");
    }


    // ── 4. Destroy and observe behavior ────────────────────────────────
    std::printf("[4] destroying\n");
    vd.Destroy();
    CHECK(!vd.active(), "vd.active() false after destroy");

    DrainRunloop(40);

    const auto final_set = SnapshotActiveDisplays();
    PrintSet("    final", final_set);
    PrintSet("    baseline", baseline);

    if (teardown_reliable) {
      // Pre-macOS-27: teardown should fully clean up.
      CHECK(final_set.size() == baseline.size(),
            "final active count equals baseline count");
      CHECK(final_set == baseline,
            "final active display set identical to baseline");
    } else {
      // macOS 27.x defect: we expect the display to persist.
      std::printf(
          "    note: macOS 27.x teardown defect — display expected to "
          "remain attached\n");
      if (final_set.size() > baseline.size()) {
        std::printf(
            "    confirmed: display count did not return to baseline "
            "(%zu vs %zu)\n",
            final_set.size(), baseline.size());
      } else {
        // If Apple fixed the defect in a newer 27.x, our detection should
        // be updated. For now report what we see.
        std::printf(
            "    unexpected: display count DID return to baseline — "
            "Apple may have fixed the defect; update TeardownReliable()\n");
      }
    }

    // ── Result ──────────────────────────────────────────────────────────
    std::printf("\n=== summary ===\n");
    if (failures == 0) {
      std::printf("PASS — semantics match platform behavior\n");
      if (!teardown_reliable) {
        std::printf(
            "NOTE: virtual display id=0x%x remains attached. Logout/login "
            "or reboot to clear.\n",
            original_id);
      }
      return 0;
    }
    std::printf("FAIL — %d check(s) failed\n", failures);
    return 1;
  }
}
