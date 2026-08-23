#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vanta::sunshine
{
    struct HostInfo
    {
        std::string address;
        std::string name;
        std::string appVersion;
        std::string gfeVersion;
        std::uint16_t httpsPort{47984};
        int codecSupport{};
    };

    struct PairedHost
    {
        HostInfo info;
        // This is the Sunshine TLS certificate in PEM form. It is kept in the
        // per-user encrypted pairing store and is used as a certificate pin
        // for every authenticated GameStream request.
        std::string certificatePem;
    };

    struct PairingResult
    {
        bool succeeded{};
        std::string message;
        std::optional<PairedHost> host;
    };

    struct StreamRequest
    {
        int width{1920};
        int height{1080};
        int fps{120};
        int bitrateKbps{80000};
        int packetSize{1392};
        // Ask Sunshine to use the requested stream mode. This is Moonlight's
        // normal default for Sunshine and lets a host/virtual display match
        // the requested dimensions and refresh rate.
        bool requestDisplayMode{true};
        // AUDIO_CONFIGURATION_STEREO without pulling protocol C headers into
        // this public C++ interface.
        int audioConfiguration{0x000302CA};
        std::array<unsigned char, 16> remoteInputKey{};
        std::array<unsigned char, 16> remoteInputIv{};
    };

    struct StreamLaunchResult
    {
        bool succeeded{};
        std::string message;
        PairedHost host;
        int appId{};
        std::string appTitle;
        std::string rtspSessionUrl;
    };

    // Implements the GameStream/Sunshine pairing sequence used by Moonlight
    // clients. Private credentials and host certificate pins are protected
    // with Windows DPAPI under the current user's LocalAppData profile.
    class PairingService
    {
    public:
        PairingService();
        ~PairingService();

        PairingService(const PairingService&) = delete;
        PairingService& operator=(const PairingService&) = delete;

        [[nodiscard]] std::optional<HostInfo> Discover(
            const std::string& host,
            std::string& error) const;

        // Browses Sunshine's standard Moonlight/GameStream DNS-SD service
        // (_nvstream._tcp.local) on the current LAN, then validates each
        // advertised endpoint through its unauthenticated server-info API.
        // Discovery does not send pairing credentials.
        [[nodiscard]] std::vector<HostInfo> DiscoverLan(
            std::string& error) const;

        // Creates a cryptographically random four-digit code for the user to
        // enter in Sunshine's web UI after Vanta begins pairing.
        [[nodiscard]] std::string GeneratePairingPin(
            std::string& error) const;

        [[nodiscard]] PairingResult Pair(
            const std::string& host,
            const std::string& pin,
            const std::function<void(const std::string&)>& setProgress = {});

        [[nodiscard]] std::optional<PairedHost> LoadPairedHost(
            const std::string& host,
            std::string& error) const;

        // Starts the selected Sunshine application over the authenticated,
        // certificate-pinned HTTPS GameStream API. This only performs the
        // HTTPS launch step; the caller passes the returned session URL to
        // moonlight-common-c for the actual UDP transport.
        [[nodiscard]] StreamLaunchResult LaunchDesktop(
            const std::string& host,
            const StreamRequest& request);

    private:
        struct Implementation;
        Implementation* implementation_{};
    };
}
