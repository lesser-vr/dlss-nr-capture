#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline double quality_mae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(static_cast<int>(a[i]) - b[i]);
    return a.empty() ? 0 : sum / a.size();
}
// Not motion compensated: a review signal, never an automatic ghosting verdict.
inline double quality_residual_change(const std::vector<uint8_t>& output, const std::vector<uint8_t>& input,
    const std::vector<uint8_t>& previous_output, const std::vector<uint8_t>& previous_input) {
    double sum = 0;
    for (size_t i = 0; i < output.size(); ++i)
        sum += std::abs((static_cast<int>(output[i]) - input[i]) -
                        (static_cast<int>(previous_output[i]) - previous_input[i]));
    return output.empty() ? 0 : sum / output.size();
}
