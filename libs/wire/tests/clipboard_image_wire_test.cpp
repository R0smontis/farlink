/*
 * Clipboard image wire protocol unit tests: framing, chunking, reassembly,
 * capability round-trip, oversize rejection, stale eviction.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <clipboard_image_wire.h>
#include <remote_action.h>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

// A minimal but structurally valid PNG byte sequence: signature plus enough
// trailing bytes to exercise multi-chunk logic. The wire layer only validates
// the 8-byte signature, which is exactly what the test asserts.
std::string FakePng(std::size_t size) {
  static constexpr char kSignature[8] = {
      '\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
  std::string png(size, '\0');
  std::memcpy(png.data(), kSignature, sizeof(kSignature));
  for (std::size_t i = sizeof(kSignature); i < size; ++i)
    png[i] = char((i * 131 + 7) & 0xff);
  return png;
}

}  // namespace

int main() {
  using namespace crossdesk;
  using namespace crossdesk::clipboard_image;
  bool ok = true;

  // ---- Magic / frame detection --------------------------------------------
  {
    ok &= Expect(!IsImageFrame(""), "empty frame detected as image");
    ok &= Expect(!IsImageFrame(std::string_view(kMagic, kMagicBytes)),
                 "magic-only frame without header detected as image");
    const auto frame = Frame(kChunk, 1, 100, 0, 1, "x");
    ok &= Expect(IsImageFrame(frame), "valid image frame not detected");
    // A plain-text clipboard value can never collide: 0x89 is not a valid
    // UTF-8 leading byte, so no legitimate text starts with the magic.
    ok &= Expect(!IsImageFrame("hello clipboard text, definitely long enough"),
                 "plain text misdetected as image frame");
  }

  // ---- PNG signature validation -------------------------------------------
  {
    ok &= Expect(ValidPngSignature("\x89PNG\r\n\x1a\n12345678", 16),
                 "valid PNG signature rejected");
    ok &= Expect(!ValidPngSignature("\x89PNG\r\n\x1a", 7),
                 "truncated signature accepted");
    ok &= Expect(!ValidPngSignature("PNG\r\n\x1a\nxx", 10),
                 "signature without 0x89 accepted");
    ok &= Expect(!ValidPngSignature(nullptr, 16), "null data accepted");
  }

  // ---- Chunked send / ordered reassembly ----------------------------------
  {
    const std::string png = FakePng(100000);  // forces multiple 32KiB chunks
    std::vector<std::string> frames;
    const bool sent = SendImage(png, 42, 48 * 1024,
                                [&](const std::string& frame) {
                                  frames.push_back(frame);
                                  return true;
                                });
    ok &= Expect(sent, "SendImage failed for valid 100000-byte PNG");
    ok &= Expect(frames.size() == 4, "unexpected chunk count for 100000 bytes");
    for (const auto& frame : frames) {
      ok &= Expect(frame.size() <= 48 * 1024, "frame exceeds message limit");
      ok &= Expect(frame.size() <= kHeaderBytes + kChunkBytes,
                   "frame exceeds chunk byte cap");
    }
    Assembler assembler;
    Assembler::Result result;
    for (const auto& frame : frames) {
      const auto step = assembler.Receive(frame);
      if (step.transfer_id) result = step;
    }
    ok &= Expect(result.transfer_id == 42, "reassembled transfer id mismatch");
    ok &= Expect(result.png == png, "reassembled PNG bytes differ");
    ok &= Expect(!assembler.pending(), "assembler still pending after completion");
  }

  // ---- Out-of-order reassembly --------------------------------------------
  {
    const std::string png = FakePng(70000);
    std::vector<std::string> frames;
    SendImage(png, 7, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames.push_back(frame);
      return true;
    });
    ok &= Expect(frames.size() >= 3, "test requires at least 3 chunks");
    // Deliver in reverse order.
    Assembler assembler;
    Assembler::Result result;
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
      const auto step = assembler.Receive(*it);
      if (step.transfer_id) result = step;
    }
    ok &= Expect(result.transfer_id == 7, "out-of-order transfer id mismatch");
    ok &= Expect(result.png == png, "out-of-order PNG bytes differ");
  }

  // ---- Incomplete transfer yields nothing ----------------------------------
  {
    const std::string png = FakePng(70000);
    std::vector<std::string> frames;
    SendImage(png, 9, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames.push_back(frame);
      return true;
    });
    Assembler assembler;
    Assembler::Result result;
    for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
      const auto step = assembler.Receive(frames[i]);
      ok &= Expect(!step.transfer_id, "incomplete transfer produced a result");
    }
    ok &= Expect(assembler.pending(), "assembler lost incomplete transfer");
  }

  // ---- Oversize rejection ---------------------------------------------------
  {
    const std::string oversized = FakePng(kMaxImageBytes + 1);
    bool send_called = false;
    const bool sent = SendImage(oversized, 5, 48 * 1024,
                                [&](const std::string&) {
                                  send_called = true;
                                  return true;
                                });
    ok &= Expect(!sent && !send_called, "oversized PNG accepted by SendImage");

    // A frame stream claiming more than kMaxImageBytes total must never
    // allocate. Craft a single syntactically valid first chunk with an
    // oversized total.
    const uint32_t count = uint32_t((uint64_t(kMaxImageBytes) + 1 + kChunkBytes - 1) / kChunkBytes);
    const auto evil = Frame(kChunk, 3, uint32_t(kMaxImageBytes + 1),
                            0, count, std::string(kChunkBytes, 'z'),
                            uint32_t(kChunkBytes));
    Assembler assembler;
    // Note: total is clamped to uint32; kMaxImageBytes+1 fits comfortably.
    const auto step = assembler.Receive(evil);
    ok &= Expect(!step.transfer_id && !assembler.pending(),
                 "oversized wire transfer not rejected");
  }

  // ---- Stale eviction -------------------------------------------------------
  {
    const std::string png = FakePng(70000);
    std::vector<std::string> frames;
    SendImage(png, 11, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames.push_back(frame);
      return true;
    });
    Assembler assembler;
    const auto t0 = std::chrono::steady_clock::now();
    assembler.Receive(frames[0], t0);
    ok &= Expect(assembler.pending(), "transfer missing after first chunk");
    // Sixteen seconds later the incomplete transfer must be evicted.
    assembler.EvictStale(t0 + std::chrono::seconds(16));
    ok &= Expect(!assembler.pending(), "stale transfer not evicted after timeout");
    // Within the timeout the transfer survives.
    assembler.Receive(frames[0], t0);
    assembler.EvictStale(t0 + std::chrono::seconds(14));
    ok &= Expect(assembler.pending(), "fresh transfer evicted too early");
  }

  // ---- Cancel frame drops the transfer --------------------------------------
  {
    const std::string png = FakePng(70000);
    std::vector<std::string> frames;
    SendImage(png, 13, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames.push_back(frame);
      return true;
    });
    Assembler assembler;
    assembler.Receive(frames[0]);
    ok &= Expect(assembler.pending(), "transfer missing before cancel");
    const auto cancel = Control(kCancel, 13);
    const auto step = assembler.Receive(cancel);
    ok &= Expect(!step.transfer_id && !assembler.pending(),
                 "cancel frame did not drop transfer");
  }

  // ---- A new transfer id supersedes an incomplete one -----------------------
  {
    const std::string png_a = FakePng(70000);
    const std::string png_b = FakePng(40000);
    std::vector<std::string> frames_a, frames_b;
    SendImage(png_a, 21, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames_a.push_back(frame);
      return true;
    });
    SendImage(png_b, 22, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames_b.push_back(frame);
      return true;
    });
    Assembler assembler;
    assembler.Receive(frames_a[0]);  // incomplete A
    Assembler::Result result;
    for (const auto& frame : frames_b) {
      const auto step = assembler.Receive(frame);
      if (step.transfer_id) result = step;
    }
    ok &= Expect(result.transfer_id == 22 && result.png == png_b,
                 "superseding transfer did not complete cleanly");
  }

  // ---- Duplicate chunks are idempotent --------------------------------------
  {
    const std::string png = FakePng(40000);
    std::vector<std::string> frames;
    SendImage(png, 31, 32 * 1024 + kHeaderBytes, [&](const std::string& frame) {
      frames.push_back(frame);
      return true;
    });
    Assembler assembler;
    Assembler::Result result;
    int completions = 0;
    for (const auto& frame : frames) {
      for (const auto& step : {assembler.Receive(frame),  // first delivery
                              assembler.Receive(frame)})  // exact duplicate
        if (step.transfer_id) {
          result = step;
          ++completions;
        }
    }
    // Duplicates within an incomplete transfer are ignored; the transfer
    // completes exactly once, when the final distinct chunk first arrives.
    ok &= Expect(completions == 1 && result.transfer_id == 31 && result.png == png,
                 "duplicate-chunk transfer corrupted");
  }

  // ---- Invalid inputs --------------------------------------------------------
  {
    bool called = false;
    ok &= Expect(!SendImage("", 1, 48 * 1024, [&](const std::string&) {
                   called = true;
                   return true;
                 }) && !called,
                 "empty PNG accepted by SendImage");
    ok &= Expect(!SendImage(FakePng(100), 0, 48 * 1024, [&](const std::string&) {
                   called = true;
                   return true;
                 }) && !called,
                 "zero transfer id accepted by SendImage");
    ok &= Expect(!SendImage(FakePng(100), 1, kHeaderBytes, [&](const std::string&) {
                   called = true;
                   return true;
                 }) && !called,
                 "header-only message limit accepted by SendImage");
    // Send failure propagates.
    int attempts = 0;
    ok &= Expect(!SendImage(FakePng(100), 1, 48 * 1024, [&](const std::string&) {
                   ++attempts;
                   return false;
                 }) && attempts == 1,
                 "send failure not propagated");
  }

  // ---- FNV-1a hash stability -------------------------------------------------
  {
    const std::string png = FakePng(4096);
    ok &= Expect(HashPng(png) == HashPng(png), "hash not deterministic");
    std::string other = png;
    other[100] = other[100] == 0 ? 1 : 0;
    ok &= Expect(HashPng(png) != HashPng(other),
                 "hash collision on single-byte difference");
    ok &= Expect(HashPng("") == 14695981039346656037ull,
                 "empty-input FNV-1a offset basis mismatch");
  }

  // ---- Capability flag round-trip through host information -------------------
  {
    RemoteAction host{};
    host.type = ControlType::host_infomation;
    host.i.display_num = 0;
    host.i.display_list = nullptr;
    host.i.left = host.i.top = host.i.right = host.i.bottom = nullptr;
    std::strcpy(host.i.host_name, "test-host");
    host.i.host_name_size = 9;
    host.i.supports_clipboard_image = true;
    const std::string message = host.to_json();
    ok &= Expect(message.find("supports_clipboard_image") != std::string::npos,
                 "supports_clipboard_image not serialized");
    RemoteAction parsed{};
    ok &= Expect(parsed.from_json(message) && parsed.i.supports_clipboard_image,
                 "supports_clipboard_image round trip lost");
    FreeRemoteAction(parsed);

    // Legacy peers: the key is absent, the flag must default to false.
    RemoteAction legacy{};
    legacy.type = ControlType::host_infomation;
    legacy.i.display_num = 0;
    legacy.i.display_list = nullptr;
    legacy.i.left = legacy.i.top = legacy.i.right = legacy.i.bottom = nullptr;
    std::strcpy(legacy.i.host_name, "legacy");
    legacy.i.host_name_size = 6;
    legacy.i.supports_clipboard_image = true;
    std::string wire = legacy.to_json();
    // Simulate a legacy peer by stripping the key entirely.
    const auto pos = wire.find("\"supports_clipboard_image\":true");
    ok &= Expect(pos != std::string::npos, "test setup: key not found");
    // Replace the key-value pair with a no-op duplicate of an existing one.
    wire.replace(pos, std::strlen("\"supports_clipboard_image\":true"),
                 "\"supports_high_fps\":false");
    RemoteAction parsed_legacy{};
    ok &= Expect(parsed_legacy.from_json(wire) &&
                     !parsed_legacy.i.supports_clipboard_image,
                 "absent capability defaulted to true");
    FreeRemoteAction(parsed_legacy);
    FreeRemoteAction(legacy);
  }

  if (ok) {
    std::cout << "clipboard_image_wire_test: all tests passed\n";
    return 0;
  }
  return 1;
}
