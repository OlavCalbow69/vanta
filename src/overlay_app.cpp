#include "overlay_app.hpp"

#include "capture_controller.hpp"
#include "target_detection.hpp"
#include "target_detection_ui.hpp"
#include "bomb_timer_controller.hpp"
#include "config_manager.hpp"
#include "hardware_video_decoder.hpp"
#include "logger.hpp"
#include "makcu_controller.hpp"
#include "pretty_menu.hpp"
#include "sunshine_pairing.hpp"
#include "testclick_controller.hpp"
#include "testmove_controller.hpp"
#include "update_controller.hpp"
#include "stream_receiver_controller.hpp"
#include "version.hpp"

#include <Windows.h>
#include <Windowsx.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

    constexpr int kMenuToggleHotkeyId = 0x5601;
    constexpr int kExitEndHotkeyId = 0x5602;
    constexpr int kExitDeleteHotkeyId = 0x5603;
    constexpr int kBombTimerResetHotkeyId = 0x5604;

    struct Options
    {
        std::uint64_t selfTestFrames{};
        int captureTestBackend{};
        bool updatePageTest{};
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
        options.updatePageTest = HasArgument(
            argumentCount,
            arguments,
            L"--update-page-test");
        return options;
    }

    constexpr UINT32 kReceiverSelfTestWidth = 640;
    constexpr UINT32 kReceiverSelfTestHeight = 480;
    constexpr UINT32 kReceiverSelfTestFps = 30;
    constexpr DWORD kReceiverSelfTestBitrate = 800000;
    constexpr std::size_t kEncoderOutputBufferBytes = 1024U * 1024U;
    constexpr std::size_t kReceiverSelfTestAccessUnitCount = 8;

    class ComApartmentScope
    {
    public:
        ComApartmentScope() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~ComApartmentScope()
        {
            if (SUCCEEDED(result_))
            {
                CoUninitialize();
            }
        }

        [[nodiscard]] bool IsUsable() const noexcept
        {
            // RPC_E_CHANGED_MODE means the caller has an existing apartment.
            // COM objects are still usable on that already-initialized thread.
            return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
        }

        [[nodiscard]] HRESULT Result() const noexcept { return result_; }

    private:
        HRESULT result_{};
    };

    [[nodiscard]] std::string HResultText(const HRESULT result)
    {
        char text[32]{};
        std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
        return text;
    }

    [[nodiscard]] bool StartsWithAnnexB(const BYTE* data, const std::size_t length)
    {
        return data != nullptr &&
            ((length >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
             (length >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1));
    }

    [[nodiscard]] std::uint32_t ReadBigEndianUint32(const BYTE* data) noexcept
    {
        return
            (static_cast<std::uint32_t>(data[0]) << 24U) |
            (static_cast<std::uint32_t>(data[1]) << 16U) |
            (static_cast<std::uint32_t>(data[2]) << 8U) |
            static_cast<std::uint32_t>(data[3]);
    }

    void AppendAnnexBStartCode(std::vector<unsigned char>& output)
    {
        output.push_back(0);
        output.push_back(0);
        output.push_back(0);
        output.push_back(1);
    }

    [[nodiscard]] bool AppendNalUnitsAsAnnexB(
        const BYTE* data,
        const std::size_t length,
        const DWORD* naluLengths,
        const std::size_t naluCount,
        std::vector<unsigned char>& output)
    {
        if (data == nullptr || naluLengths == nullptr || naluCount == 0)
        {
            return false;
        }

        std::size_t payloadLength{};
        for (std::size_t index = 0; index < naluCount; ++index)
        {
            if (naluLengths[index] == 0 ||
                payloadLength > std::numeric_limits<std::size_t>::max() - naluLengths[index])
            {
                return false;
            }
            payloadLength += naluLengths[index];
        }

        std::size_t prefixLength{};
        if (payloadLength == length)
        {
            prefixLength = 0;
        }
        else if (naluCount <= (std::numeric_limits<std::size_t>::max() - payloadLength) / 4U &&
                 payloadLength + naluCount * 4U == length)
        {
            prefixLength = 4;
        }
        else
        {
            return false;
        }

        std::size_t position{};
        for (std::size_t index = 0; index < naluCount; ++index)
        {
            const std::size_t naluLength = naluLengths[index];
            if (position > length || prefixLength > length - position || naluLength > length - position - prefixLength)
            {
                return false;
            }
            if (prefixLength != 0 && ReadBigEndianUint32(data + position) != naluLength)
            {
                return false;
            }
            position += prefixLength;
            AppendAnnexBStartCode(output);
            output.insert(output.end(), data + position, data + position + naluLength);
            position += naluLength;
        }
        return position == length;
    }

    [[nodiscard]] bool ConvertLengthPrefixedH264ToAnnexB(
        const BYTE* data,
        const std::size_t length,
        std::vector<unsigned char>& output)
    {
        if (data == nullptr || length < 5)
        {
            return false;
        }

        std::size_t position{};
        while (position < length)
        {
            if (length - position < 4)
            {
                return false;
            }
            const std::size_t naluLength = ReadBigEndianUint32(data + position);
            position += 4;
            if (naluLength == 0 || naluLength > length - position)
            {
                return false;
            }
            AppendAnnexBStartCode(output);
            output.insert(output.end(), data + position, data + position + naluLength);
            position += naluLength;
        }
        return !output.empty();
    }

    [[nodiscard]] bool ConvertEncodedH264SampleToAnnexB(
        IMFSample* sample,
        std::vector<unsigned char>& output,
        std::string& error)
    {
        if (sample == nullptr)
        {
            error = "The H.264 test encoder produced an empty sample";
            return false;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || buffer == nullptr)
        {
            error = "The H.264 test encoder did not expose its output bytes";
            return false;
        }

        BYTE* data{};
        DWORD maximumLength{};
        DWORD currentLength{};
        if (FAILED(buffer->Lock(&data, &maximumLength, &currentLength)) || data == nullptr || currentLength == 0)
        {
            error = "The H.264 test encoder output could not be read";
            return false;
        }

        bool converted{};
        if (StartsWithAnnexB(data, currentLength))
        {
            output.assign(data, data + currentLength);
            converted = true;
        }
        else
        {
            UINT32 naluInfoSize{};
            const HRESULT naluInfoResult = sample->GetBlobSize(MF_NALU_LENGTH_INFORMATION, &naluInfoSize);
            if (SUCCEEDED(naluInfoResult) &&
                naluInfoSize >= sizeof(DWORD) &&
                naluInfoSize % sizeof(DWORD) == 0)
            {
                std::vector<DWORD> naluLengths(naluInfoSize / sizeof(DWORD));
                converted = SUCCEEDED(sample->GetBlob(
                    MF_NALU_LENGTH_INFORMATION,
                    reinterpret_cast<BYTE*>(naluLengths.data()),
                    naluInfoSize,
                    nullptr)) &&
                    AppendNalUnitsAsAnnexB(
                        data,
                        currentLength,
                        naluLengths.data(),
                        naluLengths.size(),
                        output);
            }
            else
            {
                converted = ConvertLengthPrefixedH264ToAnnexB(data, currentLength, output);
            }
        }
        buffer->Unlock();

        if (!converted || output.empty())
        {
            error = "The H.264 test encoder did not return a complete Annex-B access unit";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ContainsH264NaluType(
        const std::vector<unsigned char>& accessUnit,
        const unsigned char naluType)
    {
        for (std::size_t index = 0; index + 3 < accessUnit.size(); ++index)
        {
            std::size_t startCodeLength{};
            if (accessUnit[index] == 0 && accessUnit[index + 1] == 0 && accessUnit[index + 2] == 1)
            {
                startCodeLength = 3;
            }
            else if (index + 4 < accessUnit.size() &&
                     accessUnit[index] == 0 && accessUnit[index + 1] == 0 &&
                     accessUnit[index + 2] == 0 && accessUnit[index + 3] == 1)
            {
                startCodeLength = 4;
            }
            if (startCodeLength != 0)
            {
                const std::size_t naluStart = index + startCodeLength;
                if (naluStart < accessUnit.size() && (accessUnit[naluStart] & 0x1FU) == naluType)
                {
                    return true;
                }
                index = naluStart;
            }
        }
        return false;
    }

    [[nodiscard]] std::string H264NaluTypeSummary(const std::vector<unsigned char>& accessUnit)
    {
        std::string result;
        for (std::size_t index = 0; index + 3 < accessUnit.size(); ++index)
        {
            std::size_t startCodeLength{};
            if (accessUnit[index] == 0 && accessUnit[index + 1] == 0 && accessUnit[index + 2] == 1)
            {
                startCodeLength = 3;
            }
            else if (index + 4 < accessUnit.size() &&
                     accessUnit[index] == 0 && accessUnit[index + 1] == 0 &&
                     accessUnit[index + 2] == 0 && accessUnit[index + 3] == 1)
            {
                startCodeLength = 4;
            }
            if (startCodeLength != 0)
            {
                const std::size_t naluStart = index + startCodeLength;
                if (naluStart < accessUnit.size())
                {
                    if (!result.empty()) result += ',';
                    result += std::to_string(accessUnit[naluStart] & 0x1FU);
                }
                index = naluStart;
            }
        }
        return result.empty() ? "none" : result;
    }

    [[nodiscard]] bool AppendSequenceHeaderAsAnnexB(
        IMFMediaType* outputType,
        std::vector<unsigned char>& accessUnit,
        std::string& error)
    {
        if (outputType == nullptr)
        {
            error = "The H.264 test encoder did not expose its output type";
            return false;
        }

        UINT32 sequenceHeaderSize{};
        const HRESULT sizeResult = outputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sequenceHeaderSize);
        if (FAILED(sizeResult) || sequenceHeaderSize == 0)
        {
            error = "The H.264 test encoder did not provide SPS/PPS data";
            return false;
        }

        std::vector<BYTE> sequenceHeader(sequenceHeaderSize);
        if (FAILED(outputType->GetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER,
                sequenceHeader.data(),
                sequenceHeaderSize,
                nullptr)))
        {
            error = "The H.264 test encoder sequence header could not be read";
            return false;
        }
        if (StartsWithAnnexB(sequenceHeader.data(), sequenceHeader.size()))
        {
            accessUnit.insert(accessUnit.begin(), sequenceHeader.begin(), sequenceHeader.end());
            return true;
        }

        // H.264 encoder media types commonly store the configuration record
        // in AVCDecoderConfigurationRecord form. Convert its SPS and PPS
        // payloads to the Annex-B form Sunshine/Moonlight delivers.
        if (sequenceHeader.size() < 7 || sequenceHeader[0] != 1)
        {
            error = "The H.264 test encoder returned an unknown SPS/PPS format";
            return false;
        }
        std::vector<unsigned char> converted;
        std::size_t position = 5;
        const std::size_t spsCount = sequenceHeader[position++] & 0x1FU;
        if (spsCount == 0)
        {
            error = "The H.264 test encoder did not return an SPS";
            return false;
        }
        for (std::size_t index = 0; index < spsCount; ++index)
        {
            if (position + 2 > sequenceHeader.size())
            {
                error = "The H.264 test encoder returned a truncated SPS";
                return false;
            }
            const std::size_t naluLength =
                (static_cast<std::size_t>(sequenceHeader[position]) << 8U) |
                sequenceHeader[position + 1];
            position += 2;
            if (naluLength == 0 || naluLength > sequenceHeader.size() - position)
            {
                error = "The H.264 test encoder returned an invalid SPS";
                return false;
            }
            AppendAnnexBStartCode(converted);
            converted.insert(converted.end(), sequenceHeader.begin() + static_cast<std::ptrdiff_t>(position), sequenceHeader.begin() + static_cast<std::ptrdiff_t>(position + naluLength));
            position += naluLength;
        }
        if (position >= sequenceHeader.size())
        {
            error = "The H.264 test encoder did not return a PPS";
            return false;
        }
        const std::size_t ppsCount = sequenceHeader[position++];
        if (ppsCount == 0)
        {
            error = "The H.264 test encoder did not return a PPS";
            return false;
        }
        for (std::size_t index = 0; index < ppsCount; ++index)
        {
            if (position + 2 > sequenceHeader.size())
            {
                error = "The H.264 test encoder returned a truncated PPS";
                return false;
            }
            const std::size_t naluLength =
                (static_cast<std::size_t>(sequenceHeader[position]) << 8U) |
                sequenceHeader[position + 1];
            position += 2;
            if (naluLength == 0 || naluLength > sequenceHeader.size() - position)
            {
                error = "The H.264 test encoder returned an invalid PPS";
                return false;
            }
            AppendAnnexBStartCode(converted);
            converted.insert(converted.end(), sequenceHeader.begin() + static_cast<std::ptrdiff_t>(position), sequenceHeader.begin() + static_cast<std::ptrdiff_t>(position + naluLength));
            position += naluLength;
        }
        accessUnit.insert(accessUnit.begin(), converted.begin(), converted.end());
        return true;
    }

    [[nodiscard]] bool BuildH264DecoderTestAccessUnits(
        std::vector<std::vector<unsigned char>>& accessUnits,
        std::string& error)
    {
        ComPtr<IMFTransform> encoder;
        HRESULT result = CoCreateInstance(
            CLSID_CMSH264EncoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&encoder));
        if (FAILED(result) || encoder == nullptr)
        {
            error = "Windows' inbox H.264 encoder is unavailable: " + HResultText(result);
            return false;
        }

        ComPtr<IMFMediaType> outputType;
        ComPtr<IMFMediaType> inputType;
        result = MFCreateMediaType(&outputType);
        if (FAILED(result) ||
            FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
            FAILED(outputType->SetUINT32(MF_MT_AVG_BITRATE, kReceiverSelfTestBitrate)) ||
            FAILED(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, kReceiverSelfTestWidth, kReceiverSelfTestHeight)) ||
            FAILED(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, kReceiverSelfTestFps, 1)) ||
            FAILED(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base)) ||
            FAILED(outputType->SetUINT32(MF_NALU_LENGTH_SET, TRUE)) ||
            FAILED(encoder->SetOutputType(0, outputType.Get(), 0)))
        {
            error = "Windows' H.264 test encoder rejected its output type";
            return false;
        }

        result = MFCreateMediaType(&inputType);
        if (FAILED(result) ||
            FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
            FAILED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, kReceiverSelfTestWidth, kReceiverSelfTestHeight)) ||
            FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, kReceiverSelfTestFps, 1)) ||
            FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, kReceiverSelfTestWidth)) ||
            FAILED(encoder->SetInputType(0, inputType.Get(), 0)))
        {
            error = "Windows' H.264 test encoder rejected its NV12 input type";
            return false;
        }

        ComPtr<IMFMediaType> negotiatedOutputType;
        if (FAILED(encoder->GetOutputCurrentType(0, &negotiatedOutputType)) || negotiatedOutputType == nullptr ||
            FAILED(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        {
            error = "Windows' H.264 test encoder could not begin encoding";
            return false;
        }

        MFT_OUTPUT_STREAM_INFO outputInfo{};
        if (FAILED(encoder->GetOutputStreamInfo(0, &outputInfo)))
        {
            error = "Windows' H.264 test encoder did not expose output allocation information";
            return false;
        }

        const auto processOutput = [&]() -> bool
        {
            for (;;)
            {
                MFT_OUTPUT_DATA_BUFFER output{};
                ComPtr<IMFSample> suppliedSample;
                if ((outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
                {
                    ComPtr<IMFMediaBuffer> outputBuffer;
                    if (FAILED(MFCreateSample(&suppliedSample)) ||
                        FAILED(MFCreateMemoryBuffer(std::max<DWORD>(outputInfo.cbSize, static_cast<DWORD>(kEncoderOutputBufferBytes)), &outputBuffer)) ||
                        FAILED(suppliedSample->AddBuffer(outputBuffer.Get())))
                    {
                        error = "Windows' H.264 test encoder could not allocate output memory";
                        return false;
                    }
                    output.pSample = suppliedSample.Get();
                }

                DWORD outputStatus{};
                const HRESULT outputResult = encoder->ProcessOutput(0, 1, &output, &outputStatus);
                if (output.pEvents != nullptr)
                {
                    output.pEvents->Release();
                }
                if (outputResult == MF_E_TRANSFORM_NEED_MORE_INPUT)
                {
                    return true;
                }
                if (outputResult == MF_E_TRANSFORM_STREAM_CHANGE)
                {
                    if (FAILED(encoder->GetOutputCurrentType(0, &negotiatedOutputType)) ||
                        FAILED(encoder->GetOutputStreamInfo(0, &outputInfo)))
                    {
                        error = "Windows' H.264 test encoder could not renegotiate its output";
                        return false;
                    }
                    continue;
                }
                if (FAILED(outputResult) || output.pSample == nullptr)
                {
                    error = "Windows' H.264 test encoder could not produce a frame: " + HResultText(outputResult);
                    return false;
                }

                ComPtr<IMFSample> encodedSample;
                if (output.pSample == suppliedSample.Get())
                {
                    encodedSample = suppliedSample;
                }
                else
                {
                    encodedSample.Attach(output.pSample);
                }
                std::vector<unsigned char> encodedAccessUnit;
                if (!ConvertEncodedH264SampleToAnnexB(encodedSample.Get(), encodedAccessUnit, error))
                {
                    return false;
                }
                if (ContainsH264NaluType(encodedAccessUnit, 5))
                {
                    if (!ContainsH264NaluType(encodedAccessUnit, 7) ||
                        !ContainsH264NaluType(encodedAccessUnit, 8))
                    {
                        if (!AppendSequenceHeaderAsAnnexB(negotiatedOutputType.Get(), encodedAccessUnit, error))
                        {
                            return false;
                        }
                    }
                    if (ContainsH264NaluType(encodedAccessUnit, 7) &&
                        ContainsH264NaluType(encodedAccessUnit, 8))
                    {
                        // Start at the next independently decodable frame so
                        // the decoder sees the same IDR + follow-up access
                        // units it receives from Sunshine.
                        if (accessUnits.empty())
                        {
                            accessUnits.push_back(std::move(encodedAccessUnit));
                        }
                        else if (accessUnits.size() < kReceiverSelfTestAccessUnitCount)
                        {
                            accessUnits.push_back(std::move(encodedAccessUnit));
                        }
                    }
                }
                else if (!accessUnits.empty() && accessUnits.size() < kReceiverSelfTestAccessUnitCount)
                {
                    accessUnits.push_back(std::move(encodedAccessUnit));
                }
                if (accessUnits.size() >= kReceiverSelfTestAccessUnitCount)
                {
                    return true;
                }
            }
        };

        const std::size_t inputBytes =
            static_cast<std::size_t>(kReceiverSelfTestWidth) *
            static_cast<std::size_t>(kReceiverSelfTestHeight) * 3U / 2U;
        for (int frameIndex = 0; frameIndex < 16 && accessUnits.size() < kReceiverSelfTestAccessUnitCount; ++frameIndex)
        {
            ComPtr<IMFSample> inputSample;
            ComPtr<IMFMediaBuffer> inputBuffer;
            if (FAILED(MFCreateSample(&inputSample)) ||
                FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(inputBytes), &inputBuffer)))
            {
                error = "Windows' H.264 test encoder could not allocate an NV12 test frame";
                return false;
            }
            BYTE* input{};
            DWORD maximumLength{};
            if (FAILED(inputBuffer->Lock(&input, &maximumLength, nullptr)) || maximumLength < inputBytes)
            {
                error = "Windows' H.264 test encoder could not fill an NV12 test frame";
                return false;
            }
            const BYTE luminance = static_cast<BYTE>(48 + frameIndex * 12);
            std::memset(input, luminance, static_cast<std::size_t>(kReceiverSelfTestWidth) * kReceiverSelfTestHeight);
            std::memset(
                input + static_cast<std::size_t>(kReceiverSelfTestWidth) * kReceiverSelfTestHeight,
                128,
                inputBytes - static_cast<std::size_t>(kReceiverSelfTestWidth) * kReceiverSelfTestHeight);
            inputBuffer->Unlock();
            if (FAILED(inputBuffer->SetCurrentLength(static_cast<DWORD>(inputBytes))) ||
                FAILED(inputSample->AddBuffer(inputBuffer.Get())) ||
                FAILED(inputSample->SetSampleTime(static_cast<LONGLONG>(frameIndex) * 10000000LL / kReceiverSelfTestFps)) ||
                FAILED(inputSample->SetSampleDuration(10000000LL / kReceiverSelfTestFps)))
            {
                error = "Windows' H.264 test encoder could not prepare an NV12 test frame";
                return false;
            }
            if (frameIndex == 0)
            {
                inputSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
            }
            result = encoder->ProcessInput(0, inputSample.Get(), 0);
            if (result == MF_E_NOTACCEPTING)
            {
                if (!processOutput())
                {
                    return false;
                }
                result = encoder->ProcessInput(0, inputSample.Get(), 0);
            }
            if (FAILED(result) || !processOutput())
            {
                if (error.empty()) error = "Windows' H.264 test encoder rejected an NV12 test frame: " + HResultText(result);
                return false;
            }
        }

        if (accessUnits.size() < kReceiverSelfTestAccessUnitCount)
        {
            result = encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            if (FAILED(result) || !processOutput())
            {
                if (error.empty()) error = "Windows' H.264 test encoder could not drain: " + HResultText(result);
                return false;
            }
        }
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

        if (accessUnits.empty())
        {
            error = "Windows' H.264 test encoder did not produce an SPS/PPS IDR frame";
            return false;
        }
        return true;
    }

    bool RunReceiverDecoderSelfTest()
    {
        const ComApartmentScope apartment;
        if (!apartment.IsUsable())
        {
            vanta::log::Error(
                "receiver self-test could not initialize COM: %s",
                HResultText(apartment.Result()).c_str());
            return false;
        }
        constexpr D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL selected{};
        const HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
        if (FAILED(result))
        {
            vanta::log::Error(
                "receiver self-test could not create a hardware D3D11 video device: 0x%08lX",
                static_cast<unsigned long>(result));
            return false;
        }

        vanta::HardwareVideoDecoder decoder;
        std::string error;
        if (!decoder.Initialize(device.Get(), error) ||
            decoder.Configure(
                VIDEO_FORMAT_H264,
                static_cast<int>(kReceiverSelfTestWidth),
                static_cast<int>(kReceiverSelfTestHeight),
                static_cast<int>(kReceiverSelfTestFps),
                error) != 0)
        {
            vanta::log::Error(
                "receiver self-test failed: %s",
                error.c_str());
            decoder.Shutdown();
            return false;
        }

        std::vector<std::vector<unsigned char>> accessUnits;
        if (!BuildH264DecoderTestAccessUnits(accessUnits, error))
        {
            vanta::log::Error("receiver self-test could not create an H.264 test frame: %s", error.c_str());
            decoder.Shutdown();
            return false;
        }
        for (std::size_t index = 0; index < accessUnits.size(); ++index)
        {
            vanta::log::Info(
                "receiver self-test H.264 access unit %llu: %llu bytes; NAL types %s",
                static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(accessUnits[index].size()),
                H264NaluTypeSummary(accessUnits[index]).c_str());
        }
        decoder.Start();
        for (std::size_t index = 0; index < accessUnits.size(); ++index)
        {
            // common-c delivers each completed picture as an LENTRY chain.
            // Split this known-good Annex-B access unit across several
            // entries (including mid-NAL boundaries) to exercise Vanta's
            // callback copy path, rather than only testing a contiguous
            // synthetic buffer.
            constexpr std::size_t fragmentCount = 3;
            std::array<LENTRY, fragmentCount> entries{};
            const std::size_t firstBoundary = accessUnits[index].size() / fragmentCount;
            const std::size_t secondBoundary = firstBoundary * 2;
            const std::array<std::size_t, fragmentCount + 1> boundaries{
                0,
                firstBoundary,
                secondBoundary,
                accessUnits[index].size()};
            for (std::size_t fragment = 0; fragment < fragmentCount; ++fragment)
            {
                entries[fragment].data = reinterpret_cast<char*>(
                    accessUnits[index].data() + boundaries[fragment]);
                entries[fragment].length = static_cast<int>(
                    boundaries[fragment + 1] - boundaries[fragment]);
                entries[fragment].bufferType = BUFFER_TYPE_PICDATA;
                entries[fragment].next =
                    fragment + 1 < fragmentCount
                    ? &entries[fragment + 1]
                    : nullptr;
            }
            DECODE_UNIT decodeUnit{};
            decodeUnit.frameType = index == 0 ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME;
            decodeUnit.presentationTimeUs = static_cast<std::uint64_t>(index) * 1000000ULL / kReceiverSelfTestFps;
            decodeUnit.fullLength = static_cast<int>(accessUnits[index].size());
            decodeUnit.bufferList = entries.data();
            bool submitted{};
            for (int attempt = 0; attempt < 50 && !submitted; ++attempt)
            {
                submitted = decoder.Submit(&decodeUnit) == DR_OK;
                if (!submitted)
                {
                    Sleep(10);
                }
            }
            if (!submitted)
            {
                vanta::log::Error("receiver self-test could not submit its H.264 access unit");
                decoder.Shutdown();
                return false;
            }
        }

        bool decoded{};
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            const vanta::DecodedVideoStats stats = decoder.GetStats();
            if (stats.decodedFrames != 0)
            {
                ID3D11ShaderResourceView* frameView = decoder.AcquireFrameView();
                decoded =
                    frameView != nullptr &&
                    stats.width == static_cast<int>(kReceiverSelfTestWidth) &&
                    stats.height == static_cast<int>(kReceiverSelfTestHeight);
                if (frameView != nullptr)
                {
                    frameView->Release();
                }
                if (!decoded)
                {
                    error = "The H.264 decoder did not publish a GPU presentation frame";
                }
                break;
            }
            Sleep(10);
        }
        const vanta::DecodedVideoStats finalStats = decoder.GetStats();
        decoder.Shutdown();
        if (!decoded)
        {
            if (error.empty())
            {
                error =
                    "Timed out waiting for a GPU-decoded H.264 frame (submitted=" +
                    std::to_string(finalStats.submittedFrames) +
                    ", decoded=" + std::to_string(finalStats.decodedFrames) +
                    ", dropped=" + std::to_string(finalStats.droppedFrames) +
                    ", status=" + finalStats.status + ")";
            }
            vanta::log::Error(
                "receiver self-test failed: %s",
                error.c_str());
            return false;
        }
        vanta::log::Info("receiver self-test passed: a real H.264 IDR frame decoded into Vanta's D3D11 GPU output");
        return true;
    }

    std::string Utf8FromWide(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr);
        return result;
    }

    bool RunReceiverDiscoveryTest(const std::wstring& requestedHost)
    {
        const std::string host = Utf8FromWide(requestedHost);
        if (host.empty())
        {
            std::fprintf(stderr, "receiver discovery failed: the host argument is not valid UTF-8\n");
            return false;
        }

        vanta::sunshine::PairingService pairing;
        std::string error;
        const auto discovered = pairing.Discover(host, error);
        if (!discovered)
        {
            vanta::log::Error("receiver discovery failed: %s", error.c_str());
            std::fprintf(stderr, "receiver discovery failed: %s\n", error.c_str());
            return false;
        }
        vanta::log::Info(
            "Sunshine receiver discovery succeeded: %s | GameStream %s | HTTPS %u | codec flags 0x%X",
            discovered->name.empty() ? discovered->address.c_str() : discovered->name.c_str(),
            discovered->appVersion.c_str(),
            discovered->httpsPort,
            static_cast<unsigned int>(discovered->codecSupport));
        std::printf(
            "Sunshine host found: %s | GameStream %s | HTTPS %u | codec flags 0x%X\n",
            discovered->name.empty() ? discovered->address.c_str() : discovered->name.c_str(),
            discovered->appVersion.c_str(),
            discovered->httpsPort,
            static_cast<unsigned int>(discovered->codecSupport));
        return true;
    }

    bool RunReceiverLanDiscoveryTest()
    {
        vanta::sunshine::PairingService pairing;
        std::string error;
        const std::vector<vanta::sunshine::HostInfo> discovered = pairing.DiscoverLan(error);
        if (discovered.empty())
        {
            vanta::log::Error("LAN Sunshine discovery failed: %s", error.c_str());
            std::fprintf(stderr, "LAN Sunshine discovery failed: %s\n", error.c_str());
            return false;
        }

        for (const vanta::sunshine::HostInfo& host : discovered)
        {
            vanta::log::Info(
                "LAN Sunshine host found: %s | address %s | GameStream %s | HTTPS %u | codec flags 0x%X",
                host.name.empty() ? host.address.c_str() : host.name.c_str(),
                host.address.c_str(),
                host.appVersion.c_str(),
                host.httpsPort,
                static_cast<unsigned int>(host.codecSupport));
            std::printf(
                "Sunshine host found: %s | address %s | GameStream %s | HTTPS %u | codec flags 0x%X\n",
                host.name.empty() ? host.address.c_str() : host.name.c_str(),
                host.address.c_str(),
                host.appVersion.c_str(),
                host.httpsPort,
                static_cast<unsigned int>(host.codecSupport));
        }
        return true;
    }

    bool RunReceiverLiveStreamSmokeTest(const std::wstring& requestedHost)
    {
        const std::string host = Utf8FromWide(requestedHost);
        if (host.empty())
        {
            std::fprintf(stderr, "receiver live smoke test failed: the host argument is not valid UTF-8\n");
            return false;
        }

        const ComApartmentScope apartment;
        if (!apartment.IsUsable())
        {
            vanta::log::Error(
                "receiver live smoke test could not initialize COM: %s",
                HResultText(apartment.Result()).c_str());
            return false;
        }

        constexpr D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL selected{};
        const HRESULT createResult = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
        if (FAILED(createResult))
        {
            vanta::log::Error(
                "receiver live smoke test could not create a hardware D3D11 video device: 0x%08lX",
                static_cast<unsigned long>(createResult));
            return false;
        }

        vanta::StreamReceiverController receiver;
        receiver.Initialize(device.Get());
        vanta::SunshineStreamSmokeTestResult testResult;
        const bool passed = receiver.RunLiveStreamSmokeTest(
            host,
            60000,
            testResult);
        receiver.Shutdown();

        std::printf(
            "%s: %s | frame=%dx%d | submitted=%llu decoded=%llu dropped=%llu\n",
            passed ? "Sunshine live smoke test passed" : "Sunshine live smoke test failed",
            testResult.message.c_str(),
            testResult.width,
            testResult.height,
            static_cast<unsigned long long>(testResult.submittedFrames),
            static_cast<unsigned long long>(testResult.decodedFrames),
            static_cast<unsigned long long>(testResult.droppedFrames));
        return passed;
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
                !InitializeImGui())
            {
                Shutdown();
                return 1;
            }

            vanta::LocalConfig defaultConfig;
            defaultConfig.menu =
                vanta::menu::GetConfig();
            if (options_.selfTestFrames != 0)
            {
                defaultConfig.menu.activePage =
                    options_.updatePageTest ? 8 : 7;
                defaultConfig.bombTimer.enabled = true;
            }
            if (!configManager_.Initialize(
                    options_.selfTestFrames == 0,
                    defaultConfig))
            {
                Shutdown();
                return 1;
            }
            const vanta::LocalConfig& initialConfig =
                configManager_.InitialLocalConfig();
            vanta::menu::ApplyConfig(
                initialConfig.menu);
            updates_.Initialize(
                initialConfig.updates,
                options_.selfTestFrames == 0 &&
                    options_.captureTestBackend == 0 &&
                    VANTA_AUTOMATED_RELEASE_BUILD != 0);
            streamReceiver_.Initialize(device_.Get(), &initialConfig.sunshine);
            capture_.SetSunshineVideoFrameProvider(
                [this](
                    const std::uint64_t afterSequence,
                    cv::Mat& frame,
                    std::uint64_t& sequence)
                {
                    return streamReceiver_.CopyLatestFrameToBgra(
                        afterSequence,
                        frame,
                        sequence);
                });

            if (
                !capture_.Initialize(
                    device_.Get(),
                    context_.Get(),
                    GetCurrentProcessId(),
                    &initialConfig.capture) ||
                !makcu_.Initialize(
                    &initialConfig.mouseOutput) ||
                !CreateOutlineWindow() ||
                !CreateFovWindow())
            {
                Shutdown();
                return 1;
            }

            testClick_.Initialize(&capture_, &makcu_);
            testMove_.Initialize(&capture_, &makcu_);
            bombTimer_.Initialize(
                &capture_,
                device_.Get(),
                context_.Get(),
                window_,
                &initialConfig.bombTimer);
            if (options_.selfTestFrames != 0)
            {
                bombTimer_.StartManualCountdown();
            }
            configManager_.PrimeRevisions(
                capture_,
                makcu_,
                bombTimer_,
                updates_, streamReceiver_);
            if (options_.selfTestFrames == 0)
            {
                RegisterHotkeys();
            }

            vanta::log::Info(
                "overlay ready; Insert toggles the menu, Home resets the Bomb Timer, End/Delete close Vanta");
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

                UpdateDesktopBounds();
                ResizeIfNeeded();
                streamReceiver_.Tick();
                capture_.Tick();
                makcu_.Tick();
                testClick_.Tick();
                testMove_.Tick();
                bombTimer_.Tick();
                UpdateCaptureOutline();
                UpdateFovOutline();

                if (overlayVisible_)
                {
                    RenderFrame();
                }
                configManager_.AutoSaveLocal(
                    capture_,
                    makcu_,
                    bombTimer_,
                    updates_, streamReceiver_);

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
                (renderedFrames_ >=
                    options_.selfTestFrames &&
                 bombTimer_.HasCaptureFrame());
            if (options_.selfTestFrames != 0 &&
                !bombTimer_.HasCaptureFrame())
            {
                vanta::log::Error(
                    "self-test did not receive a Bomb Timer auxiliary frame");
            }
            Shutdown();
            return selfTestPassed ? 0 : 1;
        }

        LRESULT WindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            if (message == WM_HOTKEY)
            {
                HandleHotkey(static_cast<int>(wParam));
                return 0;
            }
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
                return (
                    vanta::menu::ContainsPoint(
                        static_cast<float>(point.x),
                        static_cast<float>(point.y)))
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
                D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
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

        bool CreateFovWindow()
        {
            Gdiplus::GdiplusStartupInput startupInput;
            if (Gdiplus::GdiplusStartup(
                    &gdiplusToken_,
                    &startupInput,
                    nullptr) != Gdiplus::Ok)
            {
                vanta::log::Error(
                    "FOV anti-aliasing initialization failed");
                return false;
            }

            WNDCLASSEXW windowClass{
                sizeof(WNDCLASSEXW),
                0,
                &StaticFovWindowProcedure,
                0,
                0,
                instance_,
                nullptr,
                LoadCursorW(nullptr, IDC_ARROW),
                nullptr,
                nullptr,
                L"VantaKillFovOutline",
                nullptr};
            if (RegisterClassExW(&windowClass) == 0)
            {
                vanta::log::Error(
                    "FOV outline RegisterClassExW failed: %lu",
                    GetLastError());
                return false;
            }
            fovClassRegistered_ = true;

            fovWindow_ = CreateWindowExW(
                WS_EX_TOPMOST |
                    WS_EX_TOOLWINDOW |
                    WS_EX_LAYERED |
                    WS_EX_TRANSPARENT |
                    WS_EX_NOACTIVATE,
                windowClass.lpszClassName,
                L"vanta kill FOV",
                WS_POPUP,
                0,
                0,
                1,
                1,
                nullptr,
                nullptr,
                instance_,
                this);
            if (fovWindow_ == nullptr)
            {
                vanta::log::Error(
                    "FOV outline CreateWindowExW failed: %lu",
                    GetLastError());
                return false;
            }

            SetWindowDisplayAffinity(
                fovWindow_,
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

        void RegisterHotkeys()
        {
            const auto registerHotkey = [this](
                const int identifier,
                const UINT virtualKey,
                const char* label,
                bool& registered)
            {
                if (RegisterHotKey(
                        window_,
                        identifier,
                        MOD_NOREPEAT,
                        virtualKey))
                {
                    registered = true;
                    vanta::log::Info(
                        "%s hotkey registered",
                        label);
                    return;
                }
                vanta::log::Warning(
                    "could not register %s hotkey: %lu",
                    label,
                    GetLastError());
            };

            // This uses Windows' registered-hotkey event delivery rather than
            // polling any Windows key state. MAKCU remains the exclusive
            // source for Vanta's mouse hold/aim buttons.
            registerHotkey(
                kMenuToggleHotkeyId,
                VK_INSERT,
                "Insert",
                menuToggleHotkeyRegistered_);
            registerHotkey(
                kExitEndHotkeyId,
                VK_END,
                "End",
                endExitHotkeyRegistered_);
            registerHotkey(
                kExitDeleteHotkeyId,
                VK_DELETE,
                "Delete",
                deleteExitHotkeyRegistered_);
            registerHotkey(
                kBombTimerResetHotkeyId,
                VK_HOME,
                "Home",
                bombTimerResetHotkeyRegistered_);
        }

        void UnregisterHotkeys()
        {
            const auto unregisterHotkey = [this](
                const int identifier,
                bool& registered)
            {
                if (!registered)
                {
                    return;
                }
                UnregisterHotKey(window_, identifier);
                registered = false;
            };

            unregisterHotkey(
                kMenuToggleHotkeyId,
                menuToggleHotkeyRegistered_);
            unregisterHotkey(
                kExitEndHotkeyId,
                endExitHotkeyRegistered_);
            unregisterHotkey(
                kExitDeleteHotkeyId,
                deleteExitHotkeyRegistered_);
            unregisterHotkey(
                kBombTimerResetHotkeyId,
                bombTimerResetHotkeyRegistered_);
        }

        void HandleHotkey(const int identifier)
        {
            switch (identifier)
            {
            case kMenuToggleHotkeyId:
                vanta::menu::Toggle();
                break;
            case kExitEndHotkeyId:
                RequestExit("End hotkey pressed");
                break;
            case kExitDeleteHotkeyId:
                RequestExit("Delete hotkey pressed");
                break;
            case kBombTimerResetHotkeyId:
                bombTimer_.ResetCountdown();
                vanta::log::Info("Bomb Timer reset with Home hotkey");
                break;
            default:
                break;
            }
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
            SetWindowRgn(window_, nullptr, TRUE);
            menuRegionRectangle_ = {};
            menuRegionApplied_ = false;
        }

        void UpdateMenuRegion()
        {
            // Hit testing already restricts input to the menu and timer
            // widget. Keep the native window region unrestricted so
            // click-through calibration graphics can render elsewhere.
            ClearMenuRegion();
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

        void UpdateFovOutline()
        {
            if (fovWindow_ == nullptr)
            {
                return;
            }

            RECT captureRectangle{};
            vanta::TestMoveFovOutline outline =
                testMove_.GetFovOutline();
            if (options_.selfTestFrames != 0)
            {
                outline.visible = true;
                outline.radius = 100;
                outline.color = {
                    0.68F,
                    0.56F,
                    0.91F,
                    1.0F};
            }
            const bool hasCapture =
                capture_.GetCaptureScreenRectangle(
                    captureRectangle);
            const int captureWidth =
                captureRectangle.right -
                captureRectangle.left;
            const int captureHeight =
                captureRectangle.bottom -
                captureRectangle.top;
            constexpr int margin = 3;
            const int maximumRadius =
                std::max(
                    0,
                    std::min(
                        captureWidth,
                        captureHeight) /
                        2 -
                        margin);
            const int radius =
                std::clamp(
                    outline.radius,
                    0,
                    maximumRadius);
            if (!outline.visible ||
                !hasCapture ||
                radius <= 0)
            {
                if (fovVisible_)
                {
                    ShowWindow(fovWindow_, SW_HIDE);
                    fovVisible_ = false;
                }
                return;
            }

            const auto channel = [](float value)
            {
                return static_cast<BYTE>(
                    std::lround(
                        std::clamp(
                            value,
                            0.0F,
                            1.0F) *
                        255.0F));
            };
            COLORREF color = RGB(
                channel(outline.color.red),
                channel(outline.color.green),
                channel(outline.color.blue));
            const BYTE alpha =
                channel(outline.color.alpha);
            const bool appearanceChanged =
                color != fovColor_ ||
                alpha != fovAlpha_ ||
                radius != fovRadius_;
            if (appearanceChanged)
            {
                fovColor_ = color;
                fovAlpha_ = alpha;
                fovRadius_ = radius;
                fovSurfaceDirty_ = true;
            }

            const int centerX =
                captureRectangle.left +
                captureWidth / 2;
            const int centerY =
                captureRectangle.top +
                captureHeight / 2;
            const RECT desired{
                centerX - radius - margin,
                centerY - radius - margin,
                centerX + radius + margin,
                centerY + radius + margin};
            const bool rectangleChanged =
                !SameRectangle(
                    desired,
                    fovRectangle_);
            if (appearanceChanged ||
                rectangleChanged ||
                !fovVisible_ ||
                fovSurfaceDirty_)
            {
                if (!RenderFovSurface(desired))
                {
                    fovSurfaceDirty_ = true;
                    if (fovVisible_)
                    {
                        ShowWindow(
                            fovWindow_,
                            SW_HIDE);
                        fovVisible_ = false;
                    }
                    return;
                }
                fovSurfaceDirty_ = false;
            }
            const ULONGLONG now =
                GetTickCount64();
            if (rectangleChanged ||
                !fovVisible_ ||
                now >= nextFovZOrderRefresh_)
            {
                fovRectangle_ = desired;
                UINT flags =
                    SWP_NOACTIVATE |
                    SWP_SHOWWINDOW |
                    SWP_NOOWNERZORDER;
                if (!rectangleChanged)
                {
                    flags |=
                        SWP_NOMOVE |
                        SWP_NOSIZE;
                }
                SetWindowPos(
                    fovWindow_,
                    vanta::menu::IsVisible() &&
                            IsWindowVisible(window_)
                        ? window_
                        : HWND_TOPMOST,
                    desired.left,
                    desired.top,
                    desired.right - desired.left,
                    desired.bottom - desired.top,
                    flags);
                nextFovZOrderRefresh_ =
                    now + 500;
            }
            fovVisible_ = true;
        }

        bool RenderFovSurface(const RECT& screenRectangle)
        {
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
            bitmapInfo.bmiHeader.biCompression =
                BI_RGB;

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
                graphics.Clear(
                    Gdiplus::Color(0, 0, 0, 0));

                const Gdiplus::RectF bounds(
                    3.0F,
                    3.0F,
                    static_cast<Gdiplus::REAL>(
                        width - 6),
                    static_cast<Gdiplus::REAL>(
                        height - 6));
                Gdiplus::Pen blackPen(
                    Gdiplus::Color(
                        fovAlpha_,
                        0,
                        0,
                        0),
                    3.0F);
                Gdiplus::Pen colorPen(
                    Gdiplus::Color(
                        fovAlpha_,
                        GetRValue(fovColor_),
                        GetGValue(fovColor_),
                        GetBValue(fovColor_)),
                    1.0F);
                blackPen.SetAlignment(
                    Gdiplus::PenAlignmentCenter);
                colorPen.SetAlignment(
                    Gdiplus::PenAlignmentCenter);

                rendered =
                    graphics.DrawEllipse(
                        &blackPen,
                        bounds) == Gdiplus::Ok &&
                    graphics.DrawEllipse(
                        &colorPen,
                        bounds) == Gdiplus::Ok;
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
                        fovWindow_,
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

            if (!rendered)
            {
                vanta::log::Warning(
                    "could not render anti-aliased FOV outline: %lu",
                    GetLastError());
            }
            return rendered;
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

        LRESULT FovWindowProcedure(
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
                bombTimer_,
                configManager_,
                makcu_,
                testClick_,
                testMove_,
                updates_,
                streamReceiver_,
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

            UnregisterHotkeys();

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
            if (fovWindow_ != nullptr &&
                IsWindow(fovWindow_))
            {
                ShowWindow(fovWindow_, SW_HIDE);
            }

            testClick_.Shutdown();
            testMove_.Shutdown();
            bombTimer_.Shutdown();
            capture_.Shutdown();
            streamReceiver_.Shutdown();
            makcu_.Shutdown();
            configManager_.Shutdown();

            if (outlineWindow_ != nullptr &&
                IsWindow(outlineWindow_))
            {
                DestroyWindow(outlineWindow_);
                outlineWindow_ = nullptr;
            }
            if (fovWindow_ != nullptr &&
                IsWindow(fovWindow_))
            {
                DestroyWindow(fovWindow_);
                fovWindow_ = nullptr;
            }
            if (gdiplusToken_ != 0)
            {
                Gdiplus::GdiplusShutdown(
                    gdiplusToken_);
                gdiplusToken_ = 0;
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
            if (fovClassRegistered_)
            {
                if (!UnregisterClassW(
                        L"VantaKillFovOutline",
                        instance_))
                {
                    vanta::log::Warning(
                        "could not unregister the FOV-outline class: %lu",
                        GetLastError());
                }
                fovClassRegistered_ = false;
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
            updates_.Shutdown(true);
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

        static LRESULT CALLBACK StaticFovWindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam)
        {
            auto* application =
                reinterpret_cast<OverlayApplication*>(
                    GetWindowLongPtrW(
                        window,
                        GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto* create =
                    reinterpret_cast<CREATESTRUCTW*>(
                        lParam);
                application =
                    static_cast<OverlayApplication*>(
                        create->lpCreateParams);
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(
                        application));
            }
            if (application != nullptr)
            {
                return application->
                    FovWindowProcedure(
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
        vanta::BombTimerController bombTimer_;
        vanta::ConfigManager configManager_;
        vanta::MakcuController makcu_;
        vanta::TestClickController testClick_;
        vanta::TestMoveController testMove_;
        vanta::UpdateController updates_;
        vanta::StreamReceiverController streamReceiver_;
        HWND window_{};
        HWND outlineWindow_{};
        HWND fovWindow_{};
        bool classRegistered_ = false;
        bool outlineClassRegistered_ = false;
        bool fovClassRegistered_ = false;
        bool exitRequested_ = false;
        bool shutdownStarted_ = false;
        bool imguiContextReady_ = false;
        bool imguiWin32Ready_ = false;
        bool imguiDx11Ready_ = false;
        bool imguiReady_ = false;
        bool menuToggleHotkeyRegistered_ = false;
        bool endExitHotkeyRegistered_ = false;
        bool deleteExitHotkeyRegistered_ = false;
        bool bombTimerResetHotkeyRegistered_ = false;
        bool overlayVisible_ = false;
        bool clickThrough_ = false;
        bool menuRegionApplied_ = false;
        bool outlineVisible_ = false;
        bool fovVisible_ = false;
        bool fovSurfaceDirty_ = true;
        UINT pendingWidth_ = 1280;
        UINT pendingHeight_ = 720;
        UINT swapChainWidth_ = 1280;
        UINT swapChainHeight_ = 720;
        std::uint64_t renderedFrames_{};
        ULONGLONG nextTopmostRefresh_{};
        ULONGLONG nextOutlineZOrderRefresh_{};
        ULONGLONG nextFovZOrderRefresh_{};
        RECT overlayRectangle_{};
        RECT menuRegionRectangle_{};
        RECT outlineRectangle_{};
        RECT fovRectangle_{};
        static constexpr COLORREF outlineTransparencyKey_ =
            RGB(1, 2, 3);
        COLORREF outlineColor_ = RGB(173, 143, 233);
        BYTE outlineAlpha_ = 255;
        int outlineThickness_ = 1;
        COLORREF fovColor_ = RGB(173, 143, 233);
        BYTE fovAlpha_ = 255;
        int fovRadius_ = 100;
        ULONG_PTR gdiplusToken_{};

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
                "  vanta.exe --config-self-test validate JSON in a temporary folder\n"
                "  vanta.exe --receiver-self-test validate H.264 D3D11 hardware decoding\n"
                "  vanta.exe --detector-self-test validate contour/color/aim/resolution handling\n"
                "  vanta.exe --detector-ui-self-test validate detector controls and preview draw lists\n"
                "  vanta.exe --makcu-self-test validate serial framing and baud handshake\n"
                "  vanta.exe --makcu-probe COMn query connected firmware without injecting moves\n"
                "  vanta.exe --receiver-capture-self-test validate Sunshine-to-capture frame delivery\n"
                "  vanta.exe --receiver-discover-lan auto-discover Sunshine hosts on this LAN\n"
                "  vanta.exe --receiver-discover HOST verify a Sunshine host without pairing\n"
                "  vanta.exe --receiver-stream-smoke HOST verify a pre-paired host sends a live H.264 frame\n"
                "  --update-page-test       render Updates during --self-test\n"
                "  --capture-test-winrt      test WinRT monitor capture\n"
                "  --capture-test-duplication test DXGI monitor capture\n"
                "  --capture-test-winrt-window test WinRT window capture\n"
                "  --capture-test-duplication-window test DXGI window crop\n");
            return 0;
        }
        if (HasArgument(argumentCount, arguments, L"--detector-self-test"))
            return RunTargetDetectionSelfTest() ? 0 : 1;
        if (HasArgument(argumentCount, arguments, L"--detector-ui-self-test"))
            return RunDetectionUiSelfTest() ? 0 : 1;
        if (HasArgument(argumentCount, arguments, L"--makcu-self-test"))
            return RunMakcuCompatibilitySelfTest() ? 0 : 1;
        for (int index=1;index+1<argumentCount;++index)
            if (wcscmp(arguments[index],L"--makcu-probe")==0)
            {
                const std::wstring port=arguments[index+1];
                return RunMakcuConnectionProbe(Utf8FromWide(port)) ? 0 : 1;
            }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--config-self-test"))
        {
            return RunConfigurationSelfTest()
                ? 0
                : 1;
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--receiver-self-test"))
        {
            return RunReceiverDecoderSelfTest()
                ? 0
                : 1;
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--receiver-capture-self-test"))
        {
            return RunSunshineCaptureSourceSelfTest()
                ? 0
                : 1;
        }
        if (HasArgument(
                argumentCount,
                arguments,
                L"--receiver-discover-lan"))
        {
            return RunReceiverLanDiscoveryTest()
                ? 0
                : 1;
        }
        if (const auto host = ArgumentValue(
                argumentCount,
                arguments,
                L"--receiver-discover"))
        {
            return RunReceiverDiscoveryTest(*host)
                ? 0
                : 1;
        }
        if (const auto host = ArgumentValue(
                argumentCount,
                arguments,
                L"--receiver-stream-smoke"))
        {
            return RunReceiverLiveStreamSmokeTest(*host)
                ? 0
                : 1;
        }

        return OverlayApplication(
            instance,
            ParseOptions(argumentCount, arguments))
            .Run();
    }
}
