#include "color_targets.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>

namespace vanta
{
    const HsvColorTarget& ColorTarget(const int index) noexcept
    {
        return kHsvColorTargets[static_cast<std::size_t>(std::clamp(
            index, 0, static_cast<int>(kHsvColorTargets.size()) - 1))];
    }

    void BuildHsvMask(const cv::Mat& hsv, const int index, cv::Mat& mask)
    {
        const auto& preset = ColorTarget(index);
        for (std::size_t i = 0; i < preset.rangeCount; ++i)
        {
            const auto& range = preset.ranges[i];
            cv::Mat selected;
            cv::inRange(hsv,
                cv::Scalar(range.lower[0], range.lower[1], range.lower[2]),
                cv::Scalar(range.upper[0], range.upper[1], range.upper[2]),
                selected);
            if (i == 0) mask = std::move(selected);
            else cv::bitwise_or(mask, selected, mask);
        }
    }
}
