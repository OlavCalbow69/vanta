#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vanta::updates
{
    inline constexpr char repositoryOwner[] = "OlavCalbow69";
    inline constexpr char repositoryName[] = "vanta";
    inline constexpr std::int64_t cacheLifetimeSeconds = 24 * 60 * 60;

    struct ReleaseInfo
    {
        std::string tag;
        std::string title;
        std::string notes;
        std::string zipName;
        std::string zipUrl;
        std::string checksumName;
        std::string checksumUrl;
        std::string githubDigest;
    };

    struct UpdaterPreferences
    {
        bool automaticChecks{true};
        bool automaticDownloads{true};
        bool silentAutomaticInstallation{true};
    };

    using ProgressCallback = std::function<bool(
        int percentage,
        const std::string& activity)>;

    std::filesystem::path UpdateRoot();
    std::int64_t CurrentUnixTime();

    bool LoadUpdaterPreferences(
        UpdaterPreferences& result,
        std::string& error);
    bool SaveUpdaterPreferences(
        const UpdaterPreferences& preferences,
        std::string& error);

    bool LoadCachedRelease(
        ReleaseInfo& release,
        std::int64_t& checkedAt,
        std::string& error);
    bool SaveCachedRelease(
        const ReleaseInfo& release,
        std::int64_t checkedAt,
        std::string& error);
    bool LoadLastCheckAttempt(
        std::int64_t& checkedAt,
        std::string& error);
    bool SaveLastCheckAttempt(
        std::int64_t checkedAt,
        std::string& error);
    bool IsCacheFresh(std::int64_t checkedAt) noexcept;

    bool FetchLatestRelease(
        ReleaseInfo& release,
        std::string& error);
    bool IsReleaseNewer(
        const std::string& releaseTag,
        const std::string& compiledVersion) noexcept;

    bool DownloadRelease(
        const ReleaseInfo& release,
        std::filesystem::path& zipPath,
        std::string& verifiedSha256,
        const ProgressCallback& progress,
        std::string& error);
    bool DownloadFile(
        const std::string& url,
        const std::filesystem::path& destination,
        const ProgressCallback& progress,
        const std::string& activity,
        std::string& error);
    bool ComputeSha256(
        const std::filesystem::path& file,
        std::string& digest,
        std::string& error);
    bool VerifySha256(
        const std::filesystem::path& file,
        const std::string& expectedDigest,
        std::string& error);

    std::wstring QuoteArgument(const std::wstring& argument);
    bool StartProcess(
        const std::filesystem::path& executable,
        const std::wstring& arguments,
        std::string& error);
    std::filesystem::path CurrentExecutablePath();
}
