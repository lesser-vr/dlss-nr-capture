#pragma once
#include "common.hpp"

// A private, non-inherited job: only the owning app keeps it alive.
class WorkerJob {
public:
    WorkerJob() = default;
    WorkerJob(const WorkerJob&) = delete;
    WorkerJob& operator=(const WorkerJob&) = delete;
    ~WorkerJob() { reset(); }
    void reset() noexcept {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
    }
    void create() {
        reset();
        handle_ = CreateJobObjectW(nullptr, nullptr);
        if (!handle_) throw std::runtime_error("Create worker job failed");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            reset();
            throw std::runtime_error("Configure worker job failed");
        }
    }
    void assign(HANDLE process) {
        if (!handle_ || !AssignProcessToJobObject(handle_, process))
            throw std::runtime_error("Assign GPU worker to lifetime job failed");
    }
    HANDLE get() const noexcept { return handle_; }
private:
    HANDLE handle_{};
};
