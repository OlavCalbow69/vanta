#include "logger.hpp"
#include "overlay_app.hpp"

#include <Windows.h>

namespace
{
    HANDLE g_cleanupCompletedEvent = nullptr;

    BOOL WINAPI ConsoleControlHandler(DWORD controlType)
    {
        switch (controlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            vanta::RequestOverlayExit();
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            vanta::RequestOverlayExit();
            if (g_cleanupCompletedEvent != nullptr)
            {
                WaitForSingleObject(
                    g_cleanupCompletedEvent,
                    4500);
            }
            return TRUE;
        default:
            return FALSE;
        }
    }
}

int wmain(int argumentCount, wchar_t** arguments)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    vanta::log::Initialize(instance);
    vanta::log::Info("vanta external overlay starting");

    g_cleanupCompletedEvent =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_cleanupCompletedEvent == nullptr)
    {
        vanta::log::Warning(
            "could not create the cleanup-completion event: %lu",
            GetLastError());
    }

    const bool consoleHandlerRegistered =
        SetConsoleCtrlHandler(
            &ConsoleControlHandler,
            TRUE) != FALSE;
    if (!consoleHandlerRegistered)
    {
        vanta::log::Warning(
            "could not install the graceful console-exit handler: %lu",
            GetLastError());
    }
    else
    {
        vanta::log::Info(
            "graceful console-exit handler installed");
    }

    const int result =
        vanta::RunOverlay(instance, argumentCount, arguments);

    vanta::log::Info(
        "vanta external overlay stopped with code %d",
        result);
    vanta::log::Shutdown();

    if (g_cleanupCompletedEvent != nullptr)
    {
        SetEvent(g_cleanupCompletedEvent);
    }
    if (consoleHandlerRegistered)
    {
        SetConsoleCtrlHandler(
            &ConsoleControlHandler,
            FALSE);
    }
    return result;
}
