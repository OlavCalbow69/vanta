#pragma once
#include "app_settings.hpp"
#include "stream_frame.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ImDrawList;
struct ImVec2;
namespace vanta
{
    struct VideoAdapterInfo { std::string id,name,driver; int supportedFormats{}; bool nvidia{}; std::string displays; };
    struct DecodedVideoStats
    {
        int width{},height{},configuredFps{};
        double receivedFramesPerSecond{},decodedFramesPerSecond{},publishedFramesPerSecond{},encodedMegabitsPerSecond{};
        std::uint64_t receivedFrames{},submittedFrames{},decodedFrames{},droppedFrames{};
        std::uint64_t transportLostFrames{},overloadDrops{},decoderFailures{},outputSkippedFrames{},idrRequests{};
        std::uint64_t publishedFrames{},readbackBytes{},generation{},session{};
        std::uint64_t sampledUs{},lastReceivedUs{},lastPublishedUs{};
        std::uint64_t readbacksIssued{},readbacksCompleted{},readbackSuperseded{},readbackSlotPressure{},cpuPoolExhaustions{},nonblockingMapRetries{};
        std::size_t queueDepth{};
        std::size_t gpuFramesInFlight{};
        bool hardwareActive{},failed{};
        std::string status,codec,adapter,synchronization;
        CapturedFrameMetadata latestMetadata;
        ReceiverNetworkTraffic network;
        TimingSummary reassembly,decode,readback,publication,hostProcessing;
        TimingSummary compressedQueue,decodeSubmit,gpuCompletionObserved,cpuCopy,publicationDispatch;
    };
    class HardwareVideoDecoder
    {
    public:
        HardwareVideoDecoder();
        ~HardwareVideoDecoder();
        HardwareVideoDecoder(const HardwareVideoDecoder&)=delete;
        HardwareVideoDecoder& operator=(const HardwareVideoDecoder&)=delete;
        static std::vector<VideoAdapterInfo> EnumerateAdapters();
        static bool HasHardwareDecoder(int videoFormat);
        static bool CreateRenderDevice(const std::string& preference,ID3D11Device** device,
                                       ID3D11DeviceContext** context,std::string& error);
        bool Initialize(ID3D11Device* device,std::string& error);
        void Shutdown();
        int SupportedFormats(int width,int height) const;
        int Configure(int videoFormat,int width,int height,int redrawRate,std::string& error);
        void Start();
        void SetIdrRequester(std::function<void()> requester);
        void Stop();
        int Submit(PDECODE_UNIT decodeUnit);
        bool Readback(const FrameReadbackRequest& request,CpuFrameSnapshot& output,std::stop_token stop);
        void WakeWaiters();
        std::uint64_t CurrentGeneration() const;
        void RecordPublication(const CapturedFrameMetadata& metadata);
        void DrawPreview(ImDrawList* list,const ImVec2& position,const ImVec2& size);
        void FinishPreviewFrame();
        bool CopyLatestFrameToBgra(std::uint64_t afterSequence,cv::Mat& output,std::uint64_t& sequence);
        DecodedVideoStats GetStats() const;
        bool SaveDiagnostics(const SunshineStreamConfig& config,const std::string& captureJson,std::string& result,const std::string& consumerCsv={}) const;
    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
