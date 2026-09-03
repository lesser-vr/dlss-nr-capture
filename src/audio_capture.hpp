#pragma once
#include "common.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
struct AudioCaptureDevice { std::wstring name; ComPtr<IMMDevice> device; };
class AudioCapture final {
public:
 using ErrorCallback = std::function<void(std::wstring)>;
 AudioCapture() = default; ~AudioCapture();
 AudioCapture(const AudioCapture&) = delete; AudioCapture& operator=(const AudioCapture&) = delete;
 static std::vector<AudioCaptureDevice> enumerate_devices();
 void start(const AudioCaptureDevice& device, ErrorCallback on_error); void stop();
private:
 void capture_loop(AudioCaptureDevice device);
 std::atomic_bool stopping_{false}; std::thread thread_; ErrorCallback on_error_;
};
