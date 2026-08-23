#define IMGUI_DEFINE_MATH_OPERATORS

#include "makcu_controller.hpp"

#include "logger.hpp"
#include "rp2040_controller.hpp"

#include "makcu_c.h"

#include "imgui.h"
#include "imgui_settings.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr int kQuickClickHoldMinimumMilliseconds = 14;
    constexpr int kQuickClickHoldMaximumMilliseconds = 34;

    int HumanizedQuickClickHoldMilliseconds()
    {
        thread_local std::mt19937 generator{
            std::random_device{}()};
        std::uniform_int_distribution<int> distribution(
            kQuickClickHoldMinimumMilliseconds,
            kQuickClickHoldMaximumMilliseconds);
        // Averaging two samples produces natural center-weighted variation
        // while retaining short bounded click times.
        return (
            distribution(generator) +
            distribution(generator) +
            1) /
            2;
    }

    std::string MakcuErrorMessage(makcu_error_t error)
    {
        const char* message = makcu_error_string(error);
        return message != nullptr
            ? message
            : "Unknown error";
    }
}

namespace vanta
{
    struct MakcuController::Implementation
    {
        struct ConnectionResult
        {
            makcu_error_t error{
                MAKCU_ERROR_CONNECTION_FAILED};
            makcu_device_info_t device{};
            std::string version;
        };

        mutable std::mutex deviceMutex;
        Rp2040Controller rp2040;
        std::atomic<int> outputBackend{0};
        makcu_device_t* device{};
        std::vector<makcu_device_info_t> devices;
        std::vector<std::string> deviceLabels;
        std::future<ConnectionResult> connectionFuture;
        int selectedDevice{};
        std::string preferredPort;
        int movementX{};
        int movementY{};
        bool initialized{};
        bool connectionPending{};
        bool highPerformanceMode{true};
        bool autoDetectAndConnect{true};
        ULONGLONG nextAutoDetectTick{};
        std::atomic_bool connected{false};
        // Register this before connecting.  This matches the MAKCU C++ demo:
        // its serial listener is already armed when km.buttons(1) is sent as
        // part of Device::connect().
        std::atomic_bool buttonCallbackRegistered{false};
        std::atomic_bool buttonMonitoringReady{false};
        std::atomic<std::uint8_t> monitoredButtonMask{0};
        std::atomic<std::uint64_t> buttonEventCount{0};
        std::atomic_bool releasePending{false};
        std::atomic<std::uint64_t> settingsRevision{0};
        std::string status{
            "Scanning for MAKCU devices..."};
        std::string firmwareVersion;
        makcu_device_info_t connectedDevice{};

        static std::uint8_t ButtonMask(
            makcu_mouse_button_t button) noexcept
        {
            switch (button)
            {
            case MAKCU_MOUSE_LEFT:
                return 1U << 0;
            case MAKCU_MOUSE_RIGHT:
                return 1U << 1;
            case MAKCU_MOUSE_SIDE1:
                return 1U << 3;
            case MAKCU_MOUSE_SIDE2:
                return 1U << 4;
            default:
                return 0;
            }
        }

        static std::uint8_t ButtonMask(
            MouseButtonBinding button) noexcept
        {
            switch (button)
            {
            case MouseButtonBinding::left:
                return ButtonMask(MAKCU_MOUSE_LEFT);
            case MouseButtonBinding::right:
                return ButtonMask(MAKCU_MOUSE_RIGHT);
            case MouseButtonBinding::x1:
                return ButtonMask(MAKCU_MOUSE_SIDE1);
            case MouseButtonBinding::x2:
                return ButtonMask(MAKCU_MOUSE_SIDE2);
            default:
                return 0;
            }
        }

        static void ButtonCallback(
            makcu_mouse_button_t button,
            bool pressed,
            void* userData) noexcept
        {
            auto* implementation =
                static_cast<Implementation*>(userData);
            if (implementation == nullptr)
            {
                return;
            }

            const std::uint8_t mask = ButtonMask(button);
            if (mask == 0)
            {
                return;
            }

            if (pressed)
            {
                implementation->monitoredButtonMask.fetch_or(
                    mask,
                    std::memory_order_acq_rel);
            }
            else
            {
                implementation->monitoredButtonMask.fetch_and(
                    static_cast<std::uint8_t>(~mask),
                    std::memory_order_acq_rel);
            }
            implementation->buttonEventCount.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        makcu_error_t RegisterButtonCallbackLocked()
        {
            if (device == nullptr)
            {
                return MAKCU_ERROR_INVALID_DEVICE;
            }
            if (buttonCallbackRegistered.load(
                    std::memory_order_acquire))
            {
                return MAKCU_SUCCESS;
            }

            const makcu_error_t callbackError =
                makcu_set_mouse_button_callback(
                    device,
                    &ButtonCallback,
                    this);
            if (callbackError == MAKCU_SUCCESS)
            {
                buttonCallbackRegistered.store(
                    true,
                    std::memory_order_release);
            }
            return callbackError;
        }

        makcu_error_t EnableButtonMonitoringLocked()
        {
            if (device == nullptr ||
                !makcu_is_connected(device))
            {
                return MAKCU_ERROR_INVALID_DEVICE;
            }

            const makcu_error_t callbackError =
                RegisterButtonCallbackLocked();
            if (callbackError != MAKCU_SUCCESS)
            {
                return callbackError;
            }

            // Device::connect() already enables km.buttons(1), exactly as the
            // supplied MAKCU demo relies on.  Only send an explicit enable if
            // a device reports that the connection did not leave it enabled.
            bool monitoringEnabled = false;
            const makcu_error_t monitoringStateError =
                makcu_is_button_monitoring_enabled(
                    device,
                    &monitoringEnabled);
            if (monitoringStateError != MAKCU_SUCCESS)
            {
                return monitoringStateError;
            }
            if (!monitoringEnabled)
            {
                const makcu_error_t monitorError =
                    makcu_enable_button_monitoring(
                        device,
                        true);
                if (monitorError != MAKCU_SUCCESS)
                {
                    return monitorError;
                }
            }

            std::uint8_t initialMask = 0;
            // This is a direct read of makcu::Device::getButtonMask() through
            // the C facade.  It also covers an event that arrived while the
            // application was completing its connection setup.
            if (makcu_get_button_mask(
                    device,
                    &initialMask) != MAKCU_SUCCESS)
            {
                initialMask = 0;
            }
            monitoredButtonMask.store(
                initialMask,
                std::memory_order_release);
            buttonMonitoringReady.store(
                true,
                std::memory_order_release);
            return MAKCU_SUCCESS;
        }

        void DisableButtonMonitoringLocked()
        {
            buttonMonitoringReady.store(
                false,
                std::memory_order_release);
            monitoredButtonMask.store(
                0,
                std::memory_order_release);
            if (device == nullptr ||
                !makcu_is_connected(device))
            {
                return;
            }

            makcu_enable_button_monitoring(device, false);
        }

        void RefreshButtonMaskFromMakcu() noexcept
        {
            if (!IsMouseButtonInputAvailable())
            {
                return;
            }

            // The callback is the lowest-latency path.  This non-blocking
            // cached-mask read is a MAKCU-only fallback if a callback event is
            // missed, and never stalls mouse output while it is in use.
            std::unique_lock<std::mutex> lock(
                deviceMutex,
                std::try_to_lock);
            if (!lock.owns_lock() ||
                device == nullptr ||
                !makcu_is_connected(device))
            {
                return;
            }

            std::uint8_t mask = 0;
            if (makcu_get_button_mask(device, &mask) == MAKCU_SUCCESS)
            {
                monitoredButtonMask.store(
                    mask,
                    std::memory_order_release);
            }
        }

        bool IsMouseButtonInputAvailable() const noexcept
        {
            return
                connected.load(std::memory_order_acquire) &&
                buttonMonitoringReady.load(
                    std::memory_order_acquire);
        }

        bool IsMouseButtonPressed(
            MouseButtonBinding button) noexcept
        {
            const std::uint8_t mask = ButtonMask(button);
            RefreshButtonMaskFromMakcu();
            return
                mask != 0 &&
                IsMouseButtonInputAvailable() &&
                (monitoredButtonMask.load(
                    std::memory_order_acquire) &
                 mask) != 0;
        }

        bool ForceReleaseLocked(bool reportFailure = true)
        {
            if (device == nullptr ||
                !makcu_is_connected(device))
            {
                releasePending.store(
                    true,
                    std::memory_order_release);
                return false;
            }

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                if (makcu_mouse_up(
                        device,
                        MAKCU_MOUSE_LEFT) ==
                    MAKCU_SUCCESS)
                {
                    if (releasePending.exchange(
                            false,
                            std::memory_order_acq_rel))
                    {
                        vanta::log::Info(
                            "MAKCU left-button release recovered");
                    }
                    return true;
                }
                if (attempt != 2)
                {
                    Sleep(1);
                }
            }

            releasePending.store(
                true,
                std::memory_order_release);
            status =
                "MAKCU left-button release failed; recovery pending";
            if (reportFailure)
            {
                vanta::log::Warning(
                    "MAKCU left-button release failed after 3 attempts");
            }
            return false;
        }

        bool Initialize(
            const MouseOutputConfig* initialConfig)
        {
            if (initialized)
            {
                return true;
            }

            if (initialConfig != nullptr)
            {
                outputBackend.store(
                    std::clamp(
                        initialConfig->backendIndex,
                        0,
                        1),
                    std::memory_order_release);
                preferredPort =
                    initialConfig->makcuPort;
                highPerformanceMode =
                    initialConfig->
                        highPerformanceMode;
                autoDetectAndConnect =
                    initialConfig->
                        autoDetectAndConnect;
            }

            {
                std::lock_guard<std::mutex> lock(deviceMutex);
                device = makcu_device_create();
                if (device != nullptr)
                {
                    const makcu_error_t callbackError =
                        RegisterButtonCallbackLocked();
                    if (callbackError != MAKCU_SUCCESS)
                    {
                        status =
                            "Could not register the MAKCU button callback: " +
                            MakcuErrorMessage(callbackError);
                        makcu_device_destroy(device);
                        device = nullptr;
                        return false;
                    }
                }
            }
            if (device == nullptr)
            {
                status =
                    "Could not create the MAKCU device interface";
                vanta::log::Error(
                    "MAKCU 1.3.5 device creation failed");
                return false;
            }

            initialized = true;
            vanta::log::Info(
                "MAKCU 1.3.5 runtime loaded");
            rp2040.Initialize();
            if (autoDetectAndConnect)
            {
                TryAutoDetectAndConnect(true);
            }
            else
            {
                RefreshDevices();
            }
            return true;
        }

        void SelectOutputBackend(int backend)
        {
            const int selected =
                std::clamp(backend, 0, 1);
            const int previous =
                outputBackend.exchange(
                    selected,
                    std::memory_order_acq_rel);
            if (previous != selected)
            {
                settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }

        void RefreshDevices()
        {
            std::string selectedPort;
            if (selectedDevice >= 0 &&
                selectedDevice <
                    static_cast<int>(devices.size()))
            {
                selectedPort =
                    devices[static_cast<std::size_t>(
                        selectedDevice)].port;
            }
            if (selectedPort.empty())
            {
                selectedPort = preferredPort;
            }

            constexpr int maximumDevices = 32;
            std::array<
                makcu_device_info_t,
                maximumDevices> discovered{};
            const int count = makcu_find_devices(
                discovered.data(),
                static_cast<int>(discovered.size()));
            devices.assign(
                discovered.begin(),
                discovered.begin() +
                    std::clamp(
                        count,
                        0,
                        maximumDevices));

            deviceLabels.clear();
            deviceLabels.reserve(devices.size());
            selectedDevice = 0;
            for (std::size_t index = 0;
                 index < devices.size();
                 ++index)
            {
                const auto& entry = devices[index];
                deviceLabels.push_back(
                    std::string(entry.port) +
                    " | " +
                    entry.description);
                if (!selectedPort.empty() &&
                    entry.port == selectedPort)
                {
                    selectedDevice =
                        static_cast<int>(index);
                }
            }
            if (!devices.empty() &&
                selectedDevice >= 0 &&
                selectedDevice <
                    static_cast<int>(
                        devices.size()))
            {
                preferredPort =
                    devices[
                        static_cast<std::size_t>(
                            selectedDevice)]
                        .port;
            }

            if (!connected && !connectionPending)
            {
                status = devices.empty()
                    ? "No MAKCU device detected"
                    : std::to_string(devices.size()) +
                        " MAKCU device" +
                        (devices.size() == 1 ? "" : "s") +
                        " detected";
            }

            vanta::log::Info(
                "MAKCU scan complete: %zu device(s)",
                devices.size());
        }

        void BeginConnection()
        {
            if (device == nullptr ||
                connectionPending ||
                connected ||
                devices.empty() ||
                selectedDevice < 0 ||
                selectedDevice >=
                    static_cast<int>(devices.size()))
            {
                return;
            }

            const std::string port =
                devices[static_cast<std::size_t>(
                    selectedDevice)].port;
            const bool enableHighPerformance =
                highPerformanceMode;
            buttonMonitoringReady.store(
                false,
                std::memory_order_release);
            monitoredButtonMask.store(
                0,
                std::memory_order_release);
            buttonEventCount.store(
                0,
                std::memory_order_release);
            connectionPending = true;
            status = "Connecting to " + port + "...";
            vanta::log::Info(
                "MAKCU connection started: %s",
                port.c_str());

            connectionFuture = std::async(
                std::launch::async,
                [this, port, enableHighPerformance]()
                {
                    ConnectionResult result;
                    std::lock_guard<std::mutex> lock(deviceMutex);
                    if (device == nullptr)
                    {
                        result.error = MAKCU_ERROR_INVALID_DEVICE;
                        return result;
                    }
                    result.error =
                        makcu_connect(
                            device,
                            port.c_str());
                    if (result.error != MAKCU_SUCCESS)
                    {
                        return result;
                    }
                    if (!ForceReleaseLocked())
                    {
                        result.error =
                            MAKCU_ERROR_COMMAND_FAILED;
                        makcu_disconnect(device);
                        return result;
                    }

                    const makcu_error_t performanceError =
                        makcu_enable_high_performance_mode(
                            device,
                            enableHighPerformance);
                    if (performanceError != MAKCU_SUCCESS)
                    {
                        vanta::log::Warning(
                            "MAKCU high-performance mode setup failed: %s",
                            MakcuErrorMessage(
                                performanceError).c_str());
                    }

                    const makcu_error_t buttonMonitorError =
                        EnableButtonMonitoringLocked();
                    if (buttonMonitorError != MAKCU_SUCCESS)
                    {
                        vanta::log::Warning(
                            "MAKCU button monitoring setup failed: %s",
                            MakcuErrorMessage(
                                buttonMonitorError).c_str());
                        result.error = buttonMonitorError;
                        DisableButtonMonitoringLocked();
                        makcu_disconnect(device);
                        return result;
                    }

                    const makcu_error_t informationError =
                        makcu_get_device_info(
                            device,
                            &result.device);
                    if (informationError != MAKCU_SUCCESS)
                    {
                        result.device = {};
                        const std::size_t copyLength =
                            std::min(
                                port.size(),
                                sizeof(result.device.port) - 1);
                        std::copy_n(
                            port.data(),
                            copyLength,
                            result.device.port);
                        result.device.port[copyLength] = '\0';
                    }

                    std::array<char, 256> version{};
                    if (makcu_get_version(
                            device,
                            version.data(),
                            version.size()) ==
                        MAKCU_SUCCESS)
                    {
                        result.version = version.data();
                    }
                    return result;
                });
        }

        void TryAutoDetectAndConnect(bool force = false)
        {
            if (!initialized ||
                !autoDetectAndConnect ||
                connectionPending ||
                connected.load(std::memory_order_acquire))
            {
                return;
            }

            const ULONGLONG now = GetTickCount64();
            if (!force && now < nextAutoDetectTick)
            {
                return;
            }
            nextAutoDetectTick = now + 2000;

            // Always scan MAKCU first, even while RP2040 is active.
            // This lets a newly attached MAKCU take priority without
            // interrupting the currently working fallback device.
            RefreshDevices();
            if (!devices.empty())
            {
                BeginConnection();
                return;
            }

            if (!rp2040.IsConnected() &&
                rp2040.TryAutoConnect())
            {
                SelectOutputBackend(1);
                vanta::log::Info(
                    "Mouse output auto-selected RP2040 fallback");
            }
        }

        void CompleteConnectionIfReady()
        {
            if (!connectionPending ||
                !connectionFuture.valid() ||
                connectionFuture.wait_for(
                    std::chrono::milliseconds(0)) !=
                    std::future_status::ready)
            {
                return;
            }

            ConnectionResult result;
            try
            {
                result = connectionFuture.get();
            }
            catch (const std::exception& error)
            {
                status =
                    "Connection failed: " +
                    std::string(error.what());
                result.error =
                    MAKCU_ERROR_CONNECTION_FAILED;
            }

            connectionPending = false;
            const bool connectionSucceeded =
                result.error == MAKCU_SUCCESS &&
                DeviceIsConnected();
            connected.store(
                connectionSucceeded,
                std::memory_order_release);
            if (connectionSucceeded)
            {
                if (rp2040.IsConnected())
                {
                    rp2040.Disconnect();
                }
                SelectOutputBackend(0);
                connectedDevice = result.device;
                firmwareVersion =
                    std::move(result.version);
                status =
                    "Connected to " +
                    std::string(connectedDevice.port);
                vanta::log::Info(
                    "MAKCU connected: port=%s firmware=%s",
                    connectedDevice.port,
                    firmwareVersion.empty()
                        ? "unknown"
                        : firmwareVersion.c_str());
            }
            else
            {
                {
                    std::lock_guard<std::mutex> lock(deviceMutex);
                    if (device != nullptr)
                    {
                        DisableButtonMonitoringLocked();
                        makcu_disconnect(device);
                    }
                }
                firmwareVersion.clear();
                status =
                    "Connection failed: " +
                    MakcuErrorMessage(result.error);
                vanta::log::Warning(
                    "MAKCU connection failed: %s",
                    MakcuErrorMessage(
                        result.error).c_str());
                if (autoDetectAndConnect &&
                    !rp2040.IsConnected() &&
                    rp2040.TryAutoConnect())
                {
                    SelectOutputBackend(1);
                    vanta::log::Info(
                        "MAKCU unavailable; mouse output auto-selected RP2040");
                }
            }
        }

        void Disconnect()
        {
            if (connectionPending)
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(deviceMutex);
                if (device != nullptr &&
                    makcu_is_connected(device))
                {
                    DisableButtonMonitoringLocked();
                    ForceReleaseLocked();
                    makcu_disconnect(device);
                    vanta::log::Info("MAKCU disconnected");
                }
            }
            connected.store(false, std::memory_order_release);
            buttonMonitoringReady.store(
                false,
                std::memory_order_release);
            monitoredButtonMask.store(
                0,
                std::memory_order_release);
            connectedDevice = {};
            firmwareVersion.clear();
            status = devices.empty()
                ? "No MAKCU device detected"
                : "MAKCU disconnected";
        }

        void LogCommand(
            const char* command,
            makcu_error_t result)
        {
            if (result == MAKCU_SUCCESS)
            {
                vanta::log::Info(
                    "MAKCU command succeeded: %s",
                    command);
                return;
            }

            const std::string error =
                MakcuErrorMessage(result);
            status =
                std::string("Command failed: ") +
                command +
                " (" +
                error +
                ")";
            vanta::log::Warning(
                "MAKCU command failed: %s (%s)",
                command,
                error.c_str());
        }

        template <typename Command>
        makcu_error_t ExecuteCommand(Command&& command)
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (device == nullptr ||
                !connected.load(std::memory_order_acquire))
            {
                return MAKCU_ERROR_INVALID_DEVICE;
            }
            return command(device);
        }

        bool DeviceIsConnected() const
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            return
                device != nullptr &&
                makcu_is_connected(device);
        }

        bool TryClick()
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (device == nullptr ||
                !connected.load(std::memory_order_acquire))
            {
                return false;
            }
            if (releasePending.load(
                    std::memory_order_acquire) &&
                !ForceReleaseLocked())
            {
                return false;
            }

            const makcu_error_t downResult =
                makcu_mouse_down(
                    device,
                    MAKCU_MOUSE_LEFT);
            if (downResult != MAKCU_SUCCESS)
            {
                releasePending.store(
                    true,
                    std::memory_order_release);
                ForceReleaseLocked();
                if (!makcu_is_connected(device))
                {
                    connected.store(
                        false,
                        std::memory_order_release);
                }
                status =
                    "MAKCU left-button press failed";
                return false;
            }

            releasePending.store(
                true,
                std::memory_order_release);
            Sleep(
                static_cast<DWORD>(
                    HumanizedQuickClickHoldMilliseconds()));
            return ForceReleaseLocked();
        }

        bool TryMove(int x, int y)
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (device == nullptr ||
                !connected.load(std::memory_order_acquire))
            {
                return false;
            }
            if (releasePending.load(
                    std::memory_order_acquire) &&
                !ForceReleaseLocked())
            {
                return false;
            }

            const makcu_error_t result =
                makcu_mouse_move(device, x, y);
            if (result != MAKCU_SUCCESS &&
                !makcu_is_connected(device))
            {
                connected.store(
                    false,
                    std::memory_order_release);
            }
            return result == MAKCU_SUCCESS;
        }

        bool ForceReleaseLeftButton()
        {
            if (outputBackend.load(
                    std::memory_order_acquire) == 1)
            {
                return rp2040.ForceReleaseLeftButton();
            }
            std::lock_guard<std::mutex> lock(deviceMutex);
            return ForceReleaseLocked();
        }

        void ForceReleaseBackendIfConnected(int backend)
        {
            if (backend == 1)
            {
                if (rp2040.IsConnected())
                {
                    rp2040.ForceReleaseLeftButton();
                }
                return;
            }

            std::lock_guard<std::mutex> lock(deviceMutex);
            if (device != nullptr &&
                connected.load(std::memory_order_acquire) &&
                makcu_is_connected(device))
            {
                ForceReleaseLocked();
            }
        }

        void RecoverReleaseIfNeeded()
        {
            if (!releasePending.load(
                    std::memory_order_acquire))
            {
                return;
            }
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (releasePending.load(
                    std::memory_order_acquire))
            {
                ForceReleaseLocked();
            }
        }

        void Shutdown()
        {
            if (!initialized)
            {
                return;
            }
            if (connectionPending &&
                connectionFuture.valid())
            {
                connectionFuture.wait();
                CompleteConnectionIfReady();
            }
            rp2040.Shutdown();
            Disconnect();
            {
                std::lock_guard<std::mutex> lock(deviceMutex);
                if (device != nullptr)
                {
                    if (makcu_is_connected(device))
                    {
                        ForceReleaseLocked();
                    }
                    makcu_device_destroy(device);
                    device = nullptr;
                }
            }
            buttonCallbackRegistered.store(
                false,
                std::memory_order_release);
            buttonMonitoringReady.store(
                false,
                std::memory_order_release);
            monitoredButtonMask.store(
                0,
                std::memory_order_release);
            devices.clear();
            deviceLabels.clear();
            autoDetectAndConnect = false;
            initialized = false;
            vanta::log::Info(
                "MAKCU 1.3.5 runtime released");
        }
    };

    MakcuController::MakcuController()
        : implementation_(
              std::make_unique<Implementation>())
    {
    }

    MakcuController::~MakcuController()
    {
        Shutdown();
    }

    bool MakcuController::Initialize(
        const MouseOutputConfig* initialConfig)
    {
        return implementation_->Initialize(
            initialConfig);
    }

    void MakcuController::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }

    void MakcuController::Tick()
    {
        auto& implementation = *implementation_;
        implementation.rp2040.Tick();
        implementation.CompleteConnectionIfReady();
        implementation.RefreshButtonMaskFromMakcu();
        implementation.RecoverReleaseIfNeeded();
        if (implementation.connected.load(
                std::memory_order_acquire) &&
            !implementation.DeviceIsConnected())
        {
            implementation.connected.store(
                false,
                std::memory_order_release);
            implementation.buttonMonitoringReady.store(
                false,
                std::memory_order_release);
            implementation.monitoredButtonMask.store(
                0,
                std::memory_order_release);
            implementation.connectedDevice = {};
            implementation.firmwareVersion.clear();
            implementation.status =
                "MAKCU connection was lost";
            vanta::log::Warning(
                "MAKCU connection was lost");
            implementation.nextAutoDetectTick = 0;
        }
        implementation.TryAutoDetectAndConnect();
    }

    void MakcuController::RenderPanel()
    {
        auto& implementation = *implementation_;
        const float bodyHeight = std::max(
            190.0F,
            ImGui::GetContentRegionAvail().y - 40.0F);
        custom::Child(
            ICON_USB_LINE
                "  Mouse output##mouse-output",
            ImVec2(0.0F, bodyHeight),
            true);

        const char* outputBackends[]{
            "MAKCU 1.3.5",
            "RP2040 USB Host"};
        int selectedBackend =
            implementation.outputBackend.load(
                std::memory_order_acquire);
        if (custom::Combo(
                "Output backend",
                &selectedBackend,
                outputBackends,
                static_cast<int>(
                    sizeof(outputBackends) /
                    sizeof(outputBackends[0]))))
        {
            implementation.ForceReleaseBackendIfConnected(
                implementation.outputBackend.load(
                    std::memory_order_acquire));
            implementation.SelectOutputBackend(
                selectedBackend);
        }

        bool autoConnect =
            implementation.autoDetectAndConnect;
        if (custom::Checkbox(
                "Auto detect / connect (MAKCU preferred)",
                &autoConnect))
        {
            implementation.autoDetectAndConnect =
                autoConnect;
            implementation.nextAutoDetectTick = 0;
            implementation.settingsRevision.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        custom::Separator();

        if (selectedBackend == 1)
        {
            implementation.rp2040.RenderPanel();
            custom::EndChild();
            return;
        }

        const ImVec4 statusColor =
            implementation.connected
            ? ImVec4(0.45F, 0.96F, 0.65F, 1.0F)
            : implementation.connectionPending
                ? ImVec4(1.0F, 0.75F, 0.32F, 1.0F)
                : ImVec4(0.72F, 0.72F, 0.78F, 1.0F);
        ImGui::TextColored(
            statusColor,
            "%s",
            implementation.status.c_str());
        ImGui::TextDisabled(
            "Runtime: makcu-cpp 1.3.5 Win64");
        ImGui::Spacing();

        std::vector<const char*> deviceItems;
        deviceItems.reserve(
            implementation.deviceLabels.size());
        for (const auto& label :
             implementation.deviceLabels)
        {
            deviceItems.push_back(label.c_str());
        }

        const char* emptyDeviceItems[]{
            "No MAKCU device found"};
        ImGui::BeginDisabled(
            implementation.connectionPending ||
            implementation.connected ||
            deviceItems.empty());
        if (custom::Combo(
            ICON_DEVICE_LINE "  Device",
            &implementation.selectedDevice,
            deviceItems.empty()
                ? emptyDeviceItems
                : deviceItems.data(),
            deviceItems.empty()
                ? 1
                : static_cast<int>(deviceItems.size())))
        {
            if (implementation.selectedDevice >= 0 &&
                implementation.selectedDevice <
                    static_cast<int>(
                        implementation.devices.size()))
            {
                implementation.preferredPort =
                    implementation.devices[
                        static_cast<std::size_t>(
                            implementation.selectedDevice)]
                        .port;
            }
            implementation.settingsRevision.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(
            implementation.connectionPending);
        if (custom::Button(
                ICON_REFRESH_1_LINE "  Rescan devices",
                ImVec2(180.0F, 36.0F)))
        {
            implementation.RefreshDevices();
        }
        ImGui::SameLine();
        if (!implementation.connected)
        {
            ImGui::BeginDisabled(
                implementation.devices.empty());
            if (custom::Button(
                    ICON_LINK_LINE "  Connect",
                    ImVec2(150.0F, 36.0F)))
            {
                implementation.BeginConnection();
            }
            ImGui::EndDisabled();
        }
        else if (custom::Button(
                     ICON_UNLINK_LINE "  Disconnect",
                     ImVec2(160.0F, 36.0F)))
        {
            implementation.Disconnect();
        }
        ImGui::EndDisabled();

        if (implementation.connected)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text(
                "Port: %s",
                implementation.connectedDevice.port);
            ImGui::Text(
                "VID:PID  %04X:%04X",
                implementation.connectedDevice.vid,
                implementation.connectedDevice.pid);
            ImGui::Text(
                "Firmware: %s",
                implementation.firmwareVersion.empty()
                    ? "unknown"
                    : implementation.firmwareVersion.c_str());

            const bool inputMonitorReady =
                implementation.buttonMonitoringReady.load(
                    std::memory_order_acquire);
            const std::uint8_t physicalButtonMask =
                implementation.monitoredButtonMask.load(
                    std::memory_order_acquire);
            const auto isPhysicalButtonHeld =
                [&](MouseButtonBinding button)
                {
                    return
                        (physicalButtonMask &
                         implementation.ButtonMask(button)) != 0;
                };
            const unsigned long long buttonEvents =
                static_cast<unsigned long long>(
                    implementation.buttonEventCount.load(
                        std::memory_order_relaxed));
            ImGui::TextDisabled(
                "Input monitor (MAKCU): %s | Events: %llu",
                inputMonitorReady ? "ready" : "starting",
                buttonEvents);
            ImGui::TextDisabled(
                "Physical buttons: L %s | R %s | X1 %s | X2 %s",
                isPhysicalButtonHeld(MouseButtonBinding::left)
                    ? "DOWN" : "up",
                isPhysicalButtonHeld(MouseButtonBinding::right)
                    ? "DOWN" : "up",
                isPhysicalButtonHeld(MouseButtonBinding::x1)
                    ? "DOWN" : "up",
                isPhysicalButtonHeld(MouseButtonBinding::x2)
                    ? "DOWN" : "up");

            bool highPerformance =
                implementation.highPerformanceMode;
            if (custom::Checkbox(
                    "High performance mode",
                    &highPerformance))
            {
                implementation.highPerformanceMode =
                    highPerformance;
                implementation.settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
                implementation.LogCommand(
                    "high performance mode",
                    implementation.ExecuteCommand(
                        [highPerformance](makcu_device_t* device)
                        {
                            return makcu_enable_high_performance_mode(
                                device,
                                highPerformance);
                        }));
            }

            ImGui::Spacing();
            custom::SliderInt(
                "Horizontal movement",
                &implementation.movementX,
                -500,
                500,
                "%d px");
            custom::SliderInt(
                "Vertical movement",
                &implementation.movementY,
                -500,
                500,
                "%d px");

            if (custom::Button(
                    ICON_MOUSE_LINE "  Move",
                    ImVec2(145.0F, 36.0F)))
            {
                implementation.LogCommand(
                    "mouse move",
                    implementation.ExecuteCommand(
                        [&](makcu_device_t* device)
                        {
                            return makcu_mouse_move(
                                device,
                                implementation.movementX,
                                implementation.movementY);
                        }));
            }
            ImGui::SameLine();
            if (custom::Button(
                    "Left click",
                    ImVec2(130.0F, 36.0F)))
            {
                const bool result =
                    implementation.TryClick();
                vanta::log::Info(
                    "MAKCU left click: %s",
                    result
                        ? "success"
                        : "failed or release recovery pending");
            }
            ImGui::SameLine();
            if (custom::Button(
                    "Right click",
                    ImVec2(130.0F, 36.0F)))
            {
                implementation.LogCommand(
                    "right click",
                    implementation.ExecuteCommand(
                        [](makcu_device_t* device)
                        {
                            return makcu_mouse_click(
                                device,
                                MAKCU_MOUSE_RIGHT);
                        }));
            }

            if (custom::Button(
                    "Wheel +1",
                    ImVec2(145.0F, 36.0F)))
            {
                implementation.LogCommand(
                    "wheel +1",
                    implementation.ExecuteCommand(
                        [](makcu_device_t* device)
                        {
                            return makcu_mouse_wheel(device, 1);
                        }));
            }
            ImGui::SameLine();
            if (custom::Button(
                    "Wheel -1",
                    ImVec2(145.0F, 36.0F)))
            {
                implementation.LogCommand(
                    "wheel -1",
                    implementation.ExecuteCommand(
                        [](makcu_device_t* device)
                        {
                            return makcu_mouse_wheel(device, -1);
                        }));
            }
        }

        custom::EndChild();
    }

    bool MakcuController::TryClick()
    {
        auto& impl = *implementation_;
        if (impl.outputBackend.load(
                std::memory_order_acquire) == 1)
        {
            return impl.rp2040.TryClick();
        }
        return impl.TryClick();
    }

    bool MakcuController::IsConnected() const noexcept
    {
        if (implementation_->outputBackend.load(
                std::memory_order_acquire) == 1)
        {
            return implementation_->rp2040.IsConnected();
        }
        return implementation_->connected.load(
            std::memory_order_acquire);
    }

    bool MakcuController::IsMouseButtonInputAvailable() const noexcept
    {
        return implementation_->
            IsMouseButtonInputAvailable();
    }

    bool MakcuController::IsMouseButtonPressed(
        MouseButtonBinding button) const noexcept
    {
        return implementation_->IsMouseButtonPressed(button);
    }

    bool MakcuController::TryMove(int x, int y)
    {
        auto& impl = *implementation_;
        if (impl.outputBackend.load(
                std::memory_order_acquire) == 1)
        {
            return impl.rp2040.TryMove(x, y);
        }
        return impl.TryMove(x, y);
    }

    bool MakcuController::ForceReleaseLeftButton()
    {
        return implementation_->ForceReleaseLeftButton();
    }

    MouseOutputConfig MakcuController::GetConfig() const
    {
        const auto& impl = *implementation_;
        MouseOutputConfig result;
        result.backendIndex =
            impl.outputBackend.load(
                std::memory_order_acquire);
        result.makcuPort =
            impl.preferredPort;
        result.highPerformanceMode =
            impl.highPerformanceMode;
        result.autoDetectAndConnect =
            impl.autoDetectAndConnect;
        return result;
    }

    void MakcuController::ApplyConfig(
        const MouseOutputConfig& config)
    {
        auto& impl = *implementation_;
        const int configuredBackend =
            std::clamp(
                config.backendIndex,
                0,
                1);
        const int previousBackend =
            impl.outputBackend.load(
                std::memory_order_acquire);
        if (impl.initialized &&
            configuredBackend != previousBackend)
        {
            impl.ForceReleaseBackendIfConnected(
                previousBackend);
        }
        impl.outputBackend.store(
            configuredBackend,
            std::memory_order_release);
        impl.preferredPort =
            config.makcuPort;
        impl.highPerformanceMode =
            config.highPerformanceMode;
        const bool autoConnectChanged =
            impl.autoDetectAndConnect !=
                config.autoDetectAndConnect;
        impl.autoDetectAndConnect =
            config.autoDetectAndConnect;
        if (impl.initialized)
        {
            if (autoConnectChanged &&
                impl.autoDetectAndConnect)
            {
                impl.nextAutoDetectTick = 0;
            }
            else if (!impl.autoDetectAndConnect)
            {
                impl.RefreshDevices();
            }
        }
        impl.settingsRevision.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    std::uint64_t
    MakcuController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }
}
