#pragma once

#include "app_settings.hpp"

#include <cstdint>
#include <memory>

namespace vanta
{
    class MakcuController
    {
    public:
        MakcuController();
        ~MakcuController();

        MakcuController(const MakcuController&) = delete;
        MakcuController& operator=(const MakcuController&) = delete;

        bool Initialize(
            const MouseOutputConfig* initialConfig = nullptr);
        void Shutdown();
        void Tick();
        void RenderPanel();
        // Attempts one left-click through the connected MAKCU device.
        // Returns true only when the device confirms command success.
        bool TryClick();
        // Sends a relative mouse move (dx, dy) through the connected MAKCU device.
        // Returns true only when the device confirms command success.
        bool TryMove(int x, int y);
        // Best-effort neutralization used by click recovery and shutdown.
        bool ForceReleaseLeftButton();
        bool IsConnected() const noexcept;
        MouseOutputConfig GetConfig() const;
        void ApplyConfig(const MouseOutputConfig& config);
        std::uint64_t SettingsRevision() const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
