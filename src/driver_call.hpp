#pragma once
#include "common.hpp"
#include <future>
#include <thread>
#include <type_traits>

// Device calls stay serialized. Only window-management messages are serviced
inline thread_local bool driver_call_active = false;
// while waiting; commands, timers and frame messages cannot reenter app state.
template<class Call>
auto driver_call(HWND window, Call call) -> std::invoke_result_t<Call> {
    using Result = std::invoke_result_t<Call>;
    struct Busy {
        Busy() { driver_call_active = true; }
        ~Busy() { driver_call_active = false; }
    } busy;
    std::packaged_task<Result()> task([call = std::move(call)]() mutable -> Result {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        throw_if_failed(hr, "Initialize driver thread");
        struct ComExit { ~ComExit() { CoUninitialize(); } } cleanup;
        return call();
    });
    auto result = task.get_future();
    std::jthread worker(std::move(task));
    const ULONGLONG started = GetTickCount64();
    const bool closing = !window || !IsWindow(window);
    wchar_t old_title[4096]{};
    if (!closing) GetWindowTextW(window, old_title, 4096);
    bool notified = false;
    while (result.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
        if (closing && GetTickCount64() - started >= 5000) {
            // Application shutdown was already requested. Do not detach a
            // thread holding references into a destroyed App.
            TerminateProcess(GetCurrentProcess(), 0);
        }
        if (!closing && !notified && GetTickCount64() - started >= 2000) {
            SetWindowTextW(window, L"Waiting for device driver - close window to terminate");
            notified = true;
        }
        MSG msg{};
        while (!closing && PeekMessageW(&msg, nullptr, 0, WM_APP - 1, PM_REMOVE)) {
            if (msg.message == WM_QUIT || msg.message == WM_CLOSE ||
                (msg.message == WM_SYSCOMMAND && (msg.wParam & 0xfff0) == SC_CLOSE)) {
                TerminateProcess(GetCurrentProcess(), 0);
            }
            switch (msg.message) {
            case WM_PAINT: case WM_NCPAINT: case WM_ERASEBKGND:
            case WM_SIZE: case WM_MOVE: case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
            case WM_NCHITTEST: case WM_SETCURSOR: case WM_SYSCOMMAND:
            case WM_DPICHANGED: case WM_GETMINMAXINFO:
                DispatchMessageW(&msg); break;
            default: break; // Discard user commands/timers during a serialized operation.
            }
        }
    }
    worker.join();
    if (notified && IsWindow(window)) SetWindowTextW(window, old_title);
    return result.get();
}
