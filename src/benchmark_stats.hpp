#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

struct BenchmarkStats {
    double mean{}, p95{}, p99{}, maximum{};
};
inline BenchmarkStats benchmark_stats(std::vector<uint64_t> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) {
        return static_cast<double>(values[static_cast<size_t>(std::ceil(p * values.size())) - 1]);
    };
    return {std::accumulate(values.begin(), values.end(), 0.0) / values.size(),
            percentile(0.95), percentile(0.99), static_cast<double>(values.back())};
}
