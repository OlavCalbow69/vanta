#pragma once
#include "Limelight.h"
#include <opencv2/core.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>

namespace vanta
{
    inline std::uint64_t StreamNowUs()
    {
        // common-c's lazy Windows clock initialization is not itself synchronized.
        static const bool initialized=[]{(void)LiGetMicroseconds();return true;}();
        (void)initialized;return LiGetMicroseconds();
    }
    struct FrameTiming
    {
        std::uint64_t receivedUs{},assembledUs{},submittedUs{},decodedUs{},readbackStartedUs{},readbackUs{},publishedUs{};
        std::uint16_t hostProcessingTenthsMs{};
    };
    struct CapturedFrameMetadata
    {
        std::uint64_t sequence{},generation{},session{};
        std::int64_t captureTimestampNanoseconds{};
        int sourceWidth{},sourceHeight{},cropX{},cropY{},textureWidth{},textureHeight{},visibleX{},visibleY{},frameNumber{};
        int cropWidth{},cropHeight{};
        std::uint64_t presentationTimeUs{},sourcePeriodUs{4167};
        FrameTiming timing;
    };
    static_assert(std::is_trivially_copyable_v<CapturedFrameMetadata>);
    enum class FrameRegionKind { centered,rectangle,fullSource,configuredCapture };
    struct FrameRegionRequest
    {
        FrameRegionKind kind{FrameRegionKind::centered};
        int side{640};
        cv::Rect rectangle;
        int maximumFps{}; // Zero requests the stream rate.
        bool active{true};
        bool operator==(const FrameRegionRequest&) const = default;
    };
    inline cv::Rect ResolveFrameRegion(const FrameRegionRequest& request,int width,int height)
    {
        if (width<=0 || height<=0) return {};
        if (request.kind==FrameRegionKind::fullSource) return {0,0,width,height};
        if (request.kind==FrameRegionKind::centered || request.kind==FrameRegionKind::configuredCapture)
        {
            const int side=std::clamp(request.side,1,std::min(width,height));
            return {(width-side)/2,(height-side)/2,side,side};
        }
        const auto& r=request.rectangle;
        const auto x=std::clamp<std::int64_t>(r.x,0,width),y=std::clamp<std::int64_t>(r.y,0,height);
        const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),0,width);
        const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),0,height);
        return {int(x),int(y),int(std::max<std::int64_t>(0,right-x)),int(std::max<std::int64_t>(0,bottom-y))};
    }
    inline cv::Rect MergeFrameRegions(const std::vector<FrameRegionRequest>& requests,int width,int height)
    {
        cv::Rect result;
        for (const auto& request:requests) if (request.active)
        {
            const auto r=ResolveFrameRegion(request,width,height);
            if (!r.empty()) result=result.empty() ? r : result|r;
        }
        return result;
    }
    inline cv::Rect AlignNv12Region(const cv::Rect& region,const CapturedFrameMetadata& frame)
    {
        const int x=(region.x+frame.visibleX)&~1,y=(region.y+frame.visibleY)&~1;
        const int right=std::min(frame.textureWidth,(region.x+frame.visibleX+region.width+1)&~1);
        const int bottom=std::min(frame.textureHeight,(region.y+frame.visibleY+region.height+1)&~1);
        return {x,y,std::max(0,right-x),std::max(0,bottom-y)};
    }
    class FrameMetadataLedger
    {
    public:
        void push_back(const CapturedFrameMetadata& frame){pending_.push_back(frame);}
        void clear(){pending_.clear();}
        std::size_t size() const{return pending_.size();}
        bool Take(std::int64_t pts,const CapturedFrameMetadata* opaque,CapturedFrameMetadata& output)
        {
            const auto match=std::find_if(pending_.begin(),pending_.end(),[&](const auto& frame)
            {return opaque ? frame.frameNumber==opaque->frameNumber && frame.generation==opaque->generation :
                pts>=0 && frame.presentationTimeUs==std::uint64_t(pts);});
            if(match==pending_.end())return false;
            output=opaque ? *opaque : *match;pending_.erase(match);return true;
        }
    private:
        std::deque<CapturedFrameMetadata> pending_;
    };
    struct CpuFrameSnapshot { cv::Mat bgra; CapturedFrameMetadata metadata; };
    class CpuFrameStoragePool
    {
    public:
        cv::Mat* Acquire()
        {
            for(auto& candidate:storage_)if(!candidate.u || CV_XADD(&candidate.u->refcount,0)==1)return &candidate;
            if(storage_.size()==8)return nullptr;
            storage_.emplace_back();return &storage_.back();
        }
        void Clear(){storage_.clear();}
    private:
        std::vector<cv::Mat> storage_;
    };
    struct FrameReadbackRequest { std::uint64_t afterSequence{}; std::vector<FrameRegionRequest> regions; std::uint64_t revision{}; };
    template<class T,std::size_t Capacity> class BoundedStreamQueue
    {
    public:
        bool push_back(T&& value){if(size_==Capacity)return false;values_[(head_+size_)%Capacity]=std::move(value);++size_;return true;}
        T& front(){return values_[head_];}
        void pop_front(){values_[head_]=T{};head_=(head_+1)%Capacity;--size_;}
        void clear(){while(size_)pop_front();head_=0;}
        bool empty() const{return size_==0;}
        std::size_t size() const{return size_;}
    private:
        static_assert(Capacity>0);
        std::array<T,Capacity> values_{};
        std::size_t head_{},size_{};
    };
    enum class RecoveryAction { none,requestIdr,terminate };
    class StreamRecoveryState
    {
    public:
        bool Begin(){flush_=waitingForKeyFrame_=true;if(active_)return false;active_=true;attempts_=0;nextRequest_=0;return true;}
        bool Active() const{return active_;}
        bool WaitingForKeyFrame() const{return active_ && waitingForKeyFrame_;}
        bool NeedsFlush() const{return flush_;}
        bool TakeFlush(){return std::exchange(flush_,false);}
        void OnKeyFrame(){waitingForKeyFrame_=false;}
        void OnOutput(){if(active_ && !waitingForKeyFrame_ && !flush_)active_=false;}
        void Reset(){active_=flush_=waitingForKeyFrame_=false;attempts_=0;nextRequest_=0;}
        RecoveryAction Poll(std::uint64_t now)
        {
            if(!active_ || now<nextRequest_)return RecoveryAction::none;
            if(attempts_==4)return RecoveryAction::terminate;
            ++attempts_;nextRequest_=now+500000;return RecoveryAction::requestIdr;
        }
    private:
        bool active_{},flush_{},waitingForKeyFrame_{};
        unsigned attempts_{};
        std::uint64_t nextRequest_{};
    };
    inline std::uint64_t FrameRequestDueUs(std::uint64_t previousUs,int maximumFps)
    {return maximumFps>0 ? previousUs+(1000000ULL+unsigned(maximumFps)-1)/unsigned(maximumFps) : 0;}
    struct CaptureDemandSignal
    {
        CaptureDemandSignal();
        ~CaptureDemandSignal();
        std::mutex mutex;
        std::condition_variable_any condition;
        std::atomic_uint64_t revision{0};
        void Wake();
        // One capture-worker waiter. Zero deadline means wait for demand or
        // cancellation; finite deadlines use the local monotonic clock.
        void WaitUntil(std::uint64_t expectedRevision,std::uint64_t deadlineUs,std::stop_token stop);
        const char* WaitMode() const;
    private:
        struct WaitState;
        std::unique_ptr<WaitState> wait_;
    };
    struct CaptureSubscriptionState
    {
        std::string consumerName;
        mutable std::mutex mutex;
        std::condition_variable_any condition;
        FrameRegionRequest request;
        CpuFrameSnapshot latest;
        bool cancelled{};
        std::uint64_t lastPublishedUs{};
        std::shared_ptr<CaptureDemandSignal> demand;
        std::function<std::uint64_t()> generationProvider;
    };
    class CaptureSubscription
    {
    public:
        CaptureSubscription()=default;
        explicit CaptureSubscription(std::shared_ptr<CaptureSubscriptionState> state):state_(std::move(state)){}
        void Update(const FrameRegionRequest& request) const
        {
            if (!state_) return;
            std::lock_guard lock(state_->mutex);
            if (state_->request!=request)
            {
                state_->request=request; state_->latest={}; state_->lastPublishedUs=0;
                state_->condition.notify_all();
                if(state_->demand)state_->demand->Wake();
            }
        }
        bool Wait(std::uint64_t after,CpuFrameSnapshot& output,std::uint32_t timeoutMs,std::stop_token stop={}) const
        {
            if (!state_) return false;
            std::unique_lock lock(state_->mutex);
            state_->condition.wait_for(lock,stop,std::chrono::milliseconds(timeoutMs),[&]
            {return state_->cancelled || (state_->request.active && !state_->latest.bgra.empty() && state_->latest.metadata.sequence>after &&
                (!state_->generationProvider || state_->latest.metadata.generation==state_->generationProvider()));});
            if (stop.stop_requested() || state_->cancelled || !state_->request.active || state_->latest.bgra.empty() || state_->latest.metadata.sequence<=after) return false;
            if(state_->generationProvider && state_->latest.metadata.generation!=state_->generationProvider())return false;
            output=state_->latest; return true;
        }
        explicit operator bool() const { return bool(state_); }
    private:
        std::shared_ptr<CaptureSubscriptionState> state_;
    };
    struct TimingSummary { double medianMs{},p95Ms{},p99Ms{}; std::size_t samples{}; };
    class RollingStreamMetric
    {
    public:
        void Clear(){std::lock_guard lock(mutex_);values_.clear();}
        void Add(double milliseconds)
        {AddAt(milliseconds,StreamNowUs());}
        void AddAt(double milliseconds,std::uint64_t now)
        {
            std::lock_guard lock(mutex_);
            values_.push_back({now,milliseconds}); Prune(now);
        }
        TimingSummary Get() const
        {return GetAt(StreamNowUs());}
        TimingSummary GetAt(std::uint64_t now) const
        {
            std::vector<double> sorted;
            {std::lock_guard lock(mutex_);Prune(now);sorted.reserve(values_.size());
                for (const auto& value:values_) sorted.push_back(value.second);}
            if (sorted.empty()) return {};
            std::sort(sorted.begin(),sorted.end());
            const auto at=[&](double q){const double index=q*(sorted.size()-1);const auto a=std::size_t(index),b=std::min(sorted.size()-1,a+1);
                return sorted[a]+(sorted[b]-sorted[a])*(index-double(a));};
            return {at(.5),at(.95),at(.99),sorted.size()};
        }
    private:
        void Prune(std::uint64_t now) const
        {while (!values_.empty() && (now-values_.front().first>10000000 || values_.size()>16384)) values_.pop_front();}
        mutable std::mutex mutex_;
        mutable std::deque<std::pair<std::uint64_t,double>> values_;
    };
    bool RunStreamPipelineSelfTest();
    std::string StreamHardwareReport();
    struct VideoAdapterInfo;
    std::string StreamHardwareReport(const std::vector<VideoAdapterInfo>& adapters);
    struct ReceiverNetworkTraffic { double receiveMbps{},transmitMbps{};bool available{}; };
    ReceiverNetworkTraffic SampleReceiverNetworkTraffic();
}
