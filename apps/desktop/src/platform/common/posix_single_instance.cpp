#include "platform/single_instance.h"
#include "platform/machine_identity.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace crossdesk::platform {

SingleInstanceGuard::SingleInstanceGuard(std::string application)
    : application_(std::move(application)) {}

SingleInstanceGuard::~SingleInstanceGuard() { Release(); }

InstanceResult SingleInstanceGuard::TryAcquire(InstanceRole role,
                                               std::string& error) {
  error.clear();
  if (handle_ != -1) return InstanceResult::kAcquired;
  if (!ValidInstanceApplication(application_)) {
    error = "Invalid single-instance application name";
    return InstanceResult::kError;
  }
  // The lease scope is the configuration root: launches sharing it are the
  // same instance, launches with different roots (separate portable copies,
  // XDG/HOME isolation) run side by side. The root enters the file name only
  // as a bounded hex digest, keeping the path short for long roots.
  // O_CLOEXEC prevents unrelated desktop/helper processes retaining it.
  const std::string scope = InstanceScopeHash();
  const std::string path =
      "/tmp/" + application_ + "-" + std::to_string(geteuid()) + "-" +
      InstanceRoleName(role) + (scope.empty() ? "" : "-" + scope) + ".lock";
  const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    error = "Cannot open single-instance lock: " + std::string(std::strerror(errno));
    return InstanceResult::kError;
  }
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid()) {
    close(fd);
    error = "Single-instance lock is not a regular file owned by this user";
    return InstanceResult::kError;
  }
  int result;
  do {
    result = flock(fd, LOCK_EX | LOCK_NB);
  } while (result != 0 && errno == EINTR);
  if (result == 0) {
    handle_ = fd;
    return InstanceResult::kAcquired;
  }
  const int lock_error = errno;
  close(fd);
  if (lock_error == EWOULDBLOCK || lock_error == EAGAIN)
    return InstanceResult::kAlreadyRunning;
  error = "Cannot acquire single-instance lock: " +
          std::string(std::strerror(lock_error));
  return InstanceResult::kError;
}

void SingleInstanceGuard::Release() {
  if (handle_ == -1) return;
  // Close rather than LOCK_UN: a forked headless application may still own the
  // same open file description. Never unlink a lock that another launch opened.
  close(static_cast<int>(handle_));
  handle_ = -1;
}

}  // namespace crossdesk::platform
