#include "stream_diagnostics.hpp"
#include "logger.hpp"
#include "version.hpp"
#include "input_backend_utils.hpp"
#include <Windows.h>
#include <psapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace
{
    using namespace vanta;
    std::string Quote(const std::string& value)
    {
        std::ostringstream out;out<<'"';
        for(const unsigned char c:value)
        {
            if(c=='"' || c=='\\')out<<'\\'<<char(c);
            else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
            else out<<char(c);
        }
        out<<'"';return out.str();
    }
    std::string CsvQuote(const std::string& value)
    {
        std::string out="\"";for(char c:value){if(c=='"')out+='"';out+=c;}return out+'"';
    }
    std::string Number(double value)
    {std::ostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(1)<<value;return out.str();}
    std::uint64_t Delta(std::uint64_t value,std::uint64_t previous)
    {return value>=previous?value-previous:0;}
    bool Low(double actual,double expected)
    {return expected>0 && actual<std::max(0.0,expected-std::max(3.0,expected*.01));}
    void RectJson(std::ostream& out,const cv::Rect& value)
    {out<<"{\"x\":"<<value.x<<",\"y\":"<<value.y<<",\"width\":"<<value.width<<",\"height\":"<<value.height<<'}';}
    void TimingJson(std::ostream& out,const TimingSummary& value)
    {out<<"{\"median_ms\":"<<value.medianMs<<",\"p95_ms\":"<<value.p95Ms<<",\"p99_ms\":"<<value.p99Ms<<",\"samples\":"<<value.samples<<'}';}
    auto Metrics(const DecodedVideoStats& video)
    {
        return std::array<std::pair<const char*,TimingSummary>,10>{{
            {"reassembly",video.reassembly},{"compressed_queue",video.compressedQueue},
            {"decode_submit_cpu_and_driver",video.decodeSubmit},{"decode_and_queue",video.decode},
            {"decode_gpu_completion_observed",video.gpuCompletionObserved},{"gpu_conversion_readback_and_map",video.readback},
            {"cpu_image_copy",video.cpuCopy},{"capture_publication_dispatch",video.publicationDispatch},
            {"receiver_arrival_to_publication",video.publication},{"host_processing_optional",video.hostProcessing}}};
    }
    void SampleJson(std::ostream& out,const StreamDiagnosticSample& sample)
    {
        const auto& s=sample.video;const auto& m=s.latestMetadata;
        out<<"{\"local_monotonic_us\":"<<sample.nowUs<<",\"connection_terminated\":"<<(sample.connectionTerminated?"true":"false")
            <<",\"termination_error\":"<<sample.terminationError<<",\"requested\":{\"width\":"<<sample.requested.width<<",\"height\":"<<sample.requested.height
            <<",\"fps\":"<<sample.requested.fps<<",\"request_host_display_mode\":"<<(sample.requested.requestDisplayMode?"true":"false")<<",\"bitrate_budget_kbps\":"<<sample.requested.bitrateKbps
            <<",\"codec_preference\":"<<int(sample.requested.codec)<<",\"adapter_preference\":"<<Quote(sample.requested.adapter)<<"},\"video\":{"
            <<"\"generation\":"<<s.generation<<",\"session\":"<<s.session<<",\"hardware_active\":"<<(s.hardwareActive?"true":"false")<<",\"failed\":"<<(s.failed?"true":"false")
            <<",\"codec\":"<<Quote(s.codec)<<",\"adapter\":"<<Quote(s.adapter)<<",\"synchronization\":"<<Quote(s.synchronization)<<",\"status\":"<<Quote(s.status)
            <<",\"received_fps\":"<<s.receivedFramesPerSecond<<",\"decoded_fps\":"<<s.decodedFramesPerSecond<<",\"cpu_published_fps\":"<<s.publishedFramesPerSecond
            <<",\"received_frames\":"<<s.receivedFrames<<",\"submitted_frames\":"<<s.submittedFrames<<",\"decoded_frames\":"<<s.decodedFrames
            <<",\"published_frames\":"<<s.publishedFrames<<",\"dropped_compressed_frames\":"<<s.droppedFrames
            <<",\"transport_frame_gaps\":"<<s.transportLostFrames<<",\"compressed_overload_events\":"<<s.overloadDrops
            <<",\"decoder_failures\":"<<s.decoderFailures<<",\"decoded_output_replacements\":"<<s.outputSkippedFrames<<",\"idr_requests\":"<<s.idrRequests
            <<",\"packet_queue_depth\":"<<s.queueDepth<<",\"gpu_frames_in_flight\":"<<s.gpuFramesInFlight
            <<",\"readbacks_issued\":"<<s.readbacksIssued<<",\"readbacks_completed\":"<<s.readbacksCompleted<<",\"readback_bytes\":"<<s.readbackBytes
            <<",\"readbacks_superseded\":"<<s.readbackSuperseded<<",\"readback_slot_pressure_events\":"<<s.readbackSlotPressure
            <<",\"cpu_pool_exhaustions\":"<<s.cpuPoolExhaustions<<",\"nonblocking_map_retries\":"<<s.nonblockingMapRetries
            <<",\"last_received_us\":"<<s.lastReceivedUs<<",\"last_published_us\":"<<s.lastPublishedUs
            <<",\"encoded_payload_mbps\":"<<s.encodedMegabitsPerSecond<<",\"observed_ethernet_rx_mbps\":"<<s.network.receiveMbps
            <<",\"observed_ethernet_tx_mbps\":"<<s.network.transmitMbps<<",\"observed_traffic_available\":"<<(s.network.available?"true":"false")
            <<",\"observed_traffic_scope\":\"all traffic on active Ethernet interfaces, not a stream-specific wire rate\""
            <<",\"visible_width\":"<<m.sourceWidth<<",\"visible_height\":"<<m.sourceHeight<<",\"visible_x\":"<<m.visibleX<<",\"visible_y\":"<<m.visibleY
            <<",\"texture_width\":"<<m.textureWidth<<",\"texture_height\":"<<m.textureHeight<<",\"frame_number\":"<<m.frameNumber
            <<",\"presentation_us\":"<<m.presentationTimeUs<<"},\"demand\":{\"capture_running\":"<<(sample.demand.running?"true":"false")
            <<",\"capture_worker_failed\":"<<(sample.demand.workerFailed?"true":"false")<<",\"stream_rate_requested\":"<<(sample.demand.streamRate?"true":"false")
            <<",\"maximum_capped_fps\":"<<sample.demand.maximumFps<<",\"wait_mode\":"<<Quote(sample.demand.waitMode)
            <<",\"scheduler_wake_lateness\":";
        TimingJson(out,sample.demand.schedulerWakeLateness);out<<",\"readback_union\":";
        RectJson(out,sample.demand.readbackUnion);out<<",\"regions\":[";bool first=true;
        for(const auto& region:sample.demand.regions)
        {
            if(!std::exchange(first,false))out<<',';
            out<<"{\"consumer\":"<<Quote(region.consumer)<<",\"kind\":"<<int(region.request.kind)<<",\"side\":"<<region.request.side
                <<",\"maximum_fps\":"<<region.request.maximumFps<<",\"resolved\":";RectJson(out,region.resolved);out<<'}';
        }
        out<<"]},\"consumers\":[";first=true;
        for(const auto& consumer:sample.consumers)
        {
            if(!std::exchange(first,false))out<<',';
            out<<"{\"name\":"<<Quote(consumer.name)<<",\"fps\":"<<consumer.fps<<",\"frames\":"<<consumer.frames
                <<",\"processing_expected\":"<<(consumer.processingExpected?"true":"false")<<",\"state\":"<<Quote(consumer.state)<<",\"processing\":";
            TimingJson(out,consumer.processing);out<<",\"algorithm_wall_time\":";TimingJson(out,consumer.algorithm);
            out<<",\"receiver_frame_age\":";TimingJson(out,consumer.age);out<<'}';
        }
        out<<"],\"timings\":{";first=true;
        for(const auto& [name,value]:Metrics(s)){if(!std::exchange(first,false))out<<',';out<<Quote(name)<<':';TimingJson(out,value);}
        const auto& p=sample.publishedMetadata;
        out<<"},\"cpu_publication_metadata\":{\"generation\":"<<p.generation<<",\"sequence\":"<<p.sequence<<",\"frame_number\":"<<p.frameNumber
            <<",\"source_width\":"<<p.sourceWidth<<",\"source_height\":"<<p.sourceHeight<<",\"crop_x\":"<<p.cropX<<",\"crop_y\":"<<p.cropY
            <<",\"crop_width\":"<<p.cropWidth<<",\"crop_height\":"<<p.cropHeight<<",\"presentation_us\":"<<p.presentationTimeUs
            <<",\"received_us\":"<<p.timing.receivedUs<<",\"assembled_us\":"<<p.timing.assembledUs<<",\"submitted_us\":"<<p.timing.submittedUs
            <<",\"decoded_us\":"<<p.timing.decodedUs<<",\"readback_started_us\":"<<p.timing.readbackStartedUs<<",\"readback_us\":"<<p.timing.readbackUs
            <<",\"published_us\":"<<p.timing.publishedUs<<"},\"receiver_load\":{\"cpu_load_available\":"<<(sample.cpuLoadAvailable?"true":"false")
            <<",\"sample_interval_ms\":"<<sample.sampleIntervalMs<<",\"process_cpu_percent_all_cores\":"<<sample.processCpuPercent
            <<",\"process_cpu_core_equivalents\":"<<sample.processCpuCores<<",\"system_cpu_percent\":"<<sample.systemCpuPercent
            <<",\"working_set_bytes\":"<<sample.workingSetBytes<<",\"available_physical_bytes\":"<<sample.availablePhysicalBytes
            <<"},\"capture\":"<<sample.captureJson<<",\"makcu_and_features\":"<<sample.outputJson<<'}';
    }
    struct ReportJob
    {
        std::string trigger,hardware;
        std::deque<StreamDiagnosticSample> history;
        std::vector<FpsFinding> findings;
    };
    bool WriteReport(const ReportJob& job,const std::filesystem::path& root,const std::wstring& stem,std::string& result)
    {
        if(job.history.empty()){result="No stream samples available yet";return false;}
        std::error_code error;std::filesystem::create_directories(root,error);
        if(error){result="Cannot create diagnostics directory: "+error.message();return false;}
        const auto jsonPath=root/(stem+L".json"),csvPath=root/(stem+L"-timings.csv"),ratesPath=root/(stem+L"-rates.csv");
        std::ofstream json(jsonPath,std::ios::binary),csv(csvPath,std::ios::binary),rates(ratesPath,std::ios::binary);
        if(!json || !csv || !rates){result="Cannot write diagnostics files";return false;}
        json.imbue(std::locale::classic());csv.imbue(std::locale::classic());rates.imbue(std::locale::classic());
        json<<std::setprecision(8)<<"{\n\"report_schema\":1,\"build\":"<<Quote(VANTA_VERSION_FILE_STRING " fps-diagnostics " __DATE__ " " __TIME__)
            <<",\"trigger\":"<<Quote(job.trigger)<<",\"hardware\":"<<job.hardware
            <<",\"scope\":\"Receiver-local observations. Source/encoder versus network cannot be isolated from received FPS alone. Stage durations are wall time and include scheduling/preemption. GPU completion timing includes completion observation delay, not pure GPU execution. Timings roll over 10 seconds; rate history is sampled about once per second. Requested bitrate includes FEC; encoded payload excludes RTP/FEC. No complete host-to-display latency.\",\n\"findings\":[";
        bool first=true;
        for(const auto& f:job.findings)
        {
            if(!std::exchange(first,false))json<<',';
            json<<"{\"code\":"<<Quote(f.code)<<",\"stage\":"<<Quote(f.stage)<<",\"severity\":"<<Quote(f.severity)
                <<",\"confidence\":"<<Quote(f.confidence)<<",\"problem\":"<<(f.problem?"true":"false")
                <<",\"evidence\":"<<Quote(f.evidence)<<",\"next_step\":"<<Quote(f.nextStep)<<'}';
        }
        std::map<std::string,unsigned> occurrenceCounts;
        for(std::size_t i=0;i<job.history.size();++i)
        {
            std::set<std::string> seen;
            for(const auto& finding:DiagnoseStream(job.history[i],i?&job.history[i-1]:nullptr))if(finding.problem)seen.insert(finding.code);
            for(const auto& code:seen)++occurrenceCounts[code];
        }
        json<<"],\n\"history_problem_sample_counts\":{";first=true;
        for(const auto& [code,count]:occurrenceCounts){if(!std::exchange(first,false))json<<',';json<<Quote(code)<<':'<<count;}
        json<<"},\n\"current\":";SampleJson(json,job.history.back());json<<",\n\"history\":[";first=true;
        for(const auto& sample:job.history){if(!std::exchange(first,false))json<<',';SampleJson(json,sample);}json<<"]\n}\n";
        csv<<"stage,median_ms,p95_ms,p99_ms,samples\n";
        const auto metric=[&](const std::string& name,const TimingSummary& m){csv<<CsvQuote(name)<<','<<m.medianMs<<','<<m.p95Ms<<','<<m.p99Ms<<','<<m.samples<<'\n';};
        for(const auto& [name,value]:Metrics(job.history.back().video))metric(name,value);
        metric("capture_request_wake_lateness",job.history.back().demand.schedulerWakeLateness);
        for(const auto& c:job.history.back().consumers){metric(c.name+" processing (includes output/intentional pauses)",c.processing);metric(c.name+" algorithm wall time (where instrumented)",c.algorithm);metric(c.name+" receiver frame age",c.age);}
        try
        {
            vanta::InputApartment apartment;
            const auto output=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(job.history.back().outputJson));
            const auto mouse=output.GetNamedObject(L"makcu");
            const auto backend=mouse.GetNamedObject(L"motion_backend");
            const auto rows=[&](const winrt::Windows::Data::Json::JsonObject& object,const std::string& prefix)
            {if(!object.HasKey(L"timings"))return;for(const auto& item:object.GetNamedObject(L"timings"))
                {const auto value=item.Value().GetObject();metric(prefix+winrt::to_string(item.Key()),
                    {value.GetNamedNumber(L"median_ms",0),value.GetNamedNumber(L"p95_ms",0),value.GetNamedNumber(L"p99_ms",0),
                    static_cast<std::size_t>(value.GetNamedNumber(L"samples",0))});}};
            rows(backend,"mouse output ");
            if(mouse.HasKey(L"input_locks"))
            {
                const auto locks=mouse.GetNamedObject(L"input_locks");
                if(locks.HasKey(L"mask_readback_timing"))
                {
                    const auto timing=locks.GetNamedObject(L"mask_readback_timing");
                    metric("physical input mask readback",{timing.GetNamedNumber(L"median_ms",0),timing.GetNamedNumber(L"p95_ms",0),
                        timing.GetNamedNumber(L"p99_ms",0),static_cast<std::size_t>(timing.GetNamedNumber(L"samples",0))});
                }
                if(locks.HasKey(L"fuse_transition_request_to_drain_completion"))
                {
                    const auto timing=locks.GetNamedObject(L"fuse_transition_request_to_drain_completion");
                    metric("Fuse transition request to old motion drain/bearing clear completion",
                        {timing.GetNamedNumber(L"median_ms",0),timing.GetNamedNumber(L"p95_ms",0),
                            timing.GetNamedNumber(L"p99_ms",0),static_cast<std::size_t>(timing.GetNamedNumber(L"samples",0))});
                }
            }
        }catch(...){/* Older/stock snapshots have no motion timing extension. */}
        rates<<"local_monotonic_us,generation,requested_fps,received_fps,decoded_fps,cpu_published_fps,packet_queue,gpu_frames_in_flight,transport_gaps_total,overload_total,decoder_errors_total,readbacks_completed_total,pool_exhaustions_total,readback_p95_ms,arrival_to_publication_p95_ms,process_cpu_percent_all_cores,system_cpu_percent,sample_interval_ms,consumer,consumer_fps,consumer_state,cpu_demand_mode,cpu_demand_cap_fps,capture_wake_p95_ms,capture_wait_mode,mouse_backend,mouse_move_command_hz,mouse_clicks_completed_total,mouse_injection_active_last_readback,mouse_faults_total,python_physical_input_mode,python_processed_fps,python_accepted_movement_commands_per_second\n";
        for(const auto& sample:job.history)
        {
            std::string mouseBackend="makcu";double mouseCommands{},mouseClicks{},mouseActive{},mouseFaults{};
            std::string pythonInputMode="existing";double pythonProcessed{},pythonAccepted{};
            try
            {
                vanta::InputApartment apartment;const auto output=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(sample.outputJson));
                const auto backend=output.GetNamedObject(L"makcu").GetNamedObject(L"motion_backend");
                mouseBackend=winrt::to_string(backend.GetNamedString(L"backend",L"makcu"));
                mouseCommands=backend.GetNamedNumber(L"move_command_hz",0);
                mouseClicks=backend.GetNamedNumber(L"clicks_completed",0);
                mouseActive=backend.GetNamedBoolean(L"injection_active_last_readback",false)?1:0;
                mouseFaults=backend.GetNamedNumber(L"faults",0);
                if(output.HasKey(L"python_output"))
                {
                    const auto python=output.GetNamedObject(L"python_output");
                    pythonInputMode=winrt::to_string(python.GetNamedString(L"physical_input_mode",L"existing"));
                    pythonProcessed=python.GetNamedNumber(L"processed_fps",0);
                    pythonAccepted=python.GetNamedNumber(L"accepted_movement_commands_per_second",0);
                }
            }catch(...){ }
            const auto row=[&](const std::string& name,double fps,const std::string& state)
            {const auto& s=sample.video;rates<<sample.nowUs<<','<<s.generation<<','<<sample.requested.fps<<','<<s.receivedFramesPerSecond<<','<<s.decodedFramesPerSecond
                <<','<<s.publishedFramesPerSecond<<','<<s.queueDepth<<','<<s.gpuFramesInFlight<<','<<s.transportLostFrames<<','<<s.overloadDrops<<','<<s.decoderFailures
                <<','<<s.readbacksCompleted<<','<<s.cpuPoolExhaustions<<','<<s.readback.p95Ms<<','<<s.publication.p95Ms<<','<<sample.processCpuPercent
                <<','<<sample.systemCpuPercent<<','<<sample.sampleIntervalMs<<','<<CsvQuote(name)<<','<<fps<<','<<CsvQuote(state)
                <<','<<(sample.demand.regions.empty()?"none":sample.demand.streamRate?"stream":"capped")<<','<<sample.demand.maximumFps
                <<','<<sample.demand.schedulerWakeLateness.p95Ms<<','<<CsvQuote(sample.demand.waitMode)<<','<<CsvQuote(mouseBackend)<<','<<mouseCommands<<','<<mouseClicks<<','<<mouseActive<<','<<mouseFaults
                <<','<<CsvQuote(pythonInputMode)<<','<<pythonProcessed<<','<<pythonAccepted<<'\n';};
            if(sample.consumers.empty())row({},0,{});else for(const auto& c:sample.consumers)row(c.name,c.fps,c.state);
        }
        json.flush();csv.flush();rates.flush();
        if(!json.good() || !csv.good() || !rates.good()){result="Writing diagnostics failed";return false;}
        const auto utf8=jsonPath.u8string();result=std::string(utf8.begin(),utf8.end());return true;
    }
}

namespace vanta
{
    std::vector<FpsFinding> DiagnoseStream(const StreamDiagnosticSample& sample,const StreamDiagnosticSample* previous)
    {
        std::vector<FpsFinding> findings;const auto& s=sample.video;
        const DecodedVideoStats empty;const auto& p=previous && previous->video.session==s.session?previous->video:empty;
        const auto add=[&](const char* code,const char* stage,bool problem,const char* confidence,std::string evidence,const char* next)
        {findings.push_back({code,stage,problem?"warning":"info",confidence,std::move(evidence),next,problem});};
        const double target=double(std::max(1,sample.requested.fps)),budget=1000.0/target;
        if(s.failed)add("decoder_failed","decoder",true,"measured",s.status,"Reconnect after saving this report; compare codec, adapter, driver and failure counters.");
        if(sample.connectionTerminated)
        {
            if(sample.terminationError==ML_ERROR_GRACEFUL_TERMINATION)
                add("host_ended_stream","connection",false,"measured","Sunshine signalled that the host ended the stream.",
                    "If this was unintentional, inspect Sunshine's host log at this time. The termination message does not identify why the host closed the session.");
            else add("stream_disconnected","connection",true,"measured",
                "common-c reported stream termination with error "+std::to_string(sample.terminationError)+'.',"Inspect vanta.log and the last rate samples before reconnecting; distinguish transport termination from a decoder recovery failure.");
        }
        if(Delta(s.transportLostFrames,p.transportLostFrames))add("transport_gaps","input",true,"measured",
            std::to_string(Delta(s.transportLostFrames,p.transportLostFrames))+" missing frame numbers since the previous sample.","Compare Sunshine capture/encode FPS and Ethernet errors/traffic on both machines. Gaps alone do not identify the failing link or host stage.");
        if(Low(s.receivedFramesPerSecond,target))add("incoming_below_target","input",true,"measured",
            "Only "+Number(s.receivedFramesPerSecond)+" complete access units/s arrive for "+Number(target)+" requested FPS.",
            "Check Sunshine source refresh/capture/encode statistics and LAN delivery using moving content. The receiver cannot distinguish a slow source from network delivery using this rate alone.");
        if(Delta(s.overloadDrops,p.overloadDrops))add("compressed_queue_overflow","decoder",true,"measured",
            std::to_string(Delta(s.overloadDrops,p.overloadDrops))+" three-packet queue overloads; pending dependent packets require IDR recovery.",
            "Compare compressed queue time, decoder CPU/driver call time and observed GPU completion time. Close previews and other GPU workloads for a manual comparison.");
        if(Delta(s.decoderFailures,p.decoderFailures))add("decoder_input_errors","decoder",true,"measured",
            std::to_string(Delta(s.decoderFailures,p.decoderFailures))+" decoder/input failures since the previous sample.","Compare H.264 and HEVC with fixed settings and inspect driver/codec support; this counter is separate from packet queue overflow.");
        if(Low(s.decodedFramesPerSecond,s.receivedFramesPerSecond))add("decode_below_delivery","decoder",true,"measured",
            Number(s.receivedFramesPerSecond)+" received versus "+Number(s.decodedFramesPerSecond)+" decoded FPS; queue p95 "+Number(s.compressedQueue.p95Ms)
            +" ms, CPU/driver submission p95 "+Number(s.decodeSubmit.p95Ms)+" ms, observed GPU completion p95 "+Number(s.gpuCompletionObserved.p95Ms)+" ms.",
            "Compare these timings and queue/recovery counters. GPU completion includes when the worker observes completion; it is not a pure GPU execution measurement.");
        if(sample.demand.workerFailed)add("capture_worker_failed","readback",true,"measured","Capture worker reported an exception.","Save vanta.log with this report and restart capture.");
        if(sample.demand.regions.empty())add("no_cpu_demand","capture",false,"measured","No CPU subscriber or visible capture preview requests pixels; zero CPU FPS is expected.","Enable Python or another CPU consumer to measure processing FPS. Receiver preview uses the GPU and the UI is capped at 60 FPS.");
        else
        {
            const double expected=std::min(s.decodedFramesPerSecond,sample.demand.streamRate?target:double(sample.demand.maximumFps));
            if(!sample.demand.running)add("capture_stopped","capture",true,"measured","A consumer requests pixels while capture is stopped.","Start capture in the Capture tab.");
            else if(Low(s.publishedFramesPerSecond,expected))add("readback_below_decode","readback",true,"measured",
                Number(s.decodedFramesPerSecond)+" decoded / "+Number(s.publishedFramesPerSecond)+" CPU-published FPS for "+Number(expected)+" expected; GPU/readback/map p95 "
                +Number(s.readback.p95Ms)+" ms, CPU copy p95 "+Number(s.cpuCopy.p95Ms)+" ms, publication dispatch p95 "+Number(s.publicationDispatch.p95Ms)+" ms.",
                "Compare hidden previews and a smaller requested ROI; inspect pool exhaustion and staging-slot pressure. Requested stream settings stay fixed.");
            if(Delta(s.cpuPoolExhaustions,p.cpuPoolExhaustions))add("cpu_snapshot_pool_exhausted","readback",true,"measured",
                std::to_string(Delta(s.cpuPoolExhaustions,p.cpuPoolExhaustions))+" readbacks could not acquire a reusable CPU buffer because immutable snapshots were still retained.",
                "Compare active previews/subscribers and retained-frame consumers. This is buffer ownership pressure, not evidence of packet loss.");
            if(Delta(s.readbackSlotPressure,p.readbackSlotPressure))add("staging_slots_busy","readback",true,"measured",
                std::to_string(Delta(s.readbackSlotPressure,p.readbackSlotPressure))+" new source frames found all three staging slots busy.","Compare readback timing and readback union dimensions; close full-source or large previews for a controlled comparison.");
            if(!sample.demand.streamRate)add("capped_cpu_demand","capture",false,"measured","All active CPU requests are capped; highest requested rate is "+std::to_string(sample.demand.maximumFps)+" FPS.","Enable a stream-rate consumer to measure stream-rate CPU publication. Visible previews are capped at 60 FPS.");
            if(sample.demand.schedulerWakeLateness.samples && sample.demand.schedulerWakeLateness.p95Ms>std::max(2.0,budget*.5))
                add("capture_wake_late","scheduler",true,"measured",
                    "Capture deadline wake lateness p95 is "+Number(sample.demand.schedulerWakeLateness.p95Ms)+" ms; "+sample.demand.waitMode+'.',
                    "Compare process/system CPU load and the wait mode. This measures late deadline observation, not GPU copy duration or decoder time.");
            if(s.publication.samples && s.publication.p95Ms>std::max(12.5,3*budget))add("publication_age_high","latency",true,"measured",
                "Receiver arrival-to-publication p95 is "+Number(s.publication.p95Ms)+" ms.","Locate the largest queue/decode/readback/dispatch timing. This measurement excludes host-to-receiver transit and display presentation.");
            const auto& r=sample.demand.readbackUnion;
            if(r.area()>640*640)add("large_readback_union","capture",false,"measured", "The union of all active regions is "+std::to_string(r.width)+" x "+std::to_string(r.height)+" native pixels; capped regions need not participate in every source frame.",
                "Capture preview size can increase shared readback cost. Python still uses its own exact centered 2 x fov_radius crop and unchanged movement coordinates.");
        }
        for(const auto& consumer:sample.consumers)
        {
            double expected{};bool active{};
            for(const auto& region:sample.demand.regions)if(region.consumer==consumer.name)
            {active=true;expected=std::max(expected,region.request.maximumFps?double(region.request.maximumFps):target);}
            if(!active)continue;
            expected=std::min(expected,s.publishedFramesPerSecond);
            const bool preview=consumer.name.find("preview")!=std::string::npos;
            if(!consumer.processingExpected && !preview)
            {add("consumer_waiting","consumer",false,"measured",consumer.name+": "+consumer.state,"An activation/input wait is deliberate; compare processed FPS only while the feature is actively processing.");continue;}
            if(Low(consumer.fps,expected))add("consumer_below_publication","consumer",true,"measured",
                consumer.name+": "+Number(consumer.fps)+" processed / "+Number(expected)+" expected FPS; processing p95 "+Number(consumer.processing.p95Ms)+" ms; algorithm p95 "+Number(consumer.algorithm.p95Ms)+" ms (samples "+std::to_string(consumer.algorithm.samples)+"). State: "+consumer.state,
                "Processing includes output calls and intentional delays. Compare MAKCU movement lock/write timing, feature settings and preview load before attributing all time to the detector algorithm.");
        }
        if(std::none_of(findings.begin(),findings.end(),[](const auto& f){return f.problem;}))
            add("no_measured_bottleneck","pipeline",false,"measured","Current stage rates meet active demand within the reporting tolerance.","Use the saved history and moving-content comparisons; this does not certify laptop performance acceptance.");
        return findings;
    }
    void AutomaticReportGate::Reset(std::uint64_t now)
    {startedUs_=now;candidateUs_=lastReportUs_=0;reports_=0;candidate_.clear();}
    bool AutomaticReportGate::Due(std::uint64_t now,const std::string& signature,bool terminal)
    {
        if(signature.empty()){candidate_.clear();candidateUs_=0;return false;}
        if(candidate_.empty())candidateUs_=now;
        candidate_=signature;
        if(reports_>=6 || (lastReportUs_ && now-lastReportUs_<60000000))return false;
        return terminal || (now>=startedUs_+8000000 && now>=candidateUs_+4000000);
    }
    void AutomaticReportGate::Queued(std::uint64_t now){lastReportUs_=now;++reports_;}

    struct StreamDiagnostics::Implementation
    {
        std::string hardware{"{}"},summary{"Waiting for a stream sample"};
        mutable std::mutex mutex;
        std::condition_variable_any condition;
        std::jthread writer;
        std::deque<ReportJob> pending;
        std::deque<StreamDiagnosticSample> history;
        std::vector<FpsFinding> findings;
        AutomaticReportGate gate;
        bool active{},initialized{};
        std::uint64_t previousLoadUs{},previousProcessCpu{},previousSystemCpu{},previousSystemIdle{};
        unsigned serial{};
        std::string status;
        std::filesystem::path root;
        void SampleLoad(StreamDiagnosticSample& sample)
        {
            const auto integer=[](const FILETIME& t){return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
            FILETIME creation{},exit{},kernel{},user{},idle{},systemKernel{},systemUser{};
            const bool process=GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)!=FALSE;
            const bool system=GetSystemTimes(&idle,&systemKernel,&systemUser)!=FALSE;
            const auto processCpu=integer(kernel)+integer(user),systemCpu=integer(systemKernel)+integer(systemUser),systemIdle=integer(idle);
            if(previousLoadUs && sample.nowUs>previousLoadUs)
            {
                sample.sampleIntervalMs=double(sample.nowUs-previousLoadUs)/1000.0;
                if(process)
                {sample.processCpuCores=double(Delta(processCpu,previousProcessCpu))/double((sample.nowUs-previousLoadUs)*10);
                    sample.processCpuPercent=100*sample.processCpuCores/std::max<DWORD>(1,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));sample.cpuLoadAvailable=true;}
                const auto total=Delta(systemCpu,previousSystemCpu),idleTime=Delta(systemIdle,previousSystemIdle);
                if(system && total)sample.systemCpuPercent=100.0*double(total-std::min(total,idleTime))/double(total);
            }
            previousLoadUs=sample.nowUs;previousProcessCpu=processCpu;previousSystemCpu=systemCpu;previousSystemIdle=systemIdle;
            PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);
            if(K32GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)))sample.workingSetBytes=memory.WorkingSetSize;
            MEMORYSTATUSEX physical{};physical.dwLength=sizeof(physical);if(GlobalMemoryStatusEx(&physical))sample.availablePhysicalBytes=physical.ullAvailPhys;
        }
        bool Queue(const std::string& trigger)
        {
            if(history.empty())return false;
            std::lock_guard lock(mutex);
            if(pending.size()>=2){status="Diagnostics writer is busy; current history is retained";return false;}
            pending.push_back({trigger,hardware,history,findings});condition.notify_one();return true;
        }
        void Write(std::stop_token stop)
        {
            for(;;)
            {
                ReportJob job;
                {std::unique_lock lock(mutex);condition.wait(lock,stop,[&]{return !pending.empty();});
                    if(pending.empty()){if(stop.stop_requested())return;continue;}
                    job=std::move(pending.front());pending.pop_front();}
                SYSTEMTIME time{};GetLocalTime(&time);wchar_t stem[128]{};
                swprintf_s(stem,L"vanta-report-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u",time.wYear,time.wMonth,time.wDay,
                    time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId(),++serial);
                std::string result;bool success{};
                try
                {
                    success=WriteReport(job,root,stem,result);
                    if(!success)
                    {
                        std::array<wchar_t,32768> local{};
                        if(GetEnvironmentVariableW(L"LOCALAPPDATA",local.data(),DWORD(local.size())))
                            success=WriteReport(job,std::filesystem::path(local.data())/L"Vanta"/L"diagnostics",stem,result);
                    }
                }
                catch(const std::exception& error){result=error.what();}
                if(success)log::Info("Automatic/manual FPS diagnostics saved: %s",result.c_str());
                else log::Error("FPS diagnostics export failed: %s",result.c_str());
                {std::lock_guard lock(mutex);status=success?"Saved "+result+" (+ timings and rates CSV)":"Diagnostics export failed: "+result;}
            }
        }
    };
    StreamDiagnostics::StreamDiagnostics():implementation_(std::make_unique<Implementation>()){}
    StreamDiagnostics::~StreamDiagnostics(){Shutdown();}
    void StreamDiagnostics::Initialize(std::string hardware)
    {
        auto& self=*implementation_;if(self.initialized)return;
        self.hardware=std::move(hardware);std::array<wchar_t,32768> executable{};GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
        self.root=std::filesystem::path(executable.data()).parent_path()/L"diagnostics";self.initialized=true;
        self.writer=std::jthread([&self](std::stop_token stop){self.Write(stop);});
    }
    void StreamDiagnostics::Observe(StreamDiagnosticSample sample)
    {
        auto& self=*implementation_;if(!self.initialized)return;
        if(!self.active){self.history.clear();self.gate.Reset(sample.nowUs);self.active=true;self.previousLoadUs=0;}
        self.SampleLoad(sample);
        self.findings=DiagnoseStream(sample,self.history.empty()?nullptr:&self.history.back());
        std::string signature;self.summary.clear();
        for(const auto& finding:self.findings)if(finding.problem)
        {signature+=finding.code+';';if(self.summary.empty())self.summary=finding.stage+": "+finding.evidence;}
        if(self.summary.empty())self.summary="Stage rates meet active demand; previews are capped at 60 FPS.";
        self.history.push_back(std::move(sample));while(self.history.size()>120)self.history.pop_front();
        const auto now=self.history.back().nowUs;
        if(self.gate.Due(now,signature,self.history.back().video.failed || self.history.back().connectionTerminated) && self.Queue("automatic_sustained_fps_or_latency_issue"))self.gate.Queued(now);
    }
    void StreamDiagnostics::EndSession(const char* reason)
    {
        auto& self=*implementation_;if(!self.active)return;
        if(!self.history.empty() && (self.history.back().video.failed || self.history.back().connectionTerminated || self.history.back().nowUs-self.history.front().nowUs>=3000000))self.Queue(reason);
        self.active=false;
    }
    void StreamDiagnostics::SaveManual(){auto& self=*implementation_;if(!self.Queue("manual")){std::lock_guard lock(self.mutex);if(self.history.empty())self.status="Start a stream to collect FPS diagnostics";}}
    std::string StreamDiagnostics::Summary() const{return implementation_->summary;}
    std::string StreamDiagnostics::Status() const{std::lock_guard lock(implementation_->mutex);return implementation_->status;}
    void StreamDiagnostics::Shutdown()
    {
        auto& self=*implementation_;EndSession("automatic_session_end");
        if(self.writer.joinable()){self.writer.request_stop();self.condition.notify_all();self.writer.join();}self.initialized=false;
    }
    bool RunStreamDiagnosticsSelfTest()
    {
        const auto require=[](bool value,const char* message){if(!value)log::Error("FPS diagnostics self-test: %s",message);return value;};
        StreamDiagnosticSample s;s.nowUs=1000000;s.requested.fps=240;s.video.generation=7;s.video.receivedFramesPerSecond=s.video.decodedFramesPerSecond=240;
        s.video.hardwareActive=true;s.video.publishedFramesPerSecond=0;
        const auto has=[](const auto& findings,const char* code){return std::any_of(findings.begin(),findings.end(),[&](const auto& f){return f.code==code;});};
        if(!require(has(DiagnoseStream(s),"no_cpu_demand") && !has(DiagnoseStream(s),"readback_below_decode"),"idle CPU demand was called slow"))return false;
        s.demand.running=true;s.demand.maximumFps=60;s.demand.regions.push_back({"Python",{FrameRegionKind::centered,120,{},60,true},{1220,660,120,120}});
        s.video.publishedFramesPerSecond=60;s.consumers.push_back({"Python",60,60,{1,2,3,60},{},true,"Preview only"});
        if(!require(!has(DiagnoseStream(s),"readback_below_decode") && !has(DiagnoseStream(s),"consumer_below_publication"),"60 FPS preview compared to 240 FPS"))return false;
        s.demand.streamRate=true;s.demand.regions[0].request.maximumFps=0;s.video.publishedFramesPerSecond=120;
        if(!require(has(DiagnoseStream(s),"readback_below_decode"),"readback bottleneck missing"))return false;
        s.video.publishedFramesPerSecond=240;
        if(!require(has(DiagnoseStream(s),"consumer_below_publication"),"consumer bottleneck missing"))return false;
        s.consumers[0].processingExpected=false;s.consumers[0].state="Waiting for MAKCU";
        if(!require(!has(DiagnoseStream(s),"consumer_below_publication") && has(DiagnoseStream(s),"consumer_waiting"),"input wait called CPU overload"))return false;
        s.video.receivedFramesPerSecond=90;s.video.decodedFramesPerSecond=90;
        if(!require(has(DiagnoseStream(s),"incoming_below_target") && !has(DiagnoseStream(s),"decode_below_delivery"),"incoming bottleneck attributed to decoder"))return false;
        auto previous=s;s.video.cpuPoolExhaustions=1;
        if(!require(has(DiagnoseStream(s,&previous),"cpu_snapshot_pool_exhausted") && !has(DiagnoseStream(s,&s),"cpu_snapshot_pool_exhausted"),"old pressure counter replayed"))return false;
        auto resized=s;++resized.video.generation;
        if(!require(!has(DiagnoseStream(resized,&s),"cpu_snapshot_pool_exhausted"),"geometry generation change replayed historical pressure"))return false;
        auto ended=s;ended.connectionTerminated=true;ended.terminationError=ML_ERROR_GRACEFUL_TERMINATION;
        if(!require(has(DiagnoseStream(ended),"host_ended_stream") && !has(DiagnoseStream(ended),"stream_disconnected"),"graceful host closure called a connection failure"))return false;
        auto late=s;late.demand.schedulerWakeLateness={3,4,5,100};late.demand.waitMode="fixture timer";
        if(!require(has(DiagnoseStream(late),"capture_wake_late"),"late capture wake missing from diagnosis"))return false;
        AutomaticReportGate gate;gate.Reset(1000000);
        if(!require(!gate.Due(2000000,"low",false) && !gate.Due(6000000,"low",false) && gate.Due(9000000,"low",false),"warmup / persistence gate"))return false;
        gate.Queued(9000000);
        if(!require(!gate.Due(10000000,"other",true) && gate.Due(69000000,"other",false),"report cooldown"))return false;
        gate.Reset(1000000);gate.Due(2000000,"low",false);gate.Due(6000000,{},false);
        if(!require(!gate.Due(9000000,"low",false),"resolved issue was treated as sustained"))return false;
        std::ostringstream serialized;serialized.imbue(std::locale::classic());s.outputJson="{\"test\":true}";s.consumers[0].state="quote \" and newline\n";SampleJson(serialized,s);
        try{const auto parsed=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(serialized.str()));
            if(!require(parsed.GetNamedObject(L"video").GetNamedNumber(L"generation")==7,"JSON association"))return false;}
        catch(...){return require(false,"invalid diagnostics JSON");}
        // Exercise the actual export path without a graphics device or a
        // stream. Remove only the three files created in this unique temp dir.
        std::array<wchar_t,32768> temporary{};
        if(!GetTempPathW(DWORD(temporary.size()),temporary.data()))return require(false,"temporary directory unavailable");
        const auto root=std::filesystem::path(temporary.data())/(L"Vanta-fps-selftest-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        struct Cleanup
        {
            std::filesystem::path root;
            ~Cleanup(){std::error_code error;for(const auto* file:{L"fixture.json",L"fixture-timings.csv",L"fixture-rates.csv"})std::filesystem::remove(root/file,error);std::filesystem::remove(root,error);}
        } cleanup{root};
        s.publishedMetadata.generation=7;s.publishedMetadata.frameNumber=99;
        ReportJob job{"self_test","{\"fixture\":true}",{previous,s},DiagnoseStream(s,&previous)};std::string path;
        if(!require(WriteReport(job,root,L"fixture",path),"JSON/CSV export failed"))return false;
        try
        {
            std::ifstream file(root/L"fixture.json",std::ios::binary);std::string json((std::istreambuf_iterator<char>(file)),{});
            const auto parsed=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));
            if(!require(parsed.GetNamedArray(L"history").Size()==2 && parsed.GetNamedObject(L"current").GetNamedObject(L"cpu_publication_metadata").GetNamedNumber(L"frame_number")==99,"export lost history / metadata association"))return false;
            std::ifstream ratesFile(root/L"fixture-rates.csv",std::ios::binary);std::string rates((std::istreambuf_iterator<char>(ratesFile)),{});
            if(!require(rates.find("\"quote \"\" and newline\n\"")!=std::string::npos && std::filesystem::file_size(root/L"fixture-timings.csv")>100,"CSV escaping / summaries"))return false;
        }
        catch(...){return require(false,"exported report is invalid");}
        gate.Reset(1000000);
        gate.Due(2000000,"low",false);
        for(unsigned i=0;i<6;++i){if(!require(gate.Due(9000000+i*60000000ULL,"low",false),"eligible report suppressed"))return false;gate.Queued(9000000+i*60000000ULL);}
        if(!require(!gate.Due(400000000,"low",true),"automatic report count was unbounded"))return false;
        log::Info("FPS diagnostics self-test passed: classification, idle/capped demand, input waits, counter deltas, persistence/cooldown/report bounds, JSON/CSV export and metadata (CPU only)");return true;
    }
}
