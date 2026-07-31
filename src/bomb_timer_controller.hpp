#pragma once

#include "app_settings.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <memory>

namespace vanta
{
    class CaptureController;

    enum class BombTimerState : int
    {
        disabled,
        waitingForCapture,
        armed,
        confirming,
        countingDown,
        waitingForClear
    };

    struct BombTimerWidgetBounds
    {
        bool visible{};
        RECT screenRectangle{};
    };

    bool RunBombTimerSelfTest();

    class BombTimerController
    {
    public:
        BombTimerController();
        ~BombTimerController();

        BombTimerController(
            const BombTimerController&) = delete;
        BombTimerController& operator=(
            const BombTimerController&) = delete;

        void Initialize(
            CaptureController* capture,
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            HWND menuWindow,
            const BombTimerConfig* initialConfig = nullptr);
        void Shutdown();
        void Tick();
        void RenderPanel();

        void StartManualCountdown();
        void StartTestCountdown();
        void ResetCountdown();
        bool IsWidgetVisible() const noexcept;
        bool HasCaptureFrame() const noexcept;
        BombTimerState State() const noexcept;
        int MatchingPixels() const noexcept;
        BombTimerWidgetBounds GetWidgetBounds() const noexcept;
        BombTimerConfig GetConfig() const;
        void ApplyConfig(const BombTimerConfig& config);
        std::uint64_t SettingsRevision() const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
