#include "audio_capture.hpp"
#include "audio_output_queue.hpp"
#include "audio_delay_queue.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <chrono>
#include <cstring>
#include <stdexcept>
namespace {
std::wstring endpoint_name(IMMDevice* device) {
 ComPtr<IPropertyStore> properties; throw_if_failed(device->OpenPropertyStore(STGM_READ, &properties), "Open audio properties");
 PROPVARIANT value{}; PropVariantInit(&value); throw_if_failed(properties->GetValue(PKEY_Device_FriendlyName, &value), "Read audio device name");
 std::wstring name = value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : L"Audio input"; PropVariantClear(&value); return name;
}
std::runtime_error wave_error(const char* operation, MMRESULT result) {
 char message[256]{}; waveOutGetErrorTextA(result, message, static_cast<UINT>(sizeof(message))); return std::runtime_error(std::string(operation) + ": " + message);
}
struct WaveOutputBackend {
 HWAVEOUT handle{};
 MMRESULT prepare(WAVEHDR* header) { return waveOutPrepareHeader(handle, header, sizeof(WAVEHDR)); }
 MMRESULT write(WAVEHDR* header) { return waveOutWrite(handle, header, sizeof(WAVEHDR)); }
 MMRESULT unprepare(WAVEHDR* header) noexcept { return waveOutUnprepareHeader(handle, header, sizeof(WAVEHDR)); }
 void reset() noexcept { if (handle) waveOutReset(handle); }
 void close() noexcept { if (handle) waveOutClose(handle); handle = nullptr; }
};
struct AudioPacketLease {
 IAudioCaptureClient* capture;
 UINT32 frames;
 ~AudioPacketLease() { capture->ReleaseBuffer(frames); }
};
struct AudioClientStop {
 IAudioClient* client;
 ~AudioClientStop() { client->Stop(); }
};
}
AudioCapture::~AudioCapture() { stop(); }
std::vector<AudioCaptureDevice> AudioCapture::enumerate_devices() {
 ComPtr<IMMDeviceEnumerator> enumerator; throw_if_failed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Create audio device enumerator");
 ComPtr<IMMDeviceCollection> collection; throw_if_failed(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection), "Enumerate audio capture devices");
 UINT count{}; throw_if_failed(collection->GetCount(&count), "Count audio capture devices"); std::vector<AudioCaptureDevice> devices;
 for (UINT index = 0; index < count; ++index) {
  ComPtr<IMMDevice> device;
  if (FAILED(collection->Item(index, &device))) continue;
  LPWSTR raw_id{};
  if (FAILED(device->GetId(&raw_id))) continue;
  std::wstring id(raw_id);
  CoTaskMemFree(raw_id);
  devices.push_back({endpoint_name(device.Get()), std::move(id), std::move(device)});
 } return devices;
}
void AudioCapture::start(const AudioCaptureDevice& device, ErrorCallback on_error) { stop(); on_error_ = std::move(on_error); stopping_.store(false); thread_ = std::thread([this, device] { capture_loop(device); }); }
void AudioCapture::stop() { stopping_.store(true); if (thread_.joinable()) thread_.join(); on_error_ = {}; }
void AudioCapture::capture_loop(AudioCaptureDevice device) {
 const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED); WAVEFORMATEX* format{};
 try {
  ComPtr<IAudioClient> client; throw_if_failed(device.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())), "Activate audio capture device");
  throw_if_failed(client->GetMixFormat(&format), "Get audio capture format");
  WaveOutputBackend backend;
  AudioOutputQueue output(backend, std::max<size_t>(format->nAvgBytesPerSec / 5, format->nBlockAlign));
  MMRESULT result = waveOutOpen(&backend.handle, WAVE_MAPPER, format, 0, 0, CALLBACK_NULL);
  if (result != MMSYSERR_NOERROR) throw wave_error("Open default audio output", result);
  throw_if_failed(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr), "Initialize audio capture");
  ComPtr<IAudioCaptureClient> capture; throw_if_failed(client->GetService(IID_PPV_ARGS(&capture)), "Create audio capture client"); throw_if_failed(client->Start(), "Start audio capture");
  AudioClientStop stop_client{client.Get()};
  AudioDelayQueue delayed;
  uint32_t delay = delay_ms_.load();
  uint64_t epoch=sync_epoch_.load();
  while (!stopping_.load()) {
   const uint32_t requested = delay_ms_.load();
   const uint64_t current_epoch=sync_epoch_.load();
   if (requested != delay || epoch!=current_epoch) { delayed.clear(); output.discard(); delay = requested;epoch=current_epoch; }
   output.reap();
   UINT32 packets{}; throw_if_failed(capture->GetNextPacketSize(&packets), "Read audio packet size");
   while (packets > 0 && !stopping_.load()) {
    BYTE* data{}; UINT32 frames{}; DWORD flags{};UINT64 qpc{};
    throw_if_failed(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc), "Read audio packet");
    {
     AudioPacketLease lease{capture.Get(), frames};
     const uint64_t now=capture_qpc_100ns();
     if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY){delayed.clear();output.discard();sync_clock_.reset();}
     const bool valid_timestamp=!(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc && qpc<=now && now-qpc<2000000;
     const uint32_t target=std::min(200u,delay+(sync_enabled_?sync_clock_.delay(now):0u));
     const uint64_t packet_time=sync_enabled_ && valid_timestamp ? qpc/10000:now/10000;
     delayed.push(packet_time, target, data, frames * format->nBlockAlign,
         (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0,
         static_cast<size_t>(format->nAvgBytesPerSec) * (target + 200) / 1000);
    }
    throw_if_failed(capture->GetNextPacketSize(&packets), "Read next audio packet size");
   }
   const uint64_t queued_ms=sync_enabled_?output.queued_bytes()*1000/format->nAvgBytesPerSec:0;
   delayed.drain(capture_qpc_100ns()/10000+queued_ms, [&](const auto& bytes) {
       output.submit(bytes.data(), static_cast<DWORD>(bytes.size()), false);
       ++output_packets_;
   });
   std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
 } catch (const std::exception& error) { if (!stopping_.load() && on_error_) on_error_(L"Audio capture failed: " + widen(error.what())); }
 if (format) CoTaskMemFree(format); if (SUCCEEDED(initialized)) CoUninitialize();
}
