/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Inert HostAudioMute backend for platforms without a mute implementation
 * (Linux). Construction is always safe; Mute reports the missing capability
 * honestly instead of pretending to mute. Never compiled on macOS or
 * Windows, where platform/macos/host_audio_mute.mm and
 * platform/windows/host_audio_mute_win.cpp provide the real backends.
 */

#include "host_audio_mute.h"

namespace crossdesk {

HostAudioMute::HostAudioMute() = default;
HostAudioMute::~HostAudioMute() = default;

bool HostAudioMute::Supported() { return false; }

bool HostAudioMute::Mute() { return false; }

void HostAudioMute::Restore() {}

}  // namespace crossdesk
