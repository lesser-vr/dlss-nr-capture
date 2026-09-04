#pragma once
#include <optional>
#include <cstddef>

// Keep the selected object when it disappears, so live-session indices and
// reconnect identity never accidentally refer to a different enumerated device.
template<class Devices, class Key>
std::optional<size_t> remap_selected_device(const Devices& old_devices, Devices& discovered,
                                          std::optional<size_t> selected, Key key) {
    if (!selected || *selected >= old_devices.size()) return std::nullopt;
    const auto& previous = old_devices[*selected];
    const auto id = key(previous);
    for (size_t i = 0; i < discovered.size(); ++i)
        if (!id.empty() && key(discovered[i]) == id) return i;
    discovered.push_back(previous);
    return discovered.size() - 1;
}
