#include "hardware_video_decoder.hpp"
#include "logger.hpp"
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d10_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "imgui.h"
#pragma warning(push,0)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/error.h>
}
#pragma warning(pop)
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

namespace
{
    using Microsoft::WRL::ComPtr;
    constexpr std::size_t kQueueLimit=3;
    constexpr std::size_t kMaximumFrameBytes=32*1024*1024;
    std::string Utf8(const wchar_t* value)
    {
        const int length=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
        if (length<2) return {};
        std::string result(length,'\0');
        WideCharToMultiByte(CP_UTF8,0,value,-1,result.data(),length,nullptr,nullptr);
        result.pop_back(); return result;
    }
    std::string AdapterId(const LUID& id)
    {
        char result[32]{}; std::snprintf(result,sizeof(result),"%08x:%08x",unsigned(id.HighPart),unsigned(id.LowPart));
        return result;
    }
    std::string AvError(int code)
    { char buffer[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code,buffer,sizeof(buffer)); return buffer; }
    std::string HrError(HRESULT code)
    { char buffer[32]{}; std::snprintf(buffer,sizeof(buffer),"0x%08lx",static_cast<unsigned long>(code)); return buffer; }
    const GUID& Profile(int format)
    {
        if (format==VIDEO_FORMAT_H265) return D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        if (format==VIDEO_FORMAT_AV1_MAIN8) return D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0;
        return D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    }
    AVCodecID CodecId(int format)
    {
        return format==VIDEO_FORMAT_H265 ? AV_CODEC_ID_HEVC : format==VIDEO_FORMAT_AV1_MAIN8 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_H264;
    }
    std::string CodecName(int format)
    { return format==VIDEO_FORMAT_H265 ? "HEVC Main" : format==VIDEO_FORMAT_AV1_MAIN8 ? "AV1 Main" : "H.264 High"; }
    int DeviceFormats(ID3D11Device* device,int width,int height)
    {
        ComPtr<ID3D11VideoDevice> video;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&video)))) return 0;
        int formats{};
        for (int format:{VIDEO_FORMAT_H264,VIDEO_FORMAT_H265,VIDEO_FORMAT_AV1_MAIN8})
        {
            BOOL supported{}; UINT count{};
            D3D11_VIDEO_DECODER_DESC description{Profile(format),UINT(width),UINT(height),DXGI_FORMAT_NV12};
            if (SUCCEEDED(video->CheckVideoDecoderFormat(&description.Guid,DXGI_FORMAT_NV12,&supported)) && supported &&
                SUCCEEDED(video->GetVideoDecoderConfigCount(&description,&count)) && count && vanta::HardwareVideoDecoder::HasHardwareDecoder(format)) formats|=format;
        }
        return formats;
    }
    bool MakeDevice(IDXGIAdapter* adapter,ID3D11Device** device,ID3D11DeviceContext** context)
    {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        return SUCCEEDED(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels,UINT(std::size(levels)),D3D11_SDK_VERSION,device,&selected,context));
    }
    std::string JsonString(const std::string& value)
    {
        std::ostringstream out; out<<'"';
        for (unsigned char c:value)
        {
            if (c=='"' || c=='\\') out<<'\\'<<char(c);
            else if (c=='\n') out<<"\\n";
            else if (c=='\r') out<<"\\r";
            else if (c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
            else out<<char(c);
        }
        out<<'"'; return out.str();
    }
    struct GpuWaitEvent
    {
        HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        ~GpuWaitEvent(){if(value)CloseHandle(value);}
    };
    struct DecodeCompletion
    {
        ComPtr<ID3D11Query> query;
        ComPtr<ID3D11Fence> fence;
        std::unique_ptr<GpuWaitEvent> event;
        std::uint64_t value{};
    };
    struct FrameLease
    {
        AVFrame* frame{};
        ComPtr<ID3D11Texture2D> texture;
        UINT slice{};
        std::uint64_t readyFence{};
        std::shared_ptr<DecodeCompletion> completion;
        vanta::CapturedFrameMetadata metadata;
        ~FrameLease() { av_frame_free(&frame); }
    };
    struct EncodedPacket
    {
        AVBufferRef* buffer{};
        int size{};
        vanta::CapturedFrameMetadata metadata;
        int type{};
        EncodedPacket()=default;
        EncodedPacket(const EncodedPacket&)=delete;
        EncodedPacket& operator=(const EncodedPacket&)=delete;
        EncodedPacket(EncodedPacket&& other) noexcept
            :buffer(std::exchange(other.buffer,nullptr)),size(other.size),metadata(other.metadata),type(other.type){}
        EncodedPacket& operator=(EncodedPacket&& other) noexcept
        {if(this!=&other){av_buffer_unref(&buffer);buffer=std::exchange(other.buffer,nullptr);size=other.size;metadata=other.metadata;type=other.type;}return *this;}
        ~EncodedPacket(){av_buffer_unref(&buffer);}
    };
}

namespace vanta
{
    struct HardwareVideoDecoder::Implementation
    {
        std::recursive_mutex lifecycleMutex;
        mutable std::mutex stateMutex;
        std::condition_variable_any queueCondition,frameCondition;
        GpuWaitEvent frameEvent;
        std::jthread decodeThread;
        BoundedStreamQueue<EncodedPacket,3> queue;
        std::mutex packetPoolMutex;
        std::array<AVBufferPool*,9> packetPools{};
        AVBufferRef* PacketBuffer(std::size_t size)
        {
            std::lock_guard lock(packetPoolMutex);
            std::size_t capacity=256*1024,index=0;
            while(capacity<size && index+1<packetPools.size()){capacity*=2;++index;}
            if(capacity<size)return nullptr;
            if(!packetPools[index])packetPools[index]=av_buffer_pool_init(capacity,nullptr);
            return packetPools[index] ? av_buffer_pool_get(packetPools[index]) : nullptr;
        }
        FrameMetadataLedger pendingMetadata;
        std::function<void()> requestIdrCallback;
        DecodedVideoStats stats;
        std::mutex aggregateMutex;
        DecodedVideoStats cachedStats;
        std::uint64_t nextAggregateUs{};
        std::deque<std::pair<std::uint64_t,int>> receivedTimes;
        std::deque<std::uint64_t> decodedTimes,publishedTimes;
        std::atomic_bool accepting{false},stopped{true};
        std::atomic_uint64_t generation{0},session{0},nextSequence{0};
        StreamRecoveryState recovery;
        int selectedFormat{};
        std::uint32_t lastFrameNumber{};
        bool haveFrameNumber{};
        std::shared_ptr<FrameLease> latestFrame;
        std::deque<std::shared_ptr<FrameLease>> pendingGpuFrames;
        std::vector<std::shared_ptr<DecodeCompletion>> decodeCompletions;
        std::atomic_size_t gpuFramesInFlight{0};
        AVCodecContext* codec{};
        AVBufferRef* hardwareDevice{};
        std::recursive_mutex contextMutex;
        ComPtr<ID3D11Device> renderDevice,device;
        ComPtr<ID3D11DeviceContext> renderContext,context;
        ComPtr<ID3D11VideoDevice> videoDevice;
        ComPtr<ID3D11VideoContext> videoContext;
        ComPtr<ID3D11DeviceContext4> decodeContext4,renderContext4;
        ComPtr<ID3D11Fence> decodeFence,renderDecodeFence,renderFence;
        ComPtr<ID3D11DeviceContext4> readbackCompletionContext;
        ComPtr<ID3D11Fence> readbackCompletionFence;
        std::uint64_t readbackFenceValue{};
        std::uint64_t nextDecodeFence{},nextRenderFence{};
        bool separateDevices{};
        std::atomic_bool bindYuv{false};
        RollingStreamMetric reassemblyMetric,decodeMetric,readbackMetric,publicationMetric,hostMetric;
        RollingStreamMetric compressedQueueMetric,decodeSubmitMetric,gpuCompletionMetric,cpuCopyMetric,dispatchMetric;
        std::uint64_t lastSlotPressureSequence{}; // readback thread only

        struct ReadbackSlot
        {
            bool busy{},previewOnly{};
            std::shared_ptr<FrameLease> frame;
            cv::Rect rectangle,aligned;
            ComPtr<ID3D11Texture2D> bgra,staging;
            ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
            ComPtr<ID3D11VideoProcessor> processor;
            ComPtr<ID3D11VideoProcessorOutputView> output;
            ComPtr<ID3D11Query> completion;
            struct InputView {ID3D11Texture2D* texture{};UINT slice{};ComPtr<ID3D11VideoProcessorInputView> view;};
            std::vector<InputView> inputViews;
            std::unique_ptr<GpuWaitEvent> completionEvent;
            std::uint64_t completionValue{};
            std::uint64_t issuedUs{};
            std::uint64_t generation{};
            int sourceWidth{},sourceHeight{};
        };
        std::mutex readbackMutex;
        std::array<ReadbackSlot,3> slots;
        CpuFrameStoragePool cpuPool;
        CpuFrameSnapshot latestCpuSnapshot;
        cv::Rect lastIssuedRegion;
        std::uint64_t lastPublishedSequence{};
        std::atomic_uint64_t lastIssuedSequence{0};

        struct Preview
        {
            std::shared_ptr<FrameLease> frame;
            ComPtr<ID3D11Texture2D> opened;
            ComPtr<ID3D11ShaderResourceView> y,uv,bgraView;
            ComPtr<ID3D11Query> completion;
            std::uint64_t fenceValue{};
            bool submitted{};
            bool conversionOnly{};
            ReadbackSlot* convertedSlot{};
        };
        std::mutex previewMutex;
        std::shared_ptr<Preview> activePreview;
        std::vector<std::shared_ptr<Preview>> retiredPreviews;
        ComPtr<ID3D11PixelShader> yuvShader;
        std::array<ReadbackSlot,3> previewSlots;
        std::uint64_t previewUs{};
        bool previewDrawn{};

        static void LockDevice(void* opaque) { static_cast<Implementation*>(opaque)->contextMutex.lock(); }
        static void UnlockDevice(void* opaque) { static_cast<Implementation*>(opaque)->contextMutex.unlock(); }

        static AVPixelFormat ChooseFormat(AVCodecContext* codec,const AVPixelFormat* formats)
        {
            auto& self=*static_cast<Implementation*>(codec->opaque);
            for (auto p=formats;*p!=AV_PIX_FMT_NONE;++p) if (*p==AV_PIX_FMT_D3D11)
            {
                av_buffer_unref(&codec->hw_frames_ctx);
                if (avcodec_get_hw_frames_parameters(codec,codec->hw_device_ctx,*p,&codec->hw_frames_ctx)<0) return AV_PIX_FMT_NONE;
                auto* frames=reinterpret_cast<AVHWFramesContext*>(codec->hw_frames_ctx->data);
                auto* d3d=reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
                if (self.bindYuv) d3d->BindFlags|=D3D11_BIND_SHADER_RESOURCE;
                if (self.separateDevices && self.bindYuv) d3d->MiscFlags|=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                frames->initial_pool_size+=12; // Decoder references plus bounded readback/preview leases.
                int initialized=av_hwframe_ctx_init(codec->hw_frames_ctx);
                if(initialized<0 && self.bindYuv)
                {
                    // Some drivers can decode NV12 but reject decoder/SRV/shared
                    // texture combinations. Keep hardware decoding and share BGRA.
                    av_buffer_unref(&codec->hw_frames_ctx);
                    if(avcodec_get_hw_frames_parameters(codec,codec->hw_device_ctx,*p,&codec->hw_frames_ctx)<0)return AV_PIX_FMT_NONE;
                    frames=reinterpret_cast<AVHWFramesContext*>(codec->hw_frames_ctx->data);frames->initial_pool_size+=12;
                    initialized=av_hwframe_ctx_init(codec->hw_frames_ctx);self.bindYuv=false;
                    {std::lock_guard lock(self.stateMutex);self.stats.synchronization+="; reusable GPU BGRA preview fallback";}
                    log::Info("Direct YUV texture allocation unavailable; using GPU BGRA preview conversion");
                }
                if(initialized<0){self.Fail("D3D11VA hardware surface allocation failed: "+AvError(initialized));return AV_PIX_FMT_NONE;}
                return *p;
            }
            return AV_PIX_FMT_NONE; // Hardware-only contract.
        }

        void Fail(const std::string& message)
        {
            std::shared_ptr<FrameLease> retired;
            {std::lock_guard lock(stateMutex);++generation;++session;stats.failed=true;stats.hardwareActive=false;stats.status=message;retired=std::move(latestFrame);}
            accepting=false; frameCondition.notify_all(); queueCondition.notify_all();if(frameEvent.value)SetEvent(frameEvent.value);
            log::Error("Receiver: %s",message.c_str());
        }

        void BeginRecovery()
        {
            std::lock_guard lock(stateMutex);
            recovery.Begin();
            stats.status="Waiting for a recovery IDR and verified hardware output";
            queueCondition.notify_all();
        }
        int RejectAccessUnit()
        {
            {std::lock_guard lock(stateMutex);if(!accepting || stopped || stats.failed)return DR_OK;
                ++stats.decoderFailures;++stats.droppedFrames;recovery.Begin();}
            queueCondition.notify_one();return DR_OK;
        }

        bool SetupDevices(ID3D11Device* ui,std::string& error)
        {
            renderDevice=ui; ui->GetImmediateContext(&renderContext);
            ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
            if (FAILED(ui->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)))
            {error="Cannot identify the rendering adapter";return false;}
            DXGI_ADAPTER_DESC description{}; adapter->GetDesc(&description);
            {std::lock_guard lock(stateMutex);stats.adapter=Utf8(description.Description)+" ["+AdapterId(description.AdapterLuid)+"]";}
            ComPtr<ID3D11Device> other; ComPtr<ID3D11DeviceContext> otherContext;
            ComPtr<ID3D11Device5> other5,ui5;
            if (MakeDevice(adapter.Get(),&other,&otherContext) && SUCCEEDED(other.As(&other5)) &&
                SUCCEEDED(renderDevice.As(&ui5)) && SUCCEEDED(otherContext.As(&decodeContext4)) &&
                SUCCEEDED(renderContext.As(&renderContext4)) &&
                SUCCEEDED(other5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&decodeFence))) &&
                SUCCEEDED(ui5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&renderFence))))
            {
                HANDLE handle{};
                if (SUCCEEDED(decodeFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)))
                {
                    const HRESULT opened=ui5->OpenSharedFence(handle,IID_PPV_ARGS(&renderDecodeFence));
                    CloseHandle(handle);
                    D3D11_TEXTURE2D_DESC probe{};
                    probe.Width=probe.Height=128; probe.MipLevels=1;probe.ArraySize=2;probe.Format=DXGI_FORMAT_NV12;
                    probe.SampleDesc.Count=1;probe.BindFlags=D3D11_BIND_DECODER|D3D11_BIND_SHADER_RESOURCE;
                    probe.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                    ComPtr<ID3D11Texture2D> texture,shared;ComPtr<IDXGIResource1> resource;
                    if (SUCCEEDED(opened) && SUCCEEDED(other->CreateTexture2D(&probe,nullptr,&texture)) &&
                        SUCCEEDED(texture.As(&resource)) &&
                        SUCCEEDED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ,nullptr,&handle)))
                    {
                        const HRESULT sharedResult=ui5->OpenSharedResource1(handle,IID_PPV_ARGS(&shared));CloseHandle(handle);
                        if (SUCCEEDED(sharedResult)) {device=other;context=otherContext;separateDevices=true;bindYuv=true;}
                    }
                }
            }
            if (!separateDevices)
            {
                device=renderDevice;context=renderContext;decodeFence.Reset();renderDecodeFence.Reset();renderFence.Reset();
                decodeContext4.Reset();renderContext4.Reset();
                ComPtr<ID3D10Multithread> protectedContext;
                if (SUCCEEDED(device.As(&protectedContext))) protectedContext->SetMultithreadProtected(TRUE);
                else if (SUCCEEDED(context.As(&protectedContext))) protectedContext->SetMultithreadProtected(TRUE);
                else {error="D3D11 multithread protection is unavailable";return false;}
                D3D11_TEXTURE2D_DESC probe{};probe.Width=probe.Height=128;probe.ArraySize=2;probe.MipLevels=1;
                probe.Format=DXGI_FORMAT_NV12;probe.SampleDesc.Count=1;probe.BindFlags=D3D11_BIND_DECODER|D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> texture;bindYuv=SUCCEEDED(device->CreateTexture2D(&probe,nullptr,&texture));
            }
            if (FAILED(device.As(&videoDevice)) || FAILED(context.As(&videoContext)))
            {error="The selected adapter has no D3D11 video interfaces";return false;}
            ComPtr<ID3D11Device5> completionDevice;
            if(FAILED(device.As(&completionDevice)) || FAILED(context.As(&readbackCompletionContext)) ||
                FAILED(completionDevice->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&readbackCompletionFence))))
            {readbackCompletionContext.Reset();readbackCompletionFence.Reset();}
            std::string adapterName,synchronization;
            {std::lock_guard lock(stateMutex);stats.synchronization=separateDevices ? "Separate devices, shared GPU fences" : "Protected single-device fallback";
                adapterName=stats.adapter;synchronization=stats.synchronization;}
            log::Info("Receiver adapter: %s; %s; YUV binding=%s",adapterName.c_str(),synchronization.c_str(),bindYuv?"yes":"no");
            const char* shader=R"(Texture2DArray<float> Y:register(t0);Texture2DArray<float2> UV:register(t1);SamplerState S:register(s0);
                struct Input{float4 position:SV_POSITION;float4 color:COLOR0;float2 uv:TEXCOORD0;};
                float4 main(Input i):SV_Target{float y=(Y.Sample(S,float3(i.uv,0))*255-16)/219;
                float2 c=(UV.Sample(S,float3(i.uv,0))*255-128)/224;
                return float4(saturate(float3(y+1.5748*c.y,y-.187324*c.x-.468124*c.y,y+1.8556*c.x)),1)*i.color;})";
            ComPtr<ID3DBlob> code,diagnostic;
            if (bindYuv && SUCCEEDED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&diagnostic)))
                renderDevice->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&yuvShader);
            return true;
        }

        bool OpenCodec(std::string& error)
        {
            hardwareDevice=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!hardwareDevice) {error="Cannot allocate FFmpeg D3D11 hardware context";return false;}
            auto* hw=reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
            auto* d3d=reinterpret_cast<AVD3D11VADeviceContext*>(hw->hwctx);
            device.CopyTo(&d3d->device);context.CopyTo(&d3d->device_context);
            d3d->lock=LockDevice;d3d->unlock=UnlockDevice;d3d->lock_ctx=this;
            int result=av_hwdevice_ctx_init(hardwareDevice);
            if (result<0) {error="D3D11VA initialization failed: "+AvError(result);return false;}
            const AVCodec* decoder=avcodec_find_decoder(CodecId(selectedFormat));
            if (!decoder) {error="This FFmpeg build does not contain the selected decoder";return false;}
            codec=avcodec_alloc_context3(decoder);
            if (!codec) {error="Cannot allocate the hardware decoder";return false;}
            codec->hw_device_ctx=av_buffer_ref(hardwareDevice);codec->opaque=this;codec->get_format=ChooseFormat;
            codec->width=stats.width;codec->height=stats.height;codec->pkt_timebase={1,1000000};
            codec->flags|=AV_CODEC_FLAG_LOW_DELAY|AV_CODEC_FLAG_COPY_OPAQUE;
            codec->apply_cropping=0; // Preserve decoder crop metadata for the visible rectangle.
            codec->err_recognition=AV_EF_EXPLODE;codec->thread_count=1;codec->thread_type=0;codec->extra_hw_frames=9;
            result=avcodec_open2(codec,decoder,nullptr);
            if (result<0) {error="Cannot open hardware "+CodecName(selectedFormat)+": "+AvError(result);return false;}
            return true;
        }

        bool ReceiveFrames()
        {
            for (;;)
            {
                if(stopped)return true;
                AVFrame* frame=av_frame_alloc();
                if (!frame) return false;
                const int result=avcodec_receive_frame(codec,frame);
                if (result==AVERROR(EAGAIN) || result==AVERROR_EOF) {av_frame_free(&frame);return true;}
                if (result<0) {av_frame_free(&frame);return false;}
                if (frame->format!=AV_PIX_FMT_D3D11 || !frame->data[0] || !frame->hw_frames_ctx)
                {av_frame_free(&frame);Fail("Decoder output is not a hardware D3D11 surface; CPU fallback is disabled");return false;}
                auto lease=std::make_shared<FrameLease>();lease->frame=frame;
                lease->texture=reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);lease->slice=UINT(reinterpret_cast<std::uintptr_t>(frame->data[1]));
                CapturedFrameMetadata opaque;const CapturedFrameMetadata* association{};
                if(frame->opaque_ref && frame->opaque_ref->size==sizeof(opaque))
                {std::memcpy(&opaque,frame->opaque_ref->data,sizeof(opaque));association=&opaque;}
                if(!pendingMetadata.Take(frame->pts,association,lease->metadata))
                {Fail("Hardware output has no matching access-unit metadata");return false;}
                D3D11_TEXTURE2D_DESC description{};lease->texture->GetDesc(&description);
                if (description.Format!=DXGI_FORMAT_NV12) {Fail("Hardware decoder returned a non-NV12 surface");return false;}
                auto& metadata=lease->metadata;
                metadata.sourceWidth=frame->width-int(frame->crop_left+frame->crop_right);
                metadata.sourceHeight=frame->height-int(frame->crop_top+frame->crop_bottom);
                metadata.cropWidth=metadata.sourceWidth;metadata.cropHeight=metadata.sourceHeight;
                metadata.visibleX=int(frame->crop_left);metadata.visibleY=int(frame->crop_top);
                metadata.textureWidth=int(description.Width);metadata.textureHeight=int(description.Height);
                if (metadata.sourceWidth<=0 || metadata.sourceHeight<=0 || metadata.visibleX+metadata.sourceWidth>int(description.Width) ||
                    metadata.visibleY+metadata.sourceHeight>int(description.Height)) {Fail("Invalid decoded visible rectangle");return false;}
                metadata.timing.decodedUs=StreamNowUs();metadata.sequence=++nextSequence;
                {std::lock_guard stateLock(stateMutex);metadata.sourcePeriodUs=1000000ULL/static_cast<unsigned>(std::max(1,stats.configuredFps));}
                auto found=std::find_if(decodeCompletions.begin(),decodeCompletions.end(),[](const auto& completion){return completion.use_count()==1;});
                if(found!=decodeCompletions.end())lease->completion=*found;
                else
                {
                    if(decodeCompletions.size()==32){Fail("Hardware surface completion leases exhausted");return false;}
                    auto completion=std::make_shared<DecodeCompletion>();D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
                    if(FAILED(device->CreateQuery(&query,&completion->query))){Fail("Cannot create hardware surface completion tracking");return false;}
                    completion->event=std::make_unique<GpuWaitEvent>();if(!completion->event->value)completion->event.reset();
                    decodeCompletions.push_back(completion);lease->completion=std::move(completion);
                }
                pendingGpuFrames.push_back(lease);gpuFramesInFlight=pendingGpuFrames.size();
                {
                    std::lock_guard gpu(contextMutex);auto& completion=*lease->completion;
                    context->End(completion.query.Get());
                    completion.fence=separateDevices ? decodeFence : readbackCompletionFence;
                    if(completion.fence)
                    {
                        completion.value=separateDevices ? ++nextDecodeFence : ++readbackFenceValue;
                        auto* signalingContext=separateDevices ? decodeContext4.Get() : readbackCompletionContext.Get();
                        if(FAILED(signalingContext->Signal(completion.fence.Get(),completion.value)))
                        {Fail("Decoder GPU fence failed");return false;}
                        if(separateDevices)lease->readyFence=completion.value;
                        if(completion.event)
                        {ResetEvent(completion.event->value);if(FAILED(completion.fence->SetEventOnCompletion(completion.value,completion.event->value)))completion.event.reset();}
                    }
                    context->Flush();
                }
                decodeMetric.Add(double(metadata.timing.decodedUs-metadata.timing.assembledUs)/1000.0);
                bool geometryChanged{};
                std::shared_ptr<FrameLease> retired;
                {
                    std::lock_guard lock(stateMutex);
                    if (stopped || stats.failed || metadata.session!=session) continue;
                    geometryChanged=stats.latestMetadata.sourceWidth!=metadata.sourceWidth || stats.latestMetadata.sourceHeight!=metadata.sourceHeight ||
                        stats.latestMetadata.textureWidth!=metadata.textureWidth || stats.latestMetadata.textureHeight!=metadata.textureHeight ||
                        stats.latestMetadata.visibleX!=metadata.visibleX || stats.latestMetadata.visibleY!=metadata.visibleY;
                    if(geometryChanged && stats.latestMetadata.sequence)++generation;
                    metadata.generation=generation.load();
                    if (latestFrame && latestFrame->metadata.sequence>lastIssuedSequence) ++stats.outputSkippedFrames;
                    retired=std::move(latestFrame);latestFrame=lease;++stats.decodedFrames;stats.hardwareActive=true;
                    recovery.OnOutput();
                    stats.width=metadata.sourceWidth;stats.height=metadata.sourceHeight;
                    stats.latestMetadata=metadata;
                    stats.status=recovery.Active() ? "Waiting for a recovery IDR and verified hardware output" : "Hardware "+CodecName(selectedFormat)+" / D3D11VA";
                    decodedTimes.push_back(metadata.timing.decodedUs);
                }
                frameCondition.notify_all();
                if(frameEvent.value)SetEvent(frameEvent.value);
                if(geometryChanged)log::Info("Decoded visible rectangle: %dx%d at (%d,%d); GPU allocation %dx%d; stream generation=%llu",
                    metadata.sourceWidth,metadata.sourceHeight,metadata.visibleX,metadata.visibleY,metadata.textureWidth,metadata.textureHeight,
                    static_cast<unsigned long long>(metadata.generation));
            }
        }

        void Decode(std::stop_token stop)
        {
            const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            while (!stop.stop_requested())
            {
                ReapPreviews();
                if(!ReapDecodedFrames())break;
                while(pendingGpuFrames.size()>=3 && !stop.stop_requested() && !stopped)
                {
                    std::function<void()> requester;bool exhausted{};
                    {std::lock_guard lock(stateMutex);const auto action=recovery.Poll(StreamNowUs());
                        if(action==RecoveryAction::requestIdr){++stats.idrRequests;requester=requestIdrCallback;}
                        exhausted=action==RecoveryAction::terminate;}
                    if(exhausted){Fail("No recovery IDR produced hardware output after three retries");break;}
                    if(requester)requester();
                    const auto completion=pendingGpuFrames.front()->completion;
                    if(completion->fence && completion->event)WaitForSingleObject(completion->event->value,2);
                    else{std::unique_lock lock(stateMutex);queueCondition.wait_for(lock,stop,std::chrono::milliseconds(1),[&]{return stopped.load();});}
                    if(!ReapDecodedFrames())break;
                }
                if(stop.stop_requested() || stopped)break;
                EncodedPacket input;std::array<EncodedPacket,3> discarded;std::size_t discardCount{};
                bool gotInput{},flush{};std::function<void()> requestIdr;
                {
                    std::unique_lock lock(stateMutex);
                    queueCondition.wait_for(lock,stop,std::chrono::milliseconds(100),[&]{return stopped || stats.failed || !queue.empty() || recovery.NeedsFlush();});
                    if (stop.stop_requested() || stopped || stats.failed) break;
                    flush=recovery.TakeFlush();
                    if(flush)while(!queue.empty() && queue.front().type!=FRAME_TYPE_IDR)
                    {discarded[discardCount++]=std::move(queue.front());queue.pop_front();++stats.droppedFrames;}
                    const auto action=recovery.Poll(StreamNowUs());
                    if(action==RecoveryAction::terminate){lock.unlock();Fail("No recovery IDR arrived after three retries; reconnect the stream");break;}
                    if(action==RecoveryAction::requestIdr){++stats.idrRequests;requestIdr=requestIdrCallback;}
                    if (!queue.empty()) {input=std::move(queue.front());queue.pop_front();gotInput=true;if(input.type==FRAME_TYPE_IDR)recovery.OnKeyFrame();}
                }
                if (flush) {avcodec_flush_buffers(codec);pendingMetadata.clear();}
                if (requestIdr) requestIdr();
                if (!gotInput) continue;
                AVPacket* packet=av_packet_alloc();
                bool success=packet && (packet->buf=av_buffer_ref(input.buffer));
                if (success)
                {
                    packet->data=packet->buf->data;packet->size=input.size;
                    packet->pts=packet->dts=std::int64_t(input.metadata.presentationTimeUs);
                    if (input.type==FRAME_TYPE_IDR) packet->flags|=AV_PKT_FLAG_KEY;
                    packet->opaque_ref=av_buffer_alloc(sizeof(CapturedFrameMetadata));
                    input.metadata.timing.submittedUs=StreamNowUs();
                    compressedQueueMetric.Add(double(input.metadata.timing.submittedUs-input.metadata.timing.assembledUs)/1000.0);
                    if (packet->opaque_ref) std::memcpy(packet->opaque_ref->data,&input.metadata,sizeof(input.metadata));
                    int result=avcodec_send_packet(codec,packet);
                    if (result==AVERROR(EAGAIN)) {success=ReceiveFrames();if(success)result=avcodec_send_packet(codec,packet);}
                    success=success && result>=0;
                    if (success) {pendingMetadata.push_back(input.metadata);success=ReceiveFrames();if(pendingMetadata.size()>32)success=false;}
                    decodeSubmitMetric.Add(double(StreamNowUs()-input.metadata.timing.submittedUs)/1000.0);
                }
                av_packet_free(&packet);
                if (!success)
                {
                    {std::lock_guard lock(stateMutex);++stats.decoderFailures;++stats.droppedFrames;}
                    BeginRecovery();
                }
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
        }

        bool IssueReadback(ReadbackSlot& slot,const std::shared_ptr<FrameLease>& frame,const cv::Rect& rectangle);
        bool CompleteReadback(ReadbackSlot& slot,CpuFrameSnapshot& output);
        void ReapPreviews();
        bool ReapDecodedFrames();
        bool MakePreview(const std::shared_ptr<FrameLease>& frame);
        bool ConvertPreview(const std::shared_ptr<FrameLease>& frame);
        void DrainGpu();
        static void BindPreviewShader(const ImDrawList*,const ImDrawCmd* command);
        static void UnbindPreviewShader(const ImDrawList*,const ImDrawCmd* command);
    };

    bool HardwareVideoDecoder::Implementation::ReapDecodedFrames()
    {
        while(!pendingGpuFrames.empty())
        {
            const auto completion=pendingGpuFrames.front()->completion;bool complete{};
            if(completion->fence)
            {const auto value=completion->fence->GetCompletedValue();if(value==UINT64_MAX){Fail("GPU device removed during decode");return false;}complete=value>=completion->value;}
            else
            {
                HRESULT result{};{std::lock_guard gpu(contextMutex);result=context->GetData(completion->query.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);}
                if(FAILED(result)){Fail("GPU decode completion failed: "+HrError(result));return false;}complete=result==S_OK;
            }
            if(!complete)break;
            gpuCompletionMetric.Add(double(StreamNowUs()-pendingGpuFrames.front()->metadata.timing.decodedUs)/1000.0);
            pendingGpuFrames.pop_front();
            gpuFramesInFlight=pendingGpuFrames.size();
        }
        return true;
    }

    bool HardwareVideoDecoder::Implementation::IssueReadback(ReadbackSlot& slot,
        const std::shared_ptr<FrameLease>& frame,const cv::Rect& rectangle)
    {
        if (rectangle.empty()) return false;
        const auto& metadata=frame->metadata;
        const auto aligned=AlignNv12Region(rectangle,metadata);
        int outputWidth=aligned.width,outputHeight=aligned.height;
        if (slot.previewOnly)
        {
            const double scale=std::min({1.0,560.0/rectangle.width,315.0/rectangle.height});
            outputWidth=std::max(1,int(rectangle.width*scale));outputHeight=std::max(1,int(rectangle.height*scale));
        }
        std::lock_guard gpu(contextMutex);
        D3D11_TEXTURE2D_DESC existing{};if(slot.bgra)slot.bgra->GetDesc(&existing);
        if (!slot.bgra || int(existing.Width)!=outputWidth || int(existing.Height)!=outputHeight ||
            slot.sourceWidth!=metadata.textureWidth || slot.sourceHeight!=metadata.textureHeight || slot.generation!=metadata.generation)
        {
            slot.bgra.Reset();slot.staging.Reset();slot.enumerator.Reset();slot.processor.Reset();slot.output.Reset();slot.completion.Reset();slot.inputViews.clear();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth=metadata.textureWidth;content.InputHeight=metadata.textureHeight;
            content.OutputWidth=outputWidth;content.OutputHeight=outputHeight;
            content.InputFrameRate={UINT(std::max(1,stats.configuredFps)),1};content.OutputFrameRate=content.InputFrameRate;
            content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&content,&slot.enumerator)) ||
                FAILED(videoDevice->CreateVideoProcessor(slot.enumerator.Get(),0,&slot.processor))) return false;
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width=outputWidth;texture.Height=outputHeight;texture.MipLevels=texture.ArraySize=1;
            texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.SampleDesc.Count=1;
            texture.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            if (slot.previewOnly && separateDevices) texture.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (FAILED(device->CreateTexture2D(&texture,nullptr,&slot.bgra))) return false;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};output.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(videoDevice->CreateVideoProcessorOutputView(slot.bgra.Get(),slot.enumerator.Get(),&output,&slot.output))) return false;
            if (!slot.previewOnly)
            {
                texture.BindFlags=texture.MiscFlags=0;texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if (FAILED(device->CreateTexture2D(&texture,nullptr,&slot.staging))) return false;
            }
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
            if (FAILED(device->CreateQuery(&query,&slot.completion))) return false;
            if(readbackCompletionFence)
            {slot.completionEvent=std::make_unique<GpuWaitEvent>();if(!slot.completionEvent->value)slot.completionEvent.reset();}
            videoContext->VideoProcessorSetStreamFrameFormat(slot.processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            videoContext->VideoProcessorSetStreamAutoProcessingMode(slot.processor.Get(),0,FALSE);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};inputColor.YCbCr_Matrix=1;inputColor.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};outputColor.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            videoContext->VideoProcessorSetStreamColorSpace(slot.processor.Get(),0,&inputColor);
            videoContext->VideoProcessorSetOutputColorSpace(slot.processor.Get(),&outputColor);
            slot.sourceWidth=metadata.textureWidth;slot.sourceHeight=metadata.textureHeight;slot.generation=metadata.generation;
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};input.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        input.Texture2D.ArraySlice=frame->slice;
        auto found=std::find_if(slot.inputViews.begin(),slot.inputViews.end(),[&](const auto& entry){return entry.texture==frame->texture.Get() && entry.slice==frame->slice;});
        if(found==slot.inputViews.end())
        {ReadbackSlot::InputView view;view.texture=frame->texture.Get();view.slice=frame->slice;
            if(FAILED(videoDevice->CreateVideoProcessorInputView(frame->texture.Get(),slot.enumerator.Get(),&input,&view.view)))return false;
            if(slot.inputViews.size()==96)slot.inputViews.clear();slot.inputViews.push_back(std::move(view));found=std::prev(slot.inputViews.end());}
        const RECT source{slot.previewOnly ? rectangle.x+metadata.visibleX : aligned.x,
            slot.previewOnly ? rectangle.y+metadata.visibleY : aligned.y,
            slot.previewOnly ? rectangle.x+metadata.visibleX+rectangle.width : aligned.x+aligned.width,
            slot.previewOnly ? rectangle.y+metadata.visibleY+rectangle.height : aligned.y+aligned.height};
        const RECT destination{0,0,outputWidth,outputHeight};
        videoContext->VideoProcessorSetStreamSourceRect(slot.processor.Get(),0,TRUE,&source);
        videoContext->VideoProcessorSetStreamDestRect(slot.processor.Get(),0,TRUE,&destination);
        videoContext->VideoProcessorSetOutputTargetRect(slot.processor.Get(),TRUE,&destination);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=found->view.Get();
        slot.rectangle=rectangle;slot.aligned=aligned;slot.frame=frame;slot.busy=true;slot.issuedUs=StreamNowUs();
        if (FAILED(videoContext->VideoProcessorBlt(slot.processor.Get(),slot.output.Get(),0,1,&stream))) return false;
        if (!slot.previewOnly) context->CopyResource(slot.staging.Get(),slot.bgra.Get());
        context->End(slot.completion.Get());
        if(readbackCompletionFence && slot.completionEvent)
        {
            slot.completionValue=++readbackFenceValue;
            ResetEvent(slot.completionEvent->value);
            if(FAILED(readbackCompletionContext->Signal(readbackCompletionFence.Get(),slot.completionValue)) ||
                FAILED(readbackCompletionFence->SetEventOnCompletion(slot.completionValue,slot.completionEvent->value)))return false;
        }
        context->Flush();
        slot.rectangle=rectangle;slot.aligned=aligned;slot.frame=frame;slot.busy=true;
        if(!slot.previewOnly){std::lock_guard lock(stateMutex);++stats.readbacksIssued;}
        return true;
    }

    bool HardwareVideoDecoder::Implementation::CompleteReadback(ReadbackSlot& slot,CpuFrameSnapshot& output)
    {
        if (!slot.busy) return false;
        HRESULT completed{};
        {std::lock_guard gpu(contextMutex);completed=context->GetData(slot.completion.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);}
        if (completed==S_FALSE) return false;
        if (FAILED(completed)) {Fail("GPU readback completion failed: "+HrError(completed));slot.busy=false;slot.frame.reset();return false;}
        if (slot.frame->metadata.generation!=generation || stopped) {slot.busy=false;slot.frame.reset();return false;}
        D3D11_MAPPED_SUBRESOURCE mapped{};
        {std::lock_guard gpu(contextMutex);completed=context->Map(slot.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);}
        if (completed==DXGI_ERROR_WAS_STILL_DRAWING){std::lock_guard lock(stateMutex);++stats.nonblockingMapRetries;return false;}
        if (FAILED(completed)) {Fail("GPU readback map failed: "+HrError(completed));slot.busy=false;slot.frame.reset();return false;}
        const auto copyStarted=StreamNowUs();
        cv::Mat* storage=cpuPool.Acquire();
        if(!storage){std::lock_guard lock(stateMutex);++stats.cpuPoolExhaustions;}
        bool copied{};
        if (storage) try
        {
            storage->create(slot.rectangle.height,slot.rectangle.width,CV_8UC4);
            const int offsetX=slot.rectangle.x+slot.frame->metadata.visibleX-slot.aligned.x;
            const int offsetY=slot.rectangle.y+slot.frame->metadata.visibleY-slot.aligned.y;
            const auto* source=static_cast<const unsigned char*>(mapped.pData)+std::size_t(offsetY)*mapped.RowPitch+offsetX*4;
            for (int row=0;row<storage->rows;++row)
                std::memcpy(storage->ptr(row),source+std::size_t(row)*mapped.RowPitch,std::size_t(storage->cols)*4);
            output.bgra=*storage;output.metadata=slot.frame->metadata;
            output.metadata.cropX=slot.rectangle.x;output.metadata.cropY=slot.rectangle.y;
            output.metadata.cropWidth=slot.rectangle.width;output.metadata.cropHeight=slot.rectangle.height;
            output.metadata.timing.readbackStartedUs=slot.issuedUs;
            output.metadata.timing.readbackUs=StreamNowUs();
            output.metadata.captureTimestampNanoseconds=std::int64_t(output.metadata.timing.readbackUs*1000);copied=true;
        }
        catch (const std::exception&) {copied=false;}
        {std::lock_guard gpu(contextMutex);context->Unmap(slot.staging.Get(),0);}
        slot.busy=false;slot.frame.reset();
        if (copied)
        {
            cpuCopyMetric.Add(double(output.metadata.timing.readbackUs-copyStarted)/1000.0);
            readbackMetric.Add(double(output.metadata.timing.readbackUs-output.metadata.timing.readbackStartedUs)/1000.0);
            std::lock_guard lock(stateMutex);
            ++stats.readbacksCompleted;
            stats.readbackBytes+=std::uint64_t(output.bgra.cols)*output.bgra.rows*4;
        }
        return copied;
    }

    void HardwareVideoDecoder::Implementation::ReapPreviews()
    {
        std::lock_guard lock(previewMutex);
        std::erase_if(retiredPreviews,[&](const std::shared_ptr<Preview>& preview)
        {
            bool done{};
            if(preview->conversionOnly)
            {std::lock_guard gpu(contextMutex);done=context->GetData(preview->convertedSlot->completion.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_FALSE;}
            else if (separateDevices) done=renderFence->GetCompletedValue()>=preview->fenceValue;
            else
            {
                std::lock_guard gpu(contextMutex);
                done=renderContext->GetData(preview->completion.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_FALSE;
            }
            if (done && preview->convertedSlot) {preview->convertedSlot->busy=false;preview->convertedSlot->frame.reset();}
            return done;
        });
    }

    bool HardwareVideoDecoder::Implementation::ConvertPreview(const std::shared_ptr<FrameLease>& frame)
    {
        for (auto& slot:previewSlots) if (!slot.busy)
        {
            slot.previewOnly=true;
            if (!IssueReadback(slot,frame,{0,0,frame->metadata.sourceWidth,frame->metadata.sourceHeight})) return false;
            const auto abandon=[&]
            {auto pending=std::make_shared<Preview>();pending->frame=frame;pending->convertedSlot=&slot;pending->conversionOnly=true;retiredPreviews.push_back(std::move(pending));return false;};
            auto preview=std::make_shared<Preview>();preview->frame=frame;preview->convertedSlot=&slot;
            ID3D11Texture2D* texture=slot.bgra.Get();
            if (separateDevices)
            {
                ComPtr<IDXGIResource1> resource;ComPtr<ID3D11Device1> ui;HANDLE handle{};
                if (FAILED(slot.bgra.As(&resource)) || FAILED(renderDevice.As(&ui)) ||
                    FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ,nullptr,&handle))) return abandon();
                const HRESULT result=ui->OpenSharedResource1(handle,IID_PPV_ARGS(&preview->opened));CloseHandle(handle);
                if (FAILED(result)) return abandon();
                texture=preview->opened.Get();
                std::lock_guard gpu(contextMutex);const auto value=++nextDecodeFence;
                if(FAILED(decodeContext4->Signal(decodeFence.Get(),value)))return abandon();context->Flush();
                if(FAILED(renderContext4->Wait(renderDecodeFence.Get(),value)))return abandon();
            }
            if (FAILED(renderDevice->CreateShaderResourceView(texture,nullptr,&preview->bgraView))) return abandon();
            activePreview=preview;
            return true;
        }
        return false;
    }

    bool HardwareVideoDecoder::Implementation::MakePreview(const std::shared_ptr<FrameLease>& frame)
    {
        if (!bindYuv || !yuvShader) return ConvertPreview(frame);
        auto preview=std::make_shared<Preview>();preview->frame=frame;
        ID3D11Texture2D* texture=frame->texture.Get();
        if (separateDevices)
        {
            ComPtr<IDXGIResource1> resource;ComPtr<ID3D11Device1> ui;HANDLE handle{};
            if (FAILED(frame->texture.As(&resource)) || FAILED(renderDevice.As(&ui)) ||
                FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ,nullptr,&handle))) return ConvertPreview(frame);
            const HRESULT result=ui->OpenSharedResource1(handle,IID_PPV_ARGS(&preview->opened));CloseHandle(handle);
            if (FAILED(result)) return ConvertPreview(frame);
            texture=preview->opened.Get();
            if(FAILED(renderContext4->Wait(renderDecodeFence.Get(),frame->readyFence)))return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        description.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        description.Texture2DArray.MipLevels=1;description.Texture2DArray.ArraySize=1;
        description.Texture2DArray.FirstArraySlice=frame->slice;description.Format=DXGI_FORMAT_R8_UNORM;
        if (FAILED(renderDevice->CreateShaderResourceView(texture,&description,&preview->y)))
        {bindYuv=false;return ConvertPreview(frame);}
        description.Format=DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(renderDevice->CreateShaderResourceView(texture,&description,&preview->uv)))
        {bindYuv=false;return ConvertPreview(frame);}
        activePreview=std::move(preview);return true;
    }

    void HardwareVideoDecoder::Implementation::BindPreviewShader(const ImDrawList*,const ImDrawCmd* command)
    {
        auto& self=*static_cast<Implementation*>(command->UserCallbackData);
        if (!self.activePreview) return;
        ID3D11ShaderResourceView* chroma=self.activePreview->uv.Get();
        self.renderContext->PSSetShaderResources(1,1,&chroma);
        self.renderContext->PSSetShader(self.yuvShader.Get(),nullptr,0);
    }
    void HardwareVideoDecoder::Implementation::UnbindPreviewShader(const ImDrawList*,const ImDrawCmd* command)
    {auto& self=*static_cast<Implementation*>(command->UserCallbackData);ID3D11ShaderResourceView* empty{};self.renderContext->PSSetShaderResources(1,1,&empty);}

    HardwareVideoDecoder::HardwareVideoDecoder():implementation_(std::make_unique<Implementation>()){}
    HardwareVideoDecoder::~HardwareVideoDecoder(){Shutdown();}

    void HardwareVideoDecoder::Implementation::DrainGpu()
    {
        // No publication/queue lock is held while waiting. Decode/capture are stopped.
        for(auto* gpuContext:{context.Get(),renderContext.Get()})
        {
            if(!gpuContext)continue;
            ComPtr<ID3D11Device> owner;gpuContext->GetDevice(&owner);ComPtr<ID3D11Query> query;
            D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};if(FAILED(owner->CreateQuery(&description,&query)))continue;
            {std::lock_guard gpu(contextMutex);gpuContext->End(query.Get());gpuContext->Flush();}
            const auto deadline=StreamNowUs()+2000000;HRESULT complete=S_FALSE;
            do
            {
                {std::lock_guard gpu(contextMutex);complete=gpuContext->GetData(query.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);}
                if(complete!=S_FALSE)break;
                Sleep(1);
            }while(StreamNowUs()<deadline);
            if(complete==S_FALSE || FAILED(complete))
                log::Warning("GPU drain %s: context=%s device-status=0x%08lx decode-fence=%llu/%llu render-fence=%llu/%llu readback-fence=%llu/%llu",
                    complete==S_FALSE?"timed out during stream teardown":"failed during stream teardown",
                    gpuContext==context.Get()?"decode/readback":"UI rendering",static_cast<unsigned long>(owner->GetDeviceRemovedReason()),
                    static_cast<unsigned long long>(decodeFence?decodeFence->GetCompletedValue():0),static_cast<unsigned long long>(nextDecodeFence),
                    static_cast<unsigned long long>(renderFence?renderFence->GetCompletedValue():0),static_cast<unsigned long long>(nextRenderFence),
                    static_cast<unsigned long long>(readbackCompletionFence?readbackCompletionFence->GetCompletedValue():0),static_cast<unsigned long long>(readbackFenceValue));
        }
    }

    bool HardwareVideoDecoder::HasHardwareDecoder(int format)
    {
        if(format!=VIDEO_FORMAT_H264 && format!=VIDEO_FORMAT_H265 && format!=VIDEO_FORMAT_AV1_MAIN8)return false;
        const auto* decoder=avcodec_find_decoder(CodecId(format));if(!decoder)return false;
        for(int index=0;;++index)
        {const auto* config=avcodec_get_hw_config(decoder,index);if(!config)break;
            if(config->device_type==AV_HWDEVICE_TYPE_D3D11VA && config->pix_fmt==AV_PIX_FMT_D3D11 &&
                (config->methods&(AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX|AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)))return true;}
        return false;
    }

    std::vector<VideoAdapterInfo> HardwareVideoDecoder::EnumerateAdapters()
    {
        std::vector<VideoAdapterInfo> adapters;ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return adapters;
        for (UINT index=0;;++index)
        {
            ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            if(!adapter)continue;DXGI_ADAPTER_DESC1 description{};adapter->GetDesc1(&description);
            if((description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) || description.VendorId==0x1414)continue;
            ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
            if(!MakeDevice(adapter.Get(),&device,&context))continue;
            LARGE_INTEGER driver{};adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver);
            std::ostringstream version;version<<HIWORD(driver.HighPart)<<'.'<<LOWORD(driver.HighPart)<<'.'<<HIWORD(driver.LowPart)<<'.'<<LOWORD(driver.LowPart);
            std::string displays;
            for(UINT outputIndex=0;;++outputIndex)
            {ComPtr<IDXGIOutput> output;if(adapter->EnumOutputs(outputIndex,&output)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_OUTPUT_DESC display{};if(output && SUCCEEDED(output->GetDesc(&display)) && display.AttachedToDesktop)
                {if(!displays.empty())displays+=", ";displays+=Utf8(display.DeviceName);}}
            adapters.push_back({AdapterId(description.AdapterLuid),Utf8(description.Description),version.str(),DeviceFormats(device.Get(),2560,1440),description.VendorId==0x10de,displays});
        }
        std::stable_sort(adapters.begin(),adapters.end(),[](const auto& a,const auto& b){return a.nvidia>b.nvidia;});
        return adapters;
    }

    bool HardwareVideoDecoder::CreateRenderDevice(const std::string& preference,ID3D11Device** device,
        ID3D11DeviceContext** context,std::string& error)
    {
        const auto adapters=EnumerateAdapters();
        for(const auto& adapter:adapters)
            log::Info("Video adapter %s [%s], driver %s; 1440p H.264=%s HEVC=%s AV1=%s; attached displays: %s",adapter.name.c_str(),adapter.id.c_str(),adapter.driver.c_str(),
                (adapter.supportedFormats&VIDEO_FORMAT_H264)?"yes":"no",(adapter.supportedFormats&VIDEO_FORMAT_H265)?"yes":"no",
                (adapter.supportedFormats&VIDEO_FORMAT_AV1_MAIN8)?"yes":"no",adapter.displays.empty()?"none (presentation may cross the desktop compositor)":adapter.displays.c_str());
        ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))){error="DXGI is unavailable";return false;}
        for(const auto& selected:adapters)
        {
            if(preference!="auto" && preference!=selected.id)continue;
            if(!selected.supportedFormats)continue;
            for(UINT index=0;;++index)
            {
                ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
                if(!adapter)continue;DXGI_ADAPTER_DESC1 description{};adapter->GetDesc1(&description);
                if(AdapterId(description.AdapterLuid)==selected.id && MakeDevice(adapter.Get(),device,context))return true;
            }
        }
        error=preference=="auto" ? "No D3D11 hardware video adapter is available" : "The selected video adapter is unavailable; select Auto and restart";
        return false;
    }

    bool HardwareVideoDecoder::Initialize(ID3D11Device* device,std::string& error)
    {
        if(!device){error="The rendering device is unavailable";return false;}
        auto& self=*implementation_;std::lock_guard lifecycle(self.lifecycleMutex);if(self.device)return true;
        return self.SetupDevices(device,error);
    }

    int HardwareVideoDecoder::SupportedFormats(int width,int height) const
    {const auto& self=*implementation_;return DeviceFormats(self.device.Get(),width,height);}

    int HardwareVideoDecoder::Configure(int format,int width,int height,int fps,std::string& error)
    {
        auto& self=*implementation_;
        std::lock_guard lifecycle(self.lifecycleMutex);
        if(!self.device || width<=0 || height<=0 || fps<1 || !(SupportedFormats(width,height)&format))
        {error="The selected adapter does not support this hardware codec/resolution";return -1;}
        Stop();self.decodeThread.request_stop();self.queueCondition.notify_all();if(self.decodeThread.joinable())self.decodeThread.join();
        std::scoped_lock consumers(self.readbackMutex,self.previewMutex);
        self.DrainGpu();
        self.activePreview.reset();self.retiredPreviews.clear();self.previewSlots={};
        avcodec_free_context(&self.codec);av_buffer_unref(&self.hardwareDevice);
        self.pendingMetadata.clear();self.slots={};self.pendingGpuFrames.clear();self.gpuFramesInFlight=0;self.decodeCompletions.clear();self.latestCpuSnapshot={};self.cpuPool.Clear();self.lastIssuedSequence=0;self.lastIssuedRegion={};self.lastPublishedSequence=0;
        self.reassemblyMetric.Clear();self.decodeMetric.Clear();self.readbackMetric.Clear();self.publicationMetric.Clear();self.hostMetric.Clear();
        self.compressedQueueMetric.Clear();self.decodeSubmitMetric.Clear();self.gpuCompletionMetric.Clear();self.cpuCopyMetric.Clear();self.dispatchMetric.Clear();self.lastSlotPressureSequence=0;
        {
            std::lock_guard lock(self.stateMutex);const auto adapter=self.stats.adapter,synchronization=self.stats.synchronization;
            self.stats={};self.stats.adapter=adapter;self.stats.synchronization=synchronization;
            self.stats.width=width;self.stats.height=height;self.stats.configuredFps=fps;self.stats.codec=CodecName(format);
            self.stats.generation=++self.generation;self.stats.status="Waiting for verified hardware output";
            self.selectedFormat=format;self.queue.clear();self.latestFrame.reset();self.haveFrameNumber=false;
            self.receivedTimes.clear();self.decodedTimes.clear();self.publishedTimes.clear();self.recovery.Reset();
        }
        if(!self.OpenCodec(error)){self.Fail(error);return -1;}
        self.stopped=false;
        self.decodeThread=std::jthread([&self](std::stop_token stop)
        {try{self.Decode(stop);}catch(const std::exception& error){self.Fail(std::string("Decoder worker exception: ")+error.what());}
            catch(...){self.Fail("Decoder worker failed unexpectedly");}});return 0;
    }
    void HardwareVideoDecoder::Start(){implementation_->accepting=true;}
    void HardwareVideoDecoder::SetIdrRequester(std::function<void()> requester)
    {std::lock_guard lock(implementation_->stateMutex);implementation_->requestIdrCallback=std::move(requester);}
    void HardwareVideoDecoder::Stop()
    {
        auto& self=*implementation_;std::lock_guard lifecycle(self.lifecycleMutex);
        self.accepting=false;
        std::shared_ptr<FrameLease> retired;
        {std::lock_guard lock(self.stateMutex);self.stopped=true;++self.generation;++self.session;self.queue.clear();retired=std::move(self.latestFrame);self.stats.hardwareActive=false;}
        self.queueCondition.notify_all();self.frameCondition.notify_all();if(self.frameEvent.value)SetEvent(self.frameEvent.value);
        self.decodeThread.request_stop();
        if(self.decodeThread.joinable() && self.decodeThread.get_id()!=std::this_thread::get_id())self.decodeThread.join();
    }
    void HardwareVideoDecoder::Shutdown()
    {
        auto& self=*implementation_;std::lock_guard lifecycle(self.lifecycleMutex);Stop();self.decodeThread.request_stop();self.queueCondition.notify_all();
        if(self.decodeThread.joinable())self.decodeThread.join();
        std::scoped_lock lock(self.readbackMutex,self.previewMutex);
        self.DrainGpu();
        self.activePreview.reset();self.retiredPreviews.clear();self.previewSlots={};self.slots={};self.latestCpuSnapshot={};self.cpuPool.Clear();
        self.pendingGpuFrames.clear();self.gpuFramesInFlight=0;self.decodeCompletions.clear();self.yuvShader.Reset();self.previewDrawn=false;
        avcodec_free_context(&self.codec);av_buffer_unref(&self.hardwareDevice);self.pendingMetadata.clear();
        {std::lock_guard pool(self.packetPoolMutex);for(auto& entry:self.packetPools)av_buffer_pool_uninit(&entry);}
        self.renderFence.Reset();self.renderDecodeFence.Reset();self.decodeFence.Reset();self.decodeContext4.Reset();self.renderContext4.Reset();
        self.readbackCompletionFence.Reset();self.readbackCompletionContext.Reset();
        self.videoContext.Reset();self.videoDevice.Reset();self.context.Reset();self.device.Reset();self.renderContext.Reset();self.renderDevice.Reset();
        self.separateDevices=self.bindYuv=false;
    }

    int HardwareVideoDecoder::Submit(PDECODE_UNIT unit)
    {
        auto& self=*implementation_;
        if(!unit || !unit->bufferList || unit->fullLength<=0 || std::size_t(unit->fullLength)>kMaximumFrameBytes)return self.RejectAccessUnit();
        EncodedPacket packet;
        {
            std::lock_guard lock(self.stateMutex);
            if(!self.accepting || self.stopped || self.stats.failed)return DR_OK;
            ++self.stats.receivedFrames;self.stats.lastReceivedUs=StreamNowUs();self.receivedTimes.push_back({self.stats.lastReceivedUs,unit->fullLength});
            const auto number=std::uint32_t(unit->frameNumber),distance=number-self.lastFrameNumber;
            if(self.haveFrameNumber && distance>0 && distance<0x80000000U)self.stats.transportLostFrames+=distance-1;
            self.lastFrameNumber=number;self.haveFrameNumber=true;
            if(self.recovery.WaitingForKeyFrame() && unit->frameType!=FRAME_TYPE_IDR){++self.stats.droppedFrames;return DR_OK;}
            if(self.queue.size()>=kQueueLimit)
            {
                ++self.stats.overloadDrops;++self.stats.droppedFrames;
                self.recovery.Begin();
                self.stats.status="Compressed queue overloaded; waiting for recovery IDR";
                self.queueCondition.notify_one();return DR_OK; // Decoder thread owns the coalesced IDR request.
            }
            if(unit->frameType==FRAME_TYPE_IDR)self.recovery.OnKeyFrame();
            packet.metadata.generation=self.generation;
            packet.metadata.session=self.session;
        }
        packet.type=unit->frameType;packet.metadata.frameNumber=unit->frameNumber;packet.metadata.presentationTimeUs=unit->presentationTimeUs;
        packet.metadata.timing.receivedUs=unit->receiveTimeUs ? unit->receiveTimeUs : StreamNowUs();
        packet.metadata.timing.assembledUs=std::max(packet.metadata.timing.receivedUs,unit->enqueueTimeUs);
        packet.metadata.timing.hostProcessingTenthsMs=unit->frameHostProcessingLatency;
        self.reassemblyMetric.Add(double(packet.metadata.timing.assembledUs-packet.metadata.timing.receivedUs)/1000.0);
        if(unit->frameHostProcessingLatency)self.hostMetric.Add(unit->frameHostProcessingLatency/10.0);
        try
        {
            packet.buffer=self.PacketBuffer(std::size_t(unit->fullLength)+AV_INPUT_BUFFER_PADDING_SIZE);
            if(!packet.buffer)return self.RejectAccessUnit();
            packet.size=unit->fullLength;std::size_t offset{};
            for(auto* entry=unit->bufferList;entry;entry=entry->next)
            {
                if(!entry->data || entry->length<=0 || offset+std::size_t(entry->length)>std::size_t(unit->fullLength))return self.RejectAccessUnit();
                std::memcpy(packet.buffer->data+offset,entry->data,entry->length);offset+=entry->length;
            }
            if(offset!=std::size_t(unit->fullLength))return self.RejectAccessUnit();
            std::memset(packet.buffer->data+offset,0,AV_INPUT_BUFFER_PADDING_SIZE);
        }
        catch(const std::bad_alloc&){return self.RejectAccessUnit();}
        {
            std::lock_guard lock(self.stateMutex);
            if(self.stopped || packet.metadata.session!=self.session)return DR_OK;
            if(!self.queue.push_back(std::move(packet)))
            {++self.stats.overloadDrops;self.recovery.Begin();self.queueCondition.notify_one();return DR_OK;}
            ++self.stats.submittedFrames;
        }
        self.queueCondition.notify_one();return DR_OK;
    }

    bool HardwareVideoDecoder::Readback(const FrameReadbackRequest& request,CpuFrameSnapshot& output,std::stop_token stop)
    {
        auto& self=*implementation_;std::unique_lock readback(self.readbackMutex);
        output={};output.metadata.generation=self.generation.load();
        if(request.regions.empty())return false;
        std::stop_callback cancel(stop,[&]{if(self.frameEvent.value)SetEvent(self.frameEvent.value);});
        if(self.stopped || !self.context)
        {
            if(!stop.stop_requested())
            {if(self.frameEvent.value)WaitForSingleObject(self.frameEvent.value,20);
                else{std::unique_lock lock(self.stateMutex);self.frameCondition.wait_for(lock,stop,std::chrono::milliseconds(20),[&]{return !self.stopped;});}}
            output.metadata.generation=self.generation.load();return false;
        }
        const auto deadline=StreamNowUs()+20000;
        do
        {
            if(stop.stop_requested() || self.stopped || !self.context)return false;
            Implementation::ReadbackSlot* newest{};
            for(auto& slot:self.slots)if(slot.busy)
            {
                HRESULT ready{};{std::lock_guard gpu(self.contextMutex);ready=self.context->GetData(slot.completion.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);}
                if(FAILED(ready)){self.Fail("GPU completion failed: "+HrError(ready));return false;}
                if(ready!=S_OK)continue;
                const auto wanted=MergeFrameRegions(request.regions,slot.frame->metadata.sourceWidth,slot.frame->metadata.sourceHeight);
                if(slot.frame->metadata.generation!=self.generation || slot.frame->metadata.sequence<=request.afterSequence || (wanted&slot.rectangle)!=wanted)
                {slot.busy=false;slot.frame.reset();{std::lock_guard lock(self.stateMutex);++self.stats.readbackSuperseded;}continue;}
                if(!newest || slot.frame->metadata.sequence>newest->frame->metadata.sequence)
                {if(newest){newest->busy=false;newest->frame.reset();std::lock_guard lock(self.stateMutex);++self.stats.readbackSuperseded;}newest=&slot;}
                else{slot.busy=false;slot.frame.reset();std::lock_guard lock(self.stateMutex);++self.stats.readbackSuperseded;}
            }
            if(newest)self.CompleteReadback(*newest,output);
            if(!output.bgra.empty())self.latestCpuSnapshot=output;
            std::shared_ptr<FrameLease> latest;
            {std::lock_guard lock(self.stateMutex);latest=self.latestFrame;}
            if(latest && latest->metadata.sequence>request.afterSequence)
            {
                const auto region=MergeFrameRegions(request.regions,latest->metadata.sourceWidth,latest->metadata.sourceHeight);
                if(region.empty())
                {if(self.frameEvent.value && !stop.stop_requested())WaitForSingleObject(self.frameEvent.value,20);return false;}
                const auto& cached=self.latestCpuSnapshot;
                const cv::Rect cachedRegion(cached.metadata.cropX,cached.metadata.cropY,cached.bgra.cols,cached.bgra.rows);
                if((output.bgra.empty() || cached.metadata.sequence>output.metadata.sequence) && cached.metadata.generation==self.generation && cached.metadata.sequence==latest->metadata.sequence && (cachedRegion&region)==region)
                    output=cached;
                const bool pending=std::any_of(self.slots.begin(),self.slots.end(),[&](const auto& slot)
                    {return slot.busy && slot.frame->metadata.sequence==latest->metadata.sequence && (slot.rectangle&region)==region;});
                if((output.bgra.empty() || output.metadata.sequence<latest->metadata.sequence) && !pending)
                {
                    bool issued{};
                    for(auto& slot:self.slots)if(!slot.busy)
                {
                    if(!self.IssueReadback(slot,latest,region)){self.Fail("Cannot issue native GPU ROI readback");return false;}
                    self.lastIssuedSequence=latest->metadata.sequence;self.lastIssuedRegion=region;issued=true;break;
                }
                    if(!issued && self.lastSlotPressureSequence!=latest->metadata.sequence)
                    {self.lastSlotPressureSequence=latest->metadata.sequence;std::lock_guard lock(self.stateMutex);++self.stats.readbackSlotPressure;}
                }
            }
            if(!output.bgra.empty())return output.metadata.generation==self.generation;
            std::array<HANDLE,4> waits{};DWORD count{};bool pollingFallback{};
            if(self.frameEvent.value)waits[count++]=self.frameEvent.value;
            for(const auto& slot:self.slots)if(slot.busy)
            {if(slot.completionEvent)waits[count++]=slot.completionEvent->value;else pollingFallback=true;}
            const auto waitNow=StreamNowUs();const auto remaining=deadline>waitNow ? (deadline-waitNow+999)/1000 : 0;
            if(count)WaitForMultipleObjects(count,waits.data(),FALSE,DWORD(pollingFallback ? std::min<std::uint64_t>(remaining,1) : remaining));
            else {std::unique_lock lock(self.stateMutex);self.frameCondition.wait_for(lock,stop,std::chrono::milliseconds(1),[&]{return self.stopped || self.stats.failed;});}
            {std::lock_guard lock(self.stateMutex);if(self.stats.failed)return false;}
        } while(StreamNowUs()<deadline);
        return false;
    }
    void HardwareVideoDecoder::WakeWaiters(){implementation_->frameCondition.notify_all();}
    std::uint64_t HardwareVideoDecoder::CurrentGeneration() const{return implementation_->generation.load();}
    void HardwareVideoDecoder::RecordPublication(const CapturedFrameMetadata& metadata)
    {
        auto& self=*implementation_;
        if(metadata.generation!=self.generation || !metadata.timing.publishedUs || self.stopped)return;
        {std::lock_guard lock(self.stateMutex);
            if(metadata.generation!=self.generation || self.stopped || metadata.sequence<=self.lastPublishedSequence)return;
            self.lastPublishedSequence=metadata.sequence;
            ++self.stats.publishedFrames;self.stats.lastPublishedUs=metadata.timing.publishedUs;self.publishedTimes.push_back(metadata.timing.publishedUs);}
        self.publicationMetric.Add(double(metadata.timing.publishedUs-metadata.timing.receivedUs)/1000.0);
        if(metadata.timing.readbackUs && metadata.timing.publishedUs>=metadata.timing.readbackUs)
            self.dispatchMetric.Add(double(metadata.timing.publishedUs-metadata.timing.readbackUs)/1000.0);
    }

    void HardwareVideoDecoder::DrawPreview(ImDrawList* list,const ImVec2& position,const ImVec2& size)
    {
        auto& self=*implementation_;self.ReapPreviews();
        std::shared_ptr<FrameLease> frame;{std::lock_guard lock(self.stateMutex);frame=self.latestFrame;}
        if(!frame || frame->metadata.generation!=self.generation)return;
        std::lock_guard lock(self.previewMutex);
        if((!self.activePreview || self.activePreview->frame->metadata.generation!=self.generation ||
            (self.activePreview->frame->metadata.sequence!=frame->metadata.sequence && StreamNowUs()-self.previewUs>=16667)) && self.retiredPreviews.size()<6)
        {
            if(self.activePreview)
            {
                if(self.activePreview->submitted)self.retiredPreviews.push_back(self.activePreview);
                else if(self.activePreview->convertedSlot){self.activePreview->convertedSlot->busy=false;self.activePreview->convertedSlot->frame.reset();}
                self.activePreview.reset();
            }
            if(self.MakePreview(frame))self.previewUs=StreamNowUs();
        }
        if(!self.activePreview || self.activePreview->frame->metadata.generation!=self.generation)return;
        self.previewDrawn=true;
        const ImVec2 end(position.x+size.x,position.y+size.y);
        if(self.activePreview->y)
        {
            const auto& m=self.activePreview->frame->metadata;
            const ImVec2 uv0(float(m.visibleX)/m.textureWidth,float(m.visibleY)/m.textureHeight);
            const ImVec2 uv1(float(m.visibleX+m.sourceWidth)/m.textureWidth,float(m.visibleY+m.sourceHeight)/m.textureHeight);
            list->AddCallback(Implementation::BindPreviewShader,&self);
            list->AddImage(reinterpret_cast<ImTextureID>(self.activePreview->y.Get()),position,end,uv0,uv1);
            list->AddCallback(Implementation::UnbindPreviewShader,&self);
            list->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
        }
        else list->AddImage(reinterpret_cast<ImTextureID>(self.activePreview->bgraView.Get()),position,end);
    }
    void HardwareVideoDecoder::FinishPreviewFrame()
    {
        auto& self=*implementation_;std::lock_guard lock(self.previewMutex);
        if(!self.activePreview)return;
        if(!std::exchange(self.previewDrawn,false))
        {self.retiredPreviews.push_back(std::move(self.activePreview));return;}
        if(self.separateDevices)
        {
            self.activePreview->fenceValue=++self.nextRenderFence;
            if(FAILED(self.renderContext4->Signal(self.renderFence.Get(),self.activePreview->fenceValue)))return;
        }
        else
        {
            D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
            if(!self.activePreview->completion && FAILED(self.renderDevice->CreateQuery(&description,&self.activePreview->completion)))return;
            self.renderContext->End(self.activePreview->completion.Get());
        }
        self.renderContext->Flush();self.activePreview->submitted=true;
    }
    bool HardwareVideoDecoder::CopyLatestFrameToBgra(std::uint64_t after,cv::Mat& output,std::uint64_t& sequence)
    {
        CpuFrameSnapshot snapshot;FrameRegionRequest region;region.kind=FrameRegionKind::fullSource;
        if(!Readback({after,{region}},snapshot,{}))return false;
        output=snapshot.bgra;sequence=snapshot.metadata.sequence;return true;
    }

    DecodedVideoStats HardwareVideoDecoder::GetStats() const
    {
        auto& self=*implementation_;DecodedVideoStats result;
        std::lock_guard aggregate(self.aggregateMutex);const auto aggregationNow=StreamNowUs();
        {std::lock_guard lock(self.stateMutex);
            if(aggregationNow<self.nextAggregateUs && self.cachedStats.generation==self.generation &&
                self.cachedStats.failed==self.stats.failed && self.cachedStats.hardwareActive==self.stats.hardwareActive)return self.cachedStats;}
        {
            std::lock_guard lock(self.stateMutex);const auto now=StreamNowUs();
            while(!self.receivedTimes.empty() && now-self.receivedTimes.front().first>1000000)self.receivedTimes.pop_front();
            while(!self.decodedTimes.empty() && now-self.decodedTimes.front()>1000000)self.decodedTimes.pop_front();
            while(!self.publishedTimes.empty() && now-self.publishedTimes.front()>1000000)self.publishedTimes.pop_front();
            result=self.stats;result.sampledUs=aggregationNow;result.generation=self.generation.load();result.session=self.session.load();result.queueDepth=self.queue.size();result.gpuFramesInFlight=self.gpuFramesInFlight.load();result.receivedFramesPerSecond=double(self.receivedTimes.size());
            result.decodedFramesPerSecond=double(self.decodedTimes.size());result.publishedFramesPerSecond=double(self.publishedTimes.size());
            for(const auto& item:self.receivedTimes)result.encodedMegabitsPerSecond+=item.second*8.0/1000000.0;
        }
        result.reassembly=self.reassemblyMetric.Get();result.decode=self.decodeMetric.Get();result.readback=self.readbackMetric.Get();
        result.publication=self.publicationMetric.Get();result.hostProcessing=self.hostMetric.Get();
        result.compressedQueue=self.compressedQueueMetric.Get();result.decodeSubmit=self.decodeSubmitMetric.Get();result.gpuCompletionObserved=self.gpuCompletionMetric.Get();
        result.cpuCopy=self.cpuCopyMetric.Get();result.publicationDispatch=self.dispatchMetric.Get();
        result.network=SampleReceiverNetworkTraffic();self.cachedStats=result;self.nextAggregateUs=aggregationNow+1000000;return result;
    }
    bool HardwareVideoDecoder::SaveDiagnostics(const SunshineStreamConfig& config,const std::string& captureJson,std::string& result,const std::string& consumerCsv) const
    {
        const auto snapshot=GetStats();std::array<wchar_t,32768> executable{};
        GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
        const auto root=std::filesystem::path(executable.data()).parent_path();
        SYSTEMTIME time{};GetLocalTime(&time);wchar_t name[96]{};
        swprintf_s(name,L"vanta-stream-%04u%02u%02u-%02u%02u%02u-%lu",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
        const auto jsonPath=root/(std::wstring(name)+L".json"),csvPath=root/(std::wstring(name)+L".csv");
        std::ofstream json(jsonPath),csv(csvPath);
        if(!json || !csv){result="Cannot write diagnostics beside vanta.exe";return false;}
        json<<"{\n\"hardware\":"<<StreamHardwareReport()<<",\n\"requested\":{\"width\":"<<config.width<<",\"height\":"<<config.height
            <<",\"fps\":"<<config.fps<<",\"bitrate_budget_kbps\":"<<config.bitrateKbps<<",\"codec_preference\":"<<int(config.codec)
            <<",\"adapter_preference\":"<<JsonString(config.adapter)<<"},\n\"active\":{\"codec\":"<<JsonString(snapshot.codec)
            <<",\"adapter\":"<<JsonString(snapshot.adapter)<<",\"synchronization\":"<<JsonString(snapshot.synchronization)
            <<",\"status\":"<<JsonString(snapshot.status)<<",\"received_fps\":"<<snapshot.receivedFramesPerSecond<<",\"decoded_fps\":"<<snapshot.decodedFramesPerSecond
            <<",\"cpu_published_fps\":"<<snapshot.publishedFramesPerSecond<<",\"encoded_payload_mbps\":"<<snapshot.encodedMegabitsPerSecond
            <<",\"observed_ethernet_rx_mbps\":"<<snapshot.network.receiveMbps<<",\"observed_ethernet_tx_mbps\":"<<snapshot.network.transmitMbps
            <<",\"observed_traffic_available\":"<<(snapshot.network.available?"true":"false")<<",\"observed_traffic_scope\":\"all traffic across active Ethernet interfaces\""
            <<",\"transport_frame_gaps\":"<<snapshot.transportLostFrames<<",\"compressed_overload_events\":"<<snapshot.overloadDrops<<",\"decoder_failures\":"<<snapshot.decoderFailures
            <<",\"output_skipped\":"<<snapshot.outputSkippedFrames<<",\"idr_requests\":"<<snapshot.idrRequests<<",\"readback_bytes\":"<<snapshot.readbackBytes
            <<",\"generation\":"<<snapshot.generation<<",\"queue_depth\":"<<snapshot.queueDepth<<",\"visible_width\":"<<snapshot.latestMetadata.sourceWidth
            <<",\"gpu_frames_in_flight\":"<<snapshot.gpuFramesInFlight
            <<",\"visible_height\":"<<snapshot.latestMetadata.sourceHeight<<",\"visible_x\":"<<snapshot.latestMetadata.visibleX<<",\"visible_y\":"<<snapshot.latestMetadata.visibleY
            <<",\"texture_width\":"<<snapshot.latestMetadata.textureWidth<<",\"texture_height\":"<<snapshot.latestMetadata.textureHeight
            <<",\"last_frame_number\":"<<snapshot.latestMetadata.frameNumber<<",\"last_presentation_us\":"<<snapshot.latestMetadata.presentationTimeUs
            <<"},\n\"timing_scope\":\"receiver local, 10 seconds; host processing is optional and separate; no complete host-to-display latency\",\n\"capture\":"<<(captureJson.empty()?"{}":captureJson)<<"\n}\n";
        csv<<"stage,median_ms,p95_ms,p99_ms,samples\n";
        for(const auto& metric:std::array<std::pair<const char*,TimingSummary>,5>{{{"reassembly",snapshot.reassembly},{"decode_and_queue",snapshot.decode},
            {"gpu_readback",snapshot.readback},{"receiver_arrival_to_publication",snapshot.publication},{"host_processing",snapshot.hostProcessing}}})
            csv<<metric.first<<','<<metric.second.medianMs<<','<<metric.second.p95Ms<<','<<metric.second.p99Ms<<','<<metric.second.samples<<'\n';
        csv<<consumerCsv;
        if(!json.good() || !csv.good()){result="Writing diagnostics failed";return false;}
        result="Saved "+Utf8(jsonPath.c_str())+" and CSV timing summaries";return true;
    }
}
