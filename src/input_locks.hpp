#pragma once
#include "app_settings.hpp"
#include "input_recovery.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vanta
{
    enum class InputOwner : std::uint8_t { manual,python,testMove,testClick };
    struct OutputContext
    {
        InputOwner owner{InputOwner::manual};
        std::uint64_t generation{},streamGeneration{},streamSession{},sequence{},frameTimeUs{};
        MouseButtonBinding activation{MouseButtonBinding::left};
        std::uint32_t axes{InputLockX|InputLockY};
        PythonPhysicalInputMode physicalInputMode{PythonPhysicalInputMode::existing};
        // Includes the source/config generation check; called before serial output.
        std::function<bool()> isCurrent;
    };
    enum class InputQuery { lockX,lockY,lockLeft,lockSide1,lockSide2,leftState,moving };
    enum class ClickOutputPhase { idle,preparing,playing,completed,blocked,cancelled,failed };
    struct ClickOutputStatus
    {
        ClickOutputPhase phase{ClickOutputPhase::idle};
        std::uint64_t attempts{},started{},completed{},rejected{},cancelled{},failed{},updatedAtUs{};
        std::string message{"No generated click requested yet"};
    };
    inline constexpr std::array<std::uint32_t,5> kManagedInputBits{
        InputLockX,InputLockY,InputLockLeft,InputLockSide1,InputLockSide2};
    struct DirectionalInputScales
    {
        std::int16_t positive{100},negative{100},with{100},against{100};
        bool operator==(const DirectionalInputScales&) const = default;
    };
    struct ManagedInputPolicy
    {
        std::array<DirectionalInputScales,5> scales{};
        std::uint32_t guidedAxes{};
        bool bearingRequired{};
        bool operator==(const ManagedInputPolicy&) const = default;
    };
    inline constexpr std::uint16_t kFuseBearingWindowMs=100;
    ManagedInputPolicy MakeManagedInputPolicy(std::uint32_t blocked,std::uint32_t guidedAxes=0);
    struct InputMaskReadback
    {
        // Scales are firmware values, not the SDK's desired/cached lock state.
        std::uint32_t knownMask{},blockedMask{},passthroughMask{};
        std::array<std::int16_t,5> positiveScale{},negativeScale{};
        std::array<std::int16_t,5> withScale{100,100,100,100,100},againstScale{100,100,100,100,100};
        bool bearingKnown{};
        std::uint16_t bearingWindowMs{};
        std::uint8_t bearingMode{};
        std::uint64_t readAtUs{};
    };
    InputQuery LockQuery(std::uint32_t bit);
    class MediusBackend;
    struct InputLockTransport
    {
        std::function<bool(std::uint32_t,bool)> lock;
        std::function<bool(InputQuery,int&)> query;
        std::function<bool(int,int,const OutputContext&)> move;
        std::function<bool(bool)> left;
        std::function<bool()> healthy,helperReady;
        std::function<void(std::uint32_t)> mark;
        std::function<void()> endMotion,cancelAll;
        std::function<bool(int,const OutputContext&)> beginClick;
        std::function<bool(bool)> endClick;
        std::function<bool()> clickSucceeded;
        std::function<std::string()> failureReason;
        std::function<bool(InputMaskReadback&)> readMasks;
        std::function<void(const std::string&)> invalidate;
        std::function<bool(std::uint32_t,const DirectionalInputScales&)> axisPolicy;
        std::function<bool()> prepareFuse,clearBearing;
    };
    class InputLockCoordinator
    {
    public:
        InputLockCoordinator();
        ~InputLockCoordinator();
        bool Attach(makcu_device* device,const std::string& port,std::string& error);
        bool Attach(MediusBackend& device,const std::string& port,std::string& error);
        // Detach/quiesce before closing or destroying the borrowed MAKCU device.
        bool Detach();
        void FinishRecovery(bool clean);
        void Configure(const MouseOutputConfig& config);
        void SetPolicy(InputOwner owner,bool enabled,std::uint64_t generation,MouseButtonBinding activation);
        void PhysicalButtons(std::uint8_t mask) noexcept;
        void PhysicalButton(MouseButtonBinding button,bool pressed) noexcept;
        bool TryMove(int x,int y,const OutputContext& context);
        bool TryClick(const OutputContext& context,int holdMilliseconds);
        void EndCorrection(InputOwner owner,std::uint64_t generation,const char* reason="Correction ended");
        void CancelOwner(InputOwner owner);
        void EmergencyRelease();
        bool CancelClick();
        bool ResetRequired() const noexcept;
        std::uint64_t EmergencyRevision() const noexcept;
        std::string Status() const;
        std::string MovementBlockReason(InputOwner owner) const;
        ClickOutputStatus ClickStatus(InputOwner owner) const;
        std::string DiagnosticsJson() const;
        static bool RunSelfTest();
    private:
        struct Implementation;
        std::unique_ptr<Implementation> state_;
    };
}
