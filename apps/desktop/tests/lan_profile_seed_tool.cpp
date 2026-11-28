/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// lan_profile_seed_tool — create a complete, bootable disposable LAN test
// profile using ONLY the production identity path.
//
// Why this exists: a LAN profile is not just identity.json. Identity::Load
// (lan_identity.cpp:386-447) creates certificate.pem/private-key.pem ONLY
// when identity.json is absent, then unconditionally reads and verifies all
// three. A hand-scripted JSON-only seed therefore produces a profile that
// categorically cannot start — and any scripted crypto/ACL duplication is a
// second implementation of exactly the code that already exists here. This
// tool drives the real production objects instead:
//
//   Identity(dir)           constructor
//   Load()                  creates cert+key+store through the production
//                           path (including private_file owner-only
//                           directory/file protection on every OS)
//   RegisterPassword(pw)    production argon2id verifier derivation
//   SetPassword(pw, v)      production verifier+encrypted-password write
//   SaveListenerMode/       production listener configuration
//     SaveSettings
//
// Usage:
//   printf '%s\n' '<6-char-pw>' | lan_profile_seed_tool --dir <fresh-lan-dir> \
//       [--port 52888] [--self-test]
//
// Contract:
//   - the password arrives on stdin as EXACTLY one line, never argv
//   - --dir must NOT exist (fresh destination only); the tool never adopts,
//     overwrites, or rewrites existing credential state — an existing path
//     is an operator decision, not a script decision
//   - no GUI, no capture, no listeners, no network: the process only writes
//     the profile files and, with --self-test, reloads them through the
//     same production path before exiting
//   - output is one JSON object on stdout with non-secret facts only:
//     device_id, port, file names, self-test booleans. The plaintext
//     password, key material and verifier are never printed.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "lan_identity.h"

namespace {

std::string ReadSinglePasswordLine() {
  // Exactly one line on stdin; a trailing newline (LF or CRLF) is the pipe
  // convention. Anything more is a malformed transport, not a password guess.
  std::string input{std::istreambuf_iterator<char>(std::cin), {}};
  if (!input.empty() && input.back() == '\n') input.pop_back();
  if (!input.empty() && input.back() == '\r') input.pop_back();
  if (input.find('\n') != std::string::npos ||
      input.find('\r') != std::string::npos) {
    std::fprintf(stderr, "stdin must carry exactly one password line\n");
    std::exit(2);
  }
  return input;
}

// Strict port parse: every character must be decimal digits (full
// consumption) and the value must fit 1..65535. std::atol would silently
// accept "52888abc" as 52888 — a mistyped port landing in a credential file
// the operator distributes.
bool ParsePortStrict(const std::string& text, unsigned* out) {
  if (text.empty() || text.size() > 5) return false;
  unsigned value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (value < 1 || value > 65535) return false;
  *out = value;
  return true;
}

// Negative/positive coverage for ParsePortStrict, exercised inside
// --self-test so the strictness claims are run, not just written.
bool PortParserSelfTest() {
  unsigned value = 0;
  const char* const bad[] = {"52888abc", "0", "65536", "", "abc", "5288 ",
                             " 5288", "52888;", "123456", "-1", "+80"};
  for (const char* input : bad)
    if (ParsePortStrict(input, &value)) return false;
  const struct { const char* text; unsigned expected; } good[] = {
      {"1", 1}, {"80", 80}, {"52888", 52888}, {"65535", 65535}};
  for (const auto& case_ : good) {
    value = 0;
    if (!ParsePortStrict(case_.text, &value) || value != case_.expected)
      return false;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dir;
  // the operator's --dir text exactly as typed: printed in the refusal
  // message instead of path::c_str(), which is wchar_t on Windows and UB
  // through a narrow %s (in practice a one-character printout on exactly
  // the path where the message matters).
  std::string dir_arg;
  unsigned port = 52888;
  bool self_test = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--dir") {
      dir_arg = next("--dir");
      dir = dir_arg;
    }
    else if (arg == "--port") {
      unsigned value = 0;
      if (!ParsePortStrict(next("--port"), &value)) {
        std::fprintf(stderr,
                     "--port must be decimal digits only, range 1..65535\n");
        return 2;
      }
      port = value;
    } else if (arg == "--self-test")
      self_test = true;
    else {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "--dir <fresh-lan-dir> is required\n");
    return 2;
  }
  if (std::filesystem::exists(dir)) {
    std::fprintf(stderr,
                 "refusing: %s already exists - this tool seeds fresh "
                 "disposable destinations only and never adopts or overwrites "
                 "existing credential state\n",
                 dir_arg.c_str());
    return 1;
  }

  const std::string password = ReadSinglePasswordLine();
  if (!crossdesk::lan::ValidPassword(password)) {
    std::fprintf(stderr,
                 "password must be exactly 6 ASCII letters/digits\n");
    return 2;
  }

  try {
    // The constructor itself is inert; Load() is the production bootstrap:
    // private_file::EnsureDirectory (owner-only directory, platform-native
    // protection), certificate+key generation, initial store write — byte-
    // identical to what the app produces on first launch.
    crossdesk::lan::Identity identity(dir);
    identity.Load();
    // RegisterPassword/SetPassword are the exact production write path for
    // the verifier and the locally encrypted password entry.
    const auto verifier = crossdesk::lan::Identity::RegisterPassword(password);
    identity.SetPassword(password, verifier);
    // listen-all dual-stack wildcard entry keyed "::", as the audit prescribes.
    identity.SaveListenerMode(true, {{crossdesk::lan::kAllInterfaces, port}});

    bool reloaded = false;
    bool verify_ok = false;
    bool port_ok = false;
    bool cert_key_present = false;
    bool port_parser_ok = false;
    if (self_test) {
      // Prove bootability through the same production read path the app
      // uses: a fresh Identity over the seeded directory must parse the
      // store, verify the certificate against the private key, accept the
      // verifier against the plaintext, and report the configured port.
      crossdesk::lan::Identity check(dir);
      check.Load();
      reloaded = true;
      verify_ok = crossdesk::lan::Identity::VerifyPassword(
          check.verifier(), password);
      port_ok = check.PortFor(crossdesk::lan::kAllInterfaces) == port;
      cert_key_present = std::filesystem::exists(check.certificate()) &&
                         std::filesystem::exists(check.private_key());
      // The strict --port parser's negative cases (trailing garbage, 0,
      // 65536 and non-digit shapes) and positive boundary cases, so the
      // strictness claim is executed rather than merely written.
      port_parser_ok = PortParserSelfTest();
    }

    nlohmann::json out = {
        {"ok", true},
        {"dir", std::filesystem::absolute(dir).string()},
        {"device_id", identity.id()},
        {"port", port},
        {"files", {"identity.json", "certificate.pem", "private-key.pem",
                   "password-key"}},
        {"self_test",
         self_test ? nlohmann::json{{"reloaded", reloaded},
                                    {"password_verifies", verify_ok},
                                    {"port_matches", port_ok},
                                    {"cert_and_key_present", cert_key_present},
                                    {"port_parser_strict", port_parser_ok}}
                   : nlohmann::json(nullptr)},
    };
    std::cout << out.dump(2) << "\n";
    if (self_test && !(reloaded && verify_ok && port_ok && cert_key_present &&
                       port_parser_ok))
      return 1;
    return 0;
  } catch (const std::exception& exc) {
    std::fprintf(stderr, "seed failed: %s\n", exc.what());
    return 1;
  }
}
