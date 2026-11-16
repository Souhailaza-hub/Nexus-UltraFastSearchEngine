#include "nexus/engine.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

double Percentile(std::vector<double> values, double percentile) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>((percentile / 100.0) * static_cast<double>(values.size() - 1));
    return values[index];
}

std::vector<double> Measure(const std::wstring& query, int runs,
                            const std::function<std::vector<nexus::FileRecord>(const std::wstring&)>& search) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        const auto begin = Clock::now();
        const auto result = search(query);
        const auto end = Clock::now();
        if (result.empty()) throw std::runtime_error("benchmark query returned no results");
        samples.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
    }
    return samples;
}
}

int wmain(int argc, wchar_t** argv) {
    std::uint64_t file_count = 100000;
    std::uint64_t directory_count = 1;
    int runs = 200;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--files" && i + 1 < argc) file_count = std::stoull(argv[++i]);
        else if (arg == L"--dirs" && i + 1 < argc) directory_count = std::stoull(argv[++i]);
        else if (arg == L"--runs" && i + 1 < argc) runs = std::stoi(argv[++i]);
        else { std::wcerr << L"Usage: nexus_benchmark [--files count] [--dirs count] [--runs count]\n"; return 2; }
    }
    if (file_count == 0 || directory_count == 0 || directory_count > file_count || runs < 3) {
        std::wcerr << L"files and dirs must be positive, dirs cannot exceed files, and runs must be at least 3\n";
        return 2;
    }

    const auto unique = std::to_wstring(Clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / (L"nexus_benchmark_" + unique);
    std::error_code ec;
    if (!std::filesystem::create_directory(root, ec) || ec) {
        std::wcerr << L"Could not create benchmark directory: " << root.wstring() << L"\n";
        return 1;
    }

    int exit_code = 0;
    try {
        const auto generate_begin = Clock::now();
        std::vector<std::filesystem::path> directories;
        directories.reserve(static_cast<std::size_t>(directory_count));
        for (std::uint64_t i = 0; i < directory_count; ++i) {
            wchar_t name[32]{};
            swprintf_s(name, std::size(name), L"shard-%04llu", static_cast<unsigned long long>(i));
            directories.push_back(root / name);
            if (!std::filesystem::create_directory(directories.back(), ec) || ec)
                throw std::runtime_error("failed to create benchmark shard directory");
        }
        for (std::uint64_t i = 0; i < file_count; ++i) {
            wchar_t name[64]{};
            swprintf_s(name, std::size(name), L"item-%08llu-alpha.txt", static_cast<unsigned long long>(i));
            std::ofstream file(directories[static_cast<std::size_t>(i % directory_count)] / name, std::ios::binary);
            if (!file) throw std::runtime_error("failed to create benchmark file");
        }
        const auto generate_end = Clock::now();

        nexus::Engine engine;
        const auto index_begin = Clock::now();
        engine.IndexPath(root);
        const auto index_end = Clock::now();
        const auto stats = engine.Stats();
        if (stats.files != file_count || stats.errors != 0) throw std::runtime_error("index count or error validation failed");

        const auto exact = Measure(L"item-00000001-alpha.txt", runs,
            [&](const auto& q) { return engine.SearchExact(q, 100); });
        const auto prefix = Measure(L"item-0000", runs,
            [&](const auto& q) { return engine.SearchPrefix(q, 100); });
        const auto substring = Measure(L"alpha", runs,
            [&](const auto& q) { return engine.SearchSubstring(q, 100); });

        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        const SIZE_T working_set = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))
            ? memory.WorkingSetSize : 0;
        const auto print = [](const wchar_t* name, const std::vector<double>& samples) {
            std::wcout << std::fixed << std::setprecision(2) << name << L"_us_p50=" << Percentile(samples, 50)
                       << L" _us_p95=" << Percentile(samples, 95)
                       << L" _us_p99=" << Percentile(samples, 99) << L"\n";
        };
        std::wcout << L"files=" << stats.files << L" directories=" << stats.directories << L" shards=" << directory_count << L" errors=" << stats.errors << L"\n";
        std::wcout << L"generation_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(generate_end - generate_begin).count() << L"\n";
        std::wcout << L"index_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(index_end - index_begin).count() << L"\n";
        std::wcout << L"estimated_index_bytes=" << stats.estimated_memory_bytes << L" working_set_bytes=" << working_set << L"\n";
        print(L"exact", exact); print(L"prefix", prefix); print(L"substring", substring);
        std::wcout << std::flush;
    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed: " << e.what() << "\n";
        exit_code = 1;
    }
    std::filesystem::remove_all(root, ec);
    return exit_code;
}
