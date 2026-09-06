cmake_minimum_required(VERSION 3.24)
project(DlssNrCapture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dlss-nr-capture WIN32
    src/main.cpp
    src/app.manifest
    src/app.cpp
    src/audio_capture.cpp
    src/capture_engine.cpp
    src/d3d11_renderer.cpp
    src/frame_processor.cpp
)

target_include_directories(dlss-nr-capture PRIVATE src)
target_compile_definitions(dlss-nr-capture PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_compile_options(dlss-nr-capture PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(dlss-nr-capture PRIVATE
    d3d11
    d3dcompiler
    d2d1
    dwrite
    dxgi
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    shlwapi
    windowscodecs
    winmm
    advapi32
)


add_library(dlss-nr-adapter-bridge SHARED src/dlss5nr_bridge_adapter.cpp)
target_include_directories(dlss-nr-adapter-bridge PRIVATE src)
target_compile_definitions(dlss-nr-adapter-bridge PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dlss-nr-adapter-bridge PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(dlss-nr-adapter-bridge PRIVATE d3d11 dxgi)
set_target_properties(dlss-nr-adapter-bridge PROPERTIES OUTPUT_NAME "dlss-nr-adapter-bridge")

# MIT-licensed public bridge components. NVIDIA runtime binaries are intentionally
# not part of this target or repository and must be supplied separately by users.
add_library(dlss5nr-runtime-bridge SHARED
    third_party/comfyui_dlss5_nr/dlss5nr_bridge.cpp
    third_party/comfyui_dlss5_nr/nvof_flow.cpp
)
target_include_directories(dlss5nr-runtime-bridge PRIVATE third_party/comfyui_dlss5_nr src)
option(DLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT "Use experimental GPU-queue input handoff" OFF)
option(DLSS_NR_EXPERIMENTAL_GPU_FLOW "GPU coarse-flow handoff with automatic CPU-transfer recovery" ON)
if(DLSS_NR_EXPERIMENTAL_GPU_FLOW)
    target_compile_definitions(dlss5nr-runtime-bridge PRIVATE DLSS_NR_EXPERIMENTAL_GPU_FLOW=1)
endif()
if(DLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT)
    target_compile_definitions(dlss5nr-runtime-bridge PRIVATE DLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT=1)
endif()
target_compile_options(dlss5nr-runtime-bridge PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(dlss5nr-runtime-bridge PRIVATE d3d12 d3d11 d3dcompiler dxgi ole32)
set_target_properties(dlss5nr-runtime-bridge PROPERTIES
    OUTPUT_NAME "dlss5nr_bridge"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/nr-runtime"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/nr-runtime"
)

add_library(dlss5nr-caller-shim SHARED third_party/comfyui_dlss5_nr/caller_shim.cpp)
target_compile_options(dlss5nr-caller-shim PRIVATE /W4 /permissive- /EHsc /utf-8)
set_target_properties(dlss5nr-caller-shim PROPERTIES
    OUTPUT_NAME "nvngx.dll_comfy"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/nr-runtime/caller"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/nr-runtime/caller"
)

add_library(dlss-nr-adapter-sample SHARED src/sample_adapter.cpp)
target_include_directories(dlss-nr-adapter-sample PRIVATE src)
target_compile_definitions(dlss-nr-adapter-sample PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dlss-nr-adapter-sample PRIVATE /W4 /permissive- /EHsc /utf-8)
set_target_properties(dlss-nr-adapter-sample PROPERTIES OUTPUT_NAME "dlss-nr-adapter-sample")

add_executable(dlss-nr-worker src/worker_main.cpp)
target_compile_definitions(dlss-nr-worker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dlss-nr-worker PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(dlss-nr-worker PRIVATE d3d11 dxgi d3dcompiler)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT dlss-nr-capture)


include(CTest)
add_executable(dlss-nr-quality-compare src/quality_compare_main.cpp)
target_include_directories(dlss-nr-quality-compare PRIVATE src)
target_compile_options(dlss-nr-quality-compare PRIVATE /W4 /permissive- /EHsc /utf-8)
add_executable(dlss-nr-benchmark src/benchmark_main.cpp src/frame_processor.cpp)
target_include_directories(dlss-nr-benchmark PRIVATE src)
target_compile_definitions(dlss-nr-benchmark PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dlss-nr-benchmark PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(dlss-nr-benchmark PRIVATE d3d11 d3dcompiler dxgi mfplat mfreadwrite mfuuid ole32)
if(BUILD_TESTING)
    add_executable(dlss-nr-composition-test tests/nr_composition.cpp)
    target_include_directories(dlss-nr-composition-test PRIVATE src)
    target_compile_definitions(dlss-nr-composition-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-composition-test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(dlss-nr-composition-test PRIVATE d3d11 d3dcompiler)
    add_test(NAME regression.nr-composition COMMAND dlss-nr-composition-test)
    set_tests_properties(regression.nr-composition PROPERTIES TIMEOUT 30)
    add_executable(dlss-nr-copy-timing tests/copy_timing.cpp)
    target_include_directories(dlss-nr-copy-timing PRIVATE src)
    target_compile_definitions(dlss-nr-copy-timing PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(dlss-nr-copy-timing PRIVATE d3d11)
    # Manual enumeration probe: never silently changes the mode under test.
    add_executable(dlss-nr-capture-modes tests/capture_modes.cpp src/capture_engine.cpp)
    target_include_directories(dlss-nr-capture-modes PRIVATE src)
    target_compile_definitions(dlss-nr-capture-modes PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-capture-modes PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(dlss-nr-capture-modes PRIVATE d3d11 mf mfplat mfreadwrite mfuuid ole32 windowscodecs)
    add_executable(dlss-nr-worker-pipeline-test tests/worker_pipeline.cpp)
    target_include_directories(dlss-nr-worker-pipeline-test PRIVATE src)
    target_compile_definitions(dlss-nr-worker-pipeline-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-worker-pipeline-test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(dlss-nr-worker-pipeline-test PRIVATE d3d11 dxgi)
    add_test(NAME regression.worker-pipeline COMMAND dlss-nr-worker-pipeline-test $<TARGET_FILE:dlss-nr-worker>)
    set_tests_properties(regression.worker-pipeline PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_test(NAME regression.worker-pipeline-warp COMMAND dlss-nr-worker-pipeline-test $<TARGET_FILE:dlss-nr-worker> --warp)
    set_tests_properties(regression.worker-pipeline-warp PROPERTIES TIMEOUT 30)
    add_library(dlss-nr-adapter-failing SHARED tests/failing_adapter.cpp)
    target_include_directories(dlss-nr-adapter-failing PRIVATE src)
    target_compile_definitions(dlss-nr-adapter-failing PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-adapter-failing PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME regression.two-pass-fallback COMMAND dlss-nr-worker-pipeline-test
        $<TARGET_FILE:dlss-nr-worker> --failure $<TARGET_FILE:dlss-nr-adapter-failing>)
    set_tests_properties(regression.two-pass-fallback PROPERTIES TIMEOUT 30)
    add_executable(dlss-nr-gpu-capture-test tests/gpu_capture.cpp src/frame_processor.cpp)
    target_include_directories(dlss-nr-gpu-capture-test PRIVATE src)
    target_compile_definitions(dlss-nr-gpu-capture-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-gpu-capture-test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(dlss-nr-gpu-capture-test PRIVATE d3d11 mfplat mfuuid)
    add_test(NAME regression.gpu-capture COMMAND dlss-nr-gpu-capture-test)
    set_tests_properties(regression.gpu-capture PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_test(NAME regression.tools COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tests/tools_smoke.ps1" -SourceDir "${CMAKE_SOURCE_DIR}")
    set_tests_properties(regression.tools PROPERTIES TIMEOUT 30)
    find_program(BENCHMARK_FFMPEG ffmpeg)
    add_test(NAME regression.benchmark
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/benchmark_smoke.ps1"
            -BenchmarkPath "$<TARGET_FILE:dlss-nr-benchmark>"
            -AdapterPath "$<TARGET_FILE:dlss-nr-adapter-sample>"
            -ComparePath "$<TARGET_FILE:dlss-nr-quality-compare>"
            -FfmpegPath "${BENCHMARK_FFMPEG}")
    set_tests_properties(regression.benchmark PROPERTIES TIMEOUT 30)
    add_executable(dlss-nr-gpu-input-test tests/gpu_input.cpp)
    target_include_directories(dlss-nr-gpu-input-test PRIVATE src)
    target_compile_definitions(dlss-nr-gpu-input-test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-gpu-input-test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(dlss-nr-gpu-input-test PRIVATE d3d11 d3d12 dxgi)
    add_test(NAME regression.gpu-input COMMAND dlss-nr-gpu-input-test)
    set_tests_properties(regression.gpu-input PROPERTIES TIMEOUT 30)
    add_executable(dlss-nr-regression-core
        tests/regression_core.cpp
        src/frame_processor.cpp
    )
    target_include_directories(dlss-nr-regression-core PRIVATE src)
    target_compile_definitions(dlss-nr-regression-core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dlss-nr-regression-core PRIVATE /W4 /permissive- /EHsc /utf-8)

    add_test(NAME regression.core
        COMMAND dlss-nr-regression-core
            $<TARGET_FILE:dlss-nr-adapter-sample>
            $<TARGET_FILE:dlss5nr-runtime-bridge>)
    add_test(NAME regression.artifacts
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/artifact_layout.ps1"
            -ReleaseDir "$<TARGET_FILE_DIR:dlss-nr-capture>"
            -SourceDir "${CMAKE_SOURCE_DIR}")
    add_test(NAME regression.app-smoke
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/app_smoke.ps1"
            -AppPath "$<TARGET_FILE:dlss-nr-capture>")
    set_tests_properties(regression.app-smoke PROPERTIES
        TIMEOUT 45
        RUN_SERIAL TRUE)

    add_custom_target(regression
        COMMAND "${CMAKE_CTEST_COMMAND}" -C $<CONFIG> --output-on-failure
        DEPENDS dlss-nr-capture dlss-nr-worker dlss-nr-adapter-sample dlss-nr-adapter-failing
                dlss-nr-composition-test
                dlss-nr-adapter-bridge dlss5nr-runtime-bridge dlss5nr-caller-shim
                dlss-nr-regression-core dlss-nr-gpu-input-test dlss-nr-gpu-capture-test dlss-nr-worker-pipeline-test dlss-nr-benchmark dlss-nr-quality-compare
        USES_TERMINAL)
endif()
