#pragma once

#include "app_settings.hpp"
#include "stream_frame.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace cv { class Mat; }

namespace vanta
{
    bool RunSunshineCaptureSourceSelfTest();

    // Supplies the union of requested native source regions, with associated
    // stream metadata. Waits must honor cancellation and avoid idle polling.
    using SunshineRegionFrameProvider = std::function<bool(const FrameReadbackRequest&,CpuFrameSnapshot&,std::stop_token)>;

    class CaptureController
    {
    public:
        CaptureController();
        ~CaptureController();

        CaptureController(const CaptureController&) = delete;
        CaptureController& operator=(const CaptureController&) = delete;

        bool Initialize(
            ID3D11Device* previewDevice,
            ID3D11DeviceContext* previewContext,
            const CaptureConfig* initialConfig = nullptr);
        void SetRegionFrameProvider(SunshineRegionFrameProvider provider);
        void SetGenerationProvider(std::function<std::uint64_t()> provider);
        void SetPublicationObserver(std::function<void(const CapturedFrameMetadata&)> observer);
        bool IsCurrent(const CapturedFrameMetadata& metadata) const;
        CaptureSubscription Subscribe(const FrameRegionRequest& request,const char* consumerName="Unnamed");
        struct RegionDemand {std::string consumer;FrameRegionRequest request;cv::Rect resolved;};
        struct DemandStats
        {
            bool running{},workerFailed{},streamRate{};
            int maximumFps{};
            TimingSummary schedulerWakeLateness;
            std::string waitMode;
            cv::Rect readbackUnion;
            std::vector<RegionDemand> regions;
        };
        DemandStats GetDemandStats(int sourceWidth,int sourceHeight) const;
        // An armed feature may deliberately wait for MAKCU or its activation
        // button. Its zero processed FPS must not be called a CPU bottleneck.
        void SetConsumerState(const char* name,bool processingExpected,const char* state);
        void RecordConsumer(const char* name,const CapturedFrameMetadata& metadata,std::uint64_t startedUs);
        void RecordConsumerAlgorithm(const char* name,const CapturedFrameMetadata& metadata,std::uint64_t startedUs);
        CapturedFrameMetadata LastPublicationMetadata() const;
        std::string DiagnosticsJson() const;
        std::string DiagnosticsCsv() const;
        struct ConsumerPerformance {std::string name;double fps{};std::uint64_t frames{};TimingSummary processing,age;bool processingExpected{};std::string state;TimingSummary algorithm;};
        std::vector<ConsumerPerformance> ConsumerPerformanceStats() const;
        void Shutdown();
        void Tick();
        void RenderPanel();
        void SetColorTargetIndex(int index) noexcept;
        CaptureConfig GetConfig() const;
        void ApplyConfig(const CaptureConfig& config);
        std::uint64_t SettingsRevision() const noexcept;
        // Draw the selected FOV inside the decoded ROI preview, never on the
        // receiver desktop (remote frames have no local screen coordinates).
        void SetFovPreview(bool visible, int radius, const RgbaColor& fovColor);
        // Returns the most recently processed centered capture frame (BGRA, CV_8UC4).
        // Returns an empty Mat if no frame is available yet.
        bool GetLatestCenteredFrame(cv::Mat& out) const;
        bool WaitForCenteredFrame(
            std::uint64_t afterSequence,
            cv::Mat& out,
            std::uint64_t& sequence,
            std::int64_t& captureTimestampNanoseconds,
            std::uint32_t timeoutMilliseconds) const;
        bool WaitForCenteredFrameWithMetadata(
            std::uint64_t afterSequence,
            cv::Mat& out,
            CapturedFrameMetadata& metadata,
            std::uint32_t timeoutMilliseconds) const;
        // Full native-resolution source, independent of the normal Vanta ROI.
        // Source/ROI metadata and sequence are published under the same lock.
        bool WaitForSourceFrameWithMetadata(
            std::uint64_t afterSequence,
            cv::Mat& out,
            CapturedFrameMetadata& metadata,
            std::uint32_t timeoutMilliseconds) const;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
    class CaptureWorkTimer
    {
    public:
        CaptureWorkTimer(CaptureController* owner,const char* name,const CapturedFrameMetadata& metadata)
            :owner_(owner),name_(name),metadata_(metadata),started_(StreamNowUs()){}
        ~CaptureWorkTimer(){if(owner_)owner_->RecordConsumer(name_,metadata_,started_);}
    private:
        CaptureController* owner_;
        const char* name_;
        CapturedFrameMetadata metadata_;
        std::uint64_t started_;
    };
}
