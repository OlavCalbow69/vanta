#pragma once

#include "app_settings.hpp"

#include <cstdint>
#include <memory>

namespace vanta
{
    class CaptureController;
    class MakcuController;

    struct TestMoveFovOutline
    {
        bool visible{};
        int radius{100};
        RgbaColor color{
            0.68F, 0.56F, 0.91F, 1.0F};
    };

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
        TestMoveConfig GetConfig() const;
        void ApplyConfig(const TestMoveConfig& config);
        std::uint64_t SettingsRevision() const noexcept;
        TestMoveFovOutline GetFovOutline() const;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
