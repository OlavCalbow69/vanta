#define IMGUI_DEFINE_MATH_OPERATORS

#include "bomb_timer_controller.hpp"

#include "capture_controller.hpp"
#include "logger.hpp"

#include <gdiplus.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_settings.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <opencv2/core.hpp>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr double kCountdownSeconds = 45.0;
    constexpr double kTestCountdownSeconds = 10.0;
    constexpr double kWarningSeconds = 6.9;
    constexpr int kClearFramesToRearm = 3;
    constexpr float kWidgetMargin = 20.0F;
    constexpr wchar_t kWidgetWindowClass[] =
        L"VantaBombTimerOverlay";

    struct WidgetStyleDefinition
    {
        const char* name;
        int width;
        int height;
        float rounding;
        float timeSize;
        bool label;
    };

    constexpr std::array<WidgetStyleDefinition, 3>
        kWidgetStyles{{
            {"Vanta Rounded", 116, 60, 18.0F, 27.0F, false},
            {"Compact Pill", 128, 46, 22.0F, 25.0F, false},
            {"Digital Panel", 144, 64, 8.0F, 27.0F, true}}};

    constexpr std::array<const char*, 3>
        kWidgetFontNames{{
            "Segoe UI Semibold",
            "Bahnschrift SemiBold",
            "Consolas Bold"}};

    constexpr std::array<const wchar_t*, 3>
        kNativeFontFamilies{{
            L"Segoe UI",
            L"Bahnschrift",
            L"Consolas"}};

    std::int64_t SteadyNowNanoseconds() noexcept
    {
        return std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().
                    time_since_epoch())
            .count();
    }

    int ColorByte(float value) noexcept
    {
        return static_cast<int>(std::lround(
            std::clamp(value, 0.0F, 1.0F) *
            255.0F));
    }

    int CountMatchingPixels(
        const cv::Mat& bgra,
        const vanta::RgbaColor& target,
        int tolerance)
    {
        if (bgra.empty() ||
            bgra.type() != CV_8UC4)
        {
            return 0;
        }

        const int targetRed = ColorByte(target.red);
        const int targetGreen = ColorByte(target.green);
        const int targetBlue = ColorByte(target.blue);
        const int allowed =
            std::clamp(tolerance, 0, 255);
        int matches = 0;
        for (int y = 0; y < bgra.rows; ++y)
        {
            const cv::Vec4b* row =
                bgra.ptr<cv::Vec4b>(y);
            for (int x = 0; x < bgra.cols; ++x)
            {
                const cv::Vec4b& pixel = row[x];
                if (std::abs(
                        static_cast<int>(pixel[2]) -
                        targetRed) <= allowed &&
                    std::abs(
                        static_cast<int>(pixel[1]) -
                        targetGreen) <= allowed &&
                    std::abs(
                        static_cast<int>(pixel[0]) -
                        targetBlue) <= allowed)
                {
                    ++matches;
                }
            }
        }
        return matches;
    }

    double RemainingSeconds(
        std::int64_t startNanoseconds,
        std::int64_t nowNanoseconds,
        double durationSeconds) noexcept
    {
        const double elapsed =
            static_cast<double>(
                std::max<std::int64_t>(
                    0,
                    nowNanoseconds -
                        startNanoseconds)) /
            1'000'000'000.0;
        return std::clamp(
            durationSeconds - elapsed,
            0.0,
            durationSeconds);
    }

    const WidgetStyleDefinition& WidgetStyle(
        int index) noexcept
    {
        return kWidgetStyles[
            static_cast<std::size_t>(
                std::clamp(
                    index,
                    0,
                    static_cast<int>(
                        kWidgetStyles.size()) - 1))];
    }

    void AddRoundedRectangle(
        Gdiplus::GraphicsPath& path,
        const Gdiplus::RectF& rectangle,
        float radius)
    {
        const float diameter = std::min(
            std::max(0.0F, radius * 2.0F),
            std::min(
                rectangle.Width,
                rectangle.Height));
        if (diameter <= 0.0F)
        {
            path.AddRectangle(rectangle);
            return;
        }

        path.AddArc(
            rectangle.X,
            rectangle.Y,
            diameter,
            diameter,
            180.0F,
            90.0F);
        path.AddArc(
            rectangle.GetRight() - diameter,
            rectangle.Y,
            diameter,
            diameter,
            270.0F,
            90.0F);
        path.AddArc(
            rectangle.GetRight() - diameter,
            rectangle.GetBottom() - diameter,
            diameter,
            diameter,
            0.0F,
            90.0F);
        path.AddArc(
            rectangle.X,
            rectangle.GetBottom() - diameter,
            diameter,
            diameter,
            90.0F,
            90.0F);
        path.CloseFigure();
    }

    struct DetectorDebounce
    {
        bool armed{true};
        int matchingFrames{};
        int clearFrames{};
        std::int64_t firstMatchTimestamp{};

        bool Process(
            int matchingPixels,
            int requiredPixels,
            int requiredFrames,
            std::int64_t timestamp)
        {
            if (!armed)
            {
                if (matchingPixels < requiredPixels)
                {
                    ++clearFrames;
                    if (clearFrames >=
                        kClearFramesToRearm)
                    {
                        armed = true;
                        clearFrames = 0;
                        matchingFrames = 0;
                        firstMatchTimestamp = 0;
                    }
                }
                else
                {
                    clearFrames = 0;
                }
                return false;
            }

            if (matchingPixels < requiredPixels)
            {
                matchingFrames = 0;
                firstMatchTimestamp = 0;
                return false;
            }

            if (matchingFrames == 0)
            {
                firstMatchTimestamp = timestamp;
            }
            ++matchingFrames;
            if (matchingFrames <
                std::max(1, requiredFrames))
            {
                return false;
            }

            armed = false;
            matchingFrames = 0;
            clearFrames = 0;
            return true;
        }

        void Disarm()
        {
            armed = false;
            matchingFrames = 0;
            clearFrames = 0;
            firstMatchTimestamp = 0;
        }

        void Arm()
        {
            armed = true;
            matchingFrames = 0;
            clearFrames = 0;
            firstMatchTimestamp = 0;
        }
    };

    const char* StateLabel(
        vanta::BombTimerState timerState)
    {
        switch (timerState)
        {
        case vanta::BombTimerState::disabled:
            return "DISABLED";
        case vanta::BombTimerState::waitingForCapture:
            return "WAITING FOR CAPTURE";
        case vanta::BombTimerState::armed:
            return "ARMED";
        case vanta::BombTimerState::confirming:
            return "CONFIRMING";
        case vanta::BombTimerState::countingDown:
            return "COUNTDOWN";
        case vanta::BombTimerState::waitingForClear:
            return "WAITING FOR HUD TO CLEAR";
        default:
            return "UNKNOWN";
        }
    }
}

namespace vanta
{
    struct BombTimerController::Implementation
    {
        CaptureController* capture{};
        HWND menuWindow{};
        HWND widgetWindow{};
        HINSTANCE moduleInstance{};
        bool widgetClassRegistered{};
        ULONG_PTR gdiplusToken{};
        ComPtr<ID3D11Device> previewDevice;
        ComPtr<ID3D11DeviceContext> previewContext;
        ComPtr<ID3D11Texture2D> previewTexture;
        ComPtr<ID3D11ShaderResourceView>
            previewShaderResource;

        mutable std::mutex settingsMutex;
        BombTimerConfig config;
        int resetKeyMode{};

        mutable std::mutex runtimeMutex;
        DetectorDebounce detector;
        bool countdownActive{};
        std::int64_t countdownStartNanoseconds{};
        double countdownDurationSeconds{
            kCountdownSeconds};

        std::atomic<BombTimerState> state{
            BombTimerState::disabled};
        std::atomic<int> matchingPixels{};
        std::atomic<int> confirmationFrames{};
        std::atomic_bool receivedCaptureFrame{false};
        std::atomic_bool stopRequested{false};
        std::thread worker;
        std::condition_variable workerCondition;
        std::mutex workerMutex;
        bool resetKeyWasDown{};

        mutable std::mutex previewFrameMutex;
        cv::Mat latestPreviewFrame;
        std::uint64_t previewFrameSequence{};
        std::uint64_t uploadedPreviewSequence{};
        int previewWidth{};
        int previewHeight{};

        mutable std::mutex widgetMutex;
        BombTimerWidgetBounds widgetBounds;
        std::atomic<ULONGLONG> lastPanelRenderTick{};
        std::atomic_bool widgetSurfaceDirty{true};
        int renderedTenths{-1};
        std::uint64_t renderedSettingsRevision{
            UINT64_MAX};
        COLORREF renderedAccent{};
        ULONGLONG nextWidgetZOrderRefresh{};
        ULONGLONG nextWidgetAffinityRetry{};
        bool widgetCaptureExcluded{};
        bool widgetAffinityWarningLogged{};
        std::atomic<std::uint64_t> settingsRevision{0};

        bool IsEnabled() const
        {
            std::lock_guard<std::mutex> lock(
                settingsMutex);
            return config.enabled;
        }

        NormalizedCaptureRegion CaptureRegion() const
        {
            std::lock_guard<std::mutex> lock(
                settingsMutex);
            return {
                config.enabled,
                config.regionLeft,
                config.regionTop,
                config.regionWidth,
                config.regionHeight};
        }

        void UpdateCaptureRequest()
        {
            if (capture != nullptr)
            {
                capture->SetAuxiliaryNormalizedRegion(
                    CaptureRegion());
            }
            workerCondition.notify_all();
        }

        bool IsCountingDown() const
        {
            std::lock_guard<std::mutex> lock(
                runtimeMutex);
            return countdownActive;
        }

        double Remaining() const
        {
            std::lock_guard<std::mutex> lock(
                runtimeMutex);
            if (!countdownActive)
            {
                return 0.0;
            }
            return RemainingSeconds(
                countdownStartNanoseconds,
                SteadyNowNanoseconds(),
                countdownDurationSeconds);
        }

        void StartCountdownAt(
            std::int64_t timestamp,
            const char* source,
            double durationSeconds =
                kCountdownSeconds,
            bool requireEnabled = true,
            bool restart = false)
        {
            if (requireEnabled && !IsEnabled())
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(
                    runtimeMutex);
                if (countdownActive && !restart)
                {
                    return;
                }
                countdownActive = true;
                countdownDurationSeconds =
                    std::max(0.1, durationSeconds);
                countdownStartNanoseconds =
                    timestamp > 0
                    ? std::min(
                        timestamp,
                        SteadyNowNanoseconds())
                    : SteadyNowNanoseconds();
                detector.Disarm();
            }
            state.store(
                BombTimerState::countingDown,
                std::memory_order_release);
            widgetSurfaceDirty.store(
                true,
                std::memory_order_release);
            vanta::log::Info(
                "Bomb Timer started (%s, %.1f s)",
                source,
                durationSeconds);
        }

        void ResetCountdown(
            const char* source,
            bool logReset = true)
        {
            {
                std::lock_guard<std::mutex> lock(
                    runtimeMutex);
                countdownActive = false;
                countdownStartNanoseconds = 0;
                countdownDurationSeconds =
                    kCountdownSeconds;
                detector.Disarm();
            }
            confirmationFrames.store(
                0,
                std::memory_order_relaxed);
            state.store(
                IsEnabled()
                    ? BombTimerState::waitingForClear
                    : BombTimerState::disabled,
                std::memory_order_release);
            widgetSurfaceDirty.store(
                true,
                std::memory_order_release);
            if (logReset)
            {
                vanta::log::Info(
                    "Bomb Timer reset (%s)",
                source);
            }
        }

        static LRESULT CALLBACK WidgetWindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            switch (message)
            {
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                return 0;
            }
            default:
                return DefWindowProcW(
                    window,
                    message,
                    wParam,
                    lParam);
            }
        }

        bool CreateWidgetWindow()
        {
            moduleInstance = GetModuleHandleW(nullptr);
            Gdiplus::GdiplusStartupInput startupInput;
            if (Gdiplus::GdiplusStartup(
                    &gdiplusToken,
                    &startupInput,
                    nullptr) != Gdiplus::Ok)
            {
                vanta::log::Error(
                    "Bomb Timer overlay anti-aliasing initialization failed");
                return false;
            }

            WNDCLASSEXW windowClass{
                sizeof(WNDCLASSEXW),
                0,
                &WidgetWindowProcedure,
                0,
                0,
                moduleInstance,
                nullptr,
                LoadCursorW(nullptr, IDC_ARROW),
                nullptr,
                nullptr,
                kWidgetWindowClass,
                nullptr};
            if (RegisterClassExW(&windowClass) == 0)
            {
                const DWORD error = GetLastError();
                if (error != ERROR_CLASS_ALREADY_EXISTS)
                {
                    vanta::log::Error(
                        "Bomb Timer overlay RegisterClassExW failed: %lu",
                        error);
                    return false;
                }
            }
            else
            {
                widgetClassRegistered = true;
            }

            widgetWindow = CreateWindowExW(
                WS_EX_TOPMOST |
                    WS_EX_TOOLWINDOW |
                    WS_EX_LAYERED |
                    WS_EX_TRANSPARENT |
                    WS_EX_NOACTIVATE,
                kWidgetWindowClass,
                L"vanta bomb timer",
                WS_POPUP,
                0,
                0,
                1,
                1,
                nullptr,
                nullptr,
                moduleInstance,
                nullptr);
            if (widgetWindow == nullptr)
            {
                vanta::log::Error(
                    "Bomb Timer overlay CreateWindowExW failed: %lu",
                    GetLastError());
                return false;
            }

            SetLayeredWindowAttributes(
                widgetWindow,
                0,
                255,
                LWA_ALPHA);
            widgetCaptureExcluded =
                SetWindowDisplayAffinity(
                    widgetWindow,
                    WDA_EXCLUDEFROMCAPTURE) != FALSE;
            const LONG_PTR extendedStyle =
                GetWindowLongPtrW(
                    widgetWindow,
                    GWL_EXSTYLE);
            SetWindowLongPtrW(
                widgetWindow,
                GWL_EXSTYLE,
                extendedStyle &
                    ~static_cast<LONG_PTR>(
                        WS_EX_LAYERED));
            SetWindowLongPtrW(
                widgetWindow,
                GWL_EXSTYLE,
                extendedStyle);
            return true;
        }

        void DestroyWidgetWindow()
        {
            if (widgetWindow != nullptr &&
                IsWindow(widgetWindow))
            {
                DestroyWindow(widgetWindow);
            }
            widgetWindow = nullptr;
            if (widgetClassRegistered &&
                moduleInstance != nullptr)
            {
                UnregisterClassW(
                    kWidgetWindowClass,
                    moduleInstance);
            }
            widgetClassRegistered = false;
            if (gdiplusToken != 0)
            {
                Gdiplus::GdiplusShutdown(
                    gdiplusToken);
                gdiplusToken = 0;
            }
        }

        RECT ResolveWidgetRectangle(
            const BombTimerConfig& snapshot)
        {
            const WidgetStyleDefinition& style =
                WidgetStyle(snapshot.widgetStyle);
            const LONG virtualLeft =
                GetSystemMetrics(SM_XVIRTUALSCREEN);
            const LONG virtualTop =
                GetSystemMetrics(SM_YVIRTUALSCREEN);
            const LONG virtualRight =
                virtualLeft +
                GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const LONG virtualBottom =
                virtualTop +
                GetSystemMetrics(SM_CYVIRTUALSCREEN);

            const float requestedX =
                snapshot.hasWidgetPosition
                ? snapshot.widgetPositionX
                : static_cast<float>(
                    virtualRight -
                    style.width) -
                    kWidgetMargin;
            const float requestedY =
                snapshot.hasWidgetPosition
                ? snapshot.widgetPositionY
                : static_cast<float>(
                    virtualBottom -
                    style.height) -
                    kWidgetMargin;
            const float clampedX = std::clamp(
                requestedX,
                static_cast<float>(virtualLeft),
                std::max(
                    static_cast<float>(virtualLeft),
                    static_cast<float>(
                        virtualRight -
                        style.width)));
            const float clampedY = std::clamp(
                requestedY,
                static_cast<float>(virtualTop),
                std::max(
                    static_cast<float>(virtualTop),
                    static_cast<float>(
                        virtualBottom -
                        style.height)));

            if (snapshot.hasWidgetPosition &&
                (std::abs(clampedX - requestedX) >
                     0.5F ||
                 std::abs(clampedY - requestedY) >
                     0.5F))
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                config.widgetPositionX = clampedX;
                config.widgetPositionY = clampedY;
                settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }

            const LONG left =
                static_cast<LONG>(
                    std::lround(clampedX));
            const LONG top =
                static_cast<LONG>(
                    std::lround(clampedY));
            return {
                left,
                top,
                left + style.width,
                top + style.height};
        }

        bool RenderWidgetSurface(
            const BombTimerConfig& snapshot,
            const RECT& screenRectangle,
            double remaining)
        {
            if (widgetWindow == nullptr)
            {
                return false;
            }
            const int width =
                screenRectangle.right -
                screenRectangle.left;
            const int height =
                screenRectangle.bottom -
                screenRectangle.top;
            if (width <= 0 || height <= 0)
            {
                return false;
            }

            HDC screenContext = GetDC(nullptr);
            if (screenContext == nullptr)
            {
                return false;
            }
            HDC memoryContext =
                CreateCompatibleDC(screenContext);
            if (memoryContext == nullptr)
            {
                ReleaseDC(nullptr, screenContext);
                return false;
            }

            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize =
                sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = width;
            bitmapInfo.bmiHeader.biHeight = -height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HBITMAP bitmap = CreateDIBSection(
                screenContext,
                &bitmapInfo,
                DIB_RGB_COLORS,
                &pixels,
                nullptr,
                0);
            if (bitmap == nullptr || pixels == nullptr)
            {
                if (bitmap != nullptr)
                {
                    DeleteObject(bitmap);
                }
                DeleteDC(memoryContext);
                ReleaseDC(nullptr, screenContext);
                return false;
            }

            HGDIOBJ previousBitmap =
                SelectObject(memoryContext, bitmap);
            bool rendered = false;
            {
                const WidgetStyleDefinition& style =
                    WidgetStyle(snapshot.widgetStyle);
                const float opacity = std::clamp(
                    snapshot.widgetOpacity,
                    0.10F,
                    1.0F);
                const bool warning =
                    remaining <= kWarningSeconds;
                const RgbaColor textColor =
                    warning
                    ? snapshot.warningColor
                    : snapshot.safeColor;
                const ImVec4 accentValue =
                    c::main_color.Value;
                const BYTE edgeAlpha =
                    static_cast<BYTE>(
                        std::lround(225.0F * opacity));
                const BYTE accentAlpha =
                    static_cast<BYTE>(
                        std::lround(255.0F * opacity));
                const BYTE backgroundAlpha =
                    static_cast<BYTE>(
                        std::lround(155.0F * opacity));

                Gdiplus::Bitmap surface(
                    width,
                    height,
                    width * 4,
                    PixelFormat32bppPARGB,
                    static_cast<BYTE*>(pixels));
                Gdiplus::Graphics graphics(&surface);
                graphics.SetSmoothingMode(
                    Gdiplus::SmoothingModeAntiAlias);
                graphics.SetPixelOffsetMode(
                    Gdiplus::PixelOffsetModeHighQuality);
                graphics.SetCompositingQuality(
                    Gdiplus::CompositingQualityHighQuality);
                graphics.SetTextRenderingHint(
                    Gdiplus::TextRenderingHintAntiAliasGridFit);
                graphics.Clear(
                    Gdiplus::Color(0, 0, 0, 0));

                Gdiplus::GraphicsPath backgroundPath;
                AddRoundedRectangle(
                    backgroundPath,
                    Gdiplus::RectF(
                        3.0F,
                        3.0F,
                        static_cast<float>(width - 6),
                        static_cast<float>(height - 6)),
                    std::max(
                        0.0F,
                        style.rounding - 3.0F));
                Gdiplus::SolidBrush backgroundBrush(
                    Gdiplus::Color(
                        backgroundAlpha,
                        13,
                        13,
                        17));
                graphics.FillPath(
                    &backgroundBrush,
                    &backgroundPath);

                Gdiplus::GraphicsPath outerPath;
                Gdiplus::GraphicsPath accentPath;
                Gdiplus::GraphicsPath innerPath;
                AddRoundedRectangle(
                    outerPath,
                    Gdiplus::RectF(
                        0.5F,
                        0.5F,
                        static_cast<float>(width - 1),
                        static_cast<float>(height - 1)),
                    style.rounding);
                AddRoundedRectangle(
                    accentPath,
                    Gdiplus::RectF(
                        1.5F,
                        1.5F,
                        static_cast<float>(width - 3),
                        static_cast<float>(height - 3)),
                    std::max(
                        0.0F,
                        style.rounding - 1.0F));
                AddRoundedRectangle(
                    innerPath,
                    Gdiplus::RectF(
                        2.5F,
                        2.5F,
                        static_cast<float>(width - 5),
                        static_cast<float>(height - 5)),
                    std::max(
                        0.0F,
                        style.rounding - 2.0F));
                Gdiplus::Pen blackPen(
                    Gdiplus::Color(
                        edgeAlpha,
                        0,
                        0,
                        0),
                    1.0F);
                Gdiplus::Pen accentPen(
                    Gdiplus::Color(
                        accentAlpha,
                        static_cast<BYTE>(
                            ColorByte(accentValue.x)),
                        static_cast<BYTE>(
                            ColorByte(accentValue.y)),
                        static_cast<BYTE>(
                            ColorByte(accentValue.z))),
                    1.0F);
                graphics.DrawPath(
                    &blackPen,
                    &outerPath);
                graphics.DrawPath(
                    &accentPen,
                    &accentPath);
                graphics.DrawPath(
                    &blackPen,
                    &innerPath);

                const int fontIndex = std::clamp(
                    snapshot.widgetFont,
                    0,
                    static_cast<int>(
                        kNativeFontFamilies.size()) - 1);
                Gdiplus::FontFamily family(
                    kNativeFontFamilies[
                        static_cast<std::size_t>(
                            fontIndex)]);
                Gdiplus::FontFamily fallbackFamily(
                    L"Segoe UI");
                Gdiplus::FontFamily* selectedFamily =
                    family.GetLastStatus() ==
                            Gdiplus::Ok
                    ? &family
                    : &fallbackFamily;
                Gdiplus::Font timeFont(
                    selectedFamily,
                    style.timeSize,
                    Gdiplus::FontStyleBold,
                    Gdiplus::UnitPixel);
                Gdiplus::StringFormat centered;
                centered.SetAlignment(
                    Gdiplus::StringAlignmentCenter);
                centered.SetLineAlignment(
                    Gdiplus::StringAlignmentCenter);

                wchar_t timeText[32]{};
                swprintf_s(
                    timeText,
                    L"%.1f",
                    remaining);
                Gdiplus::SolidBrush textBrush(
                    Gdiplus::Color(
                        accentAlpha,
                        static_cast<BYTE>(
                            ColorByte(textColor.red)),
                        static_cast<BYTE>(
                            ColorByte(textColor.green)),
                        static_cast<BYTE>(
                            ColorByte(textColor.blue))));
                Gdiplus::RectF timeBounds(
                    4.0F,
                    style.label ? 13.0F : 3.0F,
                    static_cast<float>(width - 8),
                    static_cast<float>(
                        height -
                        (style.label ? 15 : 6)));
                graphics.DrawString(
                    timeText,
                    -1,
                    &timeFont,
                    timeBounds,
                    &centered,
                    &textBrush);

                if (style.label)
                {
                    Gdiplus::Font labelFont(
                        selectedFamily,
                        8.0F,
                        Gdiplus::FontStyleBold,
                        Gdiplus::UnitPixel);
                    Gdiplus::SolidBrush labelBrush(
                        Gdiplus::Color(
                            accentAlpha,
                            static_cast<BYTE>(
                                ColorByte(accentValue.x)),
                            static_cast<BYTE>(
                                ColorByte(accentValue.y)),
                            static_cast<BYTE>(
                                ColorByte(accentValue.z))));
                    Gdiplus::RectF labelBounds(
                        4.0F,
                        4.0F,
                        static_cast<float>(width - 8),
                        14.0F);
                    graphics.DrawString(
                        L"VANTA  //  BOMB",
                        -1,
                        &labelFont,
                        labelBounds,
                        &centered,
                        &labelBrush);
                }
                rendered = true;
            }

            if (rendered)
            {
                POINT destination{
                    screenRectangle.left,
                    screenRectangle.top};
                SIZE size{width, height};
                POINT source{0, 0};
                BLENDFUNCTION blend{
                    AC_SRC_OVER,
                    0,
                    255,
                    AC_SRC_ALPHA};
                rendered =
                    UpdateLayeredWindow(
                        widgetWindow,
                        screenContext,
                        &destination,
                        &size,
                        memoryContext,
                        &source,
                        0,
                        &blend,
                        ULW_ALPHA) != FALSE;
            }

            SelectObject(
                memoryContext,
                previousBitmap);
            DeleteObject(bitmap);
            DeleteDC(memoryContext);
            ReleaseDC(nullptr, screenContext);
            return rendered;
        }

        void UpdateNativeWidget()
        {
            if (widgetWindow == nullptr)
            {
                return;
            }
            const ULONGLONG nowTick =
                GetTickCount64();
            const bool menuVisible =
                menuWindow != nullptr &&
                IsWindowVisible(menuWindow);
            const bool editorPreview =
                menuVisible &&
                nowTick -
                    lastPanelRenderTick.load(
                        std::memory_order_acquire) <
                    180;
            const bool counting = IsCountingDown();
            const double remaining =
                counting ? Remaining() : 12.3;
            const bool shouldShow =
                (counting && remaining > 0.0) ||
                editorPreview;
            if (!shouldShow)
            {
                if (IsWindowVisible(widgetWindow))
                {
                    ShowWindow(widgetWindow, SW_HIDE);
                }
                std::lock_guard<std::mutex> lock(
                    widgetMutex);
                widgetBounds = {};
                renderedTenths = -1;
                return;
            }

            BombTimerConfig snapshot;
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                snapshot = config;
            }
            const RECT rectangle =
                ResolveWidgetRectangle(snapshot);
            const int tenths =
                static_cast<int>(
                    std::lround(remaining * 10.0));
            const COLORREF accent = RGB(
                ColorByte(c::main_color.Value.x),
                ColorByte(c::main_color.Value.y),
                ColorByte(c::main_color.Value.z));
            const std::uint64_t revision =
                settingsRevision.load(
                    std::memory_order_relaxed);
            const bool surfaceChanged =
                widgetSurfaceDirty.exchange(
                    false,
                    std::memory_order_acq_rel) ||
                tenths != renderedTenths ||
                revision != renderedSettingsRevision ||
                accent != renderedAccent;
            if (surfaceChanged)
            {
                if (!RenderWidgetSurface(
                        snapshot,
                        rectangle,
                        remaining))
                {
                    vanta::log::Warning(
                        "Bomb Timer overlay surface update failed");
                }
                renderedTenths = tenths;
                renderedSettingsRevision = revision;
                renderedAccent = accent;
            }

            if (surfaceChanged ||
                !IsWindowVisible(widgetWindow) ||
                nowTick >= nextWidgetZOrderRefresh)
            {
                const HWND insertAfter =
                    menuVisible
                    ? menuWindow
                    : HWND_TOPMOST;
                SetWindowPos(
                    widgetWindow,
                    insertAfter,
                    rectangle.left,
                    rectangle.top,
                    rectangle.right -
                        rectangle.left,
                    rectangle.bottom -
                        rectangle.top,
                    SWP_NOACTIVATE |
                        SWP_SHOWWINDOW |
                        SWP_NOOWNERZORDER);
                nextWidgetZOrderRefresh =
                    nowTick + 500;
            }
            if (!widgetCaptureExcluded &&
                nowTick >= nextWidgetAffinityRetry)
            {
                widgetCaptureExcluded =
                    SetWindowDisplayAffinity(
                        widgetWindow,
                        WDA_EXCLUDEFROMCAPTURE) != FALSE;
                if (!widgetCaptureExcluded &&
                    !widgetAffinityWarningLogged)
                {
                    vanta::log::Warning(
                        "Bomb Timer overlay capture exclusion retry failed: %lu",
                        GetLastError());
                    widgetAffinityWarningLogged = true;
                }
                nextWidgetAffinityRetry =
                    nowTick + 2000;
            }
            {
                std::lock_guard<std::mutex> lock(
                    widgetMutex);
                widgetBounds.visible = true;
                widgetBounds.screenRectangle =
                    rectangle;
            }
        }

        void Initialize(
            CaptureController* captureController,
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            HWND overlayMenuWindow,
            const BombTimerConfig* initialConfig)
        {
            capture = captureController;
            menuWindow = overlayMenuWindow;
            previewDevice = device;
            previewContext = context;
            if (initialConfig != nullptr)
            {
                ApplyConfig(*initialConfig, false);
            }
            else
            {
                std::lock_guard<std::mutex> lock(
                    runtimeMutex);
                detector.Arm();
            }
            if (!CreateWidgetWindow())
            {
                vanta::log::Warning(
                    "Bomb Timer will run without its native timer overlay");
            }
            UpdateCaptureRequest();
            stopRequested.store(
                false,
                std::memory_order_release);
            worker = std::thread(
                [this]()
                {
                    WorkerMain();
                });
            vanta::log::Info(
                "Bomb Timer controller initialized");
        }

        void WorkerMain()
        {
            std::uint64_t lastSequence = 0;
            int missingFrameWaits = 0;
            while (!stopRequested.load(
                std::memory_order_acquire))
            {
                BombTimerConfig snapshot;
                {
                    std::lock_guard<std::mutex> lock(
                        settingsMutex);
                    snapshot = config;
                }
                if (!snapshot.enabled ||
                    capture == nullptr)
                {
                    state.store(
                        IsCountingDown()
                            ? BombTimerState::countingDown
                            : BombTimerState::disabled,
                        std::memory_order_release);
                    std::unique_lock<std::mutex> lock(
                        workerMutex);
                    workerCondition.wait_for(
                        lock,
                        std::chrono::milliseconds(50));
                    continue;
                }

                if (IsCountingDown())
                {
                    state.store(
                        BombTimerState::countingDown,
                        std::memory_order_release);
                }

                cv::Mat frame;
                std::uint64_t sequence = 0;
                std::int64_t timestamp = 0;
                RECT screenRectangle{};
                if (!capture->WaitForAuxiliaryFrame(
                        lastSequence,
                        frame,
                        sequence,
                        timestamp,
                        screenRectangle,
                        20))
                {
                    ++missingFrameWaits;
                    if (!IsCountingDown() &&
                        missingFrameWaits >= 5)
                    {
                        std::lock_guard<std::mutex> lock(
                            runtimeMutex);
                        detector.matchingFrames = 0;
                        detector.firstMatchTimestamp = 0;
                        confirmationFrames.store(
                            0,
                            std::memory_order_relaxed);
                        state.store(
                            BombTimerState::
                                waitingForCapture,
                            std::memory_order_release);
                    }
                    continue;
                }
                missingFrameWaits = 0;
                lastSequence = sequence;
                receivedCaptureFrame.store(
                    true,
                    std::memory_order_release);

                {
                    std::lock_guard<std::mutex> lock(
                        previewFrameMutex);
                    latestPreviewFrame = frame;
                    ++previewFrameSequence;
                }

                const int matches =
                    CountMatchingPixels(
                        frame,
                        snapshot.targetColor,
                        snapshot.colorTolerance);
                matchingPixels.store(
                    matches,
                    std::memory_order_relaxed);

                bool trigger = false;
                std::int64_t triggerTimestamp = 0;
                {
                    std::lock_guard<std::mutex> lock(
                        runtimeMutex);
                    if (countdownActive)
                    {
                        state.store(
                            BombTimerState::countingDown,
                            std::memory_order_release);
                        continue;
                    }

                    trigger = detector.Process(
                        matches,
                        std::max(
                            1,
                            snapshot.requiredMatchingPixels),
                        std::max(
                            1,
                            snapshot.requiredConsecutiveFrames),
                        timestamp);
                    confirmationFrames.store(
                        detector.matchingFrames,
                        std::memory_order_relaxed);
                    triggerTimestamp =
                        detector.firstMatchTimestamp;
                    state.store(
                        detector.armed
                            ? detector.matchingFrames > 0
                                ? BombTimerState::confirming
                                : BombTimerState::armed
                            : BombTimerState::
                                waitingForClear,
                        std::memory_order_release);
                }
                if (trigger)
                {
                    StartCountdownAt(
                        triggerTimestamp,
                        "HUD detection");
                }
            }
            vanta::log::Info(
                "Bomb Timer worker stopped");
        }

        void Shutdown()
        {
            if (stopRequested.exchange(
                    true,
                    std::memory_order_acq_rel))
            {
                return;
            }
            workerCondition.notify_all();
            if (worker.joinable())
            {
                worker.join();
            }
            if (capture != nullptr)
            {
                capture->SetAuxiliaryNormalizedRegion({});
            }
            capture = nullptr;
            {
                std::lock_guard<std::mutex> lock(
                    runtimeMutex);
                countdownActive = false;
                countdownStartNanoseconds = 0;
                countdownDurationSeconds =
                    kCountdownSeconds;
            }
            DestroyWidgetWindow();
            menuWindow = nullptr;
            previewShaderResource.Reset();
            previewTexture.Reset();
            previewContext.Reset();
            previewDevice.Reset();
            state.store(
                BombTimerState::disabled,
                std::memory_order_release);
            vanta::log::Info(
                "Bomb Timer controller released");
        }

        void Tick()
        {
            const bool enabled = IsEnabled();
            int resetKey = VK_HOME;
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                resetKey = config.resetKey;
            }
            const bool resetDown =
                enabled &&
                resetKey != 0 &&
                (GetAsyncKeyState(resetKey) &
                 0x8000) != 0;
            if (resetDown && !resetKeyWasDown)
            {
                ResetCountdown("hotkey");
            }
            resetKeyWasDown = resetDown;

            bool expired = false;
            {
                std::lock_guard<std::mutex> lock(
                    runtimeMutex);
                expired =
                    countdownActive &&
                    RemainingSeconds(
                        countdownStartNanoseconds,
                        SteadyNowNanoseconds(),
                        countdownDurationSeconds) <= 0.0;
            }
            if (expired)
            {
                ResetCountdown(
                    "countdown completed",
                    false);
                vanta::log::Info(
                    "Bomb Timer countdown completed");
            }
            UpdateNativeWidget();
        }

        bool UpdatePreviewTexture()
        {
            cv::Mat frame;
            std::uint64_t sequence = 0;
            {
                std::lock_guard<std::mutex> lock(
                    previewFrameMutex);
                if (previewFrameSequence ==
                        uploadedPreviewSequence ||
                    latestPreviewFrame.empty())
                {
                    return previewShaderResource != nullptr;
                }
                frame = latestPreviewFrame;
                sequence = previewFrameSequence;
            }
            if (previewDevice == nullptr ||
                previewContext == nullptr ||
                frame.type() != CV_8UC4)
            {
                return false;
            }

            if (previewTexture == nullptr ||
                frame.cols != previewWidth ||
                frame.rows != previewHeight)
            {
                previewShaderResource.Reset();
                previewTexture.Reset();

                D3D11_TEXTURE2D_DESC description{};
                description.Width =
                    static_cast<UINT>(frame.cols);
                description.Height =
                    static_cast<UINT>(frame.rows);
                description.MipLevels = 1;
                description.ArraySize = 1;
                description.Format =
                    DXGI_FORMAT_B8G8R8A8_UNORM;
                description.SampleDesc.Count = 1;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags =
                    D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(previewDevice->CreateTexture2D(
                        &description,
                        nullptr,
                        &previewTexture)) ||
                    FAILED(previewDevice->
                        CreateShaderResourceView(
                            previewTexture.Get(),
                            nullptr,
                            &previewShaderResource)))
                {
                    previewTexture.Reset();
                    previewShaderResource.Reset();
                    return false;
                }
                previewWidth = frame.cols;
                previewHeight = frame.rows;
            }

            previewContext->UpdateSubresource(
                previewTexture.Get(),
                0,
                nullptr,
                frame.data,
                static_cast<UINT>(frame.step),
                0);
            uploadedPreviewSequence = sequence;
            return true;
        }

        void DrawCalibrationRectangle(
            const RgbaColor& target)
        {
            if (capture == nullptr)
            {
                return;
            }
            RECT rectangle{};
            if (!capture->GetAuxiliaryScreenRectangle(
                    rectangle))
            {
                return;
            }
            const float originX =
                static_cast<float>(
                    GetSystemMetrics(
                        SM_XVIRTUALSCREEN));
            const float originY =
                static_cast<float>(
                    GetSystemMetrics(
                        SM_YVIRTUALSCREEN));
            const ImVec2 minimum(
                rectangle.left - originX,
                rectangle.top - originY);
            const ImVec2 maximum(
                rectangle.right - originX,
                rectangle.bottom - originY);
            ImDrawList* draw =
                ImGui::GetBackgroundDrawList();
            draw->AddRect(
                minimum,
                maximum,
                IM_COL32(0, 0, 0, 230),
                0.0F,
                0,
                4.0F);
            draw->AddRect(
                minimum,
                maximum,
                IM_COL32(
                    ColorByte(target.red),
                    ColorByte(target.green),
                    ColorByte(target.blue),
                    255),
                0.0F,
                0,
                2.0F);
        }

        void DrawWidgetPreview(
            const BombTimerConfig& snapshot)
        {
            const WidgetStyleDefinition& style =
                WidgetStyle(snapshot.widgetStyle);
            ImFont* selectedFont = nullptr;
            switch (std::clamp(
                snapshot.widgetFont,
                0,
                2))
            {
            case 1:
                selectedFont =
                    font::timer_bahnschrift;
                break;
            case 2:
                selectedFont =
                    font::timer_consolas;
                break;
            default:
                selectedFont =
                    font::timer_segoe;
                break;
            }
            if (selectedFont == nullptr)
            {
                selectedFont = ImGui::GetFont();
            }

            ImGui::TextDisabled(
                "Live overlay preview (always click-through)");
            const ImVec2 previewPosition =
                ImGui::GetCursorScreenPos();
            const ImVec2 size(
                static_cast<float>(style.width),
                static_cast<float>(style.height));
            const float opacity = std::clamp(
                snapshot.widgetOpacity,
                0.10F,
                1.0F);
            ImDrawList* draw =
                ImGui::GetWindowDrawList();
            draw->AddRectFilled(
                previewPosition + ImVec2(3.0F, 3.0F),
                previewPosition + size -
                    ImVec2(3.0F, 3.0F),
                IM_COL32(
                    13,
                    13,
                    17,
                    static_cast<int>(
                        std::lround(
                            155.0F * opacity))),
                std::max(
                    0.0F,
                    style.rounding - 3.0F));
            draw->AddRect(
                previewPosition + ImVec2(0.5F, 0.5F),
                previewPosition + size -
                    ImVec2(0.5F, 0.5F),
                IM_COL32(
                    0,
                    0,
                    0,
                    static_cast<int>(
                        std::lround(
                            225.0F * opacity))),
                style.rounding,
                0,
                1.0F);
            draw->AddRect(
                previewPosition + ImVec2(1.5F, 1.5F),
                previewPosition + size -
                    ImVec2(1.5F, 1.5F),
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(
                        c::main_color.Value.x,
                        c::main_color.Value.y,
                        c::main_color.Value.z,
                        opacity)),
                std::max(
                    0.0F,
                    style.rounding - 1.0F),
                0,
                1.0F);
            draw->AddRect(
                previewPosition + ImVec2(2.5F, 2.5F),
                previewPosition + size -
                    ImVec2(2.5F, 2.5F),
                IM_COL32(
                    0,
                    0,
                    0,
                    static_cast<int>(
                        std::lround(
                            225.0F * opacity))),
                std::max(
                    0.0F,
                    style.rounding - 2.0F),
                0,
                1.0F);

            if (style.label)
            {
                const char* label =
                    "VANTA  //  BOMB";
                const ImVec2 labelSize =
                    ImGui::CalcTextSize(label);
                draw->AddText(
                    previewPosition +
                        ImVec2(
                            (size.x -
                             labelSize.x) *
                                0.5F,
                            6.0F),
                    ImGui::ColorConvertFloat4ToU32(
                        ImVec4(
                            c::main_color.Value.x,
                            c::main_color.Value.y,
                            c::main_color.Value.z,
                            opacity)),
                    label);
            }

            const char* timeText = "12.3";
            const ImVec2 textSize =
                selectedFont->CalcTextSizeA(
                    style.timeSize,
                    1000.0F,
                    0.0F,
                    timeText);
            const float availableTop =
                style.label ? 15.0F : 0.0F;
            draw->AddText(
                selectedFont,
                style.timeSize,
                previewPosition +
                    ImVec2(
                        (size.x - textSize.x) *
                            0.5F,
                        availableTop +
                            (size.y -
                             availableTop -
                             textSize.y) *
                                0.5F),
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(
                        snapshot.safeColor.red,
                        snapshot.safeColor.green,
                        snapshot.safeColor.blue,
                        opacity)),
                timeText);
            ImGui::Dummy(size);
        }

        void RenderPanel()
        {
            lastPanelRenderTick.store(
                GetTickCount64(),
                std::memory_order_release);
            const float bodyHeight = std::max(
                190.0F,
                ImGui::GetContentRegionAvail().y -
                    40.0F);
            custom::Child(
                ICON_BOMB_LINE
                    "  Bomb Timer##bomb-timer-panel",
                ImVec2(0.0F, bodyHeight),
                true);
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));

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
            drawCategory("STATUS");

            const BombTimerState currentState =
                state.load(std::memory_order_acquire);
            const ImVec4 statusColor =
                currentState ==
                    BombTimerState::countingDown
                ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                : currentState ==
                        BombTimerState::armed
                    ? ImVec4(
                        0.45F,
                        0.96F,
                        0.65F,
                        1.0F)
                    : currentState ==
                            BombTimerState::confirming
                        ? ImVec4(
                            1.0F,
                            0.75F,
                            0.32F,
                            1.0F)
                        : ImVec4(
                            0.72F,
                            0.72F,
                            0.78F,
                            1.0F);
            ImGui::TextColored(
                statusColor,
                "%s",
                StateLabel(currentState));
            if (IsCountingDown())
            {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    " | %.1f s",
                    Remaining());
            }

            bool settingsChanged = false;
            bool enabledChanged = false;
            bool startRequested = false;
            bool testRequested = false;
            bool resetRequested = false;
            BombTimerConfig snapshot;
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                enabledChanged =
                    custom::Checkbox(
                        "Enable Bomb Timer",
                        &config.enabled);
                settingsChanged |= enabledChanged;
                if (config.enabled)
                {
                    drawCategory("TIMER");
                    if (custom::Button(
                            ICON_PLAY_LINE
                                "  Start 45s",
                            ImVec2(155.0F, 36.0F)))
                    {
                        startRequested = true;
                    }
                    ImGui::SameLine();
                    if (custom::Button(
                            ICON_REFRESH_1_LINE
                                "  Reset",
                            ImVec2(130.0F, 36.0F)))
                    {
                        resetRequested = true;
                    }
                    settingsChanged |= custom::Keybind(
                        "Reset hotkey",
                        &config.resetKey,
                        &resetKeyMode);
                    ImGui::TextDisabled(
                        "45.0 s countdown | warning at 6.9 s");

                }

                drawCategory("WIDGET");
                static const char* styles[]{
                    "Vanta Rounded",
                    "Compact Pill",
                    "Digital Panel"};
                static const char* fonts[]{
                    "Segoe UI Semibold",
                    "Bahnschrift SemiBold",
                    "Consolas Bold"};
                settingsChanged |= custom::Combo(
                    "Panel style",
                    &config.widgetStyle,
                    styles,
                    IM_ARRAYSIZE(styles));
                settingsChanged |= custom::Combo(
                    "Timer font",
                    &config.widgetFont,
                    fonts,
                    IM_ARRAYSIZE(fonts));

                float safe[4]{
                    config.safeColor.red,
                    config.safeColor.green,
                    config.safeColor.blue,
                    1.0F};
                if (custom::ColorEdit4(
                        "Safe color",
                        safe,
                        ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoInputs))
                {
                    config.safeColor = {
                        safe[0],
                        safe[1],
                        safe[2],
                        1.0F};
                    settingsChanged = true;
                }
                float warning[4]{
                    config.warningColor.red,
                    config.warningColor.green,
                    config.warningColor.blue,
                    1.0F};
                if (custom::ColorEdit4(
                        "Warning color",
                        warning,
                        ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoInputs))
                {
                    config.warningColor = {
                        warning[0],
                        warning[1],
                        warning[2],
                        1.0F};
                    settingsChanged = true;
                }
                settingsChanged |= custom::SliderFloat(
                    "Panel opacity",
                    &config.widgetOpacity,
                    0.10F,
                    1.0F,
                    "%.2f");

                const WidgetStyleDefinition& style =
                    WidgetStyle(config.widgetStyle);
                const float virtualLeft =
                    static_cast<float>(
                        GetSystemMetrics(
                            SM_XVIRTUALSCREEN));
                const float virtualTop =
                    static_cast<float>(
                        GetSystemMetrics(
                            SM_YVIRTUALSCREEN));
                const float virtualRight =
                    virtualLeft +
                    GetSystemMetrics(
                        SM_CXVIRTUALSCREEN);
                const float virtualBottom =
                    virtualTop +
                    GetSystemMetrics(
                        SM_CYVIRTUALSCREEN);
                float widgetX =
                    config.hasWidgetPosition
                    ? config.widgetPositionX
                    : virtualRight -
                        style.width -
                        kWidgetMargin;
                float widgetY =
                    config.hasWidgetPosition
                    ? config.widgetPositionY
                    : virtualBottom -
                        style.height -
                        kWidgetMargin;
                const bool xChanged =
                    custom::SliderFloat(
                        "Timer X",
                        &widgetX,
                        virtualLeft,
                        std::max(
                            virtualLeft,
                            virtualRight -
                                style.width),
                        "%.0f px");
                const bool yChanged =
                    custom::SliderFloat(
                        "Timer Y",
                        &widgetY,
                        virtualTop,
                        std::max(
                            virtualTop,
                            virtualBottom -
                                style.height),
                        "%.0f px");
                if (xChanged || yChanged)
                {
                    config.hasWidgetPosition = true;
                    config.widgetPositionX = widgetX;
                    config.widgetPositionY = widgetY;
                    settingsChanged = true;
                }
                if (custom::Button(
                        ICON_REFRESH_1_LINE
                            "  Reset position",
                        ImVec2(155.0F, 36.0F)))
                {
                    config.hasWidgetPosition = false;
                    settingsChanged = true;
                }
                ImGui::SameLine();
                if (custom::Button(
                        ICON_PLAY_LINE
                            "  Test 10s",
                        ImVec2(140.0F, 36.0F)))
                {
                    testRequested = true;
                }
                snapshot = config;
            }

            if (startRequested)
            {
                StartCountdownAt(
                    SteadyNowNanoseconds(),
                    "manual");
            }
            if (resetRequested)
            {
                ResetCountdown("menu");
            }
            if (testRequested)
            {
                StartCountdownAt(
                    SteadyNowNanoseconds(),
                    "widget test",
                    kTestCountdownSeconds,
                    false,
                    true);
            }
            if (settingsChanged)
            {
                settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
                UpdateCaptureRequest();
            }
            if (enabledChanged)
            {
                if (snapshot.enabled)
                {
                    std::lock_guard<std::mutex> lock(
                        runtimeMutex);
                    countdownActive = false;
                    detector.Arm();
                    state.store(
                        BombTimerState::armed,
                        std::memory_order_release);
                }
                else
                {
                    ResetCountdown(
                        "disabled",
                        false);
                }
            }

            DrawWidgetPreview(snapshot);

            ImGui::PopStyleVar();
            custom::EndChild();
        }

#if 0
        void RenderWidget(bool menuVisible)
        {
            const double remaining = Remaining();
            if (!IsEnabled() ||
                !IsCountingDown() ||
                remaining <= 0.0)
            {
                std::lock_guard<std::mutex> lock(
                    widgetMutex);
                widgetBounds = {};
                return;
            }

            BombTimerConfig snapshot;
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                snapshot = config;
            }

            const LONG virtualLeft =
                GetSystemMetrics(SM_XVIRTUALSCREEN);
            const LONG virtualTop =
                GetSystemMetrics(SM_YVIRTUALSCREEN);
            const LONG virtualRight =
                virtualLeft +
                GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const LONG virtualBottom =
                virtualTop +
                GetSystemMetrics(SM_CYVIRTUALSCREEN);

            float screenX =
                snapshot.hasWidgetPosition
                ? snapshot.widgetPositionX
                : virtualRight -
                    kWidgetMargin -
                    kWidgetWidth;
            float screenY =
                snapshot.hasWidgetPosition
                ? snapshot.widgetPositionY
                : virtualBottom -
                    kWidgetMargin -
                    kWidgetHeight;
            screenX = std::clamp(
                screenX,
                static_cast<float>(virtualLeft),
                std::max(
                    static_cast<float>(virtualLeft),
                    virtualRight - kWidgetWidth));
            screenY = std::clamp(
                screenY,
                static_cast<float>(virtualTop),
                std::max(
                    static_cast<float>(virtualTop),
                    virtualBottom - kWidgetHeight));

            ImGui::SetNextWindowPos(
                ImVec2(
                    screenX - virtualLeft,
                    screenY - virtualTop),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(kWidgetWidth, kWidgetHeight),
                ImGuiCond_Always);
            ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoBackground |
                ImGuiWindowFlags_NoBringToFrontOnFocus;
            if (!menuVisible)
            {
                flags |=
                    ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_NoMove;
            }

            ImGui::PushStyleVar(
                ImGuiStyleVar_WindowPadding,
                ImVec2(0.0F, 0.0F));
            ImGui::Begin(
                "##vanta-bomb-timer-widget",
                nullptr,
                flags);

            ImVec2 widgetPosition =
                ImGui::GetWindowPos();
            float actualScreenX =
                widgetPosition.x + virtualLeft;
            float actualScreenY =
                widgetPosition.y + virtualTop;
            if (menuVisible &&
                ImGui::IsWindowHovered() &&
                ImGui::IsMouseClicked(
                    ImGuiMouseButton_Left))
            {
                widgetDragging = true;
            }
            if (!ImGui::IsMouseDown(
                    ImGuiMouseButton_Left))
            {
                widgetDragging = false;
            }
            if (menuVisible && widgetDragging)
            {
                actualScreenX +=
                    ImGui::GetIO().MouseDelta.x;
                actualScreenY +=
                    ImGui::GetIO().MouseDelta.y;
            }
            actualScreenX = std::clamp(
                actualScreenX,
                static_cast<float>(virtualLeft),
                std::max(
                    static_cast<float>(virtualLeft),
                    virtualRight - kWidgetWidth));
            actualScreenY = std::clamp(
                actualScreenY,
                static_cast<float>(virtualTop),
                std::max(
                    static_cast<float>(virtualTop),
                    virtualBottom - kWidgetHeight));
            const ImVec2 clampedPosition(
                actualScreenX - virtualLeft,
                actualScreenY - virtualTop);
            if (widgetPosition.x != clampedPosition.x ||
                widgetPosition.y != clampedPosition.y)
            {
                ImGui::SetWindowPos(clampedPosition);
                widgetPosition = clampedPosition;
            }

            const RgbaColor accent =
                remaining <= kWarningSeconds
                ? snapshot.warningColor
                : snapshot.safeColor;
            const float opacity =
                std::clamp(
                    snapshot.widgetOpacity,
                    0.10F,
                    1.0F);
            const ImU32 background =
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(
                        accent.red * 0.18F,
                        accent.green * 0.18F,
                        accent.blue * 0.18F,
                        opacity * 0.90F));
            const ImU32 foreground =
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(
                        accent.red,
                        accent.green,
                        accent.blue,
                        opacity));
            ImDrawList* draw =
                ImGui::GetWindowDrawList();
            const ImVec2 minimum =
                ImGui::GetWindowPos();
            const ImVec2 maximum =
                minimum +
                ImVec2(
                    kWidgetWidth,
                    kWidgetHeight);
            draw->AddRectFilled(
                minimum,
                maximum,
                background,
                25.0F);
            draw->AddRect(
                minimum + ImVec2(1.0F, 1.0F),
                maximum - ImVec2(1.0F, 1.0F),
                foreground,
                24.0F,
                0,
                2.0F);

            char timeText[32]{};
            std::snprintf(
                timeText,
                sizeof(timeText),
                "%.1f",
                remaining);
            if (font::bold_font != nullptr)
            {
                ImGui::PushFont(font::bold_font);
            }
            const ImVec2 textSize =
                ImGui::CalcTextSize(timeText);
            draw->AddText(
                minimum +
                    (ImVec2(
                        kWidgetWidth,
                        kWidgetHeight) -
                     textSize) *
                        0.5F,
                foreground,
                timeText);
            if (font::bold_font != nullptr)
            {
                ImGui::PopFont();
            }

            if (menuVisible &&
                (std::abs(
                    actualScreenX - screenX) > 0.5F ||
                 std::abs(
                    actualScreenY - screenY) > 0.5F))
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                config.hasWidgetPosition = true;
                config.widgetPositionX =
                    actualScreenX;
                config.widgetPositionY =
                    actualScreenY;
                settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }

            {
                std::lock_guard<std::mutex> lock(
                    widgetMutex);
                widgetBounds.visible = true;
                widgetBounds.screenRectangle = {
                    static_cast<LONG>(std::lround(
                        actualScreenX)),
                    static_cast<LONG>(std::lround(
                        actualScreenY)),
                    static_cast<LONG>(std::lround(
                        actualScreenX +
                        kWidgetWidth)),
                    static_cast<LONG>(std::lround(
                        actualScreenY +
                        kWidgetHeight))};
            }

            ImGui::End();
            ImGui::PopStyleVar();
        }
#endif

        BombTimerConfig GetConfig() const
        {
            std::lock_guard<std::mutex> lock(
                settingsMutex);
            return config;
        }

        void ApplyConfig(
            const BombTimerConfig& input,
            bool updateRevision = true)
        {
            BombTimerConfig validated = input;
            const BombTimerConfig detectionDefaults;
            validated.targetColor =
                detectionDefaults.targetColor;
            validated.colorTolerance =
                detectionDefaults.colorTolerance;
            validated.regionLeft =
                detectionDefaults.regionLeft;
            validated.regionTop =
                detectionDefaults.regionTop;
            validated.regionWidth =
                detectionDefaults.regionWidth;
            validated.regionHeight =
                detectionDefaults.regionHeight;
            validated.requiredMatchingPixels =
                detectionDefaults.requiredMatchingPixels;
            validated.requiredConsecutiveFrames =
                detectionDefaults.requiredConsecutiveFrames;
            validated.widgetOpacity =
                std::clamp(
                    validated.widgetOpacity,
                    0.10F,
                    1.0F);
            validated.widgetStyle =
                std::clamp(
                    validated.widgetStyle,
                    0,
                    static_cast<int>(
                        kWidgetStyles.size()) - 1);
            validated.widgetFont =
                std::clamp(
                    validated.widgetFont,
                    0,
                    static_cast<int>(
                        kWidgetFontNames.size()) - 1);
            const auto clampColor =
                [](RgbaColor& styleColor)
                {
                    styleColor.red =
                        std::clamp(
                            styleColor.red,
                            0.0F,
                            1.0F);
                    styleColor.green =
                        std::clamp(
                            styleColor.green,
                            0.0F,
                            1.0F);
                    styleColor.blue =
                        std::clamp(
                            styleColor.blue,
                            0.0F,
                            1.0F);
                    styleColor.alpha = 1.0F;
                };
            clampColor(validated.safeColor);
            clampColor(validated.warningColor);

            bool previouslyEnabled = false;
            {
                std::lock_guard<std::mutex> lock(
                    settingsMutex);
                previouslyEnabled = config.enabled;
                config = validated;
            }
            {
                std::lock_guard<std::mutex> lock(
                    runtimeMutex);
                countdownActive = false;
                countdownStartNanoseconds = 0;
                countdownDurationSeconds =
                    kCountdownSeconds;
                if (validated.enabled)
                {
                    detector.Arm();
                }
                else
                {
                    detector.Disarm();
                }
            }
            state.store(
                validated.enabled
                    ? BombTimerState::armed
                    : BombTimerState::disabled,
                std::memory_order_release);
            if (capture != nullptr)
            {
                UpdateCaptureRequest();
            }
            if (updateRevision ||
                previouslyEnabled !=
                    validated.enabled)
            {
                settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            widgetSurfaceDirty.store(
                true,
                std::memory_order_release);
        }
    };

    bool RunBombTimerSelfTest()
    {
        cv::Mat frame(
            4,
            4,
            CV_8UC4,
            cv::Scalar(0, 0, 0, 255));
        frame.at<cv::Vec4b>(0, 0) =
            cv::Vec4b(0, 0, 170, 255);
        frame.at<cv::Vec4b>(0, 1) =
            cv::Vec4b(30, 30, 140, 255);
        frame.at<cv::Vec4b>(0, 2) =
            cv::Vec4b(0, 0, 171, 255);
        const RgbaColor target{
            170.0F / 255.0F,
            0.0F,
            0.0F,
            1.0F};
        if (CountMatchingPixels(
                frame,
                target,
                30) != 3)
        {
            return false;
        }

        DetectorDebounce detector;
        if (detector.Process(3, 3, 3, 100) ||
            detector.Process(3, 3, 3, 200) ||
            !detector.Process(3, 3, 3, 300) ||
            detector.firstMatchTimestamp != 100)
        {
            return false;
        }
        detector.Process(0, 3, 3, 400);
        detector.Process(0, 3, 3, 500);
        detector.Process(0, 3, 3, 600);
        if (!detector.armed)
        {
            return false;
        }

        if (std::abs(
                RemainingSeconds(
                    1'000'000'000,
                    1'000'000'000,
                    kCountdownSeconds) -
                45.0) > 0.0001 ||
            std::abs(
                RemainingSeconds(
                    1'000'000'000,
                    39'100'000'000,
                    kCountdownSeconds) -
                6.9) > 0.0001 ||
            RemainingSeconds(
                1'000'000'000,
                46'000'000'000,
                kCountdownSeconds) != 0.0 ||
            std::abs(
                RemainingSeconds(
                    1'000'000'000,
                    1'000'000'000,
                    kTestCountdownSeconds) -
                10.0) > 0.0001)
        {
            return false;
        }

        const BombTimerConfig defaults;
        const int left =
            static_cast<int>(std::lround(
                defaults.regionLeft * 1920.0F));
        const int top =
            static_cast<int>(std::lround(
                defaults.regionTop * 1080.0F));
        const int width =
            static_cast<int>(std::lround(
                defaults.regionWidth * 1920.0F));
        const int height =
            static_cast<int>(std::lround(
                defaults.regionHeight * 1080.0F));
        return
            left == 918 &&
            top == 6 &&
            width == 79 &&
            height == 68;
    }

    BombTimerController::BombTimerController()
        : implementation_(
              std::make_unique<Implementation>())
    {
    }

    BombTimerController::~BombTimerController()
    {
        Shutdown();
    }

    void BombTimerController::Initialize(
        CaptureController* capture,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        HWND menuWindow,
        const BombTimerConfig* initialConfig)
    {
        implementation_->Initialize(
            capture,
            device,
            context,
            menuWindow,
            initialConfig);
    }

    void BombTimerController::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }

    void BombTimerController::Tick()
    {
        implementation_->Tick();
    }

    void BombTimerController::RenderPanel()
    {
        implementation_->RenderPanel();
    }

    void BombTimerController::StartManualCountdown()
    {
        implementation_->StartCountdownAt(
            SteadyNowNanoseconds(),
            "manual");
    }

    void BombTimerController::StartTestCountdown()
    {
        implementation_->StartCountdownAt(
            SteadyNowNanoseconds(),
            "widget test",
            kTestCountdownSeconds,
            false,
            true);
    }

    void BombTimerController::ResetCountdown()
    {
        implementation_->ResetCountdown("manual");
    }

    bool BombTimerController::IsWidgetVisible() const noexcept
    {
        std::lock_guard<std::mutex> lock(
            implementation_->widgetMutex);
        return implementation_->
            widgetBounds.visible;
    }

    bool BombTimerController::HasCaptureFrame() const noexcept
    {
        return implementation_->
            receivedCaptureFrame.load(
                std::memory_order_acquire);
    }

    BombTimerState
    BombTimerController::State() const noexcept
    {
        return implementation_->state.load(
            std::memory_order_acquire);
    }

    int BombTimerController::MatchingPixels() const noexcept
    {
        return implementation_->matchingPixels.load(
            std::memory_order_relaxed);
    }

    BombTimerWidgetBounds
    BombTimerController::GetWidgetBounds() const noexcept
    {
        std::lock_guard<std::mutex> lock(
            implementation_->widgetMutex);
        return implementation_->widgetBounds;
    }

    BombTimerConfig BombTimerController::GetConfig() const
    {
        return implementation_->GetConfig();
    }

    void BombTimerController::ApplyConfig(
        const BombTimerConfig& config)
    {
        implementation_->ApplyConfig(config);
    }

    std::uint64_t
    BombTimerController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }
}
