#pragma once

#include "app_settings.hpp"
#include "input_locks.hpp"

#include <cstdint>
#include <memory>

namespace vanta
{
    bool RunMakcuCompatibilitySelfTest();
    bool RunMakcuConnectionProbe(const std::string& port);
    class MakcuController
    {
    public:
        MakcuController();
        ~MakcuController();

        MakcuController(const MakcuController&) = delete;
        MakcuController& operator=(const MakcuController&) = delete;

        bool Initialize(
            const MouseOutputConfig* initialConfig = nullptr);
        void Shutdown();
        void Tick();
        void RenderPanel();
        // Attempts one left-click through the selected output backend.
        // Returns true after release/readback and physical-mask restoration.
        bool TryClick();
        bool TryClick(const OutputContext& context);
        bool TryMove(int x,int y,const OutputContext& context);
        std::string MovementBlockReason(InputOwner owner) const;
        ClickOutputStatus ClickStatus(InputOwner owner) const;
        std::string FuseUnavailableReason() const;
        std::string InputControlStatus() const;
        void SetInputPolicy(InputOwner owner,bool enabled,std::uint64_t generation,MouseButtonBinding activation);
        void EndCorrection(InputOwner owner,std::uint64_t generation,const char* reason="Correction ended");
        void CancelOutput(InputOwner owner);
        void ReleaseAllInput();
        std::uint64_t EmergencyRevision() const noexcept;
        // Sends a relative mouse move (dx, dy) through the connected MAKCU device.
        // Both backends return serial-command acceptance.
        // Neither reports an acknowledgement of actual USB movement delivery.
        bool TryMove(int x, int y);
        // Automation uses TryMove(OutputContext), including frame/config
        // generations and physical activation, rather than an unscoped send.
        void ActivatePythonMovement(bool autoReconnect,std::uint64_t generation);
        std::string PythonConnectionNote() const;
        std::string DiagnosticsJson() const;
        // Best-effort neutralization used by click recovery and shutdown.
        bool ForceReleaseLeftButton();
        bool IsConnected() const noexcept;
        // Physical mouse-button state is supplied by MAKCU's button monitor.
        bool IsMouseButtonInputAvailable() const noexcept;
        bool IsMouseButtonPressed(
            MouseButtonBinding button) const noexcept;
        MouseOutputConfig GetConfig() const;
        void ApplyConfig(const MouseOutputConfig& config);
        void ConnectOnce();
        std::uint64_t SettingsRevision() const noexcept;
        static bool RunMovementOwnershipSelfTest();

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };

    // Every fresh processing iteration either submits a correction or explicitly
    // ends the old one, including early returns, pauses and exceptions.
    class CorrectionScope
    {
    public:
        CorrectionScope(MakcuController* output,InputOwner owner,std::uint64_t generation)
            :output_(output),owner_(owner),generation_(generation){}
        ~CorrectionScope(){if(output_ && !submitted_)output_->EndCorrection(owner_,generation_,
            reason_.empty()?"Processing iteration ended without a correction":reason_.c_str());}
        void Submitted() noexcept{submitted_=true;}
        void EndReason(const std::string& reason){if(!submitted_)reason_=reason;}
    private:
        MakcuController* output_{};InputOwner owner_;std::uint64_t generation_{};bool submitted_{};
        std::string reason_;
    };
}
