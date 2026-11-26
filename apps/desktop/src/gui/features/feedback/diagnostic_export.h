/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-11
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DIAGNOSTIC_EXPORT_H_
#define _DIAGNOSTIC_EXPORT_H_

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

namespace crossdesk::feedback {

enum class ExportError { none, no_logs, read_failed, write_failed, too_large };
struct ExportResult {
  ExportError error = ExportError::none;
};

// Plain-data per-session metrics snapshot for the optional
// session_summary.json entry. Numeric fields only; the exporter serializes
// them verbatim and never invents values. The caller decides which session
// is summarized (in practice the focused/most-recent controller session) —
// the exporter itself has no session awareness.
struct SessionSummaryFields {
  std::string remote_id;      // already non-secret: bare id, never id@password
  double duration_s = 0;
  std::string codec;
  int fps_tier = 0;
  int width = 0;
  int height = 0;
  uint64_t frames_presented = 0;
  // D8 inter-present cumulative counters.
  uint64_t intervals = 0;
  uint64_t freeze_count = 0;
  double freeze_max_ms = 0;
  double p95_max_ms = 0;
  double freezes_per_min = 0;
  // Latest network sample.
  double rtt_ms = -1;
  double video_in_loss = 0;
};

// Exports sanitized snapshots of the active application and transport logs,
// plus basic environment information. Log content is passed through
// RedactDiagnosticText before archiving: passwords, tokens, reconnect
// credentials, private keys and home-directory usernames never leave the
// machine. No account configuration is read. When `session` is non-null a
// session_summary.json entry is added.
ExportResult ExportDiagnostics(const std::filesystem::path& application_logs,
                               const std::filesystem::path& transport_logs,
                               const std::filesystem::path& destination,
                               const std::string& version,
                               std::time_t now = std::time(nullptr),
                               const SessionSummaryFields* session = nullptr);
std::string DiagnosticFileName();

}  // namespace crossdesk::feedback

#endif
