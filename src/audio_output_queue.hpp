#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <cstring>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Backend owns the output handle. Queue teardown returns driver-owned buffers
// before freeing storage, including when capture or output submission throws.
template<class Backend>
class AudioOutputQueue {
    struct Packet {
        std::vector<char> bytes;
        WAVEHDR header{};
        bool prepared{};
        explicit Packet(DWORD size) : bytes(size, 0) {
            header.lpData = bytes.data();
            header.dwBufferLength = size;
        }
    };
public:
    explicit AudioOutputQueue(Backend& backend) : backend_(backend) {}
    AudioOutputQueue(const AudioOutputQueue&) = delete;
    AudioOutputQueue& operator=(const AudioOutputQueue&) = delete;
    ~AudioOutputQueue() {
        backend_.reset();
        bool retained = false;
        for (auto& packet : pending_) {
            if (packet->prepared && backend_.unprepare(&packet->header) != MMSYSERR_NOERROR) {
                // A broken driver may still reference this memory. Retain it
                // rather than create a use-after-free or block shutdown forever.
                (void)packet.release();
                retained = true;
            }
        }
        pending_.clear();
        if (!retained) backend_.close();
    }
    void submit(const BYTE* data, DWORD size, bool silent) {
        auto packet = std::make_unique<Packet>(size);
        if (!silent && data) std::memcpy(packet->bytes.data(), data, size);
        // Allocate queue storage before giving the driver a pointer.
        pending_.push_back(std::move(packet));
        auto& item = *pending_.back();
        const MMRESULT prepared = backend_.prepare(&item.header);
        if (prepared != MMSYSERR_NOERROR)
            throw std::runtime_error("Prepare audio output failed: " + std::to_string(prepared));
        item.prepared = true;
        const MMRESULT written = backend_.write(&item.header);
        if (written != MMSYSERR_NOERROR)
            throw std::runtime_error("Write audio output failed: " + std::to_string(written));
    }
    void reap() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto& packet = **it;
            if ((packet.header.dwFlags & WHDR_DONE) == 0) { ++it; continue; }
            if (backend_.unprepare(&packet.header) != MMSYSERR_NOERROR)
                throw std::runtime_error("Release audio output buffer failed");
            packet.prepared = false;
            it = pending_.erase(it);
        }
    }
private:
    Backend& backend_;
    std::list<std::unique_ptr<Packet>> pending_;
};
