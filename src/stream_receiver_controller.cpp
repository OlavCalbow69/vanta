#include "stream_receiver_controller.hpp"

#include "hardware_video_decoder.hpp"
#include "logger.hpp"
#include "sunshine_pairing.hpp"

#include "Limelight.h"

#include <Windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <wrl/client.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    struct ActiveStreamCallbacks
    {
        vanta::HardwareVideoDecoder* decoder{};
        std::function<void(const std::string&)> reportStatus;
        std::atomic_bool* stopRequested{};
        std::atomic_bool connectionStarted{false};
        std::atomic_bool connectionTerminated{false};
        std::atomic_int terminationError{};
        bool acceptingCallbacks{};
        unsigned int inFlightCallbacks{};
    };

    // moonlight-common-c owns one process-wide connection. Its termination
    // notification can be dispatched on a detached thread, so a raw atomic
    // pointer alone is not enough to protect Vanta during stop/shutdown. Each
    // callback holds a short lease; teardown removes the global endpoint and
    // waits for leases to drain before releasing the decoder or controller.
    std::mutex gActiveCallbacksMutex;
    std::condition_variable gActiveCallbacksIdle;
    ActiveStreamCallbacks* gActiveStreamCallbacks{};

    void ReleaseCallbackLease(ActiveStreamCallbacks* callbacks)
    {
        std::lock_guard<std::mutex> lock(gActiveCallbacksMutex);
        if (callbacks->inFlightCallbacks > 0)
        {
            --callbacks->inFlightCallbacks;
            if (callbacks->inFlightCallbacks == 0)
            {
                gActiveCallbacksIdle.notify_all();
            }
        }
    }

    class CallbackLease
    {
    public:
        CallbackLease() = default;
        explicit CallbackLease(ActiveStreamCallbacks* callbacks) noexcept : callbacks_(callbacks) {}
        ~CallbackLease() { Reset(); }

        CallbackLease(const CallbackLease&) = delete;
        CallbackLease& operator=(const CallbackLease&) = delete;

        CallbackLease(CallbackLease&& other) noexcept : callbacks_(std::exchange(other.callbacks_, nullptr)) {}
        CallbackLease& operator=(CallbackLease&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                callbacks_ = std::exchange(other.callbacks_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return callbacks_ != nullptr; }
        [[nodiscard]] ActiveStreamCallbacks* Get() const noexcept { return callbacks_; }

    private:
        void Reset()
        {
            if (callbacks_ != nullptr)
            {
                ReleaseCallbackLease(std::exchange(callbacks_, nullptr));
            }
        }

        ActiveStreamCallbacks* callbacks_{};
    };

    // Keep the visible "starting" state true for the complete launch attempt,
    // not only while LiStartConnection() is on the stack. This gives shutdown
    // one reliable cancellation path while the HTTPS launch request is in
    // progress too. The flag is deliberately cleared by RAII on every error
    // path in the worker.
    class StartAttemptGuard
    {
    public:
        explicit StartAttemptGuard(std::atomic_bool& starting) noexcept : starting_(starting)
        {
            starting_.store(true);
        }

        ~StartAttemptGuard()
        {
            starting_.store(false);
        }

        StartAttemptGuard(const StartAttemptGuard&) = delete;
        StartAttemptGuard& operator=(const StartAttemptGuard&) = delete;

    private:
        std::atomic_bool& starting_;
    };

    [[nodiscard]] CallbackLease AcquireActiveCallbacks()
    {
        std::lock_guard<std::mutex> lock(gActiveCallbacksMutex);
        if (gActiveStreamCallbacks == nullptr || !gActiveStreamCallbacks->acceptingCallbacks)
        {
            return {};
        }
        ++gActiveStreamCallbacks->inFlightCallbacks;
        return CallbackLease(gActiveStreamCallbacks);
    }

    [[nodiscard]] bool ActivateCallbacks(ActiveStreamCallbacks& callbacks)
    {
        std::lock_guard<std::mutex> lock(gActiveCallbacksMutex);
        if (gActiveStreamCallbacks != nullptr && gActiveStreamCallbacks != &callbacks)
        {
            return false;
        }
        callbacks.acceptingCallbacks = true;
        callbacks.inFlightCallbacks = 0;
        gActiveStreamCallbacks = &callbacks;
        return true;
    }

    void DeactivateCallbacks(ActiveStreamCallbacks& callbacks)
    {
        std::unique_lock<std::mutex> lock(gActiveCallbacksMutex);
        callbacks.acceptingCallbacks = false;
        if (gActiveStreamCallbacks == &callbacks)
        {
            gActiveStreamCallbacks = nullptr;
        }
        gActiveCallbacksIdle.wait(lock, [&callbacks]() { return callbacks.inFlightCallbacks == 0; });
    }

    void ReportStreamStatus(ActiveStreamCallbacks& callbacks, const std::string& status)
    {
        if (callbacks.reportStatus)
        {
            callbacks.reportStatus(status);
        }
    }

    int OnDecoderSetup(const int format, const int width, const int height, const int redrawRate, void*, const int)
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (!callbacks || callbacks.Get()->decoder == nullptr)
        {
            return -1;
        }
        std::string error;
        const int result = callbacks.Get()->decoder->Configure(format, width, height, redrawRate, error);
        if (result != 0)
        {
            ReportStreamStatus(*callbacks.Get(), error.empty() ? "Vanta could not configure its hardware video decoder." : error);
            vanta::log::Error("Sunshine H.264 decoder setup failed: %s", error.c_str());
        }
        return result;
    }

    void OnDecoderStart()
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks && callbacks.Get()->decoder != nullptr)
        {
            callbacks.Get()->decoder->Start();
        }
    }

    void OnDecoderStop()
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks && callbacks.Get()->decoder != nullptr)
        {
            callbacks.Get()->decoder->Stop();
        }
    }

    void OnDecoderCleanup()
    {
        // LiStopConnection() guarantees no additional decode units after this
        // callback. The owning controller joins the decoder worker immediately
        // after the Moonlight transport has fully stopped.
        OnDecoderStop();
    }

    int OnDecodeUnit(PDECODE_UNIT decodeUnit)
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks && callbacks.Get()->decoder != nullptr)
        {
            return callbacks.Get()->decoder->Submit(decodeUnit);
        }
        return DR_NEED_IDR;
    }

    void OnConnectionStageStarting(const int stage)
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks)
        {
            // LiStartConnection() resets its interruption flag during early
            // initialization. If shutdown raced with entry to that call, this
            // stage notification occurs afterwards and reissues the request,
            // so the pending connect cannot escape cancellation.
            if (callbacks.Get()->stopRequested != nullptr && callbacks.Get()->stopRequested->load())
            {
                LiInterruptConnection();
                return;
            }
            ReportStreamStatus(*callbacks.Get(), std::string("Connecting to Sunshine: ") + LiGetStageName(stage) + "...");
        }
    }

    void OnConnectionStageFailed(const int stage, const int errorCode)
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks)
        {
            ReportStreamStatus(
                *callbacks.Get(),
                std::string("Sunshine connection failed at ") + LiGetStageName(stage) +
                " (error " + std::to_string(errorCode) + ").");
        }
    }

    void OnConnectionStarted()
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks)
        {
            callbacks.Get()->connectionStarted.store(true);
            ReportStreamStatus(*callbacks.Get(), "Sunshine Desktop connected. Waiting for the first GPU-decoded video frame...");
        }
    }

    void OnConnectionTerminated(const int errorCode)
    {
        const CallbackLease callbacks = AcquireActiveCallbacks();
        if (callbacks)
        {
            callbacks.Get()->terminationError.store(errorCode);
            callbacks.Get()->connectionTerminated.store(true);
            ReportStreamStatus(
                *callbacks.Get(),
                errorCode == ML_ERROR_GRACEFUL_TERMINATION
                    ? "Sunshine ended the stream."
                    : "Sunshine stream ended unexpectedly (error " + std::to_string(errorCode) + ").");
        }
    }

    void OnMoonlightLog(const char* format, ...)
    {
        if (format == nullptr)
        {
            return;
        }

        char message[2048]{};
        va_list arguments;
        va_start(arguments, format);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
        va_end(arguments);
        std::size_t length = std::strlen(message);
        while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        {
            message[--length] = '\0';
        }
        if (length != 0)
        {
            // Common-c emits this callback from its transport threads. Vanta's
            // logger serializes cross-thread output, so the protocol trace is
            // safe to retain beside Vanta's own receiver status messages.
            vanta::log::Info("Moonlight: %s", message);
        }
    }

    [[nodiscard]] bool CreateStreamRequest(
        const int width,
        const int height,
        const int fps,
        const int bitrateKbps,
        const bool requestDisplayMode,
        vanta::sunshine::StreamRequest& request,
        std::string& error)
    {
        request.width = width;
        request.height = height;
        request.fps = fps;
        request.bitrateKbps = bitrateKbps;
        request.packetSize = 1392;
        request.requestDisplayMode = requestDisplayMode;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(
                nullptr,
                request.remoteInputKey.data(),
                static_cast<ULONG>(request.remoteInputKey.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
            !BCRYPT_SUCCESS(BCryptGenRandom(
                nullptr,
                request.remoteInputIv.data(),
                4,
                BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        {
            error = "Windows could not create the secure remote-input key for this Sunshine stream";
            return false;
        }
        return true;
    }

    struct StreamProfile
    {
        const char* label;
        int width;
        int height;
        int fps;
        int bitrateKbps;
    };

    constexpr std::array<StreamProfile, 5> kLanStreamProfiles{{
        {"1080p 120", 1920, 1080, 120, 80000},
        {"1080p 144", 1920, 1080, 144, 95000},
        {"1080p 240", 1920, 1080, 240, 125000},
        {"1440p 144", 2560, 1440, 144, 150000},
        {"4K 120", 3840, 2160, 120, 200000},
    }};
}

namespace vanta
{
    struct StreamReceiverController::Implementation
    {
        sunshine::PairingService pairingService;
        HardwareVideoDecoder decoder;
        ID3D11Device* device{};
        std::mutex mutex;
        std::thread worker;
        std::atomic_bool busy{false};
        std::atomic_bool stopping{false};
        std::atomic_bool moonlightStarting{false};
        std::atomic_bool moonlightStarted{false};
        char host[256]{};
        char pin[5]{};
        bool lanDiscoveryStarted{};
        int requestedWidth{1920};
        int requestedHeight{1080};
        int requestedFps{120};
        int requestedBitrateKbps{80000};
        bool requestDisplayMode{true};
        std::string status{"Enter the LAN address of the Sunshine host, then select Discover."};
        std::optional<sunshine::HostInfo> discoveredHost;
        std::optional<sunshine::PairedHost> pairedHost;
        std::vector<sunshine::HostInfo> lanHosts;
        int selectedLanHost{-1};
        ActiveStreamCallbacks streamCallbacks;
        ComPtr<ID3D11ShaderResourceView> frameViewForRender;

        void JoinWorker()
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        void SetStatus(std::string value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = std::move(value);
        }

        void StartJob(std::function<void()> job)
        {
            if (busy.exchange(true))
            {
                return;
            }
            JoinWorker();
            worker = std::thread([this, job = std::move(job)]() mutable
            {
                job();
                busy.store(false);
            });
        }

        void StartDiscovery()
        {
            const std::string requestedHost(host);
            SetStatus("Discovering Sunshine host...");
            StartJob([this, requestedHost]()
            {
                std::string error;
                const auto hostInfo = stopping.load() ? std::optional<sunshine::HostInfo>{} : pairingService.Discover(requestedHost, error);
                if (stopping.load())
                {
                    SetStatus("Discovery cancelled");
                    return;
                }
                if (!hostInfo)
                {
                    SetStatus(error.empty() ? "Could not discover a compatible Sunshine host" : error);
                    vanta::log::Warning("Sunshine receiver discovery failed: %s", error.c_str());
                    return;
                }

                std::string pairingError;
                const auto savedPairing = pairingService.LoadPairedHost(requestedHost, pairingError);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    discoveredHost = hostInfo;
                    pairedHost = savedPairing;
                    status = "Found " + (hostInfo->name.empty() ? hostInfo->address : hostInfo->name) +
                             " | server " + hostInfo->appVersion +
                             (savedPairing ? " | this Vanta client is already paired" : " | ready to pair");
                }
                vanta::log::Info("Sunshine receiver discovered host %s", hostInfo->address.c_str());
            });
        }

        bool GeneratePairingPin()
        {
            std::string error;
            const std::string generatedPin = pairingService.GeneratePairingPin(error);
            if (generatedPin.size() != 4)
            {
                SetStatus(error.empty() ? "Could not generate a secure Sunshine pairing PIN" : error);
                return false;
            }

            std::memset(pin, 0, sizeof(pin));
            std::memcpy(pin, generatedPin.data(), generatedPin.size());
            SetStatus(
                "PIN " + generatedPin +
                " is ready. Start pairing, then enter this PIN in Sunshine's Web UI on the host PC.");
            return true;
        }

        bool EnsurePairingPin()
        {
            return std::string(pin).size() == 4 || GeneratePairingPin();
        }

        void StartLanDiscovery()
        {
            if (busy.load() || moonlightStarting.load() || moonlightStarted.load())
            {
                return;
            }

            SetStatus("Scanning this LAN for Sunshine hosts through _nvstream._tcp.local...");
            StartJob([this]()
            {
                std::string error;
                std::vector<sunshine::HostInfo> hosts = stopping.load()
                    ? std::vector<sunshine::HostInfo>{}
                    : pairingService.DiscoverLan(error);
                if (stopping.load())
                {
                    SetStatus("LAN Sunshine discovery cancelled");
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    lanHosts = std::move(hosts);
                    selectedLanHost = lanHosts.empty() ? -1 : 0;
                    status = lanHosts.empty()
                        ? (error.empty() ? "No Sunshine hosts were found on this LAN" : error)
                        : "Found " + std::to_string(lanHosts.size()) +
                          " Sunshine host" + (lanHosts.size() == 1 ? "" : "s") +
                          " on this LAN. Select one below to use it.";
                }
                if (lanHosts.empty())
                {
                    vanta::log::Warning("LAN Sunshine discovery found no usable hosts: %s", error.c_str());
                }
                else
                {
                    vanta::log::Info("LAN Sunshine discovery found %zu host(s)", lanHosts.size());
                }
            });
        }

        void SelectLanHost(const int index)
        {
            sunshine::HostInfo selected;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (index < 0 || static_cast<std::size_t>(index) >= lanHosts.size())
                {
                    return;
                }
                selected = lanHosts[static_cast<std::size_t>(index)];
            }
            if (selected.address.empty() || selected.address.size() >= sizeof(host))
            {
                SetStatus("The selected Sunshine host address is not valid");
                return;
            }

            std::string pairingError;
            const auto savedPairing = pairingService.LoadPairedHost(selected.address, pairingError);
            std::memset(host, 0, sizeof(host));
            std::memcpy(host, selected.address.data(), selected.address.size());
            {
                std::lock_guard<std::mutex> lock(mutex);
                selectedLanHost = index;
                discoveredHost = selected;
                pairedHost = savedPairing;
                status = "Selected " + (selected.name.empty() ? selected.address : selected.name) +
                         (savedPairing ? " | this Vanta client is already paired" : " | ready to pair");
            }
        }

        bool EnsureHostInInput()
        {
            if (host[0] != '\0')
            {
                return true;
            }

            sunshine::HostInfo selected;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (selectedLanHost < 0 ||
                    static_cast<std::size_t>(selectedLanHost) >= lanHosts.size())
                {
                    return false;
                }
                selected = lanHosts[static_cast<std::size_t>(selectedLanHost)];
            }
            if (selected.address.empty() || selected.address.size() >= sizeof(host))
            {
                return false;
            }

            std::memset(host, 0, sizeof(host));
            std::memcpy(host, selected.address.data(), selected.address.size());
            return true;
        }

        void StartPairing()
        {
            if (!EnsurePairingPin())
            {
                return;
            }
            if (!EnsureHostInInput())
            {
                SetStatus("Select a Sunshine host or enter its LAN IP before pairing");
                return;
            }
            const std::string requestedHost(host);
            const std::string requestedPin(pin);

            SetStatus(
                "Starting pairing. In Sunshine's Web UI, enter Vanta PIN " + requestedPin +
                " and select Send while this status remains visible...");
            vanta::log::Info("Starting Sunshine pairing with %s; awaiting Web UI PIN approval", requestedHost.c_str());
            StartJob([this, requestedHost, requestedPin]()
            {
                const sunshine::PairingResult result = stopping.load()
                    ? sunshine::PairingResult{false, "Pairing cancelled", std::nullopt}
                    : pairingService.Pair(
                        requestedHost,
                        requestedPin,
                        [this](const std::string& progress)
                        {
                            if (!stopping.load())
                            {
                                SetStatus(progress);
                            }
                        });
                if (stopping.load())
                {
                    SetStatus("Pairing cancelled");
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    status = result.message;
                    if (result.host)
                    {
                        pairedHost = result.host;
                        discoveredHost = result.host->info;
                    }
                }
                if (result.succeeded)
                {
                    vanta::log::Info("Vanta paired with Sunshine host %s", requestedHost.c_str());
                }
                else
                {
                    vanta::log::Warning("Sunshine pairing failed: %s", result.message.c_str());
                }
            });
        }

        void StartStream()
        {
            if (moonlightStarting.load() || moonlightStarted.load())
            {
                return;
            }
            if (!EnsureHostInInput())
            {
                SetStatus("Select a paired Sunshine host or enter its LAN IP before connecting");
                return;
            }
            const std::string requestedHost(host);
            const int width = requestedWidth;
            const int height = requestedHeight;
            const int fps = requestedFps;
            const int bitrateKbps = requestedBitrateKbps;
            const bool useRequestedDisplayMode = requestDisplayMode;
            SetStatus("Preparing Vanta's H.264 hardware receiver...");
            StartJob([this, requestedHost, width, height, fps, bitrateKbps, useRequestedDisplayMode]()
            {
                StartAttemptGuard startAttempt(moonlightStarting);
                if (stopping.load())
                {
                    return;
                }

                std::string error;
                if (!decoder.Initialize(device, error))
                {
                    SetStatus(error);
                    return;
                }
                if (stopping.load())
                {
                    decoder.Shutdown();
                    return;
                }

                sunshine::StreamRequest request;
                if (!CreateStreamRequest(width, height, fps, bitrateKbps, useRequestedDisplayMode, request, error))
                {
                    SetStatus(error);
                    decoder.Shutdown();
                    return;
                }
                SetStatus("Launching Sunshine Desktop through Vanta's paired connection...");
                const sunshine::StreamLaunchResult launch = pairingService.LaunchDesktop(requestedHost, request);
                if (!launch.succeeded)
                {
                    SetStatus(launch.message);
                    decoder.Shutdown();
                    return;
                }
                if (stopping.load())
                {
                    decoder.Shutdown();
                    return;
                }

                STREAM_CONFIGURATION streamConfig{};
                LiInitializeStreamConfiguration(&streamConfig);
                streamConfig.width = request.width;
                streamConfig.height = request.height;
                streamConfig.fps = request.fps;
                streamConfig.bitrate = request.bitrateKbps;
                streamConfig.packetSize = request.packetSize;
                streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
                streamConfig.audioConfiguration = request.audioConfiguration;
                // H.264 is selected deliberately for the first Vanta decoder.
                // Sunshine is free to keep HEVC/AV1 enabled for other clients.
                streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
                streamConfig.clientRefreshRateX100 = request.fps * 100;
                streamConfig.colorSpace = COLORSPACE_REC_709;
                streamConfig.colorRange = COLOR_RANGE_LIMITED;
                streamConfig.encryptionFlags = ENCFLG_ALL;
                std::memcpy(streamConfig.remoteInputAesKey, request.remoteInputKey.data(), request.remoteInputKey.size());
                std::memcpy(streamConfig.remoteInputAesIv, request.remoteInputIv.data(), request.remoteInputIv.size());

                SERVER_INFORMATION serverInfo{};
                LiInitializeServerInformation(&serverInfo);
                serverInfo.address = launch.host.info.address.c_str();
                serverInfo.serverInfoAppVersion = launch.host.info.appVersion.c_str();
                serverInfo.serverInfoGfeVersion = launch.host.info.gfeVersion.empty() ? nullptr : launch.host.info.gfeVersion.c_str();
                serverInfo.rtspSessionUrl = launch.rtspSessionUrl.c_str();
                serverInfo.serverCodecModeSupport = launch.host.info.codecSupport;

                CONNECTION_LISTENER_CALLBACKS connectionCallbacks{};
                LiInitializeConnectionCallbacks(&connectionCallbacks);
                connectionCallbacks.stageStarting = OnConnectionStageStarting;
                connectionCallbacks.stageFailed = OnConnectionStageFailed;
                connectionCallbacks.connectionStarted = OnConnectionStarted;
                connectionCallbacks.connectionTerminated = OnConnectionTerminated;
                connectionCallbacks.logMessage = OnMoonlightLog;

                DECODER_RENDERER_CALLBACKS videoCallbacks{};
                LiInitializeVideoCallbacks(&videoCallbacks);
                videoCallbacks.setup = OnDecoderSetup;
                videoCallbacks.start = OnDecoderStart;
                videoCallbacks.stop = OnDecoderStop;
                videoCallbacks.cleanup = OnDecoderCleanup;
                videoCallbacks.submitDecodeUnit = OnDecodeUnit;
                // Submit() only copies the completed Annex-B access unit into
                // Vanta's shallow worker queue, so common-c can deliver it
                // directly from the receive path without an extra queue.
                videoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;

                streamCallbacks.decoder = &decoder;
                streamCallbacks.connectionStarted.store(false);
                streamCallbacks.connectionTerminated.store(false);
                streamCallbacks.terminationError.store(0);
                streamCallbacks.stopRequested = &stopping;
                streamCallbacks.reportStatus = [this](const std::string& progress)
                {
                    if (!stopping.load())
                    {
                        SetStatus(progress);
                    }
                };
                if (!ActivateCallbacks(streamCallbacks))
                {
                    decoder.Shutdown();
                    SetStatus("Vanta already has an active Sunshine receiver connection.");
                    return;
                }
                if (stopping.load())
                {
                    DeactivateCallbacks(streamCallbacks);
                    decoder.Shutdown();
                    return;
                }
                const int connectionResult = LiStartConnection(
                    &serverInfo,
                    &streamConfig,
                    &connectionCallbacks,
                    &videoCallbacks,
                    nullptr,
                    &decoder,
                    0,
                    nullptr,
                    0);
                if (connectionResult != 0)
                {
                    DeactivateCallbacks(streamCallbacks);
                    decoder.Shutdown();
                    SetStatus("Vanta could not start Sunshine's UDP stream (error " + std::to_string(connectionResult) + ").");
                    return;
                }
                // Shutdown can race with LiStartConnection() completing.
                // Do not publish a live session after the owner has begun
                // tearing down; stop common-c while the callback endpoint and
                // decoder are still valid instead.
                if (stopping.load())
                {
                    LiStopConnection();
                    DeactivateCallbacks(streamCallbacks);
                    decoder.Shutdown();
                    return;
                }
                moonlightStarted.store(true);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    pairedHost = launch.host;
                    discoveredHost = launch.host.info;
                }
                if (!streamCallbacks.connectionTerminated.load())
                {
                    SetStatus("Sunshine Desktop is streaming to Vanta's H.264 hardware decoder.");
                }
                vanta::log::Info("Vanta started Sunshine Desktop stream from %s", launch.host.info.address.c_str());
            });
        }

        void StopStream(const bool preserveTerminationStatus = false)
        {
            const bool wasStarting = moonlightStarting.load();
            if (wasStarting)
            {
                LiInterruptConnection();
                JoinWorker();
            }
            const bool wasStarted = moonlightStarted.exchange(false);
            if (wasStarted)
            {
                if (!preserveTerminationStatus)
                {
                    SetStatus("Stopping Sunshine stream...");
                }
                LiStopConnection();
            }
            if (wasStarting || wasStarted)
            {
                DeactivateCallbacks(streamCallbacks);
                decoder.Shutdown();
                frameViewForRender.Reset();
            }
        }
    };

    StreamReceiverController::StreamReceiverController() : implementation_(std::make_unique<Implementation>()) {}
    StreamReceiverController::~StreamReceiverController() { Shutdown(); }

    void StreamReceiverController::Initialize(ID3D11Device* device)
    {
        implementation_->device = device;
        if (!implementation_->GeneratePairingPin())
        {
            vanta::log::Warning("Sunshine receiver could not generate its initial pairing PIN");
        }
        vanta::log::Info(
            "Sunshine receiver initialized; Moonlight launch query: %s",
            LiGetLaunchUrlQueryParameters());
    }

    void StreamReceiverController::Tick()
    {
        auto& impl = *implementation_;
        // moonlight-common-c invokes connectionTerminated from an auxiliary
        // thread. Complete the non-thread-safe LiStopConnection cleanup on
        // Vanta's main thread, after preserving the useful terminal status.
        if (impl.streamCallbacks.connectionTerminated.load() &&
            !impl.moonlightStarting.load() &&
            impl.moonlightStarted.load())
        {
            impl.StopStream(true);
        }
    }

    void StreamReceiverController::RenderPanel()
    {
        auto& impl = *implementation_;
        const float height = std::max(450.0F, ImGui::GetContentRegionAvail().y - 40.0F);
        custom::Child(ICON_RADIO_LINE "  Sunshine receiver##sunshine-receiver", ImVec2(0.0F, height), true);
        ImGui::TextWrapped("Vanta pairs directly with Sunshine, launches Sunshine Desktop through a certificate-pinned HTTPS request, then receives its UDP video with Moonlight common-c. No external Moonlight process is used.");
        ImGui::TextDisabled("To feed the stream into Vanta's capture-based tools, select Sunshine stream on the Capture page.");
        ImGui::Spacing();

        if (!impl.lanDiscoveryStarted && !impl.busy.load() &&
            !impl.moonlightStarting.load() && !impl.moonlightStarted.load())
        {
            impl.lanDiscoveryStarted = true;
            impl.StartLanDiscovery();
        }

        const bool streamActive = impl.moonlightStarting.load() || impl.moonlightStarted.load();
        const bool connectionStarted = impl.moonlightStarted.load();
        const bool activeJob = impl.busy.load();
        std::vector<sunshine::HostInfo> lanHosts;
        int selectedLanHost{};
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            lanHosts = impl.lanHosts;
            selectedLanHost = impl.selectedLanHost;
        }
        if (activeJob || streamActive)
        {
            ImGui::BeginDisabled();
        }
        ImGui::InputText("Sunshine host (LAN IP or name)", impl.host, sizeof(impl.host));
        if (custom::Button("Scan LAN for Sunshine hosts", ImVec2(245.0F, 34.0F)))
        {
            impl.StartLanDiscovery();
        }
        ImGui::SameLine();
        if (custom::Button("Discover entered host", ImVec2(190.0F, 34.0F)))
        {
            impl.StartDiscovery();
        }
        if (!lanHosts.empty())
        {
            std::string selectedLabel{"Select a discovered Sunshine host"};
            if (selectedLanHost >= 0 && static_cast<std::size_t>(selectedLanHost) < lanHosts.size())
            {
                const sunshine::HostInfo& selected = lanHosts[static_cast<std::size_t>(selectedLanHost)];
                selectedLabel = (selected.name.empty() ? selected.address : selected.name) +
                                " (" + selected.address + ")";
            }
            if (ImGui::BeginCombo("LAN Sunshine hosts", selectedLabel.c_str()))
            {
                for (std::size_t index = 0; index < lanHosts.size(); ++index)
                {
                    const sunshine::HostInfo& candidate = lanHosts[index];
                    const std::string label =
                        (candidate.name.empty() ? candidate.address : candidate.name) +
                        " (" + candidate.address + ")";
                    const bool selected = static_cast<int>(index) == selectedLanHost;
                    if (ImGui::Selectable(label.c_str(), selected))
                    {
                        impl.SelectLanHost(static_cast<int>(index));
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (activeJob || streamActive)
        {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Vanta pairing PIN: %s", impl.pin[0] == '\0' ? "Generating..." : impl.pin);
        if (!activeJob && !streamActive)
        {
            ImGui::SameLine();
            if (custom::Button("New PIN", ImVec2(105.0F, 30.0F)))
            {
                impl.GeneratePairingPin();
            }
        }
        ImGui::TextWrapped(
            "1. Select a host and start pairing. 2. On the Sunshine host, open its Web UI > PIN, enter the Vanta PIN shown above, optionally name the device Vanta, then select Send. Vanta stays waiting for two minutes.");
        if (activeJob || streamActive)
        {
            ImGui::BeginDisabled();
        }
        if (custom::Button("Start Vanta pairing", ImVec2(220.0F, 36.0F)))
        {
            impl.StartPairing();
        }
        if (activeJob || streamActive)
        {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        if (activeJob || streamActive)
        {
            ImGui::BeginDisabled();
        }
        ImGui::TextUnformatted("LAN stream profile");
        for (std::size_t index = 0; index < kLanStreamProfiles.size(); ++index)
        {
            const StreamProfile& profile = kLanStreamProfiles[index];
            if (index != 0)
            {
                ImGui::SameLine();
            }
            if (custom::Button(profile.label, ImVec2(112.0F, 30.0F)))
            {
                impl.requestedWidth = profile.width;
                impl.requestedHeight = profile.height;
                impl.requestedFps = profile.fps;
                impl.requestedBitrateKbps = profile.bitrateKbps;
            }
        }
        ImGui::TextDisabled("240 FPS requires a matching host/virtual-display mode and a 240 Hz receiver display.");
        ImGui::InputInt("Width", &impl.requestedWidth, 0, 0);
        ImGui::SameLine();
        ImGui::InputInt("Height", &impl.requestedHeight, 0, 0);
        ImGui::SameLine();
        ImGui::InputInt("FPS", &impl.requestedFps, 0, 0);
        ImGui::InputInt("Bitrate (kbps)", &impl.requestedBitrateKbps, 0, 0);
        custom::Checkbox("Ask Sunshine to match this display mode", &impl.requestDisplayMode);
        if (activeJob || streamActive)
        {
            ImGui::EndDisabled();
        }
        if (!streamActive && !activeJob)
        {
            ImGui::TextDisabled("After Vanta reports that it is paired, connect to begin the Sunshine Desktop stream.");
            ImGui::TextDisabled("Disable display-mode matching if the host display must stay unchanged.");
            if (custom::Button("Connect and start Sunshine Desktop", ImVec2(310.0F, 38.0F)))
            {
                impl.StartStream();
            }
        }
        else if (connectionStarted)
        {
            if (custom::Button("Stop Sunshine stream", ImVec2(220.0F, 38.0F)))
            {
                impl.StopStream();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Receiving through Vanta...");
        }
        else
        {
            ImGui::TextDisabled(streamActive ? "Opening Sunshine UDP session..." : "Sunshine operation in progress...");
        }

        std::string status;
        std::optional<sunshine::HostInfo> discovered;
        std::optional<sunshine::PairedHost> paired;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            status = impl.status;
            discovered = impl.discoveredHost;
            paired = impl.pairedHost;
        }
        const DecodedVideoStats decoderStats = impl.decoder.GetStats();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", status.c_str());
        if (discovered)
        {
            ImGui::Text("Host: %s | GameStream: %s | HTTPS: %u", discovered->name.empty() ? discovered->address.c_str() : discovered->name.c_str(), discovered->appVersion.c_str(), discovered->httpsPort);
            ImGui::TextDisabled("%s", paired ? "Certificate pin saved for this Vanta user." : "Pair before starting a protected Sunshine stream.");
        }
        if (decoderStats.hardwareActive)
        {
            ImGui::Text(
                "Stream: %dx%d @ %d FPS | %.1f received / %.1f decoded FPS",
                decoderStats.width,
                decoderStats.height,
                decoderStats.configuredFps,
                decoderStats.receivedFramesPerSecond,
                decoderStats.decodedFramesPerSecond);
            ImGui::Text(
                "Hardware H.264: %llu received | %llu queued | %llu decoded | %llu dropped",
                static_cast<unsigned long long>(decoderStats.receivedFrames),
                static_cast<unsigned long long>(decoderStats.submittedFrames),
                static_cast<unsigned long long>(decoderStats.decodedFrames),
                static_cast<unsigned long long>(decoderStats.droppedFrames));
            if (!decoderStats.status.empty())
            {
                ImGui::TextDisabled("Decoder: %s", decoderStats.status.c_str());
            }
        }

        impl.frameViewForRender.Attach(impl.decoder.AcquireFrameView());
        if (impl.frameViewForRender && decoderStats.width > 0 && decoderStats.height > 0)
        {
            ImGui::Separator();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float scale = std::max(
                0.0F,
                std::min(
                    available.x / static_cast<float>(decoderStats.width),
                    available.y / static_cast<float>(decoderStats.height)));
            ImGui::Image(
                reinterpret_cast<ImTextureID>(impl.frameViewForRender.Get()),
                ImVec2(
                    std::max(1.0F, static_cast<float>(decoderStats.width) * scale),
                    std::max(1.0F, static_cast<float>(decoderStats.height) * scale)));
        }
        else
        {
            ImGui::TextDisabled("Receiver output: H.264 GPU-decoded preview appears here after Sunshine sends the first frame.");
        }
        custom::EndChild();
    }

    void StreamReceiverController::Shutdown()
    {
        if (implementation_ == nullptr)
        {
            return;
        }
        implementation_->stopping.store(true);
        implementation_->StopStream();
        implementation_->JoinWorker();
        implementation_->decoder.Shutdown();
        implementation_->frameViewForRender.Reset();
    }

    bool StreamReceiverController::RunLiveStreamSmokeTest(
        const std::string& requestedHost,
        const std::uint32_t timeoutMilliseconds,
        SunshineStreamSmokeTestResult& result)
    {
        result = {};
        if (implementation_ == nullptr || implementation_->device == nullptr)
        {
            result.message = "Vanta's D3D11 receiver device is not initialized";
            return false;
        }
        if (requestedHost.empty() || requestedHost.size() >= sizeof(implementation_->host))
        {
            result.message = "The Sunshine host is empty or too long";
            return false;
        }
        if (timeoutMilliseconds < 1000 || timeoutMilliseconds > 180000)
        {
            result.message = "The live receiver test timeout must be between 1 and 180 seconds";
            return false;
        }

        auto& impl = *implementation_;
        if (impl.busy.load() || impl.moonlightStarting.load() || impl.moonlightStarted.load())
        {
            result.message = "Vanta already has a Sunshine receiver operation in progress";
            return false;
        }

        // Use a modest but representative LAN profile. Crucially, a smoke
        // test must not change the host's physical/virtual display mode.
        std::memset(impl.host, 0, sizeof(impl.host));
        std::memcpy(impl.host, requestedHost.data(), requestedHost.size());
        impl.requestedWidth = 1920;
        impl.requestedHeight = 1080;
        impl.requestedFps = 60;
        impl.requestedBitrateKbps = 30000;
        impl.requestDisplayMode = false;
        impl.stopping.store(false);
        impl.SetStatus("Starting Vanta's pre-paired Sunshine live-stream smoke test...");
        impl.StartStream();

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMilliseconds);
        for (;;)
        {
            Tick();
            const DecodedVideoStats liveStats = impl.decoder.GetStats();
            ID3D11ShaderResourceView* frameView = impl.decoder.AcquireFrameView();
            if (frameView != nullptr)
            {
                frameView->Release();
                if (liveStats.decodedFrames != 0 && liveStats.width > 0 && liveStats.height > 0)
                {
                    result.receivedFrame = true;
                    result.width = liveStats.width;
                    result.height = liveStats.height;
                    break;
                }
            }

            if (!impl.busy.load() &&
                !impl.moonlightStarting.load() &&
                !impl.moonlightStarted.load())
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const DecodedVideoStats statsBeforeStop = impl.decoder.GetStats();
        std::string statusBeforeStop;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            statusBeforeStop = impl.status;
        }

        // Do all teardown while the callback endpoint and decoder are still
        // alive. The receiver's regular shutdown path handles a launch that
        // failed before common-c reported a started connection too.
        impl.stopping.store(true);
        impl.StopStream();
        impl.JoinWorker();

        result.submittedFrames = statsBeforeStop.submittedFrames;
        result.decodedFrames = statsBeforeStop.decodedFrames;
        result.droppedFrames = statsBeforeStop.droppedFrames;
        if (result.receivedFrame)
        {
            result.message = "Vanta received and copied a live Sunshine H.264 frame";
            vanta::log::Info(
                "Sunshine live smoke test passed: %dx%d, submitted=%llu decoded=%llu dropped=%llu",
                result.width,
                result.height,
                static_cast<unsigned long long>(result.submittedFrames),
                static_cast<unsigned long long>(result.decodedFrames),
                static_cast<unsigned long long>(result.droppedFrames));
            return true;
        }

        result.message = statusBeforeStop.empty()
            ? "Vanta did not receive a decoded Sunshine frame before the test ended"
            : statusBeforeStop;
        vanta::log::Error(
            "Sunshine live smoke test failed: %s (submitted=%llu decoded=%llu dropped=%llu)",
            result.message.c_str(),
            static_cast<unsigned long long>(result.submittedFrames),
            static_cast<unsigned long long>(result.decodedFrames),
            static_cast<unsigned long long>(result.droppedFrames));
        return false;
    }

    bool StreamReceiverController::CopyLatestFrameToBgra(
        const std::uint64_t afterSequence,
        cv::Mat& output,
        std::uint64_t& sequence)
    {
        if (implementation_ == nullptr)
        {
            return false;
        }
        return implementation_->decoder.CopyLatestFrameToBgra(
            afterSequence,
            output,
            sequence);
    }
}
