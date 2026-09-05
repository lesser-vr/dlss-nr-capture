#include "capture_engine.hpp"
#include "av_sync.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <d3d10.h>

#include <algorithm>
#include <limits>
#include <sstream>

namespace {
uint8_t clamp_byte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

bool is_supported_format(const GUID& subtype)
{
    return subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32 || subtype == MFVideoFormat_RGB24 ||
           subtype == MFVideoFormat_P010 || subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2 ||
           subtype == MFVideoFormat_UYVY || subtype == MFVideoFormat_MJPG;
}
std::wstring format_name(const GUID& subtype)
{
    if (subtype == MFVideoFormat_RGB32) return L"RGB32"; if (subtype == MFVideoFormat_ARGB32) return L"ARGB32";
    if (subtype == MFVideoFormat_RGB24) return L"RGB24"; if (subtype == MFVideoFormat_P010) return L"P010";
    if (subtype == MFVideoFormat_NV12) return L"NV12"; if (subtype == MFVideoFormat_YUY2) return L"YUY2";
    if (subtype == MFVideoFormat_UYVY) return L"UYVY"; if (subtype == MFVideoFormat_MJPG) return L"MJPG";
    if (subtype == MFVideoFormat_H264) return L"H264"; if (subtype == MFVideoFormat_HEVC) return L"HEVC";
    const uint32_t code = subtype.Data1;
    wchar_t fourcc[5]{static_cast<wchar_t>(code & 0xff), static_cast<wchar_t>((code >> 8) & 0xff), static_cast<wchar_t>((code >> 16) & 0xff), static_cast<wchar_t>((code >> 24) & 0xff), 0};
    if (std::all_of(fourcc, fourcc + 4, [](wchar_t v) { return v >= 32 && v <= 126; })) return fourcc;
    wchar_t guid[64]{}; StringFromGUID2(subtype, guid, static_cast<int>(std::size(guid))); return guid;
}
}
CaptureEngine::CaptureEngine()
{
    throw_if_failed(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
    HRESULT wic_result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
    if (FAILED(wic_result))
        throw_if_failed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_)), "Create WIC factory");
}

CaptureEngine::~CaptureEngine()
{
    stop();
    MFShutdown();
}

std::vector<CaptureDevice> CaptureEngine::enumerate_devices()
{
    ComPtr<IMFAttributes> attributes;
    throw_if_failed(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    throw_if_failed(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "Set capture source type");

    IMFActivate** raw_devices = nullptr;
    UINT32 count = 0;
    throw_if_failed(MFEnumDeviceSources(attributes.Get(), &raw_devices, &count),
                    "MFEnumDeviceSources");

    std::vector<CaptureDevice> devices;
    devices.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* raw_name = nullptr;
        UINT32 name_length = 0;
        std::wstring name = L"Unnamed capture device";
        if (SUCCEEDED(raw_devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &raw_name, &name_length))) {
            name.assign(raw_name, name_length);
            CoTaskMemFree(raw_name);
        }
        CaptureDevice entry;
        entry.name = std::move(name);
        WCHAR* link = nullptr;
        UINT32 link_length = 0;
        if (SUCCEEDED(raw_devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                         &link, &link_length))) {
            entry.symbolic_link.assign(link, link_length);
            CoTaskMemFree(link);
        }
        entry.activation.Attach(raw_devices[i]);
        devices.push_back(std::move(entry));
    }
    CoTaskMemFree(raw_devices);
    return devices;
}


std::wstring CaptureMode::display_name() const
{
    wchar_t fps[32]{};
    const double value = frame_rate_denominator
        ? static_cast<double>(frame_rate_numerator) / frame_rate_denominator : 0.0;
    swprintf_s(fps, L"%.2f", value);
    return std::to_wstring(width) + L"x" + std::to_wstring(height) + L" @ " + fps +
           L" fps — " + format_name + (supported ? L"" : L" [unsupported]");
}

std::vector<CaptureMode> CaptureEngine::enumerate_modes(const CaptureDevice& device)
{
    if (!device.activation) throw std::runtime_error("Capture device is not available");
    ComPtr<IMFMediaSource> source;
    throw_if_failed(device.activation->ActivateObject(IID_PPV_ARGS(&source)),
                    "Activate capture device for mode enumeration");
    ComPtr<IMFAttributes> attributes;
    throw_if_failed(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);

    ComPtr<IMFSourceReader> reader;
    throw_if_failed(MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader),
                    "Create mode-enumeration reader");

    std::vector<CaptureMode> modes;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(video_stream, index, &type);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr))
            continue;
        CaptureMode mode;
        mode.native_index = index;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &mode.subtype)) ||
            FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &mode.width, &mode.height)))
            continue;
        mode.format_name = format_name(mode.subtype);
        mode.supported = is_supported_format(mode.subtype);        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &mode.frame_rate_numerator,
                            &mode.frame_rate_denominator);
        if (mode.frame_rate_denominator == 0)
            mode.frame_rate_denominator = 1;
        const bool duplicate = std::any_of(modes.begin(), modes.end(), [&](const CaptureMode& item) {
            return item.subtype == mode.subtype && item.width == mode.width &&
                   item.height == mode.height &&
                   static_cast<uint64_t>(item.frame_rate_numerator) * mode.frame_rate_denominator ==
                   static_cast<uint64_t>(mode.frame_rate_numerator) * item.frame_rate_denominator;
        });
        if (!duplicate)
            modes.push_back(std::move(mode));
    }
    reader.Reset();
    source->Shutdown();
    device.activation->ShutdownObject();
    return modes;
}
void CaptureEngine::start(const CaptureDevice& device, const CaptureMode& mode,
                          FrameCallback on_frame, ErrorCallback on_error)
{
    stop();
    path_=0;
    if (!mode.supported)
        throw std::runtime_error("The selected native format is listed but not yet decodable");

    ComPtr<IMFMediaSource> source;
    throw_if_failed(device.activation->ActivateObject(IID_PPV_ARGS(&source)),
                    "Activate capture device");

    ComPtr<IMFAttributes> attributes;
    throw_if_failed(MFCreateAttributes(&attributes, 3), "MFCreateAttributes");
    throw_if_failed(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this),
                    "Set source reader callback");
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    const bool gpu_reader_requested = gpu_device_ && gpu_requested_.load() &&
        (mode.subtype == MFVideoFormat_NV12 || mode.subtype == MFVideoFormat_P010 ||
         mode.subtype == MFVideoFormat_RGB32 || mode.subtype == MFVideoFormat_ARGB32);
    if (gpu_reader_requested) {
        UINT token{};
        if (!gpu_manager_ && SUCCEEDED(MFCreateDXGIDeviceManager(&token, &gpu_manager_))) {
            if (FAILED(gpu_manager_->ResetDevice(gpu_device_.Get(), token))) gpu_manager_.Reset();
        }
        if (gpu_manager_) attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu_manager_.Get());
    }

    ComPtr<IMFSourceReader> reader;
    HRESULT reader_result = MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader);
    if (FAILED(reader_result) && gpu_manager_ && gpu_reader_requested) {
        reader.Reset();
        attributes->DeleteItem(MF_SOURCE_READER_D3D_MANAGER);
        reader_result = MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader);
    }
    throw_if_failed(reader_result, "MFCreateSourceReaderFromMediaSource");
    ComPtr<IMFMediaType> selected;
    throw_if_failed(reader->GetNativeMediaType(video_stream, mode.native_index, &selected),
                    "Get selected native capture mode");
    throw_if_failed(reader->SetCurrentMediaType(video_stream, nullptr, selected.Get()),
                    "Set selected native capture mode");
    ComPtr<IMFMediaType> negotiated;
    throw_if_failed(reader->GetCurrentMediaType(video_stream,&negotiated),"Read capture color metadata");
    color_=CaptureColor::read(negotiated.Get(),mode.subtype==MFVideoFormat_RGB24 || mode.subtype==MFVideoFormat_RGB32 || mode.subtype==MFVideoFormat_ARGB32 || mode.subtype==MFVideoFormat_MJPG);
    if(color_.load().unsupported) {source->Shutdown();device.activation->ShutdownObject();throw std::runtime_error("HDR/BT.2020 or unsupported color range is not supported. Set the source to BT.601/709 SDR.");}

    PixelFormat selected_format = PixelFormat::bgra;
    if (mode.subtype == MFVideoFormat_RGB24) selected_format = PixelFormat::bgr24;
    else if (mode.subtype == MFVideoFormat_P010) selected_format = PixelFormat::p010;
    else if (mode.subtype == MFVideoFormat_NV12) selected_format = PixelFormat::nv12;
    else if (mode.subtype == MFVideoFormat_YUY2) selected_format = PixelFormat::yuy2;
    else if (mode.subtype == MFVideoFormat_UYVY) selected_format = PixelFormat::uyvy;
    else if (mode.subtype == MFVideoFormat_MJPG) selected_format = PixelFormat::mjpg;

    {
        std::scoped_lock lock(mutex_);
        reader_ = std::move(reader);
        source_ = std::move(source);
        activation_ = device.activation;
        on_frame_ = std::move(on_frame);
        on_error_ = std::move(on_error);
        width_ = mode.width;
        height_ = mode.height;
        pixel_format_ = selected_format;
        active_subtype_=mode.subtype;
        sequence_ = 0;
        running_ = true;
    }
    request_next();
}
void CaptureEngine::stop()
{
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFActivate> activation;
    {
        std::scoped_lock lock(mutex_);
        running_ = false;
        reader = std::move(reader_);
        source = std::move(source_);
        activation = std::move(activation_);
        on_frame_ = {};
        on_error_ = {};
    }
    if (reader) {
        reader->Flush(video_stream);
        reader.Reset();
    }
    if (source) {
        source->Shutdown();
        source.Reset();
    }
    if (activation)
        activation->ShutdownObject();
}
void CaptureEngine::request_next()
{
    ComPtr<IMFSourceReader> reader;
    {
        std::scoped_lock lock(mutex_);
        if (!running_)
            return;
        reader = reader_;
    }
    const HRESULT hr = reader->ReadSample(video_stream, 0,
                                          nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr))
        report_error(hr, L"ReadSample");
}

bool CaptureEngine::convert_to_bgra(PixelFormat format, const uint8_t* source, size_t length,
                                    uint32_t width, uint32_t height,
                                    std::vector<uint8_t>& output)
{
    const auto color=color_.load();
    const auto write_yuv_pixel=[color](uint8_t y,uint8_t u,uint8_t v,uint8_t* p){color.yuv(y,u,v,p);};
    if (!source || width == 0 || height == 0)
        return false;
    if (format == PixelFormat::mjpg) {
        if (!wic_factory_ || length > MAXDWORD)
            return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> decoded_frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic_factory_->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(source), static_cast<DWORD>(length))) ||
            FAILED(wic_factory_->CreateDecoderFromStream(stream.Get(), nullptr,
                                                         WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &decoded_frame)) ||
            FAILED(wic_factory_->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(decoded_frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            return false;
        output.resize(static_cast<size_t>(width) * height * 4);
        return SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
                                               static_cast<UINT>(output.size()), output.data()));
    }    output.resize(static_cast<size_t>(width) * height * 4);
    if (format == PixelFormat::bgra) {
        const size_t pitch = length / height;
        if (pitch < static_cast<size_t>(width) * 4)
            return false;
        for (uint32_t y = 0; y < height; ++y)
            memcpy(output.data() + static_cast<size_t>(y) * width * 4,
                   source + static_cast<size_t>(y) * pitch,
                   static_cast<size_t>(width) * 4);
        if(color.rgb && !color.full)
            for(size_t i=0;i<output.size();i++)if(i%4!=3)output[i]=clamp_byte((int(output[i])-16)*255/219);
        return true;
    }
    if (format == PixelFormat::bgr24) {
        const size_t pitch = length / height;
        if (pitch < static_cast<size_t>(width) * 3)
            return false;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = source + static_cast<size_t>(y) * pitch;
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t* target = output.data() + (static_cast<size_t>(y) * width + x) * 4;
                target[0] = row[static_cast<size_t>(x) * 3];
                target[1] = row[static_cast<size_t>(x) * 3 + 1];
                target[2] = row[static_cast<size_t>(x) * 3 + 2];
                target[3] = 255;
                if(color.rgb && !color.full)for(int c=0;c<3;c++)target[c]=clamp_byte((int(target[c])-16)*255/219);
            }
        }
        return true;
    }
    if (format == PixelFormat::p010) {
        const size_t pitch = (length * 2) / (static_cast<size_t>(height) * 3);
        if (pitch < static_cast<size_t>(width) * 2 || (pitch & 1u) != 0 ||
            length < pitch * height + pitch * ((height + 1) / 2))
            return false;
        const uint8_t* uv_bytes = source + pitch * height;
        for (uint32_t y = 0; y < height; ++y) {
            const auto* y_row = reinterpret_cast<const uint16_t*>(source + static_cast<size_t>(y) * pitch);
            const auto* uv_row = reinterpret_cast<const uint16_t*>(uv_bytes + static_cast<size_t>(y / 2) * pitch);
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t uv = x & ~1u;
                write_yuv_pixel(static_cast<uint8_t>(y_row[x] >> 8),
                                static_cast<uint8_t>(uv_row[uv] >> 8),
                                static_cast<uint8_t>(uv_row[uv + 1] >> 8),
                                output.data() + (static_cast<size_t>(y) * width + x) * 4);
            }
        }
        return true;
    }    if (format == PixelFormat::nv12) {
        const size_t pitch = (length * 2) / (static_cast<size_t>(height) * 3);
        if (pitch < width || length < pitch * height + pitch * ((height + 1) / 2))
            return false;
        const uint8_t* uv_plane = source + pitch * height;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t uv = static_cast<size_t>(y / 2) * pitch + (x & ~1u);
                write_yuv_pixel(source[static_cast<size_t>(y) * pitch + x],
                                uv_plane[uv], uv_plane[uv + 1],
                                output.data() + (static_cast<size_t>(y) * width + x) * 4);
            }
        }
        return true;
    }

    const size_t pitch = length / height;
    if (pitch < static_cast<size_t>(width) * 2 || (width & 1u) != 0)
        return false;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = source + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < width; x += 2) {
            const size_t i = static_cast<size_t>(x) * 2;
            const uint8_t y0 = format == PixelFormat::yuy2 ? row[i] : row[i + 1];
            const uint8_t u  = format == PixelFormat::yuy2 ? row[i + 1] : row[i];
            const uint8_t y1 = format == PixelFormat::yuy2 ? row[i + 2] : row[i + 3];
            const uint8_t v  = format == PixelFormat::yuy2 ? row[i + 3] : row[i + 2];
            write_yuv_pixel(y0, u, v, output.data() + (static_cast<size_t>(y) * width + x) * 4);
            write_yuv_pixel(y1, u, v, output.data() + (static_cast<size_t>(y) * width + x + 1) * 4);
        }
    }
    return true;
}

HRESULT CaptureEngine::OnReadSample(HRESULT status, DWORD, DWORD flags,
                                    LONGLONG timestamp, IMFSample* sample)
{
    if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED){
        ComPtr<IMFSourceReader> reader;{std::scoped_lock lock(mutex_);if(!running_)return S_OK;reader=reader_;}
        ComPtr<IMFMediaType> type;GUID subtype{};UINT w{},h{};
        if(!reader || FAILED(reader->GetCurrentMediaType(video_stream,&type)) || FAILED(type->GetGUID(MF_MT_SUBTYPE,&subtype))) {report_error(E_FAIL,L"Capture format changed");return S_OK;}
        color_=CaptureColor::read(type.Get(),subtype==MFVideoFormat_RGB24 || subtype==MFVideoFormat_RGB32 || subtype==MFVideoFormat_ARGB32 || subtype==MFVideoFormat_MJPG);
        MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h);
        bool changed=false;{std::scoped_lock lock(mutex_);changed=w!=width_ || h!=height_ || subtype!=active_subtype_;}
        if(color_.load().unsupported || changed){report_error(E_INVALIDARG,L"Capture changed to unsupported color or dimensions; select SDR mode");return S_OK;}
    }
    if (FAILED(status)) {
        report_error(status, L"Capture callback");
        return S_OK;
    }

    if ((flags & MF_SOURCE_READERF_STREAMTICK) == 0 && sample) {
        const uint64_t arrival = GetTickCount64();
        const uint64_t arrival_qpc=capture_qpc_100ns();
        unsigned next_path=!gpu_requested_.load()?2u:!gpu_allowed_.load()?3u:4u;
        if (gpu_device_ && gpu_requested_.load() && gpu_allowed_.load()) {
            std::scoped_lock gpu_lock(gpu_mutex_);
            ComPtr<IMFMediaBuffer> native;
            ComPtr<IMFDXGIBuffer> dxgi;
            ComPtr<ID3D11Texture2D> texture;
            UINT subresource{};
            VideoFrame frame;
            FrameCallback callback;
            frame.gpu_flipped=gpu_flip_.load();
            if (SUCCEEDED(sample->GetBufferByIndex(0, &native)) &&
                SUCCEEDED(native.As(&dxgi)) &&
                SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&texture))) &&
                SUCCEEDED(dxgi->GetSubresourceIndex(&subresource))) {
              next_path=5;
              if (gpu_converter_.convert(gpu_device_.Get(), texture.Get(), subresource,
                    frame.gpu, frame.bgra, frame.analysis_height,color_.load(),frame.gpu_flipped)) {
                {
                    std::scoped_lock lock(mutex_);
                    frame.width = width_; frame.height = height_;
                    frame.sequence = sequence_++; callback = on_frame_;
                }
                D3D11_TEXTURE2D_DESC converted{}; frame.gpu->texture->GetDesc(&converted);
                // Native coded surfaces may include padding beyond the negotiated image.
                // Do not copy mismatched dimensions into the renderer's resource.
                if (converted.Width == frame.width && converted.Height == frame.height) {
                    path_=1;
                    frame.timestamp_100ns = timestamp;
                    frame.arrival_tick_ms = arrival;
                    frame.arrival_qpc_100ns=arrival_qpc;
                    if (callback) { ++gpu_frames_; callback(std::move(frame)); }
                    request_next();
                    return S_OK;
                }
                next_path=6;
              }
            }
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
                FrameCallback callback;
                PixelFormat format{};
                VideoFrame frame;
                {
                    std::scoped_lock lock(mutex_);
                    frame.width = width_;
                    frame.height = height_;
                    frame.sequence = sequence_++;
                    format = pixel_format_;
                    callback = on_frame_;
                }
                frame.timestamp_100ns = timestamp;
                frame.arrival_tick_ms = arrival;
                frame.arrival_qpc_100ns=arrival_qpc;
                if (convert_to_bgra(format, bytes, length, frame.width, frame.height, frame.bgra) && callback) {
                    path_=next_path;
                    ++cpu_frames_;
                    callback(std::move(frame));
                }
                buffer->Unlock();
            }
        }
    }

    request_next();
    return S_OK;
}

void CaptureEngine::report_error(HRESULT hr, const wchar_t* operation)
{
    ErrorCallback callback;
    {
        std::scoped_lock lock(mutex_);
        callback = on_error_;
        running_ = false;
    }
    if (callback) {
        std::wostringstream message;
        message << operation << L" failed (HRESULT 0x" << std::hex
                << static_cast<unsigned long>(hr) << L")";
        callback(message.str());
    }
}

HRESULT CaptureEngine::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
        *object = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG CaptureEngine::AddRef() { return ++references_; }
ULONG CaptureEngine::Release()
{
    const ULONG value = --references_;
    // Lifetime is owned by the application, not COM.
    return value;
}
