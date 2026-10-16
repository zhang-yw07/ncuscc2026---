#pragma once

#ifndef __linux__
#error "The stage1 metrics programs only support Linux"
#endif

#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace hpc_metrics {

inline constexpr const char* kSchema = "hpc-metrics.v1";
inline constexpr std::size_t kMaxLineBytes = 16 * 1024;
inline constexpr std::size_t kMaxFileBytes = 4 * 1024 * 1024;

inline std::string json_string(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<unsigned>(ch) << std::dec << std::setfill(' ');
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  out << '"';
  return out.str();
}

template <typename T>
inline void write_optional_number(std::ostringstream& out,
                                  const std::optional<T>& value) {
  if (value.has_value()) out << *value;
  else out << "null";
}

inline void write_optional_decimal(std::ostringstream& out,
                                   const std::optional<double>& value,
                                   int precision = 1) {
  if (value.has_value()) {
    out << std::fixed << std::setprecision(precision) << *value;
  } else {
    out << "null";
  }
}

}  // namespace hpc_metrics
