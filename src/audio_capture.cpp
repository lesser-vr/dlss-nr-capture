#include "audio_capture.hpp"
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
}
AudioCapture::~AudioCapture() { stop(); }
std::vector<AudioCaptureDevice> AudioCapture::enumerate_devices() {
 ComPtr<IMMDeviceEnumerator> enumerator; throw_if_failed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Create audio device enumerator");
 ComPtr<IMMDeviceCollection> collection; throw_if_failed(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection), "Enumerate audio capture devices");
 UINT count{}; throw_if_failed(collection->GetCount(&count), "Count audio capture devices"); std::vector<AudioCaptureDevice> devices;
 for (UINT index = 0; index < count; ++index) { ComPtr<IMMDevice> device; if (SUCCEEDED(collection->Item(index, &device))) devices.push_back({endpoint_name(device.Get()), std::move(device)}); } return devices;
}
void AudioCapture::start(const AudioCaptureDevice& device, ErrorCallback on_error) { stop(); on_error_ = std::move(on_error); stopping_.store(false); thread_ = std::thread([this, device] { capture_loop(device); }); }
void AudioCapture::stop() { stopping_.store(true); if (thread_.joinable()) thread_.join(); on_error_ = {}; }
void AudioCapture::capture_loop(AudioCaptureDevice device) {
 const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED); HWAVEOUT output{}; WAVEFORMATEX* format{};
 try {
  ComPtr<IAudioClient> client; throw_if_failed(device.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())), "Activate audio capture device");
  throw_if_failed(client->GetMixFormat(&format), "Get audio capture format"); MMRESULT result = waveOutOpen(&output, WAVE_MAPPER, format, 0, 0, CALLBACK_NULL);
  if (result != MMSYSERR_NOERROR) throw wave_error("Open default audio output", result);
  throw_if_failed(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr), "Initialize audio capture");
  ComPtr<IAudioCaptureClient> capture; throw_if_failed(client->GetService(IID_PPV_ARGS(&capture)), "Create audio capture client"); throw_if_failed(client->Start(), "Start audio capture"); std::vector<WAVEHDR*> pending;
  while (!stopping_.load()) {
   for (auto it = pending.begin(); it != pending.end();) { if (((*it)->dwFlags & WHDR_DONE) == 0) { ++it; continue; } waveOutUnprepareHeader(output, *it, sizeof(WAVEHDR)); delete[] (*it)->lpData; delete *it; it = pending.erase(it); }
   UINT32 packets{}; throw_if_failed(capture->GetNextPacketSize(&packets), "Read audio packet size");
   while (packets > 0) {
    BYTE* data{}; UINT32 frames{}; DWORD flags{}; throw_if_failed(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Read audio packet");
    const DWORD bytes = frames * format->nBlockAlign; auto* header = new WAVEHDR{}; header->dwBufferLength = bytes; header->lpData = new char[bytes]{};
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) std::memcpy(header->lpData, data, bytes);
    result = waveOutPrepareHeader(output, header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) { capture->ReleaseBuffer(frames); delete[] header->lpData; delete header; throw wave_error("Prepare audio output", result); }
    result = waveOutWrite(output, header, sizeof(WAVEHDR)); capture->ReleaseBuffer(frames);
    if (result != MMSYSERR_NOERROR) { waveOutUnprepareHeader(output, header, sizeof(WAVEHDR)); delete[] header->lpData; delete header; throw wave_error("Write audio output", result); }
    pending.push_back(header); throw_if_failed(capture->GetNextPacketSize(&packets), "Read next audio packet size");
   } std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  client->Stop(); waveOutReset(output); for (auto* header : pending) { waveOutUnprepareHeader(output, header, sizeof(WAVEHDR)); delete[] header->lpData; delete header; }
 } catch (const std::exception& error) { if (!stopping_.load() && on_error_) on_error_(L"Audio capture failed: " + widen(error.what())); }
 if (output) waveOutClose(output); if (format) CoTaskMemFree(format); if (SUCCEEDED(initialized)) CoUninitialize();
}
