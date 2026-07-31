#define IMGUI_DEFINE_MATH_OPERATORS

#include "testclick_controller.hpp"

#include "capture_controller.hpp"
#include "color_targets.hpp"
#include "makcu_controller.hpp"
#include "logger.hpp"

#include "imgui.h"
#include "imgui_settings.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <Windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace
{
    enum class TestClickState : int
    {
        offline,
        disabled,
        waitingForKey,
        movementPaused,
        waitingForCapture,
        waitingForMakcu,
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
            return "WAITING FOR KEY";
        case TestClickState::movementPaused:
            return "MOVEMENT PAUSED";
        case TestClickState::waitingForCapture:
            return "WAITING FOR CAPTURE";
        case TestClickState::waitingForMakcu:
            return "WAITING FOR MAKCU";
        case TestClickState::watching:
            return "WATCHING";
        case TestClickState::firing:
            return "FIRING";
        default:
            return "OFFLINE";
        }
    }

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
        int requiredHits)
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
                    if (!bl.count(BgrKey(px[0], px[1], px[2])))
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
                    if (!bl.count(BgrKey(px[0], px[1], px[2])))
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
                    if (!bl.count(BgrKey(px[0], px[1], px[2])))
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
        int        triggerKey{VK_XBUTTON1};
        int        roiOffset{50};            // half-side of capture square (px)
        int        rayLength{49};            // max step length along each ray (px)
        int        rayHalfWidth{1};          // thickness tolerance (+/- px)
        int        hitMode{0};               // 0 = Strict (3/3 All Rays), 1 = Flexible (2/3 Rays)
        bool       burstLimiter{false};       // false = click every matching frame
        float      burstDelay{0.05F};        // low delay default
        float      burstPause{0.20F};
        int        burstSize{3};
        bool       respectMovement{true};

        CaptureController* capture{nullptr};
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

        int    burstCount{0};
        double lastShotTime{0.0};

        static double NowSeconds()
        {
            using namespace std::chrono;
            return duration<double>(
                steady_clock::now().time_since_epoch()).count();
        }

        static bool IsMoving()
        {
            return
                (GetAsyncKeyState(0x57) & 0x8000) ||
                (GetAsyncKeyState(0x41) & 0x8000) ||
                (GetAsyncKeyState(0x53) & 0x8000) ||
                (GetAsyncKeyState(0x44) & 0x8000);
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

            while (running.load(std::memory_order_acquire))
            {
                bool  snapEnabled, snapRespectMovement, snapBurstLimiter;
                int   snapHsvRange, snapKey, snapRoi, snapRayLength, snapRayHalf, snapHitMode, snapBurstSize;
                float snapBurstDelay, snapBurstPause;
                {
                    std::lock_guard<std::mutex> lk(settingsMutex);
                    snapEnabled          = enabled;
                    snapHsvRange         = hsvRangeIndex;
                    snapKey              = triggerKey;
                    snapRoi              = roiOffset;
                    snapRayLength        = rayLength;
                    snapRayHalf          = rayHalfWidth;
                    snapHitMode          = hitMode;
                    snapBurstLimiter     = burstLimiter;
                    snapBurstSize        = burstSize;
                    snapBurstDelay       = burstDelay;
                    snapBurstPause       = burstPause;
                    snapRespectMovement  = respectMovement;
                }

                if (!snapEnabled)
                {
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

                if ((GetAsyncKeyState(snapKey) & 0x8000) == 0)
                {
                    state.store(TestClickState::waitingForKey);
                    triggerActive.store(false);
                    lastHits.store(0);
                    matchingPixels.store(0);
                    hitUpState.store(false);
                    hitLeftState.store(false);
                    hitRightState.store(false);
                    burstCount   = 0;
                    lastShotTime = 0.0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                if (snapRespectMovement && IsMoving())
                {
                    state.store(TestClickState::movementPaused);
                    triggerActive.store(false);
                    lastHits.store(0);
                    matchingPixels.store(0);
                    hitUpState.store(false);
                    hitLeftState.store(false);
                    hitRightState.store(false);
                    burstCount = 0;
                    lastShotTime = 0.0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                if (makcu == nullptr || !makcu->IsConnected())
                {
                    state.store(TestClickState::waitingForMakcu);
                    triggerActive.store(false);
                    lastHits.store(0);
                    matchingPixels.store(0);
                    hitUpState.store(false);
                    hitLeftState.store(false);
                    hitRightState.store(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                // ZERO-DELAY FRAME SYNC: Blocks on condition_variable until DXGI captures a new frame!
                if (capture == nullptr || !capture->WaitForCenteredFrame(lastSequence, frame, sequence, timestamp, 10))
                {
                    state.store(TestClickState::waitingForCapture);
                    triggerActive.store(false);
                    continue;
                }
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
                const auto& hsvRange =
                    vanta::kHsvColorTargets[
                        static_cast<std::size_t>(
                        std::clamp(
                            snapHsvRange,
                            0,
                            static_cast<int>(
                                vanta::kHsvColorTargets.size()) - 1))];
                cv::inRange(
                    hsv,
                    cv::Scalar(
                        hsvRange.lower[0],
                        hsvRange.lower[1],
                        hsvRange.lower[2]),
                    cv::Scalar(
                        hsvRange.upper[0],
                        hsvRange.upper[1],
                        hsvRange.upper[2]),
                    mask);

                const double diagnosticTime = NowSeconds();
                if (diagnosticTime >= nextPixelSampleTime)
                {
                    matchingPixels.store(
                        RemoveBlacklistedAndCount(
                            bgrRoi,
                            mask));
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
                    requiredHits);

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
                    bool shouldClick = !snapBurstLimiter;
                    if (snapBurstLimiter &&
                        burstCount < snapBurstSize)
                    {
                        if (now - lastShotTime > static_cast<double>(snapBurstDelay))
                        {
                            shouldClick = true;
                        }
                    }
                    else if (snapBurstLimiter &&
                             now - lastShotTime >
                                 static_cast<double>(
                                     snapBurstPause))
                    {
                        burstCount = 0;
                    }

                    if (shouldClick)
                    {
                        const bool clicked =
                            makcu != nullptr &&
                            makcu->TryClick();
                        lastShotTime = now;
                        if (clicked)
                        {
                            if (snapBurstLimiter)
                            {
                                ++burstCount;
                            }
                            shotsFired.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                        else
                        {
                            state.store(
                                TestClickState::waitingForMakcu);
                        }
                    }
                }
                else
                {
                    burstCount = 0;
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
            workerThread = std::thread(&Implementation::WorkerLoop, this);
        }

        void Stop()
        {
            running.store(false, std::memory_order_release);
            if (workerThread.joinable())
            {
                workerThread.join();
            }
            state.store(TestClickState::offline);
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
                            currentState == TestClickState::movementPaused ||
                            currentState == TestClickState::waitingForCapture ||
                            currentState == TestClickState::waitingForMakcu
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

        custom::Separator();

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

            custom::Checkbox("Enable TestClick", &impl.enabled);

            if (impl.enabled)
            {
            drawCategory("ACTIVATION");

            const char* keyNames[]{
                "Mouse4 (XButton1)", "Mouse5 (XButton2)",
                "Right Mouse",       "Caps Lock",
                "Left Alt",          "Left Shift"};
            const int keyValues[]{
                VK_XBUTTON1, VK_XBUTTON2,
                VK_RBUTTON,  VK_CAPITAL,
                VK_LMENU,    VK_LSHIFT};
            constexpr int keyCount =
                static_cast<int>(sizeof(keyValues) / sizeof(keyValues[0]));

            int selectedKey = 0;
            for (int i = 0; i < keyCount; ++i)
            {
                if (keyValues[i] == impl.triggerKey)
                {
                    selectedKey = i;
                    break;
                }
            }
            if (custom::Combo("Hold key", &selectedKey, keyNames, keyCount))
            {
                impl.triggerKey = keyValues[selectedKey];
                settingsChanged = true;
            }
            settingsChanged |= custom::Checkbox(
                "Pause while moving (WASD)",
                &impl.respectMovement);

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
                impl.settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }

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
        result.triggerKey = impl.triggerKey;
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
        result.respectMovement =
            impl.respectMovement;
        return result;
    }

    void TestClickController::ApplyConfig(
        const TestClickConfig& config)
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(
            impl.settingsMutex);
        impl.enabled = config.enabled;
        impl.hsvRangeIndex = std::clamp(
            config.hsvRangeIndex,
            0,
            static_cast<int>(
                kHsvColorTargets.size()) - 1);
        impl.triggerKey =
            config.triggerKey != 0
            ? config.triggerKey
            : VK_XBUTTON1;
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
        impl.respectMovement =
            config.respectMovement;
        impl.settingsRevision.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    std::uint64_t
    TestClickController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }
}
