#pragma once
#include <cstdint>
#include <string_view>
constexpr bool audio_endpoint_matches(std::wstring_view selected, std::wstring_view candidate) noexcept {
    return !selected.empty() && selected == candidate;
}
// Legacy name-only preferences migrate only when exactly one endpoint matches.
// Once an ID is known, never fall back to a same-name but different endpoint.
template<class Devices>
size_t audio_restore_index(const Devices& devices, std::wstring_view id, std::wstring_view name) {
    size_t selected = devices.size();
    for (size_t i = 0; i < devices.size(); ++i) {
        if (!id.empty()) {
            if (audio_endpoint_matches(id, devices[i].id)) return i;
        } else if (!name.empty() && devices[i].name == name) {
            if (selected != devices.size()) return devices.size();
            selected = i;
        }
    }
    return selected;
}
constexpr bool audio_retry_due(uint64_t now, uint64_t last_retry, bool failed) noexcept {
    return failed && now >= last_retry && now - last_retry >= 5000;
}
