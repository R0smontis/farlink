// Hermetic test fixture mirroring the LaunchOptions shape the controller
// consumes. Drift in the real launch_options.h breaks the fixture build.
#ifndef _LAUNCH_OPTIONS_H_
#define _LAUNCH_OPTIONS_H_
#include <set>
#include <vector>
#include <cstdint>
namespace crossdesk {
struct LaunchOptions {
  struct PortForwardRule { unsigned local_port = 0; unsigned remote_port = 0; };
  std::vector<PortForwardRule> port_forward_rules;
  std::vector<PortForwardRule> port_forward_reverse_rules;
  std::set<std::uint16_t> port_forward_allow;
};
inline LaunchOptions& MutableLaunchOptions() { static LaunchOptions o; return o; }
inline const LaunchOptions& GetLaunchOptions() { return MutableLaunchOptions(); }
}
#endif
