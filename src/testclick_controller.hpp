#pragma once

#include "app_settings.hpp"

#include <cstdint>
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
        TestClickConfig GetConfig() const;
        void ApplyConfig(const TestClickConfig& config);
        std::uint64_t SettingsRevision() const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
