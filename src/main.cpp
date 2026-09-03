#include "app.hpp"

#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    using SetDpiContext = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto set_context = reinterpret_cast<SetDpiContext>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
            set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    try {
        App app;
        return app.run(instance, show_command);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "DLSS NR Capture error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
