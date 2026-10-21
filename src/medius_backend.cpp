#include "medius_backend.hpp"
#include "input_backend_utils.hpp"
#include "stream_frame.hpp"
#include "logger.hpp"
#include "medius.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

namespace vanta
{
    namespace
    {
        constexpr auto kOk=MEDIUS_STATUS_OK;
        MediusLockTarget Target(std::uint32_t bit)
        {
            if(bit==InputLockX)return medius_lock_target_axis(MEDIUS_LOCK_TARGET_KIND_X);
            if(bit==InputLockY)return medius_lock_target_axis(MEDIUS_LOCK_TARGET_KIND_Y);
            const auto button=bit==InputLockLeft?MEDIUS_BUTTON_LEFT:bit==InputLockSide1?MEDIUS_BUTTON_SIDE1:MEDIUS_BUTTON_SIDE2;
            return medius_lock_target_usage(medius_usage_button(button));
        }
        std::uint32_t QueryBit(InputQuery query)
        {
            switch(query){case InputQuery::lockX:return InputLockX;case InputQuery::lockY:return InputLockY;
            case InputQuery::lockLeft:return InputLockLeft;case InputQuery::lockSide1:return InputLockSide1;
            case InputQuery::lockSide2:return InputLockSide2;default:return 0;}
        }
        std::string LastMediusError(){std::array<char,1024> text{};medius_last_error_message(text.data(),text.size());return text.data();}
        bool Firmware346(const MediusChipFirmware& f){return f.major==3 && f.minor==4 && f.patch==6;}
        std::string FirmwareText(const MediusChipFirmware& f)
        {return std::to_string(f.major)+'.'+std::to_string(f.minor)+'.'+std::to_string(f.patch);}
        bool DecodeMasks(const MediusLocks& locks,const MediusBearing& bearing,InputMaskReadback& snapshot)
        {
            // A saturated bounded C-API table cannot establish that omitted
            // managed entries are absent, so reject it conservatively too.
            if(locks.n>=MEDIUS_MAX_LOCKS)return false;
            snapshot={};snapshot.knownMask=InputManagedLocks;
            snapshot.bearingKnown=true;snapshot.bearingWindowMs=bearing.window_ms;snapshot.bearingMode=static_cast<std::uint8_t>(bearing.mode);
            for(std::size_t i=0;i<kManagedInputBits.size();++i)
            {
                const auto target=Target(kManagedInputBits[i]);
                std::array<std::int16_t,4> scales{100,100,100,100};std::array<bool,4> seen{};
                for(std::size_t e=0;e<locks.n;++e)
                {
                    const auto& entry=locks.entries[e];
                    if(entry.target.kind!=target.kind)continue;
                    if(target.kind==MEDIUS_LOCK_TARGET_KIND_USAGE && (entry.target.usage.kind!=target.usage.kind ||
                        (!entry.is_blanket && entry.target.usage.id!=target.usage.id)))continue;
                    if(entry.direction>MEDIUS_DIRECTION_AGAINST || entry.scale<-255 || entry.scale>255 ||
                        (entry.is_blanket && target.kind!=MEDIUS_LOCK_TARGET_KIND_USAGE))return false;
                    // BOTH describes the fixed pair only. The SDK's generic
                    // helper treats it as covering relative slots too, which
                    // hides drift in WITH/AGAINST when the fixed pair is blocked.
                    for(std::size_t slot=0;slot<4;++slot)
                    {
                        if(entry.direction!=slot+1 && !(entry.direction==MEDIUS_DIRECTION_BOTH && slot<2))continue;
                        if(slot>=2 && target.kind==MEDIUS_LOCK_TARGET_KIND_USAGE)return false;
                        if(!seen[slot] || std::abs(int(entry.scale))<std::abs(int(scales[slot])) ||
                            (std::abs(int(entry.scale))==std::abs(int(scales[slot])) && entry.scale<scales[slot]))scales[slot]=entry.scale;
                        seen[slot]=true;
                    }
                }
                snapshot.positiveScale[i]=scales[0];snapshot.negativeScale[i]=scales[1];
                snapshot.withScale[i]=scales[2];snapshot.againstScale[i]=scales[3];
                if(scales[0]==0 && scales[1]==0)snapshot.blockedMask|=kManagedInputBits[i];
                if(std::all_of(scales.begin(),scales.end(),[](int scale){return scale==100;}))snapshot.passthroughMask|=kManagedInputBits[i];
            }
            return true;
        }
        template<class Write,class Read>
        bool RestoreOptions(const MediusRecoverySettings& saved,Write write,Read read)
        {
            if(!saved.valid || !saved.bearingValid || !write(saved))return false;
            MediusRecoverySettings actual;
            return read(actual) && actual.renderMode==saved.renderMode && actual.renderFull==saved.renderFull &&
                actual.spreadPercent==saved.spreadPercent && actual.rideEnabled==saved.rideEnabled && actual.rideWindowMs==saved.rideWindowMs &&
                actual.bearingWindowMs==saved.bearingWindowMs && actual.bearingMode==saved.bearingMode;
        }
        bool RestoreOptions(MediusDevice* device,const MediusRecoverySettings& saved)
        {
            return RestoreOptions(saved,[device](const MediusRecoverySettings& value)
            {
                return medius_device_set_render(device,value.renderMode,value.renderFull!=0)==kOk &&
                    medius_device_set_spread(device,value.spreadPercent)==kOk &&
                    medius_device_set_movement_riding(device,value.rideEnabled,value.rideWindowMs)==kOk &&
                    medius_device_set_bearing(device,value.bearingWindowMs,value.bearingMode)==kOk;
            },[device](MediusRecoverySettings& value)
            {
                MediusRenderStatus render{};MediusSpreadStatus spread{};MediusBearing bearing{};bool ride{};std::uint32_t window{};
                if(medius_device_query_render(device,&render)!=kOk || medius_device_query_spread(device,&spread)!=kOk ||
                    medius_device_query_movement_riding(device,&ride,&window)!=kOk || medius_device_query_bearing(device,&bearing)!=kOk)return false;
                value.renderMode=static_cast<std::uint8_t>(render.mode);value.renderFull=render.full;value.spreadPercent=spread.percent;
                value.rideEnabled=ride;value.rideWindowMs=window;value.bearingWindowMs=bearing.window_ms;value.bearingMode=static_cast<std::uint8_t>(bearing.mode);
                return true;
            });
        }
        bool HeldLeft(const MediusClipStatus& status)
        {return medius_clip_status_is_held(&status,medius_usage_button(MEDIUS_BUTTON_LEFT));}
        bool ClickIdle(const MediusClipStatus& status)
        {return status.state==MEDIUS_CLIP_STATE_IDLE && status.total==0 && !HeldLeft(status);}
        bool ClickEdgesPlayed(const MediusClipStatus& status,std::uint32_t baselineTicks)
        {
            // Firmware ticks count content entries, not the gap's elapsed frames.
            // The counter is cumulative since boot and wraps at 32 bits.
            return static_cast<std::uint32_t>(status.ticks-baselineTicks)==2 && status.total==0 && !HeldLeft(status);
        }
        bool ClickSettingsMatch(const MediusClipSettings& settings)
        {return settings.finalized && !settings.loop_ && !settings.retain && !settings.ride &&
            !settings.autolock_bits && !settings.n && !settings.packet_n;}
        std::string ClickReadbackText(const MediusClipStatus& status,std::uint32_t baselineTicks)
        {
            std::ostringstream text;text<<"state="<<unsigned(status.state)<<", buffered_bytes="<<status.total
                <<", content_edges="<<static_cast<std::uint32_t>(status.ticks-baselineTicks)
                <<", left_held="<<(HeldLeft(status)?"yes":"no");return text.str();
        }
        bool Current(const OutputContext& c,std::uint64_t now)
        {
            if(c.frameTimeUs && (now<c.frameTimeUs || now-c.frameTimeUs>100000))return false;
            try{return !c.isCurrent || c.isCurrent();}catch(...){return false;}
        }
        bool SameCorrection(const OutputContext& current,const OutputContext& ended)
        {
            return current.owner==ended.owner && current.generation==ended.generation &&
                current.streamGeneration==ended.streamGeneration && current.streamSession==ended.streamSession && current.sequence<=ended.sequence;
        }
        bool NativeDelta(int dx,int dy,const OutputContext& c)
        {
            return dx>=-32768 && dx<=32767 && dy>=-32768 && dy<=32767 &&
                ((c.axes&InputLockX) || !dx) && ((c.axes&InputLockY) || !dy);
        }
        bool NewClipFault(const MediusClipStatus& current,unsigned overruns,unsigned gaps,bool lost)
        {return current.state==MEDIUS_CLIP_STATE_FAULTED || current.overruns!=overruns || current.seq_gaps!=gaps || lost;}
        bool BuildClick(MediusClipBuilder* builder,int hold)
        {
            // Each edge occupies one native tick. The release is hold ticks after
            // the press. FINALIZE makes this a finite click, with no refill clock.
            return medius_clip_builder_clear(builder)==kOk &&
                medius_clip_builder_press(builder,medius_usage_button(MEDIUS_BUTTON_LEFT))==kOk &&
                medius_clip_builder_gap(builder,static_cast<std::uint16_t>(std::clamp(hold,14,34)-1))==kOk &&
                medius_clip_builder_release(builder,medius_usage_button(MEDIUS_BUTTON_LEFT))==kOk;
        }
    }
    struct MediusBackend::Implementation
    {
        // Serial operations take io before state. Callbacks never run under state.
        mutable std::mutex state,io;std::condition_variable_any changed;
        MediusDevice* device{};MediusClip* clip{};MediusClipBuilder* builder{};MediusEventStream* events{};
        std::jthread maintenance,buttonReader;std::atomic_bool healthy{false},armed{false},connectCancelled{false};
        bool optionsOwned{},fusePrepared{},moving{},motionPending{},clickActive{},clickCompleted{},clickAborted{};
        OutputContext context;
        MediusRecoverySettings saved;std::function<void(std::uint8_t)> buttons;
        std::string identity,status="Medius disconnected",deviceFirmware="unknown",hostFirmware="unknown",lastFailure;
        std::uint64_t lastFailureUs{};unsigned protocol{},nativePeriodUs{},pollPeriodUs{};bool rateConfirmed{};
        MediusCaps capabilities{};MediusStats baseline{};std::uint16_t session{};
        unsigned baselineOverruns{},baselineGaps{};
        std::uint32_t clickBaselineTicks{};
        MediusClipStatus lastClickReadback{};
        std::uint64_t lastClickReadbackUs{};
        std::atomic_uint64_t drainedClickStops{};
        std::atomic_uint64_t moves{},clicks{},completedClicks{},cancelledClicks{},cancellations{},faults{},inputDrops{};
        RollingStreamMetric serialWrite,healthQueries,clipQueries;
        std::deque<std::uint64_t> moveTimes;
        void Fail(const std::string& why)
        {
            const bool wasHealthy=healthy.exchange(false);armed=false;
            {std::lock_guard guard(state);status=lastFailure=why;lastFailureUs=StreamNowUs();}
            if(wasHealthy){++faults;log::Warning("Medius native: %s",why.c_str());}
            changed.notify_all();
        }
        bool Good(MediusStatus result,const char* operation)
        {if(result==kOk)return true;Fail(std::string(operation)+": "+LastMediusError()+"; reconnect required before more queries");return false;}
        void ObserveClick(const MediusClipStatus& value)
        {
            std::lock_guard guard(state);lastClickReadback=value;lastClickReadbackUs=StreamNowUs();
            if(clickActive && ClickIdle(value) && ClickEdgesPlayed(value,clickBaselineTicks))
            {clickActive=false;clickCompleted=!clickAborted;if(clickCompleted)++completedClicks;}
        }
        bool SnapshotLocked(MediusClipStatus& clipState,MediusHealth& health)
        {
            if(!healthy || !clip)return false;
            MediusStats stats{};const auto start=StreamNowUs();
            const auto result=medius_clip_query_output_status(clip,&clipState,&stats,&health);
            healthQueries.Add(double(StreamNowUs()-start)/1000.0);
            if(!Good(result,"Output health query"))return false;
            if(stats.session!=session || !health.link_up || !health.mouse_attached || !health.clone_configured ||
                stats.tx_drops!=baseline.tx_drops || stats.relay_drops!=baseline.relay_drops)
            {Fail("Medius session changed or output delivery became uncertain; reconnect required");return false;}
            if(NewClipFault(clipState,baselineOverruns,baselineGaps,medius_clip_lost(clip)))
            {Fail("Medius finite click clip fault; pending input invalidated");return false;}
            {std::lock_guard guard(state);motionPending=health.injection_active!=0;}
            ObserveClick(clipState);
            return true;
        }
        bool ClipStatusLocked(MediusClipStatus& result)
        {
            if(!healthy || !clip)return false;
            const auto start=StreamNowUs();const auto queried=medius_clip_query_status(clip,&result);
            clipQueries.Add(double(StreamNowUs()-start)/1000.0);
            if(!Good(queried,"Generated-click release query"))return false;
            if(NewClipFault(result,baselineOverruns,baselineGaps,medius_clip_lost(clip)))
            {Fail("Medius finite click playback failed");return false;}
            ObserveClick(result);
            return true;
        }
        bool ClearClickLocked()
        {
            if(!clip)return true;
            {std::lock_guard guard(state);if(clickActive)++cancelledClicks;
                clickAborted=true;clickCompleted=false;clickActive=false;}
            const bool sent=medius_clip_clear(clip)==kOk;
            const bool released=medius_device_soft_release(device,medius_usage_button(MEDIUS_BUTTON_LEFT))==kOk;
            if(!healthy)return false; // A poisoned serial session permits writes only.
            if(!sent || !released){Fail("Generated-click cancellation write failed");return false;}
            MediusClipStatus check{};
            if(!Good(medius_clip_query_status(clip,&check),"Generated-click cancellation readback"))return false;
            if(!ClickIdle(check)){Fail("Generated click remained active after cancellation");return false;}
            baselineOverruns=check.overruns;baselineGaps=check.seq_gaps;
            {std::lock_guard guard(state);clickBaselineTicks=check.ticks;lastClickReadback=check;lastClickReadbackUs=StreamNowUs();}
            return true;
        }
        void Watch(std::stop_token stop)
        {
            while(!stop.stop_requested() && healthy && armed)
            {
                {std::unique_lock guard(state);changed.wait_for(guard,stop,std::chrono::milliseconds(100),[&]{return !healthy || !armed;});}
                if(stop.stop_requested() || !healthy || !armed)break;
                std::lock_guard transaction(io);MediusClipStatus clipState{};MediusHealth health{};
                if(!SnapshotLocked(clipState,health))break;
            }
        }
        void ReadButtons(std::stop_token stop)
        {
            while(!stop.stop_requested())
            {
                MediusCatchEvent event{};
                if(!medius_event_stream_recv_timeout(events,20,&event))
                {if(!medius_event_stream_is_connected(events) && !stop.stop_requested()){Fail("Physical-button event stream disconnected");break;}continue;}
                if(event.kind!=MEDIUS_CATCH_EVENT_KIND_USAGES)continue;
                const auto& usages=event.data.usages;if(usages.class_!=MEDIUS_CLASS_BUTTON)continue;
                std::uint8_t mask{};for(unsigned i=0;i<usages.n;++i)
                    if(usages.usages[i].id<5)mask|=static_cast<std::uint8_t>(1U<<usages.usages[i].id);
                if(buttons)buttons(mask);
                const auto dropped=medius_event_stream_dropped(events);
                if(dropped!=inputDrops.exchange(dropped)){Fail("Physical input monitoring lost events; automation cancelled");break;}
            }
        }
    };
    MediusBackend::MediusBackend():state_(std::make_unique<Implementation>()){}
    MediusBackend::~MediusBackend(){Close();}
    void MediusBackend::PrepareConnection(){state_->connectCancelled=false;}
    void MediusBackend::CancelConnection(){state_->connectCancelled=true;state_->changed.notify_all();}
    std::vector<std::string> MediusBackend::Ports()
    {
        std::array<MediusPortInfo,32> ports{};std::uintptr_t total{};std::vector<std::string> out;
        const auto n=medius_find_ports(ports.data(),ports.size(),&total);
        for(std::uintptr_t i=0;i<std::min<std::uintptr_t>(n,ports.size());++i)out.emplace_back(ports[i].path);return out;
    }
    bool MediusBackend::Open(const std::string& port,const MouseOutputConfig& /*settings*/,std::function<void(std::uint8_t)> buttons,std::string& error)
    {
        Close();auto& s=*state_;s.inputDrops=0;
        if(s.connectCancelled){error="Medius connection cancelled";return false;}
        {std::lock_guard guard(s.state);s.buttons=std::move(buttons);s.deviceFirmware=s.hostFirmware="unknown";
            s.protocol=s.nativePeriodUs=s.pollPeriodUs=0;s.rateConfirmed=false;s.capabilities={};
            s.context={};s.moving=s.motionPending=s.clickActive=s.clickCompleted=s.clickAborted=false;
            s.status="Checking Medius capabilities";}
        if(medius_device_open(port.c_str(),&s.device)!=kOk){error=LastMediusError();return false;}
        if(medius_device_set_manual_recovery(s.device,true,20)!=kOk){error="Medius manual session control unavailable";Close();return false;}
        MediusVersion version{};MediusFirmwareInfo firmware{};MediusRate rate{};MediusHealth health{};MediusCaps caps{};
        if(medius_device_query_version(s.device,&version)!=kOk || medius_device_firmware_info(s.device,&firmware)!=kOk ||
            medius_device_query_rate(s.device,&rate)!=kOk || medius_device_query_health(s.device,&health)!=kOk || medius_device_caps(s.device,&caps)!=kOk)
        {error="Medius capability query failed: "+LastMediusError();Close();return false;}
        {std::lock_guard guard(s.state);s.deviceFirmware=FirmwareText(firmware.device);s.hostFirmware=firmware.host_present?FirmwareText(firmware.host):"not present";
            s.protocol=version.proto_ver;s.nativePeriodUs=rate.native_period_us;s.pollPeriodUs=rate.poll_period_us;s.rateConfirmed=rate.confident;s.capabilities=caps;}
        if(version.proto_ver!=9){error="Medius protocol 9 required; detected "+std::to_string(version.proto_ver);Close();return false;}
        if(!Firmware346(firmware.device) || !firmware.host_present || !Firmware346(firmware.host))
        {error="Medius 3.4.6 required on both chips; detected device "+s.deviceFirmware+", host "+s.hostFirmware+". Install manually using the official dashboard";Close();return false;}
        if(!health.link_up || !health.mouse_attached || !health.clone_configured || !caps.mouse.has_x || !caps.mouse.has_y || !caps.mouse.n_buttons)
        {error="Medius mouse/clone output is not ready; check cloning and mouse compatibility in the official dashboard";Close();return false;}
        if(!rate.confident || rate.native_period_us!=1000 || rate.poll_period_us!=1000)
        {error="Medius requires a confirmed native 1000 Hz mouse and 1000 Hz clone poll rate; Vanta does not force a rate change";Close();return false;}
        MediusRenderStatus render{};MediusSpreadStatus spread{};bool ride{};std::uint32_t window{};MediusBearing bearing{};
        if(medius_device_query_render(s.device,&render)!=kOk || medius_device_query_spread(s.device,&spread)!=kOk ||
            medius_device_query_movement_riding(s.device,&ride,&window)!=kOk || medius_device_query_stats(s.device,&s.baseline)!=kOk ||
            medius_device_query_bearing(s.device,&bearing)!=kOk || bearing.mode>MEDIUS_BEARING_MODE_VECTOR)
        {error="Medius initial settings could not be captured";Close();return false;}
        s.session=s.baseline.session;MediusRecoverySettings captured;std::copy(std::begin(version.mac),std::end(version.mac),captured.mac);
        captured.renderMode=static_cast<std::uint8_t>(render.mode);captured.renderFull=render.full;captured.spreadPercent=spread.percent;
        captured.rideEnabled=ride;captured.rideWindowMs=window;captured.valid=true;
        captured.bearingWindowMs=bearing.window_ms;captured.bearingMode=static_cast<std::uint8_t>(bearing.mode);captured.bearingValid=true;
        {std::lock_guard guard(s.state);s.saved=captured;}s.fusePrepared=false;
        std::ostringstream identity;identity<<"Medius device 3.4.6 / host 3.4.6 / protocol "<<unsigned(version.proto_ver)<<" / MAC ";
        for(auto c:version.mac)identity<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);
        {std::lock_guard guard(s.state);s.identity=identity.str();}
        if(s.connectCancelled){error="Medius connection cancelled";Close();return false;}
        s.healthy=true;{std::lock_guard guard(s.state);s.status="Medius native output verified; waiting for recovery helper";}return true;
    }
    bool MediusBackend::Arm(std::string& error)
    {
        auto& s=*state_;if(!s.device || !s.healthy || s.connectCancelled){error="Medius connection is unavailable or cancelled";s.Fail(error);return false;}
        std::lock_guard transaction(s.io);
        MediusStats baseline{};MediusRate rate{};MediusHealth health{};
        if(medius_device_query_stats(s.device,&baseline)!=kOk || medius_device_query_rate(s.device,&rate)!=kOk ||
            medius_device_query_health(s.device,&health)!=kOk || !rate.confident || rate.native_period_us!=1000 || rate.poll_period_us!=1000 ||
            !health.link_up || !health.mouse_attached || !health.clone_configured)
        {error="Medius state changed while preparing recovery; reconnect required";s.Fail(error);return false;}
        s.baseline=baseline;s.session=baseline.session;s.optionsOwned=true;
        if(medius_device_set_render(s.device,0,false)!=kOk || medius_device_set_spread(s.device,0)!=kOk ||
            medius_device_set_movement_riding(s.device,false,0)!=kOk || medius_device_clip(s.device,&s.clip)!=kOk)
        {error="Medius native output preparation failed: "+LastMediusError();s.Fail(error);return false;}
        MediusRenderStatus render{};MediusSpreadStatus spread{};bool ride{};std::uint32_t window{};
        if(medius_device_query_render(s.device,&render)!=kOk || render.mode!=0 || render.full ||
            medius_device_query_spread(s.device,&spread)!=kOk || spread.percent ||
            medius_device_query_movement_riding(s.device,&ride,&window)!=kOk || ride)
        {error="Medius native output settings did not verify";s.Fail(error);return false;}
        if(medius_clip_set_loop(s.clip,0)!=kOk || medius_clip_set_retain(s.clip,0)!=kOk || medius_clip_set_ride(s.clip,0)!=kOk ||
            medius_clip_set_autolock(s.clip,nullptr,0)!=kOk || medius_clip_clear_triggers(s.clip)!=kOk)
        {error="Medius finite click configuration failed";s.Fail(error);return false;}
        MediusClipSettings configuration{};
        if(medius_clip_query_config(s.clip,&configuration)!=kOk || configuration.autolock_bits || configuration.loop_ || configuration.retain || configuration.ride || configuration.n || configuration.packet_n)
        {error="Medius finite click configuration readback disagrees";s.Fail(error);return false;}
        const auto filter=medius_catch_filter_watch_class(MEDIUS_CLASS_BUTTON);
        if(medius_device_catch_events(s.device,&filter,1,&s.events)!=kOk){error="Medius physical-button monitoring unavailable";s.Fail(error);return false;}
        s.builder=medius_clip_builder_new();
        if(!s.builder){error="Medius click storage allocation failed";s.Fail(error);return false;}
        if(!s.Good(medius_device_discard_motion(s.device),"Initial pending motion discard") || !s.ClearClickLocked())
        {error="Medius initial input neutralization failed";return false;}
        s.armed=true;{std::lock_guard guard(s.state);s.status=s.identity+"; native MOVE; finite 14-34 ms clicks; 1000 Hz mouse";}
        s.maintenance=std::jthread([&s](std::stop_token stop){s.Watch(stop);});
        s.buttonReader=std::jthread([&s](std::stop_token stop){s.ReadButtons(stop);});return true;
    }
    bool MediusBackend::Close()
    {
        auto& s=*state_;s.armed=false;s.maintenance.request_stop();s.buttonReader.request_stop();s.changed.notify_all();
        if(s.events)medius_event_stream_close(s.events);
        if(s.maintenance.joinable())s.maintenance.join();if(s.buttonReader.joinable())s.buttonReader.join();
        std::lock_guard transaction(s.io);bool clean=true;
        if(s.device && s.optionsOwned)
        {
            const bool discarded=medius_device_discard_motion(s.device)==kOk;
            clean=s.ClearClickLocked() && discarded;
            if(s.healthy && clean)
            {
                MediusHealth health{};
                clean=s.Good(medius_device_query_health(s.device,&health),"Final input cleanup readback") && !health.injection_active;
            }
            if(s.healthy && clean)clean=RestoreOptions(s.device,s.saved);
            else {medius_device_set_render(s.device,s.saved.renderMode,s.saved.renderFull!=0);medius_device_set_spread(s.device,s.saved.spreadPercent);
                medius_device_set_movement_riding(s.device,s.saved.rideEnabled,s.saved.rideWindowMs);
                if(s.saved.bearingValid)medius_device_set_bearing(s.device,s.saved.bearingWindowMs,s.saved.bearingMode);clean=false;}
        }
        if(s.events){medius_event_stream_free(s.events);s.events=nullptr;}
        if(s.builder){medius_clip_builder_free(s.builder);s.builder=nullptr;}
        if(s.clip){medius_clip_free(s.clip);s.clip=nullptr;}
        if(s.device){medius_device_free(s.device);s.device=nullptr;}
        s.healthy=false;s.optionsOwned=s.fusePrepared=false;{std::lock_guard guard(s.state);s.saved={};}return clean;
    }
    bool MediusBackend::Lock(std::uint32_t bit,bool enabled)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);if(!s.device)return false;
        const auto result=enabled?medius_device_lock(s.device,Target(bit),MEDIUS_DIRECTION_BOTH):medius_device_unlock(s.device,Target(bit),MEDIUS_DIRECTION_BOTH);
        return s.Good(result,"Physical input mask write");
    }
    bool MediusBackend::ReadMasks(InputMaskReadback& result)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);if(!s.device || !s.healthy)return false;
        MediusLocks locks{};MediusBearing bearing{};
        if(!s.Good(medius_device_query_locks(s.device,&locks),"Managed physical-input mask query"))return false;
        if(!s.Good(medius_device_query_bearing(s.device,&bearing),"Bearing configuration query"))return false;
        InputMaskReadback snapshot;
        if(!DecodeMasks(locks,bearing,snapshot)){s.Fail("Invalid firmware directional mask table");return false;}
        snapshot.readAtUs=StreamNowUs();
        result=snapshot;return true;
    }
    bool MediusBackend::SetAxisPolicy(std::uint32_t bit,const DirectionalInputScales& policy)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        if(!s.device || !s.healthy || (bit!=InputLockX && bit!=InputLockY))return false;
        if(!s.Good(medius_device_scale(s.device,Target(bit),MEDIUS_DIRECTION_BOTH,policy.positive),"Axis fixed scales"))return false;
        if(policy.negative!=policy.positive && !s.Good(medius_device_scale(s.device,Target(bit),MEDIUS_DIRECTION_NEGATIVE,policy.negative),"Axis negative scale"))return false;
        if(policy.with!=100 && !s.Good(medius_device_scale(s.device,Target(bit),MEDIUS_DIRECTION_WITH,policy.with),"Axis with-bearing scale"))return false;
        return policy.against==100 || s.Good(medius_device_scale(s.device,Target(bit),MEDIUS_DIRECTION_AGAINST,policy.against),"Axis against-bearing scale");
    }
    bool MediusBackend::PrepareFuse()
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        if(!s.device || !s.healthy || !s.armed || !s.saved.bearingValid)return false;
        if(s.fusePrepared)return true;
        if(!s.Good(medius_device_set_bearing(s.device,kFuseBearingWindowMs,MEDIUS_BEARING_MODE_PER_AXIS),"Fuse bearing setup"))return false;
        // The coordinator verifies the actual option together with all four
        // scale slots before permitting the first correction.
        s.fusePrepared=true;return true;
    }
    bool MediusBackend::ClearBearing()
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        return s.device && s.healthy && s.Good(medius_device_discard_motion(s.device),"Clear drained movement bearing");
    }
    std::string MediusBackend::FuseUnavailableReason() const
    {
        const auto& s=*state_;std::lock_guard guard(s.state);
        if(!s.healthy || !s.armed)return "Connect and verify the Medius backend before enabling Fuse inputs";
        if(!s.saved.bearingValid)return "Medius bearing configuration readback is unavailable";
        if(!s.rateConfirmed || s.nativePeriodUs!=1000 || s.pollPeriodUs!=1000)return "Fuse inputs requires confirmed native and clone 1000 Hz rates";
        return {};
    }
    void MediusBackend::InvalidateOutput(const std::string& reason)
    {if(state_->healthy)state_->Fail("Input mask coordinator: "+reason);}
    bool MediusBackend::Query(InputQuery query,int& value)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);if(!s.device || !s.clip || !s.healthy)return false;
        if(const auto bit=QueryBit(query))
        {
            MediusLocks locks{};if(!s.Good(medius_device_query_locks(s.device,&locks),"Physical input mask query"))return false;
            const auto positive=medius_locks_scale_of(&locks,Target(bit),MEDIUS_DIRECTION_POSITIVE);
            const auto negative=medius_locks_scale_of(&locks,Target(bit),MEDIUS_DIRECTION_NEGATIVE);
            if(positive!=negative || (positive!=0 && positive!=100)){s.Fail("Physical input mask directions disagree");return false;}
            value=positive==0?1:0;return true;
        }
        if(query==InputQuery::moving)
        {
            MediusClipStatus clip{};MediusHealth health{};if(!s.SnapshotLocked(clip,health))return false;
            // HEALTH has no axis-specific pending counter. A valid independent
            // click may conservatively delay unlock; it is never cancelled here.
            value=health.injection_active?1:0;return true;
        }
        MediusClipStatus clip{};if(!s.ClipStatusLocked(clip))return false;
        value=ClickIdle(clip)?0:2;return true;
    }
    bool MediusBackend::Move(int dx,int dy,const OutputContext& c)
    {
        auto& s=*state_;if(!NativeDelta(dx,dy,c) || (!dx && !dy))return false;
        std::lock_guard transaction(s.io);if(!s.healthy || !s.armed || !Current(c,StreamNowUs()))return false;
        const auto start=StreamNowUs();
        // Ordinary MOVE goes into the discardable accumulator. Riding, render
        // and spread are disabled and verified, so still mice can move too.
        const auto result=medius_device_move_rel(s.device,static_cast<std::int16_t>(dx),static_cast<std::int16_t>(dy));
        s.serialWrite.Add(double(StreamNowUs()-start)/1000.0);
        if(!s.Good(result,"Native movement send"))return false;
        std::lock_guard guard(s.state);s.context=c;s.moving=true;s.motionPending=true;++s.moves;
        const auto now=StreamNowUs();s.moveTimes.push_back(now);while(!s.moveTimes.empty() && now-s.moveTimes.front()>1000000)s.moveTimes.pop_front();
        return true; // Serial acceptance, not a USB-delivery acknowledgement.
    }
    bool MediusBackend::BeginClick(int hold,const OutputContext& c)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);if(!s.healthy || !s.armed || !Current(c,StreamNowUs()))return false;
        {std::lock_guard guard(s.state);if(s.clickActive)return false;s.clickCompleted=false;s.clickAborted=false;}
        if(!s.ClearClickLocked())return false;
        {std::lock_guard guard(s.state);s.clickAborted=false;}
        if(!BuildClick(s.builder,hold) || !Current(c,StreamNowUs()))return false;
        if(!s.Good(medius_clip_append(s.clip,s.builder),"Finite click upload") ||
            !s.Good(medius_clip_finalize(s.clip),"Finite click finalize"))return false;
        // These commands have no delivery acknowledgement. Confirm the complete
        // finite upload/settings before START, rather than accepting an empty
        // idle ring as evidence that a click was played.
        MediusClipSettings config{};MediusClipStatus uploaded{};
        if(!s.Good(medius_clip_query_config(s.clip,&config),"Finite click settings readback") ||
            !s.ClipStatusLocked(uploaded))return false;
        if(!ClickSettingsMatch(config) || uploaded.state!=MEDIUS_CLIP_STATE_IDLE || HeldLeft(uploaded) ||
            uploaded.total!=medius_clip_builder_byte_len(s.builder))
        {s.Fail("Finite click upload/settings did not verify: "+ClickReadbackText(uploaded,s.clickBaselineTicks));return false;}
        if(!Current(c,StreamNowUs()) || !s.Good(medius_clip_start(s.clip),"Finite click start"))return false;
        {std::lock_guard guard(s.state);s.clickActive=true;++s.clicks;}return true;
    }
    bool MediusBackend::FinishClick(bool cancelled)
    {
        auto& s=*state_;
        if(cancelled){std::lock_guard transaction(s.io);return s.device && s.ClearClickLocked();}
        // Per-query timeout remains 20 ms. This bounds the whole completion
        // sequence, allowing native playback/serial scheduling to settle.
        const auto until=StreamNowUs()+100000;
        MediusClipStatus last{};
        do
        {
            {std::lock_guard transaction(s.io);if(!s.ClipStatusLocked(last))return false;
                if(ClickEdgesPlayed(last,s.clickBaselineTicks))
                {
                    if(ClickIdle(last))return true;
                    // Normalize a drained engine only after both button edges
                    // actually played. STOP never cancels ordinary MOVE output.
                    if(last.state==MEDIUS_CLIP_STATE_PLAYING)
                    {
                        if(!s.Good(medius_clip_stop(s.clip),"Drained click stop") || !s.ClipStatusLocked(last))return false;
                        ++s.drainedClickStops;
                        if(ClickIdle(last) && ClickEdgesPlayed(last,s.clickBaselineTicks))return true;
                    }
                }}
            // Never retain the serial mutex while waiting for firmware playback.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }while(StreamNowUs()<until);
        s.Fail("Finite click completion exceeded 100 ms: "+ClickReadbackText(last,s.clickBaselineTicks));return false;
    }
    bool MediusBackend::ClickCompleted() const
    {std::lock_guard guard(state_->state);return state_->clickCompleted && !state_->clickAborted;}
    bool MediusBackend::NeutralizeLeft()
    {auto& s=*state_;std::lock_guard transaction(s.io);return s.device && s.ClearClickLocked();}
    void MediusBackend::EndMotion(const OutputContext* ended)
    {
        auto& s=*state_;std::lock_guard guard(s.state);if(ended && !SameCorrection(s.context,*ended))return;
        s.moving=false; // Sent native counts drain normally; independent clicks continue.
    }
    void MediusBackend::CancelAll()
    {
        auto& s=*state_;std::lock_guard transaction(s.io);if(!s.device)return;++s.cancellations;
        const auto discarded=medius_device_discard_motion(s.device);if(s.healthy)s.Good(discarded,"Pending motion cancellation");
        s.ClearClickLocked();std::lock_guard guard(s.state);s.moving=false;s.context={};
    }
    bool MediusBackend::Healthy() const{return state_->healthy.load();}
    std::string MediusBackend::Identity() const{std::lock_guard guard(state_->state);return state_->identity;}
    std::string MediusBackend::FailureReason() const{std::lock_guard guard(state_->state);return state_->lastFailure;}
    std::string MediusBackend::Status() const
    {
        auto& s=*state_;std::lock_guard guard(s.state);const auto now=StreamNowUs();
        const auto rate=std::count_if(s.moveTimes.begin(),s.moveTimes.end(),[&](auto time){return now>=time && now-time<=1000000;});
        auto result=s.status+"\nNative move commands: "+std::to_string(rate)+" /s; click completions: "+std::to_string(s.completedClicks.load())+".";
        result+="\nDevice chip: "+s.deviceFirmware+"; host chip: "+s.hostFirmware+"; protocol: "+std::to_string(s.protocol);
        if(!s.lastFailure.empty())result+="\nLast output fault: "+s.lastFailure;return result;
    }
    MediusRecoverySettings MediusBackend::RecoverySettings() const
    {std::lock_guard guard(state_->state);return state_->saved;}
    std::string MediusBackend::DiagnosticsJson() const
    {
        auto& s=*state_;const auto write=s.serialWrite.Get(),health=s.healthQueries.Get(),click=s.clipQueries.Get();
        std::lock_guard guard(s.state);const auto now=StreamNowUs();
        const auto rate=std::count_if(s.moveTimes.begin(),s.moveTimes.end(),[&](auto t){return now>=t && now-t<=1000000;});
        std::ostringstream out;out<<"{\"backend\":\"medius\",\"sdk\":\"3.4.6-vanta-manual-recovery\",\"output_path\":\"native_move_finite_click\""
            <<",\"device_firmware\":"<<InputJsonString(s.deviceFirmware)<<",\"host_firmware\":"<<InputJsonString(s.hostFirmware)<<",\"protocol\":"<<s.protocol
            <<",\"healthy\":"<<(s.healthy?"true":"false")<<",\"native_period_us\":"<<s.nativePeriodUs<<",\"clone_poll_period_us\":"<<s.pollPeriodUs
            <<",\"native_rate_confirmed\":"<<(s.rateConfirmed?"true":"false")<<",\"native_move_commands\":"<<s.moves.load()<<",\"move_command_hz\":"<<rate
            <<",\"bearing_capability_verified\":"<<(s.saved.bearingValid?"true":"false")
            <<",\"original_bearing_window_ms\":"<<s.saved.bearingWindowMs<<",\"original_bearing_mode\":"<<unsigned(s.saved.bearingMode)
            <<",\"clicks_started\":"<<s.clicks.load()<<",\"clicks_completed\":"<<s.completedClicks.load()<<",\"clicks_cancelled\":"<<s.cancelledClicks.load()
            <<",\"click_active\":"<<(s.clickActive?"true":"false")<<",\"correction_active\":"<<(s.moving?"true":"false")
            <<",\"click_completion_deadline_ms\":100,\"click_query_timeout_ms\":20,\"drained_click_stops\":"<<s.drainedClickStops.load()
            <<",\"click_readback\":{\"at_local_us\":"<<s.lastClickReadbackUs<<",\"state\":"<<unsigned(s.lastClickReadback.state)
            <<",\"buffered_bytes\":"<<s.lastClickReadback.total<<",\"content_edges\":"<<static_cast<std::uint32_t>(s.lastClickReadback.ticks-s.clickBaselineTicks)
            <<",\"left_held\":"<<(HeldLeft(s.lastClickReadback)?"true":"false")<<'}'
            <<",\"injection_active_last_readback\":"<<(s.motionPending?"true":"false")
            <<",\"faults\":"<<s.faults.load()<<",\"cancellations\":"<<s.cancellations.load()<<",\"physical_events_dropped\":"<<s.inputDrops.load()
            <<",\"last_failure\":"<<InputJsonString(s.lastFailure)<<",\"last_failure_local_us\":"<<s.lastFailureUs
            <<",\"identity\":"<<InputJsonString(s.identity)<<",\"motion_idle_measurement\":\"firmware health includes independent clicks\""
            <<",\"send_measurement\":\"serial acceptance only; no per-command USB delivery acknowledgement\",\"timings\":{";
        const auto metric=[&](const char* name,const TimingSummary& value)
        {out<<'"'<<name<<"\":{\"median_ms\":"<<value.medianMs<<",\"p95_ms\":"<<value.p95Ms<<",\"p99_ms\":"<<value.p99Ms<<",\"samples\":"<<value.samples<<'}';};
        metric("native_move_serial_write",write);out<<',';metric("output_health_roundtrip",health);out<<',';metric("click_release_roundtrip",click);
        out<<"}}";return out.str();
    }
    bool RecoverMedius(const std::string& port,const MediusRecoverySettings& saved)
    {
        MediusDevice* device{};if(!saved.valid || !saved.bearingValid || medius_device_open(port.c_str(),&device)!=kOk)return false;
        if(medius_device_set_manual_recovery(device,true,20)!=kOk){medius_device_free(device);return false;}
        MediusVersion version{};MediusFirmwareInfo firmware{};bool clean=false;
        if(medius_device_query_version(device,&version)==kOk && version.proto_ver==9 &&
            std::equal(std::begin(version.mac),std::end(version.mac),saved.mac) &&
            medius_device_firmware_info(device,&firmware)==kOk && Firmware346(firmware.device) && firmware.host_present && Firmware346(firmware.host))
        {
            MediusClip* clip{};clean=medius_device_clip(device,&clip)==kOk;
            clean=(medius_device_discard_motion(device)==kOk) && clean;
            if(clip)clean=(medius_clip_clear(clip)==kOk) && clean;
            clean=(medius_device_soft_release(device,medius_usage_button(MEDIUS_BUTTON_LEFT))==kOk) && clean;
            for(auto bit:{InputLockX,InputLockY,InputLockLeft,InputLockSide1,InputLockSide2})
                clean=(medius_device_unlock(device,Target(bit),MEDIUS_DIRECTION_BOTH)==kOk) && clean;
            MediusClipStatus clipState{};MediusLocks locks{};MediusHealth health{};
            clean=clean && medius_clip_query_status(clip,&clipState)==kOk && ClickIdle(clipState) &&
                medius_device_query_health(device,&health)==kOk && !health.injection_active && medius_device_query_locks(device,&locks)==kOk;
            MediusBearing bearing{};InputMaskReadback actual;
            clean=clean && medius_device_query_bearing(device,&bearing)==kOk && DecodeMasks(locks,bearing,actual) &&
                actual.passthroughMask==InputManagedLocks;
            if(clean)clean=RestoreOptions(device,saved);
            else {medius_device_set_render(device,saved.renderMode,saved.renderFull!=0);medius_device_set_spread(device,saved.spreadPercent);
                medius_device_set_movement_riding(device,saved.rideEnabled,saved.rideWindowMs);
                if(saved.bearingValid)medius_device_set_bearing(device,saved.bearingWindowMs,saved.bearingMode);}
            if(clip)medius_clip_free(clip);
        }
        medius_device_free(device);return clean;
    }
    bool MediusBackend::RunSelfTest()
    {
        // Opt-in checks compiled into Vanta, never run by the compile-only build.
        MediusLocks table{};MediusBearing bearing{};bearing.window_ms=kFuseBearingWindowMs;bearing.mode=MEDIUS_BEARING_MODE_PER_AXIS;
        InputMaskReadback actual;
        if(!DecodeMasks(table,bearing,actual) || actual.passthroughMask!=InputManagedLocks)return false;
        table.n=1;table.entries[0]={Target(InputLockX),false,MEDIUS_DIRECTION_BOTH,0};
        if(!DecodeMasks(table,bearing,actual) || actual.positiveScale[0]!=0 || actual.negativeScale[0]!=0 ||
            actual.withScale[0]!=100 || actual.againstScale[0]!=100)return false;
        table.n=2;table.entries[1]={Target(InputLockX),false,MEDIUS_DIRECTION_AGAINST,50};
        if(!DecodeMasks(table,bearing,actual) || actual.againstScale[0]!=50 || actual.withScale[0]!=100)return false;
        table.entries[0].scale=100;table.entries[1].scale=0;
        if(!DecodeMasks(table,bearing,actual) || actual.blockedMask || (actual.passthroughMask&InputLockX) || actual.againstScale[0]!=0)return false;
        table.n=3;table.entries[2]={Target(InputLockY),false,MEDIUS_DIRECTION_WITH,255};
        if(!DecodeMasks(table,bearing,actual) || actual.withScale[1]!=255 || (actual.passthroughMask&InputLockY))return false;
        table.entries[2].direction=99;if(DecodeMasks(table,bearing,actual))return false;
        table.n=MEDIUS_MAX_LOCKS+1;if(DecodeMasks(table,bearing,actual))return false;
        MediusRecoverySettings saved;saved.valid=saved.bearingValid=true;saved.bearingWindowMs=47;saved.bearingMode=MEDIUS_BEARING_MODE_VECTOR;
        saved.renderMode=1;saved.renderFull=1;saved.spreadPercent=17;saved.rideEnabled=true;saved.rideWindowMs=77;
        MediusRecoverySettings hardware;int writes{},reads{};bool failWrite{},failRead{},drift{};
        const auto write=[&](const MediusRecoverySettings& value){++writes;if(failWrite)return false;hardware=value;return true;};
        const auto read=[&](MediusRecoverySettings& value){++reads;if(failRead)return false;value=hardware;if(drift)value.bearingWindowMs=100;return true;};
        if(!RestoreOptions(saved,write,read) || writes!=1 || reads!=1 || hardware.bearingWindowMs!=47 || hardware.bearingMode!=MEDIUS_BEARING_MODE_VECTOR)return false;
        failWrite=true;if(RestoreOptions(saved,write,read) || reads!=1)return false;
        failWrite=false;failRead=true;if(RestoreOptions(saved,write,read))return false;
        failRead=false;drift=true;if(RestoreOptions(saved,write,read))return false;
        saved.bearingValid=false;const auto priorWrites=writes;if(RestoreOptions(saved,write,read) || writes!=priorWrites)return false;
        OutputContext c;c.owner=InputOwner::python;c.generation=7;c.streamGeneration=2;c.sequence=10;
        if(!NativeDelta(32767,-32768,c) || NativeDelta(32768,0,c))return false;
        c.axes=InputLockX;if(!NativeDelta(900,0,c) || NativeDelta(1,1,c))return false;
        auto old=c;old.sequence=9;if(SameCorrection(c,old))return false;
        old.sequence=10;if(!SameCorrection(c,old))return false;
        old.streamGeneration=1;if(SameCorrection(c,old))return false;
        c.frameTimeUs=1;if(Current(c,100002))return false;
        c.frameTimeUs=0;c.isCurrent=[]{return false;};if(Current(c,1))return false;
        MediusClipStatus status{};status.state=MEDIUS_CLIP_STATE_IDLE;
        if(!ClickIdle(status) || NewClipFault(status,0,0,false))return false;
        status.total=1;if(ClickIdle(status))return false;
        status.total=0;status.overruns=7;status.seq_gaps=11;
        if(NewClipFault(status,7,11,false) || !NewClipFault(status,6,11,false))return false;
        status={};status.state=MEDIUS_CLIP_STATE_IDLE;status.ticks=12;
        if(!ClickEdgesPlayed(status,10) || ClickEdgesPlayed(status,12) || ClickEdgesPlayed(status,11))return false;
        status.ticks=0;if(!ClickEdgesPlayed(status,0xfffffffeU))return false;
        status.total=1;if(ClickEdgesPlayed(status,0xfffffffeU))return false;
        status.total=0;status.held_n=1;status.held[0]=medius_usage_button(MEDIUS_BUTTON_LEFT);
        if(ClickEdgesPlayed(status,0xfffffffeU))return false;
        MediusClipSettings finite{};finite.finalized=1;if(!ClickSettingsMatch(finite))return false;
        finite.loop_=1;if(ClickSettingsMatch(finite))return false;finite.loop_=0;finite.retain=1;
        if(ClickSettingsMatch(finite))return false;finite.retain=0;finite.finalized=0;
        if(ClickSettingsMatch(finite))return false;
        // Idle alone is not success. Count each confirmed press/release once,
        // including consecutive clips with a cumulative tick baseline.
        Implementation playback;status={};status.state=MEDIUS_CLIP_STATE_IDLE;
        for(std::uint32_t base=10;base<18;base+=2)
        {
            playback.clickActive=true;playback.clickAborted=playback.clickCompleted=false;playback.clickBaselineTicks=base;
            status.ticks=base;playback.ObserveClick(status);if(playback.clickCompleted)return false;
            status.ticks=base+2;status.state=MEDIUS_CLIP_STATE_PLAYING;playback.ObserveClick(status);
            if(playback.clickCompleted)return false;
            status.state=MEDIUS_CLIP_STATE_IDLE;playback.ObserveClick(status);playback.ObserveClick(status);
            if(!playback.clickCompleted || playback.clickActive)return false;
        }
        if(playback.completedClicks!=4)return false;
        auto* builder=medius_clip_builder_new();if(!builder)return false;
        const bool valid=BuildClick(builder,14) && medius_clip_builder_byte_len(builder)>0 && BuildClick(builder,34);
        medius_clip_builder_free(builder);
        MediusBackend backend;backend.state_->context=old;backend.state_->moving=true;backend.state_->clickActive=true;
        backend.EndMotion(&c);if(!backend.state_->moving || !backend.state_->clickActive)return false;
        backend.EndMotion(&old);if(backend.state_->moving || !backend.state_->clickActive)return false;
        return valid;
    }
}
