#pragma once

#include "app_settings.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <memory>

namespace cv { class Mat; }

namespace vanta
{
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
