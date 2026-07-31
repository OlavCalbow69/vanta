#include "update_controller.hpp"

#include "logger.hpp"
#include "update_shared.hpp"
#include "version.hpp"

#include <Windows.h>
#include <winrt/base.h>

#include "custom_widgets.hpp"
#include "font_defines.h"
#include "imgui.h"
#include "imgui_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{
    enum class UpdateState
    {
        idle,
        checking,
        upToDate,
        available,
        downloading,
        ready,
        error
    };

    bool HasArgument(
        int argumentCount,
        wchar_t** arguments,
        const wchar_t* expected)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            if (_wcsicmp(arguments[index], expected) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool ShouldBypassBootstrap(
        int argumentCount,
        wchar_t** arguments)
    {
        constexpr const wchar_t* bypassArguments[]{
            L"--updater-launched",
            L"--no-update-bootstrap",
            L"--help",
            L"--self-test",
            L"--config-self-test",
            L"--capture-test-winrt",
            L"--capture-test-duplication",
            L"--capture-test-winrt-window",
            L"--capture-test-duplication-window"};
        for (const wchar_t* argument : bypassArguments)
        {
            if (HasArgument(
                    argumentCount,
                    arguments,
                    argument))
            {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path CachedUpdaterPath()
    {
        return vanta::updates::UpdateRoot() /
            L"runner" / L"vanta-updater.exe";
    }

    bool CopyUpdaterToCache(
        const std::filesystem::path& installedUpdater,
        std::filesystem::path& cachedUpdater,
        std::string& error)
    {
        cachedUpdater = CachedUpdaterPath();
        std::error_code directoryError;
        std::filesystem::create_directories(
            cachedUpdater.parent_path(),
            directoryError);
        if (directoryError)
        {
            error = "could not create updater runner directory: " +
                directoryError.message();
            return false;
        }
        if (!CopyFileW(
                installedUpdater.c_str(),
                cachedUpdater.c_str(),
                FALSE))
        {
            error = "could not stage vanta-updater.exe (error " +
                std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }

    const char* StateTitle(
        UpdateState updateState,
        const vanta::updates::ReleaseInfo& release)
    {
        switch (updateState)
        {
        case UpdateState::checking:
            return "Checking for updates";
        case UpdateState::upToDate:
            return "Up to date";
        case UpdateState::available:
        case UpdateState::downloading:
        case UpdateState::ready:
            return release.tag.empty()
                ? "Update available"
                : "Update available";
        case UpdateState::error:
            return "Update check failed";
        default:
            return "Update service ready";
        }
    }
}

namespace vanta
{
    bool LaunchUpdateBootstrapIfNeeded(
        int argumentCount,
        wchar_t** arguments,
        bool& launched)
    {
        launched = false;
#if !VANTA_AUTOMATED_RELEASE_BUILD
        (void)argumentCount;
        (void)arguments;
        return true;
#else
        if (ShouldBypassBootstrap(argumentCount, arguments))
        {
            return true;
        }
        const std::filesystem::path application =
            updates::CurrentExecutablePath();
        if (application.empty())
        {
            log::Warning(
                "update bootstrap skipped: executable path unavailable");
            return true;
        }
        const std::filesystem::path installedUpdater =
            application.parent_path() / L"vanta-updater.exe";
        if (!std::filesystem::is_regular_file(installedUpdater))
        {
            log::Warning(
                "update bootstrap skipped: vanta-updater.exe is missing");
            return true;
        }
        std::filesystem::path cachedUpdater;
        std::string error;
        if (!CopyUpdaterToCache(
                installedUpdater,
                cachedUpdater,
                error))
        {
            log::Warning(
                "update bootstrap skipped: %s",
                error.c_str());
            return true;
        }
        const std::wstring updaterArguments =
            L"--bootstrap --app " +
            updates::QuoteArgument(application.wstring()) +
            L" --current " +
            updates::QuoteArgument(VANTA_VERSION_WSTRING) +
            L" --pid " +
            std::to_wstring(GetCurrentProcessId());
        if (!updates::StartProcess(
                cachedUpdater,
                updaterArguments,
                error))
        {
            log::Warning(
                "update bootstrap could not start: %s",
                error.c_str());
            return true;
        }
        launched = true;
        log::Info(
            "update bootstrap started; handing off to vanta-updater.exe");
        return true;
#endif
    }

    struct UpdateController::Implementation
    {
        mutable std::mutex mutex;
        UpdateConfig config;
        updates::ReleaseInfo release;
        UpdateState state{UpdateState::idle};
        std::string activity{"Waiting"};
        std::string error;
        std::filesystem::path downloadedZip;
        std::string verifiedSha256;
        int progress{};
        bool installOnExit{};
        bool networkEnabled{true};
        bool initialized{};
        bool shutdown{};
        std::thread worker;
        std::atomic_bool workerRunning{false};
        std::atomic_bool stopRequested{false};
        std::atomic<std::uint64_t> settingsRevision{0};

        ~Implementation()
        {
            StopWorker();
        }

        void SavePreferences()
        {
            updates::UpdaterPreferences preferences;
            {
                std::lock_guard<std::mutex> lock(mutex);
                preferences.automaticChecks =
                    config.automaticChecks;
                preferences.automaticDownloads =
                    config.automaticDownloads;
                preferences.silentAutomaticInstallation =
                    config.silentAutomaticInstallation;
            }
            std::string saveError;
            if (!updates::SaveUpdaterPreferences(
                    preferences,
                    saveError))
            {
                log::Warning(
                    "could not save updater preferences: %s",
                    saveError.c_str());
            }
        }

        void StopWorker()
        {
            stopRequested.store(true, std::memory_order_release);
            if (worker.joinable())
            {
                worker.join();
            }
            workerRunning.store(false, std::memory_order_release);
        }

        bool ShouldStop() const noexcept
        {
            return stopRequested.load(std::memory_order_acquire);
        }

        void SetError(const std::string& message)
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = UpdateState::error;
            error = message;
            activity = message;
            progress = 0;
            log::Warning("update service: %s", message.c_str());
        }

        void ProcessRelease(
            const updates::ReleaseInfo& latest)
        {
            bool automaticDownload = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                release = latest;
                error.clear();
                if (!updates::IsReleaseNewer(
                        release.tag,
                        VANTA_VERSION_STRING))
                {
                    state = UpdateState::upToDate;
                    activity = "Latest release is already installed";
                    progress = 100;
                    return;
                }
                state = UpdateState::available;
                activity = "Update " + release.tag + " available";
                progress = 0;
                automaticDownload = config.automaticDownloads;
            }
            if (automaticDownload && !ShouldStop())
            {
                DownloadCurrentRelease();
            }
        }

        void DownloadCurrentRelease()
        {
            updates::ReleaseInfo current;
            {
                std::lock_guard<std::mutex> lock(mutex);
                current = release;
                state = UpdateState::downloading;
                activity = "Preparing download";
                progress = 0;
                error.clear();
            }
            std::filesystem::path zip;
            std::string sha256;
            std::string downloadError;
            const bool succeeded = updates::DownloadRelease(
                current,
                zip,
                sha256,
                [&](int percentage, const std::string& text)
                {
                    if (ShouldStop())
                    {
                        return false;
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    progress = std::clamp(percentage, 0, 100);
                    activity = text;
                    return true;
                },
                downloadError);
            if (!succeeded)
            {
                if (!ShouldStop())
                {
                    SetError(downloadError);
                }
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            downloadedZip = std::move(zip);
            verifiedSha256 = std::move(sha256);
            state = UpdateState::ready;
            progress = 100;
            activity = "Download verified and ready to install";
            if (config.silentAutomaticInstallation)
            {
                installOnExit = true;
                activity = "Verified; installation scheduled on exit";
            }
        }

        void BeginCheck(bool force)
        {
            if (!networkEnabled || shutdown)
            {
                return;
            }
            if (workerRunning.exchange(
                    true,
                    std::memory_order_acq_rel))
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            stopRequested.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mutex);
                state = UpdateState::checking;
                activity = force
                    ? "Checking GitHub now"
                    : "Checking cached release information";
                progress = 5;
                error.clear();
            }
            worker = std::thread([this, force]()
            {
                bool apartmentInitialized = false;
                try
                {
                    winrt::init_apartment(
                        winrt::apartment_type::multi_threaded);
                    apartmentInitialized = true;
                }
                catch (...)
                {
                }

                updates::ReleaseInfo latest;
                std::int64_t checkedAt = 0;
                std::string operationError;
                bool loaded = false;
                if (!force &&
                    updates::LoadCachedRelease(
                        latest,
                        checkedAt,
                        operationError) &&
                    updates::IsCacheFresh(checkedAt))
                {
                    loaded = true;
                    std::lock_guard<std::mutex> lock(mutex);
                    activity = "Using today's cached update check";
                    progress = 35;
                }
                if (!loaded && !force && !ShouldStop())
                {
                    std::int64_t lastAttempt = 0;
                    std::string attemptError;
                    if (updates::LoadLastCheckAttempt(
                            lastAttempt,
                            attemptError) &&
                        updates::IsCacheFresh(lastAttempt))
                    {
                        SetError(
                            "Today's automatic update check was unavailable; "
                            "use Check for updates to retry now");
                        if (apartmentInitialized)
                        {
                            winrt::uninit_apartment();
                        }
                        workerRunning.store(
                            false,
                            std::memory_order_release);
                        return;
                    }
                }
                if (!loaded && !ShouldStop())
                {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        activity = "Checking GitHub releases/latest";
                        progress = 20;
                    }
                    operationError.clear();
                    std::string attemptError;
                    updates::SaveLastCheckAttempt(
                        updates::CurrentUnixTime(),
                        attemptError);
                    if (updates::FetchLatestRelease(
                            latest,
                            operationError))
                    {
                        std::string cacheError;
                        if (!updates::SaveCachedRelease(
                                latest,
                                updates::CurrentUnixTime(),
                                cacheError))
                        {
                            log::Warning(
                                "could not cache update result: %s",
                                cacheError.c_str());
                        }
                        loaded = true;
                    }
                }
                if (!ShouldStop())
                {
                    if (loaded)
                    {
                        ProcessRelease(latest);
                    }
                    else
                    {
                        SetError(operationError.empty()
                            ? "Update check failed"
                            : operationError);
                    }
                }
                if (apartmentInitialized)
                {
                    winrt::uninit_apartment();
                }
                workerRunning.store(false, std::memory_order_release);
            });
        }

        void BeginDownload()
        {
            if (!networkEnabled || shutdown)
            {
                return;
            }
            if (workerRunning.exchange(
                    true,
                    std::memory_order_acq_rel))
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            stopRequested.store(false, std::memory_order_release);
            worker = std::thread([this]()
            {
                bool apartmentInitialized = false;
                try
                {
                    winrt::init_apartment(
                        winrt::apartment_type::multi_threaded);
                    apartmentInitialized = true;
                }
                catch (...)
                {
                }
                DownloadCurrentRelease();
                if (apartmentInitialized)
                {
                    winrt::uninit_apartment();
                }
                workerRunning.store(false, std::memory_order_release);
            });
        }

        bool LaunchInstaller()
        {
            std::filesystem::path zip;
            std::string sha256;
            std::string tag;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!installOnExit ||
                    state != UpdateState::ready ||
                    downloadedZip.empty())
                {
                    return false;
                }
                zip = downloadedZip;
                sha256 = verifiedSha256;
                tag = release.tag;
            }
            const std::filesystem::path application =
                updates::CurrentExecutablePath();
            const std::filesystem::path installedUpdater =
                application.parent_path() / L"vanta-updater.exe";
            std::filesystem::path cachedUpdater;
            std::string launchError;
            if (!CopyUpdaterToCache(
                    installedUpdater,
                    cachedUpdater,
                    launchError))
            {
                log::Error(
                    "could not stage updater on exit: %s",
                    launchError.c_str());
                return false;
            }
            const std::wstring arguments =
                L"--install --app " +
                updates::QuoteArgument(application.wstring()) +
                L" --zip " +
                updates::QuoteArgument(zip.wstring()) +
                L" --sha256 " +
                updates::QuoteArgument(
                    winrt::to_hstring(sha256).c_str()) +
                L" --tag " +
                updates::QuoteArgument(
                    winrt::to_hstring(tag).c_str()) +
                L" --pid " +
                std::to_wstring(GetCurrentProcessId());
            if (!updates::StartProcess(
                    cachedUpdater,
                    arguments,
                    launchError))
            {
                log::Error(
                    "could not start updater on exit: %s",
                    launchError.c_str());
                return false;
            }
            log::Info(
                "verified update %s handed to updater for installation",
                tag.c_str());
            return true;
        }

        void RenderPanel()
        {
            const float bodyHeight = std::max(
                190.0F,
                ImGui::GetContentRegionAvail().y - 40.0F);
            custom::Child(
                ICON_DOWNLOAD_2_LINE "  Updates##updates-panel",
                ImVec2(0.0F, bodyHeight),
                true);
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));

            UpdateState currentState;
            updates::ReleaseInfo currentRelease;
            UpdateConfig currentConfig;
            std::string currentActivity;
            std::string currentError;
            int currentProgress = 0;
            bool scheduled = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                currentState = state;
                currentRelease = release;
                currentConfig = config;
                currentActivity = activity;
                currentError = error;
                currentProgress = progress;
                scheduled = installOnExit;
            }

            const ImVec4 statusColor =
                currentState == UpdateState::error
                ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                : currentState == UpdateState::available ||
                      currentState == UpdateState::downloading ||
                      currentState == UpdateState::ready
                    ? ImVec4(1.0F, 0.75F, 0.32F, 1.0F)
                    : ImVec4(0.45F, 0.96F, 0.65F, 1.0F);
            std::string title = StateTitle(
                currentState,
                currentRelease);
            if ((currentState == UpdateState::available ||
                 currentState == UpdateState::downloading ||
                 currentState == UpdateState::ready) &&
                !currentRelease.tag.empty())
            {
                title = "Update " + currentRelease.tag + " available";
            }
            ImGui::TextColored(
                statusColor,
                "%s",
                title.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(
                "Installed %s",
                VANTA_VERSION_STRING);

            char overlay[16]{};
            std::snprintf(
                overlay,
                sizeof(overlay),
                "%d%%",
                std::clamp(currentProgress, 0, 100));
            ImGui::PushStyleColor(
                ImGuiCol_FrameBg,
                c::elements::background);
            ImGui::PushStyleColor(
                ImGuiCol_PlotHistogram,
                c::main_color.Value);
            ImGui::ProgressBar(
                std::clamp(currentProgress / 100.0F, 0.0F, 1.0F),
                ImVec2(-1.0F, 18.0F),
                overlay);
            ImGui::PopStyleColor(2);
            ImGui::TextDisabled(
                "%s",
                currentActivity.c_str());

            if (custom::Button(
                    ICON_REFRESH_1_LINE "  Check for updates",
                    ImVec2(190.0F, 36.0F)))
            {
                BeginCheck(true);
            }
            if (currentState == UpdateState::available)
            {
                ImGui::SameLine();
                if (custom::Button(
                        ICON_DOWNLOAD_LINE "  Download",
                        ImVec2(145.0F, 36.0F)))
                {
                    BeginDownload();
                }
            }
            if (currentState == UpdateState::ready)
            {
                ImGui::SameLine();
                const char* label = scheduled
                    ? ICON_REFRESH_1_LINE "  Cancel install"
                    : ICON_ROCKET_LINE "  Install on exit";
                if (custom::Button(label, ImVec2(170.0F, 36.0F)))
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    installOnExit = !installOnExit;
                }
            }

            custom::Separator();
            bool changed = false;
            changed |= custom::Checkbox(
                "Automatic update checks",
                &currentConfig.automaticChecks);
            changed |= custom::Checkbox(
                "Automatic downloads",
                &currentConfig.automaticDownloads);
            changed |= custom::Checkbox(
                "Silent automatic installation",
                &currentConfig.silentAutomaticInstallation);
            if (changed)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    config = currentConfig;
                    if (!config.silentAutomaticInstallation)
                    {
                        installOnExit = false;
                    }
                }
                settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
                SavePreferences();
            }

            custom::Separator();
            ImGui::Text("Release notes");
            ImGui::BeginChild(
                "##update-release-notes",
                ImVec2(0.0F, 145.0F),
                true,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (currentRelease.notes.empty())
            {
                ImGui::TextDisabled(
                    "Release notes will appear after an update check.");
            }
            else
            {
                ImGui::TextWrapped(
                    "%s",
                    currentRelease.notes.c_str());
            }
            ImGui::EndChild();
            if (!currentError.empty())
            {
                ImGui::TextColored(
                    ImVec4(1.0F, 0.35F, 0.35F, 1.0F),
                    "%s",
                    currentError.c_str());
            }

            ImGui::PopStyleVar();
            custom::EndChild();
        }
    };

    UpdateController::UpdateController()
        : implementation_(std::make_unique<Implementation>())
    {
    }

    UpdateController::~UpdateController()
    {
        Shutdown(false);
    }

    void UpdateController::Initialize(
        const UpdateConfig& initialConfig,
        bool networkEnabled)
    {
        auto& impl = *implementation_;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.config = initialConfig;
            impl.networkEnabled = networkEnabled;
            impl.initialized = true;
            impl.shutdown = false;
            if (!networkEnabled)
            {
                impl.activity =
                    "Local development build; automatic updates are disabled";
            }
        }
        impl.SavePreferences();
        if (initialConfig.automaticChecks && networkEnabled)
        {
            impl.BeginCheck(false);
        }
    }

    void UpdateController::Shutdown(bool launchInstaller)
    {
        auto& impl = *implementation_;
        if (!impl.initialized || impl.shutdown)
        {
            return;
        }
        impl.shutdown = true;
        impl.StopWorker();
        if (launchInstaller)
        {
            impl.LaunchInstaller();
        }
        impl.initialized = false;
    }

    void UpdateController::RenderPanel()
    {
        implementation_->RenderPanel();
    }

    UpdateConfig UpdateController::GetConfig() const
    {
        std::lock_guard<std::mutex> lock(
            implementation_->mutex);
        return implementation_->config;
    }

    void UpdateController::ApplyConfig(
        const UpdateConfig& config)
    {
        {
            std::lock_guard<std::mutex> lock(
                implementation_->mutex);
            implementation_->config = config;
        }
        implementation_->SavePreferences();
        implementation_->settingsRevision.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    std::uint64_t
    UpdateController::SettingsRevision() const noexcept
    {
        return implementation_->settingsRevision.load(
            std::memory_order_relaxed);
    }
}
