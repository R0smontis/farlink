// Structural verification of the leak-proof encode in-flight admission gate.
//
// The encode in-flight bound exists to keep capture-to-output latency bounded
// when the hardware encoder is asynchronous: Encode() returns right after
// submission and the output callback fires later on another thread, so the
// generic task-queue depth never reflects frames still inside the hardware
// pipeline. The previous design tracked the count in the transport and relied
// on every encoder invoking its output callback exactly once — true only for
// VideoToolbox. A synchronous encoder (openh264, a stub, a future NVENC/AV1
// path) can legitimately return success without ever invoking the callback,
// which would leak slots until the gate wedged the stream black.
//
// The leak-proof design moves the count into the encoder itself:
//   * MediaCodec::FramesInFlight() defaults to 0. A synchronous encoder never
//     has work in flight (Encode() resolves the frame before returning), so 0
//     is correct and the admission gate never triggers for it — the stream is
//     always offered frame N+1 regardless of whether the encoder swallowed the
//     previous one.
//   * VideoToolbox derives the count from its pending-callback registry. Every
//     registration is consumed exactly once (output callback, cancel, or
//     teardown), so the count returns to zero no matter how a frame resolves.
//   * The transport only reads the count; nothing outside the encoder mutates
//     it, so there is no cross-component invariant that can be violated.
//
// These are compile-time/source invariants, so this test reads the sources and
// asserts the patterns that the argument depends on.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path FindRepoRoot() {
  std::filesystem::path current = std::filesystem::current_path();
  while (!current.empty()) {
    if (std::filesystem::exists(current / "xmake.lua") &&
        std::filesystem::exists(
            current /
            "deps/submodules/minirtc/src/transport/ice_transport_controller.cpp")) {
      return current;
    }
    current = current.parent_path();
  }
  return {};
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

std::string ExtractSection(const std::string& source, const std::string& begin,
                           const std::string& end) {
  const size_t begin_pos = source.find(begin);
  if (begin_pos == std::string::npos) {
    return {};
  }
  const size_t end_pos = source.find(end, begin_pos + begin.size());
  if (end_pos == std::string::npos) {
    return {};
  }
  return source.substr(begin_pos, end_pos - begin_pos);
}

bool ExpectContains(const char* name, const std::string& value,
                    const std::string& expected) {
  if (value.find(expected) != std::string::npos) {
    return true;
  }
  std::cerr << name << " missing expected text: " << expected << "\n";
  return false;
}

bool ExpectNotContains(const char* name, const std::string& value,
                       const std::string& unexpected) {
  if (value.find(unexpected) == std::string::npos) {
    return true;
  }
  std::cerr << name << " contains unsafe text: " << unexpected << "\n";
  return false;
}

}  // namespace

int main() {
  const std::filesystem::path repo_root = FindRepoRoot();
  if (repo_root.empty()) {
    std::cerr << "failed to locate repository root\n";
    return 1;
  }

  const std::string media_codec =
      ReadFile(repo_root / "deps/submodules/minirtc/src/media/media_codec.h");
  const std::string vt_encoder = ReadFile(
      repo_root /
      "deps/submodules/minirtc/src/media/video/encode/video_toolbox/"
      "video_toolbox_encoder.mm");
  const std::string controller = ReadFile(
      repo_root /
      "deps/submodules/minirtc/src/transport/ice_transport_controller.cpp");
  const std::string controller_header = ReadFile(
      repo_root /
      "deps/submodules/minirtc/src/transport/ice_transport_controller.h");

  if (media_codec.empty() || vt_encoder.empty() || controller.empty() ||
      controller_header.empty()) {
    std::cerr << "failed to read one or more sources\n";
    return 1;
  }

  bool ok = true;

  // 1. The default is zero, so synchronous encoders are never gated: they are
  //    always offered the next frame even if they never invoke a callback.
  ok &= ExpectContains("media_codec", media_codec,
                       "virtual int FramesInFlight() const { return 0; }");

  // 2. VideoToolbox overrides the hook and tracks the count in its own
  //    registry — incremented when a frame callback is registered, decremented
  //    on every consumption path (output callback / cancel / teardown).
  ok &= ExpectContains("vt_encoder", vt_encoder,
                       "int FramesInFlight() const {");
  ok &= ExpectContains("vt_encoder", vt_encoder,
                       "atomic<int> frames_in_flight_{0};");
  ok &= ExpectContains("vt_encoder", vt_encoder,
                       "frames_in_flight_.fetch_add(1, std::memory_order_acq_rel);");
  ok &= ExpectContains("vt_encoder", vt_encoder,
                       "frames_in_flight_.fetch_sub(1, std::memory_order_acq_rel);");

  // 3. The transport gates admission on the encoder's own count.
  ok &= ExpectContains("controller", controller,
                       "codec->FramesInFlight()");
  ok &= ExpectContains("controller", controller, "kMaxEncodeFramesInFlight");

  // 4. Nothing in the transport keeps its own in-flight counter anymore: the
  //    leak-prone cross-component invariant is gone entirely.
  ok &= ExpectNotContains("controller", controller, "encode_frames_in_flight");
  ok &= ExpectNotContains("controller_header", controller_header,
                          "encode_frames_in_flight");
  ok &= ExpectNotContains("controller", controller, "in_flight_release");
  ok &= ExpectNotContains("controller", controller, "InFlightRelease");

  if (!ok) {
    return 1;
  }
  std::cout << "PASS\n";
  return 0;
}
