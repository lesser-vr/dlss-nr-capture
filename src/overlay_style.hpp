#pragma once
#include <cstdint>

enum class OverlayMessageStyle { information, error };

struct OverlayPalette {
    uint32_t background_rgb;
    uint32_t text_rgb;
    float background_opacity;
};

constexpr OverlayPalette overlay_palette(OverlayMessageStyle style) noexcept
{
    return style == OverlayMessageStyle::error
        ? OverlayPalette{0xA81919, 0xFFFFFF, 0.88f}
        : OverlayPalette{0x000000, 0x76B900, 1.0f};
}
