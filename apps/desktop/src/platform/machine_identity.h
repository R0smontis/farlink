/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef CROSSDESK_PLATFORM_MACHINE_IDENTITY_H_
#define CROSSDESK_PLATFORM_MACHINE_IDENTITY_H_

#include <string>

namespace crossdesk::platform {

// Machine-level application identity of this fork. This is the ONLY
// definition; every per-machine resource — single-instance locks, the
// activation socket, config/cache/log directory names, the LaunchAgent and
// autostart entries — derives from it. User-visible strings, wire protocol
// tokens, signalling hostnames and bundle metadata of upstream interoperability
// do NOT use this constant; they are not machine-level identity.
inline constexpr char kMachineAppName[] = "Farlink";

// Canonical, symlinks-resolved configuration root for the current launch —
// the portable data directory in portable builds, otherwise the backend's
// config path for kMachineAppName. This is the scope key of the single-
// instance lease and the activation endpoint: two launches sharing this root
// are the same instance; two launches with different roots (separate portable
// copies, or XDG/HOME isolation) may run side by side. Environment-dependent
// by design, but stable across launches in the same environment.
std::string InstanceScopeRoot();

// Lowercase hex FNV-1a-64 of InstanceScopeRoot(), empty when the root cannot
// be determined. Bounded length regardless of how long the root path is, for
// use inside /tmp file names and sockaddr_un path inputs.
std::string InstanceScopeHash();

// FNV-1a-64 over an arbitrary byte string. Exposed for tests and for the
// activation endpoint, which folds the session into the same digest scheme.
std::uint64_t InstanceFnv1a64(const std::string& bytes);
std::string InstanceHex64(std::uint64_t value);

}  // namespace crossdesk::platform

#endif  // CROSSDESK_PLATFORM_MACHINE_IDENTITY_H_
