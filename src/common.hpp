#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

inline void throw_if_failed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        char message[256]{};
        sprintf_s(message, "%s failed (HRESULT 0x%08lX)", operation,
                  static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

inline std::wstring widen(const std::string& input)
{
    if (input.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, input.data(),
                                           static_cast<int>(input.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                        result.data(), count);
    return result;
}
