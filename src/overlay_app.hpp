#pragma once

#include <Windows.h>

namespace vanta
{
    void RequestOverlayExit() noexcept;
    int RunOverlay(HINSTANCE instance, int argumentCount, wchar_t** arguments);
}
