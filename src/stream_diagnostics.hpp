#pragma once
#include "capture_controller.hpp"
#include "hardware_video_decoder.hpp"
#include <deque>
#include <memory>

namespace vanta
{
    struct StreamDiagnosticSample
    {
        std::uint64_t nowUs{};
        SunshineStreamConfig requested;
        DecodedVideoStats video;
        CapturedFrameMetadata publishedMetadata;
        CaptureController::DemandStats demand;
        std::vector<CaptureController::ConsumerPerformance> consumers;
        std::string captureJson{"{}"},outputJson{"{}"};
        double sampleIntervalMs{},processCpuPercent{},processCpuCores{},systemCpuPercent{};
        bool cpuLoadAvailable{};
        bool connectionTerminated{};
        int terminationError{};
        std::uint64_t workingSetBytes{},availablePhysicalBytes{};
    };
    struct FpsFinding
    {
        std::string code,stage,severity,confidence,evidence,nextStep;
        bool problem{};
    };
    std::vector<FpsFinding> DiagnoseStream(const StreamDiagnosticSample& sample,const StreamDiagnosticSample* previous=nullptr);
    // Pure state machine shared by the live reporter and deterministic checks.
    class AutomaticReportGate
    {
    public:
        void Reset(std::uint64_t now);
        bool Due(std::uint64_t now,const std::string& signature,bool terminal);
        void Queued(std::uint64_t now);
    private:
        std::uint64_t startedUs_{},candidateUs_{},lastReportUs_{};
        unsigned reports_{};
        std::string candidate_;
    };
    class StreamDiagnostics
    {
    public:
        StreamDiagnostics();
        ~StreamDiagnostics();
        void Initialize(std::string hardwareJson);
        void Observe(StreamDiagnosticSample sample);
        void EndSession(const char* reason);
        void SaveManual();
        std::string Summary() const;
        std::string Status() const;
        void Shutdown();
    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
    bool RunStreamDiagnosticsSelfTest();
}
