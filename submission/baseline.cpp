#include "support.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>

// STARTER CODE: main intentionally combines setup, execution, validation and reporting.
// First identify these responsibilities; then extract a clean filter_records interface.
// Keep support.hpp's reference/codec semantics and the public tests unchanged.
int main(int argc, char** argv) try {
    std::uint64_t count=100000, max_records=10000000;
    std::uint32_t seed=2026;
    unsigned hit_percent=50, repeats=5;
    std::string pattern="random", input_path, output_path, export_path;
    Query query;
    std::set<std::string> seen;
    for (int i=1; i<argc; ++i) {
        const std::string key=argv[i];
        if (key=="--help") {
            std::cout << "log_filter [--count N | --input FILE] [--hit-percent 0..100]\n"
                      << " [--pattern random|blocks] [--seed U32] [--repeats 1..100]\n"
                      << " [--begin U64] [--end U64] [--min-latency U32] [--flags U32]\n"
                      << " [--output FILE] [--export-input FILE] [--max-records N]\n"
                      << "Defaults: count=100000 seed=2026 hit=50 repeats=5 max-records=10000000\n"
                      << "Query: [1000,2000), latency>=100, all bits of flags=3. Decimal arguments.\n";
            return 0;
        }
        if (!seen.insert(key).second || i+1==argc) throw std::runtime_error("duplicate option or missing value");
        const std::string value=argv[++i];
        if (key=="--count") count=number(value);
        else if (key=="--max-records") max_records=number(value);
        else if (key=="--seed") seed=narrow32(number(value));
        else if (key=="--hit-percent") { const auto v=number(value); if(v>100) throw std::runtime_error("invalid hit percent"); hit_percent=static_cast<unsigned>(v); }
        else if (key=="--repeats") { const auto v=number(value); if(v<1 || v>100) throw std::runtime_error("invalid repeats"); repeats=static_cast<unsigned>(v); }
        else if (key=="--begin") query.begin=number(value);
        else if (key=="--end") query.end=number(value);
        else if (key=="--min-latency") query.min_latency_us=narrow32(number(value));
        else if (key=="--flags") query.required_flags=narrow32(number(value));
        else if (key=="--pattern") pattern=value;
        else if (key=="--input") input_path=value;
        else if (key=="--output") output_path=value;
        else if (key=="--export-input") export_path=value;
        else throw std::runtime_error("unknown option: "+key);
    }
    if (query.begin>query.end || (pattern!="random" && pattern!="blocks")) throw std::runtime_error("invalid query or pattern");
    if (seen.count("--input") && input_path.empty()) throw std::runtime_error("empty input path");
    if ((seen.count("--output") && output_path.empty()) || (seen.count("--export-input") && export_path.empty())) throw std::runtime_error("empty output path");
    if (!input_path.empty() && (seen.count("--count") || seen.count("--seed") || seen.count("--hit-percent") || seen.count("--pattern") || seen.count("--export-input")))
        throw std::runtime_error("input file cannot be combined with generator options");
    if (input_path.empty() && (count>max_records || count>std::numeric_limits<std::size_t>::max()/sizeof(Record)))
        throw std::runtime_error("generated count exceeds limit");
    // Generation and file decoding are deliberately outside core timing.
    auto input=input_path.empty() ? generate(static_cast<std::size_t>(count),hit_percent,pattern,seed) : load(input_path,max_records);
    if (!export_path.empty()) save(export_path,input,input.size());
    auto expected=reference_filter(input,query);
    std::vector<Record> output(input.size());
    std::vector<double> times;
    std::size_t matched=0;
    std::cout << std::setprecision(10)
              << "implementation=baseline threads=1 generator=v1 source=" << (input_path.empty()?"generated":"file")
              << " count=" << input.size() << " sizeof_record=" << sizeof(Record)
              << " seed=" << seed << " pattern=" << pattern << " requested_hit_percent=" << hit_percent
              << " begin=" << query.begin << " end=" << query.end << " min_latency=" << query.min_latency_us
              << " flags=" << query.required_flags << " warmup=1 repeats=" << repeats << '\n';
    for (int run=-1; run<static_cast<int>(repeats); ++run) {
        matched=0;
        const auto start=std::chrono::steady_clock::now();
        // BEGIN CORE: stable serial filter; no artificial slow path or extra copying.
        for (std::size_t i=0; i<input.size(); ++i) {
            const Record& r=input[i];
            if (query.begin<=r.timestamp && r.timestamp<query.end &&
                r.latency_us>=query.min_latency_us && (r.flags & query.required_flags)==query.required_flags) {
                output[matched++]=r;
            }
        }
        // END CORE: include all output writes, exclude validation and console output.
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if (matched!=expected.size()) throw std::runtime_error("validation: wrong count");
        for (std::size_t i=0; i<matched; ++i)
            if (!equal_record(output[i],expected[i])) throw std::runtime_error("validation: wrong record/order at "+std::to_string(i));
        if (run>=0) {
            times.push_back(ms);
            std::cout << "sample=" << run << " core_ms=" << ms << " matched=" << matched << " check=PASS\n";
        }
    }
    std::sort(times.begin(),times.end());
    const std::size_t middle=times.size()/2;
    const double median=times.size()%2 ? times[middle] : (times[middle-1]+times[middle])/2;
    std::cout << "median_core_ms=" << median << " actual_hit_percent=" << (input.empty()?0:100.0*matched/input.size()) << " input_Mrecords_per_s=";
    if(median>0) std::cout << input.size()/(median*1000.0);
    else std::cout << "unavailable(timer_resolution)";
    std::cout << '\n';
    // Serialize only the final validated result. File writing is NOT core timing.
    if (!output_path.empty()) save(output_path,output,matched);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 2;
}
