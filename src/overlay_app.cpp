#include "overlay_app.hpp"

#include "capture_controller.hpp"
#include "logger.hpp"
#include "makcu_controller.hpp"
#include "pretty_menu.hpp"
#include "testclick_controller.hpp"
#include "testmove_controller.hpp"

#include <Windows.h>
#include <Windowsx.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include "resource.h"

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

namespace
{
    std::atomic_bool g_externalExitRequested{false};

    struct Options
    {
        std::uint64_t selfTestFrames{};
        int captureTestBackend{};
    };

    bool SameRectangle(const RECT& left, const RECT& right)
    {
        return left.left == right.left &&
            left.top == right.top &&
            left.right == right.right &&
            left.bottom == right.bottom;
    }

    std::optional<std::wstring> ArgumentValue(
        int argumentCount,
        wchar_t** arguments,
        const wchar_t* name)
    {
        for (int index = 1; index + 1 < argumentCount; ++index)
        {
            if (_wcsicmp(arguments[index], name) == 0)
            {
                return arguments[index + 1];
            }
        }
        return std::nullopt;
    }

    bool HasArgument(
        int argumentCount,
        wchar_t** arguments,
        const wchar_t* name)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            if (_wcsicmp(arguments[index], name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    Options ParseOptions(int argumentCount, wchar_t** arguments)
    {
        Options options;
        if (const auto value =
                ArgumentValue(argumentCount, arguments, L"--self-test"))
        {
            options.selfTestFrames =
                std::wcstoull(value->c_str(), nullptr, 10);
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--capture-test-winrt"))
        {
            options.captureTestBackend = 1;
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--capture-test-duplication"))
        {
            options.captureTestBackend = 2;
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--capture-test-winrt-window"))
        {
            options.captureTestBackend = 3;
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--capture-test-duplication-window"))
        {
            options.captureTestBackend = 4;
        }
        return options;
    }

    class OverlayApplication
    {
    public:
        OverlayApplication(HINSTANCE instance, Options options)
            : instance_(instance),
              options_(std::move(options))
        {
        }

        ~OverlayApplication()
        {
            Shutdown();
        }

        int Run()
        {
            if (!CreateOverlayWindow() ||
                !CreateGraphics() ||
                !CreateLogoTexture() ||
                !InitializeImGui() ||
                !capture_.Initialize(
                    device_.Get(),
                    context_.Get(),
                    GetCurrentProcessId()) ||
                !makcu_.Initialize() ||
                !CreateOutlineWindow())
            {
                Shutdown();
                return 1;
            }

            testClick_.Initialize(&capture_, &makcu_);
            testMove_.Initialize(&capture_, &makcu_);

            vanta::log::Info(
                "overlay ready; Insert toggles the menu, End closes vanta");
            vanta::log::Info(
                "mode: always-on-top virtual desktop (all applications)");
            if (options_.captureTestBackend != 0 &&
                !capture_.StartAutomatedCapture(
                    options_.captureTestBackend == 2 ||
                        options_.captureTestBackend == 4,
                    options_.captureTestBackend >= 3))
            {
                vanta::log::Error(
                    "automated capture test could not start");
                Shutdown();
                return 1;
            }

            while (!exitRequested_)
            {
                if (g_externalExitRequested.load(
                        std::memory_order_acquire))
                {
                    RequestExit(
                        "console or system exit requested");
                }

                MSG message{};
                while (PeekMessageW(
                    &message,
                    nullptr,
                    0,
                    0,
                    PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                    if (message.message == WM_QUIT)
                    {
                        RequestExit("WM_QUIT received");
                    }
                }
                if (exitRequested_)
                {
                    break;
                }

                HandleHotkeys();
                UpdateDesktopBounds();
                ResizeIfNeeded();
                capture_.Tick();
                makcu_.Tick();
                testClick_.Tick();
                testMove_.Tick();
                UpdateCaptureOutline();

                if (overlayVisible_)
                {
                    RenderFrame();
                }

                if (options_.selfTestFrames != 0 &&
                    renderedFrames_ >= options_.selfTestFrames)
                {
                    vanta::log::Info(
                        "self-test rendered %llu frames",
                        static_cast<unsigned long long>(renderedFrames_));
                    RequestExit("self-test completed");
                }
            }

            const bool selfTestPassed =
                options_.selfTestFrames == 0 ||
                renderedFrames_ >= options_.selfTestFrames;
            Shutdown();
            return selfTestPassed ? 0 : 1;
        }

        LRESULT WindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            if (imguiReady_ &&
                vanta::menu::IsVisible() &&
                ImGui_ImplWin32_WndProcHandler(
                    window,
                    message,
                    wParam,
                    lParam))
            {
                return TRUE;
            }

            switch (message)
            {
            case WM_NCHITTEST:
            {
                if (!vanta::menu::IsVisible())
                {
                    return HTTRANSPARENT;
                }
                POINT point{
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)};
                ScreenToClient(window, &point);
                return vanta::menu::ContainsPoint(
                           static_cast<float>(point.x),
                           static_cast<float>(point.y))
                    ? HTCLIENT
                    : HTTRANSPARENT;
            }
            case WM_MOUSEACTIVATE:
                return vanta::menu::IsVisible()
                    ? MA_ACTIVATE
                    : MA_NOACTIVATE;
            case WM_SIZE:
                if (wParam != SIZE_MINIMIZED)
                {
                    pendingWidth_ = LOWORD(lParam);
                    pendingHeight_ = HIWORD(lParam);
                }
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_CLOSE:
                RequestExit("overlay window close requested");
                return 0;
            case WM_DESTROY:
                if (!shutdownStarted_)
                {
                    RequestExit(
                        "overlay window was destroyed");
                }
                PostQuitMessage(0);
                return 0;
            case WM_QUERYENDSESSION:
                RequestExit(
                    "Windows session shutdown requested");
                return TRUE;
            default:
                return DefWindowProcW(
                    window,
                    message,
                    wParam,
                    lParam);
            }
        }

    private:
        bool CreateOverlayWindow()
        {
            SetProcessDpiAwarenessContext(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

            WNDCLASSEXW windowClass{
                sizeof(WNDCLASSEXW),
                CS_CLASSDC,
                &StaticWindowProcedure,
                0,
                0,
                instance_,
                LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VANTA)),
                LoadCursorW(nullptr, IDC_ARROW),
                nullptr,
                nullptr,
                L"VantaExternalOverlay",
                LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VANTA))};
            if (RegisterClassExW(&windowClass) == 0)
            {
                vanta::log::Error(
                    "RegisterClassExW failed: %lu",
                    GetLastError());
                return false;
            }
            classRegistered_ = true;

            window_ = CreateWindowExW(
                WS_EX_TOPMOST |
                    WS_EX_TOOLWINDOW |
                    WS_EX_NOREDIRECTIONBITMAP,
                windowClass.lpszClassName,
                L"vanta overlay",
                WS_POPUP,
                0,
                0,
                1280,
                720,
                nullptr,
                nullptr,
                instance_,
                this);
            if (window_ == nullptr)
            {
                vanta::log::Error(
                    "CreateWindowExW failed: %lu",
                    GetLastError());
                return false;
            }
            SetWindowDisplayAffinity(
                window_,
                WDA_EXCLUDEFROMCAPTURE);
            return true;
        }

        bool CreateGraphics()
        {
            constexpr D3D_FEATURE_LEVEL levels[]{
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0};

            D3D_FEATURE_LEVEL selected{};
            HRESULT result = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                &device_,
                &selected,
                &context_);
            if (FAILED(result))
            {
                vanta::log::Warning(
                    "hardware D3D11 device failed (0x%08lX); trying WARP",
                    static_cast<unsigned long>(result));
                result = D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    levels,
                    static_cast<UINT>(std::size(levels)),
                    D3D11_SDK_VERSION,
                    &device_,
                    &selected,
                    &context_);
            }
            if (FAILED(result))
            {
                vanta::log::Error(
                    "D3D11CreateDevice failed: 0x%08lX",
                    static_cast<unsigned long>(result));
                return false;
            }

            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            if (FAILED(device_.As(&dxgiDevice)) ||
                FAILED(dxgiDevice->GetAdapter(&adapter)) ||
                FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            {
                vanta::log::Error("could not acquire DXGI factory");
                return false;
            }

            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = 1280;
            description.Height = 720;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

            ComPtr<IDXGISwapChain1> swapChain1;
            result = factory->CreateSwapChainForComposition(
                device_.Get(),
                &description,
                nullptr,
                &swapChain1);
            if (FAILED(result))
            {
                vanta::log::Error(
                    "CreateSwapChainForComposition failed: 0x%08lX",
                    static_cast<unsigned long>(result));
                return false;
            }
            if (FAILED(swapChain1.As(&swapChain_)))
            {
                vanta::log::Error(
                    "IDXGISwapChain2 is unavailable");
                return false;
            }
            swapChain_->SetMaximumFrameLatency(1);

            result = DCompositionCreateDevice(
                dxgiDevice.Get(),
                IID_PPV_ARGS(&compositionDevice_));
            if (SUCCEEDED(result))
            {
                result = compositionDevice_->CreateTargetForHwnd(
                    window_,
                    TRUE,
                    &compositionTarget_);
            }
            if (SUCCEEDED(result))
            {
                result = compositionDevice_->CreateVisual(
                    &compositionVisual_);
            }
            if (SUCCEEDED(result))
            {
                result = compositionVisual_->SetContent(
                    swapChain_.Get());
            }
            if (SUCCEEDED(result))
            {
                result = compositionTarget_->SetRoot(
                    compositionVisual_.Get());
            }
            if (SUCCEEDED(result))
            {
                result = compositionDevice_->Commit();
            }
            if (FAILED(result))
            {
                vanta::log::Error(
                    "DirectComposition initialization failed: 0x%08lX",
                    static_cast<unsigned long>(result));
                return false;
            }

            if (!CreateRenderTarget())
            {
                return false;
            }

            vanta::log::Info(
                "D3D11 DirectComposition surface initialized");
            return true;
        }

        bool CreateOutlineWindow()
        {
            WNDCLASSEXW windowClass{
                sizeof(WNDCLASSEXW),
                0,
                &StaticOutlineWindowProcedure,
                0,
                0,
                instance_,
                nullptr,
                LoadCursorW(nullptr, IDC_ARROW),
                nullptr,
                nullptr,
                L"VantaCaptureOutline",
                nullptr};
            if (RegisterClassExW(&windowClass) == 0)
            {
                vanta::log::Error(
                    "outline RegisterClassExW failed: %lu",
                    GetLastError());
                return false;
            }
            outlineClassRegistered_ = true;

            outlineWindow_ = CreateWindowExW(
                WS_EX_TOPMOST |
                    WS_EX_TOOLWINDOW |
                    WS_EX_LAYERED |
                    WS_EX_TRANSPARENT |
                    WS_EX_NOACTIVATE,
                windowClass.lpszClassName,
                L"vanta capture region",
                WS_POPUP,
                0,
                0,
                1,
                1,
                nullptr,
                nullptr,
                instance_,
                this);
            if (outlineWindow_ == nullptr)
            {
                vanta::log::Error(
                    "capture outline CreateWindowExW failed: %lu",
                    GetLastError());
                return false;
            }

            SetLayeredWindowAttributes(
                outlineWindow_,
                outlineTransparencyKey_,
                255,
                LWA_COLORKEY | LWA_ALPHA);
            SetWindowDisplayAffinity(
                outlineWindow_,
                WDA_EXCLUDEFROMCAPTURE);
            return true;
        }

        bool CreateRenderTarget()
        {
            ComPtr<ID3D11Texture2D> backBuffer;
            HRESULT result = swapChain_->GetBuffer(
                0,
                IID_PPV_ARGS(&backBuffer));
            if (SUCCEEDED(result))
            {
                result = device_->CreateRenderTargetView(
                    backBuffer.Get(),
                    nullptr,
                    &renderTarget_);
            }
            if (FAILED(result))
            {
                vanta::log::Error(
                    "render-target creation failed: 0x%08lX",
                    static_cast<unsigned long>(result));
                return false;
            }
            return true;
        }

        bool CreateLogoTexture()
        {
            constexpr int logoSize = 64;
            HICON icon = static_cast<HICON>(
                LoadImageW(
                    instance_,
                    MAKEINTRESOURCEW(IDI_VANTA),
                    IMAGE_ICON,
                    logoSize,
                    logoSize,
                    LR_DEFAULTCOLOR));
            if (icon == nullptr)
            {
                vanta::log::Error(
                    "could not load the embedded Vanta logo: %lu",
                    GetLastError());
                return false;
            }

            HDC deviceContext =
                CreateCompatibleDC(nullptr);
            BITMAPINFO bitmapInformation{};
            bitmapInformation.bmiHeader.biSize =
                sizeof(BITMAPINFOHEADER);
            bitmapInformation.bmiHeader.biWidth =
                logoSize;
            bitmapInformation.bmiHeader.biHeight =
                -logoSize;
            bitmapInformation.bmiHeader.biPlanes = 1;
            bitmapInformation.bmiHeader.biBitCount = 32;
            bitmapInformation.bmiHeader.biCompression =
                BI_RGB;

            void* pixels = nullptr;
            HBITMAP bitmap = deviceContext != nullptr
                ? CreateDIBSection(
                    deviceContext,
                    &bitmapInformation,
                    DIB_RGB_COLORS,
                    &pixels,
                    nullptr,
                    0)
                : nullptr;
            HGDIOBJ previousObject =
                bitmap != nullptr
                ? SelectObject(deviceContext, bitmap)
                : nullptr;

            const auto releaseGdiResources = [&]()
            {
                if (previousObject != nullptr &&
                    previousObject != HGDI_ERROR)
                {
                    SelectObject(
                        deviceContext,
                        previousObject);
                }
                if (bitmap != nullptr)
                {
                    DeleteObject(bitmap);
                }
                if (deviceContext != nullptr)
                {
                    DeleteDC(deviceContext);
                }
                DestroyIcon(icon);
            };

            if (deviceContext == nullptr ||
                bitmap == nullptr ||
                pixels == nullptr ||
                previousObject == nullptr ||
                previousObject == HGDI_ERROR)
            {
                releaseGdiResources();
                vanta::log::Error(
                    "could not create the Vanta logo bitmap");
                return false;
            }

            auto* bytes =
                static_cast<std::uint8_t*>(pixels);
            std::fill_n(
                bytes,
                static_cast<std::size_t>(
                    logoSize * logoSize * 4),
                std::uint8_t{0});
            if (!DrawIconEx(
                    deviceContext,
                    0,
                    0,
                    icon,
                    logoSize,
                    logoSize,
                    0,
                    nullptr,
                    DI_NORMAL))
            {
                releaseGdiResources();
                vanta::log::Error(
                    "could not rasterize the embedded Vanta logo");
                return false;
            }

            bool hasAlpha = false;
            for (int pixel = 0;
                 pixel < logoSize * logoSize;
                 ++pixel)
            {
                if (bytes[pixel * 4 + 3] != 0)
                {
                    hasAlpha = true;
                    break;
                }
            }
            if (!hasAlpha)
            {
                for (int pixel = 0;
                     pixel < logoSize * logoSize;
                     ++pixel)
                {
                    const int offset = pixel * 4;
                    bytes[offset + 3] = std::max({
                        bytes[offset],
                        bytes[offset + 1],
                        bytes[offset + 2]});
                }
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width = logoSize;
            description.Height = logoSize;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format =
                DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage =
                D3D11_USAGE_IMMUTABLE;
            description.BindFlags =
                D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = pixels;
            initialData.SysMemPitch = logoSize * 4;

            ComPtr<ID3D11Texture2D> texture;
            HRESULT result = device_->CreateTexture2D(
                &description,
                &initialData,
                &texture);
            if (SUCCEEDED(result))
            {
                result =
                    device_->CreateShaderResourceView(
                        texture.Get(),
                        nullptr,
                        &logoShaderResource_);
            }
            releaseGdiResources();
            if (FAILED(result))
            {
                vanta::log::Error(
                    "could not create the Vanta logo texture: 0x%08lX",
                    static_cast<unsigned long>(result));
                return false;
            }

            vanta::log::Info(
                "embedded Vanta logo texture initialized");
            return true;
        }

        bool InitializeImGui()
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            imguiContextReady_ = true;
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

            vanta::menu::ApplyStyle();
            if (!vanta::menu::InitializeFonts())
            {
                vanta::log::Error(
                    "ImGui font initialization failed");
                return false;
            }
            if (!ImGui_ImplWin32_Init(window_))
            {
                vanta::log::Error(
                    "ImGui Win32 backend initialization failed");
                return false;
            }
            imguiWin32Ready_ = true;
            if (!ImGui_ImplDX11_Init(
                    device_.Get(),
                    context_.Get()))
            {
                vanta::log::Error(
                    "ImGui D3D11 backend initialization failed");
                return false;
            }
            imguiDx11Ready_ = true;

            imguiReady_ = true;
            vanta::log::Info(
                "modified ImGui + FreeType renderer initialized");
            return true;
        }

        void RequestExit(const char* reason)
        {
            if (exitRequested_)
            {
                return;
            }
            exitRequested_ = true;
            vanta::log::Info(
                "graceful exit requested: %s",
                reason);
        }

        void HandleHotkeys()
        {
            const bool insertDown =
                (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
            if (insertDown && !insertWasDown_)
            {
                vanta::menu::Toggle();
            }
            insertWasDown_ = insertDown;

            const bool endDown =
                (GetAsyncKeyState(VK_END) & 0x8000) != 0;
            if (endDown && !endWasDown_)
            {
                RequestExit("End hotkey pressed");
            }
            endWasDown_ = endDown;
        }

        void UpdateDesktopBounds()
        {
            const bool menuVisible = vanta::menu::IsVisible();
            SetClickThrough(!menuVisible);
            if (!menuVisible)
            {
                ClearMenuRegion();
                if (overlayVisible_)
                {
                    ShowWindow(window_, SW_HIDE);
                    overlayVisible_ = false;
                }
                return;
            }

            const RECT desired{
                GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CYVIRTUALSCREEN)};

            if (desired.right > desired.left &&
                desired.bottom > desired.top)
            {
                const bool boundsChanged =
                    !SameRectangle(desired, overlayRectangle_);
                const ULONGLONG now = GetTickCount64();
                if (boundsChanged ||
                    !overlayVisible_ ||
                    now >= nextTopmostRefresh_)
                {
                    overlayRectangle_ = desired;
                    UINT flags =
                        SWP_NOACTIVATE |
                        SWP_SHOWWINDOW |
                        SWP_NOOWNERZORDER;
                    if (!boundsChanged)
                    {
                        flags |= SWP_NOMOVE | SWP_NOSIZE;
                    }
                    if (!SetWindowPos(
                        window_,
                        HWND_TOPMOST,
                        desired.left,
                        desired.top,
                        desired.right - desired.left,
                        desired.bottom - desired.top,
                        flags))
                    {
                        vanta::log::Warning(
                            "could not refresh topmost overlay state: %lu",
                            GetLastError());
                    }
                    nextTopmostRefresh_ = now + 500;
                }
                overlayVisible_ = true;
            }
        }

        void SetClickThrough(bool enabled)
        {
            if (clickThrough_ == enabled)
            {
                return;
            }

            LONG_PTR extendedStyle =
                GetWindowLongPtrW(window_, GWL_EXSTYLE);
            if (enabled)
            {
                extendedStyle |=
                    WS_EX_TRANSPARENT |
                    WS_EX_NOACTIVATE;
            }
            else
            {
                extendedStyle &=
                    ~(static_cast<LONG_PTR>(
                        WS_EX_TRANSPARENT |
                        WS_EX_NOACTIVATE));
            }

            SetLastError(ERROR_SUCCESS);
            if (SetWindowLongPtrW(
                    window_,
                    GWL_EXSTYLE,
                    extendedStyle) == 0 &&
                GetLastError() != ERROR_SUCCESS)
            {
                vanta::log::Warning(
                    "could not update click-through style: %lu",
                    GetLastError());
                return;
            }

            SetWindowPos(
                window_,
                nullptr,
                0,
                0,
                0,
                0,
                SWP_NOMOVE |
                    SWP_NOSIZE |
                    SWP_NOZORDER |
                    SWP_NOACTIVATE |
                    SWP_FRAMECHANGED);
            clickThrough_ = enabled;
        }

        void ClearMenuRegion()
        {
            if (!menuRegionApplied_)
            {
                return;
            }

            HRGN emptyRegion = CreateRectRgn(0, 0, 0, 0);
            if (emptyRegion == nullptr)
            {
                return;
            }
            if (SetWindowRgn(window_, emptyRegion, TRUE) == 0)
            {
                DeleteObject(emptyRegion);
                return;
            }

            menuRegionRectangle_ = {};
            menuRegionApplied_ = false;
        }

        void UpdateMenuRegion()
        {
            float x = 0.0F;
            float y = 0.0F;
            float width = 0.0F;
            float height = 0.0F;
            if (!vanta::menu::GetBounds(
                    x,
                    y,
                    width,
                    height))
            {
                SetClickThrough(true);
                ClearMenuRegion();
                return;
            }

            const RECT desired{
                static_cast<LONG>(std::floor(x)),
                static_cast<LONG>(std::floor(y)),
                static_cast<LONG>(std::ceil(x + width)),
                static_cast<LONG>(std::ceil(y + height))};
            if (menuRegionApplied_ &&
                SameRectangle(
                    desired,
                    menuRegionRectangle_))
            {
                return;
            }

            HRGN menuRegion = CreateRoundRectRgn(
                desired.left,
                desired.top,
                desired.right + 1,
                desired.bottom + 1,
                24,
                24);
            if (menuRegion == nullptr)
            {
                vanta::log::Warning(
                    "could not create the menu input region");
                return;
            }
            if (SetWindowRgn(window_, menuRegion, TRUE) == 0)
            {
                DeleteObject(menuRegion);
                vanta::log::Warning(
                    "could not apply the menu input region: %lu",
                    GetLastError());
                return;
            }

            menuRegionRectangle_ = desired;
            menuRegionApplied_ = true;
        }

        void UpdateCaptureOutline()
        {
            if (outlineWindow_ == nullptr)
            {
                return;
            }

            const vanta::CaptureOutline outline =
                capture_.GetOutline();
            const bool valid =
                outline.visible &&
                outline.screenRectangle.right >
                    outline.screenRectangle.left &&
                outline.screenRectangle.bottom >
                    outline.screenRectangle.top;
            if (!valid)
            {
                if (outlineVisible_)
                {
                    ShowWindow(outlineWindow_, SW_HIDE);
                    outlineVisible_ = false;
                }
                return;
            }

            const auto channel = [](float value)
            {
                return static_cast<BYTE>(std::lround(
                    std::clamp(value, 0.0F, 1.0F) *
                    255.0F));
            };
            COLORREF color = RGB(
                channel(outline.color[0]),
                channel(outline.color[1]),
                channel(outline.color[2]));
            if (color == outlineTransparencyKey_)
            {
                color = RGB(2, 2, 4);
            }
            const BYTE alpha = channel(outline.color[3]);
            const int thickness =
                std::clamp(outline.thickness, 1, 10);
            const bool colorOrAlphaChanged =
                color != outlineColor_ ||
                alpha != outlineAlpha_;
            const bool thicknessChanged =
                thickness != outlineThickness_;
            if (colorOrAlphaChanged)
            {
                outlineColor_ = color;
                outlineAlpha_ = alpha;
                SetLayeredWindowAttributes(
                    outlineWindow_,
                    outlineTransparencyKey_,
                    outlineAlpha_,
                    LWA_COLORKEY | LWA_ALPHA);
            }
            if (thicknessChanged)
            {
                outlineThickness_ = thickness;
            }
            if (colorOrAlphaChanged ||
                thicknessChanged)
            {
                InvalidateRect(outlineWindow_, nullptr, TRUE);
            }

            const bool rectangleChanged =
                !SameRectangle(
                    outline.screenRectangle,
                    outlineRectangle_);
            const ULONGLONG now = GetTickCount64();
            if (rectangleChanged ||
                !outlineVisible_ ||
                now >= nextOutlineZOrderRefresh_)
            {
                outlineRectangle_ =
                    outline.screenRectangle;
                UINT flags =
                    SWP_NOACTIVATE |
                    SWP_SHOWWINDOW |
                    SWP_NOOWNERZORDER;
                if (!rectangleChanged)
                {
                    flags |= SWP_NOMOVE | SWP_NOSIZE;
                }
                SetWindowPos(
                    outlineWindow_,
                    vanta::menu::IsVisible() &&
                            IsWindowVisible(window_)
                        ? window_
                        : HWND_TOPMOST,
                    outlineRectangle_.left,
                    outlineRectangle_.top,
                    outlineRectangle_.right -
                        outlineRectangle_.left,
                    outlineRectangle_.bottom -
                        outlineRectangle_.top,
                    flags);
                nextOutlineZOrderRefresh_ = now + 500;
            }
            outlineVisible_ = true;
        }

        LRESULT OutlineWindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            switch (message)
            {
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                HDC deviceContext =
                    BeginPaint(window, &paint);
                RECT client{};
                GetClientRect(window, &client);

                HBRUSH transparentBrush =
                    CreateSolidBrush(
                        outlineTransparencyKey_);
                FillRect(
                    deviceContext,
                    &client,
                    transparentBrush);
                DeleteObject(transparentBrush);

                const auto fillFrame = [](
                    HDC target,
                    const RECT& rectangle,
                    LONG requestedThickness,
                    HBRUSH brush)
                {
                    const LONG width =
                        rectangle.right -
                        rectangle.left;
                    const LONG height =
                        rectangle.bottom -
                        rectangle.top;
                    if (width <= 0 ||
                        height <= 0 ||
                        requestedThickness <= 0)
                    {
                        return;
                    }

                    const LONG thickness =
                        std::min(
                            requestedThickness,
                            std::max<LONG>(
                                1,
                                std::min(width, height)));
                    const RECT top{
                        rectangle.left,
                        rectangle.top,
                        rectangle.right,
                        std::min(
                            rectangle.bottom,
                            rectangle.top + thickness)};
                    const RECT bottom{
                        rectangle.left,
                        std::max(
                            rectangle.top,
                            rectangle.bottom - thickness),
                        rectangle.right,
                        rectangle.bottom};
                    const RECT left{
                        rectangle.left,
                        rectangle.top,
                        std::min(
                            rectangle.right,
                            rectangle.left + thickness),
                        rectangle.bottom};
                    const RECT right{
                        std::max(
                            rectangle.left,
                            rectangle.right - thickness),
                        rectangle.top,
                        rectangle.right,
                        rectangle.bottom};

                    FillRect(target, &top, brush);
                    FillRect(target, &bottom, brush);
                    FillRect(target, &left, brush);
                    FillRect(target, &right, brush);
                };

                HBRUSH blackBrush =
                    CreateSolidBrush(RGB(0, 0, 0));
                fillFrame(
                    deviceContext,
                    client,
                    outlineThickness_ + 2,
                    blackBrush);
                DeleteObject(blackBrush);

                const RECT colorRectangle{
                    client.left + 1,
                    client.top + 1,
                    client.right - 1,
                    client.bottom - 1};
                HBRUSH outlineBrush =
                    CreateSolidBrush(outlineColor_);
                fillFrame(
                    deviceContext,
                    colorRectangle,
                    outlineThickness_,
                    outlineBrush);
                DeleteObject(outlineBrush);
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

        void ResizeIfNeeded()
        {
            if (pendingWidth_ == 0 ||
                pendingHeight_ == 0 ||
                (pendingWidth_ == swapChainWidth_ &&
                 pendingHeight_ == swapChainHeight_))
            {
                return;
            }

            renderTarget_.Reset();
            const HRESULT result = swapChain_->ResizeBuffers(
                0,
                pendingWidth_,
                pendingHeight_,
                DXGI_FORMAT_UNKNOWN,
                0);
            if (FAILED(result) || !CreateRenderTarget())
            {
                vanta::log::Error(
                    "swap-chain resize failed: 0x%08lX",
                    static_cast<unsigned long>(result));
                return;
            }

            swapChainWidth_ = pendingWidth_;
            swapChainHeight_ = pendingHeight_;
        }

        void RenderFrame()
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            vanta::menu::Render(
                "Always-on-top virtual desktop | all applications",
                window_,
                capture_,
                makcu_,
                testClick_,
                testMove_,
                logoShaderResource_.Get());
            UpdateMenuRegion();

            ImGui::Render();
            constexpr float clearColor[]{0.0F, 0.0F, 0.0F, 0.0F};
            ID3D11RenderTargetView* target = renderTarget_.Get();
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->ClearRenderTargetView(target, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            const HRESULT result = swapChain_->Present(0, 0);
            if (FAILED(result))
            {
                vanta::log::Warning(
                    "Present failed: 0x%08lX",
                    static_cast<unsigned long>(result));
            }
            else
            {
                ++renderedFrames_;
            }
        }

        void Shutdown()
        {
            if (shutdownStarted_)
            {
                return;
            }
            shutdownStarted_ = true;
            exitRequested_ = true;
            vanta::log::Info(
                "graceful shutdown started");

            if (window_ != nullptr &&
                IsWindow(window_))
            {
                ShowWindow(window_, SW_HIDE);
            }
            if (outlineWindow_ != nullptr &&
                IsWindow(outlineWindow_))
            {
                ShowWindow(outlineWindow_, SW_HIDE);
            }

            testClick_.Shutdown();
            testMove_.Shutdown();
            capture_.Shutdown();
            makcu_.Shutdown();

            if (outlineWindow_ != nullptr &&
                IsWindow(outlineWindow_))
            {
                DestroyWindow(outlineWindow_);
                outlineWindow_ = nullptr;
            }

            if (imguiDx11Ready_)
            {
                ImGui_ImplDX11_Shutdown();
                imguiDx11Ready_ = false;
            }
            if (imguiWin32Ready_)
            {
                ImGui_ImplWin32_Shutdown();
                imguiWin32Ready_ = false;
            }
            if (imguiContextReady_)
            {
                ImGui::DestroyContext();
                imguiContextReady_ = false;
            }
            imguiReady_ = false;

            logoShaderResource_.Reset();
            renderTarget_.Reset();
            if (compositionTarget_ != nullptr)
            {
                compositionTarget_->SetRoot(nullptr);
            }
            if (compositionVisual_ != nullptr)
            {
                compositionVisual_->SetContent(nullptr);
            }
            if (compositionDevice_ != nullptr)
            {
                compositionDevice_->Commit();
            }
            compositionVisual_.Reset();
            compositionTarget_.Reset();
            compositionDevice_.Reset();
            swapChain_.Reset();
            if (context_ != nullptr)
            {
                context_->ClearState();
                context_->Flush();
            }
            context_.Reset();
            device_.Reset();

            if (window_ != nullptr && IsWindow(window_))
            {
                DestroyWindow(window_);
                window_ = nullptr;
            }
            if (outlineClassRegistered_)
            {
                if (!UnregisterClassW(
                        L"VantaCaptureOutline",
                        instance_))
                {
                    vanta::log::Warning(
                        "could not unregister the capture-outline class: %lu",
                        GetLastError());
                }
                outlineClassRegistered_ = false;
            }
            if (classRegistered_)
            {
                if (!UnregisterClassW(
                        L"VantaExternalOverlay",
                        instance_))
                {
                    vanta::log::Warning(
                        "could not unregister the overlay class: %lu",
                        GetLastError());
                }
                classRegistered_ = false;
            }
            vanta::log::Info(
                "graceful shutdown completed");
        }

        static LRESULT CALLBACK StaticWindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            auto* application =
                reinterpret_cast<OverlayApplication*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto* create =
                    reinterpret_cast<CREATESTRUCTW*>(lParam);
                application =
                    static_cast<OverlayApplication*>(
                        create->lpCreateParams);
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(application));
            }

            if (application != nullptr)
            {
                return application->WindowProcedure(
                    window,
                    message,
                    wParam,
                    lParam);
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        static LRESULT CALLBACK StaticOutlineWindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            auto* application =
                reinterpret_cast<OverlayApplication*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto* create =
                    reinterpret_cast<CREATESTRUCTW*>(lParam);
                application =
                    static_cast<OverlayApplication*>(
                        create->lpCreateParams);
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(application));
            }
            if (application != nullptr)
            {
                return application->OutlineWindowProcedure(
                    window,
                    message,
                    wParam,
                    lParam);
            }
            return DefWindowProcW(
                window,
                message,
                wParam,
                lParam);
        }

        HINSTANCE instance_{};
        Options options_;
        vanta::CaptureController capture_;
        vanta::MakcuController makcu_;
        vanta::TestClickController testClick_;
        vanta::TestMoveController testMove_;
        HWND window_{};
        HWND outlineWindow_{};
        bool classRegistered_ = false;
        bool outlineClassRegistered_ = false;
        bool exitRequested_ = false;
        bool shutdownStarted_ = false;
        bool imguiContextReady_ = false;
        bool imguiWin32Ready_ = false;
        bool imguiDx11Ready_ = false;
        bool imguiReady_ = false;
        bool overlayVisible_ = false;
        bool clickThrough_ = false;
        bool menuRegionApplied_ = false;
        bool outlineVisible_ = false;
        bool insertWasDown_ = false;
        bool endWasDown_ = false;
        UINT pendingWidth_ = 1280;
        UINT pendingHeight_ = 720;
        UINT swapChainWidth_ = 1280;
        UINT swapChainHeight_ = 720;
        std::uint64_t renderedFrames_{};
        ULONGLONG nextTopmostRefresh_{};
        ULONGLONG nextOutlineZOrderRefresh_{};
        RECT overlayRectangle_{};
        RECT menuRegionRectangle_{};
        RECT outlineRectangle_{};
        static constexpr COLORREF outlineTransparencyKey_ =
            RGB(1, 2, 3);
        COLORREF outlineColor_ = RGB(173, 143, 233);
        BYTE outlineAlpha_ = 255;
        int outlineThickness_ = 1;

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11ShaderResourceView>
            logoShaderResource_;
        ComPtr<IDXGISwapChain2> swapChain_;
        ComPtr<ID3D11RenderTargetView> renderTarget_;
        ComPtr<IDCompositionDevice> compositionDevice_;
        ComPtr<IDCompositionTarget> compositionTarget_;
        ComPtr<IDCompositionVisual> compositionVisual_;
    };
}

namespace vanta
{
    void RequestOverlayExit() noexcept
    {
        g_externalExitRequested.store(
            true,
            std::memory_order_release);
    }

    int RunOverlay(
        HINSTANCE instance,
        int argumentCount,
        wchar_t** arguments)
    {
        if (HasArgument(argumentCount, arguments, L"--help"))
        {
            std::printf(
                "vanta external overlay\n"
                "  vanta.exe                 always-on-top desktop overlay\n"
                "  vanta.exe --self-test N   render N frames and exit\n"
                "  --capture-test-winrt      test WinRT monitor capture\n"
                "  --capture-test-duplication test DXGI monitor capture\n"
                "  --capture-test-winrt-window test WinRT window capture\n"
                "  --capture-test-duplication-window test DXGI window crop\n");
            return 0;
        }

        return OverlayApplication(
            instance,
            ParseOptions(argumentCount, arguments))
            .Run();
    }
}
