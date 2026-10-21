#pragma once
#include "python_bot.hpp"
#include <cstdint>
#include <memory>

namespace vanta
{
    class CaptureController;
    class MakcuController;
    class PythonBotController
    {
    public:
        PythonBotController();
        ~PythonBotController();
        void Initialize(CaptureController* capture,MakcuController* makcu,const std::string& configJson);
        void Shutdown();
        void RenderPanel();
        bool ApplyConfigJson(const std::string& json,std::string& error);
        std::string GetConfigJson() const;
        PythonBotConfig GetConfig() const;
        std::uint64_t SettingsRevision() const noexcept;
        bool SetEnabled(bool enabled);
        bool IsEnabled() const noexcept;
        std::uint64_t ProcessedFrames() const noexcept;
        std::string DiagnosticsJson() const;
    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
