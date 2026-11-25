/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "platform/machine_identity.h"

#include <cstdint>
#include <filesystem>

#include "platform/path_backend.h"

#ifndef CROSSDESK_PORTABLE
#define CROSSDESK_PORTABLE 0
#endif

namespace crossdesk::platform {

std::uint64_t InstanceFnv1a64(const std::string& bytes) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string InstanceHex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(16, '0');
  for (int i = 15; i >= 0; --i) {
    result[static_cast<size_t>(i)] = digits[value & 0xf];
    value >>= 4;
  }
  return result;
}

std::string InstanceScopeRoot() {
  std::filesystem::path root;
#if CROSSDESK_PORTABLE
  // Mirror PathManager's portable layout: the data root beside the executable.
  root = GetExecutableDirectory();
  if (root.empty()) {
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    root = error ? std::filesystem::path(".") : current;
  }
  root /= "data";
#else
  root = GetConfigPath(kMachineAppName);
#endif
  if (root.empty()) return {};
  std::error_code error;
  // weakly_canonical resolves symlinks for the existing prefix and normalises
  // the rest, so /tmp-vs-/private/tmp and symlinked HOMEs collapse to one key
  // even before the directory exists. Lexically normalise as a fallback when
  // resolution fails (e.g. dangling symlink in the prefix).
  std::filesystem::path canonical = std::filesystem::weakly_canonical(root, error);
  if (error || canonical.empty()) canonical = root.lexically_normal();
  return canonical.generic_string();
}

std::string InstanceScopeHash() {
  const std::string root = InstanceScopeRoot();
  if (root.empty()) return {};
  return InstanceHex64(InstanceFnv1a64(root));
}

}  // namespace crossdesk::platform
