#include "update_shared.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winhttp.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <vector>

namespace
{
    using winrt::Windows::Data::Json::JsonArray;
    using winrt::Windows::Data::Json::JsonObject;
    using winrt::Windows::Data::Json::JsonValue;

    struct InternetHandle
    {
        HINTERNET value{};

        ~InternetHandle()
        {
            if (value != nullptr)
            {
                WinHttpCloseHandle(value);
            }
        }

        InternetHandle() = default;
        explicit InternetHandle(HINTERNET input) : value(input) {}
        InternetHandle(const InternetHandle&) = delete;
        InternetHandle& operator=(const InternetHandle&) = delete;
    };

    std::string WindowsError(DWORD code)
    {
        char* buffer = nullptr;
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            code,
            0,
            reinterpret_cast<char*>(&buffer),
            0,
            nullptr);
        std::string result = length != 0 && buffer != nullptr
            ? std::string(buffer, length)
            : "Windows error " + std::to_string(code);
        if (buffer != nullptr)
        {
            LocalFree(buffer);
        }
        while (!result.empty() &&
               (result.back() == '\r' || result.back() == '\n'))
        {
            result.pop_back();
        }
        return result;
    }

    std::string LowerHex(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    std::string NormalizeDigest(std::string value)
    {
        const std::string prefix = "sha256:";
        if (value.size() >= prefix.size() &&
            LowerHex(value.substr(0, prefix.size())) == prefix)
        {
            value.erase(0, prefix.size());
        }
        value.erase(
            std::remove_if(
                value.begin(),
                value.end(),
                [](unsigned char character)
                {
                    return std::isspace(character) != 0;
                }),
            value.end());
        return LowerHex(std::move(value));
    }

    bool AtomicWriteText(
        const std::filesystem::path& path,
        const std::string& text,
        std::string& error)
    {
        std::error_code directoryError;
        std::filesystem::create_directories(
            path.parent_path(),
            directoryError);
        if (directoryError)
        {
            error = "could not create update directory: " +
                directoryError.message();
            return false;
        }
        const auto temporary = path.wstring() + L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "could not open temporary update state";
                return false;
            }
            output.write(
                text.data(),
                static_cast<std::streamsize>(text.size()));
            if (!output)
            {
                error = "could not write temporary update state";
                return false;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH))
        {
            error = "could not commit update state: " +
                WindowsError(GetLastError());
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }

    bool ReadText(
        const std::filesystem::path& path,
        std::string& text,
        std::string& error)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            error = "file does not exist";
            return false;
        }
        text.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof())
        {
            error = "could not read file";
            return false;
        }
        return true;
    }

    std::string JsonString(
        const JsonObject& object,
        const wchar_t* name,
        const std::string& fallback = {})
    {
        try
        {
            return object.HasKey(name)
                ? winrt::to_string(object.GetNamedString(name))
                : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    bool JsonBoolean(
        const JsonObject& object,
        const wchar_t* name,
        bool fallback)
    {
        try
        {
            return object.HasKey(name)
                ? object.GetNamedBoolean(name)
                : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    double JsonNumber(
        const JsonObject& object,
        const wchar_t* name,
        double fallback)
    {
        try
        {
            return object.HasKey(name)
                ? object.GetNamedNumber(name)
                : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    void PutString(
        JsonObject& object,
        const wchar_t* name,
        const std::string& value)
    {
        object.Insert(
            name,
            JsonValue::CreateStringValue(
                winrt::to_hstring(value)));
    }

    JsonObject ReleaseToJson(
        const vanta::updates::ReleaseInfo& release)
    {
        JsonObject object;
        PutString(object, L"tag", release.tag);
        PutString(object, L"title", release.title);
        PutString(object, L"notes", release.notes);
        PutString(object, L"zip_name", release.zipName);
        PutString(object, L"zip_url", release.zipUrl);
        PutString(object, L"checksum_name", release.checksumName);
        PutString(object, L"checksum_url", release.checksumUrl);
        PutString(object, L"github_digest", release.githubDigest);
        return object;
    }

    vanta::updates::ReleaseInfo ReleaseFromJson(
        const JsonObject& object)
    {
        vanta::updates::ReleaseInfo release;
        release.tag = JsonString(object, L"tag");
        release.title = JsonString(object, L"title");
        release.notes = JsonString(object, L"notes");
        release.zipName = JsonString(object, L"zip_name");
        release.zipUrl = JsonString(object, L"zip_url");
        release.checksumName = JsonString(object, L"checksum_name");
        release.checksumUrl = JsonString(object, L"checksum_url");
        release.githubDigest = JsonString(object, L"github_digest");
        return release;
    }

    bool HttpGet(
        const std::string& url,
        const std::function<bool(
            const unsigned char*,
            std::size_t,
            std::uint64_t,
            std::uint64_t)>& consume,
        std::string& error)
    {
        const std::wstring wideUrl = winrt::to_hstring(url).c_str();
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(
                wideUrl.c_str(),
                static_cast<DWORD>(wideUrl.size()),
                0,
                &components))
        {
            error = "invalid update URL: " + WindowsError(GetLastError());
            return false;
        }

        const std::wstring host(
            components.lpszHostName,
            components.dwHostNameLength);
        std::wstring requestPath(
            components.lpszUrlPath,
            components.dwUrlPathLength);
        if (components.dwExtraInfoLength != 0)
        {
            requestPath.append(
                components.lpszExtraInfo,
                components.dwExtraInfoLength);
        }
        if (requestPath.empty())
        {
            requestPath = L"/";
        }

        InternetHandle session(WinHttpOpen(
            L"Vanta-Updater/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));
        if (session.value == nullptr)
        {
            error = "WinHTTP initialization failed: " +
                WindowsError(GetLastError());
            return false;
        }
        WinHttpSetTimeouts(
            session.value,
            10000,
            10000,
            15000,
            30000);
        InternetHandle connection(WinHttpConnect(
            session.value,
            host.c_str(),
            components.nPort,
            0));
        if (connection.value == nullptr)
        {
            error = "update connection failed: " +
                WindowsError(GetLastError());
            return false;
        }
        const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
            ? WINHTTP_FLAG_SECURE
            : 0;
        InternetHandle request(WinHttpOpenRequest(
            connection.value,
            L"GET",
            requestPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags));
        if (request.value == nullptr)
        {
            error = "update request failed: " +
                WindowsError(GetLastError());
            return false;
        }
        const wchar_t headers[] =
            L"Accept: application/vnd.github+json\r\n"
            L"X-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpAddRequestHeaders(
                request.value,
                headers,
                static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD |
                    WINHTTP_ADDREQ_FLAG_REPLACE) ||
            !WinHttpSendRequest(
                request.value,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0) ||
            !WinHttpReceiveResponse(request.value, nullptr))
        {
            error = "update request failed: " +
                WindowsError(GetLastError());
            return false;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(
                request.value,
                WINHTTP_QUERY_STATUS_CODE |
                    WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX) ||
            status != 200)
        {
            error = "GitHub returned HTTP " + std::to_string(status);
            return false;
        }

        std::uint64_t total = 0;
        wchar_t lengthBuffer[64]{};
        DWORD lengthSize = sizeof(lengthBuffer);
        if (WinHttpQueryHeaders(
                request.value,
                WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX,
                lengthBuffer,
                &lengthSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            total = _wcstoui64(lengthBuffer, nullptr, 10);
        }

        std::array<unsigned char, 64 * 1024> buffer{};
        std::uint64_t received = 0;
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!WinHttpReadData(
                    request.value,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &bytesRead))
            {
                error = "update download failed: " +
                    WindowsError(GetLastError());
                return false;
            }
            if (bytesRead == 0)
            {
                break;
            }
            received += bytesRead;
            if (!consume(
                    buffer.data(),
                    bytesRead,
                    received,
                    total))
            {
                error = "update operation cancelled";
                SetLastError(ERROR_CANCELLED);
                return false;
            }
        }
        return true;
    }

    bool HttpGetText(
        const std::string& url,
        std::string& text,
        std::string& error)
    {
        text.clear();
        return HttpGet(
            url,
            [&](const unsigned char* data,
                std::size_t size,
                std::uint64_t,
                std::uint64_t)
            {
                if (text.size() + size > 4 * 1024 * 1024)
                {
                    error = "GitHub response is unexpectedly large";
                    return false;
                }
                text.append(
                    reinterpret_cast<const char*>(data),
                    size);
                return true;
            },
            error);
    }

    bool ParseVersion(
        std::string value,
        std::tuple<int, int, int>& result) noexcept
    {
        if (!value.empty() &&
            (value.front() == 'v' || value.front() == 'V'))
        {
            value.erase(value.begin());
        }
        const std::size_t suffix = value.find_first_not_of("0123456789.");
        if (suffix != std::string::npos)
        {
            value.resize(suffix);
        }
        std::array<int, 3> parts{};
        std::size_t begin = 0;
        for (std::size_t index = 0; index < parts.size(); ++index)
        {
            const std::size_t end = value.find('.', begin);
            const std::size_t count = end == std::string::npos
                ? value.size() - begin
                : end - begin;
            if (count == 0)
            {
                return false;
            }
            const char* first = value.data() + begin;
            const char* last = first + count;
            const auto converted = std::from_chars(
                first,
                last,
                parts[index]);
            if (converted.ec != std::errc{} || converted.ptr != last)
            {
                return false;
            }
            if (index + 1 < parts.size())
            {
                if (end == std::string::npos)
                {
                    return false;
                }
                begin = end + 1;
            }
            else if (end != std::string::npos)
            {
                return false;
            }
        }
        result = {parts[0], parts[1], parts[2]};
        return true;
    }
}

namespace vanta::updates
{
    std::filesystem::path UpdateRoot()
    {
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(
                FOLDERID_LocalAppData,
                KF_FLAG_CREATE,
                nullptr,
                &localAppData)) &&
            localAppData != nullptr)
        {
            std::filesystem::path result(localAppData);
            CoTaskMemFree(localAppData);
            return result / L"Vanta" / L"updates";
        }
        if (localAppData != nullptr)
        {
            CoTaskMemFree(localAppData);
        }
        wchar_t fallback[MAX_PATH]{};
        GetTempPathW(MAX_PATH, fallback);
        return std::filesystem::path(fallback) /
            L"Vanta" / L"updates";
    }

    std::int64_t CurrentUnixTime()
    {
        FILETIME fileTime{};
        GetSystemTimeAsFileTime(&fileTime);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = fileTime.dwLowDateTime;
        ticks.HighPart = fileTime.dwHighDateTime;
        constexpr std::uint64_t windowsToUnix =
            11644473600ULL * 10000000ULL;
        return static_cast<std::int64_t>(
            (ticks.QuadPart - windowsToUnix) /
            10000000ULL);
    }

    bool LoadUpdaterPreferences(
        UpdaterPreferences& result,
        std::string& error)
    {
        result = {};
        const auto path = UpdateRoot() / L"settings.json";
        if (!std::filesystem::exists(path))
        {
            error.clear();
            return true;
        }
        try
        {
            std::string text;
            if (!ReadText(path, text, error))
            {
                return false;
            }
            const JsonObject root = JsonObject::Parse(
                winrt::to_hstring(text));
            result.automaticChecks = JsonBoolean(
                root, L"automatic_checks", true);
            result.automaticDownloads = JsonBoolean(
                root, L"automatic_downloads", true);
            result.silentAutomaticInstallation = JsonBoolean(
                root, L"silent_automatic_installation", true);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
        catch (const winrt::hresult_error& exception)
        {
            error = winrt::to_string(exception.message());
            return false;
        }
    }

    bool SaveUpdaterPreferences(
        const UpdaterPreferences& preferences,
        std::string& error)
    {
        JsonObject root;
        root.Insert(
            L"automatic_checks",
            JsonValue::CreateBooleanValue(
                preferences.automaticChecks));
        root.Insert(
            L"automatic_downloads",
            JsonValue::CreateBooleanValue(
                preferences.automaticDownloads));
        root.Insert(
            L"silent_automatic_installation",
            JsonValue::CreateBooleanValue(
                preferences.silentAutomaticInstallation));
        return AtomicWriteText(
            UpdateRoot() / L"settings.json",
            winrt::to_string(root.Stringify()) + "\n",
            error);
    }

    bool LoadCachedRelease(
        ReleaseInfo& release,
        std::int64_t& checkedAt,
        std::string& error)
    {
        try
        {
            std::string text;
            if (!ReadText(
                    UpdateRoot() / L"release-cache.json",
                    text,
                    error))
            {
                return false;
            }
            const JsonObject root = JsonObject::Parse(
                winrt::to_hstring(text));
            checkedAt = static_cast<std::int64_t>(
                JsonNumber(root, L"checked_at", 0));
            release = ReleaseFromJson(
                root.GetNamedObject(L"release"));
            if (release.tag.empty() || release.zipUrl.empty())
            {
                error = "cached release is incomplete";
                return false;
            }
            return true;
        }
        catch (const winrt::hresult_error& exception)
        {
            error = winrt::to_string(exception.message());
            return false;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    bool SaveCachedRelease(
        const ReleaseInfo& release,
        std::int64_t checkedAt,
        std::string& error)
    {
        JsonObject root;
        root.Insert(
            L"checked_at",
            JsonValue::CreateNumberValue(
                static_cast<double>(checkedAt)));
        root.Insert(L"release", ReleaseToJson(release));
        return AtomicWriteText(
            UpdateRoot() / L"release-cache.json",
            winrt::to_string(root.Stringify()) + "\n",
            error);
    }

    bool LoadLastCheckAttempt(
        std::int64_t& checkedAt,
        std::string& error)
    {
        std::string text;
        if (!ReadText(
                UpdateRoot() / L"last-check-at.txt",
                text,
                error))
        {
            return false;
        }
        const char* first = text.data();
        const char* last = first + text.size();
        const auto converted = std::from_chars(
            first,
            last,
            checkedAt);
        if (converted.ec != std::errc{})
        {
            error = "invalid last update-check timestamp";
            return false;
        }
        return true;
    }

    bool SaveLastCheckAttempt(
        std::int64_t checkedAt,
        std::string& error)
    {
        return AtomicWriteText(
            UpdateRoot() / L"last-check-at.txt",
            std::to_string(checkedAt) + "\n",
            error);
    }

    bool IsCacheFresh(std::int64_t checkedAt) noexcept
    {
        const std::int64_t age = CurrentUnixTime() - checkedAt;
        return checkedAt > 0 &&
            age >= 0 &&
            age < cacheLifetimeSeconds;
    }

    bool FetchLatestRelease(
        ReleaseInfo& release,
        std::string& error)
    {
        try
        {
            std::string response;
            const std::string url =
                "https://api.github.com/repos/" +
                std::string(repositoryOwner) + "/" +
                repositoryName + "/releases/latest";
            if (!HttpGetText(url, response, error))
            {
                return false;
            }
            const JsonObject root = JsonObject::Parse(
                winrt::to_hstring(response));
            if (JsonBoolean(root, L"draft", true) ||
                JsonBoolean(root, L"prerelease", true))
            {
                error = "latest GitHub release is not stable";
                return false;
            }
            release = {};
            release.tag = JsonString(root, L"tag_name");
            release.title = JsonString(root, L"name", release.tag);
            release.notes = JsonString(root, L"body");
            const JsonArray assets = root.GetNamedArray(L"assets");
            for (const auto& value : assets)
            {
                const JsonObject asset = value.GetObject();
                const std::string name = JsonString(asset, L"name");
                if (name.size() > 7 &&
                    name.ends_with(".sha256"))
                {
                    release.checksumName = name;
                    release.checksumUrl = JsonString(
                        asset, L"browser_download_url");
                }
                else if (name.starts_with("vanta-v") &&
                         name.ends_with("-win64.zip"))
                {
                    release.zipName = name;
                    release.zipUrl = JsonString(
                        asset, L"browser_download_url");
                    release.githubDigest = JsonString(
                        asset, L"digest");
                }
            }
            if (release.tag.empty() ||
                release.zipName.empty() ||
                release.zipUrl.empty() ||
                release.checksumUrl.empty() ||
                release.githubDigest.empty())
            {
                error = "latest release is missing updater assets";
                return false;
            }
            return true;
        }
        catch (const winrt::hresult_error& exception)
        {
            error = winrt::to_string(exception.message());
            return false;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    bool IsReleaseNewer(
        const std::string& releaseTag,
        const std::string& compiledVersion) noexcept
    {
        std::tuple<int, int, int> release{};
        std::tuple<int, int, int> current{};
        if (!ParseVersion(releaseTag, release) ||
            !ParseVersion(compiledVersion, current))
        {
            return false;
        }
        return release > current;
    }

    bool DownloadFile(
        const std::string& url,
        const std::filesystem::path& destination,
        const ProgressCallback& progress,
        const std::string& activity,
        std::string& error)
    {
        std::error_code directoryError;
        std::filesystem::create_directories(
            destination.parent_path(),
            directoryError);
        if (directoryError)
        {
            error = "could not create download directory: " +
                directoryError.message();
            return false;
        }
        const std::filesystem::path partial =
            destination.wstring() + L".part";
        std::ofstream output(
            partial,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "could not create download file";
            return false;
        }
        int lastPercentage = -1;
        const bool downloaded = HttpGet(
            url,
            [&](const unsigned char* data,
                std::size_t size,
                std::uint64_t received,
                std::uint64_t total)
            {
                output.write(
                    reinterpret_cast<const char*>(data),
                    static_cast<std::streamsize>(size));
                if (!output)
                {
                    error = "could not write download file";
                    return false;
                }
                const int percentage = total == 0
                    ? 0
                    : static_cast<int>(std::min<std::uint64_t>(
                        100,
                        received * 100 / total));
                if (percentage != lastPercentage && progress)
                {
                    lastPercentage = percentage;
                    return progress(percentage, activity);
                }
                return true;
            },
            error);
        output.close();
        if (!downloaded)
        {
            std::filesystem::remove(partial, directoryError);
            return false;
        }
        if (!MoveFileExW(
                partial.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH))
        {
            error = "could not commit download: " +
                WindowsError(GetLastError());
            std::filesystem::remove(partial, directoryError);
            return false;
        }
        if (progress)
        {
            progress(100, activity);
        }
        return true;
    }

    bool ComputeSha256(
        const std::filesystem::path& file,
        std::string& digest,
        std::string& error)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        std::array<unsigned char, 32> result{};
        HANDLE input = INVALID_HANDLE_VALUE;
        bool succeeded = false;
        do
        {
            if (BCryptOpenAlgorithmProvider(
                    &algorithm,
                    BCRYPT_SHA256_ALGORITHM,
                    nullptr,
                    0) < 0)
            {
                error = "could not initialize SHA-256";
                break;
            }
            DWORD objectLength = 0;
            DWORD returned = 0;
            if (BCryptGetProperty(
                    algorithm,
                    BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectLength),
                    sizeof(objectLength),
                    &returned,
                    0) < 0)
            {
                error = "could not query SHA-256 state size";
                break;
            }
            object.resize(objectLength);
            if (BCryptCreateHash(
                    algorithm,
                    &hash,
                    object.data(),
                    objectLength,
                    nullptr,
                    0,
                    0) < 0)
            {
                error = "could not create SHA-256 state";
                break;
            }
            input = CreateFileW(
                file.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL |
                    FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr);
            if (input == INVALID_HANDLE_VALUE)
            {
                error = "could not open downloaded update: " +
                    WindowsError(GetLastError());
                break;
            }
            std::array<unsigned char, 128 * 1024> buffer{};
            for (;;)
            {
                DWORD read = 0;
                if (!ReadFile(
                        input,
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &read,
                        nullptr))
                {
                    error = "could not read downloaded update: " +
                        WindowsError(GetLastError());
                    break;
                }
                if (read == 0)
                {
                    if (BCryptFinishHash(
                            hash,
                            result.data(),
                            static_cast<ULONG>(result.size()),
                            0) < 0)
                    {
                        error = "could not finish SHA-256";
                        break;
                    }
                    std::ostringstream stream;
                    stream << std::hex << std::setfill('0');
                    for (const unsigned char value : result)
                    {
                        stream << std::setw(2)
                               << static_cast<unsigned int>(value);
                    }
                    digest = stream.str();
                    succeeded = true;
                    break;
                }
                if (BCryptHashData(
                        hash,
                        buffer.data(),
                        read,
                        0) < 0)
                {
                    error = "could not update SHA-256";
                    break;
                }
            }
        } while (false);
        if (input != INVALID_HANDLE_VALUE)
        {
            CloseHandle(input);
        }
        if (hash != nullptr)
        {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return succeeded;
    }

    bool VerifySha256(
        const std::filesystem::path& file,
        const std::string& expectedDigest,
        std::string& error)
    {
        std::string actual;
        if (!ComputeSha256(file, actual, error))
        {
            return false;
        }
        const std::string expected = NormalizeDigest(expectedDigest);
        if (expected.size() != 64 || actual != expected)
        {
            error = "SHA-256 mismatch (expected " + expected +
                ", received " + actual + ")";
            return false;
        }
        return true;
    }

    bool DownloadRelease(
        const ReleaseInfo& release,
        std::filesystem::path& zipPath,
        std::string& verifiedSha256,
        const ProgressCallback& progress,
        std::string& error)
    {
        const std::filesystem::path releaseDirectory =
            UpdateRoot() / winrt::to_hstring(release.tag).c_str();
        const std::filesystem::path checksumPath =
            releaseDirectory /
            winrt::to_hstring(release.checksumName).c_str();
        zipPath = releaseDirectory /
            winrt::to_hstring(release.zipName).c_str();
        if (!DownloadFile(
                release.checksumUrl,
                checksumPath,
                [&](int percentage, const std::string& activity)
                {
                    return !progress || progress(
                        percentage * 5 / 100,
                        activity);
                },
                "Downloading checksum",
                error))
        {
            return false;
        }
        std::string checksumText;
        if (!ReadText(checksumPath, checksumText, error))
        {
            return false;
        }
        const std::size_t separator = checksumText.find_first_of(" \t\r\n");
        verifiedSha256 = NormalizeDigest(
            checksumText.substr(0, separator));
        const std::string apiDigest = NormalizeDigest(
            release.githubDigest);
        if (!apiDigest.empty() && apiDigest != verifiedSha256)
        {
            error = "GitHub asset digest does not match checksum asset";
            return false;
        }
        if (!DownloadFile(
                release.zipUrl,
                zipPath,
                [&](int percentage, const std::string& activity)
                {
                    return !progress || progress(
                        5 + percentage * 90 / 100,
                        activity);
                },
                "Downloading update",
                error))
        {
            return false;
        }
        if (progress && !progress(96, "Verifying SHA-256"))
        {
            error = "update operation cancelled";
            return false;
        }
        if (!VerifySha256(zipPath, verifiedSha256, error))
        {
            return false;
        }
        return !progress || progress(100, "Update verified");
    }

    std::wstring QuoteArgument(const std::wstring& argument)
    {
        std::wstring result = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    bool StartProcess(
        const std::filesystem::path& executable,
        const std::wstring& arguments,
        std::string& error)
    {
        std::wstring command = QuoteArgument(executable.wstring());
        if (!arguments.empty())
        {
            command += L" ";
            command += arguments;
        }
        std::vector<wchar_t> mutableCommand(
            command.begin(),
            command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                executable.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                executable.parent_path().c_str(),
                &startup,
                &process))
        {
            error = "could not start " +
                executable.filename().string() + ": " +
                WindowsError(GetLastError());
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    std::filesystem::path CurrentExecutablePath()
    {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
        {
            return {};
        }
        buffer.resize(length);
        return buffer;
    }
}
