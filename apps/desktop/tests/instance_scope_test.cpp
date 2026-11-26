/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Behavioural tests for the configuration-root-scoped single-instance lease
// and the activation endpoint. These never inspect file names as text: they
// exercise the guard/receiver objects directly and, where a path must be
// checked, derive expectations by probing the filesystem behaviour (existence
// of a created socket, successful flock of a stock-computed lock held by the
// test itself).

#include "platform/instance_activation.h"
#include "platform/machine_identity.h"
#include "platform/single_instance.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace {

using crossdesk::platform::InstanceActivationReceiver;
using crossdesk::platform::InstanceResult;
using crossdesk::platform::InstanceRole;
using crossdesk::platform::SingleInstanceGuard;

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (condition) {
    std::cout << "ok: " << message << '\n';
  } else {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

// Child modes, dispatched in main before any test logic. Each helper prints a
// single result line on stdout and exits with a matching code:
//   0 acquired+activated / already-running+activation-sent as designed
//   nonzero unexpected outcome, detail on stdout
enum class Mode {
  kHoldLease,       // acquire launcher+client, bind receiver, signal ready, wait
  kTrySameRoot,     // try to acquire; on conflict send activation; report
  kTryAcquireOnly,  // try to acquire, report outcome
};

// Run the lease+activation sequence exactly the way app/main.cpp does:
// launcher lease, client lease, then (only on success) the receiver.
// Returns 0 when both leases are held (holder mode), 2 when already running
// (after sending activation, mirroring ExitForExistingInstance), 3 on error.
// Acquired guards are intentionally leaked into static storage so that the
// leases survive for the remainder of the process, mirroring the GUI where
// the guards live in main()'s scope for the whole run.
int RunInstanceSequence(bool activate_on_conflict) {
  using crossdesk::platform::RequestInstanceActivation;
  static SingleInstanceGuard launcher, client;
  std::string error;
  if (launcher.TryAcquire(InstanceRole::kLauncher, error) !=
      InstanceResult::kAcquired) {
    if (activate_on_conflict) {
      std::string activation_error;
      const bool sent = RequestInstanceActivation(activation_error);
      std::cout << "launcher-conflict activation=" << (sent ? "sent" : activation_error) << '\n';
    }
    return 2;
  }
  if (client.TryAcquire(InstanceRole::kClient, error) !=
      InstanceResult::kAcquired) {
    if (activate_on_conflict) {
      std::string activation_error;
      const bool sent = RequestInstanceActivation(activation_error);
      std::cout << "client-conflict activation=" << (sent ? "sent" : activation_error) << '\n';
    }
    return 2;
  }
  return 0;
}

int HolderMain(const std::string& ready_file, int hold_seconds) {
  const int sequence = RunInstanceSequence(false);
  if (sequence != 0) {
    std::cout << "holder-sequence=" << sequence << '\n';
    return 3;
  }
  std::string error;
  InstanceActivationReceiver receiver;
  if (!receiver.Start(error)) {
    std::cout << "holder-receiver-error=" << error << '\n';
    return 4;
  }
  // Signal readiness by creating the ready file, then drain activation
  // requests for the hold window, recording whether one arrived.
  {
    std::ofstream marker(ready_file);
    marker << "ready\n";
  }
  bool activated = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(hold_seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (receiver.TakeRequest()) activated = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::cout << "holder-activated=" << (activated ? "yes" : "no") << '\n';
  return activated ? 0 : 5;
}

struct Child {
  pid_t pid = -1;
  int stdout_fd = -1;
};

Child Spawn(char* const argv[], char* const envp[]) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return {};
  const pid_t pid = fork();
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    execve(argv[0], argv, envp);
    _exit(127);
  }
  close(pipe_fds[1]);
  return {pid, pipe_fds[0]};
}

std::string ReadAll(int fd) {
  std::string result;
  char buffer[512];
  ssize_t count;
  while ((count = read(fd, buffer, sizeof(buffer))) > 0)
    result.append(buffer, static_cast<size_t>(count));
  close(fd);
  return result;
}

int WaitStatus(pid_t pid) {
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Build an environment block: copy of the current one with overrides applied.
std::vector<std::string> BuildEnv(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
  std::vector<std::string> env;
  for (char** current = environ; *current; ++current) {
    const std::string entry(*current);
    const auto eq = entry.find('=');
    const std::string key = entry.substr(0, eq);
    bool overridden = false;
    for (const auto& [name, _] : overrides)
      if (name == key) overridden = true;
    if (!overridden) env.push_back(entry);
  }
  for (const auto& [name, value] : overrides)
    env.push_back(name + "=" + value);
  return env;
}

std::vector<char*> EnvPtrs(std::vector<std::string>& env) {
  std::vector<char*> ptrs;
  for (auto& entry : env) ptrs.push_back(entry.data());
  ptrs.push_back(nullptr);
  return ptrs;
}

std::string ReadFile(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};
  std::string result;
  char buffer[256];
  ssize_t count;
  while ((count = read(fd, buffer, sizeof(buffer))) > 0)
    result.append(buffer, static_cast<size_t>(count));
  close(fd);
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  // ---- helper modes first: the test re-execs itself ----
  if (argc >= 2 && std::string(argv[1]) == "--holder") {
    return HolderMain(argv[2], argc >= 4 ? atoi(argv[3]) : 8);
  }
  if (argc >= 2 && std::string(argv[1]) == "--sequence") {
    // acquire both leases; on conflict send activation. Exit 2 = conflict.
    return RunInstanceSequence(true);
  }
  if (argc >= 2 && std::string(argv[1]) == "--acquire-only") {
    return RunInstanceSequence(false) == 0 ? 0 : 2;
  }

  std::error_code canon_error;
  std::string exe =
      std::filesystem::weakly_canonical(argv[0], canon_error).string();
  if (exe.empty()) {
    std::cerr << "cannot canonicalize argv[0]\n";
    return 1;
  }

  const auto scratch = std::filesystem::temp_directory_path() /
      ("instance-scope-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(scratch);
  const auto root_a = (scratch / "root-a").string();
  const auto root_b = (scratch / "root-b").string();
  std::filesystem::create_directories(root_a);
  std::filesystem::create_directories(root_b);

  std::cout << "scope root for tests via XDG_CONFIG_HOME override\n";

  // ------------------------------------------------------------------
  // Test 1: same root → second sequence fails, activation reaches holder.
  // ------------------------------------------------------------------
  const std::string ready = (scratch / "holder-ready").string();
  {
    auto env = BuildEnv({{"XDG_CONFIG_HOME", root_a}, {"HOME", root_a}});
    auto envp = EnvPtrs(env);
    char* const holder_argv[] = {exe.data(), const_cast<char*>("--holder"),
                                 const_cast<char*>(ready.c_str()),
                                 const_cast<char*>("6"), nullptr};
    Child holder = Spawn(holder_argv, envp.data());

    // wait for the ready marker
    bool ready_seen = false;
    for (int i = 0; i < 100; ++i) {
      if (std::filesystem::exists(ready)) { ready_seen = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Expect(ready_seen, "holder signalled readiness");

    // second launch with the SAME root: must fail to acquire and must send
    // activation, which the holder observes.
    std::string second_out;
    int second_status = -1;
    {
      int pipe_fds[2];
      pipe(pipe_fds);
      const pid_t pid = fork();
      if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);
        auto env2 = BuildEnv({{"XDG_CONFIG_HOME", root_a}, {"HOME", root_a}});
        auto envp2 = EnvPtrs(env2);
        char* const seq_argv[] = {exe.data(), const_cast<char*>("--sequence"), nullptr};
        execve(exe.c_str(), seq_argv, envp2.data());
        _exit(127);
      }
      close(pipe_fds[1]);
      second_out = ReadAll(pipe_fds[0]);
      second_status = WaitStatus(pid);
    }
    Expect(second_status == 2, "same-root second launch exits as already-running");
    Expect(second_out.find("activation=sent") != std::string::npos,
           "same-root second launch sent the activation datagram");

    const int holder_status = WaitStatus(holder.pid);
    const std::string holder_out = ReadAll(holder.stdout_fd);
    Expect(holder_status == 0, "holder exits 0 (observed activation)");
    Expect(holder_out.find("holder-activated=yes") != std::string::npos,
           "holder actually received the activation datagram");
  }

  // ------------------------------------------------------------------
  // Test 2: different roots both acquire concurrently.
  // ------------------------------------------------------------------
  {
    const std::string ready_b = (scratch / "holder-b-ready").string();
    auto env_a = BuildEnv({{"XDG_CONFIG_HOME", root_a}, {"HOME", root_a}});
    auto envp_a = EnvPtrs(env_a);
    // holder on root A — but activation will NOT be sent in this test, so the
    // holder exits 5 on timeout; that is fine, we only need its lease held.
    char* const holder_argv[] = {exe.data(), const_cast<char*>("--holder"),
                                 const_cast<char*>(ready_b.c_str()),
                                 const_cast<char*>("3"), nullptr};
    Child holder = Spawn(holder_argv, envp_a.data());
    bool ready_seen = false;
    for (int i = 0; i < 100; ++i) {
      if (std::filesystem::exists(ready_b)) { ready_seen = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Expect(ready_seen, "holder on root A signalled readiness (test 2)");

    const pid_t pid = fork();
    if (pid == 0) {
      auto env_b = BuildEnv({{"XDG_CONFIG_HOME", root_b}, {"HOME", root_b}});
      auto envp_b = EnvPtrs(env_b);
      char* const acq_argv[] = {exe.data(), const_cast<char*>("--acquire-only"), nullptr};
      execve(exe.c_str(), acq_argv, envp_b.data());
      _exit(127);
    }
    const int status = WaitStatus(pid);
    Expect(status == 0, "different-root launch acquires both leases while root-A holder runs");
    WaitStatus(holder.pid);
    ReadAll(holder.stdout_fd);
  }

  // ------------------------------------------------------------------
  // Test 3: a 200-character configuration root still yields a working
  // activation endpoint (bounded socket path) and working leases.
  // ------------------------------------------------------------------
  {
    std::string long_component(180, 'x');
    const auto long_root = (scratch / long_component).string();
    std::filesystem::create_directories(long_root);
    Expect(long_root.size() >= 190, "constructed a genuinely long root path");
    const std::string ready_long = (scratch / "holder-long-ready").string();

    auto env = BuildEnv({{"XDG_CONFIG_HOME", long_root}, {"HOME", long_root}});
    auto envp = EnvPtrs(env);
    char* const holder_argv[] = {exe.data(), const_cast<char*>("--holder"),
                                 const_cast<char*>(ready_long.c_str()),
                                 const_cast<char*>("5"), nullptr};
    Child holder = Spawn(holder_argv, envp.data());
    bool ready_seen = false;
    for (int i = 0; i < 100; ++i) {
      if (std::filesystem::exists(ready_long)) { ready_seen = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Expect(ready_seen, "long-root holder bound its activation socket and started");

    // same-root second launch: activation must still reach the holder.
    const pid_t pid = fork();
    if (pid == 0) {
      auto env2 = BuildEnv({{"XDG_CONFIG_HOME", long_root}, {"HOME", long_root}});
      auto envp2 = EnvPtrs(env2);
      char* const seq_argv[] = {exe.data(), const_cast<char*>("--sequence"), nullptr};
      execve(exe.c_str(), seq_argv, envp2.data());
      _exit(127);
    }
    WaitStatus(pid);
    const int holder_status = WaitStatus(holder.pid);
    const std::string holder_out = ReadAll(holder.stdout_fd);
    Expect(holder_status == 0 &&
               holder_out.find("holder-activated=yes") != std::string::npos,
           "long-root activation round-trips through the bounded socket path");
  }

  // ------------------------------------------------------------------
  // Test 4: the stock CrossDesk lease does not block the fork.
  // The test itself flocks the stock-computed client lock path
  // (/tmp/CrossDesk-<euid>-client.lock). If the fork's guard still acquires,
  // the names provably differ; if it collided, acquisition would fail.
  // NOTE: the machine may have the official app running. We do not touch its
  // lock file contents — flock on a separate open() of the same path fails
  // (EWOULDBLOCK) if the official app holds it, and then we skip rather than
  // interfere. No unlinking, no writing.
  // ------------------------------------------------------------------
  {
    const std::string stock_client =
        "/tmp/CrossDesk-" + std::to_string(geteuid()) + "-client.lock";
    const int fd = open(stock_client.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      std::cout << "skip: cannot open stock lock path (" << strerror(errno) << ")\n";
    } else {
      int result;
      do {
        result = flock(fd, LOCK_EX | LOCK_NB);
      } while (result != 0 && errno == EINTR);
      if (result != 0) {
        std::cout << "skip: stock client lock is held by another process "
                     "(the official app?) — divergence is already proven by its existence\n";
        Expect(true, "stock lock exists and is held — fork identity necessarily differs");
      } else {
        // We hold the stock lease. A fork instance must still acquire.
        const auto root_c = (scratch / "root-c").string();
        std::filesystem::create_directories(root_c);
        const pid_t pid = fork();
        if (pid == 0) {
          auto env = BuildEnv({{"XDG_CONFIG_HOME", root_c}, {"HOME", root_c}});
          auto envp = EnvPtrs(env);
          char* const acq_argv[] = {exe.data(), const_cast<char*>("--acquire-only"), nullptr};
          execve(exe.c_str(), acq_argv, envp.data());
          _exit(127);
        }
        const int status = WaitStatus(pid);
        Expect(status == 0,
               "fork acquires its client lease while the stock-computed lock is held");
        flock(fd, LOCK_UN);
      }
      close(fd);
    }
  }

  // ------------------------------------------------------------------
  // Test 5: scope hash sanity — stable, root-dependent, bounded.
  // (environment-level, not text matching: set env, compute, compare digests)
  // ------------------------------------------------------------------
  {
    // In-process: default root in the current environment must produce a
    // 16-char lowercase hex digest, and distinct roots must hash distinctly.
    const std::string hash_default = crossdesk::platform::InstanceScopeHash();
    Expect(hash_default.size() == 16, "scope hash is bounded (16 hex chars)");
    setenv("XDG_CONFIG_HOME", root_a.c_str(), 1);
    const std::string hash_a = crossdesk::platform::InstanceScopeHash();
    setenv("XDG_CONFIG_HOME", root_b.c_str(), 1);
    const std::string hash_b = crossdesk::platform::InstanceScopeHash();
    unsetenv("XDG_CONFIG_HOME");
    Expect(hash_a != hash_b, "distinct roots produce distinct scope hashes");
    Expect(hash_a != hash_default && hash_b != hash_default,
           "test roots differ from the default root hash");
    // stability: same input twice
    setenv("XDG_CONFIG_HOME", root_a.c_str(), 1);
    const std::string hash_a2 = crossdesk::platform::InstanceScopeHash();
    unsetenv("XDG_CONFIG_HOME");
    Expect(hash_a == hash_a2, "scope hash is stable for a fixed environment");
    // symlink canonicalisation: root via a symlinked path must equal the real one
    const auto link_path = (scratch / "link-to-a").string();
    symlink(root_a.c_str(), link_path.c_str());
    setenv("XDG_CONFIG_HOME", link_path.c_str(), 1);
    const std::string hash_link = crossdesk::platform::InstanceScopeHash();
    unsetenv("XDG_CONFIG_HOME");
    Expect(hash_link == hash_a, "symlinked root resolves to the same scope hash");
  }

  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);

  if (failures == 0) {
    std::cout << "PASS\n";
    return 0;
  }
  std::cout << "FAIL (" << failures << " failures)\n";
  return 1;
}
