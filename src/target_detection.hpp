#pragma once

#include "app_settings.hpp"
#include <opencv2/core.hpp>
#include <array>
#include <cstddef>
#include <vector>

namespace vanta
{
    struct DetectionGeometry
    {
        int sourceWidth{};
        int sourceHeight{};
        int cropX{};
        int cropY{};
    };

    enum class ContourRejection : std::size_t
    {
        degenerate, area, width, height, aspect, solidity, flat, orb, count
    };
    const char* RejectionName(ContourRejection reason) noexcept;
    TargetDetectionConfig SanitizeDetectionConfig(TargetDetectionConfig config);

    struct ContourDiagnostic
    {
        cv::Rect bounds;
        ContourRejection reason{};
        double area{};
        double solidity{};
        double fill{};
    };

    struct DetectedTarget
    {
        cv::Rect bounds;
        cv::Point2d aim;
    };

    struct TargetDetectionResult
    {
        cv::Mat mask;
        std::vector<DetectedTarget> targets;
        std::vector<ContourDiagnostic> accepted;
        std::vector<ContourDiagnostic> rejected;
        std::array<std::size_t, static_cast<std::size_t>(ContourRejection::count)> rejectionCounts{};
        std::size_t acceptedContours{};
        double scaleX{1.0};
        double scaleY{1.0};
        double minimumArea{};
        double maximumArea{};
        double minimumWidth{};
        double maximumWidth{};
        double minimumHeight{};
        double maximumHeight{};
        int fovRadius{};
    };

    // Input is an HSV-derived binary mask, never the resized UI preview.
    TargetDetectionResult DetectTargets(
        const cv::Mat& colorMask,
        TargetDetectionConfig config,
        DetectionGeometry geometry,
        int fovRadius,
        int targetHeightPercent);

    bool RunTargetDetectionSelfTest();
}
