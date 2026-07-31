#pragma once

#include "app_settings.hpp"

#include <cstdint>
#include <memory>

namespace vanta
{
    bool LaunchUpdateBootstrapIfNeeded(
        int argumentCount,
        wchar_t** arguments,
        bool& launched);

    class UpdateController
    {
    public:
        UpdateController();
        ~UpdateController();

        UpdateController(const UpdateController&) = delete;
        UpdateController& operator=(const UpdateController&) = delete;

        void Initialize(
            const UpdateConfig& initialConfig,
            bool networkEnabled = true);
        void Shutdown(bool launchInstaller);
        void RenderPanel();

        UpdateConfig GetConfig() const;
        void ApplyConfig(const UpdateConfig& config);
        std::uint64_t SettingsRevision() const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
