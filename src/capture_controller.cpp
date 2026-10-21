#define IMGUI_DEFINE_MATH_OPERATORS

#include "capture_controller.hpp"

#include "color_targets.hpp"
#include "logger.hpp"

#include <d3d11.h>
#include <wrl/client.h>

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
#include <thread>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    struct CapturedFrame
    {
        cv::Mat bgra;
        int sourceWidth{};
        int sourceHeight{};
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
    vanta::CaptureController* owner{};
    bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const vanta::CaptureConfig* initialConfig)
    {
        if (device == nullptr || context == nullptr)
        {
            status = "The Sunshine preview needs a graphics device";
            return false;
        }
        previewDevice = device;
        previewContext = context;
        if (initialConfig != nullptr) ApplyConfigValues(*initialConfig);
        SyncWorkerConfig();
        StartCapture();
        return true;
    }

    void Shutdown()
    {
        StopCapture();
        {std::lock_guard lock(subscriptionMutex);for(auto& weak:subscriptions)if(auto subscriptionState=weak.lock())
        {std::lock_guard frameLock(subscriptionState->mutex);subscriptionState->cancelled=true;subscriptionState->latest={};subscriptionState->condition.notify_all();}}
        regionFrameProvider={};
        previewShaderResource.Reset();
        previewTexture.Reset();
        previewContext.Reset();
        previewDevice.Reset();
    }

    void ApplyConfigValues(
        const vanta::CaptureConfig& config)
    {
        regionIndex =
            std::clamp(config.regionIndex, 0, 1);
        regionSize =
            std::clamp(config.regionSize, 128, 1280);
        drawOutline = config.drawOutline;
        outlineColor[0] = std::clamp(
            config.outlineColor.red, 0.0F, 1.0F);
        outlineColor[1] = std::clamp(
            config.outlineColor.green, 0.0F, 1.0F);
        outlineColor[2] = std::clamp(
            config.outlineColor.blue, 0.0F, 1.0F);
        outlineColor[3] = std::clamp(
            config.outlineColor.alpha, 0.0F, 1.0F);
        applyFilter = config.applyFilter;
        binaryMask = config.binaryMask;
        colorTargetIndex = std::clamp(
            config.colorTargetIndex,
            0,
            static_cast<int>(
                vanta::kHsvColorTargets.size()) - 1);

    }

    bool StartCapture()
    {
        StopCapture();
        if (!regionFrameProvider)
        {
            status = "Waiting for a Sunshine frame provider";
            return false;
        }
        status = "Waiting for the first decoded Sunshine video frame";
        running = true;
        captureWorkerFailed=false;
        frameCounter = cpuPublishedCount.load();
        sessionPublishedFrames = 0;
        framesPerSecond = 0.0F;
        firstFrameLogged = false;
        fpsWindowStart =
            std::chrono::steady_clock::now();
        nextFpsLog =
            fpsWindowStart +
            std::chrono::seconds(1);
        vanta::log::Info("capture started: %s", status.c_str());
        previewWorker=std::jthread([this](std::stop_token stop){RunPreview(stop);});
        captureWorker=std::jthread([this](std::stop_token stop)
        {try{RunCapture(stop);}catch(const std::exception& error){vanta::log::Error("Capture worker: %s",error.what());ClearPublished();captureWorkerFailed=true;}
            catch(...){vanta::log::Error("Capture worker failed unexpectedly");ClearPublished();captureWorkerFailed=true;}});
        return true;
    }

    void StopCapture()
    {
        captureWorker.request_stop();
        previewWorker.request_stop();previewWorkCondition.notify_all();
        demandCondition.notify_all();
        demandSignal->Wake();
        if (captureWorker.joinable()) captureWorker.join();
        if (previewWorker.joinable()) previewWorker.join();
        {std::lock_guard lock(previewWorkMutex);pendingPreview={};hasPendingPreview=false;}
        {
            std::lock_guard lock(subscriptionMutex);
            for (auto& weak:subscriptions) if(auto subscription=weak.lock())
            {std::lock_guard frameLock(subscription->mutex);subscription->latest={};subscription->condition.notify_all();}
        }
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
        running = false;
        previewShaderResource.Reset();
        previewTexture.Reset();
        previewWidth=previewHeight=0;
        lastPreviewSequence=0;
        {std::lock_guard backgroundLock(backgroundPreviewMutex);backgroundPreview={};}
        sourceWidth=sourceHeight=captureWidth=captureHeight=0;
        framesPerSecond=0.0F;
        {
            std::lock_guard<std::mutex> lock(latestFrameMutex);
            latestCenteredFrame.release();
            latestSourceFrame.release();
        }
        latestFrameCondition.notify_all();
    }

    void SyncWorkerConfig()
    {
        vanta::CaptureConfig config;
        config.regionIndex=regionIndex;config.regionSize=regionSize;config.applyFilter=applyFilter;
        config.binaryMask=binaryMask;config.colorTargetIndex=colorTargetIndex;
        {std::lock_guard lock(workerConfigMutex);workerConfig=config;++workerConfigRevision;}
        demandSignal->Wake();
    }
    static vanta::FrameRegionRequest EffectiveRegion(vanta::FrameRegionRequest request,const vanta::CaptureConfig& config)
    {
        if(request.kind==vanta::FrameRegionKind::configuredCapture)
        {
            request.kind=config.regionIndex==1 ? vanta::FrameRegionKind::centered : vanta::FrameRegionKind::fullSource;
            request.side=config.regionSize;
        }
        return request;
    }
    void BuildPreview(const cv::Mat& capture,const vanta::CapturedFrameMetadata& metadata,const vanta::CaptureConfig& config,std::uint64_t settingsVersion)
    {
        cv::Mat processed;int matches{};
        if(config.applyFilter || config.binaryMask)
        {
            cv::Mat bgr,hsv,mask;
            cv::cvtColor(capture,bgr,cv::COLOR_BGRA2BGR);cv::cvtColor(bgr,hsv,cv::COLOR_BGR2HSV);
            const int target=std::clamp(config.colorTargetIndex,0,int(vanta::kHsvColorTargets.size())-1);
            vanta::BuildHsvMask(hsv,target,mask);
            if(vanta::kHsvColorTargets[target].purpleBlacklist)RemoveBlacklistedPixels(bgr,mask);
            matches=cv::countNonZero(mask);
            if(config.binaryMask)cv::cvtColor(mask,processed,cv::COLOR_GRAY2BGRA);
            else{processed=cv::Mat::zeros(capture.size(),CV_8UC4);capture.copyTo(processed,mask);}
        }
        else processed=capture;
        const double scale=std::min({1.0,560.0/processed.cols,315.0/processed.rows});
        cv::Mat preview;
        if(scale<1.0)cv::resize(processed,preview,{std::max(1,int(std::lround(processed.cols*scale))),
            std::max(1,int(std::lround(processed.rows*scale)))},0,0,cv::INTER_AREA);
        else preview=processed;
        std::lock_guard lock(backgroundPreviewMutex);backgroundPreview={preview,metadata};backgroundMatches=matches;backgroundPreviewRevision=settingsVersion;
    }
    void SchedulePreview(const cv::Mat& image,const vanta::CapturedFrameMetadata& metadata,const vanta::CaptureConfig& config,std::uint64_t settingsVersion)
    {
        auto geometry=metadata;geometry.cropWidth=image.cols;geometry.cropHeight=image.rows;
        {std::lock_guard lock(previewWorkMutex);pendingPreview={image,geometry};pendingPreviewConfig=config;pendingPreviewRevision=settingsVersion;hasPendingPreview=true;}
        previewWorkCondition.notify_one();
    }
    void RunPreview(std::stop_token stop)
    {
        std::uint64_t next{};
        while(!stop.stop_requested())
        {
            vanta::CpuFrameSnapshot input;vanta::CaptureConfig config;std::uint64_t revision{};
            {
                std::unique_lock lock(previewWorkMutex);
                previewWorkCondition.wait(lock,stop,[&]{return hasPendingPreview;});
                const auto now=vanta::StreamNowUs();
                if(now<next)previewWorkCondition.wait_for(lock,stop,std::chrono::microseconds(next-now),[]{return false;});
                if(stop.stop_requested())break;
                input=std::move(pendingPreview);config=pendingPreviewConfig;revision=pendingPreviewRevision;hasPendingPreview=false;
                next=vanta::StreamNowUs()+16667;
            }
            if(input.bgra.empty() || vanta::StreamNowUs()>=previewVisibleUntil.load() ||
                (generationProvider && input.metadata.generation!=generationProvider()))continue;
            try
            {const auto started=vanta::StreamNowUs();BuildPreview(input.bgra,input.metadata,config,revision);
                if(owner)owner->RecordConsumer("Capture preview",input.metadata,started);}
            catch(const std::exception& error){vanta::log::Warning("Capture preview processing failed: %s",error.what());}
        }
    }
    void ClearPublished()
    {
        {std::lock_guard lock(latestFrameMutex);latestCenteredFrame.release();latestSourceFrame.release();}
        std::lock_guard lock(subscriptionMutex);
        for(auto& weak:subscriptions)if(auto subscriptionState=weak.lock())
        {std::lock_guard frameLock(subscriptionState->mutex);subscriptionState->latest={};subscriptionState->condition.notify_all();}
        {std::lock_guard backgroundLock(backgroundPreviewMutex);backgroundPreview={};}
        latestFrameCondition.notify_all();
    }
    void RunCapture(std::stop_token stop)
    {
        std::uint64_t after{},sourceGeneration{},previewLast{},observedDemand=~std::uint64_t{};
        schedulerWakeLateness.Clear();
        const auto waitForDemand=[&](std::uint64_t revision,std::uint64_t deadline=0)
        {
            demandSignal->WaitUntil(revision,deadline,stop);
            const auto woke=vanta::StreamNowUs();
            if(deadline && woke>=deadline && demandSignal->revision.load()==revision && !stop.stop_requested())
                schedulerWakeLateness.Add(double(woke-deadline)/1000.0);
        };
        vanta::CpuFrameSnapshot cachedInput;
        while(!stop.stop_requested())
        {
            const auto now=vanta::StreamNowUs(),revision=demandSignal->revision.load();
            std::uint64_t nextDemandUs{};
            const auto schedule=[&](std::uint64_t due){if(due && (!nextDemandUs || due<nextDemandUs))nextDemandUs=due;};
            vanta::CaptureConfig config;std::uint64_t configRevision{};
            {std::lock_guard lock(workerConfigMutex);config=workerConfig;configRevision=workerConfigRevision.load();}
            std::vector<std::pair<std::shared_ptr<vanta::CaptureSubscriptionState>,vanta::FrameRegionRequest>> interested;
            const bool demandChanged=observedDemand!=revision;
            vanta::FrameReadbackRequest request;request.afterSequence=demandChanged ? 0 : after;request.revision=revision;
            {
                std::lock_guard lock(subscriptionMutex);std::erase_if(subscriptions,[](const auto& weak){return weak.expired();});
                for(auto& weak:subscriptions)if(auto subscriptionState=weak.lock())
                {
                    std::lock_guard frameLock(subscriptionState->mutex);auto region=EffectiveRegion(subscriptionState->request,config);
                    if(subscriptionState->cancelled || !region.active)continue;
                    const auto due=vanta::FrameRequestDueUs(subscriptionState->lastPublishedUs,region.maximumFps);
                    if(due>now){schedule(due);continue;}
                    interested.push_back({subscriptionState,subscriptionState->request});request.regions.push_back(region);
                }
            }
            const bool preview=now<previewVisibleUntil.load(),compatibility=now<compatibilityDemandUntil.load();
            const vanta::FrameRegionRequest primary=EffectiveRegion({vanta::FrameRegionKind::configuredCapture,640,{},0,true},config);
            if(preview && !cachedInput.bgra.empty() && now-previewLast>=16667 &&
                (!generationProvider || cachedInput.metadata.generation==generationProvider()) &&
                (cachedInput.metadata.sequence!=lastBuiltPreviewSequence || configRevision!=lastBuiltPreviewRevision))
            {
                const auto wanted=vanta::ResolveFrameRegion(primary,cachedInput.metadata.sourceWidth,cachedInput.metadata.sourceHeight);
                const cv::Rect available(cachedInput.metadata.cropX,cachedInput.metadata.cropY,cachedInput.bgra.cols,cachedInput.bgra.rows);
                if(!wanted.empty() && (wanted&available)==wanted)
                {auto metadata=cachedInput.metadata;metadata.cropX=wanted.x;metadata.cropY=wanted.y;
                    SchedulePreview(cachedInput.bgra({wanted.x-available.x,wanted.y-available.y,wanted.width,wanted.height}),metadata,config,configRevision);
                    previewLast=now;lastBuiltPreviewSequence=metadata.sequence;lastBuiltPreviewRevision=configRevision;}
            }
            if(compatibility || (preview && now-previewLast>=16667))
            {auto previewRequest=primary;previewRequest.maximumFps=compatibility ? 0 : 60;request.regions.push_back(previewRequest);}
            else if(preview)schedule(vanta::FrameRequestDueUs(previewLast,60));
            if(request.regions.empty())
            {
                if(generationProvider && sourceGeneration!=generationProvider())
                {ClearPublished();sourceGeneration=generationProvider();}
                waitForDemand(revision,nextDemandUs);continue;
            }
            // Each capped subscription/preview already contributes only when
            // due. A second global throttle can delay a faster subscription
            // behind a slower one, or impose an extra polling sleep.
            vanta::CpuFrameSnapshot snapshot;bool received{};const auto acquisitionStarted=vanta::StreamNowUs();
            if(regionFrameProvider)received=regionFrameProvider(request,snapshot,stop);
            if(sourceGeneration && snapshot.metadata.generation && snapshot.metadata.generation!=sourceGeneration)ClearPublished();
            if(snapshot.metadata.generation)sourceGeneration=snapshot.metadata.generation;
            if(!received || snapshot.bgra.empty() || snapshot.bgra.type()!=CV_8UC4 || (!demandChanged && snapshot.metadata.sequence<=after))
            {
                if(!regionFrameProvider || vanta::StreamNowUs()-acquisitionStarted<1000)
                {waitForDemand(revision,vanta::StreamNowUs()+1000);}
                continue;
            }
            if(stop.stop_requested())break;
            if(generationProvider && snapshot.metadata.generation!=generationProvider())continue;
            const bool unique=snapshot.metadata.sequence>after;
            after=std::max(after,snapshot.metadata.sequence);observedDemand=revision;
            const cv::Rect available(snapshot.metadata.cropX,snapshot.metadata.cropY,snapshot.bgra.cols,snapshot.bgra.rows);
            const auto published=vanta::StreamNowUs();snapshot.metadata.timing.publishedUs=published;
            cachedInput=snapshot;
            for(auto& [subscriptionState,original]:interested)
            {
                const auto wanted=vanta::ResolveFrameRegion(EffectiveRegion(original,config),snapshot.metadata.sourceWidth,snapshot.metadata.sourceHeight);
                if(wanted.empty() || (wanted&available)!=wanted)continue;
                auto metadata=snapshot.metadata;metadata.cropX=wanted.x;metadata.cropY=wanted.y;metadata.cropWidth=wanted.width;metadata.cropHeight=wanted.height;
                std::lock_guard frameLock(subscriptionState->mutex);if(subscriptionState->cancelled || subscriptionState->request!=original)continue;
                if(subscriptionState->latest.metadata.sequence>metadata.sequence)continue;
                subscriptionState->latest={snapshot.bgra({wanted.x-available.x,wanted.y-available.y,wanted.width,wanted.height}),metadata};
                subscriptionState->lastPublishedUs=published;subscriptionState->condition.notify_all();
            }
            const auto primaryCrop=vanta::ResolveFrameRegion(primary,snapshot.metadata.sourceWidth,snapshot.metadata.sourceHeight);
            if(!primaryCrop.empty() && (primaryCrop&available)==primaryCrop)
            {
                auto metadata=snapshot.metadata;metadata.cropX=primaryCrop.x;metadata.cropY=primaryCrop.y;metadata.cropWidth=primaryCrop.width;metadata.cropHeight=primaryCrop.height;
                const cv::Mat image=snapshot.bgra({primaryCrop.x-available.x,primaryCrop.y-available.y,primaryCrop.width,primaryCrop.height});
                {
                    std::lock_guard lock(latestFrameMutex);latestCenteredFrame=image;latestMetadata=metadata;latestFrameSequence=metadata.sequence;++sessionPublishedFrames;
                    if(available==cv::Rect(0,0,metadata.sourceWidth,metadata.sourceHeight))latestSourceFrame=snapshot.bgra;else latestSourceFrame.release();
                }
                latestFrameCondition.notify_all();
            }
            if(unique)++cpuPublishedCount;lastCpuPublishedUs=published;
            {std::lock_guard lock(metadataMutex);lastPublishedMetadata=snapshot.metadata;lastPublishedMetadata.cropWidth=snapshot.bgra.cols;lastPublishedMetadata.cropHeight=snapshot.bgra.rows;}
            if(publicationObserver)publicationObserver(snapshot.metadata);
            if(preview && published-previewLast>=16667 && !primaryCrop.empty() && (primaryCrop&available)==primaryCrop)
            {auto metadata=snapshot.metadata;metadata.cropX=primaryCrop.x;metadata.cropY=primaryCrop.y;
                SchedulePreview(snapshot.bgra({primaryCrop.x-available.x,primaryCrop.y-available.y,primaryCrop.width,primaryCrop.height}),metadata,config,configRevision);
                previewLast=published;lastBuiltPreviewSequence=metadata.sequence;lastBuiltPreviewRevision=configRevision;}
        }
    }
    void Tick()
    {
        if(!running)return;
        if(captureWorkerFailed){status="Capture worker failed; see vanta.log and restart capture";framesPerSecond=0;return;}
        const auto now=std::chrono::steady_clock::now();const double elapsed=std::chrono::duration<double>(now-fpsWindowStart).count();
        if(elapsed>=1.0){const auto count=cpuPublishedCount.load();framesPerSecond=float((count-frameCounter)/elapsed);frameCounter=count;fpsWindowStart=now;}
        if(vanta::StreamNowUs()-lastCpuPublishedUs.load()>1000000)framesPerSecond=0;
        vanta::CpuFrameSnapshot preview;int matches{};std::uint64_t previewRevision{};
        {std::lock_guard lock(backgroundPreviewMutex);preview=backgroundPreview;matches=backgroundMatches;previewRevision=backgroundPreviewRevision;}
        if(generationProvider && lastPreviewGeneration!=generationProvider())
        {previewShaderResource.Reset();previewTexture.Reset();lastPreviewSequence=0;sourceWidth=sourceHeight=captureWidth=captureHeight=0;lastPreviewGeneration=generationProvider();}
        if(generationProvider && preview.metadata.generation!=generationProvider())return;
        if(!preview.bgra.empty() && (preview.metadata.sequence!=lastPreviewSequence || previewRevision!=lastPreviewRevision) && vanta::StreamNowUs()<previewVisibleUntil.load())
        {
            UploadPreview(preview.bgra);lastPreviewSequence=preview.metadata.sequence;lastPreviewRevision=previewRevision;sourceWidth=preview.metadata.sourceWidth;sourceHeight=preview.metadata.sourceHeight;
            vanta::CaptureConfig config;{std::lock_guard lock(workerConfigMutex);config=workerConfig;}
            const auto region=vanta::ResolveFrameRegion(EffectiveRegion({vanta::FrameRegionKind::configuredCapture},config),sourceWidth,sourceHeight);
            captureWidth=region.width;captureHeight=region.height;matchedPixels=matches;status="Receiving native GPU ROI frames";
        }
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

        bool settingsChanged = false;
        if (custom::Child(
                ICON_SETTINGS_3_LINE
                    "  Capture settings##capture-settings",
                ImVec2(settingsWidth, childBodyHeight),
                true))
        {
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));
            const char* regions[]{"Full source","Centered square"};
            ImGui::TextUnformatted("Source: Sunshine stream");
            ImGui::TextWrapped("Native decoded frames feed TestMove, TestClick and Python. Pair and start the stream on Stream Receiver.");
            ImGui::TextDisabled("No desktop or window capture is used.");
            custom::Separator();
            settingsChanged |= custom::Combo(
                "Capture area",
                &regionIndex,
                regions,
                IM_ARRAYSIZE(regions));
            if (regionIndex == 1)
            {
                settingsChanged |= custom::SliderInt(
                    "Square size",
                    &regionSize,
                    128,
                    1280,
                    "%d px");
            }

            settingsChanged |= custom::Checkbox("Draw preview border",&drawOutline);
            if (drawOutline)
                settingsChanged |= custom::ColorEdit4("Preview border color",outlineColor,
                    ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_AlphaBar |
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview);

            settingsChanged |= custom::Checkbox(
                "Apply HSV + blacklist filter",
                &applyFilter);
            settingsChanged |= custom::Checkbox(
                "Black/white mask",
                &binaryMask);
            const char* colorTargets[
                vanta::kHsvColorTargets.size()]{};
            for (std::size_t index = 0;
                 index <
                    vanta::kHsvColorTargets.size();
                 ++index)
            {
                colorTargets[index] =
                    vanta::kHsvColorTargets[index].label;
            }
            settingsChanged |= custom::Combo(
                "Preview color target",
                &colorTargetIndex,
                colorTargets,
                static_cast<int>(
                    vanta::kHsvColorTargets.size()));
            const auto& colorTarget =
                vanta::kHsvColorTargets[
                    static_cast<std::size_t>(
                        std::clamp(
                            colorTargetIndex,
                            0,
                            static_cast<int>(
                                vanta::kHsvColorTargets.size()) - 1))];
            ImGui::TextDisabled("%s\n%s", colorTarget.description,
                colorTarget.purpleBlacklist ? "Purple blacklist enabled" : "Purple blacklist not applied");

            ImGui::PopStyleVar();
        }
        custom::EndChild();

        if (settingsChanged)
        {
            SyncWorkerConfig();
            settingsRevision.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        ImGui::SameLine(0.0F, gap);
        if (custom::Child(
                ICON_VIDEO_CAMERA_LINE
                    "  Capture output##capture-output",
                ImVec2(previewPanelWidth, childBodyHeight),
                true))
        {
            const auto visibleNow=vanta::StreamNowUs();
            if(previewVisibleUntil.exchange(visibleNow+100000)<=visibleNow)demandSignal->Wake();
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
                if (drawOutline)
                    ImGui::GetWindowDrawList()->AddRect(
                        imagePosition-ImVec2(4.0F,4.0F),
                        imagePosition+ImVec2(displayWidth,displayHeight)+ImVec2(4.0F,4.0F),
                        ImGui::GetColorU32(ImVec4(outlineColor[0],outlineColor[1],outlineColor[2],outlineColor[3])),6.0F);
                ImGui::Image(
                    reinterpret_cast<ImTextureID>(
                        previewShaderResource.Get()),
                    ImVec2(displayWidth, displayHeight));
                if (fovPreviewVisible && captureWidth>0 && captureHeight>0)
                {
                    const int radius=std::clamp(fovPreviewRadius,0,std::max(0,std::min(captureWidth,captureHeight)/2-3));
                    if (radius>0)
                        ImGui::GetWindowDrawList()->AddCircle(
                            imagePosition+ImVec2(displayWidth*.5F,displayHeight*.5F),
                            radius*displayWidth/static_cast<float>(captureWidth),
                            ImGui::GetColorU32(ImVec4(fovPreviewColor.red,fovPreviewColor.green,
                                fovPreviewColor.blue,fovPreviewColor.alpha)),0,1.5F);
                }
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
                const char* placeholder = "Waiting for decoded Sunshine frames...";
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

    bool running = false;
    int regionIndex = 1;
    int regionSize = 640;
    bool drawOutline = true;
    bool applyFilter = false;
    bool binaryMask = false;
    int colorTargetIndex = 0;
    std::atomic<std::uint64_t> settingsRevision{0};
    bool firstFrameLogged = false;
    float outlineColor[4]{
        0.68F,
        0.56F,
        0.91F,
        1.0F};
    std::string status;
    vanta::SunshineRegionFrameProvider regionFrameProvider;
    std::jthread captureWorker;
    std::jthread previewWorker;
    std::mutex previewWorkMutex;
    std::condition_variable_any previewWorkCondition;
    vanta::CpuFrameSnapshot pendingPreview;
    vanta::CaptureConfig pendingPreviewConfig;
    std::uint64_t pendingPreviewRevision{};
    bool hasPendingPreview{};
    std::atomic_bool captureWorkerFailed{false};
    std::function<std::uint64_t()> generationProvider;
    std::function<void(const vanta::CapturedFrameMetadata&)> publicationObserver;
    std::mutex subscriptionMutex,workerConfigMutex,backgroundPreviewMutex,demandMutex;
    std::condition_variable_any demandCondition;
    std::shared_ptr<vanta::CaptureDemandSignal> demandSignal=std::make_shared<vanta::CaptureDemandSignal>();
    vanta::RollingStreamMetric schedulerWakeLateness;
    std::vector<std::weak_ptr<vanta::CaptureSubscriptionState>> subscriptions;
    vanta::CaptureConfig workerConfig;
    std::atomic_uint64_t workerConfigRevision{0};
    std::atomic_uint64_t previewVisibleUntil{0},compatibilityDemandUntil{0};
    std::atomic_uint64_t cpuPublishedCount{0},lastCpuPublishedUs{0};
    mutable std::mutex metadataMutex;
    vanta::CapturedFrameMetadata lastPublishedMetadata;
    vanta::CpuFrameSnapshot backgroundPreview;
    int backgroundMatches{};
    std::uint64_t backgroundPreviewRevision{},lastBuiltPreviewSequence{},lastBuiltPreviewRevision{};
    std::uint64_t lastPreviewSequence{},lastPreviewRevision{},lastPreviewGeneration{},previewRateUs{};
    struct ConsumerStats
    {
        bool processingExpected{};
        std::string state;
        std::uint64_t frames{},lastUs{},generation{};
        std::deque<std::uint64_t> times;
        vanta::RollingStreamMetric processing,age;
        vanta::RollingStreamMetric algorithm;
    };
    mutable std::mutex consumerMutex;
    std::map<std::string,ConsumerStats> consumers;

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
    bool fovPreviewVisible{};
    int fovPreviewRadius{100};
    vanta::RgbaColor fovPreviewColor{0.68F,0.56F,0.91F,1.0F};

    // Full decoded source and centered ROI share immutable BGRA storage.
    mutable std::mutex latestFrameMutex;
    mutable std::condition_variable latestFrameCondition;
    cv::Mat latestCenteredFrame;
    cv::Mat latestSourceFrame;
    std::uint64_t latestFrameSequence{};
    std::uint64_t sessionPublishedFrames{};
    vanta::CapturedFrameMetadata latestMetadata;

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
        const CaptureConfig* initialConfig)
    {
        implementation_->owner=this;
        return implementation_->Initialize(
            previewDevice,
            previewContext,
            initialConfig);
    }

    void CaptureController::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }

    void CaptureController::SetRegionFrameProvider(SunshineRegionFrameProvider provider)
    {
        auto& impl=*implementation_;impl.StopCapture();
        impl.regionFrameProvider=std::move(provider);
        if(impl.previewDevice)impl.StartCapture();
    }
    CaptureSubscription CaptureController::Subscribe(const FrameRegionRequest& request,const char* consumerName)
    {
        auto subscriptionState=std::make_shared<CaptureSubscriptionState>();subscriptionState->request=request;subscriptionState->demand=implementation_->demandSignal;
        subscriptionState->consumerName=consumerName;
        subscriptionState->generationProvider=implementation_->generationProvider;
        {std::lock_guard lock(implementation_->subscriptionMutex);implementation_->subscriptions.push_back(subscriptionState);}
        subscriptionState->demand->Wake();return CaptureSubscription(std::move(subscriptionState));
    }
    void CaptureController::SetGenerationProvider(std::function<std::uint64_t()> provider)
    {implementation_->generationProvider=std::move(provider);}
    CaptureController::DemandStats CaptureController::GetDemandStats(int width,int height) const
    {
        auto& impl=*implementation_;DemandStats result;result.running=impl.running;result.workerFailed=impl.captureWorkerFailed;
        result.schedulerWakeLateness=impl.schedulerWakeLateness.Get();result.waitMode=impl.demandSignal->WaitMode();
        CaptureConfig config;{std::lock_guard lock(impl.workerConfigMutex);config=impl.workerConfig;}
        const auto append=[&](const std::string& name,const FrameRegionRequest& request)
        {
            if(!request.active)return;
            const auto effective=Implementation::EffectiveRegion(request,config);
            const auto rectangle=ResolveFrameRegion(effective,width,height);
            result.regions.push_back({name,effective,rectangle});
            result.streamRate=result.streamRate || effective.maximumFps==0;
            result.maximumFps=std::max(result.maximumFps,effective.maximumFps);
            if(!rectangle.empty())result.readbackUnion=result.readbackUnion.empty()?rectangle:result.readbackUnion|rectangle;
        };
        {std::lock_guard lock(impl.subscriptionMutex);
            for(const auto& weak:impl.subscriptions)if(const auto subscription=weak.lock())
            {std::lock_guard frameLock(subscription->mutex);if(!subscription->cancelled)append(subscription->consumerName,subscription->request);}}
        const auto now=StreamNowUs();
        if(now<impl.previewVisibleUntil.load())append("Capture preview",{FrameRegionKind::configuredCapture,640,{},60,true});
        if(now<impl.compatibilityDemandUntil.load())append("Compatibility capture",{FrameRegionKind::configuredCapture,640,{},0,true});
        return result;
    }
    void CaptureController::SetConsumerState(const char* name,bool expected,const char* activity)
    {
        auto& impl=*implementation_;std::lock_guard lock(impl.consumerMutex);
        auto& consumer=impl.consumers[name];consumer.processingExpected=expected;
        if(consumer.state!=activity)consumer.state=activity;
    }
    void CaptureController::SetPublicationObserver(std::function<void(const CapturedFrameMetadata&)> observer)
    {implementation_->publicationObserver=std::move(observer);}
    bool CaptureController::IsCurrent(const CapturedFrameMetadata& metadata) const
    {const auto& impl=*implementation_;return !impl.generationProvider || metadata.generation==impl.generationProvider();}
    void CaptureController::RecordConsumer(const char* name,const CapturedFrameMetadata& metadata,std::uint64_t startedUs)
    {
        auto& impl=*implementation_;const auto now=StreamNowUs();std::lock_guard lock(impl.consumerMutex);
        if(!IsCurrent(metadata))return;
        auto& stats=impl.consumers[name];
        if(stats.generation!=metadata.generation){stats.generation=metadata.generation;stats.frames=0;stats.times.clear();stats.processing.Clear();stats.age.Clear();stats.algorithm.Clear();}
        ++stats.frames;stats.lastUs=now;stats.times.push_back(now);
        while(!stats.times.empty() && now-stats.times.front()>1000000)stats.times.pop_front();
        stats.processing.Add(double(now-startedUs)/1000.0);
        if(metadata.timing.receivedUs && startedUs>=metadata.timing.receivedUs)
            stats.age.Add(double(startedUs-metadata.timing.receivedUs)/1000.0);
    }
    void CaptureController::RecordConsumerAlgorithm(const char* name,const CapturedFrameMetadata& metadata,std::uint64_t startedUs)
    {
        auto& impl=*implementation_;const auto now=StreamNowUs();std::lock_guard lock(impl.consumerMutex);
        if(!IsCurrent(metadata))return;auto& stats=impl.consumers[name];
        if(stats.generation!=metadata.generation)
        {stats.generation=metadata.generation;stats.frames=0;stats.times.clear();stats.processing.Clear();stats.age.Clear();stats.algorithm.Clear();}
        stats.algorithm.Add(double(now-startedUs)/1000.0);
    }
    CapturedFrameMetadata CaptureController::LastPublicationMetadata() const
    {const auto& impl=*implementation_;std::lock_guard lock(impl.metadataMutex);return impl.lastPublishedMetadata;}
    std::string CaptureController::DiagnosticsJson() const
    {
        const auto& impl=*implementation_;std::ostringstream output;
        CapturedFrameMetadata frame;{std::lock_guard lock(impl.metadataMutex);frame=impl.lastPublishedMetadata;}
        output<<"{\"cpu_published_total\":"<<impl.cpuPublishedCount.load()<<",\"last_publication\":{\"sequence\":"<<frame.sequence
            <<",\"generation\":"<<frame.generation<<",\"frame_number\":"<<frame.frameNumber<<",\"crop_x\":"<<frame.cropX<<",\"crop_y\":"<<frame.cropY
            <<",\"crop_width\":"<<frame.cropWidth<<",\"crop_height\":"<<frame.cropHeight<<",\"source_width\":"<<frame.sourceWidth<<",\"source_height\":"<<frame.sourceHeight
            <<",\"presentation_us\":"<<frame.presentationTimeUs<<",\"received_us\":"<<frame.timing.receivedUs<<",\"assembled_us\":"<<frame.timing.assembledUs
            <<",\"submitted_us\":"<<frame.timing.submittedUs<<",\"decoded_us\":"<<frame.timing.decodedUs<<",\"readback_issued_us\":"<<frame.timing.readbackStartedUs
            <<",\"readback_us\":"<<frame.timing.readbackUs<<",\"published_us\":"<<frame.timing.publishedUs<<"},\"consumers\":{";
        std::lock_guard lock(impl.consumerMutex);bool first=true;const auto now=StreamNowUs();
        for(const auto& [name,stats]:impl.consumers)
        {
            if(!first)output<<',';first=false;
            const auto duration=stats.processing.Get(),age=stats.age.Get();std::size_t rate{};
            for(auto time:stats.times)if(now-time<=1000000)++rate;
            output<<'"'<<name<<"\":{\"frames\":"<<stats.frames<<",\"fps\":"<<rate
                <<",\"processing_median_ms\":"<<duration.medianMs<<",\"processing_p95_ms\":"<<duration.p95Ms
                <<",\"processing_p99_ms\":"<<duration.p99Ms<<",\"receiver_age_median_ms\":"<<age.medianMs
                <<",\"receiver_age_p95_ms\":"<<age.p95Ms<<",\"receiver_age_p99_ms\":"<<age.p99Ms<<'}';
        }
        output<<"}}";return output.str();
    }
    std::vector<CaptureController::ConsumerPerformance> CaptureController::ConsumerPerformanceStats() const
    {
        const auto& impl=*implementation_;std::lock_guard lock(impl.consumerMutex);std::vector<ConsumerPerformance> result;
        const auto now=StreamNowUs();
        for(const auto& [name,stats]:impl.consumers)
        {double rate{};for(auto time:stats.times)if(now-time<=1000000)++rate;
            const bool current=!impl.generationProvider || stats.generation==impl.generationProvider();
            result.push_back({name,current?rate:0,stats.frames,current?stats.processing.Get():TimingSummary{},current?stats.age.Get():TimingSummary{},stats.processingExpected,stats.state,current?stats.algorithm.Get():TimingSummary{}});}
        return result;
    }
    std::string CaptureController::DiagnosticsCsv() const
    {
        std::ostringstream csv;
        for(const auto& consumer:ConsumerPerformanceStats())
        {const auto row=[&](const char* stage,const TimingSummary& metric)
            {csv<<consumer.name<<'_'<<stage<<','<<metric.medianMs<<','<<metric.p95Ms<<','<<metric.p99Ms<<','<<metric.samples<<'\n';};
            row("processing",consumer.processing);row("receiver_frame_age",consumer.age);}
        return csv.str();
    }

    void CaptureController::Tick()
    {
        implementation_->Tick();
    }

    void CaptureController::RenderPanel()
    {
        implementation_->RenderPanel();
    }

    CaptureConfig CaptureController::GetConfig() const
    {
        const auto& impl = *implementation_;
        CaptureConfig result;
        result.regionIndex =
            impl.regionIndex;
        result.regionSize =
            impl.regionSize;
        result.drawOutline =
            impl.drawOutline;
        result.outlineColor = {
            impl.outlineColor[0],
            impl.outlineColor[1],
            impl.outlineColor[2],
            impl.outlineColor[3]};
        result.applyFilter =
            impl.applyFilter;
        result.binaryMask =
            impl.binaryMask;
        result.colorTargetIndex =
            impl.colorTargetIndex;
        return result;
    }

    void CaptureController::ApplyConfig(
        const CaptureConfig& config)
    {
        auto& impl = *implementation_;
        impl.ApplyConfigValues(config);
        impl.SyncWorkerConfig();
        if (!impl.StartCapture())
        {
            vanta::log::Warning(
                "capture config apply could not start source: %s",
                impl.status.c_str());
        }
        impl.settingsRevision.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    std::uint64_t
    CaptureController::SettingsRevision() const noexcept
    {
        return implementation_->
            settingsRevision.load(
                std::memory_order_relaxed);
    }

    void CaptureController::SetFovPreview(const bool visible,const int radius,const RgbaColor& fovColor)
    {
        auto& impl=*implementation_;
        impl.fovPreviewVisible=visible;
        impl.fovPreviewRadius=radius;
        impl.fovPreviewColor=fovColor;
    }

    bool CaptureController::GetLatestCenteredFrame(cv::Mat& out) const
    {
        implementation_->compatibilityDemandUntil=StreamNowUs()+100000;implementation_->demandSignal->Wake();
        std::lock_guard<std::mutex> lock(
            implementation_->latestFrameMutex);
        if (implementation_->latestCenteredFrame.empty() || !IsCurrent(implementation_->latestMetadata))
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
        CapturedFrameMetadata metadata;
        if (!WaitForCenteredFrameWithMetadata(afterSequence, out, metadata, timeoutMilliseconds)) return false;
        sequence = metadata.sequence;
        captureTimestampNanoseconds = metadata.captureTimestampNanoseconds;
        return true;
    }

    bool CaptureController::WaitForCenteredFrameWithMetadata(
        const std::uint64_t afterSequence, cv::Mat& out,
        CapturedFrameMetadata& metadata, const std::uint32_t timeoutMilliseconds) const
    {
        implementation_->compatibilityDemandUntil=StreamNowUs()+100000;implementation_->demandSignal->Wake();
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
        if (!available || !IsCurrent(implementation_->latestMetadata))
        {
            return false;
        }
        out = implementation_->latestCenteredFrame;
        metadata = implementation_->latestMetadata;
        return true;
    }

    bool CaptureController::WaitForSourceFrameWithMetadata(
        const std::uint64_t afterSequence, cv::Mat& out,
        CapturedFrameMetadata& metadata, const std::uint32_t timeoutMilliseconds) const
    {
        FrameRegionRequest request;request.kind=FrameRegionKind::fullSource;
        auto subscription=const_cast<CaptureController*>(this)->Subscribe(request);CpuFrameSnapshot snapshot;
        if(!subscription.Wait(afterSequence,snapshot,timeoutMilliseconds))return false;
        out=snapshot.bgra;metadata=snapshot.metadata;return true;
    }

}
