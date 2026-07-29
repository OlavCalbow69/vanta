#pragma once

#include <Windows.h>

namespace vanta::log
{
    void Initialize(HMODULE module);
    void Shutdown();
    void Info(const char* format, ...);
    void Warning(const char* format, ...);
    void Error(const char* format, ...);
}
