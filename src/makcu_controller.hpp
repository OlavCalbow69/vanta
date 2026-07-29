#pragma once

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

        bool Initialize();
        void Shutdown();
        void Tick();
        void RenderPanel();
        // Attempts one left-click through the connected MAKCU device.
        // Returns true only when the device confirms command success.
        bool TryClick();
        // Sends a relative mouse move (dx, dy) through the connected MAKCU device.
        // Returns true only when the device confirms command success.
        bool TryMove(int x, int y);
        bool IsConnected() const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
