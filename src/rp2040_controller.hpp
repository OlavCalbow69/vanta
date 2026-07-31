#pragma once

#include <memory>

namespace vanta
{
    class Rp2040Controller
    {
    public:
        Rp2040Controller();
        ~Rp2040Controller();

        Rp2040Controller(const Rp2040Controller&) = delete;
        Rp2040Controller& operator=(const Rp2040Controller&) = delete;

        void Initialize();
        void Shutdown();
        void Tick();
        void RenderPanel();
        bool TryAutoConnect();
        void Disconnect();
        bool TryClick();
        bool TryMove(int x, int y);
        bool ForceReleaseLeftButton();
        bool IsConnected() const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
