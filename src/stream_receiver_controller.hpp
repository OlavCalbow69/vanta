#pragma once
#include "app_settings.hpp"
#include "stream_frame.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace cv { class Mat; }

struct ID3D11Device;

namespace vanta
{
    class CaptureController;
    // A deliberately short, opt-in check for a host that has already been
    // paired with this Windows user. It launches the configured Sunshine
    // Desktop session with display-mode switching disabled, waits for Vanta's
    // hardware decoder to verify a D3D11 frame, and stops the session again.
    // It is used by the command-line live smoke test; normal UI streaming
    // remains long-running.
    struct SunshineStreamSmokeTestResult
    {
        bool receivedFrame{};
        int width{};
        int height{};
        std::uint64_t submittedFrames{};
        std::uint64_t decodedFrames{};
        std::uint64_t droppedFrames{};
        std::string message;
    };

    // Discovers Sunshine hosts through the unauthenticated GameStream
    // server-info endpoint. Pairing and stream setup are deliberately separate
    // stages, so no credentials are sent during discovery.
    class StreamReceiverController
    {
    public:
        StreamReceiverController();
        ~StreamReceiverController();

        StreamReceiverController(const StreamReceiverController&) = delete;
        StreamReceiverController& operator=(const StreamReceiverController&) = delete;

        void Initialize(ID3D11Device* device, const SunshineStreamConfig* config = nullptr,CaptureController* capture = nullptr);
        SunshineStreamConfig GetConfig() const;
        std::uint64_t SettingsRevision() const noexcept;
        void Tick();
        void SetDiagnosticContextProvider(std::function<std::string()> provider);
        void FinishPreviewFrame();
        std::uint64_t CurrentGeneration() const;
        void RecordPublication(const CapturedFrameMetadata& metadata);
        void InvalidateFrames();
        bool ReadbackRegions(const FrameReadbackRequest& request,CpuFrameSnapshot& output,std::stop_token stop);
        void RenderPanel();
        void Shutdown();

        // Requires Initialize() and an already-paired host. This operation
        // intentionally has no pairing behavior; callers must explicitly
        // pair through Vanta's Stream Receiver page first.
        [[nodiscard]] bool RunLiveStreamSmokeTest(
            const std::string& host,
            std::uint32_t timeoutMilliseconds,
            SunshineStreamSmokeTestResult& result);

        // Supplies the next decoded BGRA frame to Vanta's optional Sunshine
        // capture source. The receiver page itself never calls this path, so
        // its preview remains entirely on the GPU.
        [[nodiscard]] bool CopyLatestFrameToBgra(
            std::uint64_t afterSequence,
            cv::Mat& output,
            std::uint64_t& sequence);

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
