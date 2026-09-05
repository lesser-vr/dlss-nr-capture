#pragma once

#include "common.hpp"
#include "frame.hpp"

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct CaptureDevice {
    std::wstring name;
    std::wstring symbolic_link;
    ComPtr<IMFActivate> activation;
};

struct CaptureMode {
    DWORD native_index{};
    GUID subtype{};
    std::wstring format_name;
    uint32_t width{};
    uint32_t height{};
    uint32_t frame_rate_numerator{};
    uint32_t frame_rate_denominator{1};
    bool supported{};

    std::wstring display_name() const;
};

class CaptureEngine final : public IMFSourceReaderCallback {
public:
    using FrameCallback = std::function<void(VideoFrame&&)>;
    using ErrorCallback = std::function<void(std::wstring)>;

    CaptureEngine();
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    static std::vector<CaptureDevice> enumerate_devices();
    static std::vector<CaptureMode> enumerate_modes(const CaptureDevice& device);
    void start(const CaptureDevice& device, const CaptureMode& mode,
               FrameCallback on_frame, ErrorCallback on_error);
    void stop();
    void set_gpu_device(ID3D11Device* device) {
        std::scoped_lock lock(gpu_mutex_);
        gpu_converter_=GpuCaptureConverter{};gpu_manager_.Reset();gpu_device_=device;
    }
    void set_gpu_allowed(bool allowed) { gpu_allowed_.store(allowed); }
    void set_gpu_requested(bool requested) { gpu_requested_.store(requested); }
    void set_gpu_flip(bool flip) {gpu_flip_=flip;}
    std::wstring color_status() const {return color_.load().label();}
    uint64_t gpu_frames() const { return gpu_frames_.load(); }
    uint64_t cpu_frames() const { return cpu_frames_.load(); }
    const wchar_t* path_status() const {
        switch (path_.load()) {
        case 1: return L"GPU native";
        case 2: return L"CPU: GPU capture disabled";
        case 3: return L"CPU: history overlay enabled";
        case 4: return L"CPU: driver supplied system-memory sample";
        case 5: return L"CPU: GPU conversion unavailable or buffers busy";
        case 6: return L"CPU: native surface dimensions mismatch";
        default: return L"waiting for capture sample";
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP OnReadSample(HRESULT status, DWORD stream_index, DWORD flags,
                              LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

private:
    ComPtr<ID3D11Device> gpu_device_;
    std::atomic<bool> gpu_allowed_{true};
    std::atomic<bool> gpu_requested_{};
    std::atomic<uint64_t> gpu_frames_{}, cpu_frames_{};
    std::atomic<unsigned> path_{};
    std::atomic<CaptureColor> color_{};
    std::atomic<bool> gpu_flip_{};
    ComPtr<IMFDXGIDeviceManager> gpu_manager_;
    GpuCaptureConverter gpu_converter_;
    std::mutex gpu_mutex_;
    enum class PixelFormat { bgra, bgr24, nv12, p010, yuy2, uyvy, mjpg };
    static constexpr DWORD video_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    bool convert_to_bgra(PixelFormat format, const uint8_t* source, size_t length,
                                uint32_t width, uint32_t height, std::vector<uint8_t>& output);
    void request_next();
    void report_error(HRESULT hr, const wchar_t* operation);

    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFActivate> activation_;
    ComPtr<IWICImagingFactory> wic_factory_;
    FrameCallback on_frame_;
    ErrorCallback on_error_;
    uint32_t width_{};
    uint32_t height_{};
    PixelFormat pixel_format_{PixelFormat::bgra};
    GUID active_subtype_{};
    uint64_t sequence_{};
    bool running_{};
};
