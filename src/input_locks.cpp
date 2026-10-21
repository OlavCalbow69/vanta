#include "input_locks.hpp"
#include "medius_backend.hpp"
#include "stream_frame.hpp"
#include "logger.hpp"
#include "makcu_c.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    constexpr auto kFreshUs=100000ULL;
    constexpr auto kDrainUs=500000ULL;
    constexpr auto kMaskVerifyUs=100000ULL;
    std::size_t Index(vanta::InputOwner owner){return static_cast<std::size_t>(owner);}
    std::uint8_t Button(vanta::MouseButtonBinding b){return static_cast<std::uint8_t>(1U<<static_cast<unsigned>(b));}
    const char* QueryName(vanta::InputQuery query)
    {
        switch(query)
        {
        case vanta::InputQuery::lockX:return "physical X mask";
        case vanta::InputQuery::lockY:return "physical Y mask";
        case vanta::InputQuery::lockLeft:return "physical left-button mask";
        case vanta::InputQuery::lockSide1:return "physical X1 mask";
        case vanta::InputQuery::lockSide2:return "physical X2 mask";
        case vanta::InputQuery::leftState:return "injected left-button release";
        default:return "pending movement";
        }
    }
    struct Policy {bool enabled{};std::uint64_t generation{};vanta::MouseButtonBinding button{};};
    std::uint32_t Reserved(const std::array<Policy,4>& policies)
    {
        std::uint32_t mask{};
        for(const auto& p:policies)if(p.enabled)
        {if(p.button==vanta::MouseButtonBinding::x1)mask|=vanta::InputLockSide1;
         if(p.button==vanta::MouseButtonBinding::x2)mask|=vanta::InputLockSide2;}
        return mask;
    }
    bool SameSession(const vanta::OutputContext& a,const vanta::OutputContext& b)
    {return a.owner==b.owner && a.generation==b.generation && a.streamGeneration==b.streamGeneration && a.streamSession==b.streamSession;}
    std::string Escape(const std::string& value)
    {std::string s="\"";for(unsigned char c:value){if(c=='"'||c=='\\')s+='\\';if(c>=32)s+=char(c);}return s+'"';}
    std::string MaskText(std::uint32_t mask)
    {
        constexpr const char* names[]{"X","Y","left","X1","X2"};std::string result;
        for(std::size_t i=0;i<vanta::kManagedInputBits.size();++i)if(mask&vanta::kManagedInputBits[i])
        {if(!result.empty())result+=' ';result+=names[i];}
        return result.empty()?"none":result;
    }
    std::uint32_t GuidedAxes(int x,int y,std::uint32_t permitted)
    {return ((x?vanta::InputLockX:0U)|(y?vanta::InputLockY:0U))&permitted;}
    int Sign(int value){return (value>0)-(value<0);}
    bool IsFuse(const vanta::OutputContext& context)
    {return context.physicalInputMode==vanta::PythonPhysicalInputMode::fuse;}
    vanta::DirectionalInputScales Scales(const vanta::InputMaskReadback& actual,std::size_t i)
    {return {actual.positiveScale[i],actual.negativeScale[i],actual.withScale[i],actual.againstScale[i]};}
    std::uint32_t Blocked(const vanta::ManagedInputPolicy& policy)
    {
        std::uint32_t mask{};
        for(std::size_t i=0;i<vanta::kManagedInputBits.size();++i)
            if(policy.scales[i].positive==0 && policy.scales[i].negative==0)mask|=vanta::kManagedInputBits[i];
        return mask;
    }
    bool PolicyChanged(const vanta::OutputContext& old,const vanta::OutputContext& current,int oldX,int oldY,int x,int y)
    {
        return old.axes!=current.axes || old.physicalInputMode!=current.physicalInputMode ||
            (IsFuse(current) && (oldX!=Sign(x) || oldY!=Sign(y)));
    }
    bool MasksMatch(const vanta::InputMaskReadback& actual,const vanta::ManagedInputPolicy& expected)
    {
        if(actual.knownMask!=vanta::InputManagedLocks || actual.blockedMask!=Blocked(expected) ||
            actual.passthroughMask!=(vanta::InputManagedLocks&~(Blocked(expected)|expected.guidedAxes)))return false;
        for(std::size_t i=0;i<vanta::kManagedInputBits.size();++i)
            if(Scales(actual,i)!=expected.scales[i])return false;
        return !expected.bearingRequired || (actual.bearingKnown && actual.bearingWindowMs==vanta::kFuseBearingWindowMs && actual.bearingMode==0);
    }
}

namespace vanta
{
    ManagedInputPolicy MakeManagedInputPolicy(std::uint32_t blocked,std::uint32_t guidedAxes)
    {
        ManagedInputPolicy result;result.guidedAxes=guidedAxes&(InputLockX|InputLockY)&~blocked;
        result.bearingRequired=result.guidedAxes!=0;
        for(std::size_t i=0;i<kManagedInputBits.size();++i)
        {
            if(blocked&kManagedInputBits[i])result.scales[i].positive=result.scales[i].negative=0;
            if(result.guidedAxes&kManagedInputBits[i])result.scales[i].against=0;
        }
        return result;
    }
    InputQuery LockQuery(std::uint32_t bit)
    {switch(bit){case InputLockX:return InputQuery::lockX;case InputLockY:return InputQuery::lockY;
        case InputLockLeft:return InputQuery::lockLeft;case InputLockSide1:return InputQuery::lockSide1;default:return InputQuery::lockSide2;}}
    namespace {
    const char* StockQuery(InputQuery query)
    {switch(query){case InputQuery::lockX:return MakcuLockQuery(InputLockX);case InputQuery::lockY:return MakcuLockQuery(InputLockY);
        case InputQuery::lockLeft:return MakcuLockQuery(InputLockLeft);case InputQuery::lockSide1:return MakcuLockQuery(InputLockSide1);
        case InputQuery::lockSide2:return MakcuLockQuery(InputLockSide2);case InputQuery::leftState:return "km.left()";default:return "km.moving()";}}
    }
    struct InputLockCoordinator::Implementation
    {
        // io serializes transactions, including uncorrelated firmware queries.
        // state is never held over serial operations or completion waits.
        mutable std::mutex state,io;
        std::condition_variable changed;
        std::jthread worker;
        InputLockTransport transport;
        std::function<void(const OutputContext&)> endNotifier; // state mutex; never serial I/O.
        InputRecoverySession recovery;
        MouseOutputConfig settings;
        std::array<Policy,4> policies{};
        std::array<std::uint64_t,4> emergencyGenerations{~0ULL,~0ULL,~0ULL,~0ULL};
        std::uint32_t motionMask{},motionGuidedAxes{},clickMask{};
        int motionXSign{},motionYSign{};
        bool transitionFreshRequired{};
        std::uint64_t freshAfterUs{};
        std::uint8_t physical{},physicalKnown{},observedReleased{},lastNativePressProof{};
        PhysicalButtonEdge lastPhysicalEdge{PhysicalButtonEdge::snapshot};
        std::uint64_t physicalSnapshots{},nativePressProofs{},lastPhysicalUs{};
        bool attached{},probed{},motion{},motionCancelled{},activationReleased{},injectedLeft{},clickCancelled{},faultCleaned{};
        std::atomic_bool uncertainLeft{};
        OutputContext movement,click;
        std::uint64_t lastMoveUs{},drainStartedUs{},nextDrainUs{},clickDeadlineUs{};
        std::shared_ptr<std::promise<bool>> clickResult;
        std::atomic<std::uint32_t> applied{},uncertain{},requested{};
        std::atomic_bool resetRequired{},draining{},ready{};
        std::atomic_bool helperHealthy{};
        std::atomic_bool cancelAllPending{};
        std::atomic_uint64_t emergencyRevision{},transitions{},failures{},releases{},staleRejections{},drainTimeouts{};
        RollingStreamMetric transactionWait,firmwareQuery,maskTransition,moveSerial;
        std::string status{"Connect MAKCU to initialize physical-input locks"};
        std::array<std::string,4> movementBlocks;
        std::array<std::uint64_t,4> movementBlockCounts{},movementBlockAt{};
        std::array<bool,4> movementBlocked{};
        std::array<ClickOutputStatus,4> clickStatus;
        std::array<std::string,4> clickLogMessages;
        std::array<std::uint64_t,4> clickLogAt{};
        InputMaskReadback firmwareMasks;
        bool refreshMasks{true},verificationValid{};
        ManagedInputPolicy verifiedExpected,requestedPolicy;
        std::uint64_t connectionEpoch{},verificationEpoch{},lastMaskSummaryUs{};
        std::atomic_uint64_t maskReadbacks{},maskReadbackFailures{},maskDrifts{};
        RollingStreamMetric maskReadbackTime;
        RollingStreamMetric fuseTransitionTime;
        std::function<std::uint64_t()> maskClock{StreamNowUs}; // Injectable only in opt-in deterministic checks.
        std::string verificationError,releaseReason,lastReleaseReason;
        std::uint64_t releaseRequestedUs{},lastReleaseUs{};
        InputOwner lastReleasedOwner{InputOwner::manual};
        std::deque<std::pair<InputOwner,std::string>> releaseLogs;

        void ResetVerificationLocked()
        {
            ++connectionEpoch;firmwareMasks={};verifiedExpected={};requestedPolicy={};verificationEpoch=0;
            physical=physicalKnown=observedReleased=lastNativePressProof=0;
            lastPhysicalEdge=PhysicalButtonEdge::snapshot;lastPhysicalUs=0;
            freshAfterUs=0;transitionFreshRequired=false;motionGuidedAxes=0;motionXSign=motionYSign=0;
            cancelAllPending=false;
            activationReleased=false;
            verificationValid=false;refreshMasks=true;verificationError.clear();lastMaskSummaryUs=0;
        }
        bool RequestEndLocked(const char* why)
        {
            if(!motion || motionCancelled)return false;
            motionCancelled=true;releaseReason=why;releaseRequestedUs=StreamNowUs();
            releaseLogs.emplace_back(movement.owner,releaseReason);return true;
        }
        void LogReleaseRequested()
        {
            std::string reason;InputOwner owner;
            {std::lock_guard guard(state);if(releaseLogs.empty())return;
                owner=releaseLogs.front().first;reason=std::move(releaseLogs.front().second);releaseLogs.pop_front();}
            log::Info("Input correction ending: owner=%u, reason=%s",unsigned(owner),reason.c_str());
        }

        bool Protected() const{return settings.backend==InputBackend::medius || settings.lockMovementWhileCorrecting || settings.lockGeneratedClick || settings.reserveActivationSideButtons;}
        const char* InvalidReason(const OutputContext& c,std::uint64_t now)
        {
            if(c.owner!=InputOwner::manual)
            {
                std::lock_guard guard(state);
                const auto& p=policies[Index(c.owner)];
                if(!p.enabled)return "Feature activation policy is disabled";
                if(p.generation!=c.generation)return "Feature configuration generation changed";
                if(p.button!=c.activation)return "Activation button binding changed";
                if(!(physical&Button(c.activation)))return "Physical activation button is not held";
                if(!(observedReleased&Button(c.activation)))return "Release and press activation after connecting or emergency release";
                if(motion && activationReleased && SameSession(c,movement))return "Physical activation released; the old correction must drain";
                if(motion && motionCancelled && SameSession(c,movement))return "The previous correction is ending; wait for it to drain";
                if(c.owner==InputOwner::testMove && policies[Index(InputOwner::python)].enabled)return "Python owns movement";
            }
            if(c.frameTimeUs && now<c.frameTimeUs)return "Frame timestamp is ahead of the local clock";
            if(c.frameTimeUs && now-c.frameTimeUs>kFreshUs)return "Source frame is older than the 100 ms freshness limit";
            try{if(c.isCurrent && !c.isCurrent())return "Source stream or feature configuration changed";}
            catch(...){return "Source validity check failed";}
            return nullptr;
        }
        bool Valid(const OutputContext& c,std::uint64_t now){return !InvalidReason(c,now);}
        const char* InvalidClickReason(const OutputContext& c,std::uint64_t now)
        {
            if(const auto reason=InvalidReason(c,now))return reason;
            std::lock_guard guard(state);
            if(injectedLeft && clickCancelled && SameSession(c,click))return "Activation or settings changed during this click; use a fresh frame";
            return nullptr;
        }
        bool BlockUnlocked(InputOwner owner,const std::string& why)
        {const auto i=Index(owner);movementBlocks[i]=why;++movementBlockCounts[i];movementBlockAt[i]=StreamNowUs();movementBlocked[i]=true;return false;}
        bool Block(InputOwner owner,const std::string& why)
        {std::lock_guard guard(state);return BlockUnlocked(owner,why);}
        void RecordClick(InputOwner owner,ClickOutputPhase phase,const std::string& message,bool report=false)
        {
            const auto i=Index(owner),now=StreamNowUs();bool writeLog{};
            std::uint64_t attempts{},completed{};
            {std::lock_guard guard(state);auto& value=clickStatus[i];value.phase=phase;value.message=message;value.updatedAtUs=now;
                if(phase==ClickOutputPhase::blocked)++value.rejected;
                if(phase==ClickOutputPhase::completed)++value.completed;
                if(phase==ClickOutputPhase::cancelled)++value.cancelled;
                if(phase==ClickOutputPhase::failed)++value.failed;
                if(report && (clickLogMessages[i]!=message || !clickLogAt[i] || now-clickLogAt[i]>=1000000))
                {writeLog=true;clickLogMessages[i]=message;clickLogAt[i]=now;}
                attempts=value.attempts;completed=value.completed;}
            if(writeLog)log::Info("Click output: owner=%u attempts=%llu completed=%llu reason=%s",unsigned(owner),
                static_cast<unsigned long long>(attempts),static_cast<unsigned long long>(completed),message.c_str());
        }
        bool BlockClick(InputOwner owner,const std::string& why)
        {RecordClick(owner,ClickOutputPhase::blocked,why,true);return false;}
        std::string BackendFailure(const char* fallback)
        {const auto reason=transport.failureReason?transport.failureReason():std::string{};return reason.empty()?fallback:reason;}
        OutputContext Checked(const OutputContext& c,std::uint64_t emergency,bool forClick=false)
        {
            OutputContext result=c;
            result.isCurrent=[this,c,emergency,forClick]
            {
                // Click cancellation is reported by the owning click transaction,
                // rather than mixing it into movement rejection diagnostics.
                if(emergencyRevision.load()!=emergency)return forClick?false:Block(c.owner,"Emergency release cancelled pending output");
                if(resetRequired)return forClick?false:Block(c.owner,"Mouse output recovery is required");
                if(const auto reason=forClick?InvalidClickReason(c,StreamNowUs()):InvalidReason(c,StreamNowUs()))
                    return forClick?false:Block(c.owner,reason);
                return true;
            };
            return result;
        }
        std::uint32_t Desired()
        {
            std::lock_guard guard(state);
            return (settings.reserveActivationSideButtons?Reserved(policies):0U) |
                (motion?motionMask:0U) |
                (injectedLeft?clickMask:0U);
        }
        ManagedInputPolicy Expected(std::uint32_t desired)
        {
            std::lock_guard guard(state);
            return MakeManagedInputPolicy(desired,motion && IsFuse(movement)?motionGuidedAxes:0U);
        }
        void Mark()
        {
            bool left{};std::uint32_t guided{};{std::lock_guard guard(state);left=injectedLeft;guided=motionGuidedAxes;}
            if(transport.mark)transport.mark(applied.load()|uncertain.load()|guided|((left || uncertainLeft)?InputInjectedLeft:0U));
        }
        void Fail(const std::string& why)
        {
            {std::lock_guard guard(state);status=why;motionCancelled=true;clickCancelled=true;verificationValid=false;
                if(motion){releaseReason="Recovery: "+why;releaseRequestedUs=StreamNowUs();}}
            ready=false;resetRequired=true;faultCleaned=false;++failures;
            if(transport.invalidate)transport.invalidate(why);
            log::Warning("Mouse output recovery required: %s",why.c_str());changed.notify_all();
        }
        bool Query(InputQuery command,int& value)
        {
            const auto started=StreamNowUs();
            const bool success=transport.query && transport.query(command,value);
            firmwareQuery.Add(double(StreamNowUs()-started)/1000.0);
            if(!success)
            {const auto why=std::string("Firmware ")+QueryName(command)+" query failed; serial reset required before another query";
                Fail(BackendFailure(why.c_str()));return false;}
            return true;
        }
        bool VerifyMasks(std::uint32_t desired,bool force=false)
        {
            if(!transport.readMasks)return true; // Stock firmware retains per-command readback.
            if(resetRequired)return false;
            const auto before=maskClock();const auto expected=Expected(desired);
            {std::lock_guard guard(state);
                if(!force && !refreshMasks && verificationValid && verificationEpoch==connectionEpoch &&
                    verifiedExpected==expected && before>=firmwareMasks.readAtUs && before-firmwareMasks.readAtUs<kMaskVerifyUs)return true;}
            InputMaskReadback actual;const bool read=transport.readMasks(actual);
            const auto now=maskClock();maskReadbackTime.Add(double(now-before)/1000.0);++maskReadbacks;
            const bool timely=read && actual.readAtUs>=before && actual.readAtUs<=now;
            const bool matches=timely && MasksMatch(actual,expected);
            const std::string why=!read?"Firmware mask readback failed; reconnect required":!timely?
                "Firmware mask readback had a stale or invalid timestamp":!matches?
                "Firmware masks disagree: requested "+MaskText(desired)+", read back "+MaskText(actual.blockedMask)+
                    "; guided axes="+MaskText(expected.guidedAxes)+"; directional or bearing policy mismatch":std::string{};
            {std::lock_guard guard(state);
                if(read){firmwareMasks=actual;verificationEpoch=connectionEpoch;}
                verificationValid=matches;verifiedExpected=expected;refreshMasks=!matches;verificationError=why;}
            if(!matches)
            {
                if(!read || !timely)++maskReadbackFailures;else ++maskDrifts;
                uncertain.fetch_or(InputManagedLocks);Mark();Fail(why);
                // Invalidate the Medius session first. Cleanup then uses writes
                // only, so delayed replies cannot be accepted as confirmations.
                BlindCleanup();return false;
            }
            applied=desired;uncertain.fetch_and(~InputManagedLocks);Mark();return true;
        }
        bool Reconcile(std::uint32_t desired)
        {
            desired&=InputManagedLocks;requested=desired;
            const auto expected=Expected(desired);
            {std::lock_guard guard(state);requestedPolicy=expected;}
            if(expected.bearingRequired && (!transport.prepareFuse || !transport.prepareFuse()))
            {Fail("Medius per-axis bearing setup is unavailable; Fuse inputs cannot arm");return false;}
            const auto transitionStarted=StreamNowUs();const auto previous=applied.load();std::uint32_t changedMask{};
            for(std::size_t i=0;i<kManagedInputBits.size();++i)
            {
                const auto bit=kManagedInputBits[i];
                const bool target=(desired&bit)!=0;
                bool directionalChange{};
                {std::lock_guard guard(state);directionalChange=transport.readMasks && (firmwareMasks.knownMask&bit) && Scales(firmwareMasks,i)!=expected.scales[i];}
                if(((applied.load()&bit)!=0)==target && !(uncertain.load()&bit) && !directionalChange && !(expected.guidedAxes&bit))continue;
                // A verified relative profile is retained between frames. It
                // must not be rewritten around each individual MOVE.
                if(!(uncertain.load()&bit) && ((applied.load()&bit)!=0)==target && !directionalChange)
                {std::lock_guard guard(state);if((firmwareMasks.knownMask&bit) && Scales(firmwareMasks,i)==expected.scales[i])continue;}
                const auto started=StreamNowUs();
                uncertain.fetch_or(bit);Mark(); // Record intent BEFORE the mutation.
                const bool written=transport.axisPolicy && (bit==InputLockX || bit==InputLockY)?
                    transport.axisPolicy(bit,expected.scales[i]):transport.lock && transport.lock(bit,target);
                if(!written){Fail("Physical-input directional policy write failed");return false;}
                changedMask|=bit;
                if(transport.readMasks)continue; // One firmware table verifies the entire batch below.
                int actual{};
                if(!Query(LockQuery(bit),actual))return false;
                if(actual!=(target?1:0)){Fail("Physical-input mask readback disagrees with request");return false;}
                if(target)applied.fetch_or(bit);else applied.fetch_and(~bit);
                uncertain.fetch_and(~bit);++transitions;Mark();
                {std::lock_guard guard(state);
                    firmwareMasks.knownMask|=bit;firmwareMasks.blockedMask=(firmwareMasks.blockedMask&~bit)|(target?bit:0U);
                    firmwareMasks.passthroughMask=(firmwareMasks.passthroughMask&~bit)|(target?0U:bit);
                    firmwareMasks.positiveScale[i]=firmwareMasks.negativeScale[i]=target?0:100;
                    firmwareMasks.readAtUs=StreamNowUs();verificationEpoch=connectionEpoch;
                    firmwareMasks.withScale[i]=firmwareMasks.againstScale[i]=100;
                    verificationValid=firmwareMasks.knownMask==InputManagedLocks;verifiedExpected=expected;}
                maskTransition.Add(double(StreamNowUs()-started)/1000.0);
            }
            if(changedMask && transport.readMasks)
            {
                if(!VerifyMasks(desired,true))return false;
                for(const auto bit:kManagedInputBits)if(changedMask&bit)++transitions;
                maskTransition.Add(double(StreamNowUs()-transitionStarted)/1000.0);
            }
            if(changedMask)log::Info("Input policy changed: previous_blocks=%s, requested_blocks=%s, guided_axes=%s; firmware readback verified",
                MaskText(previous).c_str(),MaskText(desired).c_str(),MaskText(expected.guidedAxes).c_str());
            return true;
        }
        void CompleteClick(bool success,const std::string& reason={})
        {
            std::shared_ptr<std::promise<bool>> completion;InputOwner owner{};std::string detail;
            {std::lock_guard guard(state);owner=click.owner;detail=reason.empty()?(resetRequired?status:"Click cancelled before confirmed completion"):reason;
                injectedLeft=false;clickCancelled=false;click={};completion=std::move(clickResult);}
            Mark();
            if(completion)
            {
                RecordClick(owner,success?ClickOutputPhase::completed:resetRequired?ClickOutputPhase::failed:ClickOutputPhase::cancelled,
                    success?"Press and release confirmed; ready for the next matching frame":detail,true);
                try{completion->set_value(success);}catch(...){}
            }
        }
        bool ReleaseClick(bool success,const std::string& reason={})
        {
            bool active{};{std::lock_guard guard(state);active=injectedLeft;}
            if(!active)return true;
            bool written=false;
            for(int i=0;i<3 && !written;++i)written=transport.endClick?transport.endClick(!success):(transport.left && transport.left(false));
            if(!written){Fail(BackendFailure("Injected left-button release write failed"));CompleteClick(false);return false;}
            int held{};
            if(!resetRequired && probed && (!Query(InputQuery::leftState,held) || (held&2)))
            {if(!resetRequired)Fail("Injected left button remained held after release");CompleteClick(false);return false;}
            if(resetRequired){CompleteClick(false);return false;}
            // Do not complete the synchronous caller until the physical mask is restored.
            uncertainLeft=false;
            {std::lock_guard guard(state);injectedLeft=false;}
            bool cancelled{};{std::lock_guard guard(state);cancelled=clickCancelled;}
            const bool ok=Reconcile(Desired());
            const bool completed=!transport.clickSucceeded || transport.clickSucceeded();
            if(success && ok && !cancelled && !completed)
            {Fail(BackendFailure("Backend released the click without confirming playback completion"));CompleteClick(false);return false;}
            CompleteClick(success && ok && !cancelled && completed,reason);return ok;
        }
        void BlindCleanup()
        {
            // No queries on a poisoned session: late numeric replies are unsafe.
            if(transport.cancelAll)transport.cancelAll();
            std::uint32_t guided{};{std::lock_guard guard(state);guided=motionGuidedAxes;}
            const auto mask=applied.load()|uncertain.load()|guided;
            uncertain.fetch_or(mask);Mark();
            if(transport.left)for(int i=0;i<3;++i)if(transport.left(false))break;
            for(const auto bit:{1U,2U,4U,8U,16U})if(mask&bit)
                for(int i=0;i<3;++i)if(transport.lock && transport.lock(bit,false))break;
            {std::lock_guard guard(state);motion=false;motionCancelled=false;motionGuidedAxes=0;}
            CompleteClick(false);draining=false;
            faultCleaned=true;
            // Keep uncertain bits until a new session verifies the cleanup.
        }
        bool CompleteMotionRelease()
        {
            std::string reason;InputOwner owner;
            {std::lock_guard guard(state);owner=movement.owner;reason=releaseReason.empty()?"Correction completed":releaseReason;
                lastReleasedOwner=owner;lastReleaseReason=reason;lastReleaseUs=StreamNowUs();releaseReason.clear();
                if(IsFuse(movement) || transitionFreshRequired)
                {freshAfterUs=lastReleaseUs;if(releaseRequestedUs && lastReleaseUs>=releaseRequestedUs)fuseTransitionTime.Add(double(lastReleaseUs-releaseRequestedUs)/1000.0);}
                motion=false;motionCancelled=false;activationReleased=false;movement={};motionGuidedAxes=0;motionXSign=motionYSign=0;transitionFreshRequired=false;refreshMasks=true;}
            drainStartedUs=nextDrainUs=0;draining=false;++releases;
            const bool clean=Reconcile(Desired());
            log::Info("Input correction released: owner=%u, reason=%s, masks_restored=%s",unsigned(owner),reason.c_str(),clean?"yes":"unconfirmed");
            return clean;
        }
        bool Drain(std::uint64_t now)
        {
            bool fuse{},switching{};{std::lock_guard guard(state);fuse=motion && IsFuse(movement);switching=transitionFreshRequired;}
            if(!drainStartedUs && transport.endMotion)transport.endMotion();
            if(!(applied.load()&(InputLockX|InputLockY)) && !fuse && !switching)
            {
                return CompleteMotionRelease();
            }
            if(!drainStartedUs){drainStartedUs=now;nextDrainUs=now;draining=true;}
            if(now-drainStartedUs>=kDrainUs){++drainTimeouts;Fail("Pending injected movement did not drain within 500 ms");return false;}
            if(now<nextDrainUs)return true;
            int moving{};
            if(!Query(InputQuery::moving,moving))return false;
            if(moving!=0 && moving!=1){Fail("Invalid pending-movement query result");return false;}
            nextDrainUs=StreamNowUs()+5000;
            if(moving)return true;
            if((fuse || switching) && (!transport.clearBearing || !transport.clearBearing()))
            {Fail("Drained Fuse bearing could not be cleared; reconnect required");return false;}
            return CompleteMotionRelease();
        }
        void Service()
        {
            std::lock_guard transaction(io);
            bool active{},finishClick{},cancelMotion{};OutputContext m,c;
            const auto now=StreamNowUs();
            {std::lock_guard guard(state);active=attached;m=movement;c=click;
                finishClick=injectedLeft && (clickCancelled || now>=clickDeadlineUs);
                cancelMotion=motion && (motionCancelled || activationReleased || now-lastMoveUs>=kFreshUs);}
            if(!active)return;
            LogReleaseRequested();
            if(transport.healthy && !transport.healthy()){if(!resetRequired)Fail("MAKCU connection lost while input state was owned");}
            bool protectedMode{};{std::lock_guard guard(state);protectedMode=Protected();}
            helperHealthy=transport.helperReady && transport.helperReady();
            if(protectedMode && probed && !helperHealthy)
                if(!resetRequired)Fail("Input recovery helper exited unexpectedly");
            if(resetRequired){if(!faultCleaned)BlindCleanup();return;}
            if(!probed && protectedMode)return;
            if(cancelAllPending.exchange(false) && transport.cancelAll)transport.cancelAll();
            bool left{},moving{};
            {std::lock_guard guard(state);left=injectedLeft;moving=motion;}
            const auto invalidClick=left?InvalidClickReason(c,now):nullptr;
            if(invalidClick)finishClick=true;
            const auto invalidMotion=moving?InvalidReason(m,now):nullptr;
            if(invalidMotion)cancelMotion=true;
            if(finishClick)
            {
                bool normal{};{std::lock_guard guard(state);normal=!clickCancelled && !invalidClick && now>=clickDeadlineUs;}
                if(!ReleaseClick(normal,invalidClick?invalidClick:"Activation, settings or output session cancelled the click"))return;
            }
            if(cancelMotion)
            {
                bool newlyEnded{};{std::lock_guard guard(state);
                    newlyEnded=RequestEndLocked(invalidMotion?invalidMotion:"No fresh correction within 100 ms");}
                if(newlyEnded)LogReleaseRequested();
                if(!Drain(now))return;
            }
            if(!Reconcile(Desired()))return;
            if((moving || left || draining) && !VerifyMasks(Desired()))return;
            if(moving || left || draining)
            {
                bool report{};InputMaskReadback verified;std::uint32_t expected{};
                {std::lock_guard guard(state);const auto at=StreamNowUs();report=!lastMaskSummaryUs || at-lastMaskSummaryUs>=1000000;
                    if(report){lastMaskSummaryUs=at;verified=firmwareMasks;expected=Blocked(verifiedExpected);}}
                if(report)log::Info("Input lock summary: requested_blocks=%s, last_firmware_blocks=%s, readbacks=%llu, failures=%llu, drifts=%llu",
                    MaskText(expected).c_str(),MaskText(verified.blockedMask).c_str(),
                    static_cast<unsigned long long>(maskReadbacks.load()),static_cast<unsigned long long>(maskReadbackFailures.load()),
                    static_cast<unsigned long long>(maskDrifts.load()));
            }
        }
        void Run(std::stop_token stop)
        {
            while(!stop.stop_requested())
            {
                Service();
                std::unique_lock guard(state);
                auto delay=std::chrono::milliseconds(50);
                if(attached && !resetRequired)
                {
                    const auto now=StreamNowUs();
                    if(motion || injectedLeft || draining)delay=std::chrono::milliseconds(5);
                    if(injectedLeft && clickDeadlineUs>now)
                        delay=std::min(delay,std::chrono::milliseconds(std::max<std::uint64_t>(1,(clickDeadlineUs-now)/1000)));
                }
                changed.wait_for(guard,delay);
            }
        }
    };
    InputLockCoordinator::InputLockCoordinator():state_(std::make_unique<Implementation>())
    {state_->worker=std::jthread([s=state_.get()](std::stop_token stop){s->Run(stop);});}
    InputLockCoordinator::~InputLockCoordinator()
    {Detach();state_->worker.request_stop();state_->changed.notify_all();if(state_->worker.joinable())state_->worker.join();}
    bool InputLockCoordinator::Attach(makcu_device_t* device,const std::string& port,std::string& error)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        s.applied=0;s.uncertain=0;s.requested=0;s.resetRequired=false;s.ready=false;s.draining=false;
        s.drainStartedUs=s.nextDrainUs=0;s.uncertainLeft=false;s.faultCleaned=false;
        {std::lock_guard guard(s.state);s.ResetVerificationLocked();s.releaseReason.clear();s.endNotifier={};
            s.attached=true;s.probed=false;s.motion=false;s.motionCancelled=false;s.injectedLeft=false;s.observedReleased=0;}
        s.transport={
            [device](std::uint32_t bit,bool lock){return WriteMakcuLock(device,bit,lock);},
            [device](InputQuery command,int& value){return QueryMakcuInteger(device,StockQuery(command),value);},
            [device](int x,int y,const OutputContext&){return makcu_mouse_move(device,x,y)==MAKCU_SUCCESS;},
            [device](bool down){return (down?makcu_mouse_down(device,MAKCU_MOUSE_LEFT):makcu_mouse_up(device,MAKCU_MOUSE_LEFT))==MAKCU_SUCCESS;},
            [device]{return makcu_is_connected(device);},
            [&s]{return s.recovery.Ready();},[&s](std::uint32_t mask){s.recovery.Mark(mask);}};
        bool protectedMode{};{std::lock_guard guard(s.state);protectedMode=s.Protected();}
        if(!protectedMode)
        {
            // Explicit opt-out keeps the existing write-only output available.
            // Neutralization writes do not acquire physical-input locks.
            if(!s.transport.left(false)){s.Fail("Initial left release failed");error=Status();return false;}
            for(const auto bit:{1U,2U,4U,8U,16U})if(!s.transport.lock(bit,false))
            {s.Fail("Initial passthrough restoration write failed");error=Status();return false;}
            {std::lock_guard guard(s.state);s.status="Physical-input locks disabled (write-only output; no recovery helper)";}
            s.changed.notify_all();return true;
        }
        if(!s.recovery.Start(port,error))
        {std::lock_guard guard(s.state);s.status="Input locks unavailable: "+error;return false;}
        // Startup restores only Vanta-managed targets, before any feature can arm.
        s.uncertain=InputManagedLocks;s.uncertainLeft=true;s.Mark();
        if(!s.transport.left(false)){s.Fail("Initial injected-button neutralization failed");error=Status();return false;}
        int held{};
        if(!s.Query(InputQuery::leftState,held) || (held&2))
        {if(!s.resetRequired)s.Fail("Initial injected-button release could not be verified");error=Status();return false;}
        s.uncertainLeft=false;
        if(!s.Reconcile(0)){error=Status();return false;}
        int moving{};
        if(!s.Query(InputQuery::moving,moving) || (moving!=0 && moving!=1))
        {if(!s.resetRequired)s.Fail("Pending-movement query is unavailable");error=Status();return false;}
        {std::lock_guard guard(s.state);s.probed=true;s.status="Physical-input locks ready; release and press activation to start";}
        s.ready=true;s.helperHealthy=true;s.changed.notify_all();return true;
    }
    bool InputLockCoordinator::Attach(MediusBackend& device,const std::string& port,std::string& error)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        s.applied=0;s.uncertain=0;s.requested=0;s.resetRequired=false;s.ready=false;s.draining=false;
        s.drainStartedUs=s.nextDrainUs=0;s.uncertainLeft=false;s.faultCleaned=false;
        {std::lock_guard guard(s.state);s.ResetVerificationLocked();s.releaseReason.clear();s.endNotifier=[&device](const OutputContext& ended){device.EndMotion(&ended);};
            s.attached=true;s.probed=false;s.motion=false;s.motionCancelled=false;s.injectedLeft=false;s.observedReleased=0;}
        s.transport={};
        s.transport.lock=[&device](std::uint32_t bit,bool enabled){return device.Lock(bit,enabled);};
        s.transport.query=[&device](InputQuery query,int& value){return device.Query(query,value);};
        s.transport.move=[&device](int x,int y,const OutputContext& context){return device.Move(x,y,context);};
        s.transport.left=[&device](bool down){return down?device.BeginClick(24):device.NeutralizeLeft();};
        s.transport.healthy=[&device]{return device.Healthy();};
        s.transport.helperReady=[&s]{return s.recovery.Ready();};
        s.transport.mark=[&s](std::uint32_t mask){s.recovery.Mark(mask);};
        s.transport.endMotion=[&device]{device.EndMotion();};
        s.transport.cancelAll=[&device]{device.CancelAll();};
        s.transport.beginClick=[&device](int hold,const OutputContext& context){return device.BeginClick(hold,context);};
        s.transport.endClick=[&device](bool cancel){return device.FinishClick(cancel);};
        s.transport.clickSucceeded=[&device]{return device.ClickCompleted();};
        s.transport.failureReason=[&device]{return device.FailureReason();};
        s.transport.readMasks=[&device](InputMaskReadback& result){return device.ReadMasks(result);};
        s.transport.invalidate=[&device](const std::string& reason){device.InvalidateOutput(reason);};
        s.transport.axisPolicy=[&device](std::uint32_t bit,const DirectionalInputScales& policy){return device.SetAxisPolicy(bit,policy);};
        s.transport.prepareFuse=[&device]{return device.PrepareFuse();};
        s.transport.clearBearing=[&device]{return device.ClearBearing();};
        const auto saved=device.RecoverySettings();
        if(!s.recovery.Start(port,error,InputBackend::medius,&saved) || !device.Arm(error))
        {s.Fail("Medius recovery/output preparation failed: "+error);return false;}
        s.uncertain=InputManagedLocks;s.uncertainLeft=true;s.Mark();
        if(!s.transport.left(false) || !s.Reconcile(0)){error="Medius neutralization/readback failed";return false;}
        int held{},moving{};
        if(!s.Query(InputQuery::leftState,held) || (held&2) || !s.Query(InputQuery::moving,moving) || moving)
        {error="Medius output state did not verify idle";return false;}
        s.uncertainLeft=false;s.probed=true;s.ready=true;s.helperHealthy=true;
        {std::lock_guard guard(s.state);s.status="Medius locks verified; release and press activation to start";}
        s.changed.notify_all();return true;
    }
    bool InputLockCoordinator::Detach()
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        bool ending{};{std::lock_guard guard(s.state);if(!s.attached)return s.uncertain==0;
            for(auto& p:s.policies)p.enabled=false;ending=s.RequestEndLocked("Disconnect or shutdown");s.clickCancelled=true;}
        if(ending)s.LogReleaseRequested();
        bool ok=!s.resetRequired;
        if(ok && !s.ReleaseClick(false))ok=false;
        if(ok)
        {
            bool moving{};{std::lock_guard guard(s.state);moving=s.motion;}
            const auto until=StreamNowUs()+kDrainUs;
            while(moving && StreamNowUs()<until && !s.resetRequired)
            {
                if(!s.Drain(StreamNowUs()))break;
                {std::lock_guard guard(s.state);moving=s.motion;}
                if(moving)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if(moving && !s.resetRequired)s.Fail("Disconnect movement-drain deadline exceeded");
            if(!s.resetRequired)ok=s.Reconcile(0);else ok=false;
        }
        if(!ok)s.BlindCleanup();
        {std::lock_guard guard(s.state);++s.connectionEpoch;s.verificationValid=false;s.refreshMasks=true;
            s.endNotifier={};s.attached=false;s.probed=false;s.motion=false;s.movement={};s.click={};}
        s.ready=false;s.helperHealthy=false;s.draining=false;s.transport={};return ok && s.uncertain==0 && !s.uncertainLeft;
    }
    void InputLockCoordinator::FinishRecovery(bool clean){state_->recovery.Finish(clean);}
    void InputLockCoordinator::Configure(const MouseOutputConfig& settings)
    {
        auto& s=*state_;bool ending{};{std::lock_guard guard(s.state);
            const bool wasProtected=s.Protected();
            if(s.settings.lockMovementWhileCorrecting!=settings.lockMovementWhileCorrecting ||
                s.settings.lockGeneratedClick!=settings.lockGeneratedClick ||
                s.settings.reserveActivationSideButtons!=settings.reserveActivationSideButtons)
            {ending=s.RequestEndLocked("Mouse Output lock settings changed");s.clickCancelled=true;}
            s.settings=settings;
            if(s.attached && !wasProtected && s.Protected() && !s.probed)
            {s.status="Reconnect required to initialize input-lock verification and recovery";s.resetRequired=true;}}
        if(ending)s.LogReleaseRequested();s.changed.notify_all();
    }
    void InputLockCoordinator::SetPolicy(InputOwner owner,bool enabled,std::uint64_t generation,MouseButtonBinding key)
    {
        if(owner==InputOwner::manual)return;
        auto& s=*state_;bool different{},ending{};
        {std::lock_guard guard(s.state);auto& p=s.policies[Index(owner)];
            if(generation<p.generation)return;
            if(enabled && s.emergencyGenerations[Index(owner)]==generation)enabled=false;
            different=p.enabled!=enabled || p.generation!=generation || p.button!=key;
            if(different){p={enabled,generation,key};
                if(s.motion && (s.movement.owner==owner || (owner==InputOwner::python && enabled)))
                    ending=s.RequestEndLocked(!enabled?"Feature disabled":s.movement.owner!=owner?"Movement ownership changed":"Feature settings or activation binding changed");
                if(s.injectedLeft && s.click.owner==owner)s.clickCancelled=true;}}
        if(ending)s.LogReleaseRequested();if(different)s.changed.notify_all();
    }
    void InputLockCoordinator::PhysicalButtons(std::uint8_t mask,PhysicalButtonEdge edge) noexcept
    {
        auto& s=*state_;mask&=0x1FU;{std::lock_guard guard(s.state);
            std::uint8_t proof{};
            if(s.attached && s.settings.backend==InputBackend::medius && edge==PhysicalButtonEdge::press)
            {
                const auto newlyHeld=static_cast<std::uint8_t>(mask&~s.physical);
                // Known-up buttons followed by a native positive edge prove a
                // fresh press. With no initial state, only a single candidate is
                // unambiguous: POS says the held set grew, so that button was up.
                // Multiple unknown held buttons must still be released first.
                proof=static_cast<std::uint8_t>(newlyHeld&s.physicalKnown);
                if(std::has_single_bit(unsigned(newlyHeld)))proof|=newlyHeld;
            }
            // This is a complete physical snapshot, including released buttons.
            s.observedReleased|=(static_cast<std::uint8_t>(~mask)&0x1FU)|proof;
            s.physical=mask;s.physicalKnown=0x1FU;s.lastNativePressProof=proof;
            s.lastPhysicalEdge=edge;s.lastPhysicalUs=StreamNowUs();++s.physicalSnapshots;
            s.nativePressProofs+=std::popcount(unsigned(proof));
            if(s.motion && !(mask&Button(s.movement.activation)))s.activationReleased=true;
            if(s.injectedLeft && s.click.owner!=InputOwner::manual && !(mask&Button(s.click.activation)))s.clickCancelled=true;}
        s.changed.notify_all();
    }
    void InputLockCoordinator::PhysicalButton(MouseButtonBinding button,bool pressed) noexcept
    {
        auto& s=*state_;const auto bit=Button(button);
        {std::lock_guard guard(s.state);s.physicalKnown|=bit;
            s.lastPhysicalEdge=pressed?PhysicalButtonEdge::press:PhysicalButtonEdge::release;
            s.lastPhysicalUs=StreamNowUs();s.lastNativePressProof=0;
            if(pressed)s.physical|=bit;
            else {s.physical&=static_cast<std::uint8_t>(~bit);s.observedReleased|=bit;
                if(s.motion && s.movement.activation==button)s.activationReleased=true;
                if(s.injectedLeft && s.click.owner!=InputOwner::manual && s.click.activation==button)s.clickCancelled=true;}}
        s.changed.notify_all();
    }
    bool InputLockCoordinator::TryMove(int x,int y,const OutputContext& c)
    {
        auto& s=*state_;
        if(x==0 && y==0){EndCorrection(c.owner,c.generation,"Calculated correction is zero");return s.Block(c.owner,"Calculated correction is zero");}
        if(c.physicalInputMode!=PythonPhysicalInputMode::existing && c.physicalInputMode!=PythonPhysicalInputMode::fuse)
            return s.Block(c.owner,"Unknown physical input policy");
        if(x<-32768 || x>32767 || y<-32768 || y>32767)return s.Block(c.owner,"Correction exceeds signed 16-bit output limits");
        if((x && !(c.axes&InputLockX)) || (y && !(c.axes&InputLockY)))return s.Block(c.owner,"Correction includes a disabled axis");
        const auto waiting=StreamNowUs(),emergency=s.emergencyRevision.load();std::lock_guard transaction(s.io);
        s.transactionWait.Add(double(StreamNowUs()-waiting)/1000.0);
        bool protectedMode{},active{},cancelled{},differentPolicy{};OutputContext old;
        {std::lock_guard guard(s.state);
            if(!s.attached)return s.BlockUnlocked(c.owner,"Mouse output transport is not attached");
            if(s.resetRequired)return s.BlockUnlocked(c.owner,"Mouse output recovery is required");
            protectedMode=s.Protected();active=s.motion;cancelled=s.motionCancelled;old=s.movement;
            differentPolicy=PolicyChanged(old,c,s.motionXSign,s.motionYSign,x,y);
            if(active && (IsFuse(old) || IsFuse(c)) && (cancelled || differentPolicy || !SameSession(old,c)))
                s.transitionFreshRequired=true;
            if(IsFuse(c) && (c.owner!=InputOwner::python || s.settings.backend!=InputBackend::medius ||
                !s.transport.axisPolicy || !s.transport.prepareFuse || !s.transport.clearBearing))
                return s.BlockUnlocked(c.owner,"Fuse inputs requires the verified Medius backend and Python ownership");
            if(IsFuse(c) && (!c.frameTimeUs || (s.freshAfterUs && c.frameTimeUs<=s.freshAfterUs)))
                return s.BlockUnlocked(c.owner,"Fuse inputs is waiting for a new source observation after draining");}
        if(protectedMode && !s.ready)return s.Block(c.owner,"Input mask verification is not ready");
        if(protectedMode && (!s.transport.helperReady || !s.transport.helperReady()))return s.Block(c.owner,"Input recovery helper is not ready");
        const auto invalid=s.InvalidReason(c,StreamNowUs());
        if(emergency!=s.emergencyRevision.load() || invalid)
        {++s.staleRejections;EndCorrection(c.owner,c.generation);return s.Block(c.owner,invalid?invalid:"Emergency release cancelled the correction");}
        if(active && (cancelled || !SameSession(old,c) || differentPolicy))
        {
            bool ending{};{std::lock_guard guard(s.state);
                s.transitionFreshRequired=s.transitionFreshRequired || IsFuse(old) || IsFuse(c);
                ending=s.RequestEndLocked(differentPolicy?
                    "Direction, guided axes or input mode changed; drain before switching":"Correction owner or stream generation changed");}
            if(ending)s.LogReleaseRequested();s.changed.notify_all();
            return s.Block(c.owner,differentPolicy?"Waiting for the previous direction and axes to drain before switching":"Waiting for the previous correction session to drain");
        }
        {std::lock_guard guard(s.state);s.motion=true;s.motionCancelled=false;s.movement=c;s.lastMoveUs=StreamNowUs();
            if(!active){s.refreshMasks=true;s.releaseReason.clear();}
            s.motionMask=!IsFuse(c) && s.settings.lockMovementWhileCorrecting?c.axes:0U;
            s.motionGuidedAxes=IsFuse(c)?GuidedAxes(x,y,c.axes):0U;
            s.motionXSign=Sign(x);s.motionYSign=Sign(y);}
        if(!s.Reconcile(s.Desired()) || !s.VerifyMasks(s.Desired()))
        {EndCorrection(c.owner,c.generation);return s.Block(c.owner,"Physical input mask verification failed; check Mouse Output recovery status");}
        const auto changed=s.InvalidReason(c,StreamNowUs());
        if(emergency!=s.emergencyRevision.load() || changed)
        {EndCorrection(c.owner,c.generation);return s.Block(c.owner,changed?changed:"Emergency release cancelled the correction");}
        const auto started=StreamNowUs();const auto checked=s.Checked(c,emergency);
        const bool sent=s.transport.move(x,y,checked);
        s.moveSerial.Add(double(StreamNowUs()-started)/1000.0);
        if(!sent)
        {
            if(const auto reason=s.InvalidReason(c,StreamNowUs()))
            {EndCorrection(c.owner,c.generation);return s.Block(c.owner,reason);}
            if(emergency!=s.emergencyRevision.load())
            {EndCorrection(c.owner,c.generation);return s.Block(c.owner,"Emergency release cancelled the correction");}
            s.Fail("Movement command write failed");return s.Block(c.owner,"Backend rejected the correction; check Mouse Output's last output fault");
        }
        {std::lock_guard guard(s.state);s.movementBlocked[Index(c.owner)]=false;}
        if(c.owner==InputOwner::manual)EndCorrection(c.owner,c.generation);
        s.changed.notify_all();return true;
    }
    bool InputLockCoordinator::TryClick(const OutputContext& c,int hold)
    {
        auto& s=*state_;const auto emergency=s.emergencyRevision.load();auto completion=std::make_shared<std::promise<bool>>();auto result=completion->get_future();
        {std::lock_guard guard(s.state);++s.clickStatus[Index(c.owner)].attempts;}
        {
            std::lock_guard transaction(s.io);bool protectedMode{};std::string blocked;
            {std::lock_guard guard(s.state);
                if(!s.attached)blocked="Mouse output is disconnected";
                else if(s.resetRequired)blocked="Recovery required: "+s.status;
                else if(s.injectedLeft)blocked="Another generated click is still completing";
                else if(s.settings.backend==InputBackend::medius && (s.draining || s.motionCancelled))
                    blocked="Waiting for the previous movement correction to drain";
                protectedMode=s.Protected();}
            if(!blocked.empty())return s.BlockClick(c.owner,blocked);
            if(protectedMode && (!s.ready || !s.transport.helperReady || !s.transport.helperReady()))
                return s.BlockClick(c.owner,"Input-lock verification or crash-recovery helper is not ready; reconnect Mouse Output");
            if(emergency!=s.emergencyRevision.load())return s.BlockClick(c.owner,"Emergency release cancelled this click request");
            if(const auto reason=s.InvalidReason(c,StreamNowUs())){++s.staleRejections;return s.BlockClick(c.owner,reason);}
            {std::lock_guard guard(s.state);s.injectedLeft=true;s.clickCancelled=false;s.click=c;s.clickResult=completion;
                s.clickMask=s.settings.lockGeneratedClick?InputLockLeft:0U;
                s.clickDeadlineUs=StreamNowUs()+static_cast<std::uint64_t>(std::clamp(hold,14,34))*1000;}
            s.RecordClick(c.owner,ClickOutputPhase::preparing,"Verifying the physical left-button mask");
            s.uncertainLeft=true;s.Mark();
            if(!s.Reconcile(s.Desired()) || !s.VerifyMasks(s.Desired()))
            {s.ReleaseClick(false);return false;}
            const auto invalid=s.InvalidClickReason(c,StreamNowUs());
            if(emergency!=s.emergencyRevision.load() || invalid)
            {s.ReleaseClick(false,invalid?invalid:"Emergency release cancelled the prepared click");return false;}
            const auto checked=s.Checked(c,emergency,true);
            if(!(s.transport.beginClick?s.transport.beginClick(hold,checked):(s.transport.left && s.transport.left(true))))
            {
                // Losing activation/freshness during preparation is cancellation,
                // not a failed serial link. Clean up and accept a new fresh frame.
                const auto cancelled=s.InvalidClickReason(c,StreamNowUs());
                const bool healthy=!s.transport.healthy || s.transport.healthy();
                if(healthy && (cancelled || emergency!=s.emergencyRevision.load()))
                {s.ReleaseClick(false,cancelled?cancelled:"Emergency release cancelled the prepared click");return false;}
                s.Fail(s.BackendFailure("Injected left-button press failed"));s.BlindCleanup();return false;
            }
            {std::lock_guard guard(s.state);s.clickDeadlineUs=StreamNowUs()+static_cast<std::uint64_t>(std::clamp(hold,14,34))*1000;
                ++s.clickStatus[Index(c.owner)].started;}
            s.RecordClick(c.owner,ClickOutputPhase::playing,"Waiting for firmware press/release completion");
        }
        s.changed.notify_all();
        // Synchronous completion is preserved, without holding either device mutex.
        if(result.wait_for(std::chrono::milliseconds(750))!=std::future_status::ready)
        {
            std::lock_guard transaction(s.io);bool stillOwned{};
            {std::lock_guard guard(s.state);stillOwned=s.clickResult==completion;}
            if(!stillOwned)return result.wait_for(std::chrono::milliseconds(0))==std::future_status::ready && result.get();
            s.Fail("Generated click completion exceeded 750 ms; reconnect required");s.BlindCleanup();return false;
        }
        return result.get();
    }
    void InputLockCoordinator::EndCorrection(InputOwner owner,std::uint64_t generation,const char* reason)
    {
        auto& s=*state_;std::function<void(const OutputContext&)> notify;OutputContext ended;bool ending{};
        {std::lock_guard guard(s.state);if(s.motion && s.movement.owner==owner && s.movement.generation==generation)
            {ending=s.RequestEndLocked(reason);notify=s.endNotifier;ended=s.movement;}}
        if(ending)s.LogReleaseRequested();
        if(notify)notify(ended); // Stop generation immediately; drain already queued motion normally.
        s.changed.notify_all();
    }
    void InputLockCoordinator::CancelOwner(InputOwner owner)
    {
        auto& s=*state_;std::lock_guard transaction(s.io);bool ending{};
        {std::lock_guard guard(s.state);s.policies[Index(owner)].enabled=false;
            if(s.motion && s.movement.owner==owner){ending=s.RequestEndLocked("Feature output cancelled");s.movement.isCurrent={};}
            if(s.injectedLeft && s.click.owner==owner){s.clickCancelled=true;s.click.isCurrent={};}}
        if(ending)s.LogReleaseRequested();s.changed.notify_all();
    }
    void InputLockCoordinator::EmergencyRelease()
    {
        auto& s=*state_;bool ending{};{std::lock_guard guard(s.state);for(std::size_t i=0;i<s.policies.size();++i)
            {s.emergencyGenerations[i]=s.policies[i].generation;s.policies[i].enabled=false;}
            ending=s.RequestEndLocked("Emergency release");s.clickCancelled=true;s.observedReleased=0;}
        if(ending)s.LogReleaseRequested();
        ++s.emergencyRevision;s.cancelAllPending=true;s.changed.notify_all();
    }
    bool InputLockCoordinator::CancelClick()
    {
        auto& s=*state_;std::lock_guard transaction(s.io);
        {std::lock_guard guard(s.state);s.clickCancelled=true;}
        return s.ReleaseClick(false);
    }
    bool InputLockCoordinator::ResetRequired() const noexcept{return state_->resetRequired;}
    std::uint64_t InputLockCoordinator::EmergencyRevision() const noexcept{return state_->emergencyRevision;}
    std::string InputLockCoordinator::MovementBlockReason(InputOwner owner) const
    {std::lock_guard guard(state_->state);return state_->movementBlocked[Index(owner)]?state_->movementBlocks[Index(owner)]:std::string{};}
    ClickOutputStatus InputLockCoordinator::ClickStatus(InputOwner owner) const
    {std::lock_guard guard(state_->state);return state_->clickStatus[Index(owner)];}
    std::string InputLockCoordinator::Status() const
    {
        const auto& s=*state_;std::lock_guard guard(s.state);std::ostringstream out;
        const auto now=StreamNowUs();
        out<<s.status<<"\nRequested: "<<MaskText(s.requested)<<" | Applied after last transition: "<<MaskText(s.applied)
            <<"\nLast firmware readback: ";
        if(!s.firmwareMasks.readAtUs)out<<"not available";
        else
        {
            out<<MaskText(s.firmwareMasks.blockedMask)<<" | age: "
                <<(now>=s.firmwareMasks.readAtUs?(now-s.firmwareMasks.readAtUs)/1000:0)<<" ms";
            if(!s.verificationValid || s.verificationEpoch!=s.connectionEpoch)out<<" | unconfirmed for this connection";
            else if(s.settings.backend==InputBackend::medius && now>=s.firmwareMasks.readAtUs && now-s.firmwareMasks.readAtUs>=kMaskVerifyUs)
                out<<" | due for refresh when correcting";
            if(s.firmwareMasks.knownMask!=InputManagedLocks)out<<" | partial readback";
        }
        constexpr const char* names[]{"manual","Python","TestMove","TestClick"};
        out<<"\nMovement: "<<(s.motion?names[Index(s.movement.owner)]:"none")
           <<" | Helper: "<<(s.helperHealthy?"ready":"offline");
        if(s.draining)out<<" | Draining";if(s.uncertain || s.uncertainLeft)out<<" | Recovery unconfirmed";
        if(s.motion && IsFuse(s.movement))
        {
            out<<"\nFuse inputs: ";
            if(s.motionGuidedAxes&InputLockX)out<<"X "<<(s.motionXSign<0?"left":"right")<<" guided; ";else out<<"X manual; ";
            if(s.motionGuidedAxes&InputLockY)out<<"Y "<<(s.motionYSign<0?"up":"down")<<" guided";else out<<"Y manual";
            out<<" | with 100%, against 0%";
            if(s.motionCancelled || s.draining)out<<" | Transition: draining old direction";
        }
        else if(!s.settings.lockMovementWhileCorrecting)out<<" | Movement locking DISABLED";
        if(s.firmwareMasks.bearingKnown)out<<"\nBearing readback: "<<s.firmwareMasks.bearingWindowMs<<" ms / "
            <<(s.firmwareMasks.bearingMode==0?"per axis":"vector");
        if(!s.verificationError.empty())out<<"\nMask verification: "<<s.verificationError;
        if(s.injectedLeft)out<<"\nClick: "<<names[Index(s.click.owner)]<<" | "<<s.clickStatus[Index(s.click.owner)].message;
        if(!s.releaseReason.empty())out<<"\nEnding: "<<s.releaseReason;
        else if(!s.lastReleaseReason.empty())out<<"\nLast correction release: "<<s.lastReleaseReason;
        out<<" | Enabled owners:";bool owners=false;
        for(std::size_t i=1;i<s.policies.size();++i)if(s.policies[i].enabled)
        {out<<' '<<names[i];owners=true;}
        if(!owners)out<<" none";return out.str();
    }
    std::string InputLockCoordinator::DiagnosticsJson() const
    {
        const auto& s=*state_;const auto transaction=s.transactionWait.Get(),query=s.firmwareQuery.Get(),transition=s.maskTransition.Get(),serial=s.moveSerial.Get();
        const auto maskQuery=s.maskReadbackTime.Get(),fuseTransition=s.fuseTransitionTime.Get();
        std::lock_guard guard(s.state);std::ostringstream out;const auto now=StreamNowUs();
        out<<"{\"ready\":"<<(s.ready?"true":"false")<<",\"draining\":"<<(s.draining?"true":"false")
            <<",\"helper_ready\":"<<(s.helperHealthy?"true":"false")
            <<",\"injected_left_release_uncertain\":"<<(s.uncertainLeft?"true":"false")
            <<",\"physical_button_mask\":"<<static_cast<unsigned>(s.physical)
            <<",\"physical_button_known_mask\":"<<unsigned(s.physicalKnown)
            <<",\"observed_released_mask\":"<<static_cast<unsigned>(s.observedReleased)
            <<",\"physical_button_snapshots\":"<<s.physicalSnapshots<<",\"last_physical_button_at_local_us\":"<<s.lastPhysicalUs
            <<",\"last_physical_button_edge\":"<<unsigned(s.lastPhysicalEdge)
            <<",\"last_native_press_release_proof_mask\":"<<unsigned(s.lastNativePressProof)
            <<",\"native_press_release_proofs\":"<<s.nativePressProofs
            <<",\"reset_required\":"<<(s.resetRequired?"true":"false")<<",\"requested_mask\":"<<s.requested.load()
            <<",\"applied_mask\":"<<s.applied.load()<<",\"uncertain_mask\":"<<s.uncertain.load()
            <<",\"lock_movement_while_correcting\":"<<(s.settings.lockMovementWhileCorrecting?"true":"false")
            <<",\"lock_generated_click\":"<<(s.settings.lockGeneratedClick?"true":"false")
            <<",\"reserve_activation_side_buttons\":"<<(s.settings.reserveActivationSideButtons?"true":"false")
            <<",\"firmware_mask_known\":"<<s.firmwareMasks.knownMask<<",\"firmware_blocked_mask\":"<<s.firmwareMasks.blockedMask
            <<",\"firmware_passthrough_mask\":"<<s.firmwareMasks.passthroughMask<<",\"firmware_mask_read_at_us\":"<<s.firmwareMasks.readAtUs
            <<",\"firmware_mask_age_us\":";
        if(s.firmwareMasks.readAtUs && now>=s.firmwareMasks.readAtUs)out<<now-s.firmwareMasks.readAtUs;else out<<"null";
        out<<",\"firmware_mask_verified\":"<<(s.verificationValid && s.verificationEpoch==s.connectionEpoch?"true":"false")
            <<",\"firmware_mask_fresh\":"<<(s.verificationValid && s.verificationEpoch==s.connectionEpoch && s.firmwareMasks.readAtUs &&
                now>=s.firmwareMasks.readAtUs && now-s.firmwareMasks.readAtUs<kMaskVerifyUs?"true":"false")
            <<",\"mask_readback_mode\":"<<Escape(s.settings.backend==InputBackend::medius?"one firmware lock-table query plus bearing option query":"stock per-command readback")
            <<",\"physical_input_mode\":"<<Escape(PythonPhysicalInputModeJsonName(s.movement.physicalInputMode))
            <<",\"guided_axes\":"<<s.motionGuidedAxes<<",\"guided_x_direction\":"<<s.motionXSign<<",\"guided_y_direction\":"<<s.motionYSign
            <<",\"fresh_observation_after_local_us\":"<<s.freshAfterUs
            <<",\"bearing_required\":"<<(s.requestedPolicy.bearingRequired?"true":"false")
            <<",\"required_bearing_window_ms\":"<<kFuseBearingWindowMs<<",\"required_bearing_mode\":\"per_axis\""
            <<",\"bearing_readback_known\":"<<(s.firmwareMasks.bearingKnown?"true":"false")
            <<",\"bearing_readback_window_ms\":"<<s.firmwareMasks.bearingWindowMs<<",\"bearing_readback_mode\":"<<unsigned(s.firmwareMasks.bearingMode)
            <<",\"connection_epoch\":"<<s.connectionEpoch<<",\"mask_readback_epoch\":"<<s.verificationEpoch
            <<",\"mask_verification_interval_us\":"<<kMaskVerifyUs<<",\"mask_verification_error\":"<<Escape(s.verificationError)
            <<",\"mask_readbacks\":"<<s.maskReadbacks.load()<<",\"mask_readback_failures\":"<<s.maskReadbackFailures.load()
            <<",\"mask_drift_events\":"<<s.maskDrifts.load()<<",\"release_requested_reason\":"<<Escape(s.releaseReason)
            <<",\"release_requested_at_us\":"<<s.releaseRequestedUs<<",\"last_release_reason\":"<<Escape(s.lastReleaseReason)
            <<",\"last_release_at_us\":"<<s.lastReleaseUs<<",\"last_released_owner\":"<<unsigned(s.lastReleasedOwner)
            <<",\"movement_owner\":"<<static_cast<int>(s.movement.owner)<<",\"movement_active\":"<<(s.motion?"true":"false")
            <<",\"movement_configuration_generation\":"<<s.movement.generation<<",\"movement_stream_generation\":"<<s.movement.streamGeneration
            <<",\"movement_stream_session\":"<<s.movement.streamSession<<",\"movement_observation_sequence\":"<<s.movement.sequence
            <<",\"movement_observation_local_us\":"<<s.movement.frameTimeUs
            <<",\"click_active\":"<<(s.injectedLeft?"true":"false")<<",\"status\":"<<Escape(s.status)
            <<",\"lock_transitions\":"<<s.transitions.load()<<",\"failures\":"<<s.failures.load()
            <<",\"completed_movement_releases\":"<<s.releases.load()<<",\"stale_rejections\":"<<s.staleRejections.load()
            <<",\"drain_timeouts\":"<<s.drainTimeouts.load()
            <<",\"transaction_wait_p95_ms\":"<<transaction.p95Ms
            <<",\"firmware_query_p95_ms\":"<<query.p95Ms
            <<",\"mask_transition_p95_ms\":"<<transition.p95Ms
            <<",\"movement_serial_write_p95_ms\":"<<serial.p95Ms
            <<",\"mask_readback_timing\":{\"median_ms\":"<<maskQuery.medianMs<<",\"p95_ms\":"<<maskQuery.p95Ms
            <<",\"p99_ms\":"<<maskQuery.p99Ms<<",\"samples\":"<<maskQuery.samples<<'}'
            <<",\"fuse_transition_request_to_drain_completion\":{\"median_ms\":"<<fuseTransition.medianMs<<",\"p95_ms\":"<<fuseTransition.p95Ms
            <<",\"p99_ms\":"<<fuseTransition.p99Ms<<",\"samples\":"<<fuseTransition.samples<<"},\"firmware_scales\":[";
        for(std::size_t i=0;i<kManagedInputBits.size();++i)
        {if(i)out<<',';out<<"{\"input_mask\":"<<kManagedInputBits[i]<<",\"positive\":"<<s.firmwareMasks.positiveScale[i]
            <<",\"negative\":"<<s.firmwareMasks.negativeScale[i]<<",\"with\":"<<s.firmwareMasks.withScale[i]
            <<",\"against\":"<<s.firmwareMasks.againstScale[i]<<'}';}
        out<<"],\"requested_scales\":[";
        for(std::size_t i=0;i<kManagedInputBits.size();++i)
        {if(i)out<<',';const auto& wanted=s.requestedPolicy.scales[i];out<<"{\"input_mask\":"<<kManagedInputBits[i]
            <<",\"positive\":"<<wanted.positive<<",\"negative\":"<<wanted.negative<<",\"with\":"<<wanted.with<<",\"against\":"<<wanted.against<<'}';}
        out<<"],\"human_contribution_measurement\":\"not available; firmware merges native input without a delivered contribution acknowledgement\",\"owners\":[";
        for(std::size_t i=0;i<s.policies.size();++i){if(i)out<<',';const auto& p=s.policies[i];
            out<<"{\"owner\":"<<i<<",\"enabled\":"<<(p.enabled?"true":"false")<<",\"generation\":"<<p.generation
               <<",\"activation\":"<<static_cast<int>(p.button)<<",\"output_rejections\":"<<s.movementBlockCounts[i]
               <<",\"last_output_check_rejected\":"<<(s.movementBlocked[i]?"true":"false")
               <<",\"last_output_block_local_us\":"<<s.movementBlockAt[i]
               <<",\"last_output_block_reason\":"<<Escape(s.movementBlocks[i]);
            const auto& click=s.clickStatus[i];
            out<<",\"click_output\":{\"phase\":"<<int(click.phase)<<",\"attempts\":"<<click.attempts<<",\"started\":"<<click.started
                <<",\"completed\":"<<click.completed<<",\"rejected\":"<<click.rejected<<",\"cancelled\":"<<click.cancelled
                <<",\"failed\":"<<click.failed<<",\"updated_at_local_us\":"<<click.updatedAtUs<<",\"message\":"<<Escape(click.message)<<"}}";}
        return out.str()+"]}";
    }
    bool InputLockCoordinator::RunSelfTest()
    {
        // Opt-in deterministic checks: no devices, child processes, windows or GPU.
        std::array<Policy,4> policies{};policies[1]={true,1,MouseButtonBinding::x1};policies[2]={true,2,MouseButtonBinding::x1};
        if(Reserved(policies)!=InputLockSide1)return false;
        policies[1].enabled=false;if(Reserved(policies)!=InputLockSide1)return false;
        policies[2].button=MouseButtonBinding::x2;if(Reserved(policies)!=InputLockSide2)return false;
        OutputContext a,b;a.owner=b.owner=InputOwner::python;a.generation=b.generation=4;a.streamGeneration=1;b.streamGeneration=2;
        if(SameSession(a,b))return false;
        b.streamGeneration=1;if(!SameSession(a,b))return false;
        Implementation cancelledSession;OutputContext manual;
        const auto checked=cancelledSession.Checked(manual,cancelledSession.emergencyRevision.load());
        if(!checked.isCurrent())return false;++cancelledSession.emergencyRevision;
        if(checked.isCurrent())return false; // A queued manual test must not survive emergency release.
        Implementation mandatory;mandatory.settings.backend=InputBackend::medius;
        mandatory.settings.lockMovementWhileCorrecting=mandatory.settings.lockGeneratedClick=mandatory.settings.reserveActivationSideButtons=false;
        if(!mandatory.Protected())return false; // Medius still owns saved options and clip state.
        // A standalone state with an injected transport exercises the production
        // reconciliation/cleanup paths. No maintenance thread is started.
        Implementation s;std::vector<std::string> events;std::uint32_t hardware{};bool failQuery{};
        s.transport.lock=[&](std::uint32_t bit,bool on){events.push_back(on?"lock":"unlock");if(on)hardware|=bit;else hardware&=~bit;return true;};
        s.transport.query=[&](InputQuery command,int& value){events.push_back("query");
            if(failQuery)return false;value=0;for(const auto bit:{1U,2U,4U,8U,16U})if(command==LockQuery(bit))value=(hardware&bit)?1:0;return true;};
        s.transport.left=[&](bool down){events.push_back(down?"down":"up");return true;};
        s.transport.mark=[&](std::uint32_t){events.push_back("mark");};
        if(!s.Reconcile(InputLockX) || hardware!=InputLockX || events.size()<3 || events[0]!="mark" || events[1]!="lock" || events[2]!="query")return false;
        const auto count=events.size();if(!s.Reconcile(InputLockX) || events.size()!=count)return false;
        failQuery=true;if(s.Reconcile(InputLockX|InputLockY) || !s.resetRequired || !(s.uncertain&InputLockY))return false;
        const auto queries=std::count(events.begin(),events.end(),"query");s.BlindCleanup();
        if(std::count(events.begin(),events.end(),"query")!=queries || hardware!=0)return false;
        if(!RecoverySessionMatches(42,42) || RecoverySessionMatches(42,43) || RecoverySessionMatches(0,0))return false;
        OutputContext stale;stale.owner=InputOwner::manual;stale.frameTimeUs=1;
        if(s.Valid(stale,kFreshUs+2))return false;
        stale.frameTimeUs=0;stale.isCurrent=[]{return false;};if(s.Valid(stale,2))return false;
        stale.isCurrent={};stale.owner=InputOwner::python;stale.generation=7;stale.activation=MouseButtonBinding::x1;
        s.policies[1]={true,8,MouseButtonBinding::x1};s.physical=Button(MouseButtonBinding::x1);s.observedReleased=s.physical;
        if(s.Valid(stale,2))return false;
        s.policies[1].generation=7;if(!s.Valid(stale,2))return false;
        // A cancelled generated click releases injection before unmasking left.
        failQuery=false;s.resetRequired=false;s.probed=true;s.uncertain=0;s.applied=InputLockLeft;
        s.motion=false;s.injectedLeft=true;s.clickMask=InputLockLeft;s.uncertainLeft=true;hardware=InputLockLeft;s.policies={};events.clear();
        s.settings.lockGeneratedClick=false;if(!(s.Desired()&InputLockLeft))return false;
        auto completed=std::make_shared<std::promise<bool>>();auto result=completed->get_future();s.clickResult=completed;
        if(!s.ReleaseClick(false) || result.get() || hardware || s.uncertainLeft)return false;
        const auto up=std::find(events.begin(),events.end(),"up"),unlock=std::find(events.begin(),events.end(),"unlock");
        if(up==events.end() || unlock==events.end() || up>=unlock)return false;
        // Timeout must retain uncertainty and must not claim physical delivery.
        s.applied=InputLockX;s.uncertain=0;
        s.resetRequired=false;s.attached=true;s.motion=true;s.motionCancelled=true;s.movement=a;
        s.drainStartedUs=StreamNowUs()-kDrainUs-1;if(s.Drain(StreamNowUs()) || !s.resetRequired)return false;
        // Native table readback exercises the real transition/drain/verification
        // code without a device, recovery child, UI or background worker.
        struct FakeMasks
        {
            std::uint32_t mask{};int reads{},pendingQueries{};bool timeout{},badTimestamp{},partial{},failY{},invalidated{},pending{},click{};
            bool directional{},prepared{},failPreparation{},failAxisAfterFixed{},wrongBearing{},failBearingClear{};
            std::array<DirectionalInputScales,2> axisScales{};
            std::vector<std::pair<int,int>> emitted;
            std::vector<OutputContext> emittedContexts;
            std::function<void()> beforeRead;
            std::uint32_t recoveryMarked{};
            std::uint64_t now{1000000};
            std::vector<std::string> events;
            InputMaskReadback Snapshot() const
            {
                InputMaskReadback value;value.knownMask=InputManagedLocks;value.blockedMask=mask;
                value.passthroughMask=InputManagedLocks&~mask;value.readAtUs=now;
                for(std::size_t i=0;i<kManagedInputBits.size();++i)
                    value.positiveScale[i]=value.negativeScale[i]=(mask&kManagedInputBits[i])?0:100;
                if(directional)
                {
                    value.bearingKnown=true;value.bearingWindowMs=prepared?kFuseBearingWindowMs:20;value.bearingMode=wrongBearing?1:0;
                    for(std::size_t i=0;i<2;++i)
                    {
                        const auto bit=kManagedInputBits[i];const auto& scale=axisScales[i];
                        value.blockedMask&=~bit;value.passthroughMask&=~bit;
                        value.positiveScale[i]=scale.positive;value.negativeScale[i]=scale.negative;
                        value.withScale[i]=scale.with;value.againstScale[i]=scale.against;
                        if(scale.positive==0 && scale.negative==0)value.blockedMask|=bit;
                        if(scale==DirectionalInputScales{})value.passthroughMask|=bit;
                    }
                }
                return value;
            }
            void Attach(Implementation& target)
            {
                target.ResetVerificationLocked();
                target.maskClock=[this]{return now;};
                target.transport.lock=[this](std::uint32_t bit,bool on)
                {events.push_back(std::string(on?"lock ":"unlock ")+std::to_string(bit));
                    if(on && bit==InputLockY && failY)return false;if(on)mask|=bit;else mask&=~bit;
                    if(directional && (bit==InputLockX || bit==InputLockY))
                    {auto& scale=axisScales[bit==InputLockX?0:1];scale={};if(on)scale.positive=scale.negative=0;}return true;};
                target.transport.readMasks=[this](InputMaskReadback& value)
                {events.push_back("read masks");++reads;if(beforeRead){auto callback=std::move(beforeRead);callback();}if(timeout)return false;value=Snapshot();
                    if(badTimestamp)value.readAtUs=1;
                    if(partial){value.negativeScale[0]=50;value.blockedMask&=~InputLockX;value.passthroughMask&=~InputLockX;}return true;};
                target.transport.query=[this](InputQuery command,int& value)
                {if(command==InputQuery::moving){++pendingQueries;value=(pending || click)?1:0;events.push_back("pending motion");return true;}
                    if(command==InputQuery::leftState){value=click?2:0;return true;}return false;};
                target.transport.invalidate=[this](const std::string&){events.push_back("invalidate");invalidated=true;};
                target.transport.cancelAll=[this]{events.push_back("cancel");pending=click=false;};
                target.transport.left=[this](bool down){events.push_back(down?"down":"up");click=down;return true;};
                target.transport.endMotion=[this]{events.push_back("end motion");};
                target.transport.mark=[this](std::uint32_t value){recoveryMarked=value;};
            }
            void EnableFuse(Implementation& target)
            {
                directional=true;
                target.transport.prepareFuse=[this]
                {if(failPreparation)return false;if(!prepared){events.push_back("prepare bearing");prepared=true;}return true;};
                target.transport.axisPolicy=[this](std::uint32_t bit,const DirectionalInputScales& scale)
                {
                    events.push_back("axis policy "+std::to_string(bit));auto& actual=axisScales[bit==InputLockX?0:1];
                    actual={};actual.positive=actual.negative=scale.positive;
                    if(failAxisAfterFixed)return false;
                    actual=scale;if(scale.positive==0 && scale.negative==0)mask|=bit;else mask&=~bit;return true;
                };
                target.transport.clearBearing=[this]{events.push_back("clear bearing");return !failBearingClear;};
                target.transport.move=[this](int x,int y,const OutputContext& context)
                {if(context.isCurrent && !context.isCurrent())return false;events.push_back("move");pending=true;
                    emitted.emplace_back(x,y);emittedContexts.push_back(context);return true;};
                target.transport.healthy=[this]{return !invalidated;};target.transport.helperReady=[]{return true;};
            }
        };
        const auto require=[](bool value,const char* why)
        {if(!value)log::Error("Input mask verification self-test: %s",why);return value;};
        FakeMasks native;Implementation verified;native.Attach(verified);
        if(!require(verified.Reconcile(InputLockX|InputLockY) && native.reads==1 &&
            native.events==std::vector<std::string>{"lock 1","lock 2","read masks"},"one actual read after both lock writes"))return false;
        if(!require(verified.VerifyMasks(InputLockX|InputLockY) && native.reads==1,"ordinary frames do not repeat the query"))return false;
        native.now+=kMaskVerifyUs;
        if(!require(verified.VerifyMasks(InputLockX|InputLockY) && native.reads==2,"100 ms due verification"))return false;
        verified.refreshMasks=true;
        if(!require(verified.VerifyMasks(InputLockX|InputLockY) && native.reads==3,"first command of a new correction refreshes masks"))return false;
        // A firmware drift cancels output before any cleanup writes and does not
        // turn a successful old readback into permission to continue sending.
        native.mask=InputLockX;native.pending=native.click=true;verified.refreshMasks=true;
        if(!require(!verified.VerifyMasks(InputLockX|InputLockY) && verified.resetRequired && native.invalidated &&
            !native.pending && !native.click && native.mask==0 && verified.uncertain==InputManagedLocks && verified.maskDrifts==1,
            "mask drift stops output and restores only managed inputs"))return false;
        const auto invalidated=std::find(native.events.begin(),native.events.end(),"invalidate");
        const auto cancelled=std::find(native.events.begin(),native.events.end(),"cancel");
        if(!require(invalidated<cancelled,"invalidate before cancellation"))return false;
        const auto readCount=native.reads;
        if(!require(!verified.VerifyMasks(InputLockX|InputLockY) && native.reads==readCount,"poisoned session rejects further readback"))return false;
        FakeMasks timeout;Implementation timed;timeout.Attach(timed);timeout.timeout=true;
        if(!require(!timed.Reconcile(InputLockX) && timed.resetRequired && timeout.invalidated && timeout.mask==0 &&
            timed.maskReadbackFailures==1 && !timed.verificationValid,"query timeout cannot be reported as unlocked or verified"))return false;
        FakeMasks partial;Implementation weighed;partial.Attach(weighed);partial.partial=true;
        if(!require(!weighed.Reconcile(InputLockX) && weighed.resetRequired && weighed.maskDrifts==1,"partial directional scale is not a full lock"))return false;
        FakeMasks writes;Implementation failedWrite;writes.Attach(failedWrite);writes.failY=true;
        if(!require(!failedWrite.Reconcile(InputLockX|InputLockY) && writes.reads==0 && writes.mask==InputLockX &&
            failedWrite.uncertain==(InputLockX|InputLockY),"partial write preserves uncertainty before querying"))return false;
        failedWrite.BlindCleanup();if(!require(writes.mask==0 && writes.reads==0,"partial-write cleanup uses no poisoned queries"))return false;
        // Hybrid restores Y only after prior motion is idle; its shared side
        // reservation remains, and the next fresh correction may reacquire X.
        FakeMasks hybrid;Implementation switching;hybrid.Attach(switching);
        switching.policies[1]={true,7,MouseButtonBinding::x1};switching.settings.reserveActivationSideButtons=true;
        if(!switching.Reconcile(InputLockX|InputLockY|InputLockSide1))return false;
        switching.motion=true;switching.motionCancelled=true;switching.movement=a;switching.releaseReason="Hybrid vertical window ended";
        hybrid.pending=true;const auto beforeDrain=hybrid.reads;
        if(!require(switching.Drain(1000000) && switching.draining && switching.motion &&
            hybrid.mask==(InputLockX|InputLockY|InputLockSide1) && hybrid.reads==beforeDrain,"Hybrid preserves Y lock while pending movement drains"))return false;
        hybrid.pending=false;switching.nextDrainUs=0;
        if(!require(switching.Drain(1005000) && !switching.draining && !switching.motion && hybrid.mask==InputLockSide1,
            "Hybrid unlocks axes after drain while keeping activation reserved"))return false;
        if(!require(switching.Reconcile(InputLockX|InputLockSide1) && hybrid.mask==(InputLockX|InputLockSide1),"Horizontal continuation leaves manual Y available"))return false;
        // New connections invalidate prior evidence. A cached table supplied by
        // a transport is rejected even if its bit values happen to match.
        const auto epoch=switching.connectionEpoch;switching.ResetVerificationLocked();hybrid.badTimestamp=true;
        if(!require(switching.connectionEpoch!=epoch && !switching.verificationValid && switching.firmwareMasks.readAtUs==0 &&
            !switching.VerifyMasks(hybrid.mask) && switching.resetRequired,"old connection/timestamp cannot authorize output"))return false;
        // A configuration with movement locks disabled remains an opt-out.
        Implementation disabled;disabled.motion=true;disabled.motionMask=0;disabled.settings.lockMovementWhileCorrecting=false;
        if(!require((disabled.Desired()&(InputLockX|InputLockY))==0,"saved unlocked setting is respected"))return false;
        if(!RecoveryLayoutMatches(kInputRecoveryRecordVersion,256,256) || RecoveryLayoutMatches(1,256,256) ||
            RecoveryLayoutMatches(kInputRecoveryRecordVersion,128,256) || RecoveryLayoutMatches(kInputRecoveryRecordVersion,0,0))return false;
        // The policy handed to firmware, evaluated independently against its
        // documented per-axis merge rule. This does not claim USB measurement.
        const auto merge=[](int guidance,int human,const DirectionalInputScales& scale)
        {const int fixed=human<0?scale.negative:scale.positive;
            const int relative=!guidance?100:Sign(guidance)==Sign(human)?scale.with:scale.against;
            return guidance+human*fixed*relative/10000;};
        const auto profile=MakeManagedInputPolicy(InputLockSide1,InputLockX|InputLockY);
        if(merge(-4,-3,profile.scales[0])!=-7 || merge(-4,3,profile.scales[0])!=-4 ||
            merge(2,-5,profile.scales[1])!=2 || merge(2,5,profile.scales[1])!=7)return false;
        const auto horizontal=MakeManagedInputPolicy(0,InputLockX);
        if(merge(0,3,horizontal.scales[1])!=3 || horizontal.scales[1]!=DirectionalInputScales{} ||
            GuidedAxes(4,0,InputLockX|InputLockY)!=InputLockX || GuidedAxes(4,5,InputLockX)!=InputLockX)return false;
        // Exercise the actual public submission path with the maintenance
        // worker stopped, so the fake clock/device are fully controlled here.
        const auto setup=[&](InputLockCoordinator& coordinator,FakeMasks& fake)
        {
            auto& state=*coordinator.state_;state.worker.request_stop();state.changed.notify_all();state.worker.join();
            fake.Attach(state);fake.EnableFuse(state);state.settings.backend=InputBackend::medius;
            state.attached=state.probed=true;state.ready=true;state.helperHealthy=true;
            state.policies[1]={true,7,MouseButtonBinding::x1};state.physical=Button(MouseButtonBinding::x1);
            state.physicalKnown=0x1FU;state.observedReleased=state.physical;
        };
        // A class-change stream may start with a genuine POS press, without a
        // preceding all-up snapshot. Do not confuse that with a cached held
        // state. These checks exercise the production validity/rearm gate.
        FakeMasks activationDevice;InputLockCoordinator activation;setup(activation,activationDevice);auto& arming=*activation.state_;
        OutputContext held;held.owner=InputOwner::testClick;held.generation=81;held.activation=MouseButtonBinding::x1;
        activation.SetPolicy(InputOwner::testClick,true,held.generation,held.activation);
        arming.ResetVerificationLocked();
        activation.PhysicalButtons(Button(held.activation));
        if(arming.Valid(held,StreamNowUs()))return false; // A held baseline is insufficient.
        activation.PhysicalButtons(Button(held.activation),PhysicalButtonEdge::press);
        if(arming.Valid(held,StreamNowUs()))return false; // Unchanged held state cannot rearm.
        arming.ResetVerificationLocked();
        activation.PhysicalButtons(Button(held.activation),PhysicalButtonEdge::press);
        if(!arming.Valid(held,StreamNowUs()) || arming.lastNativePressProof!=Button(held.activation))return false;
        // Ambiguous first snapshots cannot attribute a press to every held key.
        arming.ResetVerificationLocked();
        activation.PhysicalButtons(Button(MouseButtonBinding::left)|Button(held.activation),PhysicalButtonEdge::press);
        if(arming.Valid(held,StreamNowUs()) || arming.lastNativePressProof)return false;
        activation.PhysicalButtons(Button(MouseButtonBinding::left),PhysicalButtonEdge::release);
        activation.PhysicalButtons(Button(MouseButtonBinding::left)|Button(held.activation),PhysicalButtonEdge::press);
        if(!arming.Valid(held,StreamNowUs()) || (arming.observedReleased&Button(MouseButtonBinding::left)))return false;
        // Emergency release preserves the held baseline. A newer configuration
        // alone, or the same held mask, cannot bypass a real release/press.
        activation.EmergencyRelease();++held.generation;
        activation.SetPolicy(InputOwner::testClick,true,held.generation,held.activation);
        activation.PhysicalButtons(Button(MouseButtonBinding::left)|Button(held.activation));
        activation.PhysicalButtons(Button(MouseButtonBinding::left)|Button(held.activation),PhysicalButtonEdge::press);
        if(arming.Valid(held,StreamNowUs()))return false;
        activation.PhysicalButtons(0,PhysicalButtonEdge::release);
        activation.PhysicalButtons(Button(held.activation),PhysicalButtonEdge::press);
        if(!arming.Valid(held,StreamNowUs()))return false;
        // A new connection forgets old evidence; the stock cached-mask path
        // retains its original requirement to actually observe release.
        arming.ResetVerificationLocked();arming.settings.backend=InputBackend::makcu;
        activation.PhysicalButtons(Button(held.activation),PhysicalButtonEdge::press);
        if(arming.Valid(held,StreamNowUs()))return false;
        activation.PhysicalButtons(0);activation.PhysicalButtons(Button(held.activation));
        if(!arming.Valid(held,StreamNowUs()))return false;
        // Repeated public TryClick calls use the same held activation, with no
        // release edge required between completed finite clicks. Service is
        // driven explicitly; these opt-in checks never open a device.
        FakeMasks clickDevice;InputLockCoordinator repeated;setup(repeated,clickDevice);auto& clicks=*repeated.state_;
        clicks.ResetVerificationLocked();
        repeated.SetPolicy(InputOwner::testClick,true,41,MouseButtonBinding::x1);
        repeated.PhysicalButtons(Button(MouseButtonBinding::x1),PhysicalButtonEdge::press);
        OutputContext clickFrame;clickFrame.owner=InputOwner::testClick;clickFrame.generation=41;
        clickFrame.activation=MouseButtonBinding::x1;clickFrame.streamGeneration=3;clickFrame.streamSession=4;
        bool clickConfirmed{};
        clicks.transport.endClick=[&](bool cancel)
        {clickDevice.events.push_back(cancel?"cancel click":"finish click");clickDevice.click=false;clickConfirmed=!cancel;return true;};
        clicks.transport.clickSucceeded=[&]{return clickConfirmed;};
        const auto completeOneClick=[&]
        {
            auto entered=std::make_shared<std::promise<void>>();auto begun=entered->get_future();
            clicks.transport.beginClick=[&,entered](int hold,const OutputContext& context)
            {
                if(hold!=14 || !context.isCurrent || !context.isCurrent())return false;
                if(!(clickDevice.mask&InputLockLeft) || !clicks.verificationValid)return false;
                clickDevice.events.push_back("begin click");clickDevice.click=true;clickConfirmed=false;entered->set_value();return true;
            };
            ++clickFrame.sequence;auto result=std::async(std::launch::async,[&]{return repeated.TryClick(clickFrame,14);});
            if(begun.wait_for(std::chrono::milliseconds(500))!=std::future_status::ready)return false;
            {std::lock_guard transaction(clicks.io);std::lock_guard guard(clicks.state);clicks.clickDeadlineUs=0;}
            clicks.Service();return result.get();
        };
        for(int i=0;i<4;++i)if(!completeOneClick())return false;
        const auto repeatedStatus=repeated.ClickStatus(InputOwner::testClick);
        if(repeatedStatus.attempts!=4 || repeatedStatus.completed!=4 || repeatedStatus.started!=4 ||
            clickDevice.click || (clickDevice.mask&InputLockLeft) || !(clickDevice.mask&InputLockSide1))return false;
        const auto clickBegin=std::find(clickDevice.events.begin(),clickDevice.events.end(),"begin click");
        const auto clickEnd=std::find(clickDevice.events.begin(),clickDevice.events.end(),"finish click");
        const auto clickUnmask=std::find(clickDevice.events.begin(),clickDevice.events.end(),"unlock 4");
        if(clickBegin>=clickEnd || clickEnd>=clickUnmask)return false;
        // The old enable generation cannot undo emergency release. A new
        // generation plus a fresh physical release/press explicitly rearms it.
        repeated.EmergencyRelease();clicks.Service();
        repeated.SetPolicy(InputOwner::testClick,true,41,MouseButtonBinding::x1);
        if(repeated.TryClick(clickFrame,14) || repeated.ClickStatus(InputOwner::testClick).rejected!=1)return false;
        repeated.SetPolicy(InputOwner::testClick,true,42,MouseButtonBinding::x1);clickFrame.generation=42;
        repeated.PhysicalButtons(0);repeated.PhysicalButtons(Button(MouseButtonBinding::x1));
        if(!completeOneClick() || repeated.ResetRequired())return false;
        // Release/repress while preparing a click must latch cancellation; the
        // newly held button cannot revive the old request or poison the link.
        clicks.transport.beginClick=[&](int,const OutputContext& context)
        {repeated.PhysicalButtons(0);repeated.PhysicalButtons(Button(MouseButtonBinding::x1));return context.isCurrent();};
        if(repeated.TryClick(clickFrame,14) || repeated.ResetRequired() || clickDevice.click ||
            repeated.ClickStatus(InputOwner::testClick).phase!=ClickOutputPhase::cancelled)return false;
        if(!completeOneClick())return false;
        // A settings change during actual firmware-mask readback is another
        // ordinary cancellation. It restores left without starting playback.
        clicks.refreshMasks=true;clickDevice.beforeRead=[&]
        {repeated.SetPolicy(InputOwner::testClick,false,43,MouseButtonBinding::x1);};
        if(repeated.TryClick(clickFrame,14) || repeated.ResetRequired() || clickDevice.click || (clickDevice.mask&InputLockLeft))return false;
        repeated.SetPolicy(InputOwner::testClick,true,44,MouseButtonBinding::x1);clickFrame.generation=44;
        if(!completeOneClick())return false;
        // A release query failure is a real fault, not a confirmed shot.
        const auto confirmedBeforeTimeout=repeated.ClickStatus(InputOwner::testClick).completed;
        const auto originalQuery=clicks.transport.query;
        clicks.transport.query=[&](InputQuery query,int& value)
        {return query!=InputQuery::leftState && originalQuery(query,value);};
        if(completeOneClick() || !repeated.ResetRequired() ||
            repeated.ClickStatus(InputOwner::testClick).completed!=confirmedBeforeTimeout)return false;
        clicks.Service();
        OutputContext frame;frame.owner=InputOwner::python;frame.generation=7;frame.streamGeneration=8;frame.streamSession=9;
        frame.activation=MouseButtonBinding::x1;frame.physicalInputMode=PythonPhysicalInputMode::fuse;
        const auto fresh=[](OutputContext& value){++value.sequence;value.frameTimeUs=StreamNowUs();};
        FakeMasks fuse;InputLockCoordinator fused;setup(fused,fuse);auto& core=*fused.state_;
        fresh(frame);if(!fused.TryMove(-4,2,frame) || fuse.emitted!=std::vector<std::pair<int,int>>{{-4,2}} ||
            !(fuse.recoveryMarked&InputLockX) || core.applied!=InputLockSide1 || !core.verificationValid)return false;
        const auto firstRead=std::find(fuse.events.begin(),fuse.events.end(),"read masks");
        const auto firstMove=std::find(fuse.events.begin(),fuse.events.end(),"move");
        if(firstRead==fuse.events.end() || firstRead>=firstMove || core.requestedPolicy!=profile)return false;
        const auto initialWrites=std::count(fuse.events.begin(),fuse.events.end(),"axis policy 1");
        fresh(frame);if(!fused.TryMove(-5,3,frame) || fuse.reads!=1 ||
            std::count(fuse.events.begin(),fuse.events.end(),"axis policy 1")!=initialWrites)return false;
        // Reversal: no new movement before drain; valid independent click is
        // preserved. Reusing the pre-drain observation must remain rejected.
        fresh(frame);if(fused.TryMove(5,3,frame) || !core.motionCancelled)return false;
        fuse.click=true;if(!core.Drain(StreamNowUs()) || !core.draining || !fuse.click || fuse.emitted.size()!=2)return false;
        fuse.pending=false;core.nextDrainUs=0;
        if(!core.Drain(StreamNowUs()) || !core.draining || !fuse.click)return false;
        fuse.click=false;core.nextDrainUs=0;
        if(!core.Drain(StreamNowUs()) || core.motion || core.draining || fuse.axisScales[0]!=DirectionalInputScales{} ||
            std::find(fuse.events.begin(),fuse.events.end(),"cancel")!=fuse.events.end() || fused.TryMove(5,3,frame))return false;
        fresh(frame);if(!fused.TryMove(5,3,frame) || fuse.emittedContexts.back().sequence!=frame.sequence)return false;
        // A neutral Y correction and then Hybrid's axis transition both drain
        // the old bearing; neither releases stored Y into the next session.
        fresh(frame);if(fused.TryMove(5,0,frame))return false;fuse.pending=false;core.nextDrainUs=0;
        if(!core.Drain(StreamNowUs()))return false;
        fresh(frame);if(!fused.TryMove(5,0,frame) || fuse.axisScales[1]!=DirectionalInputScales{} || core.motionGuidedAxes!=InputLockX)return false;
        frame.axes=InputLockX;fresh(frame);if(fused.TryMove(5,0,frame))return false;
        fuse.pending=false;core.nextDrainUs=0;if(!core.Drain(StreamNowUs()))return false;
        fresh(frame);if(!fused.TryMove(5,0,frame) || core.motionGuidedAxes!=InputLockX)return false;
        // Release/repress during firmware verification cannot resurrect the
        // correction that was in flight when activation went up.
        fuse.now+=kMaskVerifyUs;fuse.beforeRead=[&]{fused.PhysicalButtons(0);fused.PhysicalButtons(Button(MouseButtonBinding::x1));};
        const auto beforeRelease=fuse.emitted.size();fresh(frame);
        if(fused.TryMove(6,0,frame) || fuse.emitted.size()!=beforeRelease || !core.motionCancelled)return false;
        fuse.pending=false;core.nextDrainUs=0;if(!core.Drain(StreamNowUs()))return false;
        fresh(frame);if(!fused.TryMove(6,0,frame))return false;
        // A drift in just the relative slot invalidates first, cancels both
        // outputs and clears all four managed axis slots without more reads.
        fuse.axisScales[0].against=100;fuse.click=true;fuse.now+=kMaskVerifyUs;fresh(frame);
        const auto beforeDrift=fuse.emitted.size();
        if(fused.TryMove(6,0,frame) || !core.resetRequired || !fuse.invalidated || fuse.click ||
            fuse.emitted.size()!=beforeDrift || fuse.axisScales[0]!=DirectionalInputScales{} || fuse.axisScales[1]!=DirectionalInputScales{})return false;
        const auto readsAfterDrift=fuse.reads;if(core.VerifyMasks(0) || fuse.reads!=readsAfterDrift)return false;
        FakeMasks badBearing;InputLockCoordinator bearingFailure;setup(bearingFailure,badBearing);badBearing.wrongBearing=true;
        fresh(frame);if(bearingFailure.TryMove(1,0,frame) || !bearingFailure.ResetRequired() || !badBearing.emitted.empty())return false;
        FakeMasks unavailable;InputLockCoordinator unsupported;setup(unsupported,unavailable);
        unsupported.state_->settings.backend=InputBackend::makcu;fresh(frame);
        if(unsupported.TryMove(1,0,frame) || !unavailable.emitted.empty() || unavailable.reads)return false;
        FakeMasks setupError;InputLockCoordinator unprepared;setup(unprepared,setupError);setupError.failPreparation=true;fresh(frame);
        if(unprepared.TryMove(1,0,frame) || !unprepared.ResetRequired() || !setupError.emitted.empty())return false;
        FakeMasks clearError;InputLockCoordinator notCleared;setup(notCleared,clearError);fresh(frame);
        if(!notCleared.TryMove(1,0,frame))return false;
        notCleared.EndCorrection(InputOwner::python,frame.generation);clearError.pending=false;clearError.failBearingClear=true;
        if(notCleared.state_->Drain(StreamNowUs()) || !notCleared.ResetRequired())return false;
        notCleared.state_->Service();if(clearError.mask || clearError.axisScales[0]!=DirectionalInputScales{})return false;
        FakeMasks fuseTimeout;InputLockCoordinator timedFuse;setup(timedFuse,fuseTimeout);fuseTimeout.timeout=true;fresh(frame);
        if(timedFuse.TryMove(1,0,frame) || !timedFuse.ResetRequired() || !fuseTimeout.emitted.empty() || !fuseTimeout.invalidated)return false;
        FakeMasks partialWrite;InputLockCoordinator partialFailure;setup(partialFailure,partialWrite);partialWrite.failAxisAfterFixed=true;
        fresh(frame);if(partialFailure.TryMove(1,0,frame) || !partialFailure.ResetRequired() || !partialWrite.emitted.empty())return false;
        partialFailure.state_->Service();if(partialWrite.axisScales[0]!=DirectionalInputScales{})return false;
        FakeMasks staleDevice;InputLockCoordinator generations;setup(generations,staleDevice);
        auto staleFrame=frame;staleFrame.generation=6;fresh(staleFrame);
        if(generations.TryMove(1,0,staleFrame) || !staleDevice.emitted.empty())return false;
        staleFrame=frame;staleFrame.isCurrent=[]{return false;};fresh(staleFrame);
        if(generations.TryMove(1,0,staleFrame) || !staleDevice.emitted.empty())return false;
        fresh(frame);if(!generations.TryMove(1,0,frame))return false;
        generations.EmergencyRelease();generations.state_->Service();
        if(staleDevice.pending || staleDevice.click || generations.state_->motion || staleDevice.mask ||
            staleDevice.axisScales[0]!=DirectionalInputScales{})return false;
        log::Info("Input-lock / Fuse shared-control deterministic checks passed");return true;
    }
}
