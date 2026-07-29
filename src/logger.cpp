#include "logger.hpp"

#include <Windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <iterator>
#include <mutex>

namespace
{
    std::mutex g_logMutex;
    bool g_allocatedConsole = false;
    FILE* g_logFile = nullptr;

    void Write(const char* level, const char* format, va_list arguments)
    {
        char message[2048]{};
        vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);

        SYSTEMTIME time{};
        GetLocalTime(&time);

        char line[2304]{};
        snprintf(
            line,
            sizeof(line),
            "[%02u:%02u:%02u.%03u] [%-5s] [T%lu] %s\n",
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds,
            level,
            GetCurrentThreadId(),
            message);

        const std::scoped_lock lock(g_logMutex);
        std::fputs(line, stdout);
        std::fflush(stdout);
        if (g_logFile != nullptr)
        {
            std::fputs(line, g_logFile);
            std::fflush(g_logFile);
        }
        OutputDebugStringA(line);
    }

    void WriteVariadic(const char* level, const char* format, va_list arguments)
    {
        Write(level, format, arguments);
    }
}

namespace vanta::log
{
    void Initialize(HMODULE module)
    {
        if (GetConsoleWindow() == nullptr && AllocConsole())
        {
            g_allocatedConsole = true;
        }

        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleTitleW(L"vanta | external overlay");

        wchar_t modulePath[32768]{};
        if (module != nullptr &&
            GetModuleFileNameW(
                module,
                modulePath,
                static_cast<DWORD>(std::size(modulePath))) != 0)
        {
            const std::filesystem::path logPath =
                std::filesystem::path(modulePath).parent_path() /
                L"vanta.log";
            HANDLE logHandle = CreateFileW(
                logPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (logHandle != INVALID_HANDLE_VALUE)
            {
                const int descriptor = _open_osfhandle(
                    reinterpret_cast<std::intptr_t>(logHandle),
                    _O_WRONLY | _O_TEXT);
                if (descriptor >= 0)
                {
                    g_logFile = _fdopen(descriptor, "w");
                }
                else
                {
                    CloseHandle(logHandle);
                }
            }
        }

        Info("console logger initialized");
    }

    void Shutdown()
    {
        {
            const std::scoped_lock lock(g_logMutex);
            if (g_logFile != nullptr)
            {
                std::fclose(g_logFile);
                g_logFile = nullptr;
            }
        }

        if (g_allocatedConsole)
        {
            FreeConsole();
            g_allocatedConsole = false;
        }
    }

    void Info(const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        WriteVariadic("INFO", format, arguments);
        va_end(arguments);
    }

    void Warning(const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        WriteVariadic("WARN", format, arguments);
        va_end(arguments);
    }

    void Error(const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        WriteVariadic("ERROR", format, arguments);
        va_end(arguments);
    }
}
