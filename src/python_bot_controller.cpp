#define IMGUI_DEFINE_MATH_OPERATORS
#include "python_bot_controller.hpp"
#include "preview_mask.hpp"
#include "capture_controller.hpp"
#include "makcu_controller.hpp"
#include "logger.hpp"
#include "input_backend_utils.hpp"
#include <winrt/base.h>
#include "imgui.h"
#include "custom_widgets.hpp"
#include "font_defines.h"
#include <Windows.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace
{
    std::filesystem::path DefaultConfigPath()
    {
        std::array<wchar_t,32768> path{};
        const DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        return std::filesystem::path(std::wstring(path.data(),length)).parent_path()/L"python-config.json";
    }
    bool ReadJson(const std::filesystem::path& path,std::string& json,std::string& error)
    {
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if (!input) { error="Cannot open the selected JSON file"; return false; }
        const auto size=input.tellg();
        if (size<0 || size>1024*1024) { error="Configuration exceeds 1 MiB"; return false; }
        input.seekg(0);
        json.assign(std::istreambuf_iterator<char>(input),{});
        return input.good() || input.eof();
    }
    bool WriteJson(const std::filesystem::path& path,const std::string& json,std::string& error)
    {
        auto temporary=path;
        temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
        {
            std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
            file<<json;
            if (!file) { error="Could not write the temporary JSON file"; return false; }
        }
        if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        { error="Could not atomically replace the JSON file; original retained"; return false; }
        return true;
    }
}

namespace vanta
{
    struct PythonBotController::Implementation
    {
        CaptureController* capture{};
        CaptureSubscription frameSubscription;
        std::atomic_uint64_t previewVisibleUntil{0};
        MakcuController* makcu{};
        mutable std::mutex settingsMutex;
        PythonBotConfig config;
        std::string configJson;
        std::atomic_bool enabled{false};
        std::atomic<std::uint64_t> revision{0};
        std::atomic<std::uint64_t> frames{0};
        std::atomic<std::uint64_t> moves{0};
        std::jthread worker;
        mutable std::mutex diagnosticMutex;
        std::shared_ptr<const PythonBotResult> latest;
        CapturedFrameMetadata latestMetadata;
        std::int64_t captureTimestamp{};
        bool physicalInputReady{},physicalActivationHeld{};
        cv::Point lastRequested;
        std::uint64_t lastOutputGeneration{};
        PythonAxisMode lastAxisMode{PythonAxisMode::standard};
        PythonPhysicalInputMode lastPhysicalInputMode{PythonPhysicalInputMode::existing};
        std::deque<std::uint64_t> processedTimes,acceptedMoveTimes;
        std::uint32_t lastEffectiveAxes{};
        std::string status{"Disabled; enable to arm MAKCU movement"};
        std::array<char,1024> path{};
        bool preview{true};
        bool preFovPreview{};

        void Status(std::string message)
        {
            std::lock_guard lock(diagnosticMutex);
            status=std::move(message);
        }
        bool Apply(const std::string& json,std::string& error)
        {
            std::uint64_t disabledGeneration{};
            {std::lock_guard lock(settingsMutex);enabled.store(false,std::memory_order_release);
                disabledGeneration=revision.fetch_add(1,std::memory_order_acq_rel)+1;}
            if(makcu)makcu->SetInputPolicy(InputOwner::python,false,disabledGeneration,MouseButtonBinding::left);
            PythonBotConfig parsed;
            if (!ParsePythonBotConfig(json,parsed,error)) { Status("Invalid configuration: "+error); return false; }
            {
                std::lock_guard lock(settingsMutex);
                config=std::move(parsed);
                configJson=json;
                revision.fetch_add(1,std::memory_order_relaxed);
            }
            Status("Configuration loaded; movement is disabled until explicitly armed");
            return true;
        }
        void Run(std::stop_token stop)
        {
            log::Info("Python mode worker started (native reference algorithm, Sunshine-only frames)");
            std::uint64_t lastSequence{};
            bool activationWasHeld{};
            MouseButtonBinding previousActivationButton{MouseButtonBinding::left};
            PythonAxisMode previousAxisMode{PythonAxisMode::standard};
            std::chrono::steady_clock::time_point hybridWindowStarted{};
            while (!stop.stop_requested())
            {
                PythonBotConfig snapshot;
                std::uint64_t generation{};
                bool armed{};
                { std::lock_guard lock(settingsMutex); snapshot=config; generation=revision.load(); armed=enabled.load(); }
                const auto activation=PythonAimButton(snapshot.aimKey);
                if(makcu)makcu->SetInputPolicy(InputOwner::python,armed && activation.has_value(),generation,activation.value_or(MouseButtonBinding::left));
                const bool visible=StreamNowUs()<previewVisibleUntil.load();
                frameSubscription.Update({FrameRegionKind::centered,snapshot.fovRadius*2,{},armed ? 0 : 60,armed || visible});
                CpuFrameSnapshot input;
                if (!capture || !frameSubscription.Wait(lastSequence,input,20,stop))
                {
                    Status("Waiting for a fresh Sunshine frame");
                    if(capture)capture->SetConsumerState("Python",armed || visible,"Waiting for a fresh Sunshine frame");
                    continue;
                }
                const auto& metadata=input.metadata;
                lastSequence=metadata.sequence;
                CaptureWorkTimer workTimer(capture,"Python",metadata);
                CorrectionScope correction(makcu,InputOwner::python,generation);
                try
                {
                    const auto algorithmStarted=StreamNowUs();
                    auto detected=ProcessPythonBotRoi(input.bgra,snapshot,
                        {metadata.cropX,metadata.cropY,input.bgra.cols,input.bgra.rows});
                    capture->RecordConsumerAlgorithm("Python",metadata,algorithmStarted);
                    const auto button=PythonAimButton(snapshot.aimKey);
                    const bool buttonInputAvailable=armed && button && makcu && makcu->IsMouseButtonInputAvailable();
                    const bool activationHeld=buttonInputAvailable && makcu->IsMouseButtonPressed(*button);
                    cv::Point requestedForFrame;
                    bool acceptedForFrame{};
                    if (!activationHeld) activationWasHeld=false;
                    bool hybridVerticalActive=true;
                    std::string nextStatus;
                    if (!detected.valid) nextStatus=detected.error;
                    else if (!armed) nextStatus="Preview only; movement disabled";
                    else if (!button) nextStatus="aim_key is not a MAKCU physical mouse button";
                    else if (!buttonInputAvailable) nextStatus="Waiting for MAKCU physical-button input";
                    else if (!activationHeld) nextStatus="Waiting for the configured MAKCU activation button";
                    else
                    {
                        const auto now=std::chrono::steady_clock::now();
                        if (!activationWasHeld || previousActivationButton!=*button || previousAxisMode!=snapshot.axisMode)
                            hybridWindowStarted=now;
                        activationWasHeld=true;
                        previousActivationButton=*button;
                        previousAxisMode=snapshot.axisMode;
                        if (snapshot.axisMode==PythonAxisMode::hybrid)
                        {
                            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-hybridWindowStarted).count();
                            hybridVerticalActive=elapsed<snapshot.hybridVerticalTimeMilliseconds;
                            if (!hybridVerticalActive) detected.movement.y=0;
                        }
                        const auto requested=detected.movement;
                        requestedForFrame=requested;
                        if (!detected.target) nextStatus="Activation held; no target";
                        else if (requested==cv::Point{})
                        {
                            if (snapshot.axisMode==PythonAxisMode::horizontalOnly)
                                nextStatus="Horizontal only: target present; generated X movement is zero";
                            else if (snapshot.axisMode==PythonAxisMode::hybrid && !hybridVerticalActive)
                                nextStatus="Hybrid: vertical window ended; generated X movement is zero";
                            else nextStatus="Target present; integer-truncated movement is zero";
                        }
                        else if (enabled.load(std::memory_order_acquire) && !stop.stop_requested())
                        {
                            const int dx=requested.x,dy=requested.y;
                            if (dx<-32768 || dx>32767 || dy<-32768 || dy>32767)
                            {
                                std::uint64_t disabledGeneration{};
                                {std::lock_guard lock(settingsMutex);enabled.store(false,std::memory_order_release);disabledGeneration=revision.fetch_add(1)+1;}
                                makcu->SetInputPolicy(InputOwner::python,false,disabledGeneration,*button);
                                nextStatus="Report exceeds signed 16-bit device limits; movement disabled";
                            }
                            else if ([&]
                            {
                                OutputContext output;output.owner=InputOwner::python;output.generation=generation;
                                output.streamGeneration=metadata.generation;output.streamSession=metadata.session;output.sequence=metadata.sequence;
                                output.frameTimeUs=metadata.timing.receivedUs?metadata.timing.receivedUs:metadata.timing.publishedUs;
                                output.activation=*button;output.axes=(snapshot.axisMode==PythonAxisMode::horizontalOnly ||
                                    (snapshot.axisMode==PythonAxisMode::hybrid && !hybridVerticalActive))?InputLockX:InputLockX|InputLockY;
                                output.physicalInputMode=snapshot.physicalInputMode;
                                output.isCurrent=[this,metadata,generation]{return enabled.load() && revision.load()==generation && capture && capture->IsCurrent(metadata);};
                                return capture->IsCurrent(metadata) && makcu->TryMove(dx,dy,output);
                            }())
                            {
                                correction.Submitted();
                                acceptedForFrame=true;
                                moves.fetch_add(1,std::memory_order_relaxed);
                                if (snapshot.axisMode==PythonAxisMode::horizontalOnly)
                                    nextStatus="Moving horizontally through MAKCU";
                                else if (snapshot.axisMode==PythonAxisMode::hybrid)
                                    nextStatus=hybridVerticalActive ? "Hybrid: moving X + Y through MAKCU" : "Hybrid: moving horizontally through MAKCU";
                                else nextStatus="Moving through MAKCU";
                                if(snapshot.physicalInputMode==PythonPhysicalInputMode::fuse)
                                    nextStatus="Fuse inputs: guidance active; matching manual motion passes, opposing motion is blocked per guided axis";
                                if (snapshot.makcuDebug) log::Info("Python mode MAKCU move(%d,%d), axis_mode=%s, frame=%llu",dx,dy,
                                    PythonAxisModeJsonName(snapshot.axisMode),static_cast<unsigned long long>(metadata.sequence));
                            }
                            else nextStatus=capture->IsCurrent(metadata)?"Move not sent: "+makcu->MovementBlockReason(InputOwner::python):"Move not sent: source stream generation changed";
                        }
                    }
                    frames.fetch_add(1,std::memory_order_release);
                    correction.EndReason(nextStatus);
                    if(capture)capture->SetConsumerState("Python",armed || visible,nextStatus.c_str());
                    std::shared_ptr<const PythonBotResult> published=std::make_shared<PythonBotResult>(std::move(detected));
                    {
                        std::lock_guard lock(diagnosticMutex);
                        latest.swap(published);latestMetadata=metadata;
                        captureTimestamp=metadata.captureTimestampNanoseconds;
                        physicalInputReady=buttonInputAvailable;physicalActivationHeld=activationHeld;
                        lastRequested=requestedForFrame;lastOutputGeneration=generation;
                        lastAxisMode=snapshot.axisMode;
                        lastPhysicalInputMode=snapshot.physicalInputMode;
                        const auto at=StreamNowUs();processedTimes.push_back(at);if(acceptedForFrame)acceptedMoveTimes.push_back(at);
                        while(!processedTimes.empty() && at-processedTimes.front()>1000000)processedTimes.pop_front();
                        while(!acceptedMoveTimes.empty() && at-acceptedMoveTimes.front()>1000000)acceptedMoveTimes.pop_front();
                        lastEffectiveAxes=activationHeld && latest && latest->target && requestedForFrame!=cv::Point{}?
                            (snapshot.axisMode==PythonAxisMode::horizontalOnly ||
                                (snapshot.axisMode==PythonAxisMode::hybrid && !hybridVerticalActive)?InputLockX:InputLockX|InputLockY):0U;
                        status=std::move(nextStatus);
                    }
                }
                catch (const std::exception& exception)
                {
                    std::uint64_t disabledGeneration{};
                    {std::lock_guard lock(settingsMutex);enabled.store(false,std::memory_order_release);disabledGeneration=revision.fetch_add(1)+1;}
                    if(makcu)makcu->SetInputPolicy(InputOwner::python,false,disabledGeneration,MouseButtonBinding::left);
                    Status(std::string("Python mode error; disabled: ")+exception.what());
                }
            }
            log::Info("Python mode worker stopped");
        }
        void Preview(const PythonBotResult& result)
        {
            const auto& mask=preFovPreview ? result.colorMask : result.fovMask;
            if (mask.empty()) { ImGui::TextDisabled("Waiting for a Sunshine frame."); return; }
            const float side=std::max(1.0F,std::min(320.0F,ImGui::GetContentRegionAvail().x));
            const float scale=side/static_cast<float>(mask.cols);
            if(!ImGui::IsRectVisible(ImVec2(side,side))){ImGui::Dummy(ImVec2(side,side));return;}
            const ImVec2 origin=ImGui::GetCursorScreenPos();
            auto* draw=ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin,origin+ImVec2(side,side),IM_COL32(10,12,18,255));
            draw->PushClipRect(origin,origin+ImVec2(side,side),true);
            DrawPreviewMask(draw,"python",mask,origin,origin+ImVec2(side,side),IM_COL32(180,150,235,230));
            for (const auto& blob:result.blobs)
                draw->AddRect(origin+ImVec2(blob.bounds.x*scale,blob.bounds.y*scale),
                    origin+ImVec2((blob.bounds.x+blob.bounds.width)*scale,(blob.bounds.y+blob.bounds.height)*scale),
                    IM_COL32(70,255,120,255));
            if (result.target)
            {
                const ImVec2 point=origin+ImVec2(result.target->x*scale,result.target->y*scale);
                draw->AddLine(point-ImVec2(5,0),point+ImVec2(5,0),IM_COL32(255,225,50,255),2);
                draw->AddLine(point-ImVec2(0,5),point+ImVec2(0,5),IM_COL32(255,225,50,255),2);
            }
            draw->PopClipRect();
            ImGui::Dummy(ImVec2(side,side));
        }
    };

    PythonBotController::PythonBotController():implementation_(std::make_unique<Implementation>()) {}
    PythonBotController::~PythonBotController() { Shutdown(); }
    void PythonBotController::Initialize(CaptureController* capture,MakcuController* makcu,const std::string& json)
    {
        auto& impl=*implementation_;
        impl.capture=capture;
        if(capture)impl.frameSubscription=capture->Subscribe({FrameRegionKind::centered,240,{},60,false},"Python");
        impl.makcu=makcu;
        const auto initialPath=DefaultConfigPath().u8string();
        const std::string pathText(initialPath.begin(),initialPath.end());
        std::copy_n(pathText.data(),std::min(pathText.size(),impl.path.size()-1),impl.path.data());
        std::string error;
        if (!impl.Apply(json.empty() ? DefaultPythonBotJson() : json,error))
            impl.Apply(DefaultPythonBotJson(),error);
        impl.worker=std::jthread([state=&impl](std::stop_token stop) { state->Run(stop); });
    }
    void PythonBotController::Shutdown()
    {
        auto& impl=*implementation_;
        SetEnabled(false);
        if (impl.worker.joinable()) { impl.worker.request_stop(); impl.worker.join(); }
        if (impl.makcu) impl.makcu->CancelOutput(InputOwner::python);
        impl.frameSubscription={};
        impl.capture=nullptr;
        impl.makcu=nullptr;
    }
    bool PythonBotController::ApplyConfigJson(const std::string& json,std::string& error)
    { return implementation_->Apply(json,error); }
    std::string PythonBotController::GetConfigJson() const
    { const auto& impl=*implementation_; std::lock_guard lock(impl.settingsMutex); return impl.configJson; }
    PythonBotConfig PythonBotController::GetConfig() const
    { const auto& impl=*implementation_; std::lock_guard lock(impl.settingsMutex); return impl.config; }
    std::uint64_t PythonBotController::SettingsRevision() const noexcept { return implementation_->revision.load(); }
    bool PythonBotController::IsEnabled() const noexcept { return implementation_->enabled.load(); }
    std::uint64_t PythonBotController::ProcessedFrames() const noexcept { return implementation_->frames.load(); }
    std::string PythonBotController::DiagnosticsJson() const
    {
        const auto& s=*implementation_;PythonPhysicalInputMode selectedMode;
        {std::lock_guard lock(s.settingsMutex);selectedMode=s.config.physicalInputMode;}
        std::lock_guard lock(s.diagnosticMutex);const auto now=StreamNowUs();
        const auto rate=[&](const auto& times){return std::count_if(times.begin(),times.end(),[&](auto at){return now>=at && now-at<=1000000;});};
        std::ostringstream out;out<<"{\"enabled\":"<<(s.enabled?"true":"false")<<",\"status\":"<<InputJsonString(s.status)
            <<",\"configuration_generation\":"<<s.revision.load()<<",\"last_output_generation\":"<<s.lastOutputGeneration
            <<",\"processed_total\":"<<s.frames.load()<<",\"movement_requests_accepted\":"<<s.moves.load()
            <<",\"processed_fps\":"<<rate(s.processedTimes)<<",\"accepted_movement_commands_per_second\":"<<rate(s.acceptedMoveTimes)
            <<",\"physical_input_mode\":"<<InputJsonString(PythonPhysicalInputModeJsonName(selectedMode))
            <<",\"last_processed_physical_input_mode\":"<<InputJsonString(PythonPhysicalInputModeJsonName(s.lastPhysicalInputMode))
            <<",\"physical_input_ready\":"<<(s.physicalInputReady?"true":"false")
            <<",\"physical_activation_held\":"<<(s.physicalActivationHeld?"true":"false")
            <<",\"target_present\":"<<(s.latest && s.latest->target?"true":"false")
            <<",\"axis_mode\":"<<InputJsonString(PythonAxisModeJsonName(s.lastAxisMode))<<",\"effective_axes\":"<<s.lastEffectiveAxes
            <<",\"requested_x\":"<<s.lastRequested.x<<",\"requested_y\":"<<s.lastRequested.y
            <<",\"frame_sequence\":"<<s.latestMetadata.sequence<<",\"stream_generation\":"<<s.latestMetadata.generation
            <<",\"frame_age_us\":"<<(s.latestMetadata.timing.receivedUs && now>=s.latestMetadata.timing.receivedUs?now-s.latestMetadata.timing.receivedUs:0)<<'}';
        return out.str();
    }
    bool PythonBotController::SetEnabled(bool enabled)
    {
        auto& impl=*implementation_;
        std::string error;std::uint64_t generation{};std::optional<MouseButtonBinding> button;
        {
            std::lock_guard lock(impl.settingsMutex);button=PythonAimButton(impl.config.aimKey);
            if(enabled && (!ValidatePythonBotConfig(impl.config,error) || !button))
                error="Cannot arm: invalid config or unsupported MAKCU activation key";
            if(enabled && error.empty() && impl.config.physicalInputMode==PythonPhysicalInputMode::fuse)
                error=impl.makcu?impl.makcu->FuseUnavailableReason():"Fuse inputs requires a connected Medius output device";
            if(error.empty())
            {generation=impl.revision.fetch_add(1,std::memory_order_acq_rel)+1;impl.enabled.store(enabled,std::memory_order_release);}
        }
        if(!error.empty()){impl.Status(error);return false;}
        if(impl.makcu)
            impl.makcu->SetInputPolicy(InputOwner::python,enabled && button.has_value(),generation,button.value_or(MouseButtonBinding::left));
        return true;
    }
    void PythonBotController::RenderPanel()
    {
        auto& impl=*implementation_;
        custom::Child(ICON_CODE_LINE "  Python##python-bot",ImVec2(0,std::max(190.0F,ImGui::GetContentRegionAvail().y-40)),true);
        bool enabled=IsEnabled();
        if (custom::Checkbox("Enable Python movement",&enabled)) SetEnabled(enabled);
        if(impl.makcu && !impl.makcu->PythonConnectionNote().empty())
            ImGui::TextWrapped("%s",impl.makcu->PythonConnectionNote().c_str());
        ImGui::TextWrapped("Native recreation of the supplied colorbot.py. Activation comes from MAKCU; pixels come only from Sunshine. TestMove and Python movement are mutually exclusive.");
        PythonBotConfig config=GetConfig();
        ImGui::TextDisabled("Existing behavior follows Mouse Output lock settings. Fuse inputs uses Medius direction filters for Python.");
        if(config.physicalInputMode==PythonPhysicalInputMode::existing && impl.makcu && !impl.makcu->GetConfig().lockMovementWhileCorrecting)
            ImGui::TextWrapped("Movement locking is disabled in Mouse Output; physical mouse movement is allowed during Python corrections.");
        ImGui::InputText("Config JSON path",impl.path.data(),impl.path.size());
        if (ImGui::Button("Load JSON"))
        {
            std::string text,error;
            if (ReadJson(std::filesystem::path(winrt::to_hstring(impl.path.data()).c_str()),text,error)) ApplyConfigJson(text,error);
            if (!error.empty()) impl.Status(error);
            config=GetConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save JSON"))
        {
            std::string error;
            if (WriteJson(std::filesystem::path(winrt::to_hstring(impl.path.data()).c_str()),GetConfigJson(),error)) impl.Status("JSON saved atomically");
            else impl.Status(error);
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore supplied config"))
        {
            std::string error;
            ApplyConfigJson(DefaultPythonBotJson(),error);
            config=GetConfig();
        }
        bool changed=false;
        std::vector<const char*> presets;
        int selected=0;
        for (std::size_t i=0;i<config.presets.size();++i)
        {
            const auto& preset=config.presets[i];
            presets.push_back(preset.key=="purple_narrow" && !preset.name.empty()?preset.name.c_str():preset.key.c_str());
            if (config.presets[i].key==config.activePreset) selected=static_cast<int>(i);
        }
        if (!presets.empty() && ImGui::Combo("active_preset",&selected,presets.data(),static_cast<int>(presets.size())))
        { config.activePreset=config.presets[static_cast<std::size_t>(selected)].key; changed=true; }
        changed|=ImGui::InputInt("fov_radius (decoded px)",&config.fovRadius);
        ImGui::Text("Independent centered crop: %d x %d decoded pixels; no resolution scaling",config.fovRadius*2,config.fovRadius*2);
        changed|=ImGui::InputDouble("smooth_factor (double)",&config.smoothFactor,0.001,0.01,"%.17g");
        const char* physicalModes[]{"Existing behavior","Fuse inputs"};
        int physicalMode=config.physicalInputMode==PythonPhysicalInputMode::fuse?1:0;
        if(ImGui::Combo("Physical input",&physicalMode,physicalModes,IM_ARRAYSIZE(physicalModes)))
        {config.physicalInputMode=physicalMode?PythonPhysicalInputMode::fuse:PythonPhysicalInputMode::existing;changed=true;}
        if(config.physicalInputMode==PythonPhysicalInputMode::fuse)
        {
            ImGui::TextWrapped("Matching manual movement contributes at 100%%; opposing movement is blocked on each guided axis. Axes without a correction remain manual. Guidance also moves while your mouse is still.");
            ImGui::TextWrapped("Release the activation button (%s) to regain full manual control. Fuse replaces Python's movement lock; click and activation-button reservations stay independent.",config.aimKey.c_str());
            const auto unavailable=impl.makcu?impl.makcu->FuseUnavailableReason():"Medius output is unavailable";
            if(!unavailable.empty())ImGui::TextWrapped("Cannot arm Fuse inputs: %s",unavailable.c_str());
        }
        const char* axisModes[]{"X + Y","Horizontal only","Hybrid"};
        int axisMode=config.axisMode==PythonAxisMode::horizontalOnly ? 1 : config.axisMode==PythonAxisMode::hybrid ? 2 : 0;
        if (ImGui::Combo("Axis mode",&axisMode,axisModes,IM_ARRAYSIZE(axisModes)))
        {
            config.axisMode=axisMode==1 ? PythonAxisMode::horizontalOnly : axisMode==2 ? PythonAxisMode::hybrid : PythonAxisMode::standard;
            changed=true;
        }
        if (config.axisMode==PythonAxisMode::horizontalOnly)
            ImGui::TextWrapped("Generated movement is (X, 0). Target selection stays the same; physical vertical mouse movement remains available.");
        else if (config.axisMode==PythonAxisMode::hybrid)
        {
            changed|=ImGui::InputInt("Hybrid vertical time (ms)",&config.hybridVerticalTimeMilliseconds,10,100);
            ImGui::TextWrapped("After each activation press, Hybrid sends X + Y for this interval, then sends X only until release. Detection and target selection remain unchanged.");
        }
        changed|=ImGui::InputDouble("min_contour_area",&config.minimumArea,1,5,"%.3f");
        changed|=ImGui::InputInt("offset_x",&config.offsetX);
        changed|=ImGui::InputInt("offset_y",&config.offsetY);
        const char* buttons[]{"Left (0x01)","Right (0x02)","Middle (0x04)","X1 (0x05)","X2 (0x06)"};
        const char* keys[]{"0x01","0x02","0x04","0x05","0x06"};
        int buttonIndex=-1;
        const auto currentButton=PythonAimButton(config.aimKey);
        if (!currentButton)
            ImGui::TextWrapped("Unsupported imported aim_key: %s. Choose a physical MAKCU mouse button to arm.",config.aimKey.c_str());
        for (int i=0;i<5;++i) if (currentButton && PythonAimButton(keys[i])==currentButton) buttonIndex=i;
        if (ImGui::Combo("aim_key (MAKCU)",&buttonIndex,buttons,5))
        { config.aimKey=keys[buttonIndex]; changed=true; }
        changed|=ImGui::Checkbox("filter_orbs (stored; unused by original)",&config.filterOrbs);
        if (!config.presets.empty() && ImGui::CollapsingHeader("Selected preset HSV / metadata"))
        {
            auto& preset=config.presets[static_cast<std::size_t>(selected)];
            ImGui::TextWrapped("%s",preset.name.c_str());
            for (std::size_t range=0;range<preset.ranges.size();++range)
            {
                ImGui::PushID(static_cast<int>(range));
                changed|=ImGui::InputInt3("HSV lower",preset.ranges[range].lower.data());
                changed|=ImGui::InputInt3("HSV upper",preset.ranges[range].upper.data());
                ImGui::PopID();
            }
            ImGui::TextDisabled("hex_variations and unknown JSON fields are preserved; HSV alone drives detection.");
        }
        if (ImGui::CollapsingHeader("Screen / MAKCU settings"))
        {
            changed|=ImGui::Checkbox("makcu_auto_reconnect",&config.makcuAutoReconnect);
            changed|=ImGui::Checkbox("makcu_debug (trace moves)",&config.makcuDebug);
            changed|=ImGui::InputInt("screen_width (original metadata)",&config.screenWidth);
            changed|=ImGui::InputInt("screen_height (original metadata)",&config.screenHeight);
            ImGui::TextWrapped("screen_width/height are metadata; decoded dimensions drive the crop. MAKCU uses the automatically detected Device in Mouse output. Imported Python COM-port fields are ignored and are removed when this JSON is saved.");
        }
        if (changed)
        {
            std::string error;
            if (!ValidatePythonBotConfig(config,error)) { SetEnabled(false); impl.Status(error); }
            else
            {
                const bool wasEnabled=IsEnabled();
                ApplyConfigJson(SerializePythonBotConfig(config,GetConfigJson()),error);
                if (wasEnabled) SetEnabled(true);
            }
        }
        std::shared_ptr<const PythonBotResult> resultSnapshot;
        std::string status;
        std::int64_t timestamp{};
        { std::lock_guard lock(impl.diagnosticMutex); resultSnapshot=impl.latest; status=impl.status; timestamp=impl.captureTimestamp;
            if(impl.capture && !impl.capture->IsCurrent(impl.latestMetadata)){resultSnapshot.reset();timestamp=0;status="Waiting for the current stream generation";} }
        const PythonBotResult emptyResult;
        const auto& result=resultSnapshot ? *resultSnapshot : emptyResult;
        ImGui::Separator();
        ImGui::TextWrapped("%s",status.c_str());
        if(config.physicalInputMode==PythonPhysicalInputMode::fuse && impl.makcu && ImGui::CollapsingHeader("Input control status"))
            ImGui::TextWrapped("%s",impl.makcu->InputControlStatus().c_str());
        {
            std::lock_guard lock(impl.diagnosticMutex);const auto at=StreamNowUs();
            const auto rate=[&](const auto& times){return std::count_if(times.begin(),times.end(),[&](auto time){return at>=time && at-time<=1000000;});};
            ImGui::Text("Python processed: %lld FPS | accepted movement commands: %lld /s",
                static_cast<long long>(rate(impl.processedTimes)),static_cast<long long>(rate(impl.acceptedMoveTimes)));
        }
        ImGui::TextDisabled("Capture FPS is reported separately in Capture / diagnostics. Command rate is not delivered human contribution.");
        const auto now=std::int64_t(StreamNowUs()*1000);
        ImGui::Text("Frames %llu | moves %llu | local readback age %.1f ms",
            static_cast<unsigned long long>(impl.frames.load()),static_cast<unsigned long long>(impl.moves.load()),
            timestamp ? (now-timestamp)/1000000.0 : 0.0);
        ImGui::Text("Contours %zu | blobs %zu | rejected area/size/flat/orb %zu/%zu/%zu/%zu",
            result.validContours.size(),result.blobs.size(),result.rejections[0],result.rejections[1],result.rejections[2],result.rejections[3]);
        if (result.target) ImGui::Text("Aim %d,%d | calculated move: %d,%d",result.target->x,result.target->y,result.movement.x,result.movement.y);
        ImGui::Checkbox("Mask / contour / aim preview",&impl.preview);
        if (impl.preview)
        {
            ImGui::Checkbox("Show mask before circular FOV",&impl.preFovPreview);
            const float side=std::max(1.0F,std::min(320.0F,ImGui::GetContentRegionAvail().x));
            if(ImGui::IsRectVisible(ImVec2(side,side)))impl.previewVisibleUntil=StreamNowUs()+100000;
            impl.Preview(result);
        }
        custom::EndChild();
    }
}
