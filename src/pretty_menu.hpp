#pragma once

#include <Windows.h>

struct ID3D11ShaderResourceView;

namespace vanta
{
    class CaptureController;
    class MakcuController;
    class TestClickController;
    class TestMoveController;
}

namespace vanta::menu
{
    bool InitializeFonts();
    void ApplyStyle();
    void Render(
        const char* surfaceDescription,
        HWND overlayWindow,
        CaptureController& capture,
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
