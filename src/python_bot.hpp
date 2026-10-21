#pragma once
#include "app_settings.hpp"
#include <opencv2/core.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vanta
{
    struct PythonHsvRange
    {
        std::array<int,3> lower{};
        std::array<int,3> upper{};
    };
    struct PythonColorPreset
    {
        std::string key;
        std::string name;
        std::vector<std::string> hexVariations;
        std::vector<PythonHsvRange> ranges;
    };
    enum class PythonAxisMode : std::uint8_t
    {
        standard = 0,
        horizontalOnly = 1,
        hybrid = 2
    };
    struct PythonBotConfig
    {
        std::string activePreset{"purple"};
        std::vector<PythonColorPreset> presets;
        int fovRadius{120};
        double smoothFactor{0.45};
        PythonAxisMode axisMode{PythonAxisMode::standard};
        PythonPhysicalInputMode physicalInputMode{PythonPhysicalInputMode::existing};
        int hybridVerticalTimeMilliseconds{300};
        double minimumArea{4.0};
        int offsetX{2};
        int offsetY{3};
        bool filterOrbs{true}; // Intentionally unused by the reference algorithm.
        std::string aimKey{"0x05"};
        int screenWidth{1920}; // Metadata only: original uses actual screen metrics.
        int screenHeight{1080};
        bool makcuDebug{};
        bool makcuAutoReconnect{true};
    };
    const char* PythonAxisModeJsonName(PythonAxisMode mode) noexcept;
    std::string DefaultPythonBotJson();
    bool ParsePythonBotConfig(const std::string& json,PythonBotConfig& config,std::string& error);
    std::string SerializePythonBotConfig(const PythonBotConfig& config,const std::string& originalJson);
    bool ValidatePythonBotConfig(const PythonBotConfig& config,std::string& error);
    std::optional<MouseButtonBinding> PythonAimButton(const std::string& key);

    struct PythonBlob
    {
        cv::Rect bounds;
        std::vector<std::vector<cv::Point>> contours;
    };
    struct PythonBotResult
    {
        bool valid{};
        std::string error;
        cv::Rect sourceRoi;
        cv::Mat colorMask;
        cv::Mat fovMask;
        std::vector<std::vector<cv::Point>> validContours;
        std::vector<PythonBlob> blobs;
        std::optional<cv::Point> target;
        cv::Point movement{};
        std::array<std::size_t,4> rejections{}; // area, size, flat, orb
    };
    // Pure per-frame recreation. No normal Vanta detector, blacklist, resolution
    // scaling, prediction, speed multiplier, deadzone, click or mouse mask.
    PythonBotResult ProcessPythonBotFrame(const cv::Mat& sourceBgra,const PythonBotConfig& config);
    PythonBotResult ProcessPythonBotRoi(const cv::Mat& cropBgra,const PythonBotConfig& config,const cv::Rect& sourceRoi);
    bool RunPythonBotSelfTest();
    bool RunPythonParityFixtures(const std::string& inputPath,const std::string& outputPath);
}
