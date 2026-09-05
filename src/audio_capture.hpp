#pragma once
#include "common.hpp"
#include "av_sync.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
struct AudioCaptureDevice { std::wstring name; std::wstring id; ComPtr<IMMDevice> device; };
class AudioCapture final {
public:
 using ErrorCallback = std::function<void(std::wstring)>;
 AudioCapture() = default; ~AudioCapture();
 AudioCapture(const AudioCapture&) = delete; AudioCapture& operator=(const AudioCapture&) = delete;
 static std::vector<AudioCaptureDevice> enumerate_devices();
 void start(const AudioCaptureDevice& device, ErrorCallback on_error); void stop();
 void set_delay_ms(uint32_t value) { delay_ms_.store(value > 200 ? 200 : value); }
 void set_sync_enabled(bool value){sync_enabled_=value;reset_sync();}
 void reset_sync(){sync_clock_.reset();++sync_epoch_;}
 void observe_video(int64_t stream,uint64_t arrival,uint64_t present){sync_clock_.observe(stream,arrival,present);}
 uint32_t sync_delay_ms(){return sync_enabled_?sync_clock_.delay(capture_qpc_100ns()):0;}
 uint64_t output_packets() const {return output_packets_.load();}
private:
 AvSyncClock sync_clock_;
 std::atomic_bool sync_enabled_{};
 std::atomic_uint64_t sync_epoch_{};
 std::atomic_uint64_t output_packets_{};
 void capture_loop(AudioCaptureDevice device);
 std::atomic_bool stopping_{false}; std::thread thread_; ErrorCallback on_error_;
 std::atomic_uint32_t delay_ms_{};
};
