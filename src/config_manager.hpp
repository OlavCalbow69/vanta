#pragma once

#include "app_settings.hpp"

#include <Windows.h>

#include <cstdint>
#include <memory>

namespace vanta
{
    bool RunConfigurationSelfTest();

    class CaptureController;
    class BombTimerController;
    class MakcuController;
    class TestClickController;
    class TestMoveController;

    class ConfigManager
    {
    public:
        ConfigManager();
        ~ConfigManager();

        ConfigManager(const ConfigManager&) = delete;
        ConfigManager& operator=(const ConfigManager&) = delete;

        bool Initialize(
            bool persistenceEnabled,
            const LocalConfig& defaults);
        const LocalConfig& InitialLocalConfig() const noexcept;
        void PrimeRevisions(
            const CaptureController& capture,
            const MakcuController& mouseOutput,
            const BombTimerController& bombTimer);
        void AutoSaveLocal(
            const CaptureController& capture,
            const MakcuController& mouseOutput,
            const BombTimerController& bombTimer);
        void RenderPanel(
            HWND owner,
            TestClickController& testClick,
            TestMoveController& testMove);
        void Shutdown();

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
