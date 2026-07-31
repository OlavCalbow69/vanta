#define IMGUI_DEFINE_MATH_OPERATORS

#include "testmove_controller.hpp"

#include "capture_controller.hpp"
#include "color_targets.hpp"
#include "makcu_controller.hpp"
#include "logger.hpp"

#include "imgui.h"
#include "imgui_settings.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <opencv2/core.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>

#include <Windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr bool AxisVerticalActive(
        vanta::AxisMovementMode mode,
        std::int64_t elapsedMilliseconds,
        int activeTimeMilliseconds) noexcept
    {
        return
            mode == vanta::AxisMovementMode::standard ||
            (mode == vanta::AxisMovementMode::hybrid &&
             elapsedMilliseconds <
                 activeTimeMilliseconds);
    }

    static_assert(
        !AxisVerticalActive(
            vanta::AxisMovementMode::horizontalOnly,
            0,
            100));
    static_assert(
        AxisVerticalActive(
            vanta::AxisMovementMode::hybrid,
            99,
            100));
    static_assert(
        !AxisVerticalActive(
            vanta::AxisMovementMode::hybrid,
            100,
            100));
    static_assert(
        AxisVerticalActive(
            vanta::AxisMovementMode::standard,
            5000,
            100));

    constexpr bool ShouldIgnoreTargetBelowCrosshair(
        int aimY,
        int crosshairY,
        bool enabled) noexcept
    {
        return enabled && aimY > crosshairY;
    }

    static_assert(
        ShouldIgnoreTargetBelowCrosshair(
            101,
            100,
            true));
    static_assert(
        !ShouldIgnoreTargetBelowCrosshair(
            100,
            100,
            true));
    static_assert(
        !ShouldIgnoreTargetBelowCrosshair(
            101,
            100,
            false));

    // ─────────────────────────────────────────────────────────────────────────
    // State machine
    // ─────────────────────────────────────────────────────────────────────────
    enum class TestMoveState : int
    {
        offline,
        disabled,
        waitingForKey,
        waitingForCapture,
        waitingForMakcu,
        waitingForNewTarget,
        briefPause,
        tracking,
        locked
    };

    const char* StateLabel(TestMoveState s)
    {
        switch (s)
        {
        case TestMoveState::disabled:          return "DISABLED";
        case TestMoveState::waitingForKey:     return "WAITING FOR KEY";
        case TestMoveState::waitingForCapture: return "WAITING FOR CAPTURE";
        case TestMoveState::waitingForMakcu:   return "WAITING FOR MAKCU";
        case TestMoveState::waitingForNewTarget:
            return "NEW TARGET DELAY";
        case TestMoveState::briefPause:        return "BRIEF PAUSE";
        case TestMoveState::tracking:          return "TRACKING";
        case TestMoveState::locked:            return "LOCKED ON";
        default:                               return "OFFLINE";
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Blacklist helpers — packed BGR key identical to testclick_controller
    // ─────────────────────────────────────────────────────────────────────────
    inline std::uint32_t BgrKey(
        std::uint8_t b, std::uint8_t g, std::uint8_t r)
    {
        return static_cast<std::uint32_t>(b) |
               (static_cast<std::uint32_t>(g) << 8) |
               (static_cast<std::uint32_t>(r) << 16);
    }

    const std::unordered_set<std::uint32_t>& Blacklist()
    {
        // Same 75-entry blacklist as testclick_controller.cpp / Python BLACKLIST
        // stored as {B, G, R} to match OpenCV's channel order.
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
                s.insert(BgrKey(e[0], e[1], e[2]));
            return s;
        }();
        return set;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Rectangle merger — direct C++ translation of Python unify_rectangles()
    // Merges all overlapping / nearby bounding boxes iteratively.
    // ─────────────────────────────────────────────────────────────────────────
    std::vector<cv::Rect> UnifyRectangles(
        std::vector<cv::Rect> rects,
        int proximity)
    {
        struct Box { int x1, y1, x2, y2; };

        std::vector<Box> boxes;
        boxes.reserve(rects.size());
        for (const auto& r : rects)
            boxes.push_back({r.x, r.y, r.x + r.width, r.y + r.height});

        while (true)
        {
            bool merged = false;
            std::vector<Box> newBoxes;
            while (!boxes.empty())
            {
                Box current = boxes.front();
                boxes.erase(boxes.begin());
                bool wasMerged = false;
                for (std::size_t i = 0; i < boxes.size(); ++i)
                {
                    const Box& other = boxes[i];
                    if (current.x2 >= other.x1 - proximity &&
                        current.x1 <= other.x2 + proximity &&
                        current.y2 >= other.y1 - proximity &&
                        current.y1 <= other.y2 + proximity)
                    {
                        newBoxes.push_back({
                            std::min(current.x1, other.x1),
                            std::min(current.y1, other.y1),
                            std::max(current.x2, other.x2),
                            std::max(current.y2, other.y2)
                        });
                        boxes.erase(boxes.begin() +
                            static_cast<std::ptrdiff_t>(i));
                        merged    = true;
                        wasMerged = true;
                        break;
                    }
                }
                if (!wasMerged)
                    newBoxes.push_back(current);
            }
            boxes = std::move(newBoxes);
            if (!merged) break;
        }

        std::vector<cv::Rect> result;
        result.reserve(boxes.size());
        for (const auto& b : boxes)
            result.emplace_back(b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1);
        return result;
    }
}

namespace vanta
{
    // =========================================================================
    // Implementation
    // =========================================================================
    struct TestMoveController::Implementation
    {
        std::mutex  settingsMutex;
        bool        enabled{false};
        int         hsvRangeIndex{0};

        // Aim key: hold to activate (default = left mouse button, same as Python)
        int aimKey{VK_LBUTTON};

        // FOV: only targets within this radius from screen centre are considered.
        // Python used kill_fov = 100.
        int killFov{100};
        int targetHeightPercent{15};
        bool drawFovOutline{true};
        float fovColor[4]{
            0.68F, 0.56F, 0.91F, 1.0F};

        // Movement parameters — exact Python defaults
        MovementMethod movementMethod{
            MovementMethod::direct};
        float speed{1.125F};
        float smooth{0.90F};
        int   deadzone{2};
        float windGravity{9.0F};
        float windStrength{3.0F};
        float windMaximumStep{15.0F};
        float windSlowdownRadius{12.0F};
        AxisMovementMode axisMode{
            AxisMovementMode::standard};
        float axisHorizontalMultiplier{1.125F};
        float axisVerticalMultiplier{1.125F};
        float axisSmoothing{0.90F};
        int hybridVerticalTimeMilliseconds{300};
        bool antiBelowObjects{};
        bool shortStopEnabled{};
        ShortStopMode shortStopMode{
            ShortStopMode::slowMove};
        int shortStopChancePercent{5};
        int shortStopMinimumPauseMilliseconds{30};
        int shortStopMaximumPauseMilliseconds{90};
        float shortStopSlowMultiplierMinimum{2.0F};
        float shortStopSlowMultiplierMaximum{4.0F};
        int newTargetDelayMinimumMilliseconds{};
        int newTargetDelayMaximumMilliseconds{};

        // Rectangle merge proximity — same as Python call: proximity=10
        int mergeProximity{10};

        CaptureController* capture{nullptr};
        MakcuController*   makcu{nullptr};
        std::thread        workerThread;
        std::atomic_bool   running{false};

        std::atomic<TestMoveState> state{TestMoveState::offline};
        std::atomic<bool>          threadAlive{false};
        std::atomic<int>           movesMade{0};
        std::atomic<int>           matchingPixels{0};
        std::atomic<bool>          targetVisible{false};
        std::atomic<int>           targetDx{0};
        std::atomic<int>           targetDy{0};
        std::atomic<std::uint64_t> settingsRevision{0};

        static double NowSeconds()
        {
            using namespace std::chrono;
            return duration<double>(
                steady_clock::now().time_since_epoch()).count();
        }

        // ─────────────────────────────────────────────────────────────────────
        // Worker thread — runs independently, zero-delay synced to capture
        // ─────────────────────────────────────────────────────────────────────
        void WorkerLoop()
        {
            threadAlive.store(true);
            state.store(TestMoveState::disabled);
            vanta::log::Info(
                "TestMove worker started (Zero-Delay Condition Variable Sync)");

            std::uint64_t lastSequence = 0;
            cv::Mat       frame;
            std::uint64_t sequence    = 0;
            std::int64_t  timestamp   = 0;
            double        nextSampleTime = 0.0;
            float         windX = 0.0F;
            float         windY = 0.0F;
            float         velocityX = 0.0F;
            float         velocityY = 0.0F;
            std::mt19937  randomEngine{
                std::random_device{}()};
            std::uniform_real_distribution<float> randomUnit(
                -1.0F,
                1.0F);
            bool aimKeyWasHeld = false;
            int previousAimKey = 0;
            MovementMethod previousMovementMethod =
                MovementMethod::direct;
            AxisMovementMode previousAxisMode =
                AxisMovementMode::standard;
            std::chrono::steady_clock::time_point
                hybridWindowStarted{};
            bool trackedTargetValid = false;
            int trackedTargetX{};
            int trackedTargetY{};
            bool pendingTargetValid = false;
            int pendingTargetX{};
            int pendingTargetY{};
            std::chrono::steady_clock::time_point
                pendingTargetReadyTime{};
            std::chrono::steady_clock::time_point
                shortPauseEndTime{};
            float shortPauseSlowFactor{1.0F};

            const auto resetWind = [&]()
            {
                windX = 0.0F;
                windY = 0.0F;
                velocityX = 0.0F;
                velocityY = 0.0F;
            };
            const auto resetShortPause = [&]()
            {
                shortPauseEndTime = {};
                shortPauseSlowFactor = 1.0F;
            };
            const auto resetTargetTracking = [&]()
            {
                trackedTargetValid = false;
                pendingTargetValid = false;
                resetShortPause();
            };

            // 4x4 dilation kernel — identical to Python:
            //   cv2.dilate(mask, np.ones((4,4), np.uint8), iterations=1)
            const cv::Mat kernel =
                cv::getStructuringElement(cv::MORPH_RECT, cv::Size(4, 4));

            while (running.load(std::memory_order_acquire))
            {
                // Snapshot settings under lock
                bool  snapEnabled;
                bool  snapAntiBelowObjects,
                      snapShortStopEnabled;
                int   snapHsvRange, snapAimKey, snapKillFov,
                      snapTargetHeightPercent,
                      snapDeadzone, snapMergeProx,
                      snapHybridVerticalTimeMilliseconds,
                      snapShortStopChancePercent,
                      snapShortStopMinimumPauseMilliseconds,
                      snapShortStopMaximumPauseMilliseconds,
                      snapNewTargetDelayMinimumMilliseconds,
                      snapNewTargetDelayMaximumMilliseconds;
                MovementMethod snapMovementMethod;
                AxisMovementMode snapAxisMode;
                ShortStopMode snapShortStopMode;
                float snapSpeed, snapSmooth, snapWindGravity,
                      snapWindStrength, snapWindMaximumStep,
                      snapWindSlowdownRadius,
                      snapAxisHorizontalMultiplier,
                      snapAxisVerticalMultiplier,
                      snapAxisSmoothing,
                      snapShortStopSlowMultiplierMinimum,
                      snapShortStopSlowMultiplierMaximum;
                {
                    std::lock_guard<std::mutex> lk(settingsMutex);
                    snapEnabled    = enabled;
                    snapHsvRange   = hsvRangeIndex;
                    snapAimKey     = aimKey;
                    snapKillFov    = killFov;
                    snapTargetHeightPercent =
                        targetHeightPercent;
                    snapMovementMethod = movementMethod;
                    snapSpeed      = speed;
                    snapSmooth     = smooth;
                    snapDeadzone   = deadzone;
                    snapMergeProx  = mergeProximity;
                    snapWindGravity = windGravity;
                    snapWindStrength = windStrength;
                    snapWindMaximumStep = windMaximumStep;
                    snapWindSlowdownRadius =
                        windSlowdownRadius;
                    snapAxisMode = axisMode;
                    snapAxisHorizontalMultiplier =
                        axisHorizontalMultiplier;
                    snapAxisVerticalMultiplier =
                        axisVerticalMultiplier;
                    snapAxisSmoothing =
                        axisSmoothing;
                    snapHybridVerticalTimeMilliseconds =
                        hybridVerticalTimeMilliseconds;
                    snapAntiBelowObjects =
                        antiBelowObjects;
                    snapShortStopEnabled =
                        shortStopEnabled;
                    snapShortStopMode =
                        shortStopMode;
                    snapShortStopChancePercent =
                        shortStopChancePercent;
                    snapShortStopMinimumPauseMilliseconds =
                        shortStopMinimumPauseMilliseconds;
                    snapShortStopMaximumPauseMilliseconds =
                        shortStopMaximumPauseMilliseconds;
                    snapShortStopSlowMultiplierMinimum =
                        shortStopSlowMultiplierMinimum;
                    snapShortStopSlowMultiplierMaximum =
                        shortStopSlowMultiplierMaximum;
                    snapNewTargetDelayMinimumMilliseconds =
                        newTargetDelayMinimumMilliseconds;
                    snapNewTargetDelayMaximumMilliseconds =
                        newTargetDelayMaximumMilliseconds;
                }

                if (!snapEnabled)
                {
                    state.store(TestMoveState::disabled);
                    targetVisible.store(false);
                    resetWind();
                    aimKeyWasHeld = false;
                    resetTargetTracking();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(5));
                    continue;
                }

                // Aimbot is only active while the aim key is held
                // Python: win32api.GetAsyncKeyState(KEY_LEFT_MOUSE) < 0
                //          (<0 means high-bit set, same as & 0x8000)
                const bool aimKeyHeld =
                    (GetAsyncKeyState(snapAimKey) &
                     0x8000) != 0;
                if (!aimKeyHeld)
                {
                    state.store(TestMoveState::waitingForKey);
                    targetVisible.store(false);
                    resetWind();
                    aimKeyWasHeld = false;
                    resetTargetTracking();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                    continue;
                }
                if (!aimKeyWasHeld ||
                    previousAimKey != snapAimKey ||
                    previousMovementMethod !=
                        snapMovementMethod ||
                    previousAxisMode != snapAxisMode)
                {
                    hybridWindowStarted =
                        std::chrono::steady_clock::now();
                }
                aimKeyWasHeld = true;
                previousAimKey = snapAimKey;
                previousMovementMethod =
                    snapMovementMethod;
                previousAxisMode = snapAxisMode;

                if (makcu == nullptr || !makcu->IsConnected())
                {
                    state.store(TestMoveState::waitingForMakcu);
                    targetVisible.store(false);
                    resetWind();
                    resetTargetTracking();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(5));
                    continue;
                }

                // Block until a fresh capture frame arrives
                if (capture == nullptr)
                {
                    state.store(TestMoveState::waitingForCapture);
                    targetVisible.store(false);
                    resetWind();
                    resetTargetTracking();
                    continue;
                }
                if (!capture->WaitForCenteredFrame(
                        lastSequence,
                        frame,
                        sequence,
                        timestamp,
                        10))
                {
                    state.store(TestMoveState::waitingForCapture);
                    targetVisible.store(false);
                    resetWind();
                    continue;
                }
                lastSequence = sequence;

                if (frame.empty())
                {
                    state.store(TestMoveState::waitingForCapture);
                    resetWind();
                    resetTargetTracking();
                    continue;
                }

                // Frame centre — equivalent to Python:  cf = view_fov // 2
                const int fcx = frame.cols / 2;
                const int fcy = frame.rows / 2;
                const int effectiveKillFov =
                    std::min(
                        snapKillFov,
                        std::max(
                            0,
                            std::min(fcx, fcy) - 3));

                // ── Convert BGRA → BGR (capture produces BGRA) ──────────────
                cv::Mat bgr;
                cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);

                // ── HSV mask — same color range as the Python bot ────────────
                // Python: cv2.cvtColor(frame[:,:,:3], cv2.COLOR_BGR2HSV)
                //         cv2.inRange(hsv, cmin, cmax)
                cv::Mat hsv;
                cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

                const auto& hsvRange =
                    vanta::kHsvColorTargets[
                        static_cast<std::size_t>(
                        std::clamp(
                            snapHsvRange,
                            0,
                            static_cast<int>(
                                vanta::kHsvColorTargets.size()) - 1))];

                cv::Mat mask;
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

                // ── Diagnostic pixel count (sampled every 100 ms) ─────────
                const double nowSec = NowSeconds();
                if (nowSec >= nextSampleTime)
                {
                    int px = 0;
                    const auto& bl = Blacklist();
                    for (int row = 0; row < bgr.rows; ++row)
                    {
                        const cv::Vec3b*  colors =
                            bgr.ptr<cv::Vec3b>(row);
                        const std::uint8_t* vals =
                            mask.ptr<std::uint8_t>(row);
                        for (int col = 0; col < bgr.cols; ++col)
                        {
                            if (vals[col] == 0) continue;
                            const auto& p = colors[col];
                            if (!bl.count(BgrKey(p[0], p[1], p[2])))
                                ++px;
                        }
                    }
                    matchingPixels.store(px);
                    nextSampleTime = nowSec + 0.10;
                }

                // ── Dilate — Python: cv2.dilate(mask, np.ones((4,4)), iter=1)
                cv::Mat dilated;
                cv::dilate(mask, dilated, kernel,
                           cv::Point(-1, -1), 1);

                // ── Contours — Python: cv2.findContours(dilated, RETR_EXTERNAL,
                //              CHAIN_APPROX_NONE)
                std::vector<std::vector<cv::Point>> contours;
                cv::findContours(dilated, contours,
                                 cv::RETR_EXTERNAL,
                                 cv::CHAIN_APPROX_NONE);

                // ── Raw rects — Python: if cv2.contourArea(c) < 5: continue
                std::vector<cv::Rect> rawRects;
                rawRects.reserve(contours.size());
                for (const auto& c : contours)
                {
                    if (cv::contourArea(c) < 5.0) continue;
                    rawRects.push_back(cv::boundingRect(c));
                }

                // ── Merge rects — Python: unify_rectangles(raw_rects, proximity=10)
                std::vector<cv::Rect> mergedRects =
                    UnifyRectangles(std::move(rawRects), snapMergeProx);

                // ── Find best target ─────────────────────────────────────────
                // Aim height is measured down from the target's top edge.
                //         dist = np.hypot(hx - cf, hy - cf)
                //         if dist < self.kill_fov  →  candidate
                const auto& bl = Blacklist();
                float minDist  =
                    static_cast<float>(effectiveKillFov);
                bool  found    = false;
                int   bestDx   = 0;
                int   bestDy   = 0;

                for (const auto& r : mergedRects)
                {
                    const int hx =
                        r.x + r.width / 2;
                    const int hy =
                        r.y + static_cast<int>(
                            static_cast<float>(r.height) *
                            static_cast<float>(
                                snapTargetHeightPercent) /
                            100.0F);
                    if (ShouldIgnoreTargetBelowCrosshair(
                            hy,
                            fcy,
                            snapAntiBelowObjects))
                    {
                        continue;
                    }

                    const float dist = std::hypot(
                        static_cast<float>(hx - fcx),
                        static_cast<float>(hy - fcy));

                    if (dist >= minDist)
                        continue;

                    // Blacklist check on the aim point pixel
                    const int px = std::clamp(hx, 0, bgr.cols - 1);
                    const int py = std::clamp(hy, 0, bgr.rows - 1);
                    const auto& pixel = bgr.at<cv::Vec3b>(py, px);
                    if (bl.count(BgrKey(pixel[0], pixel[1], pixel[2])))
                        continue;

                    minDist = dist;
                    found   = true;
                    bestDx  = hx - fcx;
                    bestDy  = hy - fcy;
                }

                targetVisible.store(found);
                targetDx.store(bestDx);
                targetDy.store(bestDy);

                if (found)
                {
                    const int selectedAimX =
                        fcx + bestDx;
                    const int selectedAimY =
                        fcy + bestDy;
                    const int associationRadius =
                        std::max(
                            24,
                            snapMergeProx * 2 + 16);
                    const auto matchesTarget =
                        [&](int leftX,
                            int leftY,
                            int rightX,
                            int rightY)
                    {
                        const std::int64_t deltaX =
                            static_cast<std::int64_t>(
                                leftX) -
                            rightX;
                        const std::int64_t deltaY =
                            static_cast<std::int64_t>(
                                leftY) -
                            rightY;
                        return
                            deltaX * deltaX +
                                deltaY * deltaY <=
                            static_cast<std::int64_t>(
                                associationRadius) *
                                associationRadius;
                    };
                    const bool sameTrackedTarget =
                        trackedTargetValid &&
                        matchesTarget(
                            selectedAimX,
                            selectedAimY,
                            trackedTargetX,
                            trackedTargetY);
                    const auto targetNow =
                        std::chrono::steady_clock::now();
                    if (sameTrackedTarget)
                    {
                        trackedTargetX = selectedAimX;
                        trackedTargetY = selectedAimY;
                        pendingTargetValid = false;
                    }
                    else
                    {
                        resetShortPause();
                        const int minimumDelay =
                            std::clamp(
                                snapNewTargetDelayMinimumMilliseconds,
                                0,
                                1000);
                        const int maximumDelay =
                            std::clamp(
                                snapNewTargetDelayMaximumMilliseconds,
                                minimumDelay,
                                1000);
                        if (maximumDelay > 0)
                        {
                            const bool samePendingTarget =
                                pendingTargetValid &&
                                matchesTarget(
                                    selectedAimX,
                                    selectedAimY,
                                    pendingTargetX,
                                    pendingTargetY);
                            if (!samePendingTarget)
                            {
                                std::uniform_int_distribution<int>
                                    waitDistribution(
                                        minimumDelay,
                                        maximumDelay);
                                pendingTargetReadyTime =
                                    targetNow +
                                    std::chrono::milliseconds(
                                        waitDistribution(
                                            randomEngine));
                                pendingTargetValid = true;
                            }
                            pendingTargetX = selectedAimX;
                            pendingTargetY = selectedAimY;
                            if (targetNow <
                                pendingTargetReadyTime)
                            {
                                state.store(
                                    TestMoveState::
                                        waitingForNewTarget);
                                resetWind();
                                continue;
                            }
                        }

                        trackedTargetValid = true;
                        trackedTargetX = selectedAimX;
                        trackedTargetY = selectedAimY;
                        pendingTargetValid = false;
                    }

                    state.store(TestMoveState::locked);

                    // ── Mouse movement formula — exactly as Python: ───────────
                    // mx = int(dx * self.speed * self.smooth)
                    // my = int(dy * self.speed * self.smooth)
                    // if abs(mx) > self.deadzone or abs(my) > self.deadzone:
                    //     self.mouse.move(mx, my)
                    int moveX = static_cast<int>(
                        static_cast<float>(bestDx) *
                        snapSpeed * snapSmooth);
                    int moveY = static_cast<int>(
                        static_cast<float>(bestDy) *
                        snapSpeed * snapSmooth);

                    if (snapMovementMethod ==
                        MovementMethod::windMouse)
                    {
                        const float distance =
                            std::hypot(
                                static_cast<float>(bestDx),
                                static_cast<float>(bestDy));
                        if (distance <=
                            static_cast<float>(snapDeadzone))
                        {
                            resetWind();
                            continue;
                        }

                        constexpr float rootThree =
                            1.73205080757F;
                        constexpr float rootFive =
                            2.23606797750F;
                        const float limitedWind =
                            std::min(
                                snapWindStrength,
                                distance);
                        if (distance >=
                            snapWindSlowdownRadius)
                        {
                            windX =
                                windX / rootThree +
                                randomUnit(randomEngine) *
                                    limitedWind / rootFive;
                            windY =
                                windY / rootThree +
                                randomUnit(randomEngine) *
                                    limitedWind / rootFive;
                        }
                        else
                        {
                            windX /= rootThree;
                            windY /= rootThree;
                        }

                        velocityX +=
                            windX +
                            snapWindGravity *
                                static_cast<float>(bestDx) /
                                distance;
                        velocityY +=
                            windY +
                            snapWindGravity *
                                static_cast<float>(bestDy) /
                                distance;

                        float maximumVelocity =
                            snapWindMaximumStep;
                        if (distance <
                            snapWindSlowdownRadius)
                        {
                            maximumVelocity =
                                std::max(
                                    1.0F,
                                    snapWindMaximumStep *
                                        distance /
                                        snapWindSlowdownRadius);
                        }
                        const float velocityLength =
                            std::hypot(
                                velocityX,
                                velocityY);
                        if (velocityLength >
                            maximumVelocity)
                        {
                            const float randomScale =
                                0.70F +
                                0.15F *
                                    (randomUnit(randomEngine) +
                                     1.0F);
                            velocityX =
                                velocityX /
                                velocityLength *
                                maximumVelocity *
                                randomScale;
                            velocityY =
                                velocityY /
                                velocityLength *
                                maximumVelocity *
                                randomScale;
                        }

                        moveX = static_cast<int>(
                            std::lround(velocityX));
                        moveY = static_cast<int>(
                            std::lround(velocityY));
                        if (moveX == 0 &&
                            std::abs(bestDx) > snapDeadzone)
                        {
                            moveX =
                                bestDx > 0 ? 1 : -1;
                        }
                        if (moveY == 0 &&
                            std::abs(bestDy) > snapDeadzone)
                        {
                            moveY =
                                bestDy > 0 ? 1 : -1;
                        }
                    }
                    else if (snapMovementMethod ==
                        MovementMethod::axisControl)
                    {
                        resetWind();
                        moveX = static_cast<int>(
                            std::lround(
                                static_cast<float>(bestDx) *
                                snapAxisHorizontalMultiplier *
                                snapAxisSmoothing));
                        const bool verticalActive =
                            AxisVerticalActive(
                                snapAxisMode,
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() -
                                    hybridWindowStarted).count(),
                                snapHybridVerticalTimeMilliseconds);
                        moveY = verticalActive
                            ? static_cast<int>(
                                std::lround(
                                    static_cast<float>(bestDy) *
                                    snapAxisVerticalMultiplier *
                                    snapAxisSmoothing))
                            : 0;
                    }
                    else
                    {
                        resetWind();
                    }

                    const auto pauseNow =
                        std::chrono::steady_clock::now();
                    if (!snapShortStopEnabled)
                    {
                        resetShortPause();
                    }
                    else
                    {
                        bool pauseActive =
                            pauseNow < shortPauseEndTime;
                        if (!pauseActive)
                        {
                            resetShortPause();
                            std::uniform_int_distribution<int>
                                chanceDistribution(1, 100);
                            const int chance =
                                std::clamp(
                                    snapShortStopChancePercent,
                                    1,
                                    20);
                            if (chanceDistribution(
                                    randomEngine) <= chance)
                            {
                                const int minimumPause =
                                    std::clamp(
                                        snapShortStopMinimumPauseMilliseconds,
                                        10,
                                        200);
                                const int maximumPause =
                                    std::clamp(
                                        snapShortStopMaximumPauseMilliseconds,
                                        minimumPause,
                                        500);
                                std::uniform_int_distribution<int>
                                    pauseDistribution(
                                        minimumPause,
                                        maximumPause);
                                shortPauseEndTime =
                                    pauseNow +
                                    std::chrono::milliseconds(
                                        pauseDistribution(
                                            randomEngine));
                                if (snapShortStopMode ==
                                    ShortStopMode::slowMove)
                                {
                                    const float minimumMultiplier =
                                        std::clamp(
                                            snapShortStopSlowMultiplierMinimum,
                                            1.0F,
                                            10.0F);
                                    const float maximumMultiplier =
                                        std::clamp(
                                            snapShortStopSlowMultiplierMaximum,
                                            minimumMultiplier,
                                            10.0F);
                                    std::uniform_real_distribution<float>
                                        multiplierDistribution(
                                            minimumMultiplier,
                                            maximumMultiplier);
                                    shortPauseSlowFactor =
                                        multiplierDistribution(
                                            randomEngine);
                                }
                                pauseActive = true;
                            }
                        }

                        if (pauseActive)
                        {
                            state.store(
                                TestMoveState::briefPause);
                            if (snapShortStopMode ==
                                ShortStopMode::fullStop)
                            {
                                moveX = 0;
                                moveY = 0;
                            }
                            else
                            {
                                const float factor =
                                    std::max(
                                        1.0F,
                                        shortPauseSlowFactor);
                                moveX = static_cast<int>(
                                    std::lround(
                                        static_cast<float>(moveX) /
                                        factor));
                                moveY = static_cast<int>(
                                    std::lround(
                                        static_cast<float>(moveY) /
                                        factor));
                            }
                        }
                    }

                    if (std::abs(moveX) > snapDeadzone ||
                        std::abs(moveY) > snapDeadzone)
                    {
                        if (makcu->TryMove(moveX, moveY))
                        {
                            movesMade.fetch_add(
                                1, std::memory_order_relaxed);
                            trackedTargetX -= moveX;
                            trackedTargetY -= moveY;
                        }
                        else
                        {
                            resetWind();
                        }
                    }
                }
                else
                {
                    state.store(TestMoveState::tracking);
                    resetWind();
                    resetTargetTracking();
                }
            }

            threadAlive.store(false);
            targetVisible.store(false);
            state.store(TestMoveState::offline);
            vanta::log::Info("TestMove worker stopped");
        }

        void Start()
        {
            if (running.load()) return;
            running.store(true, std::memory_order_release);
            workerThread =
                std::thread(&Implementation::WorkerLoop, this);
        }

        void Stop()
        {
            running.store(false, std::memory_order_release);
            if (workerThread.joinable())
                workerThread.join();
            state.store(TestMoveState::offline);
        }
    };

    // =========================================================================
    // Public interface
    // =========================================================================
    TestMoveController::TestMoveController()
        : implementation_(std::make_unique<Implementation>())
    {
    }

    TestMoveController::~TestMoveController()
    {
        Shutdown();
    }

    void TestMoveController::Initialize(
        CaptureController* capture,
        MakcuController*   makcu)
    {
        auto& impl = *implementation_;
        impl.capture = capture;
        impl.makcu   = makcu;
        impl.Start();
        vanta::log::Info("TestMove controller initialized");
    }

    void TestMoveController::Shutdown()
    {
        implementation_->Stop();
    }

    void TestMoveController::Tick()
    {
        // Nothing needed on the main thread for now.
    }

    void TestMoveController::RenderPanel()
    {
        auto& impl = *implementation_;

        const float bodyHeight = std::max(
            190.0F,
            ImGui::GetContentRegionAvail().y - 40.0F);

        custom::Child(
            ICON_AIMING_LINE "  TestMove##testmove-panel",
            ImVec2(0.0F, bodyHeight),
            true);

        ImGui::PushStyleVar(
            ImGuiStyleVar_ItemSpacing,
            ImVec2(12.0F, 12.0F));

        // ── Status bar ───────────────────────────────────────────────────────
        const TestMoveState currentState = impl.state.load();

        const ImVec4 statusColor =
            currentState == TestMoveState::locked
                ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)   // red: aiming
                : currentState == TestMoveState::tracking
                    ? ImVec4(0.45F, 0.96F, 0.65F, 1.0F)  // green: searching
                    : currentState == TestMoveState::waitingForKey  ||
                      currentState == TestMoveState::waitingForCapture ||
                      currentState == TestMoveState::waitingForMakcu ||
                      currentState == TestMoveState::waitingForNewTarget ||
                      currentState == TestMoveState::briefPause
                        ? ImVec4(1.0F, 0.75F, 0.32F, 1.0F)  // amber: standby
                        : ImVec4(0.72F, 0.72F, 0.78F, 1.0F); // grey: offline

        ImGui::TextColored(statusColor, "%s", StateLabel(currentState));
        ImGui::SameLine();
        ImGui::TextDisabled(
            " | Moves %d  |  Target %d, %d",
            impl.movesMade.load(),
            impl.targetDx.load(),
            impl.targetDy.load());

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

            custom::Checkbox("Enable TestMove", &impl.enabled);

            if (impl.enabled)
            {
                drawCategory("ACTIVATION");

                // Aim key selection
                const char* keyNames[]{
                    "Left Mouse Button",
                    "Right Mouse Button",
                    "Mouse4 (XButton1)",
                    "Mouse5 (XButton2)",
                    "Left Alt",
                    "Left Shift"};
                const int keyValues[]{
                    VK_LBUTTON,  VK_RBUTTON,
                    VK_XBUTTON1, VK_XBUTTON2,
                    VK_LMENU,    VK_LSHIFT};
                constexpr int keyCount =
                    static_cast<int>(
                        sizeof(keyValues) / sizeof(keyValues[0]));

                int selectedKey = 0;
                for (int i = 0; i < keyCount; ++i)
                {
                    if (keyValues[i] == impl.aimKey)
                    {
                        selectedKey = i;
                        break;
                    }
                }
                if (custom::Combo(
                        "Aim key (hold)",
                        &selectedKey,
                        keyNames,
                        keyCount))
                {
                    impl.aimKey = keyValues[selectedKey];
                    settingsChanged = true;
                }

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

                settingsChanged |= custom::SliderInt(
                    "Kill FOV",
                    &impl.killFov,
                    20, 350, "%d px");
                settingsChanged |= custom::SliderInt(
                    "Target height",
                    &impl.targetHeightPercent,
                    0, 100, "%d%% from top");
                settingsChanged |= custom::Checkbox(
                    "Draw Kill FOV outline",
                    &impl.drawFovOutline);
                if (impl.drawFovOutline)
                {
                    settingsChanged |= custom::ColorEdit4(
                        "Kill FOV color",
                        impl.fovColor,
                        ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_AlphaBar |
                            ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_AlphaPreview);
                }
                drawCategory("MOVEMENT");

                const char* movementMethods[]{
                    "Direct",
                    "WindMouse",
                    "Axis control"};
                int movementMethod =
                    static_cast<int>(
                        impl.movementMethod);
                if (custom::Combo(
                        "Movement method",
                        &movementMethod,
                        movementMethods,
                        IM_ARRAYSIZE(movementMethods)))
                {
                    impl.movementMethod =
                        static_cast<MovementMethod>(
                            std::clamp(
                                movementMethod,
                                0,
                                2));
                    settingsChanged = true;
                }

                if (impl.movementMethod ==
                    MovementMethod::direct)
                {
                    settingsChanged |= custom::SliderFloat(
                        "Speed multiplier",
                        &impl.speed,
                        0.10F, 3.0F, "%.3f");
                    settingsChanged |= custom::SliderFloat(
                        "Smoothing",
                        &impl.smooth,
                        0.10F, 1.0F, "%.3f");
                }
                else if (impl.movementMethod ==
                    MovementMethod::windMouse)
                {
                    settingsChanged |= custom::SliderFloat(
                        "Wind gravity",
                        &impl.windGravity,
                        1.0F, 20.0F, "%.2f");
                    settingsChanged |= custom::SliderFloat(
                        "Wind strength",
                        &impl.windStrength,
                        0.0F, 10.0F, "%.2f");
                    settingsChanged |= custom::SliderFloat(
                        "Maximum step",
                        &impl.windMaximumStep,
                        1.0F, 30.0F, "%.2f px");
                    settingsChanged |= custom::SliderFloat(
                        "Slowdown radius",
                        &impl.windSlowdownRadius,
                        1.0F, 50.0F, "%.2f px");
                }
                else
                {
                    settingsChanged |= custom::SliderFloat(
                        "Horizontal multiplier",
                        &impl.axisHorizontalMultiplier,
                        0.10F, 3.0F, "%.3f");
                    settingsChanged |= custom::SliderFloat(
                        "Axis smoothing",
                        &impl.axisSmoothing,
                        0.10F, 1.0F, "%.3f");

                    const char* axisModes[]{
                        "X + Y",
                        "Horizontal only",
                        "Hybrid"};
                    int axisMode =
                        impl.axisMode ==
                            AxisMovementMode::standard
                        ? 0
                        : impl.axisMode ==
                                AxisMovementMode::horizontalOnly
                            ? 1
                            : 2;
                    if (custom::Combo(
                            "Axis mode",
                            &axisMode,
                            axisModes,
                            IM_ARRAYSIZE(axisModes)))
                    {
                        impl.axisMode =
                            axisMode == 0
                            ? AxisMovementMode::standard
                            : axisMode == 1
                                ? AxisMovementMode::horizontalOnly
                                : AxisMovementMode::hybrid;
                        settingsChanged = true;
                    }

                    if (impl.axisMode !=
                        AxisMovementMode::horizontalOnly)
                    {
                        settingsChanged |=
                            custom::SliderFloat(
                                "Vertical multiplier",
                                &impl.axisVerticalMultiplier,
                                0.10F, 3.0F, "%.3f");
                        if (impl.axisMode ==
                            AxisMovementMode::hybrid)
                        {
                            settingsChanged |=
                                custom::SliderInt(
                                    "Vertical active time",
                                    &impl.hybridVerticalTimeMilliseconds,
                                    100, 1000, "%d ms");
                        }
                    }
                }
                settingsChanged |= custom::SliderInt(
                    "Deadzone",
                    &impl.deadzone,
                    0, 20, "%d px");

                drawCategory(
                    ICON_CODE_LINE "  EXPERIMENTAL");

                settingsChanged |= custom::Checkbox(
                    "Anti Below Objects",
                    &impl.antiBelowObjects);
                settingsChanged |= custom::SliderInt(
                    "Merge proximity",
                    &impl.mergeProximity,
                    0, 50, "%d px");

                settingsChanged |= custom::Checkbox(
                    "Enable short stop",
                    &impl.shortStopEnabled);
                if (impl.shortStopEnabled)
                {
                    const char* shortStopModes[]{
                        "Full Stop",
                        "Slow Move"};
                    int shortStopMode =
                        static_cast<int>(
                            impl.shortStopMode);
                    if (custom::Combo(
                            "Short stop mode",
                            &shortStopMode,
                            shortStopModes,
                            IM_ARRAYSIZE(shortStopModes)))
                    {
                        impl.shortStopMode =
                            shortStopMode == 0
                            ? ShortStopMode::fullStop
                            : ShortStopMode::slowMove;
                        settingsChanged = true;
                    }
                    settingsChanged |= custom::SliderInt(
                        "Pause chance",
                        &impl.shortStopChancePercent,
                        1, 20, "%d%%");
                    settingsChanged |= custom::SliderInt(
                        "Pause minimum",
                        &impl.shortStopMinimumPauseMilliseconds,
                        10, 200, "%d ms");
                    impl.shortStopMaximumPauseMilliseconds =
                        std::max(
                            impl.shortStopMaximumPauseMilliseconds,
                            impl.shortStopMinimumPauseMilliseconds);
                    settingsChanged |= custom::SliderInt(
                        "Pause maximum",
                        &impl.shortStopMaximumPauseMilliseconds,
                        impl.shortStopMinimumPauseMilliseconds,
                        500,
                        "%d ms");
                    if (impl.shortStopMode ==
                        ShortStopMode::slowMove)
                    {
                        settingsChanged |= custom::SliderFloat(
                            "Slow multiplier minimum",
                            &impl.shortStopSlowMultiplierMinimum,
                            1.0F, 10.0F, "%.2fx");
                        impl.shortStopSlowMultiplierMaximum =
                            std::max(
                                impl.shortStopSlowMultiplierMaximum,
                                impl.shortStopSlowMultiplierMinimum);
                        settingsChanged |= custom::SliderFloat(
                            "Slow multiplier maximum",
                            &impl.shortStopSlowMultiplierMaximum,
                            impl.shortStopSlowMultiplierMinimum,
                            10.0F,
                            "%.2fx");
                    }
                }

                settingsChanged |= custom::SliderInt(
                    "New target delay minimum",
                    &impl.newTargetDelayMinimumMilliseconds,
                    0, 1000, "%d ms");
                impl.newTargetDelayMaximumMilliseconds =
                    std::max(
                        impl.newTargetDelayMaximumMilliseconds,
                        impl.newTargetDelayMinimumMilliseconds);
                settingsChanged |= custom::SliderInt(
                    "New target delay maximum",
                    &impl.newTargetDelayMaximumMilliseconds,
                    impl.newTargetDelayMinimumMilliseconds,
                    1000,
                    "%d ms");
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

    TestMoveConfig TestMoveController::GetConfig() const
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(
            impl.settingsMutex);
        TestMoveConfig result;
        result.enabled = impl.enabled;
        result.hsvRangeIndex = impl.hsvRangeIndex;
        result.aimKey = impl.aimKey;
        result.killFov = impl.killFov;
        result.targetHeightPercent =
            impl.targetHeightPercent;
        result.drawFovOutline =
            impl.drawFovOutline;
        result.fovColor = {
            impl.fovColor[0],
            impl.fovColor[1],
            impl.fovColor[2],
            impl.fovColor[3]};
        result.movementMethod =
            impl.movementMethod;
        result.speed = impl.speed;
        result.smooth = impl.smooth;
        result.deadzone = impl.deadzone;
        result.mergeProximity =
            impl.mergeProximity;
        result.windGravity = impl.windGravity;
        result.windStrength = impl.windStrength;
        result.windMaximumStep =
            impl.windMaximumStep;
        result.windSlowdownRadius =
            impl.windSlowdownRadius;
        result.axisMode = impl.axisMode;
        result.axisHorizontalMultiplier =
            impl.axisHorizontalMultiplier;
        result.axisVerticalMultiplier =
            impl.axisVerticalMultiplier;
        result.axisSmoothing =
            impl.axisSmoothing;
        result.hybridVerticalTimeMilliseconds =
            impl.hybridVerticalTimeMilliseconds;
        result.antiBelowObjects =
            impl.antiBelowObjects;
        result.shortStopEnabled =
            impl.shortStopEnabled;
        result.shortStopMode =
            impl.shortStopMode;
        result.shortStopChancePercent =
            impl.shortStopChancePercent;
        result.shortStopMinimumPauseMilliseconds =
            impl.shortStopMinimumPauseMilliseconds;
        result.shortStopMaximumPauseMilliseconds =
            impl.shortStopMaximumPauseMilliseconds;
        result.shortStopSlowMultiplierMinimum =
            impl.shortStopSlowMultiplierMinimum;
        result.shortStopSlowMultiplierMaximum =
            impl.shortStopSlowMultiplierMaximum;
        result.newTargetDelayMinimumMilliseconds =
            impl.newTargetDelayMinimumMilliseconds;
        result.newTargetDelayMaximumMilliseconds =
            impl.newTargetDelayMaximumMilliseconds;
        return result;
    }

    void TestMoveController::ApplyConfig(
        const TestMoveConfig& config)
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
        impl.aimKey =
            config.aimKey != 0
            ? config.aimKey
            : VK_LBUTTON;
        impl.killFov =
            std::clamp(config.killFov, 20, 350);
        impl.targetHeightPercent =
            std::clamp(
                config.targetHeightPercent,
                0,
                100);
        impl.drawFovOutline =
            config.drawFovOutline;
        impl.fovColor[0] = std::clamp(
            config.fovColor.red, 0.0F, 1.0F);
        impl.fovColor[1] = std::clamp(
            config.fovColor.green, 0.0F, 1.0F);
        impl.fovColor[2] = std::clamp(
            config.fovColor.blue, 0.0F, 1.0F);
        impl.fovColor[3] = std::clamp(
            config.fovColor.alpha, 0.0F, 1.0F);
        switch (config.movementMethod)
        {
        case MovementMethod::windMouse:
            impl.movementMethod =
                MovementMethod::windMouse;
            break;
        case MovementMethod::axisControl:
            impl.movementMethod =
                MovementMethod::axisControl;
            break;
        default:
            impl.movementMethod =
                MovementMethod::direct;
            break;
        }
        impl.speed =
            std::clamp(config.speed, 0.10F, 3.0F);
        impl.smooth =
            std::clamp(config.smooth, 0.10F, 1.0F);
        impl.deadzone =
            std::clamp(config.deadzone, 0, 20);
        impl.mergeProximity =
            std::clamp(
                config.mergeProximity,
                0,
                50);
        impl.windGravity =
            std::clamp(
                config.windGravity,
                1.0F,
                20.0F);
        impl.windStrength =
            std::clamp(
                config.windStrength,
                0.0F,
                10.0F);
        impl.windMaximumStep =
            std::clamp(
                config.windMaximumStep,
                1.0F,
                30.0F);
        impl.windSlowdownRadius =
            std::clamp(
                config.windSlowdownRadius,
                1.0F,
                50.0F);
        switch (config.axisMode)
        {
        case AxisMovementMode::horizontalOnly:
            impl.axisMode =
                AxisMovementMode::horizontalOnly;
            break;
        case AxisMovementMode::hybrid:
            impl.axisMode =
                AxisMovementMode::hybrid;
            break;
        default:
            impl.axisMode =
                AxisMovementMode::standard;
            break;
        }
        impl.axisHorizontalMultiplier =
            std::clamp(
                config.axisHorizontalMultiplier,
                0.10F,
                3.0F);
        impl.axisVerticalMultiplier =
            std::clamp(
                config.axisVerticalMultiplier,
                0.10F,
                3.0F);
        impl.axisSmoothing =
            std::clamp(
                config.axisSmoothing,
                0.10F,
                1.0F);
        impl.hybridVerticalTimeMilliseconds =
            std::clamp(
                config.hybridVerticalTimeMilliseconds,
                100,
                1000);
        impl.antiBelowObjects =
            config.antiBelowObjects;
        impl.shortStopEnabled =
            config.shortStopEnabled;
        impl.shortStopMode =
            config.shortStopMode ==
                ShortStopMode::fullStop
            ? ShortStopMode::fullStop
            : ShortStopMode::slowMove;
        impl.shortStopChancePercent =
            std::clamp(
                config.shortStopChancePercent,
                1,
                20);
        impl.shortStopMinimumPauseMilliseconds =
            std::clamp(
                config.shortStopMinimumPauseMilliseconds,
                10,
                200);
        impl.shortStopMaximumPauseMilliseconds =
            std::clamp(
                config.shortStopMaximumPauseMilliseconds,
                impl.shortStopMinimumPauseMilliseconds,
                500);
        impl.shortStopSlowMultiplierMinimum =
            std::clamp(
                config.shortStopSlowMultiplierMinimum,
                1.0F,
                10.0F);
        impl.shortStopSlowMultiplierMaximum =
            std::clamp(
                config.shortStopSlowMultiplierMaximum,
                impl.shortStopSlowMultiplierMinimum,
                10.0F);
        impl.newTargetDelayMinimumMilliseconds =
            std::clamp(
                config.newTargetDelayMinimumMilliseconds,
                0,
                1000);
        impl.newTargetDelayMaximumMilliseconds =
            std::clamp(
                config.newTargetDelayMaximumMilliseconds,
                impl.newTargetDelayMinimumMilliseconds,
                1000);
        impl.settingsRevision.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    std::uint64_t
    TestMoveController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }

    TestMoveFovOutline
    TestMoveController::GetFovOutline() const
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(
            impl.settingsMutex);
        TestMoveFovOutline result;
        result.visible =
            impl.enabled &&
            impl.drawFovOutline;
        result.radius = impl.killFov;
        result.color = {
            impl.fovColor[0],
            impl.fovColor[1],
            impl.fovColor[2],
            impl.fovColor[3]};
        return result;
    }
}
