#pragma once
#include "input_locks.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace vanta
{
    // POD shared with the crash helper. Options are captured before Vanta changes them.
    struct MediusRecoverySettings
    {
        std::uint8_t mac[6]{};std::uint8_t renderMode{},renderFull{};
        std::uint16_t spreadPercent{};std::uint32_t rideWindowMs{};
        std::uint16_t bearingWindowMs{};std::uint8_t bearingMode{};
        bool bearingValid{};
        bool rideEnabled{},valid{};
    };
    bool RecoverMedius(const std::string& port,const MediusRecoverySettings& settings);
    class MediusBackend
    {
    public:
        MediusBackend();~MediusBackend();
        static std::vector<std::string> Ports();
        void PrepareConnection();
        void CancelConnection();
        bool Open(const std::string& port,const MouseOutputConfig& settings,
            std::function<void(std::uint8_t,PhysicalButtonEdge)> buttons,std::string& error);
        bool Arm(std::string& error);
        bool Close();
        bool Lock(std::uint32_t bit,bool enabled);
        bool Query(InputQuery query,int& result);
        bool ReadMasks(InputMaskReadback& result);
        bool SetAxisPolicy(std::uint32_t bit,const DirectionalInputScales& policy);
        bool PrepareFuse();
        bool ClearBearing();
        std::string FuseUnavailableReason() const;
        void InvalidateOutput(const std::string& reason);
        bool Move(int dx,int dy,const OutputContext& context);
        bool BeginClick(int holdMilliseconds,const OutputContext& context={});
        bool FinishClick(bool cancelled);
        bool ClickCompleted() const;
        bool NeutralizeLeft();
        void EndMotion(const OutputContext* ended=nullptr);
        void CancelAll();
        bool Healthy() const;
        std::string Identity() const;
        std::string FailureReason() const;
        std::string Status() const;
        std::string DiagnosticsJson() const;
        MediusRecoverySettings RecoverySettings() const;
        static bool RunSelfTest();
    private:
        struct Implementation;std::unique_ptr<Implementation> state_;
    };
}
