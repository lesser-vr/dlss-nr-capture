#pragma once
#include <windows.h>
#include <cstdint>

inline bool capture_requires_awake(bool enabled, bool expected, bool failed,
                                   uint64_t now, uint64_t last_frame) noexcept {
    return enabled && expected && !failed && last_frame && now >= last_frame &&
           now - last_frame < 2000;
}
// All calls, including destruction, belong to the window thread.
class CapturePowerRequest {
public:
    using Setter = EXECUTION_STATE (WINAPI*)(EXECUTION_STATE);
    explicit CapturePowerRequest(Setter setter = SetThreadExecutionState) : setter_(setter) {}
    ~CapturePowerRequest() { update(false); }
    CapturePowerRequest(const CapturePowerRequest&) = delete;
    CapturePowerRequest& operator=(const CapturePowerRequest&) = delete;
    bool update(bool required) noexcept {
        if (required == active_) return true;
        const EXECUTION_STATE flags = ES_CONTINUOUS |
            (required ? ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : 0u);
        if (!setter_(flags)) return false;
        active_ = required;
        return true;
    }
    bool active() const noexcept { return active_; }
private:
    Setter setter_;
    bool active_{};
};
