#pragma once

#include <array>
#include <cstddef>

namespace cv { class Mat; }

namespace vanta
{
    struct HsvRange
    {
        std::array<int, 3> lower;
        std::array<int, 3> upper;
    };

    struct HsvColorTarget
    {
        const char* label;
        const char* description;
        std::array<HsvRange, 2> ranges;
        std::size_t rangeCount;
        bool purpleBlacklist;
    };

    // IDs 0 and 1 are persisted by legacy profiles. Never reorder them.
    inline constexpr std::array<HsvColorTarget, 4> kHsvColorTargets{{
        {"Purple (narrow)", "HSV 144,106,172 - 151,255,255",
            {{{{144,106,172},{151,255,255}}, {}}}, 1, true},
        {"Purple (wide)", "HSV 135,63,102 - 155,255,255",
            {{{{135,63,102},{155,255,255}}, {}}}, 1, true},
        {"Yellow", "HSV 20,100,100 - 40,255,255",
            {{{{20,100,100},{40,255,255}}, {}}}, 1, false},
        {"Red (both hue ranges)", "HSV 0-10 and 170-179, S/V 100-255",
            {{{{0,100,100},{10,255,255}}, {{170,100,100},{179,255,255}}}}, 2, false}
    }};

    const HsvColorTarget& ColorTarget(int index) noexcept;
    void BuildHsvMask(const cv::Mat& hsv, int index, cv::Mat& mask);
}
