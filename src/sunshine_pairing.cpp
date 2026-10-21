#include "sunshine_pairing.hpp"

#include "Limelight.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <shlobj_core.h>
#include <windns.h>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Bytes = std::vector<unsigned char>;

    constexpr std::uint16_t kGameStreamHttpPort = 47989;
    constexpr std::uint16_t kDefaultHttpsPort = 47984;
    constexpr DWORD kRequestTimeoutMilliseconds = 5000;
    // Sunshine waits for the user to approve pairing in its Web UI. The
    // initial GameStream /pair response is therefore intentionally delayed
    // until that PIN has been entered on the host.
    constexpr DWORD kPairingPinEntryTimeoutMilliseconds = 120000;
    constexpr auto kLanDiscoveryWindow = std::chrono::milliseconds(1500);
    // Sunshine can take noticeably longer to start Desktop than it takes to
    // answer normal GameStream control requests. These match the time budgets
    // used by Moonlight's native HTTP client.
    constexpr DWORD kLaunchTimeoutMilliseconds = 120000;
    constexpr std::array<unsigned char, 5> kIdentityMagic{'V', 'S', 'I', '1', 0};
    constexpr std::array<unsigned char, 5> kHostMagic{'V', 'S', 'H', '2', 0};

    struct HttpResponse
    {
        int status{};
        std::string body;
    };

    struct ClientIdentity
    {
        std::string certificatePem;
        std::string privateKeyPem;
        std::string uniqueId;
    };

    class SocketHandle
    {
    public:
        SocketHandle() = default;
        explicit SocketHandle(SOCKET value) noexcept : value_(value) {}
        ~SocketHandle()
        {
            if (value_ != INVALID_SOCKET)
            {
                closesocket(value_);
            }
        }

        SocketHandle(const SocketHandle&) = delete;
        SocketHandle& operator=(const SocketHandle&) = delete;

        SocketHandle(SocketHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
        SocketHandle& operator=(SocketHandle&& other) noexcept
        {
            if (this != &other)
            {
                if (value_ != INVALID_SOCKET)
                {
                    closesocket(value_);
                }
                value_ = std::exchange(other.value_, INVALID_SOCKET);
            }
            return *this;
        }

        [[nodiscard]] SOCKET Get() const noexcept { return value_; }
        [[nodiscard]] explicit operator bool() const noexcept { return value_ != INVALID_SOCKET; }

    private:
        SOCKET value_{INVALID_SOCKET};
    };

    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
    using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using EvpCipherContextPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    using EvpDigestContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    using SslContextPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    using SslPtr = std::unique_ptr<SSL, decltype(&SSL_free)>;

    [[nodiscard]] std::string Trim(const std::string& value)
    {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character)
        {
            return std::isspace(character) != 0;
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character)
        {
            return std::isspace(character) != 0;
        }).base();
        return first < last ? std::string(first, last) : std::string{};
    }

    [[nodiscard]] std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    [[nodiscard]] std::string Utf8FromWide(const wchar_t* value)
    {
        if (value == nullptr || *value == L'\0')
        {
            return {};
        }

        const int characterCount = static_cast<int>(std::wcslen(value));
        const int requiredBytes = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value,
            characterCount,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (requiredBytes <= 0)
        {
            return {};
        }

        std::string result(static_cast<std::size_t>(requiredBytes), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value,
                characterCount,
                result.data(),
                requiredBytes,
                nullptr,
                nullptr) != requiredBytes)
        {
            return {};
        }
        return result;
    }

    [[nodiscard]] std::string TrimDnsTrailingDot(std::string value)
    {
        value = Trim(value);
        while (!value.empty() && value.back() == '.')
        {
            value.pop_back();
        }
        return value;
    }

    struct MdnsEndpoint
    {
        std::string host;
        std::uint16_t port{};
    };

    struct MdnsBrowseContext
    {
        std::mutex mutex;
        bool accepting{true};
        std::vector<MdnsEndpoint> endpoints;
    };

    void WINAPI OnSunshineMdnsBrowse(
        const DWORD status,
        PVOID context,
        PDNS_RECORD records)
    {
        MdnsBrowseContext* const state = static_cast<MdnsBrowseContext*>(context);
        if (status == ERROR_SUCCESS && state != nullptr && records != nullptr)
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->accepting)
            {
                for (PDNS_RECORD record = records; record != nullptr; record = record->pNext)
                {
                    if (record->wType != DNS_TYPE_SRV ||
                        record->Data.SRV.pNameTarget == nullptr ||
                        record->Data.SRV.wPort == 0)
                    {
                        continue;
                    }

                    MdnsEndpoint endpoint;
                    endpoint.host = TrimDnsTrailingDot(Utf8FromWide(record->Data.SRV.pNameTarget));
                    endpoint.port = record->Data.SRV.wPort;
                    if (endpoint.host.empty())
                    {
                        continue;
                    }

                    const bool alreadyPresent = std::any_of(
                        state->endpoints.begin(),
                        state->endpoints.end(),
                        [&endpoint](const MdnsEndpoint& existing)
                        {
                            return existing.port == endpoint.port &&
                                   Lowercase(existing.host) == Lowercase(endpoint.host);
                        });
                    if (!alreadyPresent)
                    {
                        state->endpoints.push_back(std::move(endpoint));
                    }
                }
            }
        }

        if (records != nullptr)
        {
            DnsRecordListFree(records, DnsFreeRecordList);
        }
    }

    [[nodiscard]] std::vector<MdnsEndpoint> BrowseSunshineLan(
        std::string& error)
    {
        MdnsBrowseContext context;
        DNS_SERVICE_CANCEL cancel{};
        DNS_SERVICE_BROWSE_REQUEST request{};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.QueryName = L"_nvstream._tcp.local";
        request.pBrowseCallback = OnSunshineMdnsBrowse;
        request.pQueryContext = &context;

        const DNS_STATUS startStatus = DnsServiceBrowse(&request, &cancel);
        if (startStatus != DNS_REQUEST_PENDING)
        {
            error = "Could not start LAN Sunshine discovery (DNS-SD error " + std::to_string(startStatus) + ")";
            return {};
        }

        std::this_thread::sleep_for(kLanDiscoveryWindow);
        const DNS_STATUS stopStatus = DnsServiceBrowseCancel(&cancel);
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            context.accepting = false;
        }
        if (stopStatus != ERROR_SUCCESS)
        {
            error = "Could not stop LAN Sunshine discovery (DNS-SD error " + std::to_string(stopStatus) + ")";
            return {};
        }

        std::lock_guard<std::mutex> lock(context.mutex);
        return context.endpoints;
    }

    [[nodiscard]] std::string OpenSslError()
    {
        const unsigned long code = ERR_get_error();
        if (code == 0)
        {
            return "OpenSSL operation failed";
        }

        std::array<char, 256> buffer{};
        ERR_error_string_n(code, buffer.data(), buffer.size());
        return buffer.data();
    }

    [[nodiscard]] std::string HexEncode(const unsigned char* data, const std::size_t size)
    {
        static constexpr char characters[] = "0123456789abcdef";
        std::string result(size * 2, '\0');
        for (std::size_t index = 0; index < size; ++index)
        {
            result[index * 2] = characters[data[index] >> 4U];
            result[index * 2 + 1] = characters[data[index] & 0x0FU];
        }
        return result;
    }

    [[nodiscard]] std::string HexEncode(const Bytes& data)
    {
        return HexEncode(data.data(), data.size());
    }

    [[nodiscard]] std::optional<Bytes> HexDecode(const std::string_view value)
    {
        if (value.empty() || value.size() % 2 != 0)
        {
            return std::nullopt;
        }

        const auto digit = [](const char character) -> int
        {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            return -1;
        };

        Bytes result(value.size() / 2);
        for (std::size_t index = 0; index < result.size(); ++index)
        {
            const int high = digit(value[index * 2]);
            const int low = digit(value[index * 2 + 1]);
            if (high < 0 || low < 0)
            {
                return std::nullopt;
            }
            result[index] = static_cast<unsigned char>((high << 4) | low);
        }
        return result;
    }

    [[nodiscard]] std::string XmlValue(const std::string& xml, const std::string_view tag)
    {
        const std::string opening = "<" + std::string(tag) + ">";
        const std::string closing = "</" + std::string(tag) + ">";
        const std::size_t start = xml.find(opening);
        if (start == std::string::npos)
        {
            return {};
        }
        const std::size_t content = start + opening.size();
        const std::size_t end = xml.find(closing, content);
        return end == std::string::npos ? std::string{} : xml.substr(content, end - content);
    }

    [[nodiscard]] bool VerifyGameStreamResponse(const HttpResponse& response, std::string& error)
    {
        if (response.status != 200)
        {
            error = "Sunshine returned HTTP " + std::to_string(response.status);
            return false;
        }

        const std::size_t root = response.body.find("<root");
        if (root == std::string::npos)
        {
            error = "Sunshine returned malformed GameStream XML";
            return false;
        }

        const std::size_t status = response.body.find("status_code=\"", root);
        if (status == std::string::npos)
        {
            error = "Sunshine response is missing its GameStream status";
            return false;
        }

        const std::size_t valueStart = status + std::strlen("status_code=\"");
        const std::size_t valueEnd = response.body.find('"', valueStart);
        if (valueEnd == std::string::npos)
        {
            error = "Sunshine response has an invalid GameStream status";
            return false;
        }

        int statusCode{};
        const std::string_view value(response.body.data() + valueStart, valueEnd - valueStart);
        const auto [pointer, conversion] = std::from_chars(value.data(), value.data() + value.size(), statusCode);
        if (conversion != std::errc{} || pointer != value.data() + value.size() || statusCode != 200)
        {
            const std::string message = XmlValue(response.body, "status_message");
            error = "Sunshine rejected the request" + (message.empty() ? std::string{} : ": " + message);
            return false;
        }

        return true;
    }

    [[nodiscard]] bool EnsureWinsock(std::string& error)
    {
        static std::once_flag initializeOnce;
        static int status = WSASYSNOTREADY;
        std::call_once(initializeOnce, []()
        {
            WSADATA data{};
            status = WSAStartup(MAKEWORD(2, 2), &data);
        });

        if (status != 0)
        {
            error = "Windows network initialization failed";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<SocketHandle> ConnectSocket(
        const std::string& host,
        const std::uint16_t port,
        const DWORD timeoutMilliseconds,
        std::string& error)
    {
        if (!EnsureWinsock(error))
        {
            return std::nullopt;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* addresses{};
        const std::string portString = std::to_string(port);
        if (getaddrinfo(host.c_str(), portString.c_str(), &hints, &addresses) != 0)
        {
            error = "Could not resolve the Sunshine host";
            return std::nullopt;
        }

        std::optional<SocketHandle> connected;
        // Sunshine discovery on a dual-stack Windows LAN often returns a
        // link-local IPv6 address before the directly reachable IPv4 address.
        // Prefer IPv4 for this LAN receiver, then retain IPv6 as a fallback.
        for (int pass = 0; pass < 2 && !connected; ++pass)
        {
            for (addrinfo* address = addresses; address != nullptr; address = address->ai_next)
            {
                const bool isIpv4 = address->ai_family == AF_INET;
                if ((pass == 0 && !isIpv4) || (pass == 1 && isIpv4))
                {
                    continue;
                }

                SocketHandle connectionSocket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
                if (!connectionSocket)
                {
                    continue;
                }

                u_long nonBlocking = 1;
                if (ioctlsocket(connectionSocket.Get(), FIONBIO, &nonBlocking) != 0)
                {
                    continue;
                }

                int result = connect(connectionSocket.Get(), address->ai_addr, static_cast<int>(address->ai_addrlen));
                if (result == SOCKET_ERROR)
                {
                    const int connectError = WSAGetLastError();
                    if (connectError != WSAEWOULDBLOCK && connectError != WSAEINPROGRESS)
                    {
                        continue;
                    }

                    fd_set writeSet;
                    FD_ZERO(&writeSet);
                    FD_SET(connectionSocket.Get(), &writeSet);
                    timeval timeout{};
                    timeout.tv_sec = static_cast<long>(timeoutMilliseconds / 1000);
                    timeout.tv_usec = static_cast<long>((timeoutMilliseconds % 1000) * 1000);
                    if (select(0, nullptr, &writeSet, nullptr, &timeout) <= 0)
                    {
                        continue;
                    }

                    int socketError{};
                    int socketErrorLength = sizeof(socketError);
                    if (getsockopt(connectionSocket.Get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &socketErrorLength) != 0 || socketError != 0)
                    {
                        continue;
                    }
                }

                nonBlocking = 0;
                ioctlsocket(connectionSocket.Get(), FIONBIO, &nonBlocking);
                const DWORD timeout = timeoutMilliseconds;
                setsockopt(connectionSocket.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
                setsockopt(connectionSocket.Get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
                const BOOL disableNagle = TRUE;
                setsockopt(connectionSocket.Get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&disableNagle), sizeof(disableNagle));
                connected.emplace(std::move(connectionSocket));
                break;
            }
        }
        freeaddrinfo(addresses);

        if (!connected)
        {
            error = "Could not connect to Sunshine";
        }
        return connected;
    }

    [[nodiscard]] std::optional<std::string> ResolveIpv4Address(
        const std::string& host)
    {
        std::string ignored;
        if (!EnsureWinsock(ignored))
        {
            return std::nullopt;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* addresses{};
        if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0)
        {
            return std::nullopt;
        }

        std::optional<std::string> result;
        for (addrinfo* address = addresses; address != nullptr; address = address->ai_next)
        {
            if (address->ai_family != AF_INET || address->ai_addr == nullptr)
            {
                continue;
            }

            const auto* const ipv4 = reinterpret_cast<const sockaddr_in*>(address->ai_addr);
            std::array<char, INET_ADDRSTRLEN> text{};
            if (InetNtopA(AF_INET, &ipv4->sin_addr, text.data(), static_cast<DWORD>(text.size())) != nullptr)
            {
                result.emplace(text.data());
                break;
            }
        }
        freeaddrinfo(addresses);
        return result;
    }

    [[nodiscard]] std::string NewRequestUuid()
    {
        Bytes random(16);
        if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        {
            return {};
        }
        return HexEncode(random);
    }

    [[nodiscard]] std::string BuildRequest(
        const std::string& host,
        const std::uint16_t port,
        const std::string_view endpoint,
        const std::string& uniqueId,
        const std::string& arguments,
        std::string& error)
    {
        const std::string uuid = NewRequestUuid();
        if (uuid.empty())
        {
            error = "Could not create a secure request identifier";
            return {};
        }

        std::string target = "/" + std::string(endpoint) + "?uniqueid=" + uniqueId + "&uuid=" + uuid;
        if (!arguments.empty())
        {
            target += "&" + arguments;
        }

        std::string hostHeader = host;
        if (host.find(':') != std::string::npos && host.front() != '[')
        {
            hostHeader = "[" + host + "]";
        }
        hostHeader += ":" + std::to_string(port);
        return "GET " + target + " HTTP/1.1\r\nHost: " + hostHeader +
               "\r\nConnection: close\r\nAccept: */*\r\nUser-Agent: Vanta Sunshine Receiver\r\n\r\n";
    }

    [[nodiscard]] bool SendSocketAll(const SOCKET socket, const std::string& request, std::string& error)
    {
        std::size_t sent{};
        while (sent < request.size())
        {
            const int count = send(socket, request.data() + sent, static_cast<int>(std::min<std::size_t>(request.size() - sent, std::numeric_limits<int>::max())), 0);
            if (count <= 0)
            {
                error = "Could not send the Sunshine request";
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    [[nodiscard]] bool ReceiveSocketAll(const SOCKET socket, std::string& response, std::string& error)
    {
        std::array<char, 8192> buffer{};
        for (;;)
        {
            const int count = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (count == 0)
            {
                return true;
            }
            if (count < 0)
            {
                error = "Timed out while reading Sunshine's response";
                return false;
            }
            response.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }

    [[nodiscard]] bool DecodeChunkedBody(const std::string_view input, std::string& output)
    {
        std::size_t position{};
        while (position < input.size())
        {
            const std::size_t lineEnd = input.find("\r\n", position);
            if (lineEnd == std::string_view::npos)
            {
                return false;
            }
            const std::size_t extension = input.find(';', position);
            const std::size_t lengthEnd = extension != std::string_view::npos && extension < lineEnd ? extension : lineEnd;
            std::size_t length{};
            const auto [pointer, conversion] = std::from_chars(input.data() + position, input.data() + lengthEnd, length, 16);
            if (conversion != std::errc{} || pointer != input.data() + lengthEnd)
            {
                return false;
            }
            position = lineEnd + 2;
            if (length == 0)
            {
                return true;
            }
            if (length > input.size() - position || input.size() - position - length < 2 || input.substr(position + length, 2) != "\r\n")
            {
                return false;
            }
            output.append(input.substr(position, length));
            position += length + 2;
        }
        return false;
    }

    [[nodiscard]] std::optional<HttpResponse> ParseHttpResponse(const std::string& raw, std::string& error)
    {
        const std::size_t headersEnd = raw.find("\r\n\r\n");
        if (headersEnd == std::string::npos)
        {
            error = "Sunshine sent an invalid HTTP response";
            return std::nullopt;
        }

        const std::size_t lineEnd = raw.find("\r\n");
        if (lineEnd == std::string::npos)
        {
            error = "Sunshine sent an invalid HTTP status line";
            return std::nullopt;
        }

        const std::string_view statusLine(raw.data(), lineEnd);
        const std::size_t statusBegin = statusLine.find(' ');
        if (statusBegin == std::string_view::npos)
        {
            error = "Sunshine sent an invalid HTTP status";
            return std::nullopt;
        }
        int status{};
        const auto [pointer, conversion] = std::from_chars(statusLine.data() + statusBegin + 1, statusLine.data() + statusLine.size(), status);
        if (conversion != std::errc{})
        {
            error = "Sunshine sent an invalid HTTP status";
            return std::nullopt;
        }

        const std::string headersLower = Lowercase(raw.substr(0, headersEnd));
        std::string body = raw.substr(headersEnd + 4);
        if (headersLower.find("transfer-encoding: chunked") != std::string::npos)
        {
            std::string decoded;
            if (!DecodeChunkedBody(body, decoded))
            {
                error = "Sunshine sent an invalid chunked response";
                return std::nullopt;
            }
            body = std::move(decoded);
        }
        return HttpResponse{status, std::move(body)};
    }

    [[nodiscard]] std::optional<HttpResponse> RequestHttp(
        const std::string& host,
        const std::uint16_t port,
        const std::string_view endpoint,
        const std::string& uniqueId,
        const std::string& arguments,
        std::string& error,
        const DWORD timeoutMilliseconds = kRequestTimeoutMilliseconds)
    {
        const std::string request = BuildRequest(host, port, endpoint, uniqueId, arguments, error);
        if (request.empty())
        {
            return std::nullopt;
        }

        auto socket = ConnectSocket(host, port, timeoutMilliseconds, error);
        if (!socket)
        {
            return std::nullopt;
        }
        if (!SendSocketAll(socket->Get(), request, error))
        {
            return std::nullopt;
        }
        std::string raw;
        if (!ReceiveSocketAll(socket->Get(), raw, error))
        {
            return std::nullopt;
        }
        return ParseHttpResponse(raw, error);
    }

    [[nodiscard]] bool LoadPemIdentity(
        const ClientIdentity& identity,
        X509Ptr& certificate,
        EvpPkeyPtr& privateKey,
        std::string& error)
    {
        BioPtr certificateBio(BIO_new_mem_buf(identity.certificatePem.data(), static_cast<int>(identity.certificatePem.size())), BIO_free);
        BioPtr keyBio(BIO_new_mem_buf(identity.privateKeyPem.data(), static_cast<int>(identity.privateKeyPem.size())), BIO_free);
        if (!certificateBio || !keyBio)
        {
            error = "Could not read Vanta's pairing identity";
            return false;
        }
        certificate.reset(PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr));
        privateKey.reset(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
        if (!certificate || !privateKey)
        {
            error = "Vanta's saved pairing identity is invalid";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool LoadPemCertificate(const std::string& certificatePem, X509Ptr& certificate, std::string& error)
    {
        BioPtr certificateBio(BIO_new_mem_buf(certificatePem.data(), static_cast<int>(certificatePem.size())), BIO_free);
        if (!certificateBio)
        {
            error = "Could not read Sunshine's certificate";
            return false;
        }
        certificate.reset(PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr));
        if (!certificate)
        {
            error = "Sunshine supplied an invalid certificate";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool SendSslAll(SSL* ssl, const std::string& request, std::string& error)
    {
        std::size_t sent{};
        while (sent < request.size())
        {
            const int count = SSL_write(ssl, request.data() + sent, static_cast<int>(std::min<std::size_t>(request.size() - sent, std::numeric_limits<int>::max())));
            if (count <= 0)
            {
                error = "Could not send the encrypted Sunshine request";
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    [[nodiscard]] bool ReceiveSslAll(SSL* ssl, std::string& response, std::string& error)
    {
        std::array<char, 8192> buffer{};
        for (;;)
        {
            const int count = SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
            if (count > 0)
            {
                response.append(buffer.data(), static_cast<std::size_t>(count));
                continue;
            }

            const int sslError = SSL_get_error(ssl, count);
            if (sslError == SSL_ERROR_ZERO_RETURN || (sslError == SSL_ERROR_SYSCALL && count == 0))
            {
                return true;
            }
            error = "Timed out while reading Sunshine's encrypted response";
            return false;
        }
    }

    [[nodiscard]] std::optional<HttpResponse> RequestHttps(
        const std::string& host,
        const std::uint16_t port,
        const std::string_view endpoint,
        const ClientIdentity& identity,
        const std::string& pinnedCertificatePem,
        const std::string& arguments,
        std::string& error,
        const DWORD timeoutMilliseconds = kRequestTimeoutMilliseconds)
    {
        const std::string request = BuildRequest(host, port, endpoint, identity.uniqueId, arguments, error);
        if (request.empty())
        {
            return std::nullopt;
        }

        auto socket = ConnectSocket(host, port, timeoutMilliseconds, error);
        if (!socket)
        {
            return std::nullopt;
        }

        X509Ptr clientCertificate(nullptr, X509_free);
        EvpPkeyPtr clientPrivateKey(nullptr, EVP_PKEY_free);
        X509Ptr expectedServerCertificate(nullptr, X509_free);
        if (!LoadPemIdentity(identity, clientCertificate, clientPrivateKey, error) ||
            !LoadPemCertificate(pinnedCertificatePem, expectedServerCertificate, error))
        {
            return std::nullopt;
        }

        SslContextPtr context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
        if (!context)
        {
            error = OpenSslError();
            return std::nullopt;
        }
        SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION);
        if (SSL_CTX_use_certificate(context.get(), clientCertificate.get()) != 1 ||
            SSL_CTX_use_PrivateKey(context.get(), clientPrivateKey.get()) != 1 ||
            SSL_CTX_check_private_key(context.get()) != 1)
        {
            error = "Could not use Vanta's pairing identity for the encrypted request";
            return std::nullopt;
        }

        SslPtr ssl(SSL_new(context.get()), SSL_free);
        if (!ssl)
        {
            error = OpenSslError();
            return std::nullopt;
        }
        SSL_set_fd(ssl.get(), static_cast<int>(socket->Get()));
        if (host.find(':') == std::string::npos)
        {
            SSL_set_tlsext_host_name(ssl.get(), host.c_str());
        }
        if (SSL_connect(ssl.get()) != 1)
        {
            error = "Could not establish Sunshine's encrypted pairing connection";
            return std::nullopt;
        }

        X509Ptr peerCertificate(SSL_get1_peer_certificate(ssl.get()), X509_free);
        if (!peerCertificate || X509_cmp(peerCertificate.get(), expectedServerCertificate.get()) != 0)
        {
            error = "Sunshine's TLS certificate does not match the certificate received during pairing";
            return std::nullopt;
        }

        if (!SendSslAll(ssl.get(), request, error))
        {
            return std::nullopt;
        }
        std::string raw;
        if (!ReceiveSslAll(ssl.get(), raw, error))
        {
            return std::nullopt;
        }
        return ParseHttpResponse(raw, error);
    }

    [[nodiscard]] bool RandomBytes(const std::size_t size, Bytes& result, std::string& error)
    {
        result.resize(size);
        if (RAND_bytes(result.data(), static_cast<int>(result.size())) != 1)
        {
            result.clear();
            error = "Could not create cryptographically secure pairing data";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Digest(const Bytes& input, const EVP_MD* algorithm, Bytes& result, std::string& error)
    {
        const int size = EVP_MD_get_size(algorithm);
        if (size <= 0)
        {
            error = "Could not select the pairing hash algorithm";
            return false;
        }
        result.resize(static_cast<std::size_t>(size));
        unsigned int outputLength{};
        if (EVP_Digest(input.data(), input.size(), result.data(), &outputLength, algorithm, nullptr) != 1)
        {
            result.clear();
            error = "Could not hash pairing data";
            return false;
        }
        result.resize(outputLength);
        return true;
    }

    [[nodiscard]] bool TransformAesEcb(
        const Bytes& input,
        const Bytes& key,
        const bool encrypt,
        Bytes& result,
        std::string& error)
    {
        if (key.size() != 16 || input.empty() || input.size() % 16 != 0)
        {
            error = "Sunshine sent invalid pairing cryptography data";
            return false;
        }

        EvpCipherContextPtr context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        if (!context)
        {
            error = OpenSslError();
            return false;
        }
        const int initialized = encrypt
            ? EVP_EncryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr)
            : EVP_DecryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr);
        if (initialized != 1 || EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1)
        {
            error = "Could not initialize Sunshine pairing encryption";
            return false;
        }

        result.resize(input.size() + 16);
        int updateLength{};
        int finalLength{};
        const int updated = encrypt
            ? EVP_EncryptUpdate(context.get(), result.data(), &updateLength, input.data(), static_cast<int>(input.size()))
            : EVP_DecryptUpdate(context.get(), result.data(), &updateLength, input.data(), static_cast<int>(input.size()));
        const int finalized = encrypt
            ? EVP_EncryptFinal_ex(context.get(), result.data() + updateLength, &finalLength)
            : EVP_DecryptFinal_ex(context.get(), result.data() + updateLength, &finalLength);
        if (updated != 1 || finalized != 1)
        {
            result.clear();
            error = "Could not process Sunshine pairing encryption";
            return false;
        }
        result.resize(static_cast<std::size_t>(updateLength + finalLength));
        return true;
    }

    [[nodiscard]] bool GetCertificateSignature(X509* certificate, Bytes& signature, std::string& error)
    {
        const ASN1_BIT_STRING* rawSignature{};
        X509_get0_signature(&rawSignature, nullptr, certificate);
        if (rawSignature == nullptr)
        {
            error = "Could not read the pairing certificate signature";
            return false;
        }
        const unsigned char* data = ASN1_STRING_get0_data(rawSignature);
        const int size = ASN1_STRING_length(rawSignature);
        if (data == nullptr || size <= 0)
        {
            error = "Could not read the pairing certificate signature";
            return false;
        }
        signature.assign(data, data + size);
        return true;
    }

    [[nodiscard]] bool GetPemCertificateSignature(const std::string& certificatePem, Bytes& signature, std::string& error)
    {
        X509Ptr certificate(nullptr, X509_free);
        if (!LoadPemCertificate(certificatePem, certificate, error))
        {
            return false;
        }
        return GetCertificateSignature(certificate.get(), signature, error);
    }

    [[nodiscard]] bool VerifyServerSignature(
        const Bytes& data,
        const Bytes& signature,
        const std::string& certificatePem,
        std::string& error)
    {
        X509Ptr certificate(nullptr, X509_free);
        if (!LoadPemCertificate(certificatePem, certificate, error))
        {
            return false;
        }
        EvpPkeyPtr publicKey(X509_get_pubkey(certificate.get()), EVP_PKEY_free);
        EvpDigestContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!publicKey || !context ||
            EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, publicKey.get()) != 1 ||
            EVP_DigestVerifyUpdate(context.get(), data.data(), data.size()) != 1)
        {
            error = "Could not verify Sunshine's pairing response";
            return false;
        }
        if (EVP_DigestVerifyFinal(context.get(), signature.data(), signature.size()) != 1)
        {
            error = "Sunshine's pairing response failed certificate verification";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool SignWithIdentity(
        const ClientIdentity& identity,
        const Bytes& message,
        Bytes& signature,
        std::string& error)
    {
        X509Ptr certificate(nullptr, X509_free);
        EvpPkeyPtr privateKey(nullptr, EVP_PKEY_free);
        if (!LoadPemIdentity(identity, certificate, privateKey, error))
        {
            return false;
        }
        EvpDigestContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context ||
            EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, privateKey.get()) != 1 ||
            EVP_DigestSignUpdate(context.get(), message.data(), message.size()) != 1)
        {
            error = "Could not sign the Sunshine pairing response";
            return false;
        }

        std::size_t length{};
        if (EVP_DigestSignFinal(context.get(), nullptr, &length) != 1 || length == 0)
        {
            error = "Could not create a Sunshine pairing signature";
            return false;
        }
        signature.resize(length);
        if (EVP_DigestSignFinal(context.get(), signature.data(), &length) != 1)
        {
            signature.clear();
            error = "Could not create a Sunshine pairing signature";
            return false;
        }
        signature.resize(length);
        return true;
    }

    [[nodiscard]] std::filesystem::path StorageDirectory()
    {
        PWSTR localAppData{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)))
        {
            return {};
        }
        const std::filesystem::path result = std::filesystem::path(localAppData) / L"Vanta" / L"sunshine";
        CoTaskMemFree(localAppData);
        return result;
    }

    [[nodiscard]] bool EnsureStorageDirectory(const std::filesystem::path& directory, std::string& error)
    {
        std::error_code fileError;
        std::filesystem::create_directories(directory, fileError);
        if (fileError)
        {
            error = "Could not create Vanta's per-user pairing storage";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ReadFileBytes(const std::filesystem::path& path, Bytes& result, std::string& error)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            error = "Could not read Vanta's saved pairing data";
            return false;
        }
        const std::streamsize size = stream.tellg();
        if (size < 0 || size > static_cast<std::streamsize>(std::numeric_limits<DWORD>::max()))
        {
            error = "Vanta's saved pairing data is invalid";
            return false;
        }
        stream.seekg(0, std::ios::beg);
        result.resize(static_cast<std::size_t>(size));
        if (!result.empty() && !stream.read(reinterpret_cast<char*>(result.data()), size))
        {
            result.clear();
            error = "Could not read Vanta's saved pairing data";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool WriteFileBytesAtomic(const std::filesystem::path& path, const Bytes& bytes, std::string& error)
    {
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream || (!bytes.empty() && !stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))))
            {
                error = "Could not write Vanta's encrypted pairing data";
                return false;
            }
            stream.flush();
            if (!stream)
            {
                error = "Could not write Vanta's encrypted pairing data";
                return false;
            }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = "Could not finalize Vanta's encrypted pairing data";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ProtectForCurrentUser(const Bytes& plaintext, Bytes& encrypted, std::string& error)
    {
        if (plaintext.empty() || plaintext.size() > std::numeric_limits<DWORD>::max())
        {
            error = "Vanta's pairing data is invalid";
            return false;
        }
        DATA_BLOB input{static_cast<DWORD>(plaintext.size()), const_cast<BYTE*>(plaintext.data())};
        DATA_BLOB output{};
        if (!CryptProtectData(&input, L"Vanta Sunshine Receiver", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        {
            error = "Could not protect Vanta's pairing credentials";
            return false;
        }
        encrypted.assign(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return true;
    }

    [[nodiscard]] bool UnprotectForCurrentUser(const Bytes& encrypted, Bytes& plaintext, std::string& error)
    {
        if (encrypted.empty() || encrypted.size() > std::numeric_limits<DWORD>::max())
        {
            error = "Vanta's saved pairing data is invalid";
            return false;
        }
        DATA_BLOB input{static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data())};
        DATA_BLOB output{};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        {
            error = "Vanta's saved pairing data could not be unlocked for this Windows user";
            return false;
        }
        plaintext.assign(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return true;
    }

    void AppendU32(Bytes& output, const std::uint32_t value)
    {
        output.push_back(static_cast<unsigned char>(value & 0xFFU));
        output.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
        output.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
        output.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    }

    [[nodiscard]] bool ReadU32(const Bytes& input, std::size_t& offset, std::uint32_t& value)
    {
        if (input.size() - offset < 4)
        {
            return false;
        }
        value = static_cast<std::uint32_t>(input[offset]) |
                (static_cast<std::uint32_t>(input[offset + 1]) << 8U) |
                (static_cast<std::uint32_t>(input[offset + 2]) << 16U) |
                (static_cast<std::uint32_t>(input[offset + 3]) << 24U);
        offset += 4;
        return true;
    }

    [[nodiscard]] bool AppendString(Bytes& output, const std::string& value, std::string& error)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
        {
            error = "Vanta's pairing data is too large";
            return false;
        }
        AppendU32(output, static_cast<std::uint32_t>(value.size()));
        output.insert(output.end(), value.begin(), value.end());
        return true;
    }

    [[nodiscard]] bool ReadString(const Bytes& input, std::size_t& offset, std::string& value)
    {
        std::uint32_t length{};
        if (!ReadU32(input, offset, length) || length > input.size() - offset)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(input.data() + offset), length);
        offset += length;
        return true;
    }

    [[nodiscard]] bool GenerateIdentity(ClientIdentity& identity, std::string& error)
    {
        EvpPkeyPtr privateKey(nullptr, EVP_PKEY_free);
        EVP_PKEY* generatedKey{};
        {
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
            if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1 ||
                EVP_PKEY_keygen(context.get(), &generatedKey) != 1)
            {
                error = "Could not generate Vanta's pairing key";
                return false;
            }
        }
        privateKey.reset(generatedKey);

        X509Ptr certificate(X509_new(), X509_free);
        if (!certificate ||
            X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 0) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60L * 60L * 24L * 365L * 20L) == nullptr ||
            X509_set_pubkey(certificate.get(), privateKey.get()) != 1)
        {
            error = "Could not generate Vanta's pairing certificate";
            return false;
        }

        X509_NAME* name = X509_NAME_new();
        if (name == nullptr ||
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>("NVIDIA GameStream Client"),
                                       -1, -1, 0) != 1 ||
            X509_set_subject_name(certificate.get(), name) != 1 ||
            X509_set_issuer_name(certificate.get(), name) != 1 ||
            X509_sign(certificate.get(), privateKey.get(), EVP_sha256()) <= 0)
        {
            if (name != nullptr) X509_NAME_free(name);
            error = "Could not generate Vanta's pairing certificate";
            return false;
        }
        X509_NAME_free(name);

        BioPtr certificateBio(BIO_new(BIO_s_mem()), BIO_free);
        BioPtr keyBio(BIO_new(BIO_s_mem()), BIO_free);
        if (!certificateBio || !keyBio ||
            PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1 ||
            PEM_write_bio_PrivateKey(keyBio.get(), privateKey.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        {
            error = "Could not serialize Vanta's pairing identity";
            return false;
        }

        BUF_MEM* certificateMemory{};
        BUF_MEM* keyMemory{};
        BIO_get_mem_ptr(certificateBio.get(), &certificateMemory);
        BIO_get_mem_ptr(keyBio.get(), &keyMemory);
        if (certificateMemory == nullptr || keyMemory == nullptr || certificateMemory->length == 0 || keyMemory->length == 0)
        {
            error = "Could not serialize Vanta's pairing identity";
            return false;
        }
        identity.certificatePem.assign(certificateMemory->data, certificateMemory->length);
        identity.privateKeyPem.assign(keyMemory->data, keyMemory->length);

        Bytes randomId;
        if (!RandomBytes(8, randomId, error))
        {
            return false;
        }
        identity.uniqueId = HexEncode(randomId);
        SecureZeroMemory(randomId.data(), randomId.size());
        return true;
    }

    [[nodiscard]] bool SerializeIdentity(const ClientIdentity& identity, Bytes& output, std::string& error)
    {
        output.assign(kIdentityMagic.begin(), kIdentityMagic.end());
        return AppendString(output, identity.certificatePem, error) &&
               AppendString(output, identity.privateKeyPem, error) &&
               AppendString(output, identity.uniqueId, error);
    }

    [[nodiscard]] bool DeserializeIdentity(const Bytes& input, ClientIdentity& identity)
    {
        if (input.size() < kIdentityMagic.size() || !std::equal(kIdentityMagic.begin(), kIdentityMagic.end(), input.begin()))
        {
            return false;
        }
        std::size_t offset = kIdentityMagic.size();
        return ReadString(input, offset, identity.certificatePem) &&
               ReadString(input, offset, identity.privateKeyPem) &&
               ReadString(input, offset, identity.uniqueId) &&
               offset == input.size();
    }

    [[nodiscard]] bool SerializePairedHost(const vanta::sunshine::PairedHost& host, Bytes& output, std::string& error)
    {
        output.assign(kHostMagic.begin(), kHostMagic.end());
        return AppendString(output, host.info.address, error) &&
               AppendString(output, host.info.name, error) &&
               AppendString(output, host.info.appVersion, error) &&
               AppendString(output, host.info.gfeVersion, error) &&
               (AppendU32(output, host.info.httpsPort), true) &&
               (AppendU32(output, static_cast<std::uint32_t>(host.info.codecSupport)), true) &&
               AppendString(output, host.certificatePem, error);
    }

    [[nodiscard]] bool DeserializePairedHost(const Bytes& input, vanta::sunshine::PairedHost& host)
    {
        if (input.size() < kHostMagic.size() || !std::equal(kHostMagic.begin(), kHostMagic.end(), input.begin()))
        {
            return false;
        }
        std::size_t offset = kHostMagic.size();
        std::uint32_t port{};
        std::uint32_t codecSupport{};
        if (!ReadString(input, offset, host.info.address) ||
            !ReadString(input, offset, host.info.name) ||
            !ReadString(input, offset, host.info.appVersion) ||
            !ReadString(input, offset, host.info.gfeVersion) ||
            !ReadU32(input, offset, port) ||
            !ReadU32(input, offset, codecSupport) ||
            !ReadString(input, offset, host.certificatePem) ||
            offset != input.size() || port == 0 || port > std::numeric_limits<std::uint16_t>::max())
        {
            return false;
        }
        host.info.httpsPort = static_cast<std::uint16_t>(port);
        host.info.codecSupport = static_cast<int>(codecSupport);
        return true;
    }

    [[nodiscard]] std::filesystem::path HostRecordPath(const std::filesystem::path& storage, const std::string& host)
    {
        const std::string canonicalHost = Lowercase(Trim(host));
        Bytes input(canonicalHost.begin(), canonicalHost.end());
        Bytes hash(EVP_MAX_MD_SIZE);
        unsigned int hashLength{};
        EVP_Digest(input.data(), input.size(), hash.data(), &hashLength, EVP_sha256(), nullptr);
        hash.resize(hashLength);
        return storage / L"hosts" / (std::filesystem::path(HexEncode(hash)).wstring() + L".bin");
    }

    [[nodiscard]] bool LoadOrCreateIdentity(const std::filesystem::path& storage, ClientIdentity& identity, std::string& error)
    {
        if (!EnsureStorageDirectory(storage, error))
        {
            return false;
        }
        const std::filesystem::path identityPath = storage / L"identity.bin";
        std::error_code existenceError;
        const bool exists = std::filesystem::exists(identityPath, existenceError);
        if (existenceError)
        {
            error = "Could not inspect Vanta's saved pairing identity";
            return false;
        }

        if (exists)
        {
            Bytes encrypted;
            Bytes plaintext;
            if (!ReadFileBytes(identityPath, encrypted, error) || !UnprotectForCurrentUser(encrypted, plaintext, error) ||
                !DeserializeIdentity(plaintext, identity))
            {
                if (error.empty()) error = "Vanta's saved pairing identity is invalid";
                return false;
            }
            X509Ptr certificate(nullptr, X509_free);
            EvpPkeyPtr privateKey(nullptr, EVP_PKEY_free);
            if (!LoadPemIdentity(identity, certificate, privateKey, error))
            {
                return false;
            }
            SecureZeroMemory(plaintext.data(), plaintext.size());
            return true;
        }

        if (!GenerateIdentity(identity, error))
        {
            return false;
        }
        Bytes plaintext;
        Bytes encrypted;
        if (!SerializeIdentity(identity, plaintext, error) || !ProtectForCurrentUser(plaintext, encrypted, error) || !WriteFileBytesAtomic(identityPath, encrypted, error))
        {
            if (!plaintext.empty()) SecureZeroMemory(plaintext.data(), plaintext.size());
            return false;
        }
        SecureZeroMemory(plaintext.data(), plaintext.size());
        return true;
    }

    [[nodiscard]] bool SavePairedHost(
        const std::filesystem::path& storage,
        const vanta::sunshine::PairedHost& host,
        std::string& error)
    {
        const std::filesystem::path hostDirectory = storage / L"hosts";
        if (!EnsureStorageDirectory(hostDirectory, error))
        {
            return false;
        }
        Bytes plaintext;
        Bytes encrypted;
        const std::filesystem::path path = HostRecordPath(storage, host.info.address);
        if (!SerializePairedHost(host, plaintext, error) || !ProtectForCurrentUser(plaintext, encrypted, error) || !WriteFileBytesAtomic(path, encrypted, error))
        {
            if (!plaintext.empty()) SecureZeroMemory(plaintext.data(), plaintext.size());
            return false;
        }
        SecureZeroMemory(plaintext.data(), plaintext.size());
        return true;
    }

    [[nodiscard]] std::optional<vanta::sunshine::PairedHost> ReadPairedHost(
        const std::filesystem::path& storage,
        const std::string& host,
        std::string& error)
    {
        const std::filesystem::path path = HostRecordPath(storage, host);
        std::error_code existenceError;
        if (!std::filesystem::exists(path, existenceError))
        {
            error = existenceError ? "Could not inspect Vanta's saved pairing records" : "This Sunshine host has not been paired with Vanta";
            return std::nullopt;
        }

        Bytes encrypted;
        Bytes plaintext;
        vanta::sunshine::PairedHost paired;
        if (!ReadFileBytes(path, encrypted, error) || !UnprotectForCurrentUser(encrypted, plaintext, error) || !DeserializePairedHost(plaintext, paired))
        {
            if (error.empty()) error = "Vanta's saved Sunshine pairing record is invalid";
            return std::nullopt;
        }
        if (!plaintext.empty()) SecureZeroMemory(plaintext.data(), plaintext.size());
        if (Lowercase(Trim(paired.info.address)) != Lowercase(Trim(host)))
        {
            error = "Vanta's saved pairing record does not match this Sunshine host";
            return std::nullopt;
        }
        X509Ptr certificate(nullptr, X509_free);
        if (!LoadPemCertificate(paired.certificatePem, certificate, error))
        {
            return std::nullopt;
        }
        return paired;
    }

    [[nodiscard]] std::optional<vanta::sunshine::HostInfo> DiscoverHost(
        const std::string& requestedHost,
        const std::string& uniqueId,
        std::string& error)
    {
        const std::string host = Trim(requestedHost);
        if (host.empty())
        {
            error = "Enter the LAN address of the Sunshine host";
            return std::nullopt;
        }
        const auto response = RequestHttp(host, kGameStreamHttpPort, "serverinfo", uniqueId, {}, error);
        if (!response || !VerifyGameStreamResponse(*response, error))
        {
            return std::nullopt;
        }

        vanta::sunshine::HostInfo info;
        info.address = host;
        info.name = XmlValue(response->body, "hostname");
        info.appVersion = XmlValue(response->body, "appversion");
        info.gfeVersion = XmlValue(response->body, "GfeVersion");
        const std::string codecSupport = XmlValue(response->body, "ServerCodecModeSupport");
        if (info.appVersion.empty() || codecSupport.empty())
        {
            error = "The host replied but is not a compatible Sunshine/GameStream server";
            return std::nullopt;
        }

        std::uint32_t codecs{};
        const auto [codecEnd, codecResult] = std::from_chars(codecSupport.data(), codecSupport.data() + codecSupport.size(), codecs);
        if (codecResult != std::errc{} || codecEnd != codecSupport.data() + codecSupport.size())
        {
            error = "Sunshine returned an invalid codec capability list";
            return std::nullopt;
        }
        info.codecSupport = static_cast<int>(codecs);

        const std::string httpsPort = XmlValue(response->body, "HttpsPort");
        if (!httpsPort.empty())
        {
            std::uint32_t parsedPort{};
            const auto [portEnd, portResult] = std::from_chars(httpsPort.data(), httpsPort.data() + httpsPort.size(), parsedPort);
            if (portResult != std::errc{} || portEnd != httpsPort.data() + httpsPort.size() || parsedPort == 0 || parsedPort > std::numeric_limits<std::uint16_t>::max())
            {
                error = "Sunshine returned an invalid HTTPS port";
                return std::nullopt;
            }
            info.httpsPort = static_cast<std::uint16_t>(parsedPort);
        }
        else
        {
            info.httpsPort = kDefaultHttpsPort;
        }
        return info;
    }

    [[nodiscard]] int ServerMajorVersion(const std::string& version)
    {
        const std::size_t end = version.find('.');
        const std::string_view first(version.data(), end == std::string::npos ? version.size() : end);
        int major{};
        const auto [pointer, result] = std::from_chars(first.data(), first.data() + first.size(), major);
        return result == std::errc{} && pointer == first.data() + first.size() ? major : 7;
    }

    struct SunshineApplication
    {
        int id{};
        std::string title;
    };

    [[nodiscard]] bool ParseInteger(const std::string& value, int& result)
    {
        if (value.empty())
        {
            return false;
        }
        const auto [pointer, conversion] = std::from_chars(value.data(), value.data() + value.size(), result);
        return conversion == std::errc{} && pointer == value.data() + value.size();
    }

    [[nodiscard]] std::vector<SunshineApplication> ParseApplications(const std::string& xml)
    {
        std::vector<SunshineApplication> applications;
        std::size_t position{};
        while ((position = xml.find("<App", position)) != std::string::npos)
        {
            const std::size_t tagEnd = xml.find('>', position);
            if (tagEnd == std::string::npos)
            {
                break;
            }
            const char separator = position + 4 < xml.size() ? xml[position + 4] : '\0';
            if (separator != '>' && std::isspace(static_cast<unsigned char>(separator)) == 0)
            {
                position = tagEnd + 1;
                continue;
            }
            const std::size_t end = xml.find("</App>", tagEnd + 1);
            if (end == std::string::npos)
            {
                break;
            }
            const std::string entry = xml.substr(tagEnd + 1, end - tagEnd - 1);
            SunshineApplication application;
            if (ParseInteger(XmlValue(entry, "ID"), application.id))
            {
                application.title = XmlValue(entry, "AppTitle");
                applications.push_back(std::move(application));
            }
            position = end + std::strlen("</App>");
        }
        return applications;
    }

    [[nodiscard]] int GetCurrentGame(const std::string& serverInfo)
    {
        const std::string state = XmlValue(serverInfo, "state");
        if (!state.ends_with("_SERVER_BUSY"))
        {
            return 0;
        }
        int gameId{};
        return ParseInteger(XmlValue(serverInfo, "currentgame"), gameId) && gameId > 0 ? gameId : 0;
    }

    [[nodiscard]] bool RefreshPairedHostInfo(
        const std::string& requestedHost,
        const ClientIdentity& identity,
        vanta::sunshine::PairedHost& pairedHost,
        std::string& authenticatedServerInfo,
        std::string& error)
    {
        const auto discovered = DiscoverHost(requestedHost, identity.uniqueId, error);
        if (!discovered)
        {
            return false;
        }
        const auto response = RequestHttps(
            discovered->address,
            discovered->httpsPort,
            "serverinfo",
            identity,
            pairedHost.certificatePem,
            {},
            error);
        if (!response || !VerifyGameStreamResponse(*response, error))
        {
            return false;
        }

        pairedHost.info = *discovered;
        const std::string authenticatedName = XmlValue(response->body, "hostname");
        const std::string authenticatedVersion = XmlValue(response->body, "appversion");
        const std::string authenticatedGfeVersion = XmlValue(response->body, "GfeVersion");
        if (!authenticatedName.empty()) pairedHost.info.name = authenticatedName;
        if (!authenticatedVersion.empty()) pairedHost.info.appVersion = authenticatedVersion;
        if (!authenticatedGfeVersion.empty()) pairedHost.info.gfeVersion = authenticatedGfeVersion;
        authenticatedServerInfo = response->body;
        return true;
    }

    [[nodiscard]] bool IsSaneStreamRequest(const vanta::sunshine::StreamRequest& request, std::string& error)
    {
        if (request.width < 320 || request.width > 8192 || request.height < 240 || request.height > 8192 ||
            request.width % 2 != 0 || request.height % 2 != 0 || request.fps < 24 || request.fps > 240 ||
            request.bitrateKbps < 1000 || request.bitrateKbps > 250000 ||
            request.packetSize < 512 || request.packetSize > 1500)
        {
            error = "The requested Sunshine stream settings are outside the supported LAN range";
            return false;
        }
        const bool hasInputKey = std::any_of(request.remoteInputKey.begin(), request.remoteInputKey.end(), [](unsigned char value) { return value != 0; });
        const bool hasInputIv = std::any_of(request.remoteInputIv.begin(), request.remoteInputIv.end(), [](unsigned char value) { return value != 0; });
        if (!hasInputKey || !hasInputIv)
        {
            error = "Vanta did not generate the secure remote-input key for this stream";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string LaunchArguments(const int appId, const vanta::sunshine::StreamRequest& request)
    {
        const std::uint32_t remoteInputKeyId =
            (static_cast<std::uint32_t>(request.remoteInputIv[0]) << 24U) |
            (static_cast<std::uint32_t>(request.remoteInputIv[1]) << 16U) |
            (static_cast<std::uint32_t>(request.remoteInputIv[2]) << 8U) |
            static_cast<std::uint32_t>(request.remoteInputIv[3]);
        const std::uint32_t surroundAudioInfo =
            ((static_cast<std::uint32_t>(request.audioConfiguration) >> 16U) & 0xFFFFU) << 16U |
            ((static_cast<std::uint32_t>(request.audioConfiguration) >> 8U) & 0xFFU);

        std::string result =
            "appid=" + std::to_string(appId) +
            "&mode=" + std::to_string(request.width) + "x" + std::to_string(request.height) + "x" + std::to_string(request.fps) +
            "&additionalStates=1&sops=" + std::to_string(request.requestDisplayMode ? 1 : 0) +
            "&rikey=" + HexEncode(request.remoteInputKey.data(), request.remoteInputKey.size()) +
            "&rikeyid=" + std::to_string(remoteInputKeyId) +
            "&localAudioPlayMode=0&surroundAudioInfo=" + std::to_string(surroundAudioInfo) +
            "&remoteControllersBitmap=0&gcmap=0&gcpersist=0";
        const char* moonlightExtensions = LiGetLaunchUrlQueryParameters();
        if (moonlightExtensions != nullptr)
        {
            result += moonlightExtensions;
        }
        return result;
    }

    void BestEffortUnpair(const vanta::sunshine::HostInfo& host, const ClientIdentity& identity)
    {
        std::string ignored;
        static_cast<void>(RequestHttp(host.address, kGameStreamHttpPort, "unpair", identity.uniqueId, {}, ignored));
    }
}

namespace vanta::sunshine
{
    struct PairingService::Implementation
    {
        std::filesystem::path storageDirectory{StorageDirectory()};
        std::mutex operationMutex;
    };

    PairingService::PairingService() : implementation_(new Implementation()) {}
    PairingService::~PairingService() { delete implementation_; }

    std::optional<HostInfo> PairingService::Discover(const std::string& host, std::string& error) const
    {
        // GameStream server-info is intentionally available before pairing.
        // Use a short random-looking UID here so a discovery does not reveal a
        // persisted client identity to an untrusted address.
        Bytes discoveryId;
        if (!RandomBytes(8, discoveryId, error))
        {
            return std::nullopt;
        }
        const std::string uniqueId = HexEncode(discoveryId);
        SecureZeroMemory(discoveryId.data(), discoveryId.size());
        return DiscoverHost(host, uniqueId, error);
    }

    std::vector<HostInfo> PairingService::DiscoverLan(std::string& error) const
    {
        std::vector<MdnsEndpoint> endpoints = BrowseSunshineLan(error);
        if (endpoints.empty())
        {
            if (error.empty())
            {
                error = "No Sunshine hosts advertised _nvstream._tcp.local on this LAN";
            }
            return {};
        }

        Bytes discoveryId;
        if (!RandomBytes(8, discoveryId, error))
        {
            return {};
        }
        const std::string uniqueId = HexEncode(discoveryId);
        SecureZeroMemory(discoveryId.data(), discoveryId.size());

        std::vector<HostInfo> hosts;
        std::string lastDiscoveryError;
        for (const MdnsEndpoint& endpoint : endpoints)
        {
            if (endpoint.port != kGameStreamHttpPort)
            {
                lastDiscoveryError = "A Sunshine host advertised an unsupported GameStream port";
                continue;
            }

            const std::string address = ResolveIpv4Address(endpoint.host).value_or(endpoint.host);
            std::string hostError;
            const auto host = DiscoverHost(address, uniqueId, hostError);
            if (!host)
            {
                if (!hostError.empty())
                {
                    lastDiscoveryError = std::move(hostError);
                }
                continue;
            }

            const bool alreadyPresent = std::any_of(
                hosts.begin(),
                hosts.end(),
                [&host](const HostInfo& existing)
                {
                    return Lowercase(existing.address) == Lowercase(host->address);
                });
            if (!alreadyPresent)
            {
                hosts.push_back(*host);
            }
        }

        if (hosts.empty())
        {
            error = lastDiscoveryError.empty()
                ? "Sunshine was advertised on this LAN but did not answer GameStream server-info"
                : lastDiscoveryError;
        }
        else
        {
            error.clear();
        }
        return hosts;
    }

    std::string PairingService::GeneratePairingPin(std::string& error) const
    {
        // Rejection sampling makes the four-digit range uniform rather than
        // introducing a small modulo bias from the 16-bit random source.
        for (;;)
        {
            Bytes random;
            if (!RandomBytes(2, random, error))
            {
                return {};
            }

            const unsigned int candidate =
                (static_cast<unsigned int>(random[0]) << 8U) |
                static_cast<unsigned int>(random[1]);
            SecureZeroMemory(random.data(), random.size());
            if (candidate >= 60000U)
            {
                continue;
            }

            unsigned int value = candidate % 10000U;
            std::string pin(4, '0');
            for (std::size_t index = pin.size(); index > 0; --index)
            {
                pin[index - 1] = static_cast<char>('0' + value % 10U);
                value /= 10U;
            }
            return pin;
        }
    }

    PairingResult PairingService::Pair(
        const std::string& requestedHost,
        const std::string& pin,
        const std::function<void(const std::string&)>& setProgress)
    {
        PairingResult result;
        const std::string host = Trim(requestedHost);
        if (host.empty())
        {
            result.message = "Enter the LAN address of the Sunshine host";
            return result;
        }
        if (pin.size() != 4 || !std::all_of(pin.begin(), pin.end(), [](unsigned char character) { return std::isdigit(character) != 0; }))
        {
            result.message = "Sunshine pairing requires the four-digit PIN shown in Vanta";
            return result;
        }

        std::lock_guard<std::mutex> lock(implementation_->operationMutex);
        const auto progress = [&setProgress](const std::string& text)
        {
            if (setProgress)
            {
                setProgress(text);
            }
        };

        std::string error;
        ClientIdentity identity;
        progress("Preparing Vanta's secure pairing identity...");
        if (!LoadOrCreateIdentity(implementation_->storageDirectory, identity, error))
        {
            result.message = error;
            return result;
        }

        progress("Reading Sunshine server information...");
        const auto info = DiscoverHost(host, identity.uniqueId, error);
        if (!info)
        {
            result.message = error;
            return result;
        }

        const int serverMajorVersion = ServerMajorVersion(info->appVersion);
        const EVP_MD* hashAlgorithm = serverMajorVersion >= 7 ? EVP_sha256() : EVP_sha1();
        const std::size_t hashLength = serverMajorVersion >= 7 ? 32 : 20;

        Bytes salt;
        Bytes pinMaterial;
        Bytes aesKey;
        if (!RandomBytes(16, salt, error))
        {
            result.message = error;
            return result;
        }
        pinMaterial = salt;
        pinMaterial.insert(pinMaterial.end(), pin.begin(), pin.end());
        if (!Digest(pinMaterial, hashAlgorithm, aesKey, error) || aesKey.size() < 16)
        {
            result.message = error;
            return result;
        }
        aesKey.resize(16);

        const auto fail = [&](const std::string& message, const bool unpair) -> PairingResult
        {
            if (unpair)
            {
                BestEffortUnpair(*info, identity);
            }
            PairingResult failed;
            failed.message = message;
            return failed;
        };

        progress(
            "Waiting for Sunshine approval: enter PIN " + pin +
            " in the host Web UI's PIN page, name the device Vanta, then select Send (up to 2 minutes)...");
        const std::string initialArguments =
            "devicename=vanta&updateState=1&phrase=getservercert&salt=" + HexEncode(salt) +
            "&clientcert=" + HexEncode(reinterpret_cast<const unsigned char*>(identity.certificatePem.data()), identity.certificatePem.size());
        const auto initialResponse = RequestHttp(
            info->address,
            kGameStreamHttpPort,
            "pair",
            identity.uniqueId,
            initialArguments,
            error,
            kPairingPinEntryTimeoutMilliseconds);
        if (!initialResponse || !VerifyGameStreamResponse(*initialResponse, error))
        {
            if (error == "Timed out while reading Sunshine's response")
            {
                error = "Timed out waiting for the PIN in Sunshine's Web UI. Start pairing again, then enter the displayed Vanta PIN within two minutes";
            }
            else if (error.find("Out of order call to getservercert") != std::string::npos)
            {
                error = "Sunshine has a stale pairing request from an earlier timed-out attempt. Restart Sunshine once on the host, then start pairing again";
            }
            return fail(error, false);
        }
        if (XmlValue(initialResponse->body, "paired") != "1")
        {
            return fail("Sunshine rejected the first pairing step", false);
        }
        const auto decodedCertificate = HexDecode(XmlValue(initialResponse->body, "plaincert"));
        if (!decodedCertificate || decodedCertificate->empty())
        {
            return fail("Sunshine is already pairing with another client; wait for that attempt to finish", true);
        }
        const std::string serverCertificatePem(decodedCertificate->begin(), decodedCertificate->end());
        X509Ptr parsedServerCertificate(nullptr, X509_free);
        if (!LoadPemCertificate(serverCertificatePem, parsedServerCertificate, error))
        {
            return fail(error, true);
        }

        Bytes challenge;
        Bytes encryptedChallenge;
        if (!RandomBytes(16, challenge, error) || !TransformAesEcb(challenge, aesKey, true, encryptedChallenge, error))
        {
            return fail(error, true);
        }
        progress("Confirming the Sunshine PIN...");
        const auto challengeResponse = RequestHttp(
            info->address,
            kGameStreamHttpPort,
            "pair",
            identity.uniqueId,
            "devicename=vanta&updateState=1&clientchallenge=" + HexEncode(encryptedChallenge),
            error);
        if (!challengeResponse || !VerifyGameStreamResponse(*challengeResponse, error))
        {
            return fail(error, true);
        }
        if (XmlValue(challengeResponse->body, "paired") != "1")
        {
            return fail("Sunshine rejected the PIN challenge", true);
        }
        const auto encryptedChallengeResponse = HexDecode(XmlValue(challengeResponse->body, "challengeresponse"));
        Bytes decryptedChallengeResponse;
        if (!encryptedChallengeResponse || !TransformAesEcb(*encryptedChallengeResponse, aesKey, false, decryptedChallengeResponse, error) ||
            decryptedChallengeResponse.size() < hashLength + 16)
        {
            return fail(error.empty() ? "Sunshine sent an invalid PIN challenge response" : error, true);
        }

        Bytes clientSecret;
        Bytes clientCertificateSignature;
        if (!RandomBytes(16, clientSecret, error))
        {
            return fail(error, true);
        }
        X509Ptr clientCertificate(nullptr, X509_free);
        EvpPkeyPtr clientPrivateKey(nullptr, EVP_PKEY_free);
        if (!LoadPemIdentity(identity, clientCertificate, clientPrivateKey, error) ||
            !GetCertificateSignature(clientCertificate.get(), clientCertificateSignature, error))
        {
            return fail(error, true);
        }

        Bytes serverResponseHash(decryptedChallengeResponse.begin(), decryptedChallengeResponse.begin() + static_cast<std::ptrdiff_t>(hashLength));
        Bytes challengeResponseData(
            decryptedChallengeResponse.begin() + static_cast<std::ptrdiff_t>(hashLength),
            decryptedChallengeResponse.begin() + static_cast<std::ptrdiff_t>(hashLength + 16));
        challengeResponseData.insert(challengeResponseData.end(), clientCertificateSignature.begin(), clientCertificateSignature.end());
        challengeResponseData.insert(challengeResponseData.end(), clientSecret.begin(), clientSecret.end());
        Bytes paddedHash;
        if (!Digest(challengeResponseData, hashAlgorithm, paddedHash, error))
        {
            return fail(error, true);
        }
        paddedHash.resize(32, 0);
        Bytes encryptedChallengeHash;
        if (!TransformAesEcb(paddedHash, aesKey, true, encryptedChallengeHash, error))
        {
            return fail(error, true);
        }

        progress("Verifying Sunshine's host identity...");
        const auto serverResponse = RequestHttp(
            info->address,
            kGameStreamHttpPort,
            "pair",
            identity.uniqueId,
            "devicename=vanta&updateState=1&serverchallengeresp=" + HexEncode(encryptedChallengeHash),
            error);
        if (!serverResponse || !VerifyGameStreamResponse(*serverResponse, error))
        {
            return fail(error, true);
        }
        if (XmlValue(serverResponse->body, "paired") != "1")
        {
            return fail("Sunshine rejected the host verification step", true);
        }
        const auto pairingSecret = HexDecode(XmlValue(serverResponse->body, "pairingsecret"));
        if (!pairingSecret || pairingSecret->size() <= 16)
        {
            return fail("Sunshine sent an invalid pairing secret", true);
        }
        const Bytes serverSecret(pairingSecret->begin(), pairingSecret->begin() + 16);
        const Bytes serverSignature(pairingSecret->begin() + 16, pairingSecret->end());
        if (!VerifyServerSignature(serverSecret, serverSignature, serverCertificatePem, error))
        {
            return fail(error, true);
        }

        Bytes serverCertificateSignature;
        if (!GetPemCertificateSignature(serverCertificatePem, serverCertificateSignature, error))
        {
            return fail(error, true);
        }
        Bytes expectedResponse = challenge;
        expectedResponse.insert(expectedResponse.end(), serverCertificateSignature.begin(), serverCertificateSignature.end());
        expectedResponse.insert(expectedResponse.end(), serverSecret.begin(), serverSecret.end());
        Bytes expectedResponseHash;
        if (!Digest(expectedResponse, hashAlgorithm, expectedResponseHash, error) ||
            expectedResponseHash.size() != serverResponseHash.size() ||
            CRYPTO_memcmp(expectedResponseHash.data(), serverResponseHash.data(), expectedResponseHash.size()) != 0)
        {
            return fail("The Sunshine PIN was incorrect", true);
        }

        Bytes clientSignature;
        if (!SignWithIdentity(identity, clientSecret, clientSignature, error))
        {
            return fail(error, true);
        }
        Bytes clientPairingSecret = clientSecret;
        clientPairingSecret.insert(clientPairingSecret.end(), clientSignature.begin(), clientSignature.end());

        progress("Completing the local pairing exchange...");
        const auto clientSecretResponse = RequestHttp(
            info->address,
            kGameStreamHttpPort,
            "pair",
            identity.uniqueId,
            "devicename=vanta&updateState=1&clientpairingsecret=" + HexEncode(clientPairingSecret),
            error);
        if (!clientSecretResponse || !VerifyGameStreamResponse(*clientSecretResponse, error))
        {
            return fail(error, true);
        }
        if (XmlValue(clientSecretResponse->body, "paired") != "1")
        {
            return fail("Sunshine rejected the final local pairing step", true);
        }

        progress("Confirming Vanta's encrypted Sunshine connection...");
        const auto finalResponse = RequestHttps(
            info->address,
            info->httpsPort,
            "pair",
            identity,
            serverCertificatePem,
            "devicename=vanta&updateState=1&phrase=pairchallenge",
            error);
        if (!finalResponse || !VerifyGameStreamResponse(*finalResponse, error))
        {
            return fail(error, true);
        }
        if (XmlValue(finalResponse->body, "paired") != "1")
        {
            return fail("Sunshine rejected encrypted pairing confirmation", true);
        }

        PairedHost pairedHost{*info, serverCertificatePem};
        if (!SavePairedHost(implementation_->storageDirectory, pairedHost, error))
        {
            BestEffortUnpair(*info, identity);
            result.message = "Sunshine paired Vanta, but Vanta could not securely save the certificate pin; the pairing was removed";
            return result;
        }

        result.succeeded = true;
        result.message = "Vanta is paired with " + (info->name.empty() ? info->address : info->name) + ".";
        result.host = std::move(pairedHost);
        return result;
    }

    StreamLaunchResult PairingService::LaunchDesktop(
        const std::string& requestedHost,
        const StreamRequest& request)
    {
        StreamLaunchResult result;
        const std::string host = Trim(requestedHost);
        if (host.empty())
        {
            result.message = "Enter the LAN address of the paired Sunshine host";
            return result;
        }

        std::string error;
        if (!IsSaneStreamRequest(request, error))
        {
            result.message = error;
            return result;
        }

        std::lock_guard<std::mutex> lock(implementation_->operationMutex);
        // Starting a stream must be side-effect free until Vanta has found a
        // local certificate pin for this exact host. In particular, do not
        // create a new private client identity just because someone presses
        // Start before pairing; identity creation belongs to Pair().
        const auto paired = ReadPairedHost(implementation_->storageDirectory, host, error);
        if (!paired)
        {
            result.message = error;
            return result;
        }

        ClientIdentity identity;
        if (!LoadOrCreateIdentity(implementation_->storageDirectory, identity, error))
        {
            result.message = error;
            return result;
        }

        PairedHost refreshedHost = *paired;
        std::string serverInfo;
        if (!RefreshPairedHostInfo(host, identity, refreshedHost, serverInfo, error))
        {
            result.message = error;
            return result;
        }
        int hostFormats{};
        if(refreshedHost.info.codecSupport&SCM_H264)hostFormats|=VIDEO_FORMAT_H264;
        if(refreshedHost.info.codecSupport&SCM_HEVC)hostFormats|=VIDEO_FORMAT_H265;
        if(refreshedHost.info.codecSupport&SCM_AV1_MAIN8)hostFormats|=VIDEO_FORMAT_AV1_MAIN8;
        result.supportedVideoFormats=hostFormats&request.supportedVideoFormats;
        if (result.supportedVideoFormats==0)
        {
            result.message = "Sunshine and the selected receiver adapter have no common hardware codec for this selection";
            return result;
        }

        const auto appListResponse = RequestHttps(
            refreshedHost.info.address,
            refreshedHost.info.httpsPort,
            "applist",
            identity,
            refreshedHost.certificatePem,
            {},
            error);
        if (!appListResponse || !VerifyGameStreamResponse(*appListResponse, error))
        {
            result.message = error;
            return result;
        }
        const std::vector<SunshineApplication> applications = ParseApplications(appListResponse->body);
        const auto desktop = std::find_if(applications.begin(), applications.end(), [](const SunshineApplication& application)
        {
            return Lowercase(Trim(application.title)) == "desktop";
        });
        if (desktop == applications.end())
        {
            result.message = "This Sunshine host does not expose a Desktop application";
            return result;
        }

        const int activeApp = GetCurrentGame(serverInfo);
        if (activeApp != 0)
        {
            // Vanta cannot prove that a server-side Desktop session belongs
            // to this Vanta process. Never resume it: doing so could take
            // over a user's active Moonlight session with a different
            // per-session remote-input key.
            result.message = "Sunshine already has an active stream; stop that session before Vanta starts Desktop";
            return result;
        }

        const auto launchResponse = RequestHttps(
            refreshedHost.info.address,
            refreshedHost.info.httpsPort,
            "launch",
            identity,
            refreshedHost.certificatePem,
            LaunchArguments(desktop->id, request),
            error,
            kLaunchTimeoutMilliseconds);
        if (!launchResponse || !VerifyGameStreamResponse(*launchResponse, error))
        {
            result.message = error;
            return result;
        }
        const std::string sessionUrl = XmlValue(launchResponse->body, "sessionUrl0");
        if (sessionUrl.empty())
        {
            result.message = "Sunshine launched Desktop but did not return its RTSP session URL";
            return result;
        }

        // Server metadata can change after an upgrade. Update the protected
        // record only after the TLS certificate pin has matched successfully.
        std::string saveError;
        if (!SavePairedHost(implementation_->storageDirectory, refreshedHost, saveError))
        {
            result.message = "Sunshine launched Desktop but Vanta could not update its protected pairing record";
            return result;
        }

        result.succeeded = true;
        result.message = "Sunshine Desktop launched for Vanta.";
        result.host = std::move(refreshedHost);
        result.appId = desktop->id;
        result.appTitle = desktop->title;
        result.rtspSessionUrl = sessionUrl;
        return result;
    }

    std::optional<PairedHost> PairingService::LoadPairedHost(const std::string& host, std::string& error) const
    {
        return ReadPairedHost(implementation_->storageDirectory, host, error);
    }
}
