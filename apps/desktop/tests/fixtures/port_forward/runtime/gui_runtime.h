// Hermetic test fixture: a controllable stand-in for the GuiRuntime owner
// surface that PortForwardController consumes. This is NOT a copy of the
// admission logic — every admission decision in the tests lives in the real
// controller; this fixture only supplies environmental facts (which sessions
// exist, which are encrypted, which keys verified) and records sent frames.
// Contract enforcement is automatic: any signature drift in the real owner
// surface breaks compilation of the real controller against this fixture.
#ifndef _GUI_RUNTIME_H_
#define _GUI_RUNTIME_H_
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace crossdesk {
class GuiRuntime {
 public:
  std::set<std::string> authorized, secure, client_active, initiator;
  std::map<std::string, std::set<std::uint16_t>> allowlists;
  std::map<std::string, std::map<std::uint16_t, std::uint16_t>> ini_rules;
  std::vector<std::string> attachable;
  std::vector<std::string> sent;
  int SendPortForwardData(const std::string&, const char* data, std::size_t size) {
    sent.emplace_back(data, size); return 0;
  }
  // Mock of the verified-identity consent key: tests populate exactly the
  // connections that completed a trusted-device proof with a pinned key.
  std::map<std::string, std::string> verified_keys;
  std::string VerifiedPortForwardConsentKey(const std::string& id) {
    const auto f = verified_keys.find(id);
    return f == verified_keys.end() ? std::string{} : f->second;
  }
  bool PortForwardSessionSecure(const std::string& id) { return secure.count(id) != 0; }
  bool PortForwardClientActive(const std::string& id) { return client_active.count(id) != 0; }
  // Faithful model of the redesigned runtime resolution: CLI session consent
  // unioned with persistent consent keyed ONLY by the verified trust key.
  // Entries keyed any other way are unreachable by construction — exactly
  // like the real ConfigCenter after the trusted/ prefix enforcement.
  std::set<std::uint16_t> cli_allow;
  std::set<std::uint16_t> HostPortForwardAllowlist(const std::string& id) {
    std::set<std::uint16_t> out = cli_allow;
    const auto key = VerifiedPortForwardConsentKey(id);
    if (!key.empty()) {
      const auto f = allowlists.find(key);
      if (f != allowlists.end()) out.insert(f->second.begin(), f->second.end());
    }
    return out;
  }
  bool IsAuthorizedController(const std::string& id) { return authorized.count(id) != 0; }
  bool PortForwardConnectionInitiator(const std::string& id) { return initiator.count(id) != 0; }
  std::string PortForwardDeviceKey(const std::string& id) { return id; }
  std::map<std::uint16_t, std::uint16_t> ConfiguredPortForwardRules(const std::string& key) {
    const auto f = ini_rules.find(key);
    return f == ini_rules.end() ? std::map<std::uint16_t, std::uint16_t>{} : f->second;
  }
  std::vector<std::string> PortForwardAttachableSessions() { return attachable; }
  std::vector<std::string> reverse_candidates;
  std::vector<std::string> PortForwardReverseCandidates() { return reverse_candidates; }
};
}
#endif
