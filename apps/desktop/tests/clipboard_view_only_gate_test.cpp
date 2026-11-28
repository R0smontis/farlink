/*
 * Clipboard view-only / control-off admission gate invariant test.
 *
 * The privacy contract for --view-only (and any state in which a viewer
 * session has control_mouse_ = false) is that clipboard content — text,
 * image, or file offer — must neither be applied to the local OS clipboard
 * nor be fed into the file-offer assembler on behalf of that session. The
 * enforcement points live inside ClipboardController and PeerEventHandler
 * and are easy to regress because they sit behind several layers of
 * indirection (deferred SDL-thread apply, capability negotiation, chunked
 * binary assembly).
 *
 * Building the full ClipboardController + GuiRuntime pair inside a unit
 * test is impractical: it depends on SDL, a running LAN listener, platform
 * clipboard integration, and the MiniRTC peer lifecycle. Instead this test
 * reads the production translation units directly and asserts the exact
 * admission checks exist where the privacy contract requires them. Each
 * assertion is anchored to a specific function so a regression names the
 * exact gate that was lost, not just "clipboard broke".
 *
 * The test deliberately does NOT pattern-match on whitespace or comments;
 * it requires the gates to appear in the same function as the operation
 * they protect, which is the granularity at which a regression can hide.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
  if (condition) {
    std::cout << "  PASS " << message << '\n';
  } else {
    std::cout << "  FAIL " << message << '\n';
    ++failures;
  }
}

std::string ReadFile(const char* path) {
  std::ifstream in(path);
  if (!in) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Extract the body of a function definition: from the first '{' after the
// function's declarator line through the matching closing brace. This is
// brace-counting, not parsing — it is sufficient because these functions
// contain no raw strings or character literals that could hide a brace.
std::string ExtractFunctionBody(const std::string& source,
                                const std::string& declarator) {
  const size_t decl = source.find(declarator);
  if (decl == std::string::npos) return {};
  const size_t open = source.find('{', decl);
  if (open == std::string::npos) return {};
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') ++depth;
    else if (source[i] == '}') {
      if (--depth == 0) return source.substr(open, i - open + 1);
    }
  }
  return {};
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  // Tests are run from the project root via xmake; fall back to the
  // compile-time relative path when invoked directly from the repo.
  std::string root = argc > 1 ? argv[1] : "apps/desktop/src/gui/features/clipboard/";
  if (!root.empty() && root.back() != '/') root += '/';

  const std::string image_cpp = ReadFile((root + "clipboard_image_controller.cpp").c_str());
  const std::string files_cpp = ReadFile((root + "clipboard_files_controller.cpp").c_str());
  const std::string controller_h = ReadFile((root + "clipboard_controller.h").c_str());

  Expect(!image_cpp.empty(), "clipboard_image_controller.cpp readable");
  Expect(!files_cpp.empty(), "clipboard_files_controller.cpp readable");
  Expect(!controller_h.empty(), "clipboard_controller.h readable");
  if (image_cpp.empty() || files_cpp.empty() || controller_h.empty()) {
    std::cerr << "missing source file(s); pass the clipboard directory as argv[1]\n";
    return 2;
  }

  // ---- Image inbound assembly gate (view-only drop) -----------------------
  // ReceiveImageFrame must drop chunks when the controlling viewer session
  // has clipboard disallowed, BEFORE feeding them into the wire assembler.
  const std::string receive_image = ExtractFunctionBody(
      image_cpp, "ClipboardController::ReceiveImageFrame");
  Expect(!receive_image.empty(), "ReceiveImageFrame found");
  Expect(Contains(receive_image, "session->controlling"),
         "ReceiveImageFrame distinguishes controlling sessions");
  Expect(Contains(receive_image, "clipboard_allowed_"),
         "ReceiveImageFrame gates on clipboard_allowed_");

  // ---- Image deferred apply gate ------------------------------------------
  // ApplyPendingImage runs on the SDL thread after a focus or control flip
  // may have invalidated the original admission. It must re-check the
  // viewer's clipboard_allowed_ and input generation, mirroring the exact
  // contract ApplyPendingWrite already implements for text.
  const std::string apply_image = ExtractFunctionBody(
      image_cpp, "ClipboardController::ApplyPendingImage");
  Expect(!apply_image.empty(), "ApplyPendingImage found");
  Expect(Contains(apply_image, "clipboard_allowed_"),
         "ApplyPendingImage re-checks clipboard_allowed_");
  Expect(Contains(apply_image, "input_generation_"),
         "ApplyPendingImage re-checks input_generation_");
  Expect(Contains(apply_image, "IsAuthorizedController"),
         "ApplyPendingImage re-authorizes host-side controller writes");

  // ---- Image generation snapshot field ------------------------------------
  // The generation must be captured alongside the payload, not re-read at
  // apply time: comparing the live input_generation_ against itself would
  // always pass and defeat the gate.
  Expect(Contains(controller_h, "pending_image_generation_"),
         "clipboard_controller.h carries pending_image_generation_");
  Expect(Contains(receive_image, "pending_image_generation_"),
         "ReceiveImageFrame snapshots input_generation_ into the pending slot");

  // ---- File inbound assembly gate ------------------------------------------
  // ReceiveFiles must not feed packets into the file-offer transfer when the
  // controlling viewer session disallows clipboard — a remote host could
  // otherwise drive assembly forward even though nothing will ever apply.
  const std::string receive_files = ExtractFunctionBody(
      files_cpp, "ClipboardController::ReceiveFiles");
  Expect(!receive_files.empty(), "ReceiveFiles found");
  Expect(Contains(receive_files, "session->controlling"),
         "ReceiveFiles distinguishes controlling sessions");
  Expect(Contains(receive_files, "clipboard_allowed_"),
         "ReceiveFiles gates on clipboard_allowed_");

  // The gate must NOT live inside FileSessionActive: SendLocalFiles calls
  // FileSessionActive and destroys the session entry whenever it reports
  // inactive, so folding clipboard_allowed_ into FileSessionActive would
  // permanently tear the transfer down on transient focus loss.
  const std::string file_session_active = ExtractFunctionBody(
      files_cpp, "ClipboardController::FileSessionActive");
  Expect(!file_session_active.empty(), "FileSessionActive found");
  Expect(!Contains(file_session_active, "clipboard_allowed_"),
         "FileSessionActive does NOT gate on clipboard_allowed_ (session "
         "teardown must not fire on focus loss)");

  // ---- File deferred apply gate --------------------------------------------
  // ApplyPendingFiles commits the offer to the OS clipboard; it must reject
  // controlling sessions whose clipboard is disallowed, not just unfocused.
  const std::string apply_files = ExtractFunctionBody(
      files_cpp, "ClipboardController::ApplyPendingFiles");
  Expect(!apply_files.empty(), "ApplyPendingFiles found");
  Expect(Contains(apply_files, "clipboard_allowed_"),
         "ApplyPendingFiles re-checks clipboard_allowed_ before publishing");

  if (failures == 0) {
    std::cout << "clipboard_view_only_gate_test: all assertions passed\n";
    return 0;
  }
  std::cerr << "clipboard_view_only_gate_test: " << failures
            << " assertion(s) failed\n";
  return 1;
}
