#pragma once

#include "app_settings.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace cv { class Mat; }

namespace vanta
{
    struct CapturedFrameMetadata
    {
        std::uint64_t sequence{};
        std::int64_t captureTimestampNanoseconds{};
        int sourceWidth{};
        int sourceHeight{};
        int cropX{};
        int cropY{};
    };
    bool RunSunshineCaptureSourceSelfTest();

    // Supplies a new CPU BGRA frame after the caller's last consumed source
    // sequence. It lets the normal capture pipeline consume a decoded
    // Sunshine frame without treating Vanta's own window as a desktop source.
    using SunshineVideoFrameProvider = std::function<bool(
        std::uint64_t afterSequence,
        cv::Mat& output,
        std::uint64_t& sequence)>;

    struct CaptureOutline
    {
        bool visible{};
        RECT screenRectangle{};
        float color[4]{0.68F, 0.56F, 0.91F, 1.0F};
        int thickness{1};
    };

    struct NormalizedCaptureRegion
    {
        bool enabled{};
        float left{};
        float top{};
        float width{1.0F};
        float height{1.0F};
    };

    class CaptureController
    {
    public:
        CaptureController();
        ~CaptureController();

        CaptureController(const CaptureController&) = delete;
        CaptureController& operator=(const CaptureController&) = delete;

        bool Initialize(
            ID3D11Device* previewDevice,
            ID3D11DeviceContext* previewContext,
            DWORD ownProcessId,
            const CaptureConfig* initialConfig = nullptr);
        void SetSunshineVideoFrameProvider(
            SunshineVideoFrameProvider provider);
        void Shutdown();
        void Tick();
        void RenderPanel();
        bool StartAutomatedCapture(
            bool desktopDuplication,
            bool windowSource = false);
        void SetColorTargetIndex(int index) noexcept;
        CaptureConfig GetConfig() const;
        void ApplyConfig(const CaptureConfig& config);
        std::uint64_t SettingsRevision() const noexcept;
        CaptureOutline GetOutline() const noexcept;
        bool GetCaptureScreenRectangle(
            RECT& rectangle) const noexcept;
        // Returns the most recently processed centered capture frame (BGRA, CV_8UC4).
        // Returns an empty Mat if no frame is available yet.
        bool GetLatestCenteredFrame(cv::Mat& out) const;
        bool WaitForCenteredFrame(
            std::uint64_t afterSequence,
            cv::Mat& out,
            std::uint64_t& sequence,
            std::int64_t& captureTimestampNanoseconds,
            std::uint32_t timeoutMilliseconds) const;
        bool WaitForCenteredFrameWithMetadata(
            std::uint64_t afterSequence,
            cv::Mat& out,
            CapturedFrameMetadata& metadata,
            std::uint32_t timeoutMilliseconds) const;
        void SetAuxiliaryNormalizedRegion(
            const NormalizedCaptureRegion& region);
        bool WaitForAuxiliaryFrame(
            std::uint64_t afterSequence,
            cv::Mat& out,
            std::uint64_t& sequence,
            std::int64_t& captureTimestampNanoseconds,
            RECT& screenRectangle,
            std::uint32_t timeoutMilliseconds) const;
        bool GetAuxiliaryScreenRectangle(
            RECT& rectangle) const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
