#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "app_settings.hpp"

struct makcu_device;
namespace vanta
{
    struct MediusRecoverySettings;
    inline constexpr std::uint32_t InputLockX=1,InputLockY=2,InputLockLeft=4,
        InputLockSide1=8,InputLockSide2=16,InputInjectedLeft=32,InputManagedLocks=31;
    constexpr bool RecoverySessionMatches(std::uint64_t expected,std::uint64_t current) noexcept
    {return expected!=0 && expected==current;}
    inline constexpr std::uint32_t kInputRecoveryRecordVersion=2;
    constexpr bool RecoveryLayoutMatches(std::uint32_t version,std::uint32_t bytes,std::uint32_t expectedBytes) noexcept
    {return version==kInputRecoveryRecordVersion && bytes==expectedBytes && expectedBytes!=0;}
    bool QueryMakcuInteger(makcu_device* device,const char* command,int& value);
    bool WriteMakcuLock(makcu_device* device,std::uint32_t bit,bool locked);
    const char* MakcuLockQuery(std::uint32_t bit);
    // Owns a per-device OS guard and a child that survives abnormal parent exit.
    class InputRecoverySession
    {
    public:
        InputRecoverySession();
        ~InputRecoverySession();
        bool Start(const std::string& port,std::string& error,InputBackend backend=InputBackend::makcu,
            const MediusRecoverySettings* medius=nullptr);
        void Mark(std::uint32_t possiblyActive) noexcept;
        bool Ready() const noexcept;
        void Finish(bool clean);
    private:
        struct Implementation;
        std::unique_ptr<Implementation> state_;
    };
    // Must run before normal logging, console, updater, COM apartment or graphics.
    bool RunInputRecoveryMode(int argc,wchar_t** argv,int& exitCode);
}
