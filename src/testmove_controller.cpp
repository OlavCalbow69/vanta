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
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
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

        // Movement parameters — exact Python defaults
        float speed{1.125F};
        float smooth{0.90F};
        int   deadzone{2};

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

            // 4x4 dilation kernel — identical to Python:
            //   cv2.dilate(mask, np.ones((4,4), np.uint8), iterations=1)
            const cv::Mat kernel =
                cv::getStructuringElement(cv::MORPH_RECT, cv::Size(4, 4));

            while (running.load(std::memory_order_acquire))
            {
                // Snapshot settings under lock
                bool  snapEnabled;
                int   snapHsvRange, snapAimKey, snapKillFov,
                      snapDeadzone, snapMergeProx;
                float snapSpeed, snapSmooth;
                {
                    std::lock_guard<std::mutex> lk(settingsMutex);
                    snapEnabled    = enabled;
                    snapHsvRange   = hsvRangeIndex;
                    snapAimKey     = aimKey;
                    snapKillFov    = killFov;
                    snapSpeed      = speed;
                    snapSmooth     = smooth;
                    snapDeadzone   = deadzone;
                    snapMergeProx  = mergeProximity;
                }

                if (!snapEnabled)
                {
                    state.store(TestMoveState::disabled);
                    targetVisible.store(false);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(5));
                    continue;
                }

                // Aimbot is only active while the aim key is held
                // Python: win32api.GetAsyncKeyState(KEY_LEFT_MOUSE) < 0
                //          (<0 means high-bit set, same as & 0x8000)
                if ((GetAsyncKeyState(snapAimKey) & 0x8000) == 0)
                {
                    state.store(TestMoveState::waitingForKey);
                    targetVisible.store(false);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                    continue;
                }

                if (makcu == nullptr || !makcu->IsConnected())
                {
                    state.store(TestMoveState::waitingForMakcu);
                    targetVisible.store(false);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(5));
                    continue;
                }

                // Block until a fresh capture frame arrives
                if (capture == nullptr ||
                    !capture->WaitForCenteredFrame(
                        lastSequence, frame, sequence, timestamp, 10))
                {
                    state.store(TestMoveState::waitingForCapture);
                    targetVisible.store(false);
                    continue;
                }
                lastSequence = sequence;

                if (frame.empty())
                {
                    state.store(TestMoveState::waitingForCapture);
                    continue;
                }

                // Frame centre — equivalent to Python:  cf = view_fov // 2
                const int fcx = frame.cols / 2;
                const int fcy = frame.rows / 2;

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
                // Python: hx = x + w//2,  hy = y + int(h * 0.15)
                //         dist = np.hypot(hx - cf, hy - cf)
                //         if dist < self.kill_fov  →  candidate
                const auto& bl = Blacklist();
                float minDist  = static_cast<float>(snapKillFov);
                bool  found    = false;
                int   bestDx   = 0;
                int   bestDy   = 0;

                for (const auto& r : mergedRects)
                {
                    const int hx =
                        r.x + r.width / 2;
                    const int hy =
                        r.y + static_cast<int>(
                            static_cast<float>(r.height) * 0.15F);

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
                    state.store(TestMoveState::locked);

                    // ── Mouse movement formula — exactly as Python: ───────────
                    // mx = int(dx * self.speed * self.smooth)
                    // my = int(dy * self.speed * self.smooth)
                    // if abs(mx) > self.deadzone or abs(my) > self.deadzone:
                    //     self.mouse.move(mx, my)
                    const int mx = static_cast<int>(
                        static_cast<float>(bestDx) *
                        snapSpeed * snapSmooth);
                    const int my = static_cast<int>(
                        static_cast<float>(bestDy) *
                        snapSpeed * snapSmooth);

                    if (std::abs(mx) > snapDeadzone ||
                        std::abs(my) > snapDeadzone)
                    {
                        if (makcu->TryMove(mx, my))
                        {
                            movesMade.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
                else
                {
                    state.store(TestMoveState::tracking);
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
        if (impl.capture != nullptr)
        {
            impl.capture->SetColorTargetIndex(impl.hsvRangeIndex);
        }
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
                      currentState == TestMoveState::waitingForMakcu
                        ? ImVec4(1.0F, 0.75F, 0.32F, 1.0F)  // amber: standby
                        : ImVec4(0.72F, 0.72F, 0.78F, 1.0F); // grey: offline

        ImGui::TextColored(statusColor, "%s", StateLabel(currentState));
        ImGui::SameLine();
        ImGui::TextDisabled(" | Moves: %d", impl.movesMade.load());
        ImGui::SameLine();
        ImGui::TextDisabled(" | Pixels: %d", impl.matchingPixels.load());
        ImGui::SameLine();
        ImGui::TextDisabled(
            " | dX: %d  dY: %d",
            impl.targetDx.load(),
            impl.targetDy.load());

        custom::Separator();

        {
            std::lock_guard<std::mutex> lk(impl.settingsMutex);

            custom::Checkbox("Enable TestMove", &impl.enabled);

            if (impl.enabled)
            {
                ImGui::Spacing();

                // Color target combo (shared with capture pipeline)
                const char* colorTargets[
                    vanta::kHsvColorTargets.size()]{};
                for (std::size_t index = 0;
                     index < vanta::kHsvColorTargets.size();
                     ++index)
                {
                    colorTargets[index] =
                        vanta::kHsvColorTargets[index].label;
                }
                if (custom::Combo(
                        "Color target",
                        &impl.hsvRangeIndex,
                        colorTargets,
                        static_cast<int>(
                            vanta::kHsvColorTargets.size())) &&
                    impl.capture != nullptr)
                {
                    impl.capture->SetColorTargetIndex(
                        impl.hsvRangeIndex);
                }
                ImGui::TextDisabled(
                    "%s",
                    vanta::kHsvColorTargets[
                        static_cast<std::size_t>(
                            std::clamp(
                                impl.hsvRangeIndex,
                                0,
                                static_cast<int>(
                                    vanta::kHsvColorTargets.size()) - 1))]
                        .description);

                custom::Separator();

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
                }

                custom::Separator();

                custom::SliderInt(
                    "Kill FOV",
                    &impl.killFov,
                    20, 350, "%d px");
                ImGui::TextDisabled(
                    "Only targets within this radius are considered");

                custom::Separator();

                custom::SliderFloat(
                    "Speed multiplier",
                    &impl.speed,
                    0.10F, 3.0F, "%.3f");
                custom::SliderFloat(
                    "Smoothing",
                    &impl.smooth,
                    0.10F, 1.0F, "%.3f");
                custom::SliderInt(
                    "Deadzone",
                    &impl.deadzone,
                    0, 20, "%d px");
                ImGui::TextDisabled(
                    "Movements below this threshold are suppressed");

                custom::Separator();

                custom::SliderInt(
                    "Merge proximity",
                    &impl.mergeProximity,
                    0, 50, "%d px");
                ImGui::TextDisabled(
                    "Max gap between boxes to merge them (Python default: 10)");
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled(
            "Hold the aim key to activate. Targets the top 15%% of the bounding box.\n"
            "Uses MAKCU hardware mouse movement. Color + merge logic mirrors the Python bot.");

        ImGui::PopStyleVar();
        custom::EndChild();
    }
}
