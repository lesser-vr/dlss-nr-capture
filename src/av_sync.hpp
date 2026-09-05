#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <windows.h>
inline uint64_t capture_qpc_100ns(){LARGE_INTEGER q,f;QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return static_cast<uint64_t>(q.QuadPart/f.QuadPart)*10000000+static_cast<uint64_t>(q.QuadPart%f.QuadPart)*10000000/f.QuadPart;}
// MF timestamps are stream-relative, not necessarily in the WASAPI QPC epoch.
// Estimate that offset from arrival; this does not measure the card's fixed delay.
class AvSyncClock {
    std::mutex mutex_;
    bool valid_{};
    int64_t offset_{},last_stream_{};
    uint64_t last_seen_{};
    uint32_t delay_{};
public:
    void reset(){std::scoped_lock lock(mutex_);valid_=false;delay_=0;last_seen_=0;}
    void observe(int64_t stream,uint64_t arrival,uint64_t presented){
        if(!arrival || presented<arrival) return;
        std::scoped_lock lock(mutex_);
        const int64_t candidate=static_cast<int64_t>(arrival)-stream;
        if(!valid_ || stream<=last_stream_ || arrival-last_seen_>10000000 || std::abs(candidate-offset_)>5000000){offset_=candidate;delay_=0;valid_=true;}
        offset_=std::min(offset_,candidate);last_stream_=stream;last_seen_=arrival;
        const int64_t latency=static_cast<int64_t>(presented)-stream-offset_;
        const uint32_t sample=static_cast<uint32_t>(std::clamp<int64_t>(latency/10000,0,200));
        delay_=(delay_*7+sample+4)/8;
    }
    uint32_t delay(uint64_t now){std::scoped_lock lock(mutex_);return valid_ && now>=last_seen_ && now-last_seen_<10000000 ? delay_:0;}
};
