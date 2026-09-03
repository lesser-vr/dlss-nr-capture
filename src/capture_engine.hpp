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

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP OnReadSample(HRESULT status, DWORD stream_index, DWORD flags,
                              LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

private:
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
    uint64_t sequence_{};
    bool running_{};
};
