#pragma once

#include "app_settings.hpp"

#include <Windows.h>
#include <cstdint>

struct ID3D11ShaderResourceView;

namespace vanta
{
    class CaptureController;
    class BombTimerController;
    class ConfigManager;
    class MakcuController;
    class TestClickController;
    class TestMoveController;
}

namespace vanta::menu
{
    bool InitializeFonts();
    void ApplyStyle();
    MenuConfig GetConfig();
    void ApplyConfig(const MenuConfig& config);
    std::uint64_t SettingsRevision() noexcept;
    void Render(
        const char* surfaceDescription,
        HWND overlayWindow,
        CaptureController& capture,
        BombTimerController& bombTimer,
        ConfigManager& configManager,
        MakcuController& makcu,
        TestClickController& testClick,
        TestMoveController& testMove,
        ID3D11ShaderResourceView* logoTexture);
    void Toggle();
    bool IsVisible() noexcept;
    bool ContainsPoint(float x, float y) noexcept;
    bool GetBounds(
        float& x,
        float& y,
        float& width,
        float& height) noexcept;
}
