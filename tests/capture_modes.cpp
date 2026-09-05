#include "capture_engine.hpp"
#include <iostream>

std::string utf8(const std::wstring& value) {
    const int bytes=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(bytes,'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),bytes,nullptr,nullptr);
    return result;
}

int main() {
    try {
        throw_if_failed(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        {
            CaptureEngine engine;
            for (const auto& device : CaptureEngine::enumerate_devices()) {
                std::cout << "DEVICE " << utf8(device.name) << '\n';
                for (const auto& mode : CaptureEngine::enumerate_modes(device))
                    std::cout << utf8(mode.display_name()) << " supported=" << mode.supported << '\n';
            }
        }
        CoUninitialize(); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
