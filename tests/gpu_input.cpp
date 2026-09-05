#include "shared_gpu_input.hpp"
#include "nr_adapter_api.hpp"
#include "blackout_probe.hpp"
#include <dxgi1_4.h>
#include <iostream>
#include <vector>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    try {
        ComPtr<IDXGIFactory4> factory;
        throw_if_failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create factory");
        ComPtr<IDXGIAdapter1> adapter;
        const bool nr = argc >= 2;
        const unsigned injection=argc>=3 ? static_cast<unsigned>(_wtoi(argv[2])) : 0;
        HMODULE bridge=nullptr;
        if (nr) {
            const auto bridge_path=std::filesystem::path(argv[1]).parent_path()/L"nr-runtime"/L"dlss5nr_bridge.dll";
            bridge=LoadLibraryExW(bridge_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!bridge) throw std::runtime_error("Load bridge for flow probe");
            auto inject=reinterpret_cast<int(__cdecl*)(unsigned)>(GetProcAddress(bridge,"dlss5nr_test_flow_failure"));
            if(!inject || !inject(injection)) throw std::runtime_error("Flow probe injection rejected");
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE) break;
                adapter.Reset();
            }
            if (!adapter) throw std::runtime_error("NVIDIA adapter required for NR probe");
        } else throw_if_failed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Get WARP");
        ComPtr<ID3D11Device> device11;
        ComPtr<ID3D11DeviceContext> context;
        throw_if_failed(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device11, nullptr, &context), "Create D3D11");
        ComPtr<ID3D12Device> device12;
        throw_if_failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device12)), "Create D3D12");
        if (nr) {
            for (int cycle = 0; cycle < 3; ++cycle) {
            HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) throw std::runtime_error("Load NR adapter");
            auto get = reinterpret_cast<NrAdapterGetApi>(GetProcAddress(module, "DlssNrAdapterGetApi"));
            const auto* api = get ? get(nr_adapter_abi_version) : nullptr;
            if (!api) throw std::runtime_error("NR adapter API unavailable");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 1920; desc.Height = 1080; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> texture;
            throw_if_failed(device11->CreateTexture2D(&desc, nullptr, &texture), "Create NR input");
            if (!api->initialize(device11.Get(), &desc)) {
                std::wcerr << api->last_error() << L'\n'; api->shutdown(); return 1;
            }
            std::vector<uint32_t> pixels(static_cast<size_t>(desc.Width) * desc.Height);
            for (uint32_t frame = 0; frame < 45; ++frame) {
                for (UINT y = 0; y < desc.Height; ++y)
                    for (UINT x = 0; x < desc.Width; ++x)
                        pixels[static_cast<size_t>(y) * desc.Width + x] = 0xff000000u |
                            (((x + frame * 4) % 256) << 16) | ((y % 256) << 8) | (x % 128);
                context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), desc.Width * 4, 0);
                TemporalAnalysisPayload temporal{}; temporal.flags = temporal_valid;
                temporal.frame_sequence = frame;
                if (frame == 15) temporal.flags |= temporal_scene_cut;
                if (frame >= 30) temporal.nr_temporal = 0; // Resource recreation path.
                if (!api->process(context.Get(), texture.Get(), &temporal)) {
                    std::wcerr << api->last_error() << L'\n'; api->shutdown(); return 1;
                }
                if (frame % 15 == 0 || frame == 44) {
                    NrTimingSnapshot timing{}; api->get_timings(&timing);
                    std::cout << "frame " << frame << " total_us=" << timing.total_us
                              << " input_us=" << timing.input_us << '\n';
                }
                if (argc>=3 && frame>=10 && frame<30) {
                    NrTimingSnapshot timing{}; api->get_timings(&timing);
                    if(timing.flow_mode!=(injection?3u:1u) || (injection && timing.flow_error==0)) throw std::runtime_error("Unexpected flow recovery mode");
                }
            }
            api->shutdown(); FreeLibrary(module);
            std::cout << "NR unload/reload cycle " << cycle + 1 << " complete\n";
            }
            std::cout << "NR shared input probe passed (synthetic frames, not visual quality verification)\n";
            FreeLibrary(bridge);
            return 0;
        }
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC q{};
        throw_if_failed(device12->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Create queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        throw_if_failed(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create allocator");
        ComPtr<ID3D12GraphicsCommandList> commands;
        throw_if_failed(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands)), "Create list");
        throw_if_failed(commands->Close(), "Close initial list");
        ComPtr<ID3D12Fence> fence;
        throw_if_failed(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
        UINT64 signal = 0;
        {
            BlackoutProbe probe;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 12; desc.Height = 12; desc.ArraySize = desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            ComPtr<ID3D11Texture2D> output;
            throw_if_failed(device11->CreateTexture2D(&desc, nullptr, &output), "Create probe fixture");
            for (UINT test = 0; test < 4; ++test) {
                std::vector<uint32_t> pixels(144, test & 1 ? 0xff000000 : 0xffffffff);
                std::vector<uint32_t> input(144, test & 2 ? 0xff000000 : 0xffffffff);
                context->UpdateSubresource(output.Get(), 0, nullptr, pixels.data(), 48, 0);
                probe.submit(device11.Get(), context.Get(), output.Get(),
                    reinterpret_cast<const uint8_t*>(input.data()), input.size() * 4, 12, 12, true, 1000 + test * 1000);
                context->Flush();
                std::optional<BlackoutSample> sample;
                const auto deadline = GetTickCount64() + 3000;
                while (!(sample = probe.poll(context.Get())) && GetTickCount64() < deadline) Sleep(1);
                if (!sample || sample->source_dark != !!(test & 2) || sample->output_dark != !!(test & 1) || !sample->nr_active)
                    throw std::runtime_error("Blackout probe classification or freshness failed");
            }
        }
        SharedGpuInput shared;
        for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16_SINT})
        for (const UINT width : {17u, 64u}) {
            const UINT height = 9;
            throw_if_failed(shared.create(device12.Get(), width, height,
                format == DXGI_FORMAT_R16G16_SINT ? DXGI_FORMAT_R16G16_TYPELESS : format), "Create shared input");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = format; desc.SampleDesc.Count = 1;
            ComPtr<ID3D11Texture2D> source;
            throw_if_failed(device11->CreateTexture2D(&desc, nullptr, &source), "Create source");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes{};
            const auto resource_desc = shared.resource()->GetDesc();
            device12->GetCopyableFootprints(&resource_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> readback;
            throw_if_failed(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create test readback");
            for (UINT frame = 0; frame < 16; ++frame) {
                std::vector<uint32_t> pixels(width * height);
                for (UINT i = 0; i < width * height; ++i) pixels[i] = 0xff000000u | (frame << 16) | i;
                context->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), width * 4, 0);
                // Exercise both queue-fence and bounded-query fallback paths.
                throw_if_failed(shared.copy_from(device12.Get(), device11.Get(), context.Get(), source.Get(),
                    frame % 2 ? queue.Get() : nullptr), "Shared GPU copy");
                if (frame % 2 && !shared.gpu_fence_available())
                    throw std::runtime_error("WARP shared fence path was not exercised");
                throw_if_failed(allocator->Reset(), "Reset allocator");
                throw_if_failed(commands->Reset(allocator.Get(), nullptr), "Reset list");
                D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = shared.resource();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                commands->ResourceBarrier(1, &barrier);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = shared.resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprint;
                commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                commands->ResourceBarrier(1, &barrier);
                throw_if_failed(commands->Close(), "Close list");
                ID3D12CommandList* lists[] = {commands.Get()}; queue->ExecuteCommandLists(1, lists);
                throw_if_failed(queue->Signal(fence.Get(), ++signal), "Signal");
                const auto deadline = GetTickCount64() + 5000;
                while (fence->GetCompletedValue() < signal && GetTickCount64() < deadline) Sleep(1);
                if (fence->GetCompletedValue() != signal) throw std::runtime_error("GPU test timeout/device loss");
                void* data{}; throw_if_failed(readback->Map(0, nullptr, &data), "Read test result");
                bool matches = true;
                for (UINT y = 0; y < height; ++y) {
                    const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + y * footprint.Footprint.RowPitch);
                    for (UINT x = 0; x < width; ++x) matches &= row[x] == pixels[y * width + x];
                }
                readback->Unmap(0, nullptr);
                if (!matches) throw std::runtime_error("Shared input pixels differ or are stale");
            }
        }
        std::cout << "shared GPU input checks passed (WARP, repeated frames and resize)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
