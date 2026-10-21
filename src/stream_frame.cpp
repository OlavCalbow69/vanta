#include <WinSock2.h>
#include <ws2ipdef.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "stream_frame.hpp"
#include "hardware_video_decoder.hpp"
#include "logger.hpp"
#include "python_bot.hpp"
#include "version.hpp"
#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <sstream>
#include <thread>
#pragma warning(push,0)
extern "C" {
#include <libavutil/avutil.h>
}
#pragma warning(pop)
namespace
{
    std::string Utf8(const wchar_t* value)
    {
        if(!value || !*value)return {};
        const int count=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
        std::string text(std::size_t(count),0);WideCharToMultiByte(CP_UTF8,0,value,-1,text.data(),count,nullptr,nullptr);
        text.pop_back();return text;
    }
    std::string Quote(const std::string& value)
    {
        std::string result="\"";
        for(unsigned char c:value){if(c=='"' || c=='\\'){result+='\\';result+=char(c);}else if(c>=32)result+=char(c);}
        return result+'"';
    }
}
namespace vanta
{
    struct CaptureDemandSignal::WaitState
    {
        HANDLE wake=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE|SYNCHRONIZE);
        bool highResolution=timer!=nullptr;
        std::atomic_bool nativeFailed{false};
        WaitState(){if(!timer)timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);}
        ~WaitState(){if(timer)CloseHandle(timer);if(wake)CloseHandle(wake);}
    };
    CaptureDemandSignal::CaptureDemandSignal():wait_(std::make_unique<WaitState>()){}
    CaptureDemandSignal::~CaptureDemandSignal()=default;
    void CaptureDemandSignal::Wake()
    {
        // The condition-variable fallback also needs the predicate mutation
        // under its wait mutex to avoid losing a wake between check and sleep.
        {std::lock_guard lock(mutex);++revision;}
        if(wait_->wake)SetEvent(wait_->wake);
        condition.notify_all();
    }
    const char* CaptureDemandSignal::WaitMode() const
    {
        if(!wait_->wake || !wait_->timer || wait_->nativeFailed)return "Cancellable condition-variable fallback";
        return wait_->highResolution ? "High-resolution deadline timer + demand event" : "Deadline timer + demand event";
    }
    void CaptureDemandSignal::WaitUntil(std::uint64_t expected,std::uint64_t deadline,std::stop_token stop)
    {
        const auto changed=[&]{return revision.load()!=expected;};
        const auto fallback=[&]
        {
            std::unique_lock lock(mutex);
            if(!deadline)condition.wait(lock,stop,changed);
            else
            {const auto now=StreamNowUs();if(deadline>now)condition.wait_for(lock,stop,std::chrono::microseconds(deadline-now),changed);}
        };
        if(stop.stop_requested() || changed())return;
        if(!wait_->wake || (deadline && !wait_->timer) || wait_->nativeFailed){fallback();return;}
        ResetEvent(wait_->wake);
        // Reset first, then recheck: a concurrent demand change must either be
        // visible in revision or leave the event signalled for the native wait.
        if(stop.stop_requested() || changed())return;
        std::stop_callback cancel(stop,[this]{SetEvent(wait_->wake);});
        HANDLE handles[]{wait_->wake,wait_->timer};
        DWORD count=1;
        if(deadline)
        {
            const auto now=StreamNowUs();if(deadline<=now)return;
            LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(deadline-now)*10;
            if(!SetWaitableTimerEx(wait_->timer,&due,0,nullptr,nullptr,nullptr,0))
            {wait_->nativeFailed=true;fallback();return;}
            count=2;
        }
        if(WaitForMultipleObjects(count,handles,FALSE,INFINITE)==WAIT_FAILED)
        {wait_->nativeFailed=true;fallback();}
        if(deadline)CancelWaitableTimer(wait_->timer);
    }

    ReceiverNetworkTraffic SampleReceiverNetworkTraffic()
    {
        static std::mutex mutex;static std::uint64_t previousUs{},previousRx{},previousTx{};
        std::lock_guard lock(mutex);MIB_IF_TABLE2* table{};if(GetIfTable2(&table)!=NO_ERROR)return {};
        std::uint64_t rx{},tx{};bool available{};
        for(ULONG i=0;i<table->NumEntries;++i)
        {const auto& row=table->Table[i];if(row.Type==IF_TYPE_ETHERNET_CSMACD && row.OperStatus==IfOperStatusUp)
            {rx+=row.InOctets;tx+=row.OutOctets;available=true;}}
        FreeMibTable(table);const auto now=StreamNowUs();ReceiverNetworkTraffic result;
        if(available && previousUs && rx>=previousRx && tx>=previousTx)
        {result={double(rx-previousRx)*8.0/double(std::max<std::uint64_t>(1,now-previousUs)),
            double(tx-previousTx)*8.0/double(std::max<std::uint64_t>(1,now-previousUs)),true};}
        previousUs=now;previousRx=rx;previousTx=tx;return result;
    }
    std::string StreamHardwareReport()
    {return StreamHardwareReport(HardwareVideoDecoder::EnumerateAdapters());}
    std::string StreamHardwareReport(const std::vector<VideoAdapterInfo>& adapters)
    {
        OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);
        using VersionFunction=LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto module=GetModuleHandleW(L"ntdll.dll");
        const auto query=module ? reinterpret_cast<VersionFunction>(GetProcAddress(module,"RtlGetVersion")) : nullptr;
        if(query)query(&version);
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);GlobalMemoryStatusEx(&memory);
        std::array<wchar_t,256> cpu{};DWORD cpuBytes=DWORD(sizeof(cpu));
        RegGetValueW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",L"ProcessorNameString",
            RRF_RT_REG_SZ,nullptr,cpu.data(),&cpuBytes);
        std::ostringstream json;
        json<<"{\"build\":"<<Quote(VANTA_VERSION_STRING " streaming-240 " __DATE__ " " __TIME__)
            <<",\"ffmpeg\":"<<Quote(av_version_info())<<",\"common_c\":\"aeba988e0a302145eff2e70fa3e11cd98ec01f5d\""
            <<",\"vcpkg\":\"a5f736224e85b32e25a8ffc52562333180396642\",\"windows\":{\"major\":"<<version.dwMajorVersion
            <<",\"minor\":"<<version.dwMinorVersion<<",\"build\":"<<version.dwBuildNumber<<"},\"cpu\":"<<Quote(Utf8(cpu.data()))
            <<",\"physical_memory_bytes\":"<<memory.ullTotalPhys<<",\"adapters\":[";
        bool first=true;
        for(const auto& adapter:adapters)
        {
            if(!std::exchange(first,false))json<<',';
            json<<"{\"id\":"<<Quote(adapter.id)<<",\"name\":"<<Quote(adapter.name)<<",\"driver\":"<<Quote(adapter.driver)
                <<",\"formats_2560x1440_mask\":"<<adapter.supportedFormats
                <<",\"h264_d3d11_1440p\":"<<((adapter.supportedFormats&VIDEO_FORMAT_H264)?"true":"false")
                <<",\"hevc_d3d11_1440p\":"<<((adapter.supportedFormats&VIDEO_FORMAT_H265)?"true":"false")
                <<",\"av1_d3d11_1440p\":"<<((adapter.supportedFormats&VIDEO_FORMAT_AV1_MAIN8)?"true":"false")
                <<",\"display_ownership\":"<<Quote(adapter.displays)<<'}';
        }
        json<<"],\"local_displays\":[";first=true;
        for(DWORD index=0;;++index)
        {
            DISPLAY_DEVICEW display{};display.cb=sizeof(display);if(!EnumDisplayDevicesW(nullptr,index,&display,0))break;
            DEVMODEW mode{};mode.dmSize=sizeof(mode);if(!EnumDisplaySettingsW(display.DeviceName,ENUM_CURRENT_SETTINGS,&mode))continue;
            if(!std::exchange(first,false))json<<',';
            json<<"{\"device\":"<<Quote(Utf8(display.DeviceName))<<",\"width\":"<<mode.dmPelsWidth<<",\"height\":"<<mode.dmPelsHeight
                <<",\"refresh_hz\":"<<mode.dmDisplayFrequency<<'}';
        }
        json<<"],\"ethernet\":[";first=true;
        ULONG bytes=16384;std::vector<unsigned char> storage(bytes);auto* addresses=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        ULONG status=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,addresses,&bytes);
        if(status==ERROR_BUFFER_OVERFLOW){storage.resize(bytes);addresses=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
            status=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,addresses,&bytes);}
        if(status==NO_ERROR)for(auto* entry=addresses;entry;entry=entry->Next)if(entry->IfType==IF_TYPE_ETHERNET_CSMACD)
        {
            if(!std::exchange(first,false))json<<',';
            json<<"{\"name\":"<<Quote(Utf8(entry->FriendlyName))<<",\"description\":"<<Quote(Utf8(entry->Description))
                <<",\"up\":"<<(entry->OperStatus==IfOperStatusUp?"true":"false")<<",\"receive_bits_per_second\":"<<entry->ReceiveLinkSpeed
                <<",\"transmit_bits_per_second\":"<<entry->TransmitLinkSpeed<<'}';
        }
        json<<"]}";return json.str();
    }
    bool RunStreamPipelineSelfTest()
    {
        const auto require=[](bool okay,const char* check){if(!okay)log::Error("Stream pipeline check failed: %s",check);return okay;};
        if(!require(FrameRequestDueUs(1000000,60)==1016667 && FrameRequestDueUs(1000000,240)==1004167 &&
            FrameRequestDueUs(1000000,0)==0,"exact capped-request deadline without fixed 20 ms polling"))return false;
        for(bool cancel:{false,true})
        {
            CaptureDemandSignal demand;
            std::promise<void> woke;auto ready=woke.get_future();
            std::jthread idle([&](std::stop_token stop){demand.WaitUntil(0,0,stop);woke.set_value();});
            if(cancel)idle.request_stop();else demand.Wake();
            if(!require(ready.wait_for(std::chrono::seconds(1))==std::future_status::ready,
                cancel?"cancel idle deadline wait":"demand change wakes idle deadline wait"))return false;
        }
        if(!require(HardwareVideoDecoder::HasHardwareDecoder(VIDEO_FORMAT_H264) && HardwareVideoDecoder::HasHardwareDecoder(VIDEO_FORMAT_H265) &&
            HardwareVideoDecoder::HasHardwareDecoder(VIDEO_FORMAT_AV1_MAIN8),"linked FFmpeg H.264/HEVC/AV1 D3D11 backends (no device created)"))return false;
        if(!require(ResolveFrameRegion({FrameRegionKind::centered,640},2560,1440)==cv::Rect(960,400,640,640),"native centered ROI"))return false;
        if(!require(ResolveFrameRegion({FrameRegionKind::centered,640},1918,1087)==cv::Rect(639,223,640,640),"unusual visible geometry"))return false;
        CapturedFrameMetadata padded;padded.sourceWidth=1918;padded.sourceHeight=1087;padded.textureWidth=1920;padded.textureHeight=1088;
        if(!require(AlignNv12Region({639,223,640,640},padded)==cv::Rect(638,222,642,642),"outward NV12 alignment retains logical crop"))return false;
        if(!require(ResolveFrameRegion({FrameRegionKind::rectangle,0,{-10,-20,41,55}},100,80)==cv::Rect(0,0,31,35),"clipped explicit ROI"))return false;
        if(!require(ResolveFrameRegion({FrameRegionKind::rectangle,0,{std::numeric_limits<int>::max(),1,100,4}},100,80).empty(),"overflow-safe ROI"))return false;
        if(!require(MergeFrameRegions({{FrameRegionKind::centered,640},{FrameRegionKind::centered,240}},2560,1440)==cv::Rect(960,400,640,640),"coalesced overlapping demand"))return false;
        CpuFrameStoragePool pool;std::array<cv::Mat,8> leases;
        for(int i=0;i<8;++i){auto* buffer=pool.Acquire();if(!require(buffer!=nullptr,"bounded pool allocation"))return false;
            buffer->create(4,4,CV_8UC4);buffer->setTo(cv::Scalar(i,i,i,255));leases[std::size_t(i)]=*buffer;}
        if(!require(pool.Acquire()==nullptr && leases[0].at<cv::Vec4b>(0,0)[0]==0,"immutable snapshot ownership"))return false;
        const auto* released=leases[3].data;leases[3].release();auto* recycled=pool.Acquire();
        if(!require(recycled && recycled->data==released,"released storage reuse"))return false;
        BoundedStreamQueue<std::unique_ptr<int>,3> packets;
        for(int i=0;i<3;++i)if(!packets.push_back(std::make_unique<int>(i)))return false;
        if(!require(!packets.push_back(std::make_unique<int>(4)) && *packets.front()==0 && packets.size()==3,"three-packet capacity and dependency order"))return false;
        packets.pop_front();if(!packets.push_back(std::make_unique<int>(4)) || *packets.front()!=1)return false;
        packets.pop_front();packets.pop_front();if(!require(*packets.front()==4,"packet ring wrap-around"))return false;
        packets.clear();if(!require(packets.empty(),"packet ring releases recovery backlog"))return false;
        StreamRecoveryState recovery;
        if(!require(recovery.Begin() && !recovery.Begin() && recovery.TakeFlush() && !recovery.TakeFlush(),"coalesced recovery flush"))return false;
        for(std::uint64_t i=0;i<4;++i)
        {if(!require(recovery.Poll(i*500000)==RecoveryAction::requestIdr && recovery.Poll(i*500000+1)==RecoveryAction::none,"500 ms IDR retry interval"))return false;}
        if(!require(recovery.Poll(2000000)==RecoveryAction::terminate,"bounded unhealthy stream recovery"))return false;
        recovery.Reset();recovery.Begin();recovery.TakeFlush();recovery.Poll(3000000);recovery.OnKeyFrame();
        if(!require(recovery.Active() && !recovery.WaitingForKeyFrame(),"IDR arrival awaits hardware output"))return false;
        recovery.OnOutput();if(!require(recovery.Poll(4000000)==RecoveryAction::none,"verified output completes recovery"))return false;
        auto state=std::make_shared<CaptureSubscriptionState>();CaptureSubscription subscription(state);
        FrameMetadataLedger ledger;CapturedFrameMetadata firstFrame,secondFrame,associated;
        firstFrame.frameNumber=11;firstFrame.generation=4;firstFrame.presentationTimeUs=101;firstFrame.timing.receivedUs=20;
        secondFrame=firstFrame;secondFrame.frameNumber=12;secondFrame.presentationTimeUs=102;secondFrame.timing.receivedUs=30;
        ledger.push_back(firstFrame);ledger.push_back(secondFrame);
        if(!require(ledger.Take(102,nullptr,associated) && associated.frameNumber==12 && associated.timing.receivedUs==30 &&
            ledger.Take(-1,&firstFrame,associated) && associated.frameNumber==11 && associated.timing.receivedUs==20 && ledger.size()==0,
            "access-unit association survives output reordering"))return false;
        state->latest={leases[0],{}};state->latest.metadata.sequence=77;state->latest.metadata.generation=9;
        state->latest.metadata.frameNumber=901;state->latest.metadata.timing.receivedUs=111;CpuFrameSnapshot snapshot;
        if(!require(subscription.Wait(76,snapshot,0) && snapshot.metadata.frameNumber==901 && snapshot.metadata.generation==9 && snapshot.metadata.timing.receivedUs==111,"atomic frame/metadata publication"))return false;
        std::promise<bool> completed;auto future=completed.get_future();
        std::jthread waiter([&](std::stop_token stop){CpuFrameSnapshot ignored;completed.set_value(subscription.Wait(77,ignored,60000,stop));});
        waiter.request_stop();
        if(!require(future.wait_for(std::chrono::seconds(1))==std::future_status::ready && !future.get(),"cancellable frame wait"))return false;
        subscription.Update({FrameRegionKind::fullSource,640,{},0,false});
        if(!require(!subscription.Wait(0,snapshot,0),"request changes invalidate cached frames"))return false;
        std::atomic_uint64_t epoch{9};state->generationProvider=[&]{return epoch.load();};state->request.active=true;
        state->latest={leases[0],{}};state->latest.metadata.sequence=88;state->latest.metadata.generation=9;epoch=10;
        if(!require(!subscription.Wait(0,snapshot,0),"old generations cannot be delivered"))return false;
        RollingStreamMetric metric;metric.AddAt(1,1000);metric.AddAt(2,1000);metric.AddAt(3,1000);metric.AddAt(4,1000);metric.AddAt(100,1000);
        const auto summary=metric.GetAt(2000);
        if(!require(summary.samples==5 && summary.medianMs==3 && std::abs(summary.p95Ms-80.8)<1e-6 &&
            std::abs(summary.p99Ms-96.16)<1e-6 && metric.GetAt(10001001).samples==0,"ten-second timing window and quantiles"))return false;
        PythonBotConfig config;std::string error;
        if(!ParsePythonBotConfig(DefaultPythonBotJson(),config,error))return false;
        cv::Mat full(1087,1918,CV_8UC4,cv::Scalar(0,0,0,255));
        const cv::Rect roi{full.cols/2-config.fovRadius,full.rows/2-config.fovRadius,2*config.fovRadius,2*config.fovRadius};
        full(cv::Rect(full.cols/2-4,full.rows/2-35,5,20)).setTo(cv::Scalar(255,0,255,255));
        const auto a=ProcessPythonBotFrame(full,config),b=ProcessPythonBotRoi(full(roi),config,roi);
        if(!require(a.valid && b.valid && a.target && a.target==b.target && a.sourceRoi==b.sourceRoi && a.movement==b.movement && cv::norm(a.fovMask,b.fovMask,cv::NORM_INF)==0,"Python crop-entry parity"))return false;
        log::Info("Stream pipeline checks passed (CPU only; no D3D device, Sunshine session or receiver benchmark)");return true;
    }
}
