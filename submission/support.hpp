#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

struct Record {
    std::uint64_t timestamp;
    std::uint32_t source_id, latency_us, payload_bytes, flags;
};
struct Query {
    std::uint64_t begin = 1000, end = 2000;
    std::uint32_t min_latency_us = 100, required_flags = 3;
};
// Shared data/codec utilities are provided so the exercise can focus on filtering.
// The measured loop deliberately does not use this reference implementation.
inline bool equal_record(const Record& a, const Record& b) {
    return a.timestamp == b.timestamp && a.source_id == b.source_id &&
        a.latency_us == b.latency_us && a.payload_bytes == b.payload_bytes && a.flags == b.flags;
}
inline std::vector<Record> reference_filter(const std::vector<Record>& input, const Query& q) {
    std::vector<Record> result;
    for (const Record& r : input) {
        if (r.timestamp < q.begin) continue;
        if (r.timestamp >= q.end) continue;
        if (r.latency_us < q.min_latency_us) continue;
        if ((r.flags & q.required_flags) != q.required_flags) continue;
        result.push_back(r);
    }
    return result;
}
inline std::uint64_t number(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected unsigned decimal integer: " + s);
    return std::stoull(s);
}
inline std::uint32_t narrow32(std::uint64_t n) {
    if (n > UINT32_MAX) throw std::runtime_error("value exceeds uint32");
    return static_cast<std::uint32_t>(n);
}
inline std::uint32_t random32(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}
// Synthetic v1 data targets the DEFAULT query only. Custom queries can change hit rate.
// Blocks use a 10,000-record repeating period; the first hit_percent*100 match.
inline std::vector<Record> generate(std::size_t n, unsigned hit_percent,
                                    const std::string& pattern, std::uint32_t seed) {
    std::vector<Record> input(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto decision = random32(seed);
        const bool hit = pattern == "random" ? (decision >> 8) % 100 < hit_percent
                                             : i % 10000 < hit_percent * 100;
        Record r{1000 + random32(seed) % 1000,
                 static_cast<std::uint32_t>(i),
                 100 + random32(seed) % 1000,
                 random32(seed), 3u | (random32(seed) & 0xfffffffcu)};
        if (!hit) {
            switch ((decision >> 16) % 3) {
                case 0: r.timestamp = 2000 + random32(seed) % 1000; break;
                case 1: r.latency_us = random32(seed) % 100; break;
                default: r.flags &= ~1u; break;
            }
        }
        input[i] = r;
    }
    return input;
}
// Explicit little-endian codec: never serialize compiler-dependent struct padding.
inline std::uint64_t get_le(const unsigned char* p, unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= std::uint64_t(p[i]) << (8*i);
    return value;
}
inline void put_le(unsigned char* p, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) p[i] = static_cast<unsigned char>(value >> (8*i));
}
inline std::vector<Record> load(const std::filesystem::path& path, std::uint64_t max_records) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input");
    std::array<unsigned char,24> h{};
    if (!file.read(reinterpret_cast<char*>(h.data()), h.size())) throw std::runtime_error("truncated header");
    const std::string magic(h.begin(), h.begin()+8);
    if (magic != "NCULOG01" || get_le(h.data()+8,4) != 1 || get_le(h.data()+12,4) != 24)
        throw std::runtime_error("invalid header");
    const auto n = get_le(h.data()+16,8);
    if (n > max_records || n > (UINT64_MAX-24)/24 || n > std::numeric_limits<std::size_t>::max()/sizeof(Record))
        throw std::runtime_error("record count exceeds limit");
    if (std::filesystem::file_size(path) != 24+24*n) throw std::runtime_error("file length mismatch");
    std::vector<Record> input(static_cast<std::size_t>(n));
    for (auto& r : input) {
        if (!file.read(reinterpret_cast<char*>(h.data()), h.size())) throw std::runtime_error("truncated record");
        r = {get_le(h.data(),8), static_cast<std::uint32_t>(get_le(h.data()+8,4)),
             static_cast<std::uint32_t>(get_le(h.data()+12,4)), static_cast<std::uint32_t>(get_le(h.data()+16,4)),
             static_cast<std::uint32_t>(get_le(h.data()+20,4))};
    }
    if (file.peek() != std::char_traits<char>::eof() || file.bad()) throw std::runtime_error("input changed or read failed");
    return input;
}
inline void save(const std::filesystem::path& path, const std::vector<Record>& records, std::size_t count) {
    // No overwrite option: also prevents accidentally truncating the input or a hard link.
    // This local teaching utility assumes no other process races to create the same path.
    if (std::filesystem::exists(path) || std::filesystem::is_symlink(path))
        throw std::runtime_error("output already exists");
    if (count > records.size()) throw std::runtime_error("invalid output count");
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot create output");
    try {
        std::array<unsigned char,24> bytes{};
        const std::string magic = "NCULOG01";
        std::copy(magic.begin(),magic.end(),bytes.begin());
        put_le(bytes.data()+8,1,4); put_le(bytes.data()+12,24,4); put_le(bytes.data()+16,count,8);
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        for (std::size_t i=0; i<count; ++i) {
            const auto& r=records[i];
            put_le(bytes.data(),r.timestamp,8); put_le(bytes.data()+8,r.source_id,4);
            put_le(bytes.data()+12,r.latency_us,4); put_le(bytes.data()+16,r.payload_bytes,4);
            put_le(bytes.data()+20,r.flags,4);
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        }
        file.close();
        if (!file) throw std::runtime_error("output write/close failed");
    } catch (...) {
        file.close();
        std::error_code ignored;
        std::filesystem::remove(path,ignored);
        throw;
    }
}
