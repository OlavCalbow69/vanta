#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace makcu::protocol
{
    inline bool IsCommandEcho(std::string_view reply)
    {
        // A prior silent mutation may be echoed after a new query was queued.
        // V4 scalar query results are numbers; identity is km.MAKCU, not a call.
        return reply.starts_with("km.") && reply.ends_with(')') && reply.find('(')!=std::string_view::npos;
    }
    inline bool IsEchoOrPrompt(std::string_view reply, std::string_view command)
    {
        return reply.empty() || reply==command || reply==">>>" || reply==">>> " || IsCommandEcho(reply);
    }
    // One incremental parser for text, km.+mask streams, and V4 MAK_API
    // frames. Byte/chunk boundaries never carry protocol meaning.
    class ReplyParser
    {
    public:
        std::function<void(const std::string&)> onText;
        std::function<void(std::uint8_t)> onButtons;
        std::function<void(std::uint8_t,std::span<const std::uint8_t>)> onBinary;

        void feed(std::span<const std::uint8_t> bytes)
        {
            for (const auto byte:bytes) consume(byte);
        }

    private:
        std::vector<std::uint8_t> frame;
        std::size_t payloadSize{};
        std::string prefix;
        std::string line;
        int streamTrailer{};

        void text(std::uint8_t byte)
        {
            if (byte=='\n')
            {
                while (line.starts_with(">>>"))
                {
                    line.erase(0,3);
                    if (line.starts_with(' ')) line.erase(0,1);
                }
                if (!line.empty() && onText) onText(line);
                line.clear();
            }
            else if (byte!='\r' && byte>=32 && byte<127)
            {
                if (line.size()<4096) line.push_back(static_cast<char>(byte));
                else line.clear();
            }
        }

        void ascii(std::uint8_t byte)
        {
            if (streamTrailer)
            {
                if ((streamTrailer==1 && byte=='\r') || byte=='\n')
                { streamTrailer=byte=='\r'?2:0; return; }
                streamTrailer=0;
            }
            if (prefix=="km." && byte<32)
            {
                if (onButtons) onButtons(byte);
                prefix.clear(); streamTrailer=1; return;
            }
            if (prefix.empty() && byte=='k') { prefix="k"; return; }
            if (prefix=="k" && byte=='m') { prefix="km"; return; }
            if (prefix=="km" && byte=='.') { prefix="km."; return; }
            if (!prefix.empty())
            {
                for (const auto pending:prefix) text(static_cast<std::uint8_t>(pending));
                prefix.clear();
                if (byte=='k') { prefix="k"; return; }
            }
            text(byte);
        }

        void consume(std::uint8_t byte)
        {
            if (frame.empty())
            {
                if (byte==0xDE) frame.push_back(byte);
                else ascii(byte);
                return;
            }
            if (frame.size()==1 && byte!=0xAD)
            {
                frame.clear();
                if (byte==0xDE) frame.push_back(byte); else ascii(byte);
                return;
            }
            frame.push_back(byte);
            if (frame.size()==4)
            {
                payloadSize=static_cast<std::size_t>(frame[2]) |
                    (static_cast<std::size_t>(frame[3])<<8U);
                if (payloadSize>4096) frame.clear();
            }
            else if (frame.size()>=5 && frame.size()==payloadSize+5)
            {
                if (onBinary) onBinary(frame[4],std::span(frame).subspan(5));
                frame.clear();
            }
        }
    };

    struct ProbeResult
    {
        std::uint32_t baud{};
        std::uint32_t firmware{};
        std::string identity;
    };
    struct ProbeOperations
    {
        std::function<bool(std::uint32_t)> open;
        std::function<void()> close;
        std::function<std::string()> identity;
        std::function<std::uint32_t()> firmware;
        std::function<bool(bool)> switchToFast; // true selects V4 text baud
    };

    inline bool ProbeConnection(const ProbeOperations& link, const bool highSpeed, ProbeResult& result)
    {
        result={};
        for (const std::uint32_t baud: {4000000U,115200U})
        {
            if (!link.open(baud)) continue;
            const std::string identity=link.identity();
            if (!identity.starts_with("km.MAKCU")) { link.close(); continue; }
            result={baud,link.firmware(),identity};
            if (baud==115200 && highSpeed)
            {
                if (!link.switchToFast(result.firmware>=4073)) { link.close(); result={}; return false; }
                link.close();
                if (!link.open(4000000)) { result={}; return false; }
                const auto repeated=link.identity();
                const auto firmware=link.firmware();
                if (!repeated.starts_with("km.MAKCU") ||
                    (result.firmware!=0 && firmware!=result.firmware))
                { link.close(); result={}; return false; }
                result={4000000,firmware,repeated};
            }
            return true;
        }
        return false;
    }

    inline std::uint32_t FirmwareInteger(std::span<const std::uint8_t> bytes)
    {
        if (bytes.size()!=4) return 0;
        return static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1])<<8U) |
            (static_cast<std::uint32_t>(bytes[2])<<16U) |
            (static_cast<std::uint32_t>(bytes[3])<<24U);
    }
}
