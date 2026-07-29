#pragma once

#include <memory>

namespace vanta
{
    class CaptureController;
    class MakcuController;

    class TestMoveController
    {
    public:
        TestMoveController();
        ~TestMoveController();

        TestMoveController(const TestMoveController&) = delete;
        TestMoveController& operator=(const TestMoveController&) = delete;

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
