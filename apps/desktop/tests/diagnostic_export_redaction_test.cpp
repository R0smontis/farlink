/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic unit test for the diagnostic-export sanitiser and the
// redacted ZIP assembly. Every secret pattern is seeded into synthetic log
// text, run through the production RedactDiagnosticText, and the output is
// checked both for absence of the secret and for survival of the safe
// context around it. The ZIP test exercises the production ExportDiagnostics
// end-to-end into a temp directory and parses the archive back by hand.

#include "diagnostic_redaction.h"
#include "diagnostic_export.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

using crossdesk::feedback::RedactDiagnosticText;

void PasswordKeyValuesAreMasked() {
  const std::string out = RedactDiagnosticText(
      "connect password=1234567890 abc\n"
      "FARLINK_LAN_PASSWORD=hunter2 rest\n"
      "{\"password\":\"s3cret-value\"}\n"
      "password: abc123 end\n");
  Expect(!Contains(out, "1234567890"), "password= value must be masked");
  Expect(!Contains(out, "hunter2"), "FARLINK_LAN_PASSWORD value must be masked");
  Expect(!Contains(out, "s3cret-value"), "JSON password value must be masked");
  Expect(!Contains(out, "abc123"), "password: value must be masked");
  Expect(Contains(out, "password="), "the key itself must survive");
  Expect(Contains(out, "FARLINK_LAN_PASSWORD="), "env key must survive");
  Expect(Contains(out, "rest"), "text after the value must survive");
  Expect(Contains(out, "\"password\""), "JSON key must survive");
}

void CompositeIdPasswordIsMaskedButIdSurvives() {
  const std::string out = RedactDiagnosticText(
      "Local user id: [123456789@pw123456]\n"
      "Use client id [987654321] and save\n");
  Expect(!Contains(out, "pw123456"), "composite password must be masked");
  Expect(Contains(out, "123456789@***"), "device id must survive with mask");
  Expect(Contains(out, "987654321"), "bare ids without @ must pass through");
}

void BearerTokensAreMasked() {
  const std::string out = RedactDiagnosticText(
      "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig\n"
      "header bearer abcdef123456\n");
  Expect(!Contains(out, "eyJhbGciOiJIUzI1NiJ9"), "bearer token must be masked");
  Expect(!Contains(out, "abcdef123456"), "lowercase bearer must be masked");
  Expect(Contains(out, "Bearer ***"), "Bearer scheme label must survive");
}

void ExactLengthHexSecretsAreMasked() {
  // Independent non-overlapping vectors: each length class uses a distinct
  // character so no vector is a substring of another — a substring collision
  // would make the absence check meaningless.
  const std::string hex64(64, 'a');
  const std::string hex63(63, 'b');
  const std::string hex65(65, 'c');
  const std::string out = RedactDiagnosticText(
      "token " + hex64 + "\nshort " + hex63 + "\nlong " + hex65 + "\n");
  Expect(!Contains(out, hex64), "64-hex reconnect credential must be masked");
  Expect(Contains(out, "***redacted-64hex***"), "mask marker must be present");
  Expect(Contains(out, hex63), "63-hex strings are not the credential shape");
  Expect(Contains(out, hex65), "65-hex strings are not the credential shape");
}

void PemBlocksAreMasked() {
  const std::string out = RedactDiagnosticText(
      "key dump:\n-----BEGIN PRIVATE KEY-----\n"
      "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQC7\n"
      "-----END PRIVATE KEY-----\nafter\n");
  Expect(!Contains(out, "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQC7"),
         "PEM body must be masked");
  Expect(Contains(out, "REDACTED PRIVATE KEY"), "PEM mask marker must appear");
  Expect(Contains(out, "after"), "content after the block must survive");
}

void HomePrefixesAreMasked() {
  const std::string out = RedactDiagnosticText(
      "log at /Users/hejiayu/Library/Logs/crossdesk.log\n"
      "linux /home/henry/.config/farlink x\n"
      "win C:\\Users\\Hejia Yu\\AppData\\x y\n"
      "unc /Users/Shared/public ok\n");
  Expect(!Contains(out, "hejiayu"), "mac home username must be masked");
  Expect(Contains(out, "$HOME/Library/Logs"), "path suffix must survive");
  Expect(!Contains(out, "henry"), "linux home username must be masked");
  Expect(!Contains(out, "Hejia Yu"), "windows profile name must be masked");
  Expect(Contains(out, "%USERPROFILE%\\AppData"), "windows suffix must survive");
  Expect(Contains(out, "/Users/Shared/public"),
         "/Users/Shared is a fixed system directory and must stay intact");
}

void SafeLogLinesPassThroughUnchanged() {
  const std::string line =
      "[2026-11-27 10:00:00] [info] Net sample: remote=C-1234 rtt_ms=23.5 "
      "video_in_bps=4500000 video_in_loss=0.0123\n"
      "Session summary (controller): remote=abc duration_s=61.0 codec=H265\n";
  const std::string out = RedactDiagnosticText(line);
  Expect(out == line, "clean operational log lines must pass through byte-identical");
}

void EmptyAndBoundaryInputs() {
  Expect(RedactDiagnosticText("").empty(), "empty input must stay empty");
  const std::string just_at = "@";
  Expect(RedactDiagnosticText(just_at) == "@", "lone @ must pass through");
  const std::string at_end = "id@";
  Expect(RedactDiagnosticText(at_end) == "id@", "trailing @ without secret must pass through");
}

// --- ZIP assembly end-to-end ----------------------------------------------

void ZipContainsRedactedLogsAndSessionJson() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "farlink_redaction_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "logs");

  const std::string hex64 =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  {
    std::ofstream app(root / "logs" / "crossdesk.log");
    app << "start password=topsecret99 x\n"
        << "id 123456789@pw99999999 y\n"
        << "token " << hex64 << "\n"
        << "path /Users/tester_x/Documents\n"
        << "clean operational line\n";
    std::ofstream transport(root / "logs" / "minirtc.log");
    transport << "transport clean\n";
  }

  crossdesk::feedback::SessionSummaryFields summary{};
  summary.remote_id = "C-abc";
  summary.duration_s = 12.5;
  summary.codec = "H265";
  summary.fps_tier = 60;
  summary.width = 2560;
  summary.height = 1440;
  summary.frames_presented = 700;
  summary.intervals = 699;
  summary.freeze_count = 2;
  summary.freeze_max_ms = 340.5;
  summary.p95_max_ms = 34.0;
  summary.freezes_per_min = 9.6;
  summary.rtt_ms = 21.0;
  summary.video_in_loss = 0.01;

  const fs::path zip_path = root / "out.zip";
  const auto result = crossdesk::feedback::ExportDiagnostics(
      root / "logs", root / "logs", zip_path, "test-version", std::time(nullptr),
      &summary);
  Expect(result.error == crossdesk::feedback::ExportError::none,
         "export with session summary must succeed");

  std::ifstream zip_in(zip_path, std::ios::binary);
  Expect(zip_in.good(), "zip file must exist");
  const std::string zip((std::istreambuf_iterator<char>(zip_in)),
                        std::istreambuf_iterator<char>());
  Expect(!Contains(zip, "topsecret99"), "zip must not contain the password");
  Expect(!Contains(zip, "pw99999999"), "zip must not contain composite secret");
  Expect(!Contains(zip, hex64), "zip must not contain the 64-hex token");
  Expect(!Contains(zip, "tester_x"), "zip must not contain the username");
  Expect(Contains(zip, "clean operational line"), "clean content must survive in zip");
  Expect(Contains(zip, "session_summary.json"), "zip must contain session json entry");
  Expect(Contains(zip, "\"freeze_count\":2"), "session json must carry freeze count");
  Expect(Contains(zip, "\"codec\":\"H265\""), "session json must carry codec");
  // The stored-ZIP entries are uncompressed, so entry content is visible in
  // the byte stream directly.
  fs::remove_all(root, ec);
}

void ZipWithoutSessionSummaryOmitsEntry() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "farlink_redaction_test2";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "logs");
  {
    std::ofstream app(root / "logs" / "crossdesk.log");
    app << "ordinary\n";
  }
  const fs::path zip_path = root / "out.zip";
  const auto result = crossdesk::feedback::ExportDiagnostics(
      root / "logs", root / "logs", zip_path, "test-version");
  Expect(result.error == crossdesk::feedback::ExportError::none,
         "export without summary must succeed");
  std::ifstream zip_in(zip_path, std::ios::binary);
  const std::string zip((std::istreambuf_iterator<char>(zip_in)),
                        std::istreambuf_iterator<char>());
  Expect(!Contains(zip, "session_summary.json"),
         "no summary must mean no json entry");
  fs::remove_all(root, ec);
}

}  // namespace

int main() {
  PasswordKeyValuesAreMasked();
  CompositeIdPasswordIsMaskedButIdSurvives();
  BearerTokensAreMasked();
  ExactLengthHexSecretsAreMasked();
  PemBlocksAreMasked();
  HomePrefixesAreMasked();
  SafeLogLinesPassThroughUnchanged();
  EmptyAndBoundaryInputs();
  ZipContainsRedactedLogsAndSessionJson();
  ZipWithoutSessionSummaryOmitsEntry();
  if (failures == 0) {
    std::cout << "diagnostic_export_redaction_test: all assertions passed\n";
    return 0;
  }
  std::cerr << "diagnostic_export_redaction_test: " << failures << " failure(s)\n";
  return 1;
}
