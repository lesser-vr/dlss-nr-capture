#include "common.hpp"
#include "nr_adapter_api.hpp"
#include "benchmark_stats.hpp"
#include "frame_processor.hpp"
#include "quality_capture.hpp"
#include "capture_replay.hpp"
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr DWORD video_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
static uint64_t us(Clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}
static std::string json(const std::wstring& value) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), bytes.data(), n, nullptr, nullptr);
    std::string out = "\"";
    for (unsigned char c : bytes) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) out += " ";
        else out += static_cast<char>(c);
    }
    return out + "\"";
}
struct Runtime {
    HMODULE module{};
    HMODULE probe_bridge{};
    const NrAdapterApi* api{};
    ~Runtime() {
        if (api) { std::cerr << "Shutting down adapter...\n"; api->shutdown(); }
        if (module) { std::cerr << "Unloading adapter...\n"; FreeLibrary(module); }
        if (probe_bridge) FreeLibrary(probe_bridge);
        std::cerr << "Adapter released.\n";
    }
};
struct MediaRuntime {
    MediaRuntime() { throw_if_failed(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); throw_if_failed(MFStartup(MF_VERSION), "MF startup"); }
    ~MediaRuntime() { std::cerr << "Shutting down media...\n"; MFShutdown(); CoUninitialize(); std::cerr << "Media released.\n"; }
};

int wmain(int argc, wchar_t** argv) {
    try {
        std::map<std::wstring, std::wstring> args;
        for (int i = 1; i < argc; ++i) {
            const std::wstring key = argv[i];
            if (key == L"--help") {
                std::cout << "--input VIDEO (or --synthetic) --output NEW_DIRECTORY [--adapter DLL]\n"
                    "[--frames 300] [--warmup 120] [--style 1] [--preset 3] [--intensity 100] [--temporal 1] [--warp] [--capture-output]\n"
                    "[--capture-format BGRA|NV12|P010] [--gpu-capture]\n"
                    "Offline sequential benchmark; no capture/presentation, no real-time drop or fallback measurement.\n";
                std::cout << "--full-resolution or --quality-width/--quality-height; optional --quality-x/--quality-y/--quality-region-width/--quality-region-height\n";
                std::cout << "--gpu-capture replays decoded BGRA through native conversion/analysis (not hardware capture); --test-flow-failure 0..3 is an isolated recovery probe\n";
                return 0;
            }
            if (key == L"--synthetic" || key == L"--warp" || key == L"--capture-output" || key == L"--full-resolution" || key == L"--gpu-capture") args[key] = L"1";
            else if (key == L"--input" || key == L"--output" || key == L"--adapter" || key == L"--frames" ||
                     key == L"--warmup" || key == L"--style" || key == L"--preset" || key == L"--intensity" || key == L"--temporal" ||
                     key == L"--quality-width" || key == L"--quality-height" || key == L"--quality-x" || key == L"--quality-y" ||
                     key == L"--quality-region-width" || key == L"--quality-region-height" || key == L"--test-flow-failure" || key==L"--capture-format") {
                if (++i == argc) throw std::runtime_error("Missing option value");
                args[key] = argv[i];
            } else throw std::runtime_error("Unknown option (see --help)");
        }
        const auto number = [&](const wchar_t* key, unsigned fallback, unsigned lo, unsigned hi) {
            if (!args.count(key)) return fallback;
            size_t used{}; const auto value = std::stoul(args.at(key), &used);
            if (used != args.at(key).size() || value < lo || value > hi) throw std::runtime_error("Numeric option out of range");
            return static_cast<unsigned>(value);
        };
        const unsigned frames = number(L"--frames", 300, 1, 1000000), warmup = number(L"--warmup", 120, 0, 1000000);
        const unsigned style = number(L"--style", 1, 0, 3), preset = number(L"--preset", 3, 1, 4);
        const unsigned intensity = number(L"--intensity", 100, 25, 100), temporal = number(L"--temporal", 1, 0, 1);
        const unsigned flow_failure=number(L"--test-flow-failure",0,0,3);
        const bool gpu_capture=args.count(L"--gpu-capture")!=0;
        const auto capture_format=args.count(L"--capture-format")?args.at(L"--capture-format"):L"BGRA";
        if(capture_format!=L"BGRA" && capture_format!=L"NV12" && capture_format!=L"P010")throw std::runtime_error("Capture replay format must be BGRA/NV12/P010");
        if (!args.count(L"--output") || (args.count(L"--input") + args.count(L"--synthetic") != 1))
            throw std::runtime_error("Specify --output and exactly one of --input / --synthetic");
        const fs::path output = fs::absolute(args.at(L"--output"));
        if (fs::exists(output)) throw std::runtime_error("Output directory already exists; use a new path");
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        const fs::path adapter_path = args.count(L"--adapter") ? fs::absolute(args.at(L"--adapter"))
            : fs::path(executable).parent_path() / L"dlss-nr-adapter-bridge.dll";
        MediaRuntime media;
        UINT width = 1280, height = 720, fps_n = 60, fps_d = 1;
        LONG stride = static_cast<LONG>(width * 4);
        ComPtr<IMFSourceReader> reader;
        if (args.count(L"--input")) {
            const fs::path input = fs::absolute(args.at(L"--input"));
            if (!fs::is_regular_file(input)) throw std::runtime_error("Input must be a local video file");
            ComPtr<IMFAttributes> attributes;
            throw_if_failed(MFCreateAttributes(&attributes, 1), "Create reader attributes");
            throw_if_failed(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "Enable RGB conversion");
            throw_if_failed(MFCreateSourceReaderFromURL(input.c_str(), attributes.Get(), &reader), "Open video");
            throw_if_failed(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Disable streams");
            throw_if_failed(reader->SetStreamSelection(video_stream, TRUE), "Select video");
            ComPtr<IMFMediaType> type;
            throw_if_failed(MFCreateMediaType(&type), "Create RGB type");
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            throw_if_failed(reader->SetCurrentMediaType(video_stream, nullptr, type.Get()), "Decode RGB32");
            throw_if_failed(reader->GetCurrentMediaType(video_stream, &type), "Get decoded format");
            throw_if_failed(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "Read dimensions");
            throw_if_failed(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &fps_n, &fps_d), "Read FPS");
            UINT32 raw_stride{};
            if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) stride = static_cast<LONG>(raw_stride);
            else throw_if_failed(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride), "RGB stride");
        }
        if (!width || !height || width > 8192 || height > 8192 || !fps_n || !fps_d) throw std::runtime_error("Invalid video format");
        ComPtr<IDXGIFactory4> factory; throw_if_failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI");
        ComPtr<IDXGIAdapter1> gpu;
        if (args.count(L"--warp")) throw_if_failed(factory->EnumWarpAdapter(IID_PPV_ARGS(&gpu)), "WARP");
        else {
            for (UINT i = 0; factory->EnumAdapters1(i, &gpu) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{}; gpu->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE) break;
                gpu.Reset();
            }
            if (!gpu) throw std::runtime_error("NVIDIA GPU not found");
        }
        DXGI_ADAPTER_DESC1 gpu_desc{}; gpu->GetDesc1(&gpu_desc);
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        throw_if_failed(D3D11CreateDevice(gpu.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11 device");
        const UINT qx = number(L"--quality-x", 0, 0, width - 1), qy = number(L"--quality-y", 0, 0, height - 1);
        const UINT rw = number(L"--quality-region-width", width - qx, 1, width - qx);
        const UINT rh = number(L"--quality-region-height", height - qy, 1, height - qy);
        const UINT qw = args.count(L"--full-resolution") ? rw : number(L"--quality-width", quality_width, 1, 8192);
        const UINT qh = args.count(L"--full-resolution") ? rh : number(L"--quality-height", quality_height, 1, 8192);
        if (args.count(L"--capture-output")) {
            auto ancestor = output.parent_path();
            while (!fs::exists(ancestor) && ancestor != ancestor.parent_path()) ancestor = ancestor.parent_path();
            const uint64_t required = static_cast<uint64_t>(qw) * qh * 6 * frames;
            if (required + 16 * 1024 * 1024 > fs::space(ancestor).available)
                throw std::runtime_error("Insufficient disk space for requested quality capture");
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture; throw_if_failed(device->CreateTexture2D(&desc, nullptr, &texture), "Input texture");
        ComPtr<ID3D11Texture2D> planar;
        if(capture_format!=L"BGRA" && gpu_capture){auto d=desc;d.Format=capture_format==L"P010"?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;throw_if_failed(device->CreateTexture2D(&d,nullptr,&planar),"Planar replay texture");}
        Runtime runtime;
        if (args.count(L"--test-flow-failure")) {
            const auto bridge=adapter_path.parent_path()/L"nr-runtime"/L"dlss5nr_bridge.dll";
            runtime.probe_bridge=LoadLibraryExW(bridge.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            auto inject=runtime.probe_bridge?reinterpret_cast<int(__cdecl*)(unsigned)>(GetProcAddress(runtime.probe_bridge,"dlss5nr_test_flow_failure")):nullptr;
            if (!inject || !inject(flow_failure)) throw std::runtime_error("Flow fault injection unavailable");
        }
        runtime.module = LoadLibraryExW(adapter_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!runtime.module) throw std::runtime_error("Cannot load adapter DLL");
        auto get = reinterpret_cast<NrAdapterGetApi>(GetProcAddress(runtime.module, "DlssNrAdapterGetApi"));
        runtime.api = get ? get(nr_adapter_abi_version) : nullptr;
        if (!runtime.api) throw std::runtime_error("Adapter ABI mismatch");
        if (!runtime.api->initialize(device.Get(), &desc)) { std::wcerr << runtime.api->last_error() << '\n'; return 1; }
        if (!fs::create_directories(output)) throw std::runtime_error("Cannot create output directory");
        std::ofstream csv(output / L"frames.csv"); csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "frame,timestamp_100ns,warmup,success,decode_us,analysis_us,upload_us,process_wall_us,pipeline_us,nr_total_us,input_us,setup_us,flow_us,mv_us,prepare_us,execute_us,bridge_output_us,correction_output_us,flow_mode,flow_error\n";
        auto analyzer = create_motion_analysis_processor();
        GpuCaptureConverter capture_converter;
        const bool capture_output = args.count(L"--capture-output") != 0;
        QualityCapture quality;
        std::ofstream inputs, outputs;
        if (capture_output) {
            quality.initialize(device.Get(), desc, qw, qh, qx, qy, rw, rh);
            inputs.open(output / L"input.rgb", std::ios::binary);
            outputs.open(output / L"output.rgb", std::ios::binary);
            inputs.exceptions(std::ios::badbit | std::ios::failbit);
            outputs.exceptions(std::ios::badbit | std::ios::failbit);
        }
        std::vector<uint64_t> process_times, pipeline_times;
        unsigned decoded = 0, failures = 0, over_budget = 0; bool eof = false;
        const double budget_us = 1000000.0 * fps_d / fps_n;
        std::wstring error;
        for (; decoded < warmup + frames; ++decoded) {
            const auto start = Clock::now();
            VideoFrame frame{}; frame.width = width; frame.height = height; frame.sequence = decoded;
            frame.bgra.resize(static_cast<size_t>(width) * height * 4);
            if (reader) {
                ComPtr<IMFSample> sample;
                for (unsigned ticks = 0; !sample; ++ticks) {
                    if (ticks > 1000) throw std::runtime_error("Too many empty decoder samples");
                    DWORD flags{};
                    throw_if_failed(reader->ReadSample(video_stream, 0, nullptr, &flags, &frame.timestamp_100ns, &sample), "Read video frame");
                    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                        // H.264 decoders can publish a refined stride/type on their first sample.
                        ComPtr<IMFMediaType> current;
                        throw_if_failed(reader->GetCurrentMediaType(video_stream, &current), "Read updated format");
                        UINT w{}, h{}, n{}, d{}; GUID subtype{};
                        throw_if_failed(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h), "Updated dimensions");
                        throw_if_failed(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &n, &d), "Updated FPS");
                        throw_if_failed(current->GetGUID(MF_MT_SUBTYPE, &subtype), "Updated pixel format");
                        if (w != width || h != height || subtype != MFVideoFormat_RGB32 ||
                            static_cast<uint64_t>(n) * fps_d != static_cast<uint64_t>(fps_n) * d)
                            throw std::runtime_error("Mid-file resolution, FPS or pixel format changes are not supported");
                        UINT32 raw_stride{};
                        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) stride = static_cast<LONG>(raw_stride);
                    }
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eof = true; break; }
                }
                if (eof) break;
                ComPtr<IMFMediaBuffer> buffer; throw_if_failed(sample->ConvertToContiguousBuffer(&buffer), "Video buffer");
                ComPtr<IMF2DBuffer> two;
                if (SUCCEEDED(buffer.As(&two))) {
                    BYTE* top{}; LONG pitch{}; throw_if_failed(two->Lock2D(&top, &pitch), "Lock RGB plane");
                    for (UINT y = 0; y < height; ++y) std::memcpy(frame.bgra.data() + static_cast<size_t>(y) * width * 4, top + static_cast<ptrdiff_t>(y) * pitch, width * 4);
                    two->Unlock2D();
                } else {
                    BYTE* data{}; DWORD length{}; throw_if_failed(buffer->Lock(&data, nullptr, &length), "Lock RGB buffer");
                    const size_t pitch = static_cast<size_t>(std::abs(static_cast<int64_t>(stride)));
                    if (pitch < width * 4 || length < pitch * height) { buffer->Unlock(); throw std::runtime_error("Short RGB frame"); }
                    const BYTE* top = stride < 0 ? data + pitch * (height - 1) : data;
                    for (UINT y = 0; y < height; ++y) std::memcpy(frame.bgra.data() + static_cast<size_t>(y) * width * 4, top + static_cast<ptrdiff_t>(y) * stride, width * 4);
                    buffer->Unlock();
                }
            } else {
                frame.timestamp_100ns = static_cast<int64_t>(decoded) * 10000000 * fps_d / fps_n;
                for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                    const size_t p = (static_cast<size_t>(y) * width + x) * 4;
                    frame.bgra[p] = static_cast<uint8_t>(x); frame.bgra[p+1] = static_cast<uint8_t>(y);
                    frame.bgra[p+2] = static_cast<uint8_t>(x + decoded * 4); frame.bgra[p+3] = 255;
                }
            }
            const auto decode_us = us(start);
            std::vector<uint8_t> planar_bytes;
            if(capture_format!=L"BGRA")planar_bytes=planar_replay(frame.bgra,width,height,capture_format==L"P010");
            std::vector<uint8_t> original;
            uint64_t conversion_us=0;
            if (gpu_capture) {
                const auto conversion_start=Clock::now();
                context->UpdateSubresource(texture.Get(),0,nullptr,frame.bgra.data(),width*4,0);
                if(planar)context->UpdateSubresource(planar.Get(),0,nullptr,planar_bytes.data(),width*(capture_format==L"P010"?2:1),0);
                original=std::move(frame.bgra);
                if (!capture_converter.convert(device.Get(),planar?planar.Get():texture.Get(),0,frame.gpu,frame.bgra,frame.analysis_height))
                    throw std::runtime_error("GPU capture conversion unavailable (no silent CPU fallback in comparison)");
                conversion_us=us(conversion_start);
            }
            const auto analysis_start = Clock::now();
            analyzer->process(frame); auto payload = analyzer->temporal_state();
            payload.nr_style = static_cast<uint16_t>(style); payload.nr_preset = static_cast<uint16_t>(preset);
            payload.nr_intensity_percent = static_cast<uint16_t>(intensity); payload.nr_temporal = static_cast<uint8_t>(temporal);
            const auto analysis_us = us(analysis_start);
            const auto upload_start = Clock::now();
            if (gpu_capture) context->CopyResource(texture.Get(),frame.gpu->texture.Get());
            else context->UpdateSubresource(texture.Get(), 0, nullptr, frame.bgra.data(), width * 4, 0);
            const auto upload_us = us(upload_start)+conversion_us;
            const auto process_start = Clock::now();
            const bool ok = runtime.api->process(context.Get(), texture.Get(), &payload);
            const auto process_us = us(process_start), pipeline_us = us(start);
            NrTimingSnapshot timing{}; if (ok) runtime.api->get_timings(&timing);
            csv << decoded << ',' << frame.timestamp_100ns << ',' << (decoded < warmup) << ',' << ok << ','
                << decode_us << ',' << analysis_us << ',' << upload_us << ',' << process_us << ',' << pipeline_us << ','
                << timing.total_us << ',' << timing.input_us << ',' << timing.setup_us << ',' << timing.optical_flow_us << ','
                << timing.motion_vector_us << ',' << timing.gpu_prepare_us << ',' << timing.gpu_execute_us << ','
                << timing.bridge_output_us << ',' << timing.correction_output_us << ',' << timing.flow_mode << ',' << timing.flow_error << '\n';
            if (!ok) { ++failures; error = runtime.api->last_error(); ++decoded; break; }
            if (decoded >= warmup) {
                if (capture_output) quality.write(context.Get(), texture.Get(), gpu_capture?original.data():frame.bgra.data(), width, height, inputs, outputs);
                process_times.push_back(process_us); pipeline_times.push_back(pipeline_us);
                if (process_us > budget_us) ++over_budget;
            }
        }
        csv.close();
        if (capture_output) { inputs.close(); outputs.close(); }
        const bool complete = process_times.size() == frames && !failures;
        const auto stats = benchmark_stats(process_times), pipeline = benchmark_stats(pipeline_times);
        std::ofstream report(output / L"summary.json"); report.exceptions(std::ios::badbit | std::ios::failbit);
        report << std::fixed << std::setprecision(3) << "{\n  \"schema_version\": 1,\n  \"status\": " << (complete ? "\"complete\"" : "\"incomplete\"")
            << ",\n  \"mode\": \"offline_sequential\",\n  \"input\": " << json(reader ? fs::absolute(args.at(L"--input")).wstring() : L"synthetic-v1")
            << ",\n  \"adapter\": " << json(adapter_path.wstring()) << ",\n  \"adapter_name\": " << json(runtime.api->display_name)
            << ",\n  \"gpu\": " << json(gpu_desc.Description) << ",\n  \"width\": " << width << ", \"height\": " << height
            << ",\n  \"fps_numerator\": " << fps_n << ", \"fps_denominator\": " << fps_d
            << ",\n  \"style\": " << style << ", \"preset\": " << preset << ", \"intensity\": " << intensity << ", \"temporal\": " << temporal
            << ",\n  \"warmup_requested\": " << warmup << ", \"frames_requested\": " << frames << ", \"frames_decoded\": " << decoded
            << ",\n  \"measured_frames\": " << process_times.size() << ", \"failures\": " << failures << ", \"eof\": " << (eof ? "true" : "false")
            << ",\n  \"process_mean_us\": " << stats.mean << ", \"process_p95_us\": " << stats.p95 << ", \"process_p99_us\": " << stats.p99
            << ", \"process_max_us\": " << stats.maximum << ",\n  \"processing_capacity_fps\": " << (stats.mean > 0 ? 1000000.0 / stats.mean : 0)
            << ",\n  \"pipeline_capacity_fps\": " << (pipeline.mean > 0 ? 1000000.0 / pipeline.mean : 0)
            << ",\n  \"over_source_frame_budget\": " << over_budget
            << ",\n  \"capture_output\": " << (capture_output ? "true" : "false")
            << ",\n  \"gpu_capture_replay\": " << (gpu_capture ? "true" : "false")
            << ",\n  \"capture_replay_format\": " << json(capture_format)
            << ",\n  \"flow_failure_injection\": " << flow_failure
            << ", \"performance_comparable\": " << (capture_output ? "false" : "true")
            << ",\n  \"proxy_format\": \"rgb24-nearest-v1\", \"proxy_width\": " << qw << ", \"proxy_height\": " << qh
            << ",\n  \"quality_region_x\": " << qx << ", \"quality_region_y\": " << qy
            << ", \"quality_region_width\": " << rw << ", \"quality_region_height\": " << rh
            << ",\n  \"live_drop_rate\": null, \"live_fallback_rate\": null, \"quality_score\": null,\n  \"error\": " << json(error) << "\n}\n";
        report.close();
        reader.Reset();
        std::wcout << L"Report: " << output.wstring() << L"\n";
        return complete ? 0 : 2;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
