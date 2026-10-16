#ifndef __linux__
#error "collector only supports Linux"
#endif

#include "linux_procfs.hpp"
#include "metrics_schema.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#ifndef COLLECTOR_VERSION
#define COLLECTOR_VERSION "0.1.0"
#endif

namespace {
using namespace hpc_metrics;

volatile sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

struct Options {
  std::optional<int> pid;
  std::string pid_file;
  std::chrono::milliseconds interval{500};
  std::chrono::milliseconds duration{20'000};
  std::string output;
  bool overwrite = false;
};

class OutputFile {
 public:
  OutputFile(const std::string& path, bool overwrite) {
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (overwrite) {
      struct stat before {};
      if (lstat(path.c_str(), &before) == 0) {
        if (!S_ISREG(before.st_mode))
          throw std::runtime_error("--overwrite only accepts a regular file");
      } else if (errno != ENOENT) {
        throw std::runtime_error("cannot inspect output: " +
                                 std::string(std::strerror(errno)));
      }
      flags |= O_TRUNC;
    } else {
      flags |= O_EXCL;
    }
    descriptor_ = open(path.c_str(), flags, 0600);
    if (descriptor_ < 0)
      throw std::runtime_error("cannot open output: " +
                               std::string(std::strerror(errno)));
    struct stat opened {};
    if (fstat(descriptor_, &opened) != 0 || !S_ISREG(opened.st_mode)) {
      close(descriptor_);
      descriptor_ = -1;
      throw std::runtime_error("output is not a regular file");
    }
  }

  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;

  ~OutputFile() {
    if (descriptor_ >= 0) close(descriptor_);
  }

  void line(const std::string& value) {
    if (value.size() + 1 > kMaxLineBytes)
      throw std::runtime_error("JSONL line exceeds 16 KiB");
    if (bytes_ + value.size() + 1 > kMaxFileBytes)
      throw std::runtime_error("JSONL output exceeds 4 MiB");
    write_all(value.data(), value.size());
    static constexpr char newline = '\n';
    write_all(&newline, 1);
    bytes_ += value.size() + 1;
  }

  void sync() {
    if (fsync(descriptor_) != 0)
      throw std::runtime_error("fsync failed: " +
                               std::string(std::strerror(errno)));
  }

 private:
  void write_all(const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const auto written = write(descriptor_, data + offset, size - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0)
        throw std::runtime_error("write failed: " +
                                 std::string(std::strerror(errno)));
      offset += static_cast<std::size_t>(written);
    }
  }

  int descriptor_ = -1;
  std::size_t bytes_ = 0;
};

std::chrono::milliseconds parse_duration(const std::string& text) {
  std::size_t suffix = 0;
  while (suffix < text.size() && text[suffix] >= '0' && text[suffix] <= '9')
    ++suffix;
  if (suffix == 0 || suffix == text.size())
    throw std::invalid_argument("duration must use an integer ms, s, or m suffix");
  std::uint64_t number = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + suffix, number);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + suffix)
    throw std::invalid_argument("invalid duration");
  const auto unit = text.substr(suffix);
  std::uint64_t multiplier = 0;
  if (unit == "ms") multiplier = 1;
  else if (unit == "s") multiplier = 1000;
  else if (unit == "m") multiplier = 60'000;
  else throw std::invalid_argument("duration suffix must be ms, s, or m");
  if (number > std::numeric_limits<std::uint64_t>::max() / multiplier)
    throw std::invalid_argument("duration is too large");
  return std::chrono::milliseconds(number * multiplier);
}

int parse_pid(const std::string& text) {
  int value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < 1 || value > 4'194'304)
    throw std::invalid_argument("PID must be in 1..4194304");
  return value;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto value = [&](const char* name) -> std::string {
      if (++i >= argc) throw std::invalid_argument(std::string(name) + " needs a value");
      return argv[i];
    };
    if (argument == "--pid") options.pid = parse_pid(value("--pid"));
    else if (argument == "--pid-file") options.pid_file = value("--pid-file");
    else if (argument == "--interval") options.interval = parse_duration(value("--interval"));
    else if (argument == "--duration") options.duration = parse_duration(value("--duration"));
    else if (argument == "--output") options.output = value("--output");
    else if (argument == "--overwrite") options.overwrite = true;
    else if (argument == "--help" || argument == "-h") {
      std::cout << "usage: collector (--pid PID | --pid-file PATH) "
                   "[--interval DURATION] [--duration DURATION] "
                   "--output PATH [--overwrite]\n";
      std::exit(0);
    } else throw std::invalid_argument("unknown argument: " + argument);
  }
  if (options.pid.has_value() == !options.pid_file.empty())
    throw std::invalid_argument("choose exactly one of --pid and --pid-file");
  if (options.output.empty()) throw std::invalid_argument("--output is required");
  if (options.interval < std::chrono::milliseconds(100) ||
      options.interval > std::chrono::seconds(10))
    throw std::invalid_argument("--interval must be in 100ms..10s");
  if (options.duration < std::chrono::seconds(1) ||
      options.duration > std::chrono::minutes(10))
    throw std::invalid_argument("--duration must be in 1s..10m");
  return options;
}

std::string read_pid_file(const std::string& path) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    throw ProcError(errno == EACCES || errno == EPERM
                        ? ProcErrorKind::permission_denied
                        : ProcErrorKind::not_found,
                    "cannot open PID file: " + std::string(std::strerror(errno)));
  char buffer[64]{};
  const auto size = read(descriptor, buffer, sizeof(buffer));
  const int read_error = errno;
  close(descriptor);
  if (size < 0)
    throw ProcError(ProcErrorKind::io,
                    "cannot read PID file: " + std::string(std::strerror(read_error)));
  if (size == 0 || size == static_cast<ssize_t>(sizeof(buffer)))
    throw ProcError(ProcErrorKind::malformed, "invalid PID file length");
  std::string text(buffer, static_cast<std::size_t>(size));
  if (!text.empty() && text.back() == '\n') text.pop_back();
  if (!text.empty() && text.back() == '\r')
    throw ProcError(ProcErrorKind::malformed, "PID file must use LF, not CRLF");
  if (text.find('\n') != std::string::npos)
    throw ProcError(ProcErrorKind::malformed, "PID file must contain one line");
  return text;
}

std::uint64_t monotonic_ns() {
  timespec now {};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
  return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(now.tv_nsec);
}

std::string wall_time() {
  timespec now {};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    throw std::runtime_error("clock_gettime(CLOCK_REALTIME) failed");
  std::tm utc {};
  if (gmtime_r(&now.tv_sec, &utc) == nullptr)
    throw std::runtime_error("gmtime_r failed");
  char date[32]{};
  if (std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
    throw std::runtime_error("strftime failed");
  std::ostringstream out;
  out << date << '.' << std::setw(3) << std::setfill('0')
      << now.tv_nsec / 1'000'000 << 'Z';
  return out.str();
}

bool sleep_until(std::uint64_t deadline_ns) {
  timespec deadline{static_cast<time_t>(deadline_ns / 1'000'000'000ULL),
                    static_cast<long>(deadline_ns % 1'000'000'000ULL)};
  while (!interrupted) {
    const int result =
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    if (result == 0) return true;
    if (result != EINTR) throw std::runtime_error("clock_nanosleep failed");
  }
  return false;
}

void write_string_array(std::ostringstream& out,
                        const std::vector<std::string>& values) {
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ',';
    out << json_string(values[i]);
  }
  out << ']';
}

std::optional<std::uint64_t> delta(
    const std::optional<std::uint64_t>& current,
    const std::optional<std::uint64_t>& previous) {
  if (!current || !previous || *current < *previous) return std::nullopt;
  return *current - *previous;
}

std::string start_json(std::uint64_t sequence, std::uint64_t now_ns,
                       const Options& options, const TargetIdentity& target,
                       const HostIdentity& host) {
  std::ostringstream out;
  out << "{\"schema\":\"" << kSchema
      << "\",\"event\":\"start\",\"sequence\":" << sequence
      << ",\"wall_time\":" << json_string(wall_time())
      << ",\"monotonic_ns\":" << now_ns
      << ",\"collector\":{\"version\":" << json_string(COLLECTOR_VERSION)
      << ",\"pid\":" << getpid()
      << ",\"interval_ms\":" << options.interval.count()
      << ",\"requested_duration_ms\":" << options.duration.count()
      << "},\"target\":{\"pid\":" << target.pid
      << ",\"start_ticks\":" << target.start_ticks
      << ",\"command\":" << json_string(target.command)
      << ",\"uid\":" << target.uid
      << "},\"host\":{\"boot_id\":" << json_string(host.boot_id)
      << ",\"hostname\":" << json_string(host.hostname)
      << ",\"logical_cpus\":" << host.logical_cpus
      << ",\"clock_ticks_per_second\":" << host.clock_ticks_per_second
      << ",\"page_size_bytes\":" << host.page_size_bytes
      << "},\"unavailable\":[]}";
  return out.str();
}

std::string sample_json(std::uint64_t sequence, std::uint64_t now_ns,
                        std::uint64_t started_ns,
                        const TargetIdentity& target,
                        const HostIdentity& identity,
                        const ProcessSnapshot& process,
                        const ProcessSnapshot& previous_process,
                        const HostSnapshot& host,
                        const HostSnapshot& previous_host,
                        std::uint64_t previous_ns) {
  const double seconds = static_cast<double>(now_ns - previous_ns) / 1e9;
  std::optional<double> core_percent;
  if (seconds > 0 && process.cpu_ticks >= previous_process.cpu_ticks) {
    core_percent = 100.0 *
                   static_cast<double>(process.cpu_ticks - previous_process.cpu_ticks) /
                   static_cast<double>(identity.clock_ticks_per_second) / seconds;
  }
  std::optional<double> capacity_percent;
  if (core_percent)
    capacity_percent = std::clamp(*core_percent /
                                      static_cast<double>(identity.logical_cpus),
                                  0.0, 100.0);
  std::optional<double> host_percent;
  if (host.cpu_total_ticks > previous_host.cpu_total_ticks &&
      host.cpu_idle_ticks >= previous_host.cpu_idle_ticks) {
    const auto total = host.cpu_total_ticks - previous_host.cpu_total_ticks;
    const auto idle = host.cpu_idle_ticks - previous_host.cpu_idle_ticks;
    host_percent = std::clamp(100.0 * static_cast<double>(total - std::min(total, idle)) /
                                  static_cast<double>(total),
                              0.0, 100.0);
  }
  auto unavailable = optional_unavailable(process, host);
  if (!core_percent) unavailable.emplace_back("process.cpu_cores_percent");
  if (!capacity_percent) unavailable.emplace_back("process.cpu_capacity_percent");
  if (!host_percent) unavailable.emplace_back("host.cpu_utilization_percent");
  if (!delta(process.voluntary_context_switches,
             previous_process.voluntary_context_switches))
    unavailable.emplace_back("process.voluntary_context_switches_delta");
  if (!delta(process.involuntary_context_switches,
             previous_process.involuntary_context_switches))
    unavailable.emplace_back("process.involuntary_context_switches_delta");
  std::sort(unavailable.begin(), unavailable.end());
  unavailable.erase(std::unique(unavailable.begin(), unavailable.end()),
                    unavailable.end());

  std::ostringstream out;
  out << "{\"schema\":\"" << kSchema
      << "\",\"event\":\"sample\",\"sequence\":" << sequence
      << ",\"wall_time\":" << json_string(wall_time())
      << ",\"monotonic_ns\":" << now_ns
      << ",\"elapsed_ms\":" << (now_ns - started_ns) / 1'000'000ULL
      << ",\"target\":{\"pid\":" << target.pid
      << ",\"start_ticks\":" << target.start_ticks
      << "},\"process\":{\"cpu_cores_percent\":";
  write_optional_decimal(out, core_percent);
  out << ",\"cpu_capacity_percent\":";
  write_optional_decimal(out, capacity_percent);
  out << ",\"threads\":" << process.threads
      << ",\"rss_bytes\":" << process.rss_bytes
      << ",\"read_bytes\":";
  write_optional_number(out, process.read_bytes);
  out << ",\"write_bytes\":";
  write_optional_number(out, process.write_bytes);
  out << ",\"voluntary_context_switches_delta\":";
  write_optional_number(out, delta(process.voluntary_context_switches,
                                   previous_process.voluntary_context_switches));
  out << ",\"involuntary_context_switches_delta\":";
  write_optional_number(out, delta(process.involuntary_context_switches,
                                   previous_process.involuntary_context_switches));
  out << "},\"host\":{\"cpu_utilization_percent\":";
  write_optional_decimal(out, host_percent);
  out << ",\"load1\":";
  write_optional_decimal(out, host.load1, 2);
  out << ",\"memory_available_bytes\":";
  write_optional_number(out, host.memory_available_bytes);
  out << "},\"unavailable\":";
  write_string_array(out, unavailable);
  out << '}';
  return out.str();
}

std::string end_json(std::uint64_t sequence, std::uint64_t now_ns,
                     std::uint64_t started_ns, const std::string& reason,
                     std::uint64_t samples, std::uint64_t missed,
                     const std::vector<std::string>& unavailable) {
  std::ostringstream out;
  out << "{\"schema\":\"" << kSchema
      << "\",\"event\":\"end\",\"sequence\":" << sequence
      << ",\"wall_time\":" << json_string(wall_time())
      << ",\"monotonic_ns\":" << now_ns
      << ",\"elapsed_ms\":" << (now_ns - started_ns) / 1'000'000ULL
      << ",\"reason\":" << json_string(reason)
      << ",\"samples_written\":" << samples
      << ",\"missed_deadlines\":" << missed
      << ",\"unavailable_fields\":";
  write_string_array(out, unavailable);
  out << '}';
  return out.str();
}

int proc_exit_code(const ProcError& error) {
  return error.kind() == ProcErrorKind::permission_denied ? 4 : 3;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    options = parse_options(argc, argv);
    if (!options.pid)
      options.pid = parse_pid(read_pid_file(options.pid_file));
  } catch (const ProcError& error) {
    std::cerr << "collector: " << error.what() << '\n';
    return proc_exit_code(error);
  } catch (const std::exception& error) {
    std::cerr << "collector: " << error.what() << '\n';
    return 2;
  }

  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    std::cerr << "collector: cannot install signal handlers\n";
    return 7;
  }

  try {
    const auto host_identity = read_host_identity();
    const auto target = read_target_identity(*options.pid,
                                             host_identity.page_size_bytes);
    auto previous_process = read_process_snapshot(
        target.pid, host_identity.page_size_bytes);
    auto previous_host = read_host_snapshot();
    if (previous_process.start_ticks != target.start_ticks)
      throw ProcError(ProcErrorKind::not_found,
                      "target identity changed before sampling");

    OutputFile output(options.output, options.overwrite);
    const auto started_ns = monotonic_ns();
    auto previous_ns = started_ns;
    std::uint64_t sequence = 0;
    std::uint64_t samples = 0;
    std::uint64_t missed = 0;
    std::vector<std::string> unavailable_fields;
    output.line(start_json(sequence++, started_ns, options, target,
                           host_identity));

    const auto interval_ns =
        static_cast<std::uint64_t>(options.interval.count()) * 1'000'000ULL;
    const auto duration_ns =
        static_cast<std::uint64_t>(options.duration.count()) * 1'000'000ULL;
    const auto finish_ns = started_ns + duration_ns;
    auto deadline_ns = started_ns + interval_ns;
    std::string reason = "duration_reached";

    while (deadline_ns <= finish_ns) {
      if (!sleep_until(deadline_ns)) {
        reason = "interrupted";
        break;
      }
      const auto now_ns = monotonic_ns();
      if (now_ns > deadline_ns + interval_ns) {
        const auto skipped = (now_ns - deadline_ns) / interval_ns;
        missed += skipped;
        deadline_ns += skipped * interval_ns;
        if (deadline_ns > finish_ns) break;
      }

      ProcessSnapshot process;
      HostSnapshot host;
      try {
        process = read_process_snapshot(target.pid,
                                        host_identity.page_size_bytes);
        if (process.start_ticks != target.start_ticks) {
          reason = "target_exited";
          break;
        }
        host = read_host_snapshot();
      } catch (const ProcError& error) {
        if (error.kind() == ProcErrorKind::not_found) {
          reason = "target_exited";
          break;
        }
        throw;
      }
      output.line(sample_json(sequence++, now_ns, started_ns, target,
                              host_identity, process, previous_process, host,
                              previous_host, previous_ns));
      ++samples;
      const auto missing = optional_unavailable(process, host);
      for (const auto& field : missing)
        if (std::find(unavailable_fields.begin(), unavailable_fields.end(), field) ==
            unavailable_fields.end())
          unavailable_fields.push_back(field);
      previous_process = process;
      previous_host = host;
      previous_ns = now_ns;
      deadline_ns += interval_ns;
    }

    if (reason == "duration_reached" && !interrupted) {
      const auto now_ns = monotonic_ns();
      if (now_ns < finish_ns && !sleep_until(finish_ns)) reason = "interrupted";
      if (reason == "duration_reached") {
        try {
          const auto final_process = read_process_snapshot(
              target.pid, host_identity.page_size_bytes);
          if (final_process.start_ticks != target.start_ticks)
            reason = "target_exited";
        } catch (const ProcError& error) {
          if (error.kind() == ProcErrorKind::not_found) reason = "target_exited";
          else throw;
        }
      }
    }

    if (interrupted) reason = "interrupted";
    auto ended_ns = monotonic_ns();
    if (ended_ns <= previous_ns) ended_ns = previous_ns + 1;
    output.line(end_json(sequence, ended_ns, started_ns, reason, samples,
                         missed, unavailable_fields));
    output.sync();
    if (reason == "duration_reached") return 0;
    if (reason == "interrupted") return 6;
    return 3;
  } catch (const ProcError& error) {
    std::cerr << "collector: " << error.what() << '\n';
    return proc_exit_code(error);
  } catch (const std::runtime_error& error) {
    std::cerr << "collector: " << error.what() << '\n';
    return 5;
  } catch (const std::exception& error) {
    std::cerr << "collector: " << error.what() << '\n';
    return 7;
  }
}
