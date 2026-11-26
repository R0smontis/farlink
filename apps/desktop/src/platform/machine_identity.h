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
// autostart entries — derives from it. Because this fork is itself named
// Farlink, the user-visible product name (window/session titles, tray tooltip,
// notification titles, process messages) is the same value and also derives
// from it, so a title that is matched functionally can never drift from the
// name the window was created with. What does NOT use this constant are the
// upstream-interoperability surfaces: wire protocol tokens, JSON type values,
// signalling hostnames and upstream service URLs — those must stay byte-for-
// byte as stock CrossDesk expects or the fork could no longer talk to peers.
inline constexpr char kMachineAppName[] = "Farlink";

// The same product name as a wide string, for the Windows tray tooltip and
// MessageBox surfaces that take UTF-16. Identical ASCII bytes; kept next to
// kMachineAppName so the user-visible name never diverges by platform.
inline constexpr wchar_t kMachineAppNameWide[] = L"Farlink";

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
