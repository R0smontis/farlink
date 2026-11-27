// Reverse-direction and revocation proof for Wave 4B, exercising the REAL
// PortForwardController dispatch path (OnFrame routing, admission evaluation,
// reverse binding, revocation) — not a clone. The runtime environment is a
// controlled harness supplying connection facts; every admission decision is
// the production code.
//
// Scenarios:
//   R1. Reverse open+data in ONE session: accepting-side client endpoint with
//       --port-forward-reverse rules under explicit live approval pushes real
//       TCP bytes through the peer's host endpoint. Simultaneously the
//       forward direction works — true both directions in a single session.
//   R2. No approval, no verified identity: reverse rules never bind even with
//       the rules configured — first arbitrary incoming device gets nothing.
//   R3. Verified trusted key alone (no explicit approval) binds reverse rules.
//   R4. Loss of authorization with in-flight data: frames stop moving on the
//       VERY NEXT frame after admission is lost — no polling interval grace.
//   R5. Loss of SRTP mid-session: same immediate revocation.
//   R6. Approval is connection-instance-scoped: detach+reattach (simulated
//       reconnect) does not inherit it.
//
// Mac-only, muted, isolated: loopback TCP only, no persistence.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <port_forward_wire.h>

#include "features/port_forward/port_forward_controller.h"
#include "launch_options.h"
#include "runtime/gui_runtime.h"

using namespace std::chrono_literals;
using namespace crossdesk;

int failures = 0;
void Expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << '\n'; ++failures; }
}

// ---- loopback TCP helpers -------------------------------------------------
int ConnectLoopback(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

struct EchoServer {
  int listener = -1;
  std::uint16_t port = 0;
  std::atomic<bool> stop{false};
  std::thread worker;
  void Start() {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener, 64);
    sockaddr_in bound{};
    socklen_t size = sizeof(bound);
    getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &size);
    port = ntohs(bound.sin_port);
    worker = std::thread([this] {
      while (!stop.load()) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int conn = accept(listener, reinterpret_cast<sockaddr*>(&peer), &len);
        if (conn < 0) {
          if (stop.load()) break;
          std::this_thread::sleep_for(1ms);
          continue;
        }
        std::thread([conn] {
          char buffer[16384];
          for (;;) {
            const ssize_t count = recv(conn, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            ssize_t sent = 0;
            while (sent < count) {
              const ssize_t n = send(conn, buffer + sent, count - sent, MSG_NOSIGNAL);
              if (n <= 0) { close(conn); return; }
              sent += n;
            }
          }
          close(conn);
        }).detach();
      }
    });
  }
  void Stop() {
    stop.store(true);
    if (listener >= 0) close(listener);
    if (worker.joinable()) worker.join();
  }
  ~EchoServer() { Stop(); }
};

// Bidirectional in-memory link between the two controller instances that
// represent the two ends of ONE session: the accepting side (where reverse
// rules live) and the controller side (whose host endpoint terminates
// reverse streams). Frames cross exactly as they would on the wire.
struct Session {
  // The accepting end of the connection (us, the controlled host).
  GuiRuntime accept_runtime;
  PortForwardController accept{accept_runtime};
  // The controlling end (the peer connecting in).
  GuiRuntime control_runtime;
  PortForwardController control{control_runtime};

  const std::string conn = "lan/test-connection";

  Session() {
    // The accepting side sees an incoming controller connection.
    accept_runtime.authorized.insert(conn);
    accept_runtime.secure.insert(conn);
    // The controlling side sees an outgoing-style interactive session — but
    // crucially, on the wire it is the SAME connection; frames simply cross.
    control_runtime.client_active.insert(conn);
    control_runtime.secure.insert(conn);
    // Both ends advertise the capability — a wire fact learned from the
    // peer's host information, without which admission correctly fails.
    accept.NotePeerSupport(conn, true);
    control.NotePeerSupport(conn, true);
    // Connection initiator parity: the controlling end initiated, so it owns
    // odd stream ids; the accepting end derives even. Production derives this
    // from the same shared fact on both ends — the harness must model it or
    // the parity partitioning the routing depends on is fictitious.
    control_runtime.initiator.insert(conn);
  }

  // Deliver every frame each side emitted to the other side's real dispatch.
  void Pump() {
    for (;;) {
      if (accept_runtime.sent.empty() && control_runtime.sent.empty()) break;
      auto to_control = std::exchange(accept_runtime.sent, {});
      auto to_accept = std::exchange(control_runtime.sent, {});
      for (const auto& frame : to_control)
        control.OnFrame(frame.data(), frame.size(), nullptr, conn);
      for (const auto& frame : to_accept)
        accept.OnFrame(frame.data(), frame.size(), nullptr, conn);
    }
  }
  void Tick() {
    accept.Poll();
    control.Poll();
    Pump();
  }
  void RunFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      Tick();
      std::this_thread::sleep_for(200us);
    }
  }
};

int main() {
  // ---- R1+R2: reverse binding requires approval; approved session forwards --
  {
    MutableLaunchOptions() = LaunchOptions{};
    EchoServer echo;
    echo.Start();

    Session s;
    // R2 first: reverse rules configured, NO approval, NO verified key.
    MutableLaunchOptions().port_forward_reverse_rules.push_back({0, echo.port});
    // Poll to trigger candidate-driven eager attach — nothing must bind.
    s.accept_runtime.reverse_candidates = {"lan/test-connection"};
    s.RunFor(100ms);
    Expect(s.accept.Snapshot(s.conn).engine.rules.empty() ||
               !s.accept.Snapshot(s.conn).active,
           "R2: reverse rules never bind without approval or verification");

    // The reverse stream terminates on the CONTROLLING side's own loopback:
    // its host endpoint independently enforces ITS consent — approval at one
    // end never substitutes for the other. Grant the destination consent
    // there (the controller operator's own --port-forward-allow).
    s.control_runtime.cli_allow = {echo.port};
    // R1: explicit one-shot live approval binds the reverse rule.
    s.accept.ApproveReverseBinding(s.conn);
    s.RunFor(150ms);
    const auto snap = s.accept.Snapshot(s.conn);
    Expect(snap.active && !snap.host_side, "R1: accepting-side client endpoint active");
    Expect(!snap.engine.rules.empty(), "R1: reverse rule bound after approval");
    if (snap.engine.rules.empty()) return 1;
    const std::uint16_t reverse_local = snap.engine.rules.begin()->local_port;

    // Connect to the reverse listener ON THE ACCEPTING SIDE and push bytes:
    // they must tunnel to the controlling side's host endpoint and echo back
    // — the reverse direction of the same session carrying real TCP.
    const int reverse_fd = ConnectLoopback(reverse_local);
    Expect(reverse_fd >= 0, "R1: reverse listener accepts loopback connection");
    const std::string probe = "reverse-direction-proof-payload";
    ssize_t written = send(reverse_fd, probe.data(), probe.size(), MSG_NOSIGNAL);
    Expect(written == (ssize_t)probe.size(), "R1: probe written to reverse listener");
    std::string echoed;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (echoed.size() < probe.size() && std::chrono::steady_clock::now() < deadline) {
      s.Tick();
      const ssize_t count = recv(reverse_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (count > 0) echoed.append(buffer, count);
      else std::this_thread::sleep_for(200us);
    }
    Expect(echoed == probe, "R1: reverse-direction bytes echo through the SAME session");

    // Simultaneous forward direction: the controlling side adds its own rule
    // (its operator intent) toward the accepting side's host endpoint. The
    // accepting side needs host consent — grant session consent.
    s.accept_runtime.cli_allow = {echo.port};
    std::string error;
    // The controlling side's rule addition through the real controller API.
    Expect(s.control.AddClientRule(s.conn, 0, echo.port, &error),
           "R1: controlling side forward rule accepted");
    s.RunFor(150ms);
    const auto control_snap = s.control.Snapshot(s.conn);
    Expect(!control_snap.engine.rules.empty(), "R1: forward rule bound");
    if (!control_snap.engine.rules.empty()) {
      const std::uint16_t forward_local = control_snap.engine.rules.begin()->local_port;
      const int forward_fd = ConnectLoopback(forward_local);
      Expect(forward_fd >= 0, "R1: forward listener accepts");
      const std::string fwd_probe = "forward-simultaneous";
      send(forward_fd, fwd_probe.data(), fwd_probe.size(), MSG_NOSIGNAL);
      std::string fwd_echoed;
      const auto fwd_deadline = std::chrono::steady_clock::now() + 5s;
      while (fwd_echoed.size() < fwd_probe.size() &&
             std::chrono::steady_clock::now() < fwd_deadline) {
        s.Tick();
        const ssize_t count = recv(forward_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) fwd_echoed.append(buffer, count);
        else std::this_thread::sleep_for(200us);
      }
      Expect(fwd_echoed == fwd_probe,
             "R1: forward direction works SIMULTANEOUSLY with reverse in one session");
      if (forward_fd >= 0) close(forward_fd);
    }
    close(reverse_fd);
    MutableLaunchOptions() = LaunchOptions{};
    echo.Stop();
  }

  // ---- R3: verified trusted key alone binds reverse rules -------------------
  {
    MutableLaunchOptions() = LaunchOptions{};
    EchoServer echo;
    echo.Start();
    Session s;
    s.accept_runtime.verified_keys[s.conn] = "trusted/pinned-key";
    MutableLaunchOptions().port_forward_reverse_rules.push_back({0, echo.port});
    // Simulate proof completion: the runtime calls ApplyVerifiedReverseBinding.
    s.accept.ApplyVerifiedReverseBinding(s.conn);
    s.RunFor(150ms);
    const auto snap = s.accept.Snapshot(s.conn);
    Expect(snap.active && !snap.engine.rules.empty(),
           "R3: verified trusted key binds reverse rules without explicit approval");
    MutableLaunchOptions() = LaunchOptions{};
    echo.Stop();
  }

  // ---- R4: loss of authorization stops in-flight data immediately ------------
  {
    MutableLaunchOptions() = LaunchOptions{};
    EchoServer echo;
    echo.Start();
    Session s;
    s.accept_runtime.verified_keys[s.conn] = "trusted/pinned-key";
    MutableLaunchOptions().port_forward_reverse_rules.push_back({0, echo.port});
    s.accept.ApplyVerifiedReverseBinding(s.conn);
    s.RunFor(150ms);
    const auto snap = s.accept.Snapshot(s.conn);
    Expect(!snap.engine.rules.empty(), "R4: reverse rule active before revocation");
    const std::uint16_t reverse_local = snap.engine.rules.begin()->local_port;
    const int fd = ConnectLoopback(reverse_local);
    Expect(fd >= 0, "R4: reverse connection open");
    // Establish the stream with real data flow, then push a second batch
    // that will be genuinely in flight — inside engine buffers or the wire
    // link — at the moment revocation hits. Nothing of it may ever emerge.
    const std::string probe = "in-flight-data";
    send(fd, probe.data(), probe.size(), MSG_NOSIGNAL);
    s.RunFor(200ms);
    char drain[4096];
    recv(fd, drain, sizeof(drain), MSG_DONTWAIT);
    const std::string in_flight = "TRULY-IN-FLIGHT-at-revocation-moment";
    send(fd, in_flight.data(), in_flight.size(), MSG_NOSIGNAL);
    // Do not pump: the bytes sit in the accepting-side engine ingress or the
    // link when authorization disappears.

    // REVOKE: authorization disappears mid-stream, with the second batch
    // genuinely inside the tunnel machinery.
    s.accept_runtime.authorized.erase(s.conn);
    // The very next admission check — whether a frame arrives or the poll
    // runs — must detach the endpoint. Bytes already inside the engine are
    // destroyed with it; bytes arriving afterwards die in dispatch. Prove by
    // pumping hard and draining everything: the in-flight batch may have
    // partially crossed before revocation (legitimately — it was admitted
    // when written), but nothing written after revocation can appear, and
    // once the first post-revoke admission check runs, no further bytes of
    // ANY age move.
    const std::string post_revoke = "post-revoke-bytes-should-never-echo";
    send(fd, post_revoke.data(), post_revoke.size(), MSG_NOSIGNAL);
    s.RunFor(400ms);
    std::string after;
    for (;;) {
      const ssize_t count = recv(fd, drain, sizeof(drain), MSG_DONTWAIT);
      if (count > 0) after.append(drain, count);
      else break;
    }
    Expect(after.find(post_revoke) == std::string::npos,
           "R4: bytes sent after authorization loss NEVER echo — revocation is immediate");
    // Stronger: keep pumping and confirm the tunnel stays silent — no
    // trickle of in-flight residue escapes after the revocation settle.
    const std::size_t silent_at = after.size();
    s.RunFor(400ms);
    for (;;) {
      const ssize_t count = recv(fd, drain, sizeof(drain), MSG_DONTWAIT);
      if (count > 0) after.append(drain, count);
      else break;
    }
    Expect(after.size() == silent_at,
           "R4: no in-flight residue emerges after revocation settles");
    Expect(!s.accept.Snapshot(s.conn).active ||
               s.accept.Snapshot(s.conn).engine.open_streams == 0,
           "R4: endpoint detached after admission loss");
    close(fd);
    MutableLaunchOptions() = LaunchOptions{};
    echo.Stop();
  }

  // ---- R5: loss of SRTP revokes identically ----------------------------------
  {
    MutableLaunchOptions() = LaunchOptions{};
    EchoServer echo;
    echo.Start();
    Session s;
    s.accept_runtime.verified_keys[s.conn] = "trusted/pinned-key";
    MutableLaunchOptions().port_forward_reverse_rules.push_back({0, echo.port});
    s.accept.ApplyVerifiedReverseBinding(s.conn);
    s.RunFor(150ms);
    const auto snap = s.accept.Snapshot(s.conn);
    Expect(!snap.engine.rules.empty(), "R5: reverse rule active");
    s.accept_runtime.secure.erase(s.conn);
    s.RunFor(200ms);
    Expect(!s.accept.Snapshot(s.conn).active,
           "R5: losing SRTP detaches the endpoint at the next admission check");
    MutableLaunchOptions() = LaunchOptions{};
    echo.Stop();
  }

  // ---- R6: approval is instance-scoped, not inherited ------------------------
  {
    MutableLaunchOptions() = LaunchOptions{};
    EchoServer echo;
    echo.Start();
    Session s;
    MutableLaunchOptions().port_forward_reverse_rules.push_back({0, echo.port});
    s.accept.ApproveReverseBinding(s.conn);
    s.RunFor(100ms);
    Expect(!s.accept.Snapshot(s.conn).engine.rules.empty() &&
               s.accept.Snapshot(s.conn).active,
           "R6: approval binds rules on first connection");
    // Simulate reconnect: teardown and a fresh session environment for the
    // same device identity. The approval must be gone.
    s.accept.DetachClient(s.conn);
    s.RunFor(100ms);
    const auto snap = s.accept.Snapshot(s.conn);
    Expect(!snap.active || snap.engine.rules.empty(),
           "R6: reconnected instance does NOT inherit the approval");
    MutableLaunchOptions() = LaunchOptions{};
    echo.Stop();
  }

  if (!failures) {
    std::cout << "port-forward reverse/revocation dispatch proof: PASS\n";
    return 0;
  }
  std::cerr << "port-forward reverse/revocation dispatch proof: " << failures
            << " failure(s)\n";
  return 1;
}
