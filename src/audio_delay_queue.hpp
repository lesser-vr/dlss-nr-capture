#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include <cstring>
class AudioDelayQueue {
    struct Packet { uint64_t due; std::vector<uint8_t> bytes; };
    std::deque<Packet> packets_;
    size_t bytes_{};
public:
    void clear() { packets_.clear(); bytes_ = 0; }
    void push(uint64_t now, uint32_t delay, const void* data, size_t size, bool silent, size_t limit) {
        if (size > limit) return;
        while (!packets_.empty() && bytes_ > limit - size) {
            bytes_ -= packets_.front().bytes.size(); packets_.pop_front();
        }
        Packet packet{now + delay, std::vector<uint8_t>(size, 0)};
        if (!silent && data) std::memcpy(packet.bytes.data(), data, size);
        packets_.push_back(std::move(packet)); bytes_ += size;
    }
    template<class Consumer> void drain(uint64_t now, Consumer consume) {
        while (!packets_.empty() && packets_.front().due <= now) {
            consume(packets_.front().bytes);
            bytes_ -= packets_.front().bytes.size(); packets_.pop_front();
        }
    }
    size_t bytes() const { return bytes_; }
};
