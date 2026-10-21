#define IMGUI_DEFINE_MATH_OPERATORS

#include "makcu_controller.hpp"
#include "medius_backend.hpp"
#include "input_backend_utils.hpp"
#include <Windows.h>
#include <winrt/base.h>

#include "logger.hpp"
#include "stream_frame.hpp"

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
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct MakcuPortChoices
    {
        std::vector<makcu_device_info_t> devices;
        std::size_t detectedCount{};
        int selected{};
        std::string detectedPorts;
    };
    MakcuPortChoices SelectMakcuPorts(std::vector<makcu_device_info_t> detected,
        const std::string& previous)
    {
        MakcuPortChoices result;
        result.devices=std::move(detected);
        result.detectedCount=result.devices.size();
        for(const auto& entry:result.devices)
        {
            if(!result.detectedPorts.empty())result.detectedPorts+=", ";
            result.detectedPorts+=entry.port;
        }
        for(std::size_t index=0;index<result.devices.size();++index)
            if(previous==result.devices[index].port)result.selected=static_cast<int>(index);
        return result;
    }
    std::string DiagnosticString(const std::string& value)
    {
        std::string result="\"";
        for(unsigned char c:value){if(c=='"' || c=='\\')result+='\\';if(c>=32)result+=char(c);}
        return result+'"';
    }
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
            std::string version,detail;
        };

        InputLockCoordinator inputLocks;
        MediusBackend medius;
        InputBackend activeBackend{InputBackend::makcu};
        mutable std::mutex configMutex;
        bool reconfigurePending{};
        MouseOutputConfig lockSettings;
        mutable std::mutex deviceMutex;
        makcu_device_t* device{};
        std::vector<makcu_device_info_t> devices;
        std::vector<std::string> deviceLabels;
        std::future<ConnectionResult> connectionFuture;
        int selectedDevice{};
        std::string preferredPort;
        std::string connectionPort,pythonConnectionNote;
        std::string detectedPorts,lastConnectionError;
        std::size_t detectedDeviceCount{};
        int movementX{40};
        int movementY{};
        std::string manualActionStatus;
        bool initialized{};
        bool connectionPending{};
        bool highPerformanceMode{true};
        bool autoDetectAndConnect{true};
        mutable std::mutex movementPolicyMutex;
        std::atomic_bool pythonMovementActive{};
        std::atomic_uint64_t pythonGeneration{};
        mutable std::mutex outputStatusMutex;
        std::array<std::string,4> movementBlocks;
        std::array<std::uint64_t,4> movementBlockCounts{},movementBlockLogUs{},movementBlockAt{};
        std::array<bool,4> movementBlocked{};
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
        std::atomic_uint64_t moveAttempts{0},movesSent{0},ownershipRejections{0},disconnectedRejections{0},sendFailures{0};
        RollingStreamMetric movementLockWait,movementSend;
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
            case MAKCU_MOUSE_MIDDLE:
                return 1U << 2;
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
            case MouseButtonBinding::middle:
                return ButtonMask(MAKCU_MOUSE_MIDDLE);
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
            implementation->inputLocks.PhysicalButton(static_cast<MouseButtonBinding>(button),pressed);
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
            if(activeBackend==InputBackend::medius)return;
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
            auto previous=monitoredButtonMask.load(std::memory_order_acquire);
            const auto before=buttonEventCount.load(std::memory_order_acquire);
            if (makcu_get_button_mask(device, &mask) == MAKCU_SUCCESS)
            {
                if(buttonEventCount.load(std::memory_order_acquire)==before &&
                    monitoredButtonMask.compare_exchange_strong(previous,mask,std::memory_order_acq_rel) && previous!=mask)
                    inputLocks.PhysicalButtons(mask);
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
                preferredPort =
                    initialConfig->makcuPort;
                highPerformanceMode =
                    initialConfig->
                        highPerformanceMode;
                autoDetectAndConnect =
                    initialConfig->
                        autoDetectAndConnect;
            }

            if(initialConfig)lockSettings=*initialConfig;
            inputLocks.Configure(lockSettings);
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

        void RefreshDevices()
        {
            if(lockSettings.backend==InputBackend::medius)
            {
                devices.clear();deviceLabels.clear();detectedPorts.clear();selectedDevice=0;
                for(const auto& port:MediusBackend::Ports())
                {makcu_device_info_t entry{};strncpy_s(entry.port,port.c_str(),_TRUNCATE);
                    devices.push_back(entry);deviceLabels.push_back("Medius | "+port);
                    if(port==preferredPort)selectedDevice=int(devices.size()-1);
                    if(!detectedPorts.empty())detectedPorts+=", ";detectedPorts+=port;}
                detectedDeviceCount=devices.size();if(!devices.empty())preferredPort=devices[std::size_t(selectedDevice)].port;
                status=devices.empty()?"No Medius control device detected":"Medius control devices detected";return;
            }
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
            std::vector<makcu_device_info_t> actualDevices(
                discovered.begin(),
                discovered.begin() +
                    std::clamp(
                        count,
                        0,
                        maximumDevices));

            auto choices=SelectMakcuPorts(std::move(actualDevices),selectedPort);
            devices=std::move(choices.devices);
            detectedDeviceCount=choices.detectedCount;
            detectedPorts=std::move(choices.detectedPorts);
            deviceLabels.clear();
            deviceLabels.reserve(devices.size());
            selectedDevice=choices.selected;
            for (std::size_t index = 0;
                 index < devices.size();
                 ++index)
            {
                const auto& entry = devices[index];
                deviceLabels.push_back(
                    std::string(entry.port) +
                    " | " +
                    entry.description);
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
                    : std::to_string(detectedDeviceCount) +
                        " MAKCU device" +
                        (detectedDeviceCount == 1 ? "" : "s") +
                        " detected";
            }

            vanta::log::Info(
                "MAKCU scan: detected=%zu ports=[%s] selected=%s",
                detectedDeviceCount,detectedPorts.c_str(),preferredPort.c_str());
        }

        void BeginConnection()
        {
            if(lockSettings.backend==InputBackend::medius)
            {
                if(connectionPending || connected || devices.empty())return;
                activeBackend=InputBackend::medius;connectionPending=true;connectionPort=devices[std::size_t(selectedDevice)].port;
                medius.PrepareConnection();
                status="Connecting Medius / checking native output...";buttonMonitoringReady=false;monitoredButtonMask=0;
                const auto config=lockSettings;const auto port=connectionPort;
                connectionFuture=std::async(std::launch::async,[this,config,port]
                {
                    ConnectionResult result;strncpy_s(result.device.port,port.c_str(),_TRUNCATE);
                    auto buttons=[this](std::uint8_t mask){monitoredButtonMask=mask;++buttonEventCount;inputLocks.PhysicalButtons(mask);};
                    if(medius.Open(port,config,buttons,result.detail) && inputLocks.Attach(medius,port,result.detail))
                    {result.error=MAKCU_SUCCESS;result.version=medius.Identity();buttonMonitoringReady=true;}
                    else {const bool clean=inputLocks.Detach();const bool closed=medius.Close();inputLocks.FinishRecovery(clean && closed);}
                    return result;
                });return;
            }
            activeBackend=InputBackend::makcu;
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
            connectionPort=port;
            status = "Connecting to " + port + "...";
            vanta::log::Info(
                "MAKCU connection started: %s",
                port.c_str());

            connectionFuture = std::async(
                std::launch::async,
                [this, port, enableHighPerformance]()
                {
                    ConnectionResult result;
                    std::unique_lock<std::mutex> lock(deviceMutex);
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
                        std::array<char,4096> detail{};
                        if(makcu_get_last_error(device,detail.data(),detail.size())==MAKCU_SUCCESS)
                            result.detail=detail.data();
                        return result;
                    }
                    if (!ForceReleaseLocked())
                    {
                        result.error =
                            MAKCU_ERROR_COMMAND_FAILED;
                        result.detail="Connected, but the initial left-button release could not be written";
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
                        result.detail="Connected, but physical-button monitoring setup failed";
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
                    lock.unlock(); // Recovery and firmware waits must not retain deviceMutex.
                    if(!inputLocks.Attach(device,port,result.detail))
                    {
                        result.error=MAKCU_ERROR_COMMAND_FAILED;
                        const bool clean=inputLocks.Detach();
                        {std::lock_guard deviceLock(deviceMutex);DisableButtonMonitoringLocked();makcu_disconnect(device);}
                        inputLocks.FinishRecovery(clean);
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

            RefreshDevices();
            if (!devices.empty()) BeginConnection();
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
                result.detail=error.what();
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
                lastConnectionError.clear();
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
                // Also cover exceptions from partially completed attachment:
                // quiesce borrowed-device operations before closing the SDK.
                const bool clean=inputLocks.Detach();
                {
                    std::lock_guard<std::mutex> lock(deviceMutex);
                    if (device != nullptr)
                    {
                        DisableButtonMonitoringLocked();
                        makcu_disconnect(device);
                    }
                }
                const bool mediusClean=activeBackend!=InputBackend::medius || medius.Close();
                inputLocks.FinishRecovery(clean && mediusClean);
                firmwareVersion.clear();
                if(result.detail.empty())result.detail=result.error==MAKCU_SUCCESS
                    ? "Device disconnected or selected port changed before connection completed"
                    : MakcuErrorMessage(result.error);
                lastConnectionError=std::move(result.detail);
                status="Connection failed on "+connectionPort;
                vanta::log::Warning(
                    "MAKCU connection failed: port=%s detail=%s",
                    connectionPort.c_str(),lastConnectionError.c_str());
            }
        }

        void Disconnect()
        {
            if(activeBackend==InputBackend::medius && !connectionPending)
            {
                const bool clean=inputLocks.Detach();const bool closed=medius.Close();inputLocks.FinishRecovery(clean && closed);
                connected=false;buttonMonitoringReady=false;monitoredButtonMask=0;firmwareVersion.clear();connectedDevice={};
                status="Medius disconnected";return;
            }
            if (connectionPending)
            {
                return;
            }
            const bool clean=inputLocks.Detach();
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
            inputLocks.FinishRecovery(clean);
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
            if(activeBackend==InputBackend::medius)return medius.Healthy();
            std::lock_guard<std::mutex> lock(deviceMutex);
            return
                device != nullptr &&
                makcu_is_connected(device);
        }

        bool TryClick(const OutputContext& context={})
        {
            return inputLocks.TryClick(context,HumanizedQuickClickHoldMilliseconds());
        }

        bool AllowsMovement(bool pythonSource,std::uint64_t generation) const noexcept
        {
            std::lock_guard lock(movementPolicyMutex);
            return pythonSource==pythonMovementActive.load() &&
                (!pythonSource || generation==pythonGeneration.load());
        }

        void SetInputPolicy(InputOwner owner,bool enabled,std::uint64_t generation,MouseButtonBinding activation)
        {
            // Publish the same generation to both gates. UI rendering and
            // connection setup no longer grant Python movement ownership.
            std::lock_guard lock(movementPolicyMutex);
            if(owner==InputOwner::python)
            {
                if(generation<pythonGeneration)return;
                pythonMovementActive=enabled;pythonGeneration=generation;
            }
            inputLocks.SetPolicy(owner,enabled,generation,activation);
        }
        bool BlockMove(InputOwner owner,const std::string& why)
        {
            const auto index=static_cast<std::size_t>(owner);const auto now=StreamNowUs();bool report{};
            {std::lock_guard lock(outputStatusMutex);movementBlocks[index]=why;++movementBlockCounts[index];movementBlockAt[index]=now;movementBlocked[index]=true;
                if(!movementBlockLogUs[index] || now-movementBlockLogUs[index]>=1000000)
                {movementBlockLogUs[index]=now;report=true;}}
            constexpr const char* names[]{"Manual","Python","TestMove","TestClick"};
            if(report)log::Info("Movement blocked: owner=%s reason=%s",names[index],why.c_str());
            return false;
        }
        std::string MovementBlockReason(InputOwner owner) const
        {
            {std::lock_guard lock(outputStatusMutex);const auto index=static_cast<std::size_t>(owner);
                if(movementBlocked[index])return movementBlocks[index];}
            const auto reason=inputLocks.MovementBlockReason(owner);return reason.empty()?"No rejected movement recorded":reason;
        }

        bool TryMove(int x,int y,const OutputContext& context)
        {
            ++moveAttempts;const auto beforeLock=StreamNowUs();
            const char* blocked{};
            {
                std::lock_guard lock(deviceMutex);
                movementLockWait.Add(double(StreamNowUs()-beforeLock)/1000.0);
                if(!AllowsMovement(context.owner==InputOwner::python,context.generation))
                {++ownershipRejections;blocked=context.owner==InputOwner::python?"Python movement ownership or configuration generation does not match":"Python owns movement; disable Python to use another movement source";}
                else if(!connected.load() || (activeBackend==InputBackend::makcu && !device))
                {++disconnectedRejections;blocked="Mouse output is disconnected";}
            }
            if(blocked)return BlockMove(context.owner,blocked);
            const auto beforeSend=StreamNowUs();
            const bool success=inputLocks.TryMove(x,y,context);
            movementSend.Add(double(StreamNowUs()-beforeSend)/1000.0);
            if(success)
            {++movesSent;std::lock_guard lock(outputStatusMutex);movementBlocked[static_cast<std::size_t>(context.owner)]=false;}
            else {++sendFailures;BlockMove(context.owner,inputLocks.MovementBlockReason(context.owner));}
            return success;
        }
        bool TryMove(int x,int y,bool pythonSource=false,std::uint64_t generation=0)
        {
            OutputContext context;context.owner=pythonSource?InputOwner::python:InputOwner::manual;
            context.generation=generation;
            return TryMove(x,y,context);
        }
        bool ForceReleaseLeftButton(){return inputLocks.CancelClick();}

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

        void RenderManualControls()
        {
            const bool isMedius=activeBackend==InputBackend::medius;
            const bool ready=connected.load() && !connectionPending && !inputLocks.ResetRequired() && (!isMedius || medius.Healthy());
            constexpr int nativeLimit=500;
            movementX=std::clamp(movementX,-nativeLimit,nativeLimit);movementY=std::clamp(movementY,-nativeLimit,nativeLimit);
            ImGui::Spacing();ImGui::TextUnformatted("Manual output tests");
            ImGui::TextWrapped("Move and left-click affect the PC connected to MAKCU USB1. Capture is not required for these manual controls.");
            if(!ready)ImGui::TextWrapped("Output is not armed. Resolve the connection/status message before testing.");
            if(pythonMovementActive)ImGui::TextWrapped("Disable Python movement before a manual movement test.");
            ImGui::BeginDisabled(!ready);
            custom::SliderInt("Horizontal movement",&movementX,-nativeLimit,nativeLimit,"%d counts");
            custom::SliderInt("Vertical movement",&movementY,-nativeLimit,nativeLimit,"%d counts");
            if(movementX==0 && movementY==0)ImGui::TextDisabled("Choose a nonzero movement before pressing Move.");
            ImGui::BeginDisabled(pythonMovementActive || (movementX==0 && movementY==0));
            if(custom::Button(ICON_MOUSE_LINE "  Move",ImVec2(145,36)))
            {
                const bool accepted=TryMove(movementX,movementY);
                manualActionStatus=accepted?"Move command accepted by the output backend":"Move rejected: "+inputLocks.Status();
                log::Info("%s manual move(%d,%d): %s",isMedius?"Medius":"MAKCU",movementX,movementY,manualActionStatus.c_str());
            }
            ImGui::EndDisabled();ImGui::SameLine();
            if(custom::Button("Left click",ImVec2(130,36)))
            {
                const bool completed=TryClick();
                manualActionStatus=completed?"Left click completed":"Left click failed: "+inputLocks.Status();
                log::Info("%s manual left click: %s",isMedius?"Medius":"MAKCU",manualActionStatus.c_str());
            }
            ImGui::EndDisabled();
            if(!manualActionStatus.empty())ImGui::TextWrapped("%s",manualActionStatus.c_str());
            if(isMedius)
            {
                const auto mask=monitoredButtonMask.load();
                ImGui::TextDisabled("Physical buttons: L %s | R %s | X1 %s | X2 %s | Events: %llu",
                    mask&1?"DOWN":"up",mask&2?"DOWN":"up",mask&8?"DOWN":"up",mask&16?"DOWN":"up",
                    static_cast<unsigned long long>(buttonEventCount.load()));
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
                if(activeBackend==InputBackend::medius)medius.CancelConnection();
                connectionFuture.wait();
                CompleteConnectionIfReady();
            }
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
        implementation.CompleteConnectionIfReady();
        implementation.RefreshButtonMaskFromMakcu();
        implementation.RecoverReleaseIfNeeded();
        if(implementation.inputLocks.ResetRequired() && implementation.connected.load())
        {
            if(implementation.activeBackend==InputBackend::medius)
            {
                const auto detail=implementation.medius.FailureReason();
                if(!detail.empty())implementation.lastConnectionError=detail;
            }
            implementation.Disconnect();
            implementation.nextAutoDetectTick=GetTickCount64()+2000;
        }
        if(implementation.reconfigurePending && !implementation.connectionPending)
        {implementation.Disconnect();implementation.reconfigurePending=false;implementation.nextAutoDetectTick=0;}
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
            implementation.Disconnect();
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

        auto outputConfig=GetConfig();int backend=int(outputConfig.backend);
        ImGui::BeginDisabled(implementation.connectionPending);
        const char* backends[]{"Stock MAKCU","Medius 3.4.6"};
        if(custom::Combo("Output backend",&backend,backends,2)){outputConfig.backend=static_cast<InputBackend>(backend);ApplyConfig(outputConfig);}
        ImGui::EndDisabled();
        auto locks=GetConfig();bool lockChanged=false;
        lockChanged|=custom::Checkbox("Lock movement while correcting",&locks.lockMovementWhileCorrecting);
        lockChanged|=custom::Checkbox("Lock generated-click button",&locks.lockGeneratedClick);
        lockChanged|=custom::Checkbox("Reserve activation side buttons",&locks.reserveActivationSideButtons);
        if(lockChanged)ApplyConfig(locks);
        if(!locks.lockMovementWhileCorrecting)
            ImGui::TextWrapped("Movement locking is DISABLED. Physical mouse movement remains available during corrections.");
        if(custom::Button("Release all Vanta locks",ImVec2(240,32)))ReleaseAllInput();
        ImGui::TextWrapped("%s",implementation.inputLocks.Status().c_str());
        ImGui::TextDisabled("Input lock details are included in Save diagnostics.");
        if(implementation.pythonMovementActive)
            ImGui::TextDisabled("Disable Python movement before selecting a different port.");
        bool autoConnect =
            implementation.autoDetectAndConnect;
        if (custom::Checkbox(
                "Auto detect / connect MAKCU",
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
        if(GetConfig().backend==InputBackend::medius)
        {
            ImGui::TextWrapped("%s",implementation.status.c_str());
            if(!implementation.lastConnectionError.empty())ImGui::TextWrapped("Last connection/output error: %s",implementation.lastConnectionError.c_str());
            implementation.RenderManualControls();
            custom::Separator();
            ImGui::TextWrapped("%s",implementation.medius.Status().c_str());
            ImGui::TextWrapped("%s",implementation.lastConnectionError.c_str());
            ImGui::TextWrapped("Native Medius movement uses the original Python and TestMove calculations. Mouse polling rate is separate from capture FPS.");
            auto configuration=GetConfig();
            if(!implementation.connected && !implementation.connectionPending && ImGui::Button("Connect Medius"))ConnectOnce();
            if(implementation.connectionPending && ImGui::Button("Cancel connection"))
            {implementation.medius.CancelConnection();configuration.autoDetectAndConnect=false;ApplyConfig(configuration);}
            if(implementation.connected && ImGui::Button("Disconnect Medius"))implementation.Disconnect();
            custom::EndChild();return;
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
        if(!implementation.lastConnectionError.empty())
            ImGui::TextWrapped("Last connection error: %s",implementation.lastConnectionError.c_str());
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
            implementation.pythonMovementActive ||
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
                implementation.lastConnectionError.clear();
                implementation.nextAutoDetectTick=0;
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

            implementation.RenderManualControls();
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

    bool MakcuController::TryClick(const OutputContext& context)
    {return implementation_->TryClick(context);}
    bool MakcuController::TryMove(int x,int y,const OutputContext& context)
    {return implementation_->TryMove(x,y,context);}
    std::string MakcuController::MovementBlockReason(InputOwner owner) const
    {return implementation_->MovementBlockReason(owner);}
    ClickOutputStatus MakcuController::ClickStatus(InputOwner owner) const
    {return implementation_->inputLocks.ClickStatus(owner);}
    std::string MakcuController::FuseUnavailableReason() const
    {
        const auto& impl=*implementation_;
        if(GetConfig().backend!=InputBackend::medius)return "Fuse inputs requires Medius; stock MAKCU supports Existing behavior";
        if(!impl.connected.load() || impl.inputLocks.ResetRequired())return "Connect and verify Medius in Mouse Output before enabling Fuse inputs";
        return impl.medius.FuseUnavailableReason();
    }
    std::string MakcuController::InputControlStatus() const
    {return implementation_->inputLocks.Status();}
    void MakcuController::SetInputPolicy(InputOwner owner,bool enabled,std::uint64_t generation,MouseButtonBinding activation)
    {implementation_->SetInputPolicy(owner,enabled,generation,activation);}
    void MakcuController::EndCorrection(InputOwner owner,std::uint64_t generation,const char* reason)
    {implementation_->inputLocks.EndCorrection(owner,generation,reason);}
    void MakcuController::CancelOutput(InputOwner owner){implementation_->inputLocks.CancelOwner(owner);}
    void MakcuController::ReleaseAllInput(){implementation_->inputLocks.EmergencyRelease();}
    std::uint64_t MakcuController::EmergencyRevision() const noexcept{return implementation_->inputLocks.EmergencyRevision();}

    bool MakcuController::TryClick()
    {
        auto& impl = *implementation_;
        return impl.TryClick();
    }

    bool MakcuController::IsConnected() const noexcept
    {
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
        return impl.TryMove(x, y);
    }

    bool MakcuController::ForceReleaseLeftButton()
    {
        return implementation_->ForceReleaseLeftButton();
    }

    MouseOutputConfig MakcuController::GetConfig() const
    {
        const auto& impl = *implementation_;
        std::lock_guard configGuard(impl.configMutex);
        MouseOutputConfig result=impl.lockSettings;
        result.makcuPort =
            impl.preferredPort;
        result.highPerformanceMode =
            impl.highPerformanceMode;
        result.autoDetectAndConnect =
            impl.autoDetectAndConnect;
        result.lockMovementWhileCorrecting=impl.lockSettings.lockMovementWhileCorrecting;
        result.lockGeneratedClick=impl.lockSettings.lockGeneratedClick;
        result.reserveActivationSideButtons=impl.lockSettings.reserveActivationSideButtons;
        return result;
    }

    void MakcuController::ApplyConfig(const MouseOutputConfig& config)
    {
        auto& impl=*implementation_;
        const bool autoChanged=impl.autoDetectAndConnect!=config.autoDetectAndConnect;
        if (!impl.initialized || (!impl.connected && !impl.connectionPending))
            impl.preferredPort=config.makcuPort;
        const bool reconnect=impl.lockSettings.backend!=config.backend ||
            (config.backend==InputBackend::medius && impl.lockSettings.lockMovementWhileCorrecting!=config.lockMovementWhileCorrecting);
        if(reconnect){impl.inputLocks.CancelOwner(InputOwner::python);impl.inputLocks.CancelOwner(InputOwner::testMove);impl.inputLocks.CancelOwner(InputOwner::testClick);
            if(!impl.connectionPending)impl.Disconnect();else impl.reconfigurePending=true;}
        {std::lock_guard configGuard(impl.configMutex);impl.lockSettings=config;}
        impl.inputLocks.Configure(config);
        impl.highPerformanceMode=config.highPerformanceMode;
        impl.autoDetectAndConnect=config.autoDetectAndConnect;
        if (autoChanged) impl.nextAutoDetectTick=0;
        if (impl.initialized && !impl.connectionPending && !impl.connected)
            impl.RefreshDevices();
        impl.settingsRevision.fetch_add(1,std::memory_order_relaxed);
    }

    void MakcuController::ConnectOnce()
    {
        auto& impl=*implementation_;
        if (impl.initialized && !impl.connected && !impl.connectionPending)
        {
            impl.RefreshDevices();
            impl.BeginConnection();
        }
    }

    void MakcuController::ActivatePythonMovement(bool autoReconnect,std::uint64_t generation)
    {
        auto& impl=*implementation_;
        auto config=GetConfig();
        if (config.autoDetectAndConnect!=autoReconnect)
        {
            config.autoDetectAndConnect=autoReconnect;
            ApplyConfig(config);
        }
        // Movement ownership is published with the feature's activation policy.
        // Connection/UI work must not revive an older Python generation.
        if(!impl.connected && !impl.connectionPending)ConnectOnce();
        impl.pythonConnectionNote=impl.connected
            ? "Python uses MAKCU on "+std::string(impl.connectedDevice.port)+"."
            : impl.connectionPending
                ? "Python is connecting to an automatically detected MAKCU device."
                : "Python is waiting for an automatically detected MAKCU device.";
        log::Info("%s generation=%llu",impl.pythonConnectionNote.c_str(),static_cast<unsigned long long>(generation));
    }
    std::string MakcuController::PythonConnectionNote() const
    {return implementation_->pythonConnectionNote;}
    std::string MakcuController::DiagnosticsJson() const
    {
        const auto& impl=*implementation_;
        const auto wait=impl.movementLockWait.Get(),send=impl.movementSend.Get();
        // Called by the UI coordinator once per second. Never stall it behind
        // a connection attempt, click hold or serial write.
        std::unique_lock lock(impl.deviceMutex,std::try_to_lock);
        std::ostringstream json;
        json<<"{\"connected\":"<<(impl.connected?"true":"false")<<",\"connecting\":"<<(impl.connectionPending?"true":"false")
            <<",\"button_monitor_ready\":"<<(impl.buttonMonitoringReady?"true":"false")
            <<",\"selected_port\":"<<DiagnosticString(impl.preferredPort)
            <<",\"detected_device_count\":"<<impl.detectedDeviceCount
            <<",\"detected_ports\":"<<DiagnosticString(impl.detectedPorts)
            <<",\"last_connection_error\":"<<DiagnosticString(impl.lastConnectionError)
            <<",\"connected_port\":"<<DiagnosticString(impl.connectedDevice.port)
            <<",\"firmware\":"<<DiagnosticString(impl.firmwareVersion)
            <<",\"python_connection_note\":"<<DiagnosticString(impl.pythonConnectionNote)
            <<",\"device_busy\":"<<(lock.owns_lock()?"false":"true");
        {std::lock_guard policyLock(impl.movementPolicyMutex);
            json<<",\"python_owns_movement\":"<<(impl.pythonMovementActive?"true":"false")<<",\"python_owner_generation\":"<<impl.pythonGeneration.load();}
        {std::lock_guard outputLock(impl.outputStatusMutex);json<<",\"movement_sources\":[";
            for(std::size_t i=0;i<impl.movementBlocks.size();++i)
            {if(i)json<<',';json<<"{\"owner\":"<<i<<",\"rejections\":"<<impl.movementBlockCounts[i]
                <<",\"last_request_rejected\":"<<(impl.movementBlocked[i]?"true":"false")<<",\"last_block_local_us\":"<<impl.movementBlockAt[i]
                <<",\"last_block_reason\":"<<DiagnosticString(impl.movementBlocks[i])<<'}';}
            json<<']';}
        json<<",\"move_attempts\":"<<impl.moveAttempts.load()<<",\"moves_sent\":"<<impl.movesSent.load()
            <<",\"move_success_scope\":\"stock: serial write; Medius: generation-checked intent accepted into bounded scheduling; neither acknowledges USB delivery\""
            <<",\"ownership_rejections\":"<<impl.ownershipRejections.load()
            <<",\"disconnected_rejections\":"<<impl.disconnectedRejections.load()<<",\"send_failures\":"<<impl.sendFailures.load()
            <<",\"movement_lock_wait_p95_ms\":"<<wait.p95Ms<<",\"movement_submission_p95_ms\":"<<send.p95Ms<<",\"input_locks\":"<<impl.inputLocks.DiagnosticsJson()<<",\"motion_backend\":"<<(impl.activeBackend==InputBackend::medius?impl.medius.DiagnosticsJson():"{\"backend\":\"makcu\"}")<<'}';
        return json.str();
    }

    std::uint64_t
    MakcuController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }

    bool MakcuController::RunMovementOwnershipSelfTest()
    {
        makcu_device_info_t detected{};
        strcpy_s(detected.port,"COM3");
        const auto automatic=SelectMakcuPorts({detected},"COM5");
        if(automatic.devices.size()!=1 || automatic.selected!=0)return false;
        const auto preserved=SelectMakcuPorts({detected},"COM3");
        if(preserved.devices.size()!=1 || preserved.selected!=0)return false;
        // Exercise the same predicate used under deviceMutex before serial
        // writes. No device is created, opened or sent any input.
        Implementation impl;
        if (!impl.AllowsMovement(false,0) || impl.AllowsMovement(true,0)) return false;
        impl.pythonMovementActive=true;
        impl.pythonGeneration=7;
        if (impl.AllowsMovement(false,0) || !impl.AllowsMovement(true,7) ||
            impl.AllowsMovement(true,6)) return false;
        impl.pythonGeneration=8;
        if (impl.AllowsMovement(true,7) || !impl.AllowsMovement(true,8)) return false;
        impl.pythonMovementActive=false;
        if(!impl.AllowsMovement(false,0) || impl.AllowsMovement(true,8))return false;
        impl.SetInputPolicy(InputOwner::python,true,10,MouseButtonBinding::x1);
        if(!impl.AllowsMovement(true,10) || impl.AllowsMovement(false,0))return false;
        impl.SetInputPolicy(InputOwner::python,false,11,MouseButtonBinding::x1);
        impl.SetInputPolicy(InputOwner::python,true,10,MouseButtonBinding::x1);
        if(impl.pythonGeneration!=11 || !impl.AllowsMovement(false,0) || impl.AllowsMovement(true,10))return false;
        impl.SetInputPolicy(InputOwner::python,true,12,MouseButtonBinding::x2);
        return impl.AllowsMovement(true,12) && !impl.AllowsMovement(true,11);
    }
}
