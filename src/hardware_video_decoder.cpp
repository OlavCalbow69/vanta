#include "hardware_video_decoder.hpp"

#include <Windows.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <opencv2/core.hpp>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr std::size_t kMaximumQueuedFrames = 3;
    constexpr std::size_t kMaximumCompressedFrameBytes = 32U * 1024U * 1024U;
    constexpr std::size_t kPresentationTextureCount = 3;

    [[nodiscard]] std::string HResultMessage(const HRESULT result)
    {
        std::array<wchar_t, 512> buffer{};
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            static_cast<DWORD>(result),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr);
        if (length == 0)
        {
            char numeric[32]{};
            std::snprintf(numeric, sizeof(numeric), "0x%08lX", static_cast<unsigned long>(result));
            return numeric;
        }
        const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        std::string message(static_cast<std::size_t>(utf8Length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length), message.data(), utf8Length, nullptr, nullptr);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        {
            message.pop_back();
        }
        return message;
    }

    [[nodiscard]] bool IsD3D11AwareSynchronousTransform(IMFTransform* transform)
    {
        ComPtr<IMFAttributes> attributes;
        UINT32 d3d11Aware{};
        UINT32 asynchronous{};
        return SUCCEEDED(transform->GetAttributes(&attributes)) &&
               SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware)) &&
               d3d11Aware != 0 &&
               (!SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous)) || asynchronous == 0);
    }

    void PreferLowLatency(IMFTransform* transform)
    {
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes)))
        {
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        // The Windows H.264 MFT exposes this optional codec setting on
        // supported systems. A decoder that does not implement it simply
        // ignores the best-effort request.
        ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codecApi))))
        {
            VARIANT enabled{};
            VariantInit(&enabled);
            enabled.vt = VT_BOOL;
            enabled.boolVal = VARIANT_TRUE;
            codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &enabled);
            VariantClear(&enabled);
        }
    }

    struct EncodedFrame
    {
        std::vector<unsigned char> data;
        std::uint64_t presentationTimeUs{};
        int frameType{};
    };
}

namespace vanta
{
    struct HardwareVideoDecoder::Implementation
    {
        mutable std::mutex stateMutex;
        std::condition_variable queueCondition;
        std::condition_variable readyCondition;
        std::thread decodeThread;
        std::deque<EncodedFrame> queue;
        std::deque<std::vector<unsigned char>> reusableCompressedBuffers;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> deviceContext;
        ComPtr<ID3D11VideoDevice> videoDevice;
        ComPtr<ID3D11VideoContext> videoContext;
        ComPtr<IMFDXGIDeviceManager> deviceManager;
        UINT deviceManagerToken{};
        bool mediaFoundationStarted{};

        mutable std::mutex frameMutex;
        std::array<ComPtr<ID3D11Texture2D>, kPresentationTextureCount> presentationTextures;
        std::array<ComPtr<ID3D11VideoProcessorOutputView>, kPresentationTextureCount> presentationOutputViews;
        std::array<ComPtr<ID3D11ShaderResourceView>, kPresentationTextureCount> presentationShaderViews;
        ComPtr<ID3D11Texture2D> cpuReadbackTexture;
        ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator;
        ComPtr<ID3D11VideoProcessor> processor;
        DXGI_FORMAT processorInputFormat{DXGI_FORMAT_UNKNOWN};
        int processorInputWidth{};
        int processorInputHeight{};
        std::size_t nextPresentationTexture{};
        std::size_t latestPresentationTexture{kPresentationTextureCount};
        std::uint64_t latestPresentationSequence{};

        bool workerStop{};
        bool workerReady{};
        bool workerFailed{};
        bool acceptingFrames{};
        int selectedFormat{};
        int configuredWidth{};
        int configuredHeight{};
        int configuredFps{};
        std::uint64_t receivedFrames{};
        std::uint64_t submittedFrames{};
        std::uint64_t decodedFrames{};
        std::uint64_t droppedFrames{};
        std::chrono::steady_clock::time_point frameRateWindowStart{};
        std::uint64_t frameRateWindowReceivedFrames{};
        std::uint64_t frameRateWindowDecodedFrames{};
        double receivedFramesPerSecond{};
        double decodedFramesPerSecond{};
        std::string status{"Hardware decoder is not configured."};
        std::string startupError;

        void ResetFrameRatesLocked()
        {
            receivedFrames = 0;
            submittedFrames = 0;
            decodedFrames = 0;
            droppedFrames = 0;
            frameRateWindowStart = std::chrono::steady_clock::now();
            frameRateWindowReceivedFrames = 0;
            frameRateWindowDecodedFrames = 0;
            receivedFramesPerSecond = 0.0;
            decodedFramesPerSecond = 0.0;
        }

        // Update only a few times per second. This keeps the displayed rate
        // stable while still showing a stopped or bottlenecked stream quickly.
        void UpdateFrameRatesLocked()
        {
            const auto now = std::chrono::steady_clock::now();
            if (frameRateWindowStart.time_since_epoch().count() == 0)
            {
                frameRateWindowStart = now;
                frameRateWindowReceivedFrames = receivedFrames;
                frameRateWindowDecodedFrames = decodedFrames;
                return;
            }

            const double elapsedSeconds = std::chrono::duration<double>(
                now - frameRateWindowStart).count();
            if (elapsedSeconds < 0.5)
            {
                return;
            }

            receivedFramesPerSecond = static_cast<double>(
                receivedFrames - frameRateWindowReceivedFrames) /
                elapsedSeconds;
            decodedFramesPerSecond = static_cast<double>(
                decodedFrames - frameRateWindowDecodedFrames) /
                elapsedSeconds;
            frameRateWindowStart = now;
            frameRateWindowReceivedFrames = receivedFrames;
            frameRateWindowDecodedFrames = decodedFrames;
        }

        void StopDecodeThread()
        {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                acceptingFrames = false;
                workerStop = true;
                queue.clear();
                reusableCompressedBuffers.clear();
            }
            queueCondition.notify_all();
            readyCondition.notify_all();
            if (decodeThread.joinable())
            {
                decodeThread.join();
            }
        }

        void ClearPresentationResources()
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            for (auto& texture : presentationTextures) texture.Reset();
            for (auto& view : presentationOutputViews) view.Reset();
            for (auto& view : presentationShaderViews) view.Reset();
            cpuReadbackTexture.Reset();
            processor.Reset();
            processorEnumerator.Reset();
            processorInputFormat = DXGI_FORMAT_UNKNOWN;
            processorInputWidth = 0;
            processorInputHeight = 0;
            nextPresentationTexture = 0;
            latestPresentationTexture = kPresentationTextureCount;
            // Keep the sequence monotonic across a stream resize/reconfigure
            // so Capture's Sunshine source never mistakes a new frame for an
            // already consumed pre-resize frame.
        }

        [[nodiscard]] bool SelectNv12OutputType(IMFTransform* transform, std::string& error)
        {
            HRESULT result{};
            for (DWORD index = 0;; ++index)
            {
                ComPtr<IMFMediaType> outputType;
                result = transform->GetOutputAvailableType(0, index, &outputType);
                if (result == MF_E_NO_MORE_TYPES)
                {
                    break;
                }
                if (FAILED(result))
                {
                    continue;
                }
                GUID subtype{};
                if (FAILED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12)
                {
                    continue;
                }
                if (SUCCEEDED(transform->SetOutputType(0, outputType.Get(), 0)))
                {
                    return true;
                }
            }

            error = "The Windows H.264 hardware decoder has no D3D11 NV12 output format";
            return false;
        }

        [[nodiscard]] bool SetInitialInputAndOutputTypes(IMFTransform* transform, std::string& error)
        {
            ComPtr<IMFMediaType> inputType;
            HRESULT result = MFCreateMediaType(&inputType);
            if (FAILED(result) ||
                FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                // common-c assembles one complete Annex-B picture per decode
                // unit (including SPS/PPS before an IDR), which is exactly
                // the contract for MFVideoFormat_H264. H264_ES is for
                // arbitrary fragmented elementary-stream samples.
                FAILED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
                FAILED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, configuredWidth, configuredHeight)) ||
                FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, std::max(1, configuredFps), 1)) ||
                FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
                // H.264 may signal progressive/interlaced structure in-band.
                // This value lets the decoder honour either form.
                FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive)) ||
                FAILED(transform->SetInputType(0, inputType.Get(), 0)))
            {
                error = "The Windows H.264 hardware decoder rejected Vanta's input format";
                return false;
            }
            return SelectNv12OutputType(transform, error);
        }

        [[nodiscard]] bool CreateTransform(ComPtr<IMFTransform>& transform, MFT_OUTPUT_STREAM_INFO& outputInfo, std::string& error)
        {
            MFT_REGISTER_TYPE_INFO inputType{MFMediaType_Video, MFVideoFormat_H264};
            MFT_REGISTER_TYPE_INFO outputType{MFMediaType_Video, MFVideoFormat_NV12};
            IMFActivate** activations{};
            UINT32 count{};
            HRESULT result = MFTEnumEx(
                MFT_CATEGORY_VIDEO_DECODER,
                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                &inputType,
                &outputType,
                &activations,
                &count);
            if (FAILED(result) || count == 0)
            {
                error = "Windows did not expose a compatible H.264 decoder";
                return false;
            }

            const auto releaseActivations = [&]()
            {
                for (UINT32 index = 0; index < count; ++index)
                {
                    if (activations[index] != nullptr)
                    {
                        activations[index]->Release();
                    }
                }
                CoTaskMemFree(activations);
            };

            for (UINT32 index = 0; index < count; ++index)
            {
                ComPtr<IMFTransform> candidate;
                result = activations[index]->ActivateObject(IID_PPV_ARGS(&candidate));
                if (FAILED(result) || !candidate || !IsD3D11AwareSynchronousTransform(candidate.Get()))
                {
                    continue;
                }

                result = candidate->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(deviceManager.Get()));
                if (FAILED(result))
                {
                    continue;
                }
                PreferLowLatency(candidate.Get());
                if (!SetInitialInputAndOutputTypes(candidate.Get(), error))
                {
                    candidate->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
                    continue;
                }
                result = candidate->GetOutputStreamInfo(0, &outputInfo);
                if (FAILED(result))
                {
                    candidate->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
                    continue;
                }
                candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                transform = std::move(candidate);
                releaseActivations();
                return true;
            }

            releaseActivations();
            if (error.empty())
            {
                error = "No Windows H.264 decoder accepted Vanta's D3D11 hardware device";
            }
            return false;
        }

        [[nodiscard]] bool EnsureVideoProcessor(const DXGI_FORMAT inputFormat, const int width, const int height, std::string& error)
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (processor && processorInputFormat == inputFormat && processorInputWidth == width && processorInputHeight == height)
            {
                return true;
            }

            for (auto& texture : presentationTextures) texture.Reset();
            for (auto& view : presentationOutputViews) view.Reset();
            for (auto& view : presentationShaderViews) view.Reset();
            cpuReadbackTexture.Reset();
            processor.Reset();
            processorEnumerator.Reset();
            latestPresentationTexture = kPresentationTextureCount;
            nextPresentationTexture = 0;

            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputFrameRate.Numerator = static_cast<UINT>(std::max(1, configuredFps));
            content.InputFrameRate.Denominator = 1;
            content.InputWidth = static_cast<UINT>(width);
            content.InputHeight = static_cast<UINT>(height);
            content.OutputFrameRate = content.InputFrameRate;
            content.OutputWidth = static_cast<UINT>(width);
            content.OutputHeight = static_cast<UINT>(height);
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

            HRESULT result = videoDevice->CreateVideoProcessorEnumerator(&content, &processorEnumerator);
            if (FAILED(result) || FAILED(videoDevice->CreateVideoProcessor(processorEnumerator.Get(), 0, &processor)))
            {
                error = "Could not create Vanta's GPU NV12-to-BGRA video processor";
                return false;
            }
            videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);

            // The GameStream request explicitly negotiates SDR Rec.709 with
            // limited-range YCbCr. Spell that out to the video processor so
            // 1080p/4K frames are not treated as the legacy Rec.601 default
            // or rendered with a washed-out range in ImGui's BGRA texture.
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
            inputColorSpace.Usage = 0; // playback
            inputColorSpace.YCbCr_Matrix = 1; // Rec.709
            inputColorSpace.YCbCr_xvYCC = 0;
            inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inputColorSpace);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
            outputColorSpace.Usage = 0; // playback
            outputColorSpace.RGB_Range = 0; // full-range BGRA texture
            outputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &outputColorSpace);

            D3D11_TEXTURE2D_DESC textureDescription{};
            textureDescription.Width = static_cast<UINT>(width);
            textureDescription.Height = static_cast<UINT>(height);
            textureDescription.MipLevels = 1;
            textureDescription.ArraySize = 1;
            textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            textureDescription.SampleDesc.Count = 1;
            textureDescription.Usage = D3D11_USAGE_DEFAULT;
            textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDescription{};
            outputDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDescription.Texture2D.MipSlice = 0;
            for (std::size_t index = 0; index < kPresentationTextureCount; ++index)
            {
                result = device->CreateTexture2D(&textureDescription, nullptr, &presentationTextures[index]);
                if (FAILED(result) ||
                    FAILED(videoDevice->CreateVideoProcessorOutputView(
                        presentationTextures[index].Get(),
                        processorEnumerator.Get(),
                        &outputDescription,
                        &presentationOutputViews[index])) ||
                    FAILED(device->CreateShaderResourceView(
                        presentationTextures[index].Get(),
                        nullptr,
                        &presentationShaderViews[index])))
                {
                    error = "Could not create Vanta's GPU presentation texture";
                    for (auto& texture : presentationTextures) texture.Reset();
                    for (auto& view : presentationOutputViews) view.Reset();
                    for (auto& view : presentationShaderViews) view.Reset();
                    processor.Reset();
                    processorEnumerator.Reset();
                    return false;
                }
            }

            processorInputFormat = inputFormat;
            processorInputWidth = width;
            processorInputHeight = height;
            return true;
        }

        [[nodiscard]] bool PresentDecodedSample(IMFSample* sample, std::string& error)
        {
            ComPtr<IMFMediaBuffer> mediaBuffer;
            if (FAILED(sample->GetBufferByIndex(0, &mediaBuffer)))
            {
                error = "The Windows decoder output is missing a video buffer";
                return false;
            }
            ComPtr<IMFDXGIBuffer> dxgiBuffer;
            if (FAILED(mediaBuffer.As(&dxgiBuffer)))
            {
                error = "The Windows decoder did not return a GPU video buffer";
                return false;
            }
            ComPtr<ID3D11Texture2D> sourceTexture;
            UINT subresource{};
            if (FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&sourceTexture))) ||
                FAILED(dxgiBuffer->GetSubresourceIndex(&subresource)))
            {
                error = "Could not access the decoded GPU video surface";
                return false;
            }
            D3D11_TEXTURE2D_DESC sourceDescription{};
            sourceTexture->GetDesc(&sourceDescription);
            if (sourceDescription.Format != DXGI_FORMAT_NV12)
            {
                error = "The Windows hardware decoder returned an unsupported video surface format";
                return false;
            }
            if (!EnsureVideoProcessor(sourceDescription.Format, static_cast<int>(sourceDescription.Width), static_cast<int>(sourceDescription.Height), error))
            {
                return false;
            }

            std::lock_guard<std::mutex> lock(frameMutex);
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
            inputDescription.FourCC = 0;
            inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputDescription.Texture2D.MipSlice = 0;
            inputDescription.Texture2D.ArraySlice = subresource;
            ComPtr<ID3D11VideoProcessorInputView> inputView;
            const HRESULT result = videoDevice->CreateVideoProcessorInputView(
                sourceTexture.Get(), processorEnumerator.Get(), &inputDescription, &inputView);
            if (FAILED(result))
            {
                error = "Could not create Vanta's GPU decoded-frame view";
                return false;
            }
            const std::size_t target = nextPresentationTexture;
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.OutputIndex = 0;
            stream.pInputSurface = inputView.Get();
            if (FAILED(videoContext->VideoProcessorBlt(
                    processor.Get(),
                    presentationOutputViews[target].Get(),
                    0,
                    1,
                    &stream)))
            {
                error = "Could not convert Vanta's decoded GPU frame for rendering";
                return false;
            }
            latestPresentationTexture = target;
            ++latestPresentationSequence;
            nextPresentationTexture = (target + 1) % kPresentationTextureCount;
            return true;
        }

        [[nodiscard]] bool CopyLatestFrameToBgra(
            const std::uint64_t afterSequence,
            cv::Mat& output,
            std::uint64_t& sequence)
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (latestPresentationTexture >= kPresentationTextureCount ||
                latestPresentationSequence <= afterSequence ||
                device == nullptr ||
                deviceContext == nullptr)
            {
                return false;
            }

            ID3D11Texture2D* source = presentationTextures[latestPresentationTexture].Get();
            if (source == nullptr)
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            source->GetDesc(&description);
            if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
                description.Width == 0 ||
                description.Height == 0)
            {
                return false;
            }

            bool recreateReadbackTexture = cpuReadbackTexture == nullptr;
            if (!recreateReadbackTexture)
            {
                D3D11_TEXTURE2D_DESC existing{};
                cpuReadbackTexture->GetDesc(&existing);
                recreateReadbackTexture =
                    existing.Width != description.Width ||
                    existing.Height != description.Height ||
                    existing.Format != description.Format;
            }
            if (recreateReadbackTexture)
            {
                cpuReadbackTexture.Reset();
                description.BindFlags = 0;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                description.Usage = D3D11_USAGE_STAGING;
                description.MiscFlags = 0;
                description.MipLevels = 1;
                description.ArraySize = 1;
                if (FAILED(device->CreateTexture2D(&description, nullptr, &cpuReadbackTexture)))
                {
                    return false;
                }
            }

            deviceContext->CopyResource(cpuReadbackTexture.Get(), source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(deviceContext->Map(cpuReadbackTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                return false;
            }
            const cv::Mat view(
                static_cast<int>(description.Height),
                static_cast<int>(description.Width),
                CV_8UC4,
                mapped.pData,
                mapped.RowPitch);
            cv::Mat copied;
            try
            {
                copied = view.clone();
            }
            catch (...)
            {
                deviceContext->Unmap(cpuReadbackTexture.Get(), 0);
                return false;
            }
            deviceContext->Unmap(cpuReadbackTexture.Get(), 0);
            if (copied.empty())
            {
                return false;
            }
            output = std::move(copied);
            sequence = latestPresentationSequence;
            return true;
        }

        [[nodiscard]] bool CreateGpuOutputSample(
            IMFTransform* transform,
            ComPtr<IMFSample>& sample,
            std::string& error)
        {
            if (transform == nullptr || device == nullptr)
            {
                error = "The Windows hardware decoder has no D3D11 output device";
                return false;
            }

            ComPtr<IMFMediaType> outputType;
            UINT32 width{};
            UINT32 height{};
            GUID subtype{};
            if (FAILED(transform->GetOutputCurrentType(0, &outputType)) ||
                outputType == nullptr ||
                FAILED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                FAILED(MFGetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
                width == 0 || height == 0 || subtype != MFVideoFormat_NV12)
            {
                error = "The Windows hardware decoder did not expose an NV12 D3D11 output type";
                return false;
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_NV12;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_DECODER;

            ComPtr<ID3D11Texture2D> texture;
            ComPtr<IMFMediaBuffer> buffer;
            const HRESULT result =
                device->CreateTexture2D(&description, nullptr, &texture);
            if (FAILED(result) ||
                FAILED(MFCreateDXGISurfaceBuffer(
                    __uuidof(ID3D11Texture2D),
                    texture.Get(),
                    0,
                    FALSE,
                    &buffer)) ||
                FAILED(MFCreateSample(&sample)) ||
                FAILED(sample->AddBuffer(buffer.Get())))
            {
                sample.Reset();
                error = "Could not allocate a D3D11 output surface for the Windows H.264 decoder";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool DrainOutput(IMFTransform* transform, MFT_OUTPUT_STREAM_INFO& outputInfo, std::string& error)
        {
            for (;;)
            {
                MFT_OUTPUT_DATA_BUFFER output{};
                ComPtr<IMFSample> suppliedSample;
                if ((outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
                {
                    if (!CreateGpuOutputSample(transform, suppliedSample, error))
                    {
                        return false;
                    }
                    output.pSample = suppliedSample.Get();
                }

                DWORD processStatus{};
                const HRESULT result = transform->ProcessOutput(0, 1, &output, &processStatus);
                if (output.pEvents != nullptr)
                {
                    output.pEvents->Release();
                }
                if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
                {
                    return true;
                }
                if (result == MF_E_TRANSFORM_STREAM_CHANGE)
                {
                    // MF_E_TRANSFORM_STREAM_CHANGE means the decoder learned
                    // the real stream format from SPS/PPS. Reapplying the
                    // input type here can reset that parsed keyframe, so only
                    // renegotiate the output side as required by the MFT
                    // contract.
                    if (!SelectNv12OutputType(transform, error) || FAILED(transform->GetOutputStreamInfo(0, &outputInfo)))
                    {
                        if (error.empty()) error = "The Windows hardware decoder could not renegotiate its output format";
                        return false;
                    }
                    continue;
                }
                if (FAILED(result))
                {
                    error = "The Windows hardware decoder could not produce a video frame: " + HResultMessage(result);
                    return false;
                }
                if (output.pSample == nullptr)
                {
                    error = "The Windows hardware decoder produced an empty video frame";
                    return false;
                }

                ComPtr<IMFSample> transformSample;
                if (output.pSample != suppliedSample.Get())
                {
                    transformSample.Attach(output.pSample);
                }
                if (!PresentDecodedSample(output.pSample, error))
                {
                    return false;
                }
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    ++decodedFrames;
                    UpdateFrameRatesLocked();
                    status = "Receiving H.264 through the GPU hardware decoder.";
                }
            }
        }

        [[nodiscard]] bool DecodeFrame(
            IMFTransform* transform,
            MFT_OUTPUT_STREAM_INFO& outputInfo,
            const EncodedFrame& frame,
            std::string& error)
        {
            ComPtr<IMFSample> inputSample;
            ComPtr<IMFMediaBuffer> inputBuffer;
            HRESULT result = MFCreateSample(&inputSample);
            if (FAILED(result) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame.data.size()), &inputBuffer)))
            {
                error = "Could not allocate a Media Foundation input sample";
                return false;
            }
            BYTE* destination{};
            DWORD maximumLength{};
            const HRESULT lockResult = inputBuffer->Lock(&destination, &maximumLength, nullptr);
            if (FAILED(lockResult))
            {
                error = "Could not write a compressed Sunshine video frame";
                return false;
            }
            if (maximumLength < frame.data.size())
            {
                inputBuffer->Unlock();
                error = "The Media Foundation input buffer was smaller than the Sunshine video frame";
                return false;
            }
            std::memcpy(destination, frame.data.data(), frame.data.size());
            inputBuffer->Unlock();
            if (FAILED(inputBuffer->SetCurrentLength(static_cast<DWORD>(frame.data.size()))) ||
                FAILED(inputSample->AddBuffer(inputBuffer.Get())))
            {
                error = "Could not prepare a compressed Sunshine video frame";
                return false;
            }
            if (frame.frameType == FRAME_TYPE_IDR)
            {
                inputSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
            }
            if (FAILED(inputSample->SetSampleTime(static_cast<LONGLONG>(frame.presentationTimeUs * 10ULL))) ||
                FAILED(inputSample->SetSampleDuration(10000000LL / std::max(1, configuredFps))))
            {
                error = "Could not timestamp a compressed Sunshine video frame";
                return false;
            }

            result = transform->ProcessInput(0, inputSample.Get(), 0);
            if (result == MF_E_NOTACCEPTING)
            {
                if (!DrainOutput(transform, outputInfo, error))
                {
                    return false;
                }
                result = transform->ProcessInput(0, inputSample.Get(), 0);
            }
            if (FAILED(result))
            {
                error = "The Windows hardware decoder rejected a Sunshine video frame: " + HResultMessage(result);
                return false;
            }
            return DrainOutput(transform, outputInfo, error);
        }

        void DecodeThreadMain()
        {
            const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool apartmentInitialized = SUCCEEDED(apartmentResult);
            ComPtr<IMFTransform> transform;
            MFT_OUTPUT_STREAM_INFO outputInfo{};
            std::string error;
            const bool initialized = apartmentInitialized && CreateTransform(transform, outputInfo, error);
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                workerReady = initialized;
                workerFailed = !initialized;
                startupError = initialized ? std::string{} : (apartmentInitialized ? error : "Could not initialize the Media Foundation decoding thread");
                status = initialized ? "H.264 hardware decoder ready." : startupError;
            }
            readyCondition.notify_all();
            if (!initialized)
            {
                if (apartmentInitialized) CoUninitialize();
                return;
            }

            // Some D3D11 video MFTs finish GPU work just after the immediate
            // ProcessOutput call made by DecodeFrame(). Give them a few short
            // output polls before sleeping for more network input. Without
            // this, the final frame of a stream can remain held until another
            // packet happens to arrive.
            unsigned int pendingOutputPolls{};
            for (;;)
            {
                EncodedFrame frame;
                bool hasFrame{};
                {
                    std::unique_lock<std::mutex> lock(stateMutex);
                    if (queue.empty() && pendingOutputPolls != 0)
                    {
                        queueCondition.wait_for(
                            lock,
                            std::chrono::milliseconds(1),
                            [this]() { return workerStop || !queue.empty(); });
                    }
                    else
                    {
                        queueCondition.wait(lock, [this]() { return workerStop || !queue.empty(); });
                    }
                    if (workerStop)
                    {
                        break;
                    }
                    if (!queue.empty())
                    {
                        frame = std::move(queue.front());
                        queue.pop_front();
                        hasFrame = true;
                    }
                }

                if (!hasFrame)
                {
                    --pendingOutputPolls;
                    if (!DrainOutput(transform.Get(), outputInfo, error))
                    {
                        {
                            std::lock_guard<std::mutex> lock(stateMutex);
                            ++droppedFrames;
                            status = error;
                        }
                        LiRequestIdrFrame();
                        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                        pendingOutputPolls = 0;
                    }
                    continue;
                }
                if (!DecodeFrame(transform.Get(), outputInfo, frame, error))
                {
                    {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        ++droppedFrames;
                        status = error;
                    }
                    // The queue is intentionally shallow for LAN latency. If
                    // the decoder rejects an access unit, ask Sunshine for a
                    // clean keyframe rather than continuing with broken refs.
                    LiRequestIdrFrame();
                    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                    pendingOutputPolls = 0;
                }
                else
                {
                    pendingOutputPolls = 8;
                }

                // Returning the allocation to the producer avoids a new
                // multi-megabyte heap allocation for every GameStream frame.
                // Keep only the few buffers that can be in flight in Vanta's
                // intentionally shallow low-latency queue.
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (!workerStop && !frame.data.empty() && reusableCompressedBuffers.size() < kMaximumQueuedFrames)
                    {
                        frame.data.clear();
                        reusableCompressedBuffers.push_back(std::move(frame.data));
                    }
                }
            }

            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            if (apartmentInitialized) CoUninitialize();
        }
    };

    HardwareVideoDecoder::HardwareVideoDecoder() : implementation_(std::make_unique<Implementation>()) {}
    HardwareVideoDecoder::~HardwareVideoDecoder() { Shutdown(); }

    bool HardwareVideoDecoder::Initialize(ID3D11Device* device, std::string& error)
    {
        auto& impl = *implementation_;
        if (device == nullptr)
        {
            error = "Vanta's D3D11 device is not available";
            return false;
        }
        if (impl.mediaFoundationStarted)
        {
            return true;
        }
        const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(startupResult))
        {
            error = "Could not start Windows Media Foundation: " + HResultMessage(startupResult);
            return false;
        }
        impl.mediaFoundationStarted = true;
        impl.device = device;
        device->GetImmediateContext(&impl.deviceContext);
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&impl.videoDevice))) ||
            FAILED(impl.deviceContext.As(&impl.videoContext)))
        {
            error = "Vanta's D3D11 device does not expose its video interfaces";
            Shutdown();
            return false;
        }
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))))
        {
            multithread->SetMultithreadProtected(TRUE);
        }
        const HRESULT managerResult = MFCreateDXGIDeviceManager(&impl.deviceManagerToken, &impl.deviceManager);
        if (FAILED(managerResult) || FAILED(impl.deviceManager->ResetDevice(device, impl.deviceManagerToken)))
        {
            error = "Could not create Media Foundation's D3D11 device manager";
            Shutdown();
            return false;
        }
        return true;
    }

    void HardwareVideoDecoder::Shutdown()
    {
        if (implementation_ == nullptr)
        {
            return;
        }
        auto& impl = *implementation_;
        impl.StopDecodeThread();
        impl.ClearPresentationResources();
        impl.deviceManager.Reset();
        impl.videoContext.Reset();
        impl.videoDevice.Reset();
        impl.deviceContext.Reset();
        impl.device.Reset();
        if (impl.mediaFoundationStarted)
        {
            MFShutdown();
            impl.mediaFoundationStarted = false;
        }
    }

    int HardwareVideoDecoder::Configure(const int videoFormat, const int width, const int height, const int redrawRate, std::string& error)
    {
        auto& impl = *implementation_;
        if (!impl.mediaFoundationStarted || impl.device == nullptr)
        {
            error = "Vanta's Media Foundation hardware decoder is not initialized";
            return -1;
        }
        if (videoFormat != VIDEO_FORMAT_H264)
        {
            error = "Vanta's first native decoder supports H.264 only; select H.264 in Sunshine";
            return -1;
        }
        if (width <= 0 || height <= 0 || redrawRate <= 0)
        {
            error = "Sunshine sent invalid H.264 stream dimensions";
            return -1;
        }

        impl.StopDecodeThread();
        impl.ClearPresentationResources();
        {
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            impl.selectedFormat = videoFormat;
            impl.configuredWidth = width;
            impl.configuredHeight = height;
            impl.configuredFps = redrawRate;
            impl.ResetFrameRatesLocked();
            impl.workerStop = false;
            impl.workerReady = false;
            impl.workerFailed = false;
            impl.acceptingFrames = false;
            impl.startupError.clear();
            impl.status = "Starting the H.264 hardware decoder...";
        }
        impl.decodeThread = std::thread(&Implementation::DecodeThreadMain, &impl);

        std::unique_lock<std::mutex> lock(impl.stateMutex);
        if (!impl.readyCondition.wait_for(lock, std::chrono::seconds(10), [&impl]() { return impl.workerReady || impl.workerFailed; }))
        {
            impl.workerStop = true;
            lock.unlock();
            impl.queueCondition.notify_all();
            impl.StopDecodeThread();
            error = "Timed out while starting Windows' H.264 hardware decoder";
            return -1;
        }
        if (impl.workerFailed)
        {
            error = impl.startupError;
            lock.unlock();
            impl.StopDecodeThread();
            return -1;
        }
        return 0;
    }

    void HardwareVideoDecoder::Start()
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(impl.stateMutex);
        if (impl.workerReady && !impl.workerStop)
        {
            impl.acceptingFrames = true;
            impl.status = "Waiting for Sunshine H.264 video frames...";
        }
    }

    void HardwareVideoDecoder::Stop()
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(impl.stateMutex);
        impl.acceptingFrames = false;
        impl.queue.clear();
        if (!impl.workerStop)
        {
            impl.status = "Sunshine video stream stopped.";
        }
    }

    int HardwareVideoDecoder::Submit(PDECODE_UNIT decodeUnit)
    {
        auto& impl = *implementation_;
        if (decodeUnit == nullptr || decodeUnit->bufferList == nullptr || decodeUnit->fullLength <= 0 ||
            static_cast<std::size_t>(decodeUnit->fullLength) > kMaximumCompressedFrameBytes)
        {
            return DR_NEED_IDR;
        }

        EncodedFrame frame;
        frame.presentationTimeUs = decodeUnit->presentationTimeUs;
        frame.frameType = decodeUnit->frameType;

        {
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            if (!impl.acceptingFrames || !impl.workerReady || impl.workerStop)
            {
                ++impl.droppedFrames;
                return DR_NEED_IDR;
            }
            ++impl.receivedFrames;
            impl.UpdateFrameRatesLocked();
            if (impl.queue.size() >= kMaximumQueuedFrames)
            {
                ++impl.droppedFrames;
                return DR_NEED_IDR;
            }
            if (!impl.reusableCompressedBuffers.empty())
            {
                frame.data = std::move(impl.reusableCompressedBuffers.back());
                impl.reusableCompressedBuffers.pop_back();
            }
        }

        const auto recycleFrameBuffer = [&impl](std::vector<unsigned char>&& buffer)
        {
            if (buffer.empty())
            {
                return;
            }
            buffer.clear();
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            if (!impl.workerStop && impl.reusableCompressedBuffers.size() < kMaximumQueuedFrames)
            {
                impl.reusableCompressedBuffers.push_back(std::move(buffer));
            }
        };

        try
        {
            frame.data.clear();
            frame.data.reserve(static_cast<std::size_t>(decodeUnit->fullLength));
            for (PLENTRY entry = decodeUnit->bufferList; entry != nullptr; entry = entry->next)
            {
                if (entry->data == nullptr || entry->length <= 0 ||
                    frame.data.size() + static_cast<std::size_t>(entry->length) > kMaximumCompressedFrameBytes)
                {
                    recycleFrameBuffer(std::move(frame.data));
                    return DR_NEED_IDR;
                }
                frame.data.insert(
                    frame.data.end(),
                    reinterpret_cast<const unsigned char*>(entry->data),
                    reinterpret_cast<const unsigned char*>(entry->data) + entry->length);
            }
        }
        catch (const std::bad_alloc&)
        {
            recycleFrameBuffer(std::move(frame.data));
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            ++impl.droppedFrames;
            return DR_NEED_IDR;
        }
        if (frame.data.size() != static_cast<std::size_t>(decodeUnit->fullLength))
        {
            recycleFrameBuffer(std::move(frame.data));
            return DR_NEED_IDR;
        }

        {
            std::lock_guard<std::mutex> lock(impl.stateMutex);
            if (!impl.acceptingFrames || !impl.workerReady || impl.workerStop || impl.queue.size() >= kMaximumQueuedFrames)
            {
                ++impl.droppedFrames;
                const bool canRecycle = !impl.workerStop && impl.reusableCompressedBuffers.size() < kMaximumQueuedFrames;
                if (canRecycle)
                {
                    frame.data.clear();
                    impl.reusableCompressedBuffers.push_back(std::move(frame.data));
                }
                return DR_NEED_IDR;
            }
            impl.queue.push_back(std::move(frame));
            ++impl.submittedFrames;
        }
        impl.queueCondition.notify_one();
        return DR_OK;
    }

    ID3D11ShaderResourceView* HardwareVideoDecoder::AcquireFrameView() const
    {
        const auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(impl.frameMutex);
        if (impl.latestPresentationTexture >= kPresentationTextureCount)
        {
            return nullptr;
        }
        ID3D11ShaderResourceView* view = impl.presentationShaderViews[impl.latestPresentationTexture].Get();
        if (view != nullptr)
        {
            view->AddRef();
        }
        return view;
    }

    bool HardwareVideoDecoder::CopyLatestFrameToBgra(
        const std::uint64_t afterSequence,
        cv::Mat& output,
        std::uint64_t& sequence)
    {
        return implementation_->CopyLatestFrameToBgra(afterSequence, output, sequence);
    }

    DecodedVideoStats HardwareVideoDecoder::GetStats() const
    {
        auto& impl = *implementation_;
        std::lock_guard<std::mutex> lock(impl.stateMutex);
        impl.UpdateFrameRatesLocked();
        return DecodedVideoStats{
            impl.configuredWidth,
            impl.configuredHeight,
            impl.configuredFps,
            impl.receivedFramesPerSecond,
            impl.decodedFramesPerSecond,
            impl.receivedFrames,
            impl.submittedFrames,
            impl.decodedFrames,
            impl.droppedFrames,
            impl.workerReady && !impl.workerFailed,
            impl.status};
    }
}
