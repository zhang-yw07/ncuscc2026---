#include "linux_procfs.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

namespace hpc_metrics {
namespace {

std::string proc_path(int pid, const char* file) {
  return "/proc/" + std::to_string(pid) + "/" + file;
}

ProcErrorKind errno_kind(int error) {
  if (error == ENOENT || error == ESRCH) return ProcErrorKind::not_found;
  if (error == EACCES || error == EPERM) return ProcErrorKind::permission_denied;
  return ProcErrorKind::io;
}

std::string read_file(const std::string& path, bool binary = false) {
  errno = 0;
  std::ifstream input(path, binary ? std::ios::binary : std::ios::in);
  if (!input) {
    const int error = errno;
    throw ProcError(errno_kind(error), "cannot open " + path + ": " +
                                              std::strerror(error));
  }
  std::ostringstream content;
  content << input.rdbuf();
  if (input.bad()) {
    const int error = errno;
    throw ProcError(errno_kind(error), "cannot read " + path + ": " +
                                              std::strerror(error));
  }
  return content.str();
}

std::optional<std::string> read_optional_file(const std::string& path) {
  try {
    return read_file(path);
  } catch (const ProcError& error) {
    if (error.kind() == ProcErrorKind::not_found ||
        error.kind() == ProcErrorKind::permission_denied) {
      return std::nullopt;
    }
    throw;
  }
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

template <typename T>
T parse_unsigned(std::string_view text, const std::string& field) {
  T value{};
  const char* first = text.data();
  const char* last = first + text.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    throw ProcError(ProcErrorKind::malformed, "invalid " + field);
  }
  return value;
}

std::vector<std::string_view> split_words(std::string_view input) {
  std::vector<std::string_view> words;
  std::size_t position = 0;
  while (position < input.size()) {
    position = input.find_first_not_of(" \t\r\n", position);
    if (position == std::string_view::npos) break;
    const auto end = input.find_first_of(" \t\r\n", position);
    words.push_back(input.substr(position, end - position));
    if (end == std::string_view::npos) break;
    position = end;
  }
  return words;
}

struct StatFields {
  std::uint64_t cpu_ticks = 0;
  std::uint64_t start_ticks = 0;
  char state = '?';
};

StatFields parse_process_stat(const std::string& content) {
  const auto close = content.rfind(')');
  if (close == std::string::npos || close + 2 >= content.size()) {
    throw ProcError(ProcErrorKind::malformed, "invalid /proc/<pid>/stat");
  }
  // Words begin at field 3 (state). utime=14, stime=15, starttime=22.
  const auto words = split_words(std::string_view(content).substr(close + 2));
  if (words.size() <= 19) {
    throw ProcError(ProcErrorKind::malformed, "short /proc/<pid>/stat");
  }
  const auto user = parse_unsigned<std::uint64_t>(words[11], "utime");
  const auto system = parse_unsigned<std::uint64_t>(words[12], "stime");
  if (words[0].size() != 1) {
    throw ProcError(ProcErrorKind::malformed, "invalid process state");
  }
  return {user + system,
          parse_unsigned<std::uint64_t>(words[19], "starttime"), words[0][0]};
}

std::optional<std::string_view> status_value(const std::string& content,
                                             std::string_view key) {
  std::size_t begin = 0;
  while (begin < content.size()) {
    const auto end = content.find('\n', begin);
    const auto line = std::string_view(content).substr(
        begin, (end == std::string::npos ? content.size() : end) - begin);
    if (line.substr(0, key.size()) == key) {
      const auto value = line.find_first_not_of(" \t", key.size());
      if (value != std::string_view::npos) return line.substr(value);
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> optional_status_number(
    const std::string& content, std::string_view key) {
  const auto value = status_value(content, key);
  if (!value) return std::nullopt;
  const auto words = split_words(*value);
  if (words.empty()) return std::nullopt;
  try {
    return parse_unsigned<std::uint64_t>(words[0], std::string(key));
  } catch (const ProcError&) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> io_value(const std::optional<std::string>& content,
                                      std::string_view key) {
  if (!content) return std::nullopt;
  return optional_status_number(*content, key);
}

std::string first_command_argument(int pid) {
  auto content = read_file(proc_path(pid, "cmdline"), true);
  const auto nul = content.find('\0');
  if (nul != std::string::npos) content.resize(nul);
  if (content.size() > 256) content.resize(256);
  // Replace control bytes and malformed high bytes with a harmless marker. The
  // command is display-only and must not make the JSON stream invalid UTF-8.
  for (char& ch : content) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte >= 0x80) ch = '?';
  }
  return content;
}

}  // namespace

ProcessSnapshot read_process_snapshot(int pid, std::uint64_t page_size) {
  const auto stat = parse_process_stat(read_file(proc_path(pid, "stat")));
  if (stat.state == 'Z' || stat.state == 'X')
    throw ProcError(ProcErrorKind::not_found, "target process has exited");
  const auto status = read_file(proc_path(pid, "status"));
  const auto thread_value = status_value(status, "Threads:");
  if (!thread_value) {
    throw ProcError(ProcErrorKind::malformed, "Threads missing from status");
  }
  const auto thread_words = split_words(*thread_value);
  if (thread_words.empty()) {
    throw ProcError(ProcErrorKind::malformed, "invalid Threads in status");
  }

  const auto statm = read_file(proc_path(pid, "statm"));
  const auto statm_words = split_words(statm);
  if (statm_words.size() < 2) {
    throw ProcError(ProcErrorKind::malformed, "invalid /proc/<pid>/statm");
  }
  const auto resident_pages =
      parse_unsigned<std::uint64_t>(statm_words[1], "resident pages");
  if (resident_pages > UINT64_MAX / page_size) {
    throw ProcError(ProcErrorKind::malformed, "resident memory overflow");
  }

  const auto io = read_optional_file(proc_path(pid, "io"));
  ProcessSnapshot result;
  result.start_ticks = stat.start_ticks;
  result.cpu_ticks = stat.cpu_ticks;
  result.threads =
      parse_unsigned<std::uint64_t>(thread_words[0], "Threads");
  result.rss_bytes = resident_pages * page_size;
  result.read_bytes = io_value(io, "read_bytes:");
  result.write_bytes = io_value(io, "write_bytes:");
  result.voluntary_context_switches =
      optional_status_number(status, "voluntary_ctxt_switches:");
  result.involuntary_context_switches =
      optional_status_number(status, "nonvoluntary_ctxt_switches:");
  return result;
}

HostSnapshot read_host_snapshot() {
  std::istringstream stat(read_file("/proc/stat"));
  std::string label;
  stat >> label;
  if (label != "cpu") {
    throw ProcError(ProcErrorKind::malformed, "aggregate cpu line missing");
  }
  std::vector<std::uint64_t> ticks;
  std::uint64_t value = 0;
  while (stat >> value) ticks.push_back(value);
  if (ticks.size() < 5) {
    throw ProcError(ProcErrorKind::malformed, "short aggregate cpu line");
  }
  HostSnapshot result;
  // guest and guest_nice are already included in user and nice. Summing them
  // again would inflate the denominator and distort host utilization.
  for (std::size_t index = 0; index < std::min<std::size_t>(ticks.size(), 8);
       ++index)
    result.cpu_total_ticks += ticks[index];
  result.cpu_idle_ticks = ticks[3] + ticks[4];  // idle + iowait

  try {
    std::istringstream load(read_file("/proc/loadavg"));
    double load1 = 0;
    if (load >> load1) result.load1 = load1;
  } catch (const ProcError&) {
    result.load1 = std::nullopt;
  }

  try {
    const auto memory = read_file("/proc/meminfo");
    const auto available = status_value(memory, "MemAvailable:");
    if (available) {
      const auto words = split_words(*available);
      if (!words.empty()) {
        const auto kib = parse_unsigned<std::uint64_t>(words[0], "MemAvailable");
        if (kib <= UINT64_MAX / 1024) result.memory_available_bytes = kib * 1024;
      }
    }
  } catch (const ProcError&) {
    result.memory_available_bytes = std::nullopt;
  }
  return result;
}

TargetIdentity read_target_identity(int pid, std::uint64_t page_size) {
  const auto snapshot = read_process_snapshot(pid, page_size);
  const auto status = read_file(proc_path(pid, "status"));
  const auto uid_value = status_value(status, "Uid:");
  if (!uid_value) throw ProcError(ProcErrorKind::malformed, "Uid missing");
  const auto uid_words = split_words(*uid_value);
  if (uid_words.empty()) throw ProcError(ProcErrorKind::malformed, "invalid Uid");
  return {pid, snapshot.start_ticks, first_command_argument(pid),
          parse_unsigned<std::uint64_t>(uid_words[0], "Uid")};
}

HostIdentity read_host_identity() {
  const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  const long ticks = sysconf(_SC_CLK_TCK);
  const long page = sysconf(_SC_PAGESIZE);
  if (cpus <= 0 || ticks <= 0 || page <= 0) {
    throw ProcError(ProcErrorKind::io, "sysconf returned an invalid value");
  }
  char hostname[HOST_NAME_MAX + 1]{};
  if (gethostname(hostname, HOST_NAME_MAX) != 0) {
    throw ProcError(errno_kind(errno), "gethostname failed");
  }
  hostname[HOST_NAME_MAX] = '\0';
  return {trim(read_file("/proc/sys/kernel/random/boot_id")), hostname,
          static_cast<std::uint64_t>(cpus), static_cast<std::uint64_t>(ticks),
          static_cast<std::uint64_t>(page)};
}

std::vector<std::string> optional_unavailable(
    const ProcessSnapshot& process, const HostSnapshot& host) {
  std::vector<std::string> missing;
  if (!process.read_bytes) missing.emplace_back("process.read_bytes");
  if (!process.write_bytes) missing.emplace_back("process.write_bytes");
  if (!process.voluntary_context_switches)
    missing.emplace_back("process.voluntary_context_switches_delta");
  if (!process.involuntary_context_switches)
    missing.emplace_back("process.involuntary_context_switches_delta");
  if (!host.load1) missing.emplace_back("host.load1");
  if (!host.memory_available_bytes)
    missing.emplace_back("host.memory_available_bytes");
  return missing;
}

}  // namespace hpc_metrics
