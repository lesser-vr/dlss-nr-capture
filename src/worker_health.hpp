#pragma once
#include <cstdint>

// Frame-independent watchdog. Allow cold NR initialization extra time, and
// throttle failed restarts so a missing executable cannot cause a tight loop.
constexpr bool worker_needs_restart(uint64_t now, uint64_t last_start,
                                    bool running_handle, bool exited, uint64_t heartbeat) noexcept {
    if (now < last_start || now - last_start < 5000) return false;
    if (!running_handle || exited) return true;
    if (!heartbeat) return now - last_start >= 30000;
    return now >= heartbeat && now - heartbeat > 2000;
}
