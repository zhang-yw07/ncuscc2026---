#ifndef __linux__
#error "workload only supports Linux"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t stop_requested = 0;
std::atomic<std::uint64_t> result_sink{0};

void on_signal(int) { stop_requested = 1; }

struct Options {
  int threads = 0;
  std::chrono::milliseconds duration{0};
  int load_percent = 100;
  std::string pid_file;
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

int parse_int(const std::string& text, const char* name) {
  int value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument(std::string("invalid ") + name);
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
    if (argument == "--mode") {
      if (value("--mode") != "cpu")
        throw std::invalid_argument("only --mode cpu is supported");
    } else if (argument == "--threads") {
      options.threads = parse_int(value("--threads"), "thread count");
    } else if (argument == "--duration") {
      options.duration = parse_duration(value("--duration"));
    } else if (argument == "--load-percent") {
      options.load_percent = parse_int(value("--load-percent"), "load percent");
    } else if (argument == "--pid-file") {
      options.pid_file = value("--pid-file");
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "usage: workload [--mode cpu] --threads N --duration DURATION "
                   "[--pid-file PATH] [--load-percent 1..100]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  if (options.threads < 1 || options.threads > 256)
    throw std::invalid_argument("--threads must be in 1..256");
  if (options.duration < std::chrono::seconds(1) ||
      options.duration > std::chrono::minutes(10))
    throw std::invalid_argument("--duration must be in 1s..10m");
  if (options.load_percent < 1 || options.load_percent > 100)
    throw std::invalid_argument("--load-percent must be in 1..100");
  return options;
}

void write_all(int descriptor, const std::string& value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto written = write(descriptor, value.data() + offset, value.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error(std::strerror(errno));
    offset += static_cast<std::size_t>(written);
  }
}

std::optional<pid_t> read_existing_pid(const std::string& path) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) return std::nullopt;
    throw std::runtime_error("cannot inspect PID file: " +
                             std::string(std::strerror(errno)));
  }
  char buffer[64]{};
  const auto size = read(descriptor, buffer, sizeof(buffer) - 1);
  const int read_error = errno;
  close(descriptor);
  if (size < 0) throw std::runtime_error("cannot read PID file: " +
                                         std::string(std::strerror(read_error)));
  std::string text(buffer, static_cast<std::size_t>(size));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  int pid = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || pid < 1)
    return std::nullopt;
  return static_cast<pid_t>(pid);
}

void create_pid_file(const std::string& path) {
  struct stat status {};
  if (lstat(path.c_str(), &status) == 0) {
    if (S_ISLNK(status.st_mode)) throw std::runtime_error("PID file is a symbolic link");
    const auto existing = read_existing_pid(path);
    if (existing && (kill(*existing, 0) == 0 || errno == EPERM))
      throw std::runtime_error("PID file refers to a live process");
  } else if (errno != ENOENT) {
    throw std::runtime_error("cannot inspect PID file: " +
                             std::string(std::strerror(errno)));
  }

  const std::string temporary = path + ".tmp." + std::to_string(getpid());
  const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                                     O_CLOEXEC | O_NOFOLLOW,
                              0600);
  if (descriptor < 0)
    throw std::runtime_error("cannot create temporary PID file: " +
                             std::string(std::strerror(errno)));
  try {
    write_all(descriptor, std::to_string(getpid()) + "\n");
    if (fsync(descriptor) != 0)
      throw std::runtime_error("cannot sync PID file: " +
                               std::string(std::strerror(errno)));
    if (close(descriptor) != 0)
      throw std::runtime_error("cannot close PID file: " +
                               std::string(std::strerror(errno)));
    if (rename(temporary.c_str(), path.c_str()) != 0)
      throw std::runtime_error("cannot install PID file: " +
                               std::string(std::strerror(errno)));
  } catch (...) {
    close(descriptor);
    unlink(temporary.c_str());
    throw;
  }
}

void remove_own_pid_file(const std::string& path) {
  if (path.empty()) return;
  try {
    const auto pid = read_existing_pid(path);
    if (pid && *pid == getpid()) unlink(path.c_str());
  } catch (...) {
    // Cleanup is best-effort; never remove a file whose content was not verified.
  }
}

void sleep_until(std::chrono::steady_clock::time_point deadline) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      deadline.time_since_epoch()).count();
  timespec time{static_cast<time_t>(ns / 1'000'000'000),
                static_cast<long>(ns % 1'000'000'000)};
  while (!stop_requested) {
    const int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, nullptr);
    if (result == 0) return;
    if (result != EINTR) throw std::runtime_error("clock_nanosleep failed");
  }
}

void run_worker(int worker_id, std::chrono::steady_clock::time_point end,
                int load_percent) {
  constexpr auto period = std::chrono::milliseconds(100);
  const auto busy = period * load_percent / 100;
  auto cycle = std::chrono::steady_clock::now();
  std::uint64_t value = 0x9e3779b97f4a7c15ULL ^
                        static_cast<std::uint64_t>(worker_id + 1);
  while (!stop_requested && cycle < end) {
    const auto busy_end = std::min(cycle + busy, end);
    while (!stop_requested &&
           std::chrono::steady_clock::now() < busy_end) {
      // Integer mixing has observable output and cannot be optimized away.
      value ^= value >> 12;
      value ^= value << 25;
      value ^= value >> 27;
      value *= 2685821657736338717ULL;
    }
    result_sink.fetch_xor(value, std::memory_order_relaxed);
    cycle += period;
    if (cycle < end) sleep_until(cycle);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    options = parse_options(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "workload: " << error.what() << '\n';
    return 2;
  }

  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    std::cerr << "workload: cannot install signal handlers\n";
    return 7;
  }

  try {
    if (!options.pid_file.empty()) create_pid_file(options.pid_file);
  } catch (const std::exception& error) {
    std::cerr << "workload: " << error.what() << '\n';
    return 5;
  }

  const auto end = std::chrono::steady_clock::now() + options.duration;
  std::vector<std::thread> workers;
  try {
    workers.reserve(static_cast<std::size_t>(options.threads));
    for (int index = 0; index < options.threads; ++index)
      workers.emplace_back(run_worker, index, end, options.load_percent);
    for (auto& worker : workers) worker.join();
  } catch (const std::exception& error) {
    stop_requested = 1;
    for (auto& worker : workers)
      if (worker.joinable()) worker.join();
    remove_own_pid_file(options.pid_file);
    std::cerr << "workload: " << error.what() << '\n';
    return 7;
  }
  remove_own_pid_file(options.pid_file);
  return stop_requested ? 6 : 0;
}
