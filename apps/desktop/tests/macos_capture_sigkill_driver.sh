#!/bin/bash
# macos_capture_sigkill_driver.sh <path-to-macos_capture_sigkill_tool> [deadline-seconds]
#
# Driver for the SIGKILL capture-cleanup verification. The tool binary is a
# helper with two modes; this script performs the orchestration:
#   1. capture mode: helper hides/dissociates the cursor and installs a
#      swallow-all event tap, prints READY
#   2. the driver SIGKILLs it — exactly the crash scenario
#   3. probe mode: a second run verifies from outside that events flow again
#      and the cursor tracks warps (the WindowServer reaped everything the
#      dead captive held)
#
# Deadline enforcement: every phase is time-boxed (default 20 s total). On any
# timeout the capture helper is SIGKILLed first — a machine must never be left
# with a swallow-all tap — and the script exits non-zero.
set -u

TOOL="${1:?usage: macos_capture_sigkill_driver.sh <tool-binary> [deadline-seconds]}"
DEADLINE="${2:-20}"
[ -x "$TOOL" ] || { echo "tool not executable: $TOOL" >&2; exit 2; }

CAPTURE_PID=""
cleanup() {
  # Never leave a swallow-all tap behind, whatever happens.
  if [ -n "$CAPTURE_PID" ] && kill -0 "$CAPTURE_PID" 2>/dev/null; then
    kill -KILL "$CAPTURE_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

START=$(python3 -c 'import time; print(time.time())')
deadline_exceeded() {
  python3 -c "import sys,time; sys.exit(0 if time.time()-$START > $DEADLINE else 1)"
}

# --- phase 1: capture helper up and READY ---
OUT=$(mktemp /tmp/sigkill-capture.XXXXXX)
"$TOOL" >"$OUT" 2>&1 &
CAPTURE_PID=$!
ready=0
for _ in $(seq 1 100); do
  grep -q "READY" "$OUT" 2>/dev/null && { ready=1; break; }
  kill -0 "$CAPTURE_PID" 2>/dev/null || break
  deadline_exceeded && break
  sleep 0.1
done
if [ "$ready" != 1 ]; then
  echo "FAIL: capture helper never became READY"
  cat "$OUT"
  rm -f "$OUT"
  exit 1
fi
echo "capture helper READY (pid $CAPTURE_PID)"

# --- phase 2: the crash ---
kill -KILL "$CAPTURE_PID" 2>/dev/null
wait "$CAPTURE_PID" 2>/dev/null
CAPTURE_PID=""
echo "capture helper SIGKILLed"

# --- phase 3: probe with deadline ---
PROBE_OUT=$(mktemp /tmp/sigkill-probe.XXXXXX)
"$TOOL" --probe >"$PROBE_OUT" 2>&1 &
PROBE_PID=$!
probe_done=0
for _ in $(seq 1 200); do
  kill -0 "$PROBE_PID" 2>/dev/null || { probe_done=1; break; }
  deadline_exceeded && break
  sleep 0.1
done
if [ "$probe_done" != 1 ]; then
  echo "FAIL: probe did not finish within the deadline"
  kill -KILL "$PROBE_PID" 2>/dev/null
  cat "$PROBE_OUT"
  rm -f "$OUT" "$PROBE_OUT"
  exit 1
fi
wait "$PROBE_PID" 2>/dev/null
PROBE_RC=$?
cat "$PROBE_OUT"
rm -f "$OUT" "$PROBE_OUT"

deadline_exceeded && { echo "FAIL: total deadline ${DEADLINE}s exceeded"; exit 1; }

if [ "$PROBE_RC" = 0 ]; then
  echo "PASS: capture cleanup survives SIGKILL (events flow, cursor tracks)"
  exit 0
fi
echo "FAIL: probe rc=$PROBE_RC"
exit 1
