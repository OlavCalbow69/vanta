#define IMGUI_DEFINE_MATH_OPERATORS

#include "rp2040_controller.hpp"

#include "logger.hpp"

#include "imgui.h"
#include "custom_widgets.hpp"
#include "font_defines.h"

#include <Windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace
{
    constexpr USHORT kRp2040Vid = 0x1E7D;
    constexpr USHORT kRp2040Pid = 0x2E2C;
    constexpr USAGE kVendorUsagePage = 0xFF00;
    constexpr USAGE kVendorUsage = 0x0001;
    constexpr std::uint8_t kOutputReportId = 2;
    constexpr std::size_t kFirmwarePayloadSize = 64;
    constexpr int kQuickClickHoldMinimumMilliseconds = 14;
    constexpr int kQuickClickHoldMaximumMilliseconds = 34;

    int HumanizedQuickClickHoldMilliseconds()
    {
        thread_local std::mt19937 generator{
            std::random_device{}()};
        std::uniform_int_distribution<int> distribution(
            kQuickClickHoldMinimumMilliseconds,
            kQuickClickHoldMaximumMilliseconds);
        return (
            distribution(generator) +
            distribution(generator) +
            1) /
            2;
    }

    std::string WideToUtf8(const wchar_t* text)
    {
        if (text == nullptr || *text == L'\0')
        {
            return {};
        }
        const int required = WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 1)
        {
            return {};
        }
        std::string result(
            static_cast<std::size_t>(required),
            '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            result.data(),
            required,
            nullptr,
            nullptr);
        result.pop_back();
        return result;
    }
}

namespace vanta
{
    struct Rp2040Controller::Implementation
    {
        mutable std::mutex deviceMutex;
        HANDLE device{INVALID_HANDLE_VALUE};
        std::atomic_bool connected{false};
        bool initialized{};
        std::size_t outputReportLength{
            kFirmwarePayloadSize + 1};
        std::wstring devicePath;
        std::string productName;
        std::string status{"RP2040 device not scanned"};
        int testMoveX{};
        int testMoveY{};
        bool releasePending{};

        void CloseLocked(bool attemptRelease = true)
        {
            if (attemptRelease &&
                device != INVALID_HANDLE_VALUE)
            {
                ForceReleaseLocked(false);
            }
            connected.store(false, std::memory_order_release);
            if (device != INVALID_HANDLE_VALUE)
            {
                CloseHandle(device);
                device = INVALID_HANDLE_VALUE;
            }
            devicePath.clear();
            productName.clear();
            outputReportLength =
                kFirmwarePayloadSize + 1;
        }

        bool WriteReportLocked(
            std::int16_t dx,
            std::int16_t dy,
            std::uint8_t buttons)
        {
            if (device == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            std::vector<std::uint8_t> report(
                outputReportLength,
                0);
            report[0] = kOutputReportId;
            // Complete 65-byte host report: report ID, int16 X, int16 Y,
            // then the application button bitmask.
            report[1] =
                static_cast<std::uint8_t>(dx & 0xFF);
            report[2] =
                static_cast<std::uint8_t>(
                    (static_cast<std::uint16_t>(dx) >> 8) &
                    0xFF);
            report[3] =
                static_cast<std::uint8_t>(dy & 0xFF);
            report[4] =
                static_cast<std::uint8_t>(
                    (static_cast<std::uint16_t>(dy) >> 8) &
                    0xFF);
            report[5] = buttons;

            DWORD written = 0;
            return
                WriteFile(
                    device,
                    report.data(),
                    static_cast<DWORD>(report.size()),
                    &written,
                    nullptr) &&
                written ==
                    static_cast<DWORD>(report.size());
        }

        bool ForceReleaseLocked(bool reportFailure = true)
        {
            if (device == INVALID_HANDLE_VALUE)
            {
                releasePending = true;
                return false;
            }

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                if (WriteReportLocked(0, 0, 0))
                {
                    if (releasePending)
                    {
                        vanta::log::Info(
                            "RP2040 left-button release recovered");
                    }
                    releasePending = false;
                    return true;
                }
                if (attempt != 2)
                {
                    Sleep(1);
                }
            }

            releasePending = true;
            status =
                "RP2040 left-button release failed; recovery pending";
            if (reportFailure)
            {
                vanta::log::Warning(
                    "RP2040 left-button release failed after 3 attempts");
            }
            return false;
        }

        bool Refresh()
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            CloseLocked();

            GUID hidGuid{};
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO information = SetupDiGetClassDevsW(
                &hidGuid,
                nullptr,
                nullptr,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (information == INVALID_HANDLE_VALUE)
            {
                status = "Could not enumerate HID devices";
                return false;
            }

            bool found = false;
            for (DWORD index = 0;; ++index)
            {
                SP_DEVICE_INTERFACE_DATA interfaceData{};
                interfaceData.cbSize =
                    sizeof(interfaceData);
                if (!SetupDiEnumDeviceInterfaces(
                        information,
                        nullptr,
                        &hidGuid,
                        index,
                        &interfaceData))
                {
                    break;
                }

                DWORD required = 0;
                SetupDiGetDeviceInterfaceDetailW(
                    information,
                    &interfaceData,
                    nullptr,
                    0,
                    &required,
                    nullptr);
                if (required <
                    sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                {
                    continue;
                }

                std::vector<std::uint8_t> storage(required);
                auto* detail =
                    reinterpret_cast<
                        SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                            storage.data());
                detail->cbSize =
                    sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(
                        information,
                        &interfaceData,
                        detail,
                        required,
                        nullptr,
                        nullptr))
                {
                    continue;
                }

                HANDLE candidate = CreateFileW(
                    detail->DevicePath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    0,
                    nullptr);
                if (candidate == INVALID_HANDLE_VALUE)
                {
                    continue;
                }

                HIDD_ATTRIBUTES attributes{};
                attributes.Size = sizeof(attributes);
                PHIDP_PREPARSED_DATA preparsed = nullptr;
                HIDP_CAPS capabilities{};
                const bool compatible =
                    HidD_GetAttributes(
                        candidate,
                        &attributes) &&
                    attributes.VendorID == kRp2040Vid &&
                    attributes.ProductID == kRp2040Pid &&
                    HidD_GetPreparsedData(
                        candidate,
                        &preparsed) &&
                    HidP_GetCaps(
                        preparsed,
                        &capabilities) ==
                        HIDP_STATUS_SUCCESS &&
                    capabilities.UsagePage ==
                        kVendorUsagePage &&
                    capabilities.Usage ==
                        kVendorUsage &&
                    capabilities.OutputReportByteLength >=
                        kFirmwarePayloadSize + 1;
                if (preparsed != nullptr)
                {
                    HidD_FreePreparsedData(preparsed);
                }
                if (!compatible)
                {
                    CloseHandle(candidate);
                    continue;
                }

                std::array<wchar_t, 256> product{};
                HidD_GetProductString(
                    candidate,
                    product.data(),
                    static_cast<ULONG>(
                        product.size() * sizeof(wchar_t)));
                device = candidate;
                devicePath = detail->DevicePath;
                productName = WideToUtf8(product.data());
                outputReportLength =
                    capabilities.OutputReportByteLength;
                if (!ForceReleaseLocked())
                {
                    CloseLocked(false);
                    continue;
                }
                connected.store(
                    true,
                    std::memory_order_release);
                status =
                    "Connected to RP2040 USB HID bridge";
                found = true;
                break;
            }

            SetupDiDestroyDeviceInfoList(information);
            if (!found)
            {
                status =
                    "RP2040 HID bridge not found "
                    "(VID 1E7D, PID 2E2C)";
            }
            vanta::log::Info(
                "RP2040 HID scan: %s",
                found ? "connected" : "not found");
            return found;
        }

        bool SendLocked(
            std::int16_t dx,
            std::int16_t dy,
            std::uint8_t buttons)
        {
            if (device == INVALID_HANDLE_VALUE ||
                !connected.load(std::memory_order_acquire))
            {
                return false;
            }

            const bool succeeded =
                WriteReportLocked(dx, dy, buttons);
            if (!succeeded)
            {
                status =
                    "RP2040 HID write failed; rescan the device";
                CloseLocked(false);
            }
            return succeeded;
        }

        bool TryClick()
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (releasePending &&
                !ForceReleaseLocked())
            {
                return false;
            }
            if (device == INVALID_HANDLE_VALUE ||
                !connected.load(std::memory_order_acquire))
            {
                return false;
            }
            if (!WriteReportLocked(0, 0, 1))
            {
                releasePending = true;
                ForceReleaseLocked();
                status =
                    "RP2040 left-button press failed; rescan the device";
                CloseLocked(false);
                return false;
            }
            releasePending = true;
            Sleep(
                static_cast<DWORD>(
                    HumanizedQuickClickHoldMilliseconds()));
            return ForceReleaseLocked();
        }

        bool TryMove(int x, int y)
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (releasePending &&
                !ForceReleaseLocked())
            {
                return false;
            }
            while (x != 0 || y != 0)
            {
                const int stepX =
                    std::clamp(x, -127, 127);
                const int stepY =
                    std::clamp(y, -127, 127);
                if (!SendLocked(
                        static_cast<std::int16_t>(stepX),
                        static_cast<std::int16_t>(stepY),
                        0))
                {
                    return false;
                }
                x -= stepX;
                y -= stepY;
            }
            return true;
        }

        void Tick()
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (releasePending &&
                device != INVALID_HANDLE_VALUE)
            {
                ForceReleaseLocked();
            }
        }

        bool ForceReleaseLeftButton()
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            return ForceReleaseLocked();
        }

        void Disconnect()
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            if (device != INVALID_HANDLE_VALUE)
            {
                ForceReleaseLocked();
            }
            CloseLocked(false);
            status =
                "RP2040 ready; use Rescan to connect";
        }

        void Shutdown()
        {
            if (!initialized)
            {
                return;
            }
            std::lock_guard<std::mutex> lock(deviceMutex);
            ForceReleaseLocked();
            CloseLocked(false);
            status = "RP2040 HID bridge released";
            initialized = false;
            vanta::log::Info(
                "RP2040 HID runtime released");
        }
    };

    Rp2040Controller::Rp2040Controller()
        : implementation_(
              std::make_unique<Implementation>())
    {
    }

    Rp2040Controller::~Rp2040Controller()
    {
        Shutdown();
    }

    void Rp2040Controller::Initialize()
    {
        if (implementation_->initialized)
        {
            return;
        }
        implementation_->initialized = true;
        implementation_->status =
            "RP2040 ready; use Rescan to connect";
        vanta::log::Info(
            "RP2040 HID runtime loaded without auto-connect");
    }

    void Rp2040Controller::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }

    void Rp2040Controller::Tick()
    {
        implementation_->Tick();
    }

    void Rp2040Controller::RenderPanel()
    {
        auto& implementation = *implementation_;
        const bool isConnected =
            implementation.connected.load(
                std::memory_order_acquire);
        std::string status;
        std::string productName;
        {
            std::lock_guard<std::mutex> lock(
                implementation.deviceMutex);
            status = implementation.status;
            productName =
                implementation.productName;
        }
        ImGui::TextColored(
            isConnected
                ? ImVec4(0.45F, 0.96F, 0.65F, 1.0F)
                : ImVec4(0.72F, 0.72F, 0.78F, 1.0F),
            "%s",
            status.c_str());
        ImGui::TextDisabled(
            "USB HID OUT | VID:PID 1E7D:2E2C | Report ID 2");
        if (!productName.empty())
        {
            ImGui::Text(
                "Product: %s",
                productName.c_str());
        }
        ImGui::Spacing();
        if (custom::Button(
                ICON_REFRESH_1_LINE "  Rescan RP2040",
                ImVec2(180.0F, 36.0F)))
        {
            implementation.Refresh();
        }

        if (isConnected)
        {
            ImGui::Spacing();
            ImGui::Separator();
            custom::SliderInt(
                "Horizontal movement",
                &implementation.testMoveX,
                -500,
                500,
                "%d px");
            custom::SliderInt(
                "Vertical movement",
                &implementation.testMoveY,
                -500,
                500,
                "%d px");
            if (custom::Button(
                    ICON_MOUSE_LINE "  Move",
                    ImVec2(145.0F, 36.0F)))
            {
                const bool result =
                    implementation.TryMove(
                        implementation.testMoveX,
                        implementation.testMoveY);
                vanta::log::Info(
                    "RP2040 test move: %s",
                    result ? "success" : "failed");
            }
            ImGui::SameLine();
            if (custom::Button(
                    "Left click",
                    ImVec2(130.0F, 36.0F)))
            {
                const bool result =
                    implementation.TryClick();
                vanta::log::Info(
                    "RP2040 test click: %s",
                    result ? "success" : "failed");
            }
        }
    }

    bool Rp2040Controller::TryAutoConnect()
    {
        return implementation_->Refresh();
    }

    void Rp2040Controller::Disconnect()
    {
        implementation_->Disconnect();
    }

    bool Rp2040Controller::TryClick()
    {
        return implementation_->TryClick();
    }

    bool Rp2040Controller::TryMove(int x, int y)
    {
        return implementation_->TryMove(x, y);
    }

    bool Rp2040Controller::ForceReleaseLeftButton()
    {
        return implementation_->ForceReleaseLeftButton();
    }

    bool Rp2040Controller::IsConnected() const noexcept
    {
        return implementation_->connected.load(
            std::memory_order_acquire);
    }
}
