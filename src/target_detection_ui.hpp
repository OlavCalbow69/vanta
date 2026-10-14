#pragma once
#include "target_detection.hpp"
namespace vanta
{
    bool RenderDetectionControls(TargetDetectionConfig& config);
    void RenderDetectionPreview(const TargetDetectionResult& result, bool ignoreBelowCrosshair = false);
    bool RunDetectionUiSelfTest();
}
