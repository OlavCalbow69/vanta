#pragma once

#include <memory>

namespace vanta
{
    class CaptureController;
    class MakcuController;

    class TestClickController
    {
    public:
        TestClickController();
        ~TestClickController();

        TestClickController(const TestClickController&) = delete;
        TestClickController& operator=(const TestClickController&) = delete;

        void Initialize(
            CaptureController* capture,
            MakcuController* makcu);
        void Shutdown();
        void Tick();
        void RenderPanel();

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
