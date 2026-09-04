#pragma once
#include "audio_output_queue.hpp"
#include "audio_delay_queue.hpp"
#include <algorithm>

struct FakeAudioOutput {
    std::vector<WAVEHDR*> prepared;
    std::vector<std::vector<char>> written;
    int prepare_calls{}, write_calls{}, resets{}, closes{}, released{};
    int fail_prepare{}, fail_write{}, fail_unprepare{};
    bool unsafe_release{};
    MMRESULT prepare(WAVEHDR* header) {
        if (++prepare_calls == fail_prepare) return MMSYSERR_ERROR;
        prepared.push_back(header);
        return MMSYSERR_NOERROR;
    }
    MMRESULT write(WAVEHDR* header) {
        if (++write_calls == fail_write) return MMSYSERR_ERROR;
        written.emplace_back(header->lpData, header->lpData + header->dwBufferLength);
        header->dwFlags |= WHDR_INQUEUE;
        return MMSYSERR_NOERROR;
    }
    MMRESULT unprepare(WAVEHDR* header) {
        if (fail_unprepare > 0) { --fail_unprepare; return WAVERR_STILLPLAYING; }
        unsafe_release |= (header->dwFlags & WHDR_INQUEUE) != 0;
        std::erase(prepared, header);
        ++released;
        return MMSYSERR_NOERROR;
    }
    void reset() {
        ++resets;
        for (auto* header : prepared) header->dwFlags &= ~WHDR_INQUEUE;
    }
    void close() { ++closes; }
};

template<class Check>
void check_audio_output(Check check) {
    const BYTE pcm[] = {1, 2, 3, 4};
    for (int fault = 0; fault < 4; ++fault) {
        FakeAudioOutput backend;
        backend.fail_prepare = fault == 1 ? 2 : 0;
        backend.fail_write = fault == 2 ? 2 : 0;
        bool threw = false;
        try {
            AudioOutputQueue queue(backend);
            queue.submit(pcm, 4, false);
            queue.submit(nullptr, 4, true);
            if (fault == 3) throw std::runtime_error("Injected capture failure");
        } catch (const std::runtime_error&) { threw = true; }
        check(threw == (fault != 0), "audio injected failure reached expected path");
        check(backend.prepared.empty() && backend.resets == 1 && backend.closes == 1,
              "audio failure/stop resets, unprepares and closes output");
        check(!backend.unsafe_release, "audio driver returns buffers before release");
        check(backend.written.front() == std::vector<char>({1, 2, 3, 4}), "audio PCM copied intact");
        if (fault == 0) check(backend.written.back() == std::vector<char>(4, 0), "silent audio zero-filled");
    }
    FakeAudioOutput backend;
    {
        AudioOutputQueue queue(backend);
        queue.submit(pcm, 4, false);
        backend.prepared.front()->dwFlags = WHDR_DONE;
        queue.reap();
        check(backend.prepared.empty() && backend.released == 1, "completed audio reclaimed during capture");
    }
    check(backend.released == 1 && backend.closes == 1, "completed audio not released twice");
    FakeAudioOutput transient;
    bool threw = false;
    try {
        AudioOutputQueue queue(transient);
        queue.submit(pcm, 4, false);
        transient.prepared.front()->dwFlags = WHDR_DONE;
        transient.fail_unprepare = 1;
        queue.reap();
    } catch (const std::runtime_error&) { threw = true; }
    check(threw && transient.prepared.empty() && transient.released == 1 && transient.closes == 1,
          "failed audio reclaim retried after reset during teardown");
    FakeAudioOutput empty;
    { AudioOutputQueue queue(empty); }
    check(empty.resets == 1 && empty.closes == 1, "audio output cleaned before first packet");
    FakeAudioOutput bounded;
    {
        AudioOutputQueue queue(bounded, 4);
        queue.submit(pcm, 4, false); queue.submit(pcm, 4, false);
        check(bounded.resets == 1 && bounded.prepared.size() == 1, "audio backlog capped by reset before new audio");
    }
    AudioDelayQueue delayed;
    int played = 0;
    delayed.push(1000, 50, pcm, 4, false, 8);
    delayed.drain(1049, [&](const auto&) { ++played; });
    check(played == 0, "audio delay waits until due");
    delayed.drain(1050, [&](const auto& bytes) { ++played; check(bytes[0] == 1, "delayed audio payload preserved"); });
    check(played == 1 && delayed.bytes() == 0, "delayed audio drains exactly once");
    for (int i = 0; i < 4; ++i) delayed.push(2000, 50, pcm, 4, false, 8);
    check(delayed.bytes() == 8, "audio delay queue has bounded memory");
    delayed.clear();
    check(delayed.bytes() == 0, "audio delay change clears stale packets");
}
