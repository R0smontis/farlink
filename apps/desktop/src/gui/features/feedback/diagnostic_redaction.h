/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _DIAGNOSTIC_REDACTION_H_
#define _DIAGNOSTIC_REDACTION_H_

#include <cctype>
#include <string>
#include <string_view>

namespace crossdesk::feedback {

// Pure, deterministic log sanitizer for the diagnostic bundle. Runs entirely
// on in-memory text before anything is written into the ZIP; no regex
// dependency, single pass per pattern family, fixed replacements.
//
// Secret shapes covered (verified against actual log formats):
//   - id@password composites: device login ids carry the password after '@'
//     in signaling-derived identifiers. Only the part after '@' is masked,
//     and only when the composite looks like an identifier (no spaces).
//   - password key-values: password=..., "password":"...", 'password': '...',
//     PASSWORD=..., FARLINK_LAN_PASSWORD=... (any casing of the key).
//   - bearer/authorization tokens: "Bearer <token>", authorization headers.
//   - 64-character hex strings: reconnect credentials are exactly this shape.
//     Shorter hex (hashes, ids) is never touched.
//   - PEM private key blocks: BEGIN/END ... PRIVATE KEY ... inclusive.
//   - Home directory prefixes: /Users/<name> -> $HOME,
//     /home/<name> -> $HOME, X:\Users\<name> -> %USERPROFILE%.
//     The username itself is local-identifying information.
// for downstream parsing; nothing is ever dropped or reordered.
inline std::string RedactDiagnosticText(std::string_view input) {
  std::string out;
  out.reserve(input.size());

  const auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
  };
  const auto is_ident = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  };

  size_t pos = 0;
  const size_t n = input.size();

  // --- PEM private key blocks: masked as whole multi-line spans before any
  // line-oriented pattern runs, since the base64 body would otherwise survive.
  // Implemented inside the main scan because blocks cannot overlap anything.
  const std::string_view pem_begin = "-----BEGIN";
  const std::string_view pem_key = "PRIVATE KEY-----";
  const std::string_view pem_end = "-----END";

  while (pos < n) {
    // PEM block: copy nothing of the original, emit a fixed marker.
    if (input.substr(pos, pem_begin.size()) == pem_begin) {
      const size_t key_at = input.find(pem_key, pos);
      if (key_at != std::string_view::npos &&
          key_at < input.find('\n', pos)) {
        // A BEGIN line carrying a PRIVATE KEY type. Find the matching END.
        const size_t end_at = input.find(pem_end, key_at);
        if (end_at != std::string_view::npos) {
          const size_t end_line = input.find('\n', end_at);
          out += "-----BEGIN REDACTED PRIVATE KEY-----\n";
          pos = end_line == std::string_view::npos ? n : end_line + 1;
          continue;
        }
      }
    }

    const char c = input[pos];

    // --- '@' composite: <ident>@<secret>. Trigger on '@' preceded by an
    // identifier character; mask everything up to the next delimiter. The
    // identifier part is kept: it is the device id, which diagnostics need.
    if (c == '@' && !out.empty() && is_ident(out.back())) {
      size_t end = pos + 1;
      // The secret never contains whitespace, quotes, commas or braces; stop
      // at the first structural delimiter. Legitimate email-like text in logs
      // is not expected in this application's own log output, and the cost of
      // over-masking is zero for diagnostics.
      while (end < n && (is_ident(input[end]) || input[end] == '.'))
        ++end;
      if (end > pos + 1) {
        out += "@***";
        pos = end;
        continue;
      }
    }

    // --- password key-value pairs. Scan case-insensitively for the key.
    // Matches: password=..., password:..., "password": "...", with optional
    // quotes around the value; the quotes are kept, the content masked.
    {
      const std::string_view rest = input.substr(pos);
      const auto key_len = [&]() -> size_t {
        // Case-insensitive key match: password/PASSWORD/Password/pAsSwOrD.
        constexpr std::string_view key = "password";
        if (rest.size() < key.size()) return 0;
        for (size_t i = 0; i < key.size(); ++i) {
          if (std::tolower(static_cast<unsigned char>(rest[i])) != key[i])
            return 0;
        }
        // Word boundary: an alphanumeric before the key means it is part of
        // a longer word ('mypasswords') and not a secret key. Underscore is
        // explicitly allowed so environment-style composite names like
        // FARLINK_LAN_PASSWORD still mask their values.
        if (pos > 0 && std::isalnum(static_cast<unsigned char>(input[pos - 1])))
          return 0;
        // Must be followed by = or : or quote-then-colon.
        size_t after = key.size();
        if (after < rest.size() && rest[after] == '"') ++after;
        if (after < rest.size() && (rest[after] == '=' || rest[after] == ':'))
          return after + 1;
        return 0;
      }();
      if (key_len != 0) {
        // Copy the key and separator verbatim, then mask the value.
        out += rest.substr(0, key_len);
        size_t vpos = pos + key_len;
        // Skip whitespace after the separator (JSON style: "password": "x").
        while (vpos < n && (input[vpos] == ' ' || input[vpos] == '\t')) {
          out += input[vpos];
          ++vpos;
        }
        char quote = 0;
        if (vpos < n && (input[vpos] == '"' || input[vpos] == '\'')) {
          quote = input[vpos];
          out += input[vpos];
          ++vpos;
        }
        size_t vend = vpos;
        if (quote) {
          while (vend < n && input[vend] != quote) ++vend;
        } else {
          while (vend < n && input[vend] != ' ' && input[vend] != '\t' &&
                 input[vend] != '\n' && input[vend] != '\r' &&
                 input[vend] != ',' && input[vend] != '}' &&
                 input[vend] != ')' && input[vend] != ']')
            ++vend;
        }
        if (vend > vpos) out += "***";
        pos = vend;
        continue;
      }
    }

    // --- Bearer tokens: the word Bearer (exact case per RFC) followed by a
    // token. Authorization values occasionally use lowercase; accept both.
    if ((input.substr(pos, 7) == "Bearer " || input.substr(pos, 7) == "bearer ") &&
        (pos == 0 || !is_ident(input[pos - 1]))) {
      out += input.substr(pos, 7);
      size_t end = pos + 7;
      while (end < n && input[end] != ' ' && input[end] != '\t' &&
             input[end] != '\n' && input[end] != '\r' && input[end] != '"' &&
             input[end] != '\'')
        ++end;
      if (end > pos + 7) {
        out += "***";
        pos = end;
        continue;
      }
      pos = end == pos + 7 ? pos + 7 : end;
      if (end == pos) continue;
    }

    // --- 64-char hex secrets. Boundary-checked: exactly 64 hex chars with
    // non-hex (or string edges) on both sides.
    if (is_hex(c) && (pos == 0 || !is_hex(input[pos - 1]))) {
      size_t end = pos;
      while (end < n && is_hex(input[end])) ++end;
      if (end - pos == 64 && (end == n || !is_hex(input[end]))) {
        // Confirm there is no hex continuation beyond 64 — the run length is
        // exactly 64 by construction of the loop above.
        out += "***redacted-64hex***";
        pos = end;
        continue;
      }
    }

    // --- Home directory prefixes. /Users/<name>/ and /home/<name>/ on
    // posix, C:\Users\<name>\ on Windows (either slash direction survives
    // because the separator after the name is what anchors the match).
    if ((input.substr(pos, 7) == "/Users/" || input.substr(pos, 6) == "/home/") &&
        (pos == 0 || input[pos - 1] == '/' || input[pos - 1] == ' ' ||
         input[pos - 1] == '"' || input[pos - 1] == '\'' || input[pos - 1] == '(' ||
         input[pos - 1] == '=' || input[pos - 1] == ':')) {
      const size_t prefix = input[pos + 1] == 'U' ? 7 : 6;
      size_t end = pos + prefix;
      while (end < n && input[end] != '/' && input[end] != ' ' &&
             input[end] != '\t' && input[end] != '\n' && input[end] != '"' &&
             input[end] != '\'')
        ++end;
      if (end > pos + prefix) {
        // /Users/Shared is a fixed system directory, not a user home; leave
        // it intact so shared-path log lines stay truthful.
        const bool shared = prefix == 7 &&
            input.substr(pos + prefix, end - pos - prefix) == "Shared";
        if (!shared) {
          // "/Users/<name>" collapses to the bare "$HOME" token: the
          // remaining path suffix (starting at the next '/') reads naturally
          // after it.
          out += "$HOME";
          pos = end;
          continue;
        }
      }
    }
    if (pos + 9 <= n && (input[pos] == 'C' || input[pos] == 'c' || input[pos] == 'D') &&
        input.substr(pos + 1, 8) == ":\\Users\\" &&
        (pos == 0 || input[pos - 1] == ' ' || input[pos - 1] == '"' ||
         input[pos - 1] == '\'' || input[pos - 1] == '(' || input[pos - 1] == '=' ||
         input[pos - 1] == ':')) {
      size_t end = pos + 9;
      // Windows user names may contain spaces, so the scan runs to the next
      // path separator, not to whitespace; quotes/newlines still terminate
      // because a path cannot span them.
      while (end < n && input[end] != '\\' && input[end] != '/' &&
             input[end] != '\t' && input[end] != '\n' &&
             input[end] != '"' && input[end] != '\'')
        ++end;
      if (end > pos + 9) {
        // "X:\Users\<name>" collapses to the bare "%USERPROFILE%" token.
        out += "%USERPROFILE%";
        pos = end;
        continue;
      }
    }

    out += c;
    ++pos;
  }
  return out;
}

}  // namespace crossdesk::feedback

#endif
