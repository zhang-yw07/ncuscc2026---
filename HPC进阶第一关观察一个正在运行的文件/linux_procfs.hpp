#pragma once

#ifndef __linux__
#error "The stage1 metrics collector only supports Linux"
#endif

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hpc_metrics {

enum class ProcErrorKind { not_found, permission_denied, malformed, io };

class ProcError : public std::runtime_error {
 public:
  ProcError(ProcErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}
  ProcErrorKind kind() const noexcept { return kind_; }

 private:
  ProcErrorKind kind_;
};

struct ProcessSnapshot {
  std::uint64_t start_ticks = 0;
  std::uint64_t cpu_ticks = 0;
  std::uint64_t threads = 0;
  std::uint64_t rss_bytes = 0;
  std::optional<std::uint64_t> read_bytes;
  std::optional<std::uint64_t> write_bytes;
  std::optional<std::uint64_t> voluntary_context_switches;
  std::optional<std::uint64_t> involuntary_context_switches;
};

struct HostSnapshot {
  std::uint64_t cpu_total_ticks = 0;
  std::uint64_t cpu_idle_ticks = 0;
  std::optional<double> load1;
  std::optional<std::uint64_t> memory_available_bytes;
};

struct TargetIdentity {
  int pid = 0;
  std::uint64_t start_ticks = 0;
  std::string command;
  std::uint64_t uid = 0;
};

struct HostIdentity {
  std::string boot_id;
  std::string hostname;
  std::uint64_t logical_cpus = 0;
  std::uint64_t clock_ticks_per_second = 0;
  std::uint64_t page_size_bytes = 0;
};

ProcessSnapshot read_process_snapshot(int pid, std::uint64_t page_size);
HostSnapshot read_host_snapshot();
TargetIdentity read_target_identity(int pid, std::uint64_t page_size);
HostIdentity read_host_identity();
std::vector<std::string> optional_unavailable(
    const ProcessSnapshot& process, const HostSnapshot& host);

}  // namespace hpc_metrics
