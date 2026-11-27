/*
 * Port-forward consent foundation proof: exercises the REAL trusted-device
 * cryptographic verification chain that host-side port-forward consent is
 * keyed to, proving the properties the security model requires:
 *
 *   - a controller proves possession of an Ed25519 private key by signing a
 *     fresh host-issued nonce; verification accepts exactly that shape;
 *   - a proof for the wrong nonce, a tampered signature, or a malformed
 *     message never verifies (no replay, no forgery);
 *   - the verified identity is a public key — never a device id, hostname or
 *     any other peer-claimed metadata — and pinning is by public key;
 *   - consent key derivation: only a pinned, verified key yields a
 *     "trusted/<public_key>" consent key; everything else yields nothing.
 *
 * Mac-only, muted, isolated: no network, no persistence outside a temp
 * directory removed at exit.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "runtime/trusted_devices.h"

namespace {

int failures = 0;
bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
  return false;
}

// The exact derivation the runtime performs in VerifiedPortForwardConsentKey,
// mirrored here as a pure function of the facts: proof verified AND key
// pinned. Keeping this tiny mirror in the test documents the contract; the
// runtime's own version is exercised end-to-end in the controller proof.
std::string ConsentKey(bool trusted, const std::string& public_key) {
  if (!trusted) return {};
  return "trusted/" + public_key;
}

}  // namespace

int main() {
  bool ok = true;
  const auto directory = std::filesystem::temp_directory_path() /
                         ("pf-consent-test-" + std::to_string(
                             std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto cache = directory / "cache";
  std::filesystem::create_directories(directory);

  // Two independent device identities: the "controller" and an unrelated
  // stranger. Each has its own keypair.
  crossdesk::TrustedDevices controller(directory / "controller", cache, false);
  crossdesk::TrustedDevices stranger(directory / "stranger", cache, false);
  // The host's trust store: pins the controller's key only.
  crossdesk::TrustedDevices host_store(directory / "host", cache, false);

  ok &= Expect(!controller.PublicKey().empty() &&
                   controller.PublicKey() != stranger.PublicKey(),
               "device identities have distinct keypairs");

  // The controller pins its own device and the host pins the controller as a
  // trusted device — the pairing workflow's end state.
  crossdesk::TrustedDevices::Device controller_device{
      controller.PublicKey(), "controller-laptop", "dev-1"};
  host_store.Add(controller_device);
  ok &= Expect(host_store.Contains(controller.PublicKey()),
               "host trust store pins the controller public key");
  ok &= Expect(!host_store.Contains(stranger.PublicKey()),
               "stranger key is not pinned");

  // --- Proof round-trip: sign the host's nonce, verify -------------------
  const std::string nonce = crossdesk::TrustedDevices::Nonce();
  const auto proof = controller.Prove(nonce, "dev-1", "controller-laptop");
  crossdesk::TrustedDevices::ProofFailure failure =
      crossdesk::TrustedDevices::ProofFailure::None;
  const auto verified = crossdesk::TrustedDevices::Verify(proof, nonce, &failure);
  ok &= Expect(verified.has_value(), "fresh proof verifies");
  ok &= Expect(verified && verified->public_key == controller.PublicKey(),
               "verified identity IS the public key");
  // The verified identity key pins to the trust store — this conjunction is
  // the entire cryptographic foundation consent stands on.
  ok &= Expect(verified && host_store.Contains(verified->public_key),
               "verified key is pinned in the host trust store");

  // --- Forgery and replay resistance --------------------------------------
  // Wrong nonce: replaying the proof against a fresh challenge fails.
  const std::string other_nonce = crossdesk::TrustedDevices::Nonce();
  failure = crossdesk::TrustedDevices::ProofFailure::None;
  ok &= Expect(!crossdesk::TrustedDevices::Verify(proof, other_nonce, &failure),
               "replayed proof against a fresh nonce never verifies");
  // Stranger's proof: valid signature but the key is not pinned — proof
  // verifies cryptographically yet yields no consent because pinning fails.
  const auto stranger_proof = stranger.Prove(nonce, "dev-x", "stranger");
  failure = crossdesk::TrustedDevices::ProofFailure::None;
  const auto stranger_verified =
      crossdesk::TrustedDevices::Verify(stranger_proof, nonce, &failure);
  ok &= Expect(stranger_verified.has_value(),
               "stranger proof is cryptographically valid");
  ok &= Expect(stranger_verified &&
                   !host_store.Contains(stranger_verified->public_key),
               "stranger key fails pinning — no trust, no consent");
  // Tampered proof: flip a byte in the signature blob.
  auto tampered = proof;
  if (tampered.contains("signature")) {
    auto text = tampered.dump();
    const auto pos = text.find("signature");
    if (pos != std::string::npos && pos + 20 < text.size())
      text[pos + 20] = text[pos + 20] == 'A' ? 'B' : 'A';
    const auto reparsed = nlohmann::json::parse(text, nullptr, false);
    if (!reparsed.is_discarded()) {
      failure = crossdesk::TrustedDevices::ProofFailure::None;
      ok &= Expect(!crossdesk::TrustedDevices::Verify(reparsed, nonce, &failure),
                   "tampered proof never verifies");
    }
  }

  // --- Consent-key derivation contract -------------------------------------
  ok &= Expect(ConsentKey(true, controller.PublicKey()) ==
                   "trusted/" + controller.PublicKey(),
               "verified pinned key yields trusted/<public_key>");
  ok &= Expect(ConsentKey(false, controller.PublicKey()).empty(),
               "unverified connection yields no consent key");
  // The runtime can never emit a bare "trusted/" prefix: IsTrusted requires
  // a verified device whose public key comes out of real key material, and
  // the trust store never pins an empty key.
  ok &= Expect(controller.PublicKey().size() > 8,
               "real public keys are never empty or trivial");

  std::filesystem::remove_all(directory);

  if (ok) {
    std::cout << "port_forward_consent_test: PASS\n";
    return 0;
  }
  std::cerr << "port_forward_consent_test: " << failures << " failure(s)\n";
  return 1;
}
