#pragma once

#include <array>

namespace vanta
{
    struct HsvColorTarget
    {
        const char* label;
        const char* description;
        std::array<int, 3> lower;
        std::array<int, 3> upper;
    };

    inline constexpr std::array<HsvColorTarget, 2> kHsvColorTargets{{
        {
            "Current target (HSV 144,106,172 - 151,255,255)",
            "Narrow current purple range",
            {144, 106, 172},
            {151, 255, 255}
        },
        {
            "Alternative target (HSV 135,63,102 - 155,255,255)",
            "Wider alternative purple range",
            {135, 63, 102},
            {155, 255, 255}
        }
    }};
}
