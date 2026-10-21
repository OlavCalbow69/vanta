#include "capture_controller.hpp"
#include "hardware_video_decoder.hpp"
#include "logger.hpp"
#include <wrl/client.h>
namespace vanta
{
    bool RunSunshineCaptureSourceSelfTest()
    {
        // Opt-in receiver fixture: no host launch, mouse output or display changes.
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;std::string error;
        if(!HardwareVideoDecoder::CreateRenderDevice("auto",&device,&context,error))
        {log::Error("Receiver capture fixture: %s",error.c_str());return false;}
        std::atomic_uint64_t available{0},generation{1},requests{0};
        std::mutex fixtureMutex;std::condition_variable_any fixtureCondition;
        CaptureController capture;capture.SetGenerationProvider([&]{return generation.load();});
        capture.SetRegionFrameProvider([&](const FrameReadbackRequest& request,CpuFrameSnapshot& output,std::stop_token stop)
        {
            output.metadata.generation=generation.load();
            {std::unique_lock lock(fixtureMutex);fixtureCondition.wait_for(lock,stop,std::chrono::milliseconds(10),[&]{return available.load()>request.afterSequence;});}
            const auto sequence=available.load();if(stop.stop_requested() || sequence<=request.afterSequence)return false;
            output.metadata.generation=generation.load();
            ++requests;
            const int width=sequence<=4 ? 1920 : sequence<=8 ? 2560 : 1918;
            const int height=sequence<=4 ? 1080 : sequence<=8 ? 1440 : 1087;
            const auto roi=MergeFrameRegions(request.regions,width,height);if(roi.empty())return false;
            output.bgra=cv::Mat(roi.height,roi.width,CV_8UC4,cv::Scalar(double(13+sequence),42,99,255));
            auto& metadata=output.metadata;metadata.sequence=sequence;metadata.frameNumber=int(sequence*2);metadata.sourceWidth=width;metadata.sourceHeight=height;
            metadata.cropX=roi.x;metadata.cropY=roi.y;metadata.textureWidth=width+16;metadata.textureHeight=height+16;
            metadata.timing.receivedUs=metadata.timing.readbackUs=StreamNowUs();return true;
        });
        CaptureConfig config;config.regionIndex=1;config.regionSize=640;
        if(!capture.Initialize(device.Get(),context.Get(),&config))return false;
        auto normal=capture.Subscribe({FrameRegionKind::configuredCapture});
        auto python=capture.Subscribe({FrameRegionKind::centered,240});
        auto full=capture.Subscribe({FrameRegionKind::fullSource});
        const auto fail=[&](const char* reason){capture.Shutdown();log::Error("Receiver capture fixture: %s",reason);return false;};
        CpuFrameSnapshot old;
        for(std::uint64_t sequence=1;sequence<=9;++sequence)
        {
            if(sequence==9)generation=2;
            available=sequence;fixtureCondition.notify_all();CpuFrameSnapshot a,b,c;
            if(!normal.Wait(sequence-1,a,1000) || !python.Wait(sequence-1,b,1000) || !full.Wait(sequence-1,c,1000))return fail("asynchronous subscription timed out");
            const auto expected=ResolveFrameRegion({FrameRegionKind::centered,640},c.bgra.cols,c.bgra.rows);
            if(a.metadata.sequence!=sequence || b.metadata.sequence!=sequence || c.metadata.sequence!=sequence || a.bgra.size()!=cv::Size(640,640) ||
                b.bgra.size()!=cv::Size(240,240) || a.metadata.cropX!=expected.x || a.metadata.cropY!=expected.y ||
                a.metadata.frameNumber!=int(sequence*2) || a.bgra.at<cv::Vec4b>(0,0)!=cv::Vec4b(std::uint8_t(13+sequence),42,99,255))
                return fail("crop geometry, raw pixel or associated metadata changed");
            if(sequence==1)old=a;
        }
        if(capture.IsCurrent(old.metadata) || old.bgra.at<cv::Vec4b>(0,0)[0]!=14)return fail("generation/immutable ownership failed");
        CpuFrameSnapshot duplicate;if(normal.Wait(9,duplicate,20))return fail("duplicate frame delivered");
        normal.Update({FrameRegionKind::configuredCapture,640,{},0,false});
        python.Update({FrameRegionKind::centered,240,{},0,false});full.Update({FrameRegionKind::fullSource,640,{},0,false});
        const auto count=requests.load();
        {std::unique_lock lock(fixtureMutex);fixtureCondition.wait_for(lock,std::chrono::milliseconds(30));}
        if(requests.load()!=count)return fail("inactive subscriptions requested CPU pixels");
        capture.Shutdown();capture.Shutdown();
        if(normal.Wait(0,duplicate,0))return fail("shutdown retained a deliverable frame");
        log::Info("Receiver capture fixture passed: asynchronous ROI/Python/full-source, odd geometry, metadata, ownership, generation and shutdown");return true;
    }
}
