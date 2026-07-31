#include "resource.h"
#include "update_shared.hpp"
#include "version.hpp"

#include <Windows.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr UINT updateMessage = WM_APP + 1;
    constexpr UINT finishMessage = WM_APP + 2;
    constexpr std::array<const wchar_t*, 5> bundleFiles{
        L"vanta.exe",
        L"vanta-updater.exe",
        L"opencv_world500.dll",
        L"makcu-cpp.dll",
        L"README.md"};

    struct Arguments
    {
        bool bootstrap{};
        bool install{};
        bool selfTest{};
        std::filesystem::path application;
        std::filesystem::path zip;
        std::wstring currentVersion;
        std::wstring tag;
        std::string sha256;
        DWORD processId{};
    };

    struct DisplayState
    {
        std::mutex mutex;
        std::wstring status{L"Starting Vanta updater"};
        std::wstring activity{L"Preparing secure update check"};
        std::wstring releaseNotes{L"Release notes will appear after the check."};
        int progress{};
    };

    DisplayState g_display;
    HWND g_window{};
    std::atomic_bool g_cancelled{false};

    std::wstring Utf16(const std::string& value)
    {
        return winrt::to_hstring(value).c_str();
    }

    std::string Utf8(const std::wstring& value)
    {
        return winrt::to_string(value);
    }

    void SetDisplay(
        int progress,
        std::wstring status,
        std::wstring activity,
        const std::wstring* notes = nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(g_display.mutex);
            g_display.progress = std::clamp(progress, 0, 100);
            g_display.status = std::move(status);
            g_display.activity = std::move(activity);
            if (notes != nullptr && !notes->empty())
            {
                g_display.releaseNotes = *notes;
            }
        }
        if (g_window != nullptr)
        {
            PostMessageW(g_window, updateMessage, 0, 0);
        }
    }

    std::wstring ArgumentValue(
        int count,
        wchar_t** values,
        const wchar_t* name)
    {
        for (int index = 1; index + 1 < count; ++index)
        {
            if (_wcsicmp(values[index], name) == 0)
            {
                return values[index + 1];
            }
        }
        return {};
    }

    bool HasArgument(
        int count,
        wchar_t** values,
        const wchar_t* name)
    {
        for (int index = 1; index < count; ++index)
        {
            if (_wcsicmp(values[index], name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    Arguments ParseArguments(int count, wchar_t** values)
    {
        Arguments result;
        result.bootstrap = HasArgument(count, values, L"--bootstrap");
        result.install = HasArgument(count, values, L"--install");
        result.selfTest = HasArgument(count, values, L"--self-test");
        result.application = ArgumentValue(count, values, L"--app");
        result.zip = ArgumentValue(count, values, L"--zip");
        result.currentVersion = ArgumentValue(count, values, L"--current");
        result.tag = ArgumentValue(count, values, L"--tag");
        result.sha256 = Utf8(ArgumentValue(count, values, L"--sha256"));
        const std::wstring process = ArgumentValue(count, values, L"--pid");
        if (!process.empty())
        {
            result.processId = static_cast<DWORD>(
                std::wcstoul(process.c_str(), nullptr, 10));
        }
        return result;
    }

    void WaitForApplication(DWORD processId)
    {
        if (processId == 0)
        {
            return;
        }
        HANDLE process = OpenProcess(
            SYNCHRONIZE,
            FALSE,
            processId);
        if (process != nullptr)
        {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }
    }

    bool LaunchVanta(
        const std::filesystem::path& application,
        std::string& error)
    {
        return vanta::updates::StartProcess(
            application,
            L"--updater-launched",
            error);
    }

    std::wstring PowerShellLiteral(const std::wstring& value)
    {
        std::wstring escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back(L'\'');
        for (const wchar_t character : value)
        {
            if (character == L'\'')
            {
                escaped += L"''";
            }
            else
            {
                escaped.push_back(character);
            }
        }
        escaped.push_back(L'\'');
        return escaped;
    }

    bool ExtractZip(
        const std::filesystem::path& zip,
        const std::filesystem::path& destination,
        std::string& error)
    {
        std::error_code filesystemError;
        std::filesystem::remove_all(destination, filesystemError);
        filesystemError.clear();
        std::filesystem::create_directories(destination, filesystemError);
        if (filesystemError)
        {
            error = "could not create staging directory: " +
                filesystemError.message();
            return false;
        }
        wchar_t windowsDirectory[MAX_PATH]{};
        if (GetWindowsDirectoryW(
                windowsDirectory,
                MAX_PATH) == 0)
        {
            error = "could not locate Windows PowerShell";
            return false;
        }
        const std::filesystem::path powershell =
            std::filesystem::path(windowsDirectory) /
            L"System32" / L"WindowsPowerShell" /
            L"v1.0" / L"powershell.exe";
        const std::wstring script =
            L"Expand-Archive -LiteralPath " +
            PowerShellLiteral(zip.wstring()) +
            L" -DestinationPath " +
            PowerShellLiteral(destination.wstring()) +
            L" -Force";
        std::wstring command =
            vanta::updates::QuoteArgument(powershell.wstring()) +
            L" -NoLogo -NoProfile -NonInteractive "
            L"-ExecutionPolicy Bypass -Command " +
            vanta::updates::QuoteArgument(script);
        std::vector<wchar_t> mutableCommand(
            command.begin(),
            command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                powershell.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                destination.c_str(),
                &startup,
                &process))
        {
            error = "could not start archive extraction";
            return false;
        }
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, 120000);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        if (exitCode != 0)
        {
            error = "archive extraction failed with code " +
                std::to_string(exitCode);
            return false;
        }
        return true;
    }

    bool ValidateStage(
        const std::filesystem::path& stage,
        std::string& error)
    {
        for (const wchar_t* file : bundleFiles)
        {
            if (!std::filesystem::is_regular_file(stage / file))
            {
                error = "update archive is missing " +
                    std::filesystem::path(file).string();
                return false;
            }
        }
        return true;
    }

    bool ReplaceBundle(
        const std::filesystem::path& stage,
        const std::filesystem::path& applicationDirectory,
        const std::filesystem::path& backupDirectory,
        std::string& error,
        int failAfterCopies = -1)
    {
        if (!ValidateStage(stage, error))
        {
            return false;
        }
        std::error_code filesystemError;
        std::filesystem::create_directories(
            backupDirectory,
            filesystemError);
        if (filesystemError)
        {
            error = "could not create backup directory: " +
                filesystemError.message();
            return false;
        }

        std::array<bool, bundleFiles.size()> existed{};
        for (std::size_t index = 0; index < bundleFiles.size(); ++index)
        {
            const auto installed = applicationDirectory / bundleFiles[index];
            existed[index] = std::filesystem::is_regular_file(installed);
            if (existed[index])
            {
                if (!CopyFileW(
                        installed.c_str(),
                        (backupDirectory / bundleFiles[index]).c_str(),
                        FALSE))
                {
                    error = "could not back up " + installed.filename().string();
                    return false;
                }
            }
        }

        int copied = 0;
        bool replacementSucceeded = true;
        for (std::size_t index = 0; index < bundleFiles.size(); ++index)
        {
            if (failAfterCopies >= 0 && copied >= failAfterCopies)
            {
                error = "simulated replacement failure";
                replacementSucceeded = false;
                break;
            }
            const auto source = stage / bundleFiles[index];
            const auto destination = applicationDirectory / bundleFiles[index];
            if (!CopyFileW(
                    source.c_str(),
                    destination.c_str(),
                    FALSE))
            {
                error = "could not replace " + destination.filename().string() +
                    " (error " + std::to_string(GetLastError()) + ")";
                replacementSucceeded = false;
                break;
            }
            ++copied;
        }
        if (replacementSucceeded)
        {
            return true;
        }

        bool rollbackSucceeded = true;
        for (std::size_t index = 0; index < bundleFiles.size(); ++index)
        {
            const auto destination = applicationDirectory / bundleFiles[index];
            if (existed[index])
            {
                if (!CopyFileW(
                        (backupDirectory / bundleFiles[index]).c_str(),
                        destination.c_str(),
                        FALSE))
                {
                    rollbackSucceeded = false;
                }
            }
            else
            {
                std::filesystem::remove(destination, filesystemError);
            }
        }
        error += rollbackSucceeded
            ? "; previous files restored"
            : "; rollback was incomplete";
        return false;
    }

    std::filesystem::path UniqueDirectory(const wchar_t* prefix)
    {
        return vanta::updates::UpdateRoot() /
            (std::wstring(prefix) +
             std::to_wstring(vanta::updates::CurrentUnixTime()) +
             L"-" + std::to_wstring(GetCurrentProcessId()));
    }

    bool InstallArchive(
        const std::filesystem::path& zip,
        const std::string& sha256,
        const std::filesystem::path& application,
        std::string& error)
    {
        SetDisplay(
            76,
            L"Installing verified update",
            L"Verifying SHA-256 again before replacement");
        if (!vanta::updates::VerifySha256(zip, sha256, error))
        {
            return false;
        }
        const auto stage = UniqueDirectory(L"staging-");
        SetDisplay(
            82,
            L"Installing verified update",
            L"Extracting the release into a staging directory");
        if (!ExtractZip(zip, stage, error))
        {
            return false;
        }
        if (!ValidateStage(stage, error))
        {
            return false;
        }
        const auto backup = UniqueDirectory(L"backup-");
        SetDisplay(
            90,
            L"Installing verified update",
            L"Backing up the current Vanta files");
        const bool replaced = ReplaceBundle(
            stage,
            application.parent_path(),
            backup,
            error);
        std::error_code cleanupError;
        std::filesystem::remove_all(stage, cleanupError);
        return replaced;
    }

    bool GetLatestRelease(
        vanta::updates::ReleaseInfo& release,
        std::string& error)
    {
        std::int64_t checkedAt = 0;
        if (vanta::updates::LoadCachedRelease(
                release,
                checkedAt,
                error) &&
            vanta::updates::IsCacheFresh(checkedAt))
        {
            SetDisplay(
                25,
                L"Checking for updates",
                L"Using today's cached GitHub release result");
            return true;
        }
        std::int64_t lastAttempt = 0;
        std::string attemptError;
        if (vanta::updates::LoadLastCheckAttempt(
                lastAttempt,
                attemptError) &&
            vanta::updates::IsCacheFresh(lastAttempt))
        {
            error =
                "Today's automatic update check was already attempted";
            return false;
        }
        error.clear();
        SetDisplay(
            15,
            L"Checking for updates",
            L"Requesting GitHub releases/latest");
        vanta::updates::SaveLastCheckAttempt(
            vanta::updates::CurrentUnixTime(),
            attemptError);
        if (!vanta::updates::FetchLatestRelease(release, error))
        {
            return false;
        }
        std::string cacheError;
        vanta::updates::SaveCachedRelease(
            release,
            vanta::updates::CurrentUnixTime(),
            cacheError);
        return true;
    }

    void RunBootstrap(const Arguments& arguments)
    {
        WaitForApplication(arguments.processId);
        std::string error;
        vanta::updates::UpdaterPreferences preferences;
        if (!vanta::updates::LoadUpdaterPreferences(
                preferences,
                error))
        {
            preferences = {};
        }
        if (!preferences.automaticChecks)
        {
            SetDisplay(100, L"Automatic checks disabled", L"Starting Vanta");
            Sleep(250);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }

        vanta::updates::ReleaseInfo release;
        if (!GetLatestRelease(release, error))
        {
            SetDisplay(
                100,
                L"Update check unavailable",
                Utf16(error) + L" — starting installed Vanta");
            Sleep(900);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }
        const std::wstring notes = Utf16(release.notes);
        if (!vanta::updates::IsReleaseNewer(
                release.tag,
                Utf8(arguments.currentVersion)))
        {
            SetDisplay(
                100,
                L"Up to date",
                L"Starting Vanta " + arguments.currentVersion,
                &notes);
            Sleep(400);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }

        SetDisplay(
            30,
            L"Update " + Utf16(release.tag) + L" available",
            preferences.automaticDownloads
                ? L"Preparing automatic download"
                : L"Automatic downloads are disabled",
            &notes);
        if (!preferences.automaticDownloads)
        {
            Sleep(900);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }

        std::filesystem::path zip;
        std::string sha256;
        const bool downloaded = vanta::updates::DownloadRelease(
            release,
            zip,
            sha256,
            [&](int percentage, const std::string& activity)
            {
                if (g_cancelled.load(std::memory_order_acquire))
                {
                    return false;
                }
                SetDisplay(
                    30 + percentage * 45 / 100,
                    L"Update " + Utf16(release.tag) + L" available",
                    Utf16(activity),
                    &notes);
                return true;
            },
            error);
        if (!downloaded)
        {
            SetDisplay(
                100,
                L"Update download failed",
                Utf16(error) + L" — starting installed Vanta",
                &notes);
            Sleep(1000);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }
        if (!preferences.silentAutomaticInstallation)
        {
            SetDisplay(
                100,
                L"Update downloaded",
                L"Silent installation is disabled; use Install on exit in Vanta",
                &notes);
            Sleep(1000);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }
        if (!InstallArchive(
                zip,
                sha256,
                arguments.application,
                error))
        {
            SetDisplay(
                100,
                L"Installation failed safely",
                Utf16(error) + L" — starting restored Vanta",
                &notes);
            Sleep(1200);
            LaunchVanta(arguments.application, error);
            PostMessageW(g_window, finishMessage, 0, 0);
            return;
        }
        SetDisplay(
            100,
            L"Update installed",
            L"Starting " + Utf16(release.tag),
            &notes);
        Sleep(500);
        LaunchVanta(arguments.application, error);
        PostMessageW(g_window, finishMessage, 0, 0);
    }

    void RunInstall(const Arguments& arguments)
    {
        WaitForApplication(arguments.processId);
        std::string error;
        vanta::updates::ReleaseInfo cached;
        std::int64_t checkedAt = 0;
        std::wstring notes;
        if (vanta::updates::LoadCachedRelease(
                cached,
                checkedAt,
                error))
        {
            notes = Utf16(cached.notes);
        }
        SetDisplay(
            70,
            L"Installing " + arguments.tag,
            L"Vanta exited cleanly; beginning replacement",
            notes.empty() ? nullptr : &notes);
        if (!InstallArchive(
                arguments.zip,
                arguments.sha256,
                arguments.application,
                error))
        {
            SetDisplay(
                100,
                L"Installation failed safely",
                Utf16(error),
                notes.empty() ? nullptr : &notes);
            Sleep(1200);
        }
        else
        {
            SetDisplay(
                100,
                L"Update installed",
                L"Relaunching Vanta",
                notes.empty() ? nullptr : &notes);
            Sleep(450);
        }
        LaunchVanta(arguments.application, error);
        PostMessageW(g_window, finishMessage, 0, 0);
    }

    std::string ReadSmallFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    }

    bool WriteSmallFile(
        const std::filesystem::path& path,
        const std::string& value)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << value;
        return static_cast<bool>(output);
    }

    int RunSelfTest()
    {
        if (!vanta::updates::IsReleaseNewer("v1.2.4", "1.2.3") ||
            vanta::updates::IsReleaseNewer("v1.2.3", "1.2.3") ||
            vanta::updates::IsReleaseNewer("v1.1.99", "1.2.0") ||
            !vanta::updates::IsCacheFresh(
                vanta::updates::CurrentUnixTime()) ||
            vanta::updates::IsCacheFresh(
                vanta::updates::CurrentUnixTime() -
                vanta::updates::cacheLifetimeSeconds - 1))
        {
            return 1;
        }
        const auto root = std::filesystem::temp_directory_path() /
            (L"vanta-updater-self-test-" +
             std::to_wstring(GetCurrentProcessId()));
        const auto installed = root / L"installed";
        const auto stage = root / L"stage";
        const auto backup = root / L"backup";
        std::error_code filesystemError;
        std::filesystem::remove_all(root, filesystemError);
        std::filesystem::create_directories(installed, filesystemError);
        std::filesystem::create_directories(stage, filesystemError);
        if (filesystemError)
        {
            return 2;
        }
        for (const wchar_t* file : bundleFiles)
        {
            if (!WriteSmallFile(installed / file, "old") ||
                !WriteSmallFile(stage / file, "new"))
            {
                return 3;
            }
        }
        std::string error;
        if (ReplaceBundle(stage, installed, backup, error, 2))
        {
            return 4;
        }
        for (const wchar_t* file : bundleFiles)
        {
            if (ReadSmallFile(installed / file) != "old")
            {
                return 5;
            }
        }
        std::filesystem::remove_all(backup, filesystemError);
        if (!ReplaceBundle(stage, installed, backup, error))
        {
            return 6;
        }
        for (const wchar_t* file : bundleFiles)
        {
            if (ReadSmallFile(installed / file) != "new")
            {
                return 7;
            }
        }
        const auto hashFile = root / L"hash.txt";
        WriteSmallFile(hashFile, "abc");
        if (!vanta::updates::VerifySha256(
                hashFile,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                error))
        {
            return 8;
        }
        if (vanta::updates::VerifySha256(
                hashFile,
                "0000000000000000000000000000000000000000000000000000000000000000",
                error))
        {
            return 9;
        }
        std::filesystem::remove_all(root, filesystemError);
        return 0;
    }

    LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (message)
        {
        case WM_NCHITTEST:
            return HTCAPTION;
        case WM_ERASEBKGND:
            return 1;
        case updateMessage:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case finishMessage:
            DestroyWindow(window);
            return 0;
        case WM_CLOSE:
            g_cancelled.store(true, std::memory_order_release);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC context = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(RGB(17, 16, 23));
            FillRect(context, &client, background);
            DeleteObject(background);

            HPEN blackPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(190, 112, 230));
            HGDIOBJ previousPen = SelectObject(context, blackPen);
            HGDIOBJ previousBrush = SelectObject(
                context,
                GetStockObject(NULL_BRUSH));
            Rectangle(context, 0, 0, client.right, client.bottom);
            SelectObject(context, accentPen);
            Rectangle(context, 1, 1, client.right - 1, client.bottom - 1);
            SelectObject(context, previousBrush);
            SelectObject(context, previousPen);
            DeleteObject(blackPen);
            DeleteObject(accentPen);

            DisplayState snapshot;
            {
                std::lock_guard<std::mutex> lock(g_display.mutex);
                snapshot.status = g_display.status;
                snapshot.activity = g_display.activity;
                snapshot.releaseNotes = g_display.releaseNotes;
                snapshot.progress = g_display.progress;
            }

            SetBkMode(context, TRANSPARENT);
            HFONT titleFont = CreateFontW(
                24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT textFont = CreateFontW(
                17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT smallFont = CreateFontW(
                15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            SelectObject(context, titleFont);
            SetTextColor(context, RGB(235, 231, 241));
            TextOutW(context, 24, 18, L"VANTA UPDATER", 13);
            SelectObject(context, smallFont);
            SetTextColor(context, RGB(154, 145, 165));
            const std::wstring version = L"Secure update gateway  " VANTA_VERSION_WSTRING;
            TextOutW(
                context,
                320,
                23,
                version.c_str(),
                static_cast<int>(version.size()));

            SelectObject(context, textFont);
            SetTextColor(context, RGB(216, 165, 240));
            TextOutW(
                context,
                24,
                66,
                snapshot.status.c_str(),
                static_cast<int>(snapshot.status.size()));

            RECT progressBackground{24, 102, client.right - 24, 124};
            HBRUSH progressBackBrush = CreateSolidBrush(RGB(34, 30, 43));
            FillRect(context, &progressBackground, progressBackBrush);
            DeleteObject(progressBackBrush);
            RECT progressFill = progressBackground;
            progressFill.right = progressFill.left +
                (progressBackground.right - progressBackground.left) *
                snapshot.progress / 100;
            HBRUSH progressBrush = CreateSolidBrush(RGB(185, 89, 224));
            FillRect(context, &progressFill, progressBrush);
            DeleteObject(progressBrush);
            FrameRect(
                context,
                &progressBackground,
                static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            std::wstring percentage =
                std::to_wstring(snapshot.progress) + L"%";
            SetTextColor(context, RGB(255, 255, 255));
            RECT percentRectangle = progressBackground;
            DrawTextW(
                context,
                percentage.data(),
                static_cast<int>(percentage.size()),
                &percentRectangle,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(context, smallFont);
            SetTextColor(context, RGB(166, 158, 178));
            RECT activityRectangle{24, 133, client.right - 24, 158};
            DrawTextW(
                context,
                snapshot.activity.data(),
                static_cast<int>(snapshot.activity.size()),
                &activityRectangle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(context, RGB(216, 165, 240));
            TextOutW(context, 24, 175, L"RELEASE NOTES", 13);
            SetTextColor(context, RGB(205, 199, 214));
            RECT notesRectangle{24, 202, client.right - 24, client.bottom - 20};
            DrawTextW(
                context,
                snapshot.releaseNotes.data(),
                static_cast<int>(snapshot.releaseNotes.size()),
                &notesRectangle,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

            DeleteObject(titleFont);
            DeleteObject(textFont);
            DeleteObject(smallFont);
            EndPaint(window, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    int argumentCount = 0;
    wchar_t** argumentValues = CommandLineToArgvW(
        GetCommandLineW(),
        &argumentCount);
    if (argumentValues == nullptr)
    {
        return 2;
    }
    const Arguments arguments = ParseArguments(
        argumentCount,
        argumentValues);
    LocalFree(argumentValues);
    if (arguments.selfTest)
    {
        return RunSelfTest();
    }
    if ((!arguments.bootstrap && !arguments.install) ||
        arguments.application.empty())
    {
        return 3;
    }

    try
    {
        winrt::init_apartment(
            winrt::apartment_type::multi_threaded);
    }
    catch (...)
    {
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(
        instance,
        MAKEINTRESOURCEW(IDI_VANTA));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"VantaUpdaterWindow";
    if (!RegisterClassExW(&windowClass))
    {
        return 4;
    }
    constexpr int width = 560;
    constexpr int height = 360;
    const int left = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int top = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    g_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        windowClass.lpszClassName,
        L"Vanta Updater",
        WS_POPUP | WS_VISIBLE,
        left,
        top,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (g_window == nullptr)
    {
        return 5;
    }
    SetLayeredWindowAttributes(
        g_window,
        0,
        248,
        LWA_ALPHA);
    SetWindowRgn(
        g_window,
        CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18),
        TRUE);

    std::thread worker([arguments]()
    {
        if (arguments.bootstrap)
        {
            RunBootstrap(arguments);
        }
        else
        {
            RunInstall(arguments);
        }
    });

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (worker.joinable())
    {
        worker.join();
    }
    UnregisterClassW(windowClass.lpszClassName, instance);
    winrt::uninit_apartment();
    return 0;
}
