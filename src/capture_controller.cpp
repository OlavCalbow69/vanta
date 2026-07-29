#define IMGUI_DEFINE_MATH_OPERATORS

#include "capture_controller.hpp"

#include "color_targets.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "imgui.h"
#include "custom_widgets.hpp"
#include "font_defines.h"
#include "imgui_settings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    enum class CaptureBackendKind
    {
        windowsGraphicsCapture,
        desktopDuplication
    };

    enum class CaptureSourceKind
    {
        window,
        monitor
    };

    struct WindowEntry
    {
        HWND window{};
        std::string label;
    };

    struct MonitorEntry
    {
        HMONITOR monitor{};
        RECT rectangle{};
        std::string label;
    };

    struct CaptureSettings
    {
        CaptureBackendKind backend{};
        CaptureSourceKind source{};
        HWND window{};
        HMONITOR monitor{};
    };

    struct CapturedFrame
    {
        cv::Mat bgra;
        RECT screenRectangle{};
        int sourceWidth{};
        int sourceHeight{};
        bool centeredRegionApplied{};
    };

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 0)
        {
            return {};
        }

        std::string result(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            required,
            nullptr,
            nullptr);
        return result;
    }

    std::wstring WindowTitle(HWND window)
    {
        const int length = GetWindowTextLengthW(window);
        if (length <= 0)
        {
            return {};
        }

        std::wstring result(
            static_cast<std::size_t>(length + 1),
            L'\0');
        GetWindowTextW(window, result.data(), length + 1);
        result.resize(static_cast<std::size_t>(length));
        return result;
    }

    std::wstring ProcessImageName(DWORD processId)
    {
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);
        if (process == nullptr)
        {
            return {};
        }

        wchar_t path[32768]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        const BOOL success =
            QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        if (!success)
        {
            return {};
        }

        const wchar_t* filename = path;
        for (DWORD index = 0; index < length; ++index)
        {
            if (path[index] == L'\\' || path[index] == L'/')
            {
                filename = path + index + 1;
            }
        }
        return filename;
    }

    bool IsCapturableWindow(HWND window, DWORD ownProcessId)
    {
        if (window == nullptr ||
            window == GetShellWindow() ||
            window == GetDesktopWindow() ||
            !IsWindowVisible(window) ||
            IsIconic(window) ||
            WindowTitle(window).empty())
        {
            return false;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == 0 || processId == ownProcessId)
        {
            return false;
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(
                window,
                DWMWA_CLOAKED,
                &cloaked,
                sizeof(cloaked))) &&
            cloaked != 0)
        {
            return false;
        }
        return true;
    }

    struct WindowEnumeration
    {
        DWORD ownProcessId{};
        std::vector<WindowEntry>* windows{};
    };

    BOOL CALLBACK EnumerateWindowCallback(
        HWND window,
        LPARAM parameter)
    {
        auto& enumeration =
            *reinterpret_cast<WindowEnumeration*>(parameter);
        if (!IsCapturableWindow(
                window,
                enumeration.ownProcessId))
        {
            return TRUE;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        const std::wstring title = WindowTitle(window);
        const std::wstring process = ProcessImageName(processId);

        WindowEntry entry;
        entry.window = window;
        entry.label = WideToUtf8(title);
        if (!process.empty())
        {
            entry.label += "  [" + WideToUtf8(process) + "]";
        }
        enumeration.windows->push_back(std::move(entry));
        return TRUE;
    }

    std::vector<WindowEntry> EnumerateWindows(DWORD ownProcessId)
    {
        std::vector<WindowEntry> result;
        WindowEnumeration enumeration{
            ownProcessId,
            &result};
        EnumWindows(
            &EnumerateWindowCallback,
            reinterpret_cast<LPARAM>(&enumeration));
        std::sort(
            result.begin(),
            result.end(),
            [](const WindowEntry& left, const WindowEntry& right)
            {
                return left.label < right.label;
            });
        return result;
    }

    BOOL CALLBACK EnumerateMonitorCallback(
        HMONITOR monitor,
        HDC,
        LPRECT,
        LPARAM parameter)
    {
        auto& monitors =
            *reinterpret_cast<std::vector<MonitorEntry>*>(parameter);

        MONITORINFOEXW information{};
        information.cbSize = sizeof(information);
        if (!GetMonitorInfoW(
                monitor,
                reinterpret_cast<MONITORINFO*>(&information)))
        {
            return TRUE;
        }

        MonitorEntry entry;
        entry.monitor = monitor;
        entry.rectangle = information.rcMonitor;
        entry.label =
            WideToUtf8(information.szDevice) +
            "  (" +
            std::to_string(
                information.rcMonitor.right -
                information.rcMonitor.left) +
            "x" +
            std::to_string(
                information.rcMonitor.bottom -
                information.rcMonitor.top) +
            ")";
        if ((information.dwFlags & MONITORINFOF_PRIMARY) != 0)
        {
            entry.label += "  [primary]";
        }
        monitors.push_back(std::move(entry));
        return TRUE;
    }

    std::vector<MonitorEntry> EnumerateMonitors()
    {
        std::vector<MonitorEntry> result;
        EnumDisplayMonitors(
            nullptr,
            nullptr,
            &EnumerateMonitorCallback,
            reinterpret_cast<LPARAM>(&result));
        return result;
    }

    RECT IntersectRectangles(
        const RECT& left,
        const RECT& right)
    {
        return {
            std::max(left.left, right.left),
            std::max(left.top, right.top),
            std::min(left.right, right.right),
            std::min(left.bottom, right.bottom)};
    }

    bool IsValidRectangle(const RECT& rectangle)
    {
        return rectangle.right > rectangle.left &&
            rectangle.bottom > rectangle.top;
    }

    std::optional<D3D11_BOX> CenteredTextureBox(
        const D3D11_TEXTURE2D_DESC& description,
        bool centeredRegion,
        int requestedSize)
    {
        if (!centeredRegion ||
            description.Width == 0 ||
            description.Height == 0)
        {
            return std::nullopt;
        }

        const UINT side = std::max(
            1U,
            std::min({
                static_cast<UINT>(
                    std::max(1, requestedSize)),
                description.Width,
                description.Height}));
        const UINT left =
            (description.Width - side) / 2;
        const UINT top =
            (description.Height - side) / 2;
        return D3D11_BOX{
            left,
            top,
            0,
            left + side,
            top + side,
            1};
    }

    RECT TextureBoxToScreenRectangle(
        const RECT& fullRectangle,
        UINT textureWidth,
        UINT textureHeight,
        const D3D11_BOX& box)
    {
        const double scaleX =
            static_cast<double>(
                fullRectangle.right -
                fullRectangle.left) /
            static_cast<double>(textureWidth);
        const double scaleY =
            static_cast<double>(
                fullRectangle.bottom -
                fullRectangle.top) /
            static_cast<double>(textureHeight);
        return {
            fullRectangle.left +
                static_cast<LONG>(std::lround(
                    box.left * scaleX)),
            fullRectangle.top +
                static_cast<LONG>(std::lround(
                    box.top * scaleY)),
            fullRectangle.left +
                static_cast<LONG>(std::lround(
                    box.right * scaleX)),
            fullRectangle.top +
                static_cast<LONG>(std::lround(
                    box.bottom * scaleY))};
    }

    bool CopyTextureToMat(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source,
        ComPtr<ID3D11Texture2D>& staging,
        cv::Mat& output,
        const D3D11_BOX* sourceBox = nullptr)
    {
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        if (sourceBox != nullptr)
        {
            description.Width =
                sourceBox->right - sourceBox->left;
            description.Height =
                sourceBox->bottom - sourceBox->top;
        }

        bool recreate = staging == nullptr;
        if (!recreate)
        {
            D3D11_TEXTURE2D_DESC existing{};
            staging->GetDesc(&existing);
            recreate =
                existing.Width != description.Width ||
                existing.Height != description.Height ||
                existing.Format != description.Format;
        }
        if (recreate)
        {
            staging.Reset();
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.Usage = D3D11_USAGE_STAGING;
            description.MiscFlags = 0;
            description.MipLevels = 1;
            description.ArraySize = 1;
            if (FAILED(device->CreateTexture2D(
                    &description,
                    nullptr,
                    &staging)))
            {
                return false;
            }
        }

        if (sourceBox != nullptr)
        {
            context->CopySubresourceRegion(
                staging.Get(),
                0,
                0,
                0,
                0,
                source,
                0,
                sourceBox);
        }
        else
        {
            context->CopyResource(staging.Get(), source);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(
                staging.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)))
        {
            return false;
        }

        const cv::Mat view(
            static_cast<int>(description.Height),
            static_cast<int>(description.Width),
            CV_8UC4,
            mapped.pData,
            mapped.RowPitch);
        output = view.clone();
        context->Unmap(staging.Get(), 0);
        return !output.empty();
    }

    class CaptureBackend
    {
    public:
        virtual ~CaptureBackend() = default;
        virtual bool Start(
            const CaptureSettings& settings,
            ID3D11Device* sharedDevice,
            std::string& status) = 0;
        virtual bool TryGetFrame(
            CapturedFrame& frame,
            std::string& status,
            bool centeredRegion,
            int regionSize) = 0;
    };

    class WindowsGraphicsCaptureBackend final
        : public CaptureBackend
    {
    public:
        bool Start(
            const CaptureSettings& settings,
            ID3D11Device* sharedDevice,
            std::string& status) override
        {
            settings_ = settings;
            sharedDevice_ = sharedDevice;
            sharedDevice_->GetImmediateContext(&context_);

            if (!winrt::Windows::Graphics::Capture::
                    GraphicsCaptureSession::IsSupported())
            {
                status =
                    "Windows Graphics Capture is not supported";
                return false;
            }

            try
            {
                auto factory = winrt::get_activation_factory<
                    winrt::Windows::Graphics::Capture::
                        GraphicsCaptureItem,
                    IGraphicsCaptureItemInterop>();

                winrt::Windows::Graphics::Capture::
                    GraphicsCaptureItem item{nullptr};
                HRESULT result = E_INVALIDARG;
                if (settings.source == CaptureSourceKind::window)
                {
                    if (!IsWindow(settings.window))
                    {
                        status = "The selected window is no longer available";
                        return false;
                    }
                    result = factory->CreateForWindow(
                        settings.window,
                        winrt::guid_of<
                            winrt::Windows::Graphics::Capture::
                                GraphicsCaptureItem>(),
                        winrt::put_abi(item));
                }
                else
                {
                    if (settings.monitor == nullptr)
                    {
                        status = "No monitor is selected";
                        return false;
                    }
                    result = factory->CreateForMonitor(
                        settings.monitor,
                        winrt::guid_of<
                            winrt::Windows::Graphics::Capture::
                                GraphicsCaptureItem>(),
                        winrt::put_abi(item));
                }
                winrt::check_hresult(result);
                item_ = item;

                ComPtr<IDXGIDevice> dxgiDevice;
                winrt::check_hresult(
                    sharedDevice_->QueryInterface(
                        IID_PPV_ARGS(&dxgiDevice)));

                winrt::com_ptr<IInspectable> inspectable;
                winrt::check_hresult(
                    CreateDirect3D11DeviceFromDXGIDevice(
                        dxgiDevice.Get(),
                        inspectable.put()));
                direct3DDevice_ = inspectable.as<
                    winrt::Windows::Graphics::DirectX::
                        Direct3D11::IDirect3DDevice>();

                frameSize_ = item_.Size();
                framePool_ =
                    winrt::Windows::Graphics::Capture::
                        Direct3D11CaptureFramePool::
                            CreateFreeThreaded(
                                direct3DDevice_,
                                winrt::Windows::Graphics::DirectX::
                                    DirectXPixelFormat::
                                        B8G8R8A8UIntNormalized,
                                3,
                                frameSize_);
                session_ = framePool_.CreateCaptureSession(item_);
                try
                {
                    session_.IsCursorCaptureEnabled(false);
                }
                catch (...)
                {
                }
                try
                {
                    session_.IsBorderRequired(false);
                }
                catch (...)
                {
                }
                session_.StartCapture();

                status = "Windows Graphics Capture is running";
                return true;
            }
            catch (const winrt::hresult_error& error)
            {
                status =
                    "WinRT capture failed: 0x" +
                    HexCode(error.code().value);
                return false;
            }
        }

        bool TryGetFrame(
            CapturedFrame& frame,
            std::string& status,
            bool centeredRegion,
            int regionSize) override
        {
            try
            {
                auto captureFrame = framePool_.TryGetNextFrame();
                if (!captureFrame)
                {
                    return false;
                }

                const auto contentSize = captureFrame.ContentSize();
                auto access = captureFrame.Surface().as<
                    ::Windows::Graphics::DirectX::Direct3D11::
                        IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> texture;
                winrt::check_hresult(access->GetInterface(
                    IID_PPV_ARGS(&texture)));

                D3D11_TEXTURE2D_DESC textureDescription{};
                texture->GetDesc(&textureDescription);
                frame.sourceWidth =
                    static_cast<int>(
                        textureDescription.Width);
                frame.sourceHeight =
                    static_cast<int>(
                        textureDescription.Height);

                if (settings_.source == CaptureSourceKind::window)
                {
                    if (!GetWindowRect(
                            settings_.window,
                            &frame.screenRectangle))
                    {
                        frame.screenRectangle = {
                            0,
                            0,
                            contentSize.Width,
                            contentSize.Height};
                    }
                }
                else
                {
                    MONITORINFO information{sizeof(MONITORINFO)};
                    if (GetMonitorInfoW(
                            settings_.monitor,
                            &information))
                    {
                        frame.screenRectangle =
                            information.rcMonitor;
                    }
                    else
                    {
                        frame.screenRectangle = {
                            0,
                            0,
                            contentSize.Width,
                            contentSize.Height};
                    }
                }

                const auto sourceBox =
                    CenteredTextureBox(
                        textureDescription,
                        centeredRegion,
                        regionSize);
                if (sourceBox.has_value())
                {
                    frame.screenRectangle =
                        TextureBoxToScreenRectangle(
                            frame.screenRectangle,
                            textureDescription.Width,
                            textureDescription.Height,
                            *sourceBox);
                    frame.centeredRegionApplied = true;
                }
                if (!CopyTextureToMat(
                        sharedDevice_.Get(),
                        context_.Get(),
                        texture.Get(),
                        staging_,
                        frame.bgra,
                        sourceBox.has_value()
                            ? &*sourceBox
                            : nullptr))
                {
                    status = "Could not read the WinRT capture texture";
                    return false;
                }

                if (contentSize.Width != frameSize_.Width ||
                    contentSize.Height != frameSize_.Height)
                {
                    frameSize_ = contentSize;
                    framePool_.Recreate(
                        direct3DDevice_,
                        winrt::Windows::Graphics::DirectX::
                            DirectXPixelFormat::
                                B8G8R8A8UIntNormalized,
                        3,
                        frameSize_);
                    staging_.Reset();
                }
                return true;
            }
            catch (const winrt::hresult_error& error)
            {
                status =
                    "WinRT frame error: 0x" +
                    HexCode(error.code().value);
                return false;
            }
        }

    private:
        static std::string HexCode(std::int32_t value)
        {
            char buffer[16]{};
            snprintf(
                buffer,
                sizeof(buffer),
                "%08lX",
                static_cast<unsigned long>(
                    static_cast<std::uint32_t>(value)));
            return buffer;
        }

        CaptureSettings settings_;
        ComPtr<ID3D11Device> sharedDevice_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11Texture2D> staging_;
        winrt::Windows::Graphics::DirectX::Direct3D11::
            IDirect3DDevice direct3DDevice_{nullptr};
        winrt::Windows::Graphics::Capture::
            GraphicsCaptureItem item_{nullptr};
        winrt::Windows::Graphics::Capture::
            Direct3D11CaptureFramePool framePool_{nullptr};
        winrt::Windows::Graphics::Capture::
            GraphicsCaptureSession session_{nullptr};
        winrt::Windows::Graphics::SizeInt32 frameSize_{};
    };

    class DesktopDuplicationCaptureBackend final
        : public CaptureBackend
    {
    public:
        ~DesktopDuplicationCaptureBackend() override
        {
            if (frameAcquired_ && duplication_ != nullptr)
            {
                duplication_->ReleaseFrame();
            }
        }

        bool Start(
            const CaptureSettings& settings,
            ID3D11Device*,
            std::string& status) override
        {
            settings_ = settings;
            HMONITOR desiredMonitor = settings.monitor;
            if (settings.source == CaptureSourceKind::window)
            {
                if (!IsWindow(settings.window))
                {
                    status = "The selected window is no longer available";
                    return false;
                }
                desiredMonitor = MonitorFromWindow(
                    settings.window,
                    MONITOR_DEFAULTTONEAREST);
            }
            if (desiredMonitor == nullptr)
            {
                status = "No monitor is selected";
                return false;
            }

            ComPtr<IDXGIFactory1> factory;
            HRESULT result = CreateDXGIFactory1(
                IID_PPV_ARGS(&factory));
            if (FAILED(result))
            {
                status = "CreateDXGIFactory1 failed";
                return false;
            }

            ComPtr<IDXGIAdapter1> selectedAdapter;
            ComPtr<IDXGIOutput> selectedOutput;
            for (UINT adapterIndex = 0;; ++adapterIndex)
            {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(
                        adapterIndex,
                        &adapter) == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }

                for (UINT outputIndex = 0;; ++outputIndex)
                {
                    ComPtr<IDXGIOutput> output;
                    if (adapter->EnumOutputs(
                            outputIndex,
                            &output) == DXGI_ERROR_NOT_FOUND)
                    {
                        break;
                    }

                    DXGI_OUTPUT_DESC description{};
                    if (SUCCEEDED(output->GetDesc(&description)) &&
                        description.Monitor == desiredMonitor)
                    {
                        selectedAdapter = adapter;
                        selectedOutput = output;
                        outputDescription_ = description;
                        break;
                    }
                }
                if (selectedOutput != nullptr)
                {
                    break;
                }
            }

            if (selectedAdapter == nullptr ||
                selectedOutput == nullptr)
            {
                status = "Could not match the monitor to a DXGI output";
                return false;
            }

            constexpr D3D_FEATURE_LEVEL levels[]{
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0};
            D3D_FEATURE_LEVEL selectedLevel{};
            result = D3D11CreateDevice(
                selectedAdapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                &device_,
                &selectedLevel,
                &context_);
            if (FAILED(result))
            {
                status = "Could not create the duplication D3D11 device";
                return false;
            }

            ComPtr<IDXGIOutput1> output1;
            result = selectedOutput.As(&output1);
            if (SUCCEEDED(result))
            {
                result = output1->DuplicateOutput(
                    device_.Get(),
                    &duplication_);
            }
            if (FAILED(result))
            {
                status =
                    "DuplicateOutput failed: 0x" +
                    HexCode(result);
                return false;
            }

            status = "Desktop Duplication is running";
            return true;
        }

        bool TryGetFrame(
            CapturedFrame& frame,
            std::string& status,
            bool centeredRegion,
            int regionSize) override
        {
            if (duplication_ == nullptr)
            {
                return false;
            }

            if (frameAcquired_)
            {
                duplication_->ReleaseFrame();
                frameAcquired_ = false;
            }

            DXGI_OUTDUPL_FRAME_INFO frameInformation{};
            ComPtr<IDXGIResource> resource;
            const HRESULT result = duplication_->AcquireNextFrame(
                0,
                &frameInformation,
                &resource);
            if (result == DXGI_ERROR_WAIT_TIMEOUT)
            {
                return false;
            }
            if (result == DXGI_ERROR_ACCESS_LOST)
            {
                status =
                    "Desktop Duplication access was lost; restart capture";
                return false;
            }
            if (FAILED(result))
            {
                status =
                    "AcquireNextFrame failed: 0x" +
                    HexCode(result);
                return false;
            }
            frameAcquired_ = true;

            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resource.As(&texture)))
            {
                status = "Could not access the duplicated desktop texture";
                duplication_->ReleaseFrame();
                frameAcquired_ = false;
                return false;
            }

            D3D11_TEXTURE2D_DESC textureDescription{};
            texture->GetDesc(&textureDescription);
            frame.sourceWidth =
                static_cast<int>(textureDescription.Width);
            frame.sourceHeight =
                static_cast<int>(textureDescription.Height);
            frame.screenRectangle =
                outputDescription_.DesktopCoordinates;

            std::optional<D3D11_BOX> sourceBox;
            if (settings_.source == CaptureSourceKind::monitor &&
                (outputDescription_.Rotation ==
                    DXGI_MODE_ROTATION_IDENTITY ||
                 outputDescription_.Rotation ==
                    DXGI_MODE_ROTATION_UNSPECIFIED))
            {
                sourceBox = CenteredTextureBox(
                    textureDescription,
                    centeredRegion,
                    regionSize);
            }
            if (sourceBox.has_value())
            {
                frame.screenRectangle =
                    TextureBoxToScreenRectangle(
                        frame.screenRectangle,
                        textureDescription.Width,
                        textureDescription.Height,
                        *sourceBox);
                frame.centeredRegionApplied = true;
            }

            if (!CopyTextureToMat(
                    device_.Get(),
                    context_.Get(),
                    texture.Get(),
                    staging_,
                    frame.bgra,
                    sourceBox.has_value()
                        ? &*sourceBox
                        : nullptr))
            {
                status = "Could not read the duplicated desktop texture";
                duplication_->ReleaseFrame();
                frameAcquired_ = false;
                return false;
            }

            duplication_->ReleaseFrame();
            frameAcquired_ = false;

            switch (outputDescription_.Rotation)
            {
            case DXGI_MODE_ROTATION_ROTATE90:
                cv::rotate(
                    frame.bgra,
                    frame.bgra,
                    cv::ROTATE_90_CLOCKWISE);
                break;
            case DXGI_MODE_ROTATION_ROTATE180:
                cv::rotate(
                    frame.bgra,
                    frame.bgra,
                    cv::ROTATE_180);
                break;
            case DXGI_MODE_ROTATION_ROTATE270:
                cv::rotate(
                    frame.bgra,
                    frame.bgra,
                    cv::ROTATE_90_COUNTERCLOCKWISE);
                break;
            default:
                break;
            }
            if (outputDescription_.Rotation ==
                    DXGI_MODE_ROTATION_ROTATE90 ||
                outputDescription_.Rotation ==
                    DXGI_MODE_ROTATION_ROTATE270)
            {
                frame.sourceWidth = frame.bgra.cols;
                frame.sourceHeight = frame.bgra.rows;
            }
            return true;
        }

    private:
        static std::string HexCode(HRESULT value)
        {
            char buffer[16]{};
            snprintf(
                buffer,
                sizeof(buffer),
                "%08lX",
                static_cast<unsigned long>(value));
            return buffer;
        }

        CaptureSettings settings_;
        DXGI_OUTPUT_DESC outputDescription_{};
        bool frameAcquired_ = false;
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDXGIOutputDuplication> duplication_;
        ComPtr<ID3D11Texture2D> staging_;
    };

    std::uint32_t ColorKey(
        std::uint8_t blue,
        std::uint8_t green,
        std::uint8_t red)
    {
        return static_cast<std::uint32_t>(blue) |
            (static_cast<std::uint32_t>(green) << 8) |
            (static_cast<std::uint32_t>(red) << 16);
    }

    const std::unordered_set<std::uint32_t>& BlacklistedColors()
    {
        static const std::array<std::array<std::uint8_t, 3>, 75>
            colors{{
                {252, 175, 253}, {252, 160, 233},
                {254, 173, 254}, {165, 141, 217},
                {173, 135, 189}, {120, 102, 196},
                {221, 162, 243}, {137, 114, 235},
                {208, 162, 230}, {176, 114, 192},
                {182, 133, 199}, {202, 152, 207},
                {214, 164, 233}, {87, 22, 61},
                {112, 34, 97}, {38, 30, 67},
                {139, 65, 147}, {158, 61, 134},
                {245, 107, 244}, {254, 110, 254},
                {255, 78, 250}, {201, 38, 151},
                {254, 85, 251}, {187, 31, 138},
                {123, 10, 130}, {154, 17, 151},
                {101, 1, 113}, {168, 23, 162},
                {114, 9, 123}, {209, 42, 153},
                {135, 23, 94}, {92, 12, 72},
                {67, 19, 55}, {75, 1, 84},
                {39, 0, 37}, {37, 3, 46},
                {160, 16, 141}, {61, 1, 55},
                {73, 2, 65}, {93, 3, 80},
                {73, 3, 72}, {255, 126, 254},
                {255, 49, 255}, {133, 15, 136},
                {108, 3, 129}, {255, 94, 254},
                {255, 62, 231}, {223, 36, 205},
                {255, 140, 255}, {255, 86, 234},
                {197, 29, 188}, {254, 146, 255},
                {139, 8, 158}, {157, 12, 172},
                {134, 31, 132}, {197, 50, 194},
                {195, 79, 183}, {206, 55, 203},
                {240, 67, 238}, {116, 2, 139},
                {255, 211, 254}, {162, 14, 177},
                {253, 51, 210}, {117, 16, 72},
                {255, 189, 255}, {255, 142, 255},
                {250, 61, 252}, {79, 22, 78},
                {250, 58, 252}, {225, 49, 140},
                {252, 170, 253}, {167, 26, 147},
                {251, 142, 253}, {143, 22, 125},
                {69, 2, 63}}};

        static const std::unordered_set<std::uint32_t> result = []
        {
            std::unordered_set<std::uint32_t> set;
            set.reserve(colors.size());
            for (const auto& listedColor : colors)
            {
                set.insert(ColorKey(
                    listedColor[0],
                    listedColor[1],
                    listedColor[2]));
            }
            return set;
        }();
        return result;
    }

    void RemoveBlacklistedPixels(
        const cv::Mat& bgr,
        cv::Mat& mask)
    {
        const auto& blacklist = BlacklistedColors();
        for (int row = 0; row < bgr.rows; ++row)
        {
            const cv::Vec3b* colors =
                bgr.ptr<cv::Vec3b>(row);
            std::uint8_t* selected =
                mask.ptr<std::uint8_t>(row);
            for (int column = 0; column < bgr.cols; ++column)
            {
                if (selected[column] == 0)
                {
                    continue;
                }

                const auto& pixelColor = colors[column];
                if (blacklist.contains(ColorKey(
                        pixelColor[0],
                        pixelColor[1],
                        pixelColor[2])))
                {
                    selected[column] = 0;
                }
            }
        }
    }
}

struct vanta::CaptureController::Implementation
{
    bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        DWORD processId)
    {
        previewDevice = device;
        previewContext = context;
        ownProcessId = processId;

        try
        {
            winrt::init_apartment(
                winrt::apartment_type::multi_threaded);
            apartmentInitialized = true;
        }
        catch (const winrt::hresult_error& error)
        {
            if (error.code() != RPC_E_CHANGED_MODE)
            {
                status = "Could not initialize Windows Runtime capture";
                return false;
            }
        }

        RefreshSources();
        sourceIndex = 1;
        captureEnabled = true;
        if (!StartCapture())
        {
            vanta::log::Warning(
                "automatic capture start failed: %s",
                status.c_str());
        }
        return true;
    }

    void Shutdown()
    {
        StopCapture();
        previewShaderResource.Reset();
        previewTexture.Reset();
        previewContext.Reset();
        previewDevice.Reset();
        windows.clear();
        monitors.clear();
        if (apartmentInitialized)
        {
            winrt::uninit_apartment();
            apartmentInitialized = false;
        }
    }

    void RefreshSources()
    {
        const HWND previousWindow =
            selectedWindow >= 0 &&
            selectedWindow < static_cast<int>(windows.size())
            ? windows[static_cast<std::size_t>(
                selectedWindow)].window
            : nullptr;
        const HMONITOR previousMonitor =
            selectedMonitor >= 0 &&
            selectedMonitor < static_cast<int>(monitors.size())
            ? monitors[static_cast<std::size_t>(
                selectedMonitor)].monitor
            : nullptr;

        windows = EnumerateWindows(ownProcessId);
        monitors = EnumerateMonitors();

        const HWND preferredWindow =
            previousWindow != nullptr
            ? previousWindow
            : GetForegroundWindow();
        selectedWindow = 0;
        for (std::size_t index = 0; index < windows.size(); ++index)
        {
            if (windows[index].window == preferredWindow)
            {
                selectedWindow = static_cast<int>(index);
                break;
            }
        }

        HMONITOR preferredMonitor = previousMonitor;
        if (preferredMonitor == nullptr)
        {
            POINT cursor{};
            if (GetCursorPos(&cursor))
            {
                preferredMonitor = MonitorFromPoint(
                    cursor,
                    MONITOR_DEFAULTTOPRIMARY);
            }
        }
        selectedMonitor = 0;
        for (std::size_t index = 0; index < monitors.size(); ++index)
        {
            if (monitors[index].monitor == preferredMonitor)
            {
                selectedMonitor = static_cast<int>(index);
                break;
            }
        }
    }

    CaptureSettings CurrentSettings() const
    {
        CaptureSettings settings;
        settings.backend =
            backendIndex == 0
            ? CaptureBackendKind::windowsGraphicsCapture
            : CaptureBackendKind::desktopDuplication;
        settings.source =
            sourceIndex == 0
            ? CaptureSourceKind::window
            : CaptureSourceKind::monitor;
        if (selectedWindow >= 0 &&
            selectedWindow < static_cast<int>(windows.size()))
        {
            settings.window =
                windows[static_cast<std::size_t>(
                    selectedWindow)].window;
        }
        if (selectedMonitor >= 0 &&
            selectedMonitor < static_cast<int>(monitors.size()))
        {
            settings.monitor =
                monitors[static_cast<std::size_t>(
                    selectedMonitor)].monitor;
        }
        return settings;
    }

    bool StartCapture()
    {
        StopCapture();
        if (!captureEnabled)
        {
            status = "Capture paused";
            return true;
        }

        activeSettings = CurrentSettings();
        if (activeSettings.source == CaptureSourceKind::window &&
            activeSettings.window == nullptr)
        {
            status = "No capturable window is selected";
            return false;
        }
        if (activeSettings.source == CaptureSourceKind::monitor &&
            activeSettings.monitor == nullptr)
        {
            status = "No monitor is selected";
            return false;
        }

        if (activeSettings.backend ==
            CaptureBackendKind::windowsGraphicsCapture)
        {
            captureBackend =
                std::make_unique<
                    WindowsGraphicsCaptureBackend>();
        }
        else
        {
            captureBackend =
                std::make_unique<
                    DesktopDuplicationCaptureBackend>();
        }

        if (!captureBackend->Start(
                activeSettings,
                previewDevice.Get(),
                status))
        {
            captureBackend.reset();
            running = false;
            return false;
        }

        running = true;
        frameCounter = 0;
        sessionPublishedFrames = 0;
        framesPerSecond = 0.0F;
        firstFrameLogged = false;
        fpsWindowStart =
            std::chrono::steady_clock::now();
        nextFpsLog =
            fpsWindowStart +
            std::chrono::seconds(1);
        vanta::log::Info("capture started: %s", status.c_str());
        return true;
    }

    void StopCapture()
    {
        if (running)
        {
            vanta::log::Info(
                "capture stopped: published_frames=%llu "
                "last_sequence=%llu",
                static_cast<unsigned long long>(
                    sessionPublishedFrames),
                static_cast<unsigned long long>(
                    latestFrameSequence));
        }
        captureBackend.reset();
        running = false;
        outline.visible = false;
        {
            std::lock_guard<std::mutex> lock(latestFrameMutex);
            latestCenteredFrame.release();
            latestFrameTimestampNanoseconds = 0;
        }
        latestFrameCondition.notify_all();
    }

    void Tick()
    {
        if (!running || captureBackend == nullptr)
        {
            return;
        }

        CapturedFrame frame;
        const std::string previousStatus = status;
        if (!captureBackend->TryGetFrame(
                frame,
                status,
                regionIndex == 1,
                regionSize))
        {
            if (status != previousStatus &&
                status.find("error") != std::string::npos)
            {
                vanta::log::Warning(
                    "capture update: %s",
                    status.c_str());
            }
            return;
        }
        if (frame.bgra.empty())
        {
            return;
        }

        ProcessFrame(frame);
        if (!firstFrameLogged &&
            previewShaderResource != nullptr)
        {
            const char* previewMode =
                binaryMask
                ? "binary"
                : applyFilter
                    ? "filtered-color"
                    : "original";
            vanta::log::Info(
                "capture preview ready: source=%dx%d region=%dx%d "
                "preview=%dx%d matches=%d mode=%s",
                sourceWidth,
                sourceHeight,
                captureWidth,
                captureHeight,
                previewWidth,
                previewHeight,
                matchedPixels,
                previewMode);
            firstFrameLogged = true;
        }
        ++frameCounter;
        const auto current =
            std::chrono::steady_clock::now();
        const float elapsed =
            std::chrono::duration<float>(
                current - fpsWindowStart).count();
        if (elapsed >= 1.0F)
        {
            framesPerSecond =
                static_cast<float>(frameCounter) / elapsed;
            frameCounter = 0;
            fpsWindowStart = current;
            if (current >= nextFpsLog)
            {
                vanta::log::Info(
                    "capture throughput: %.1f FPS",
                    framesPerSecond);
                nextFpsLog =
                    current +
                    std::chrono::seconds(5);
            }
        }
    }

    void ProcessFrame(const CapturedFrame& captured)
    {
        cv::Mat source = captured.bgra;
        RECT sourceScreenRectangle =
            captured.screenRectangle;

        if (activeSettings.backend ==
                CaptureBackendKind::desktopDuplication &&
            activeSettings.source == CaptureSourceKind::window)
        {
            RECT windowRectangle{};
            if (!GetWindowRect(
                    activeSettings.window,
                    &windowRectangle))
            {
                status = "The selected window is no longer available";
                return;
            }

            const RECT intersection = IntersectRectangles(
                sourceScreenRectangle,
                windowRectangle);
            if (!IsValidRectangle(intersection))
            {
                status =
                    "The selected window is outside the duplicated monitor";
                return;
            }

            const float scaleX =
                static_cast<float>(source.cols) /
                static_cast<float>(
                    sourceScreenRectangle.right -
                    sourceScreenRectangle.left);
            const float scaleY =
                static_cast<float>(source.rows) /
                static_cast<float>(
                    sourceScreenRectangle.bottom -
                    sourceScreenRectangle.top);
            const int left = std::clamp(
                static_cast<int>(std::lround(
                    (intersection.left -
                     sourceScreenRectangle.left) * scaleX)),
                0,
                source.cols - 1);
            const int top = std::clamp(
                static_cast<int>(std::lround(
                    (intersection.top -
                     sourceScreenRectangle.top) * scaleY)),
                0,
                source.rows - 1);
            const int right = std::clamp(
                static_cast<int>(std::lround(
                    (intersection.right -
                     sourceScreenRectangle.left) * scaleX)),
                left + 1,
                source.cols);
            const int bottom = std::clamp(
                static_cast<int>(std::lround(
                    (intersection.bottom -
                     sourceScreenRectangle.top) * scaleY)),
                top + 1,
                source.rows);
            source = source(
                cv::Rect(
                    left,
                    top,
                    right - left,
                    bottom - top));
            sourceScreenRectangle = intersection;
        }

        const bool desktopWindowSource =
            activeSettings.backend ==
                CaptureBackendKind::desktopDuplication &&
            activeSettings.source ==
                CaptureSourceKind::window;
        sourceWidth =
            !desktopWindowSource &&
                captured.sourceWidth > 0
            ? captured.sourceWidth
            : source.cols;
        sourceHeight =
            !desktopWindowSource &&
                captured.sourceHeight > 0
            ? captured.sourceHeight
            : source.rows;
        cv::Rect captureRectangle(
            0,
            0,
            source.cols,
            source.rows);
        if (regionIndex == 1 &&
            !captured.centeredRegionApplied)
        {
            const int side = std::max(
                1,
                std::min({
                    regionSize,
                    source.cols,
                    source.rows}));
            captureRectangle = cv::Rect(
                (source.cols - side) / 2,
                (source.rows - side) / 2,
                side,
                side);
        }

        const float screenScaleX =
            static_cast<float>(
                sourceScreenRectangle.right -
                sourceScreenRectangle.left) /
            static_cast<float>(source.cols);
        const float screenScaleY =
            static_cast<float>(
                sourceScreenRectangle.bottom -
                sourceScreenRectangle.top) /
            static_cast<float>(source.rows);
        outline.screenRectangle = {
            sourceScreenRectangle.left +
                static_cast<LONG>(std::lround(
                    captureRectangle.x * screenScaleX)),
            sourceScreenRectangle.top +
                static_cast<LONG>(std::lround(
                    captureRectangle.y * screenScaleY)),
            sourceScreenRectangle.left +
                static_cast<LONG>(std::lround(
                    (captureRectangle.x +
                     captureRectangle.width) * screenScaleX)),
            sourceScreenRectangle.top +
                static_cast<LONG>(std::lround(
                    (captureRectangle.y +
                     captureRectangle.height) * screenScaleY))};
        outline.visible =
            running &&
            drawOutline &&
            IsValidRectangle(outline.screenRectangle);
        std::copy(
            std::begin(outlineColor),
            std::end(outlineColor),
            std::begin(outline.color));
        outline.thickness = 1;

        const cv::Mat capture =
            source(captureRectangle);
        captureWidth = capture.cols;
        captureHeight = capture.rows;

        // Publish the immutable BGRA capture region. cv::Mat reference
        // counting keeps its storage alive after this lock is released.
        {
            std::lock_guard<std::mutex> lock(latestFrameMutex);
            latestCenteredFrame = capture;
            ++latestFrameSequence;
            ++sessionPublishedFrames;
            latestFrameTimestampNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
        }
        latestFrameCondition.notify_all();

        cv::Mat processed;
        if (applyFilter || binaryMask)
        {
            cv::Mat bgr;
            cv::cvtColor(
                capture,
                bgr,
                cv::COLOR_BGRA2BGR);

            cv::Mat hsv;
            cv::cvtColor(
                bgr,
                hsv,
                cv::COLOR_BGR2HSV);

            cv::Mat mask;
            const auto& colorTarget =
                vanta::kHsvColorTargets[
                    static_cast<std::size_t>(
                        std::clamp(
                            colorTargetIndex,
                            0,
                            static_cast<int>(
                                vanta::kHsvColorTargets.size()) - 1))];
            cv::inRange(
                hsv,
                cv::Scalar(
                    colorTarget.lower[0],
                    colorTarget.lower[1],
                    colorTarget.lower[2]),
                cv::Scalar(
                    colorTarget.upper[0],
                    colorTarget.upper[1],
                    colorTarget.upper[2]),
                mask);
            RemoveBlacklistedPixels(bgr, mask);
            matchedPixels = cv::countNonZero(mask);

            if (binaryMask)
            {
                cv::cvtColor(
                    mask,
                    processed,
                    cv::COLOR_GRAY2BGRA);
            }
            else
            {
                cv::Mat colorFrame;
                cv::cvtColor(
                    bgr,
                    colorFrame,
                    cv::COLOR_BGR2BGRA);
                processed = cv::Mat::zeros(
                    colorFrame.size(),
                    colorFrame.type());
                colorFrame.copyTo(processed, mask);
            }
        }
        else
        {
            processed = capture;
            matchedPixels = 0;
        }

        constexpr int maximumPreviewWidth = 560;
        constexpr int maximumPreviewHeight = 315;
        const float previewScale = std::min({
            1.0F,
            static_cast<float>(maximumPreviewWidth) /
                static_cast<float>(processed.cols),
            static_cast<float>(maximumPreviewHeight) /
                static_cast<float>(processed.rows)});
        const cv::Size previewSize(
            std::max(
                1,
                static_cast<int>(std::lround(
                    processed.cols * previewScale))),
            std::max(
                1,
                static_cast<int>(std::lround(
                    processed.rows * previewScale))));

        cv::Mat preview;
        if (previewSize.width != processed.cols ||
            previewSize.height != processed.rows)
        {
            cv::resize(
                processed,
                preview,
                previewSize,
                0.0,
                0.0,
                cv::INTER_AREA);
        }
        else
        {
            preview = processed;
        }
        UploadPreview(preview);
        status = "Capturing";
    }

    void UploadPreview(const cv::Mat& bgra)
    {
        if (bgra.empty() ||
            bgra.type() != CV_8UC4)
        {
            return;
        }

        if (previewTexture == nullptr ||
            previewWidth != bgra.cols ||
            previewHeight != bgra.rows)
        {
            previewShaderResource.Reset();
            previewTexture.Reset();

            D3D11_TEXTURE2D_DESC description{};
            description.Width =
                static_cast<UINT>(bgra.cols);
            description.Height =
                static_cast<UINT>(bgra.rows);
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
                status = "Could not create the preview texture";
                return;
            }
            previewWidth = bgra.cols;
            previewHeight = bgra.rows;
        }

        cv::Mat contiguous =
            bgra.isContinuous()
            ? bgra
            : bgra.clone();
        previewContext->UpdateSubresource(
            previewTexture.Get(),
            0,
            nullptr,
            contiguous.data,
            static_cast<UINT>(
                contiguous.cols *
                contiguous.elemSize()),
            0);
    }

    bool RenderWindowSelector()
    {
        const char* preview =
            selectedWindow >= 0 &&
            selectedWindow < static_cast<int>(windows.size())
            ? windows[static_cast<std::size_t>(
                selectedWindow)].label.c_str()
            : "No capturable windows";

        bool changed = false;
        if (custom::BeginCombo(
                "Window",
                preview,
                std::max(
                    1,
                    static_cast<int>(windows.size()))))
        {
            for (std::size_t index = 0;
                 index < windows.size();
                 ++index)
            {
                const bool selected =
                    selectedWindow ==
                    static_cast<int>(index);
                if (custom::Selectable(
                        windows[index].label.c_str(),
                        selected))
                {
                    selectedWindow =
                        static_cast<int>(index);
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            custom::EndCombo();
        }
        return changed;
    }

    bool RenderMonitorSelector()
    {
        const char* preview =
            selectedMonitor >= 0 &&
            selectedMonitor < static_cast<int>(monitors.size())
            ? monitors[static_cast<std::size_t>(
                selectedMonitor)].label.c_str()
            : "No monitors";

        bool changed = false;
        if (custom::BeginCombo(
                "Monitor",
                preview,
                std::max(
                    1,
                    static_cast<int>(monitors.size()))))
        {
            for (std::size_t index = 0;
                 index < monitors.size();
                 ++index)
            {
                const bool selected =
                    selectedMonitor ==
                    static_cast<int>(index);
                if (custom::Selectable(
                        monitors[index].label.c_str(),
                        selected))
                {
                    selectedMonitor =
                        static_cast<int>(index);
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            custom::EndCombo();
        }
        return changed;
    }

    void RenderPanel()
    {
        const ImVec2 available =
            ImGui::GetContentRegionAvail();
        const float gap = ImGui::GetStyle().ItemSpacing.x * 3.0F;
        const float settingsWidth = std::clamp(
            available.x * 0.43F,
            265.0F,
            330.0F);
        const float previewPanelWidth = std::max(
            180.0F,
            available.x - settingsWidth - gap);
        const float childBodyHeight = std::max(
            190.0F,
            available.y - 40.0F);

        bool selectionChanged = false;
        if (custom::Child(
                ICON_SETTINGS_3_LINE
                    "  Capture settings##capture-settings",
                ImVec2(settingsWidth, childBodyHeight),
                true))
        {
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));
            const char* backends[]{
                "Windows Graphics Capture",
                "Desktop Duplication"};
            const char* sources[]{"Window", "Monitor"};
            const char* regions[]{
                "Full source",
                "Centered square"};

            selectionChanged |= custom::Combo(
                "Backend",
                &backendIndex,
                backends,
                IM_ARRAYSIZE(backends));
            selectionChanged |= custom::Combo(
                "Source",
                &sourceIndex,
                sources,
                IM_ARRAYSIZE(sources));

            if (sourceIndex == 0)
            {
                selectionChanged |= RenderWindowSelector();
            }
            else
            {
                selectionChanged |= RenderMonitorSelector();
            }

            if (custom::Button(
                    "Refresh sources",
                    ImVec2(
                        ImGui::GetContentRegionAvail().x,
                        34.0F)))
            {
                RefreshSources();
                selectionChanged = true;
            }

            custom::Separator();
            custom::Combo(
                "Capture area",
                &regionIndex,
                regions,
                IM_ARRAYSIZE(regions));
            if (regionIndex == 1)
            {
                custom::SliderInt(
                    "Square size",
                    &regionSize,
                    128,
                    1280,
                    "%d px");
            }

            custom::Checkbox(
                "Draw capture outline",
                &drawOutline);
            if (drawOutline)
            {
                custom::ColorEdit4(
                    "Outline color",
                    outlineColor,
                    ImGuiColorEditFlags_NoSidePreview |
                        ImGuiColorEditFlags_AlphaBar |
                    ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_AlphaPreview);
            }

            custom::Checkbox(
                "Apply HSV + blacklist filter",
                &applyFilter);
            custom::Checkbox(
                "Black/white mask",
                &binaryMask);
            const auto& colorTarget =
                vanta::kHsvColorTargets[
                    static_cast<std::size_t>(
                        std::clamp(
                            colorTargetIndex,
                            0,
                            static_cast<int>(
                                vanta::kHsvColorTargets.size()) - 1))];
            ImGui::TextDisabled(
                "HSV [%d,%d,%d] to [%d,%d,%d]\n"
                "75 blacklist colors (OpenCV BGR)",
                colorTarget.lower[0],
                colorTarget.lower[1],
                colorTarget.lower[2],
                colorTarget.upper[0],
                colorTarget.upper[1],
                colorTarget.upper[2]);

            if (backendIndex == 1 && sourceIndex == 0)
            {
                ImGui::TextDisabled(
                    "Desktop Duplication crops the visible window "
                    "rectangle; occlusion remains.");
            }
            ImGui::PopStyleVar();
        }
        custom::EndChild();

        if (selectionChanged)
        {
            if (captureEnabled)
            {
                StartCapture();
            }
            else
            {
                status = "Source selected; capture is paused";
            }
        }

        ImGui::SameLine(0.0F, gap);
        if (custom::Child(
                ICON_VIDEO_CAMERA_LINE
                    "  Capture output##capture-output",
                ImVec2(previewPanelWidth, childBodyHeight),
                true))
        {
            ImGui::TextColored(
                running
                ? ImVec4(0.45F, 0.96F, 0.65F, 1.0F)
                : ImVec4(0.75F, 0.75F, 0.78F, 1.0F),
                running ? "LIVE  |  %s" : "OFFLINE  |  %s",
                status.c_str());
            custom::Separator();

            if (previewShaderResource != nullptr &&
                previewWidth > 0 &&
                previewHeight > 0)
            {
                ImGui::Text(
                    "Source %dx%d | region %dx%d",
                    sourceWidth,
                    sourceHeight,
                    captureWidth,
                    captureHeight);
                ImGui::Text(
                    "Preview %dx%d | %.1f FPS",
                    previewWidth,
                    previewHeight,
                    framesPerSecond);
                if (applyFilter || binaryMask)
                {
                    ImGui::Text(
                        "Matching pixels: %d",
                        matchedPixels);
                }

                const ImVec2 imageArea =
                    ImGui::GetContentRegionAvail();
                const float imageScale = std::min({
                    1.0F,
                    imageArea.x /
                        static_cast<float>(previewWidth),
                    imageArea.y /
                        static_cast<float>(previewHeight)});
                const float displayWidth =
                    static_cast<float>(previewWidth) *
                    imageScale;
                const float displayHeight =
                    static_cast<float>(previewHeight) *
                    imageScale;
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    std::max(
                        0.0F,
                        (imageArea.x - displayWidth) * 0.5F));
                const ImVec2 imagePosition =
                    ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    imagePosition - ImVec2(4.0F, 4.0F),
                    imagePosition +
                        ImVec2(displayWidth, displayHeight) +
                        ImVec2(4.0F, 4.0F),
                    IM_COL32(10, 10, 12, 220),
                    6.0F);
                ImGui::GetWindowDrawList()->AddRect(
                    imagePosition - ImVec2(4.0F, 4.0F),
                    imagePosition +
                        ImVec2(displayWidth, displayHeight) +
                        ImVec2(4.0F, 4.0F),
                    ImGui::GetColorU32(c::main_color.Value),
                    6.0F);
                ImGui::Image(
                    reinterpret_cast<ImTextureID>(
                        previewShaderResource.Get()),
                    ImVec2(displayWidth, displayHeight));
            }
            else
            {
                const ImVec2 placeholderSize(
                    ImGui::GetContentRegionAvail().x,
                    std::max(
                        150.0F,
                        ImGui::GetContentRegionAvail().y));
                const ImVec2 placeholderPosition =
                    ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    placeholderPosition,
                    placeholderPosition + placeholderSize,
                    IM_COL32(10, 10, 12, 210),
                    6.0F);
                ImGui::GetWindowDrawList()->AddRect(
                    placeholderPosition,
                    placeholderPosition + placeholderSize,
                    ImGui::GetColorU32(c::child::stroke),
                    6.0F);
                const char* placeholder =
                    captureEnabled
                    ? "Starting capture preview..."
                    : "Capture is paused";
                const ImVec2 textSize =
                    ImGui::CalcTextSize(placeholder);
                ImGui::GetWindowDrawList()->AddText(
                    placeholderPosition +
                        (placeholderSize - textSize) * 0.5F,
                    ImGui::GetColorU32(
                        c::text::label::default_color.Value),
                    placeholder);
                ImGui::Dummy(placeholderSize);
            }
        }
        custom::EndChild();
    }

    bool StartAutomatedCapture(
        bool desktopDuplication,
        bool windowSource)
    {
        RefreshSources();
        if (windowSource && windows.empty())
        {
            status = "No window is available for the capture test";
            return false;
        }
        if (!windowSource && monitors.empty())
        {
            status = "No monitor is available for the capture test";
            return false;
        }
        backendIndex = desktopDuplication ? 1 : 0;
        sourceIndex = windowSource ? 0 : 1;
        captureEnabled = true;
        if (windowSource)
        {
            selectedWindow = 0;
        }
        else
        {
            selectedMonitor = 0;
        }
        regionIndex = 1;
        regionSize = 640;
        applyFilter = true;
        binaryMask = desktopDuplication;
        return StartCapture();
    }

    DWORD ownProcessId{};
    bool apartmentInitialized = false;
    bool running = false;
    bool captureEnabled = true;
    int backendIndex = 0;
    int sourceIndex = 1;
    int selectedWindow = 0;
    int selectedMonitor = 0;
    int regionIndex = 1;
    int regionSize = 640;
    bool drawOutline = true;
    bool applyFilter = false;
    bool binaryMask = false;
    int colorTargetIndex = 0;
    bool firstFrameLogged = false;
    float outlineColor[4]{
        0.68F,
        0.56F,
        0.91F,
        1.0F};
    std::string status;
    std::vector<WindowEntry> windows;
    std::vector<MonitorEntry> monitors;
    CaptureSettings activeSettings;
    std::unique_ptr<CaptureBackend> captureBackend;

    ComPtr<ID3D11Device> previewDevice;
    ComPtr<ID3D11DeviceContext> previewContext;
    ComPtr<ID3D11Texture2D> previewTexture;
    ComPtr<ID3D11ShaderResourceView> previewShaderResource;
    int previewWidth = 0;
    int previewHeight = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int captureWidth = 0;
    int captureHeight = 0;
    int matchedPixels = 0;
    float framesPerSecond = 0.0F;
    std::uint64_t frameCounter = 0;
    std::chrono::steady_clock::time_point fpsWindowStart{};
    std::chrono::steady_clock::time_point nextFpsLog{};
    vanta::CaptureOutline outline;

    // Store a BGR copy of the centered region for testclick (BGRA, CV_8UC4).
    mutable std::mutex latestFrameMutex;
    mutable std::condition_variable latestFrameCondition;
    cv::Mat latestCenteredFrame;
    std::uint64_t latestFrameSequence{};
    std::uint64_t sessionPublishedFrames{};
    std::int64_t latestFrameTimestampNanoseconds{};
};

namespace vanta
{
    CaptureController::CaptureController()
        : implementation_(std::make_unique<Implementation>())
    {
    }

    CaptureController::~CaptureController()
    {
        Shutdown();
    }

    bool CaptureController::Initialize(
        ID3D11Device* previewDevice,
        ID3D11DeviceContext* previewContext,
        DWORD ownProcessId)
    {
        return implementation_->Initialize(
            previewDevice,
            previewContext,
            ownProcessId);
    }

    void CaptureController::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }

    void CaptureController::Tick()
    {
        implementation_->Tick();
    }

    void CaptureController::RenderPanel()
    {
        implementation_->RenderPanel();
    }

    bool CaptureController::StartAutomatedCapture(
        bool desktopDuplication,
        bool windowSource)
    {
        return implementation_->StartAutomatedCapture(
            desktopDuplication,
            windowSource);
    }

    void CaptureController::SetColorTargetIndex(int index) noexcept
    {
        implementation_->colorTargetIndex =
            std::clamp(
                index,
                0,
                static_cast<int>(
                    kHsvColorTargets.size()) - 1);
    }

    CaptureOutline CaptureController::GetOutline() const noexcept
    {
        CaptureOutline result =
            implementation_->outline;
        result.visible =
            implementation_->running &&
            implementation_->drawOutline &&
            IsValidRectangle(result.screenRectangle);
        std::copy(
            std::begin(implementation_->outlineColor),
            std::end(implementation_->outlineColor),
            std::begin(result.color));
        result.thickness = 1;
        return result;
    }

    bool CaptureController::GetLatestCenteredFrame(cv::Mat& out) const
    {
        std::lock_guard<std::mutex> lock(
            implementation_->latestFrameMutex);
        if (implementation_->latestCenteredFrame.empty())
        {
            return false;
        }
        out = implementation_->latestCenteredFrame;
        return true;
    }

    bool CaptureController::WaitForCenteredFrame(
        std::uint64_t afterSequence,
        cv::Mat& out,
        std::uint64_t& sequence,
        std::int64_t& captureTimestampNanoseconds,
        std::uint32_t timeoutMilliseconds) const
    {
        std::unique_lock<std::mutex> lock(
            implementation_->latestFrameMutex);
        const bool available =
            implementation_->latestFrameCondition.wait_for(
                lock,
                std::chrono::milliseconds(timeoutMilliseconds),
                [&]()
                {
                    return
                        implementation_->latestFrameSequence >
                            afterSequence &&
                        !implementation_->latestCenteredFrame.empty();
                });
        if (!available)
        {
            return false;
        }
        out = implementation_->latestCenteredFrame;
        sequence = implementation_->latestFrameSequence;
        captureTimestampNanoseconds =
            implementation_->latestFrameTimestampNanoseconds;
        return true;
    }
}
