#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace vanta
{
    struct RgbaColor
    {
        float red{};
        float green{};
        float blue{};
        float alpha{1.0F};
    };

    enum class MovementMethod : int
    {
        direct = 0,
        windMouse = 1,
        axisControl = 2
    };

    enum class AxisMovementMode : int
    {
        horizontalOnly = 0,
        hybrid = 1,
        standard = 2
    };

    enum class ShortStopMode : int
    {
        fullStop = 0,
        slowMove = 1
    };

    // These values intentionally mirror MAKCU's mouse-button identifiers.
    // Keeping the binding independent of Win32 virtual-key values lets the
    // automation workers use only the MAKCU input monitor.
    enum class MouseButtonBinding : std::uint8_t
    {
        left = 0,
        right = 1,
        x1 = 3,
        x2 = 4
    };

    constexpr bool IsMouseButtonBinding(
        MouseButtonBinding button) noexcept
    {
        switch (button)
        {
        case MouseButtonBinding::left:
        case MouseButtonBinding::right:
        case MouseButtonBinding::x1:
        case MouseButtonBinding::x2:
            return true;
        default:
            return false;
        }
    }

    struct CaptureConfig
    {
        int backendIndex{};
        int sourceIndex{1};
        int regionIndex{1};
        int regionSize{640};
        bool drawOutline{true};
        RgbaColor outlineColor{
            0.68F, 0.56F, 0.91F, 1.0F};
        bool applyFilter{};
        bool binaryMask{};
        int colorTargetIndex{};
        std::string monitorDevice;
        std::string windowExecutable;
        std::string windowClass;
        std::string windowTitle;
    };

    struct MouseOutputConfig
    {
        int backendIndex{};
        std::string makcuPort;
        bool highPerformanceMode{true};
        bool autoDetectAndConnect{true};
    };

    struct UpdateConfig
    {
        bool automaticChecks{true};
        bool automaticDownloads{true};
        bool silentAutomaticInstallation{true};
    };

    struct BombTimerConfig
    {
        bool enabled{};
        RgbaColor targetColor{
            170.0F / 255.0F,
            0.0F,
            0.0F,
            1.0F};
        int colorTolerance{30};
        float regionLeft{918.0F / 1920.0F};
        float regionTop{6.0F / 1080.0F};
        float regionWidth{79.0F / 1920.0F};
        float regionHeight{68.0F / 1080.0F};
        int requiredMatchingPixels{3};
        int requiredConsecutiveFrames{3};
        RgbaColor safeColor{
            120.0F / 255.0F,
            1.0F,
            120.0F / 255.0F,
            1.0F};
        RgbaColor warningColor{
            1.0F,
            100.0F / 255.0F,
            100.0F / 255.0F,
            1.0F};
        float widgetOpacity{0.80F};
        int widgetStyle{};
        int widgetFont{};
        bool hasWidgetPosition{};
        float widgetPositionX{};
        float widgetPositionY{};
    };

    struct SunshineStreamConfig
    {
        int width{2560};
        int height{1440};
        int fps{120};
        int bitrateKbps{150000};
        bool requestDisplayMode{true};
        bool operator==(const SunshineStreamConfig&) const = default;
    };

    enum class AimPointMode : int { boundingBox = 0, maskTop = 1 };

    // Lengths/areas are reference-frame pixels, independent of the ROI crop.
    // Morphology kernels are always actual decoded pixels.
    struct TargetDetectionConfig
    {
        bool enabled{true};
        int referenceWidth{2560};
        int referenceHeight{1440};
        bool areaFilter{true};
        double minimumArea{5.0};
        double maximumArea{3500.0};
        bool widthFilter{true};
        double minimumWidth{1.0};
        double maximumWidth{160.0};
        bool heightFilter{true};
        double minimumHeight{1.0};
        double maximumHeight{240.0};
        bool aspectFilter{true};
        double minimumAspect{0.0};
        double maximumAspect{100.0};
        bool solidityFilter{true};
        double minimumSolidity{0.0};
        double maximumSolidity{1.0};
        bool flatFilter{true};
        double flatAspectMaximum{0.25};
        double flatMinimumWidth{27.0};
        bool orbFilter{true};
        double orbAspectMinimum{0.70};
        double orbAspectMaximum{1.25};
        double orbMinimumWidth{20.0};
        double orbFillMinimum{0.65};
        double orbSolidityMinimum{0.90};
        bool closingEnabled{true};
        int closingKernel{3};
        int dilationKernel{3};
        double mergeProximity{16.0};
        AimPointMode aimPointMode{AimPointMode::maskTop};
        int topRows{4};
        double offsetX{1.0};
        double offsetY{7.0};
    };

    struct TestClickConfig
    {
        bool enabled{};
        int hsvRangeIndex{};
        MouseButtonBinding holdButton{
            MouseButtonBinding::x1};
        int roiOffset{50};
        int rayLength{49};
        int rayHalfWidth{1};
        int hitMode{};
        bool burstLimiter{};
        float burstDelay{0.05F};
        float burstPause{0.20F};
        int burstSize{3};
    };

    struct TestMoveConfig
    {
        bool enabled{};
        TargetDetectionConfig detector;
        int hsvRangeIndex{};
        MouseButtonBinding aimButton{
            MouseButtonBinding::left};
        int killFov{100};
        int targetHeightPercent{15};
        bool drawFovOutline{true};
        RgbaColor fovColor{
            0.68F, 0.56F, 0.91F, 1.0F};
        MovementMethod movementMethod{
            MovementMethod::direct};
        float speed{1.125F};
        float smooth{0.90F};
        int deadzone{2};
        int mergeProximity{10};
        float windGravity{9.0F};
        float windStrength{3.0F};
        float windMaximumStep{15.0F};
        float windSlowdownRadius{12.0F};
        AxisMovementMode axisMode{
            AxisMovementMode::standard};
        float axisHorizontalMultiplier{1.125F};
        float axisVerticalMultiplier{1.125F};
        float axisSmoothing{0.90F};
        int hybridVerticalTimeMilliseconds{300};
        bool antiBelowObjects{};
        bool shortStopEnabled{};
        ShortStopMode shortStopMode{
            ShortStopMode::slowMove};
        int shortStopChancePercent{5};
        int shortStopMinimumPauseMilliseconds{30};
        int shortStopMaximumPauseMilliseconds{90};
        float shortStopSlowMultiplierMinimum{2.0F};
        float shortStopSlowMultiplierMaximum{4.0F};
        int newTargetDelayMinimumMilliseconds{};
        int newTargetDelayMaximumMilliseconds{};
    };

    enum class PaletteSlot : std::size_t
    {
        accent,
        secondary,
        windowBackground,
        widgetBackground,
        stroke,
        separator,
        animationActive,
        animationDefault,
        mainBackground,
        childBackground,
        childStroke,
        checkboxMark,
        pageActive,
        pageBackground,
        pageHoverText,
        pageText,
        elementHovered,
        elementBackground,
        labelActive,
        labelHovered,
        labelDefault,
        descriptionActive,
        descriptionHovered,
        descriptionDefault,
        textActive,
        textHovered,
        textDefault,
        count
    };

    struct MenuConfig
    {
        bool notifications{true};
        bool compactMode{};
        int activePage{1};
        bool hasGeometry{};
        float positionX{};
        float positionY{};
        float width{850.0F};
        float height{596.0F};
        std::array<
            RgbaColor,
            static_cast<std::size_t>(
                PaletteSlot::count)> palette{};
    };

    struct LocalConfig
    {
        static constexpr int schemaVersion = 7;
        CaptureConfig capture;
        SunshineStreamConfig sunshine;
        MouseOutputConfig mouseOutput;
        UpdateConfig updates;
        BombTimerConfig bombTimer;
        MenuConfig menu;
    };

    struct SharedProfile
    {
        static constexpr int schemaVersion = 8;
        std::string name;
        TestClickConfig testClick;
        TestMoveConfig testMove;
    };
}
