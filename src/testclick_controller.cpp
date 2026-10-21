#define IMGUI_DEFINE_MATH_OPERATORS

#include "testclick_controller.hpp"

#include "capture_controller.hpp"
#include "color_targets.hpp"
#include "makcu_controller.hpp"
#include "logger.hpp"
#include "input_backend_utils.hpp"

#include "imgui.h"
#include "imgui_settings.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace
{
    enum class TestClickState : int
    {
        offline,
        disabled,
        waitingForKey,
        waitingForCapture,
        waitingForOutput,
        watching,
        firing
    };

    const char* StateLabel(TestClickState currentState)
    {
        switch (currentState)
        {
        case TestClickState::disabled:
            return "DISABLED";
        case TestClickState::waitingForKey:
            return "WAITING FOR HOLD BUTTON";
        case TestClickState::waitingForCapture:
            return "WAITING FOR CAPTURE";
        case TestClickState::waitingForOutput:
            return "OUTPUT BLOCKED";
        case TestClickState::watching:
            return "WATCHING";
        case TestClickState::firing:
            return "FIRING";
        default:
            return "OFFLINE";
        }
    }

    struct ClickRepeatState
    {
        int burstCount{};
        double lastShotTime{};
        void Reset(){burstCount=0;lastShotTime=0.0;}
        bool Due(bool limited,int size,double delay,double pause,double now)
        {
            if(!limited)return true;
            if(burstCount>=size)
            {
                if(now-lastShotTime<=pause)return false;
                burstCount=0;
            }
            return now-lastShotTime>delay;
        }
        void Completed(bool limited,double now){lastShotTime=now;if(limited)++burstCount;}
    };

    // Packed 24-bit key for BGR: B | (G<<8) | (R<<16)
    inline std::uint32_t BgrKey(std::uint8_t b, std::uint8_t g, std::uint8_t r)
    {
        return static_cast<std::uint32_t>(b) |
               (static_cast<std::uint32_t>(g) << 8) |
               (static_cast<std::uint32_t>(r) << 16);
    }

    const std::unordered_set<std::uint32_t>& Blacklist()
    {
        static const std::unordered_set<std::uint32_t> set = []
        {
            const std::array<std::array<std::uint8_t, 3>, 75> entries{{
                {253,175,252}, {233,160,252}, {254,173,254}, {217,141,165},
                {189,135,173}, {196,102,120}, {243,162,221}, {235,114,137},
                {230,162,208}, {192,114,176}, {199,133,182}, {207,152,202},
                {233,164,214}, {61, 22, 87 }, {97, 34, 112}, {67, 30, 38 },
                {147,65, 139}, {134,61, 158}, {244,107,245}, {254,110,254},
                {250,78, 255}, {151,38, 201}, {251,85, 254}, {138,31, 187},
                {130,10, 123}, {151,17, 154}, {113,1,  101}, {162,23, 168},
                {123,9,  114}, {153,42, 209}, {94, 23, 135}, {72, 12, 92 },
                {55, 19, 67 }, {84, 1,  75 }, {37, 0,  39 }, {46, 3,  37 },
                {141,16, 160}, {55, 1,  61 }, {65, 2,  73 }, {80, 3,  93 },
                {72, 3,  73 }, {254,126,255}, {255,49, 255}, {136,15, 133},
                {129,3,  108}, {254,94, 255}, {231,62, 255}, {205,36, 223},
                {255,140,255}, {234,86, 255}, {188,29, 197}, {255,146,254},
                {158,8,  139}, {172,12, 157}, {132,31, 134}, {194,50, 197},
                {183,79, 195}, {206,55, 203}, {238,67, 240}, {139,2,  116},
                {254,211,255}, {177,14, 162}, {210,51, 253}, {72, 16, 117},
                {255,189,255}, {255,142,255}, {252,61, 250}, {78, 22, 79 },
                {252,58, 250}, {140,49, 225}, {253,170,252}, {147,26, 167},
                {253,142,251}, {125,22, 143}, {63, 2,  69 }
            }};
            std::unordered_set<std::uint32_t> s;
            s.reserve(entries.size());
            for (const auto& e : entries)
            {
                s.insert(BgrKey(e[0], e[1], e[2]));
            }
            return s;
        }();
        return set;
    }

    int RemoveBlacklistedAndCount(
        const cv::Mat& bgr,
        cv::Mat& mask)
    {
        const auto& blacklist = Blacklist();
        int matches = 0;
        for (int row = 0; row < bgr.rows; ++row)
        {
            const cv::Vec3b* colors =
                bgr.ptr<cv::Vec3b>(row);
            std::uint8_t* values =
                mask.ptr<std::uint8_t>(row);
            for (int column = 0;
                 column < bgr.cols;
                 ++column)
            {
                if (values[column] == 0)
                {
                    continue;
                }
                const auto& pixel = colors[column];
                if (blacklist.count(
                        BgrKey(
                            pixel[0],
                            pixel[1],
                            pixel[2])) != 0)
                {
                    values[column] = 0;
                    continue;
                }
                ++matches;
            }
        }
        return matches;
    }

    struct RayResults
    {
        bool hitUp{false};
        bool hitLeft{false};
        bool hitRight{false};

        int TotalHits() const
        {
            return (hitUp ? 1 : 0) + (hitLeft ? 1 : 0) + (hitRight ? 1 : 0);
        }
    };

    // ──────────────────────────────────────────────────────────────────
    // Ultra-Fast Zero-Delay Ray Marching
    // Directly checks pointer rows without overhead or intermediate allocations.
    // ──────────────────────────────────────────────────────────────────
    RayResults FastRayMarching(
        const cv::Mat& bgr,
        const cv::Mat& mask,
        int cx,
        int cy,
        int rayLength,
        int rayHalf,
        int requiredHits,
        bool applyBlacklist)
    {
        RayResults res;
        const auto& bl = Blacklist();

        const int rows = mask.rows;
        const int cols = mask.cols;

        // Up Ray
        for (int step = 1; step <= rayLength; ++step)
        {
            const int y = cy - step;
            if (y < 0) break;

            const std::uint8_t* mRow = mask.ptr<std::uint8_t>(y);
            const cv::Vec3b*    cRow = bgr.ptr<cv::Vec3b>(y);

            const int minX = std::max(0, cx - rayHalf);
            const int maxX = std::min(cols - 1, cx + rayHalf);

            for (int x = minX; x <= maxX; ++x)
            {
                if (mRow[x] != 0)
                {
                    const auto& px = cRow[x];
                    if (!applyBlacklist || !bl.count(BgrKey(px[0], px[1], px[2])))
                    {
                        res.hitUp = true;
                        break;
                    }
                }
            }
            if (res.hitUp) break;
        }
        if (res.TotalHits() >= requiredHits ||
            res.TotalHits() + 2 < requiredHits)
        {
            return res;
        }

        // Left Ray
        for (int step = 1; step <= rayLength; ++step)
        {
            const int x = cx - step;
            if (x < 0) break;

            const int minY = std::max(0, cy - rayHalf);
            const int maxY = std::min(rows - 1, cy + rayHalf);

            for (int y = minY; y <= maxY; ++y)
            {
                if (mask.ptr<std::uint8_t>(y)[x] != 0)
                {
                    const auto& px = bgr.ptr<cv::Vec3b>(y)[x];
                    if (!applyBlacklist || !bl.count(BgrKey(px[0], px[1], px[2])))
                    {
                        res.hitLeft = true;
                        break;
                    }
                }
            }
            if (res.hitLeft) break;
        }
        if (res.TotalHits() >= requiredHits ||
            res.TotalHits() + 1 < requiredHits)
        {
            return res;
        }

        // Right Ray
        for (int step = 1; step <= rayLength; ++step)
        {
            const int x = cx + step;
            if (x >= cols) break;

            const int minY = std::max(0, cy - rayHalf);
            const int maxY = std::min(rows - 1, cy + rayHalf);

            for (int y = minY; y <= maxY; ++y)
            {
                if (mask.ptr<std::uint8_t>(y)[x] != 0)
                {
                    const auto& px = bgr.ptr<cv::Vec3b>(y)[x];
                    if (!applyBlacklist || !bl.count(BgrKey(px[0], px[1], px[2])))
                    {
                        res.hitRight = true;
                        break;
                    }
                }
            }
            if (res.hitRight) break;
        }

        return res;
    }
}

namespace vanta
{
    struct TestClickController::Implementation
    {
        std::mutex settingsMutex;
        bool       enabled{false};
        int        hsvRangeIndex{0};           // 0 = current, 1 = alternative
        MouseButtonBinding holdButton{
            MouseButtonBinding::x1};
        int        roiOffset{50};            // half-side of capture square (px)
        int        rayLength{49};            // max step length along each ray (px)
        int        rayHalfWidth{1};          // thickness tolerance (+/- px)
        int        hitMode{0};               // 0 = Strict (3/3 All Rays), 1 = Flexible (2/3 Rays)
        bool       burstLimiter{false};       // false = click every matching frame
        float      burstDelay{0.05F};        // low delay default
        float      burstPause{0.20F};
        int        burstSize{3};

        CaptureController* capture{nullptr};
        CaptureSubscription frameSubscription;
        MakcuController*   makcu{nullptr};
        std::thread        workerThread;
        std::atomic_bool   running{false};

        std::atomic<int>  shotsFired{0};
        std::atomic<bool> threadAlive{false};
        std::atomic<bool> triggerActive{false};
        std::atomic<int>  lastHits{0};
        std::atomic<bool> hitUpState{false};
        std::atomic<bool> hitLeftState{false};
        std::atomic<bool> hitRightState{false};
        std::atomic<int> matchingPixels{0};
        std::atomic<TestClickState> state{TestClickState::offline};
        std::atomic<std::uint64_t> settingsRevision{0};

        ClickRepeatState repeat;

        static double NowSeconds()
        {
            using namespace std::chrono;
            return duration<double>(
                steady_clock::now().time_since_epoch()).count();
        }

        void WorkerLoop()
        {
            threadAlive.store(true);
            state.store(TestClickState::disabled);
            vanta::log::Info("TestClick worker started (Zero-Delay Condition Variable Sync)");

            std::uint64_t lastSequence = 0;
            cv::Mat frame;
            std::uint64_t sequence = 0;
            std::int64_t timestamp = 0;
            double nextPixelSampleTime = 0.0;
            std::uint64_t repeatRevision=~0ULL;

            while (running.load(std::memory_order_acquire))
            {
                std::uint64_t outputGeneration{};
                bool  snapEnabled, snapBurstLimiter;
                MouseButtonBinding snapHoldButton;
                int   snapHsvRange, snapRoi, snapRayLength, snapRayHalf, snapHitMode, snapBurstSize;
                float snapBurstDelay, snapBurstPause;
                {
                    std::lock_guard<std::mutex> lk(settingsMutex);
                    snapEnabled          = enabled;
                    snapHsvRange         = hsvRangeIndex;
                    snapHoldButton       = holdButton;
                    snapRoi              = roiOffset;
                    snapRayLength        = rayLength;
                    snapRayHalf          = rayHalfWidth;
                    snapHitMode          = hitMode;
                    snapBurstLimiter     = burstLimiter;
                    snapBurstSize        = burstSize;
                    snapBurstDelay       = burstDelay;
                    snapBurstPause       = burstPause;
                    outputGeneration=settingsRevision.load();
                }

                if(repeatRevision!=outputGeneration){repeat.Reset();repeatRevision=outputGeneration;}
                if(makcu)makcu->SetInputPolicy(InputOwner::testClick,snapEnabled,outputGeneration,snapHoldButton);
                frameSubscription.Update({FrameRegionKind::configuredCapture,640,{},0,snapEnabled});
                if(capture)capture->SetConsumerState("TestClick",false,snapEnabled ? "Waiting for activation / mouse output" : "Disabled");
                if (!snapEnabled)
                {
                    repeat.Reset();
                    state.store(TestClickState::disabled);
                    triggerActive.store(false);
                    lastHits.store(0);
                    matchingPixels.store(0);
                    hitUpState.store(false);
                    hitLeftState.store(false);
                    hitRightState.store(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                if (makcu == nullptr ||
                    !makcu->IsMouseButtonInputAvailable())
                {
                    state.store(TestClickState::waitingForOutput);
                    triggerActive.store(false);
                    lastHits.store(0);
                    matchingPixels.store(0);
                    hitUpState.store(false);
                    hitLeftState.store(false);
                    hitRightState.store(false);
                    repeat.Reset();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                if (!makcu->IsMouseButtonPressed(snapHoldButton))
                {
                    state.store(TestClickState::waitingForKey);
                    triggerActive.store(false);
                    lastHits.store(0);
                    matchingPixels.store(0);
                    hitUpState.store(false);
                    hitLeftState.store(false);
                    hitRightState.store(false);
                    repeat.Reset();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                CpuFrameSnapshot snapshot;
                if(capture)capture->SetConsumerState("TestClick",true,"Processing while activation is held");
                if (capture == nullptr || !frameSubscription.Wait(lastSequence,snapshot,10))
                {
                    state.store(TestClickState::waitingForCapture);
                    triggerActive.store(false);
                    continue;
                }
                frame=snapshot.bgra;
                sequence=snapshot.metadata.sequence;
                timestamp=snapshot.metadata.captureTimestampNanoseconds;
                CaptureWorkTimer workTimer(capture,"TestClick",snapshot.metadata);
                lastSequence = sequence;

                const int fcx = frame.cols / 2;
                const int fcy = frame.rows / 2;
                const int x1 = std::max(0, fcx - snapRoi);
                const int y1 = std::max(0, fcy - snapRoi);
                const int x2 = std::min(frame.cols, fcx + snapRoi);
                const int y2 = std::min(frame.rows, fcy + snapRoi);

                if (x2 <= x1 || y2 <= y1)
                {
                    state.store(TestClickState::waitingForCapture);
                    continue;
                }

                const cv::Mat bgraRoi =
                    frame(cv::Rect(x1, y1, x2 - x1, y2 - y1));
                cv::Mat bgrRoi;
                cv::cvtColor(
                    bgraRoi,
                    bgrRoi,
                    cv::COLOR_BGRA2BGR);
                const int lcx = bgrRoi.cols / 2;
                const int lcy = bgrRoi.rows / 2;

                cv::Mat hsv, mask;
                cv::cvtColor(
                    bgrRoi,
                    hsv,
                    cv::COLOR_BGR2HSV);
                vanta::BuildHsvMask(hsv, snapHsvRange, mask);

                const double diagnosticTime = NowSeconds();
                if (diagnosticTime >= nextPixelSampleTime)
                {
                    matchingPixels.store(vanta::ColorTarget(snapHsvRange).purpleBlacklist
                        ? RemoveBlacklistedAndCount(bgrRoi, mask)
                        : cv::countNonZero(mask));
                    nextPixelSampleTime =
                        diagnosticTime + 0.10;
                }

                const int effectiveRayLength =
                    std::min(
                        snapRayLength,
                        std::max(1, snapRoi - 1));
                const int requiredHits =
                    snapHitMode == 0 ? 3 : 2;
                RayResults rays = FastRayMarching(
                    bgrRoi,
                    mask,
                    lcx,
                    lcy,
                    effectiveRayLength,
                    snapRayHalf,
                    requiredHits,
                    vanta::ColorTarget(snapHsvRange).purpleBlacklist);

                hitUpState.store(rays.hitUp);
                hitLeftState.store(rays.hitLeft);
                hitRightState.store(rays.hitRight);
                lastHits.store(rays.TotalHits());

                const bool matchFound =
                    rays.TotalHits() >= requiredHits;

                triggerActive.store(matchFound);
                state.store(
                    matchFound
                        ? TestClickState::firing
                        : TestClickState::watching);

                if (matchFound)
                {
                    const double now = NowSeconds();
                    if (repeat.Due(snapBurstLimiter,snapBurstSize,snapBurstDelay,snapBurstPause,now))
                    {
                        OutputContext output;output.owner=InputOwner::testClick;output.generation=outputGeneration;
                        const auto metadata=snapshot.metadata;
                        output.streamGeneration=metadata.generation;output.streamSession=metadata.session;output.sequence=metadata.sequence;
                        output.frameTimeUs=metadata.timing.receivedUs?metadata.timing.receivedUs:metadata.timing.publishedUs;
                        output.activation=snapHoldButton;
                        output.isCurrent=[this,metadata,outputGeneration]{return running.load() && settingsRevision.load()==outputGeneration && capture && capture->IsCurrent(metadata);};
                        const bool clicked = makcu && makcu->TryClick(output);
                        if (clicked)
                        {
                            repeat.Completed(snapBurstLimiter,now);
                            shotsFired.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                        else
                        {
                            state.store(
                                TestClickState::waitingForOutput);
                        }
                    }
                }
                else
                {
                    repeat.Reset();
                }
            }

            threadAlive.store(false);
            triggerActive.store(false);
            state.store(TestClickState::offline);
            vanta::log::Info("TestClick worker stopped");
        }

        void Start()
        {
            if (running.load()) return;
            running.store(true, std::memory_order_release);
            workerThread = std::thread([this]
            {
                try{WorkerLoop();}
                catch(const std::exception& error){vanta::log::Error("TestClick worker failed: %s",error.what());}
                catch(...){vanta::log::Error("TestClick worker failed with an unknown exception");}
                running.store(false);threadAlive.store(false);
                if(makcu)makcu->CancelOutput(InputOwner::testClick);
            });
        }

        void Stop()
        {
            running.store(false, std::memory_order_release);
            if(makcu)makcu->CancelOutput(InputOwner::testClick);
            if (workerThread.joinable())
            {
                workerThread.join();
            }
            if(makcu)makcu->CancelOutput(InputOwner::testClick);
            frameSubscription.Update({FrameRegionKind::configuredCapture,640,{},0,false});
            state.store(TestClickState::offline);
            makcu=nullptr;capture=nullptr;
        }
    };

    TestClickController::TestClickController()
        : implementation_(std::make_unique<Implementation>())
    {
    }

    TestClickController::~TestClickController()
    {
        Shutdown();
    }

    void TestClickController::Initialize(
        CaptureController* capture,
        MakcuController* makcu)
    {
        auto& impl = *implementation_;
        impl.capture = capture;
        if (capture) impl.frameSubscription=capture->Subscribe({FrameRegionKind::configuredCapture,640,{},0,false},"TestClick");
        impl.makcu   = makcu;
        impl.Start();
        vanta::log::Info("TestClick controller initialized");
    }

    void TestClickController::Shutdown()
    {
        implementation_->Stop();
    }

    void TestClickController::Tick()
    {
    }

    void TestClickController::RenderPanel()
    {
        auto& impl = *implementation_;

        const float bodyHeight = std::max(
            190.0F,
            ImGui::GetContentRegionAvail().y - 40.0F);

        custom::Child(
            ICON_AIMING_LINE "  TestClick##testclick-panel",
            ImVec2(0.0F, bodyHeight),
            true);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0F, 12.0F));

        const TestClickState currentState =
            impl.state.load();

        const ImVec4 statusColor =
            currentState == TestClickState::firing
                ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                : currentState == TestClickState::watching
                    ? ImVec4(0.45F, 0.96F, 0.65F, 1.0F)
                    : currentState == TestClickState::waitingForKey ||
                            currentState == TestClickState::waitingForCapture ||
                            currentState == TestClickState::waitingForOutput
                        ? ImVec4(1.0F, 0.75F, 0.32F, 1.0F)
                        : ImVec4(0.72F, 0.72F, 0.78F, 1.0F);

        ImGui::TextColored(
            statusColor,
            "%s",
            StateLabel(currentState));
        ImGui::SameLine();
        ImGui::TextDisabled(" | Shots: %d", impl.shotsFired.load());
        ImGui::SameLine();
        ImGui::TextDisabled(
            " | Pixels: %d",
            impl.matchingPixels.load());

        ImGui::SameLine(0.0F, 18.0F);
        ImGui::TextDisabled("Rays:");
        ImGui::SameLine();
        ImGui::TextColored(
            impl.hitUpState.load() ? ImVec4(0.4F, 1.0F, 0.4F, 1.0F) : ImVec4(0.4F, 0.4F, 0.4F, 1.0F),
            "[UP]");
        ImGui::SameLine();
        ImGui::TextColored(
            impl.hitLeftState.load() ? ImVec4(0.4F, 1.0F, 0.4F, 1.0F) : ImVec4(0.4F, 0.4F, 0.4F, 1.0F),
            "[LEFT]");
        ImGui::SameLine();
        ImGui::TextColored(
            impl.hitRightState.load() ? ImVec4(0.4F, 1.0F, 0.4F, 1.0F) : ImVec4(0.4F, 0.4F, 0.4F, 1.0F),
            "[RIGHT]");

        if(impl.makcu)
        {
            const auto output=impl.makcu->ClickStatus(InputOwner::testClick);
            ImGui::TextDisabled("Output backend: %s",impl.makcu->GetConfig().backend==InputBackend::medius?"Medius":"Stock MAKCU");
            if(!impl.makcu->IsMouseButtonInputAvailable())
                ImGui::TextWrapped("Connect Mouse Output and wait for physical-button monitoring before activating TestClick.");
            ImGui::TextWrapped("Click status: %s",output.message.c_str());
            ImGui::TextDisabled("Requests: %llu | Started: %llu | Confirmed: %llu",
                static_cast<unsigned long long>(output.attempts),static_cast<unsigned long long>(output.started),
                static_cast<unsigned long long>(output.completed));
            ImGui::TextDisabled("Rejected: %llu | Cancelled: %llu | Failed: %llu",static_cast<unsigned long long>(output.rejected),
                static_cast<unsigned long long>(output.cancelled),static_cast<unsigned long long>(output.failed));
        }

        custom::Separator();

        bool publishPolicy{};bool policyEnabled{};std::uint64_t policyGeneration{};MouseButtonBinding policyButton{};
        {
            std::lock_guard<std::mutex> lk(impl.settingsMutex);
            bool settingsChanged = false;
            const auto drawCategory =
                [](const char* label)
                {
                    ImGui::Spacing();
                    ImGui::TextColored(
                        ImGui::GetStyleColorVec4(
                            ImGuiCol_CheckMark),
                        "%s",
                        label);
                    custom::Separator();
                };

            settingsChanged |= custom::Checkbox("Enable TestClick", &impl.enabled);

            if (impl.enabled)
            {
            drawCategory("ACTIVATION");

            const char* buttonNames[]{
                "Left click",
                "Right click",
                "Middle click",
                "Mouse X1",
                "Mouse X2"};
            const MouseButtonBinding buttonValues[]{
                MouseButtonBinding::left,
                MouseButtonBinding::right,
                MouseButtonBinding::middle,
                MouseButtonBinding::x1,
                MouseButtonBinding::x2};
            constexpr int buttonCount =
                static_cast<int>(
                    sizeof(buttonValues) /
                    sizeof(buttonValues[0]));

            int selectedButton = 0;
            for (int i = 0; i < buttonCount; ++i)
            {
                if (buttonValues[i] == impl.holdButton)
                {
                    selectedButton = i;
                    break;
                }
            }
            if (custom::Combo(
                    "Activation button",
                    &selectedButton,
                    buttonNames,
                    buttonCount))
            {
                impl.holdButton = buttonValues[selectedButton];
                settingsChanged = true;
            }
            ImGui::TextWrapped("Hold this button to repeat left clicks while a fresh frame contains a valid target. Release to stop.");

            drawCategory("TARGET");

            const char* colorTargets[
                vanta::kHsvColorTargets.size()]{};
            for (std::size_t index = 0;
                 index < vanta::kHsvColorTargets.size();
                 ++index)
            {
                colorTargets[index] =
                    vanta::kHsvColorTargets[index].label;
            }
            settingsChanged |= custom::Combo(
                "Color target",
                &impl.hsvRangeIndex,
                colorTargets,
                static_cast<int>(
                    vanta::kHsvColorTargets.size()));

            drawCategory("DETECTION");

            const char* modeNames[]{
                "Strict (3 rays)",
                "Flexible (2+ rays)"};
            settingsChanged |= custom::Combo(
                "Detection Mode",
                &impl.hitMode,
                modeNames,
                2);

            settingsChanged |= custom::SliderInt(
                "Capture Radius", &impl.roiOffset, 10, 150, "%d px");
            impl.rayLength = std::clamp(
                impl.rayLength,
                1,
                std::max(1, impl.roiOffset - 1));

            settingsChanged |= custom::SliderInt(
                "Ray Length",
                &impl.rayLength,
                1,
                std::max(1, impl.roiOffset - 1),
                "%d px");

            settingsChanged |= custom::SliderInt(
                "Ray Thickness", &impl.rayHalfWidth, 0, 5, "+/- %d px");

            drawCategory("FIRE CONTROL");

            settingsChanged |= custom::Checkbox(
                "Enable burst limiter",
                &impl.burstLimiter);
            if(!impl.burstLimiter)ImGui::TextDisabled("Repeats after each completed click while the target stays valid.");
            if (impl.burstLimiter)
            {
                settingsChanged |= custom::SliderFloat(
                    "Shot delay", &impl.burstDelay,
                    0.00F, 0.50F, "%.3f s");
                settingsChanged |= custom::SliderFloat(
                    "Burst pause", &impl.burstPause,
                    0.00F, 1.50F, "%.3f s");
                settingsChanged |= custom::SliderInt(
                    "Burst size", &impl.burstSize, 1, 8, "%d shots");
            }
            }
            if (settingsChanged)
            {
                policyGeneration=impl.settingsRevision.fetch_add(1)+1;
                policyEnabled=impl.enabled;policyButton=impl.holdButton;publishPolicy=true;
            }
        }
        // Publish enable/disable/binding changes immediately. In-flight clicks
        // see the new generation even while the capture worker is waiting.
        if(publishPolicy && impl.makcu)
            impl.makcu->SetInputPolicy(InputOwner::testClick,policyEnabled,policyGeneration,policyButton);

        ImGui::PopStyleVar();
        custom::EndChild();
    }

    TestClickConfig TestClickController::GetConfig() const
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(
            impl.settingsMutex);
        TestClickConfig result;
        result.enabled = impl.enabled;
        result.hsvRangeIndex =
            impl.hsvRangeIndex;
        result.holdButton = impl.holdButton;
        result.roiOffset = impl.roiOffset;
        result.rayLength = impl.rayLength;
        result.rayHalfWidth =
            impl.rayHalfWidth;
        result.hitMode = impl.hitMode;
        result.burstLimiter =
            impl.burstLimiter;
        result.burstDelay = impl.burstDelay;
        result.burstPause = impl.burstPause;
        result.burstSize = impl.burstSize;
        return result;
    }

    void TestClickController::ApplyConfig(
        const TestClickConfig& config)
    {
        auto& impl = *implementation_;
        std::uint64_t generation{};MouseButtonBinding binding{};
        {
        std::lock_guard<std::mutex> lock(
            impl.settingsMutex);
        impl.enabled = config.enabled;
        impl.hsvRangeIndex = std::clamp(
            config.hsvRangeIndex,
            0,
            static_cast<int>(
                kHsvColorTargets.size()) - 1);
        impl.holdButton =
            IsMouseButtonBinding(config.holdButton)
            ? config.holdButton
            : MouseButtonBinding::x1;
        impl.roiOffset =
            std::clamp(config.roiOffset, 10, 150);
        impl.rayLength =
            std::clamp(
                config.rayLength,
                1,
                std::max(
                    1,
                    impl.roiOffset - 1));
        impl.rayHalfWidth =
            std::clamp(
                config.rayHalfWidth,
                0,
                5);
        impl.hitMode =
            std::clamp(config.hitMode, 0, 1);
        impl.burstLimiter =
            config.burstLimiter;
        impl.burstDelay =
            std::clamp(
                config.burstDelay,
                0.0F,
                0.50F);
        impl.burstPause =
            std::clamp(
                config.burstPause,
                0.0F,
                1.50F);
        impl.burstSize =
            std::clamp(config.burstSize, 1, 8);
        generation=impl.settingsRevision.fetch_add(1)+1;
        binding=impl.holdButton;
        }
        if(impl.makcu)impl.makcu->SetInputPolicy(InputOwner::testClick,config.enabled,generation,binding);
    }

    std::uint64_t
    TestClickController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }

    std::string TestClickController::DiagnosticsJson() const
    {
        const auto& impl=*implementation_;const auto config=GetConfig();
        const auto output=impl.makcu?impl.makcu->ClickStatus(InputOwner::testClick):ClickOutputStatus{};
        std::ostringstream out;out<<"{\"state\":"<<InputJsonString(StateLabel(impl.state.load()))
            <<",\"worker_alive\":"<<(impl.threadAlive?"true":"false")<<",\"enabled\":"<<(config.enabled?"true":"false")
            <<",\"configuration_generation\":"<<impl.settingsRevision.load()<<",\"activation\":"<<unsigned(config.holdButton)
            <<",\"activation_held\":"<<(impl.makcu && impl.makcu->IsMouseButtonPressed(config.holdButton)?"true":"false")
            <<",\"target_present\":"<<(impl.triggerActive?"true":"false")<<",\"ray_hits\":"<<impl.lastHits.load()
            <<",\"shots_confirmed\":"<<impl.shotsFired.load()<<",\"burst_limiter\":"<<(config.burstLimiter?"true":"false")
            <<",\"click_phase\":"<<int(output.phase)<<",\"attempts\":"<<output.attempts<<",\"started\":"<<output.started
            <<",\"completed\":"<<output.completed<<",\"rejected\":"<<output.rejected<<",\"cancelled\":"<<output.cancelled
            <<",\"failed\":"<<output.failed<<",\"click_status\":"<<InputJsonString(output.message)
            <<",\"click_status_at_local_us\":"<<output.updatedAtUs<<'}';return out.str();
    }

    bool TestClickController::RunSelfTest()
    {
        // Opt-in, compile-only on the development PC. No capture or hardware.
        ClickRepeatState repeat;
        for(int i=0;i<20;++i)
        {
            const double now=1.0+i*0.04;
            if(!repeat.Due(false,3,0.05,0.20,now))return false;
            repeat.Completed(false,now);
        }
        repeat.Reset();
        for(int i=0;i<3;++i)
        {
            const double now=1.0+i*0.06;
            if(!repeat.Due(true,3,0.05,0.20,now))return false;
            repeat.Completed(true,now);
        }
        if(repeat.Due(true,3,0.05,0.20,1.2) || !repeat.Due(true,3,0.05,0.20,1.4))return false;
        // Rejected output never consumes a burst slot or a repeat interval.
        if(!repeat.Due(true,3,0.05,0.20,1.4) || repeat.burstCount!=0)return false;
        repeat.Reset();if(!repeat.Due(true,3,0.05,0.20,1.0))return false;
        log::Info("TestClick repeat scheduling deterministic checks passed");return true;
    }
}
