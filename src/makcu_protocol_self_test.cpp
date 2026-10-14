#include "makcu_controller.hpp"
#include "host_protocol.hpp"
#include "makcu_c.h"
#include "logger.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace vanta
{
    bool RunMakcuCompatibilitySelfTest()
    {
        const auto fail=[](const char* message) { log::Error("MAKCU compatibility self-test: %s",message); return false; };
        const std::vector<std::uint8_t> wire{
            'k','m','.','M','A','K','C','U','\r','\n','>','>','>',' ',
            'k','m','.',0x10,'\r','\n',
            0xDE,0xAD,4,0,4,0xFE,0x0F,0,0,
            'k','m','.',0x0A,'\r','\n','k','m','.',0x0D,'\r','\n',
            'k','m','.',0,'\r','\n',
            'k','m','.','v','e','r','s','i','o','n','(',')','\r','\n'};
        for (std::size_t chunk=1;chunk<=wire.size();++chunk)
        {
            makcu::protocol::ReplyParser parser;
            std::vector<std::string> text;
            std::vector<std::uint8_t> buttons;
            std::uint32_t version=0;
            parser.onText=[&](const std::string& value) { text.push_back(value); };
            parser.onButtons=[&](std::uint8_t value) { buttons.push_back(value); };
            parser.onBinary=[&](std::uint8_t opcode,std::span<const std::uint8_t> data) {
                if (opcode==4) version=makcu::protocol::FirmwareInteger(data);
            };
            for (std::size_t offset=0;offset<wire.size();offset+=chunk)
                parser.feed(std::span(wire).subspan(offset,std::min(chunk,wire.size()-offset)));
            if (text!=std::vector<std::string>{"km.MAKCU","km.version()"} ||
                buttons!=std::vector<std::uint8_t>{0x10,0x0A,0x0D,0} || version!=4094)
                return fail("fragmented text/buttons/binary firmware were mixed");
        }
        if (!makcu::protocol::IsEchoOrPrompt("km.version()","km.version()") ||
            !makcu::protocol::IsEchoOrPrompt(">>> ","km.version()") ||
            makcu::protocol::IsEchoOrPrompt("km.MAKCU","km.version()"))
            return fail("echo/prompt confused with result");
        makcu::protocol::ReplyParser interleaved;
        std::string joined;
        interleaved.onText=[&](const std::string& value) { joined=value; };
        const std::array<std::uint8_t,10> interrupted{'1','2','k','m','.',16,'\r','\n','3','4'};
        interleaved.feed(interrupted);
        interleaved.feed(std::array<std::uint8_t,1>{'\n'});
        if (joined!="1234") return fail("button interrupt truncated a text result");
        std::uint32_t currentBaud=0,deviceBaud=115200;
        std::vector<std::uint32_t> opened;
        int closed=0,switched=0;
        bool wrongFirmware=false;
        makcu::protocol::ProbeOperations ops;
        ops.open=[&](std::uint32_t baud) { opened.push_back(baud); currentBaud=baud; return true; };
        ops.close=[&] { ++closed; currentBaud=0; };
        ops.identity=[&] { return currentBaud==deviceBaud ? "km.MAKCU" : "ERR"; };
        ops.firmware=[&] { return wrongFirmware ? 4095U : 4094U; };
        ops.switchToFast=[&](bool text) { if (!text) return false; ++switched; deviceBaud=4000000; return true; };
        makcu::protocol::ProbeResult result;
        if (!makcu::protocol::ProbeConnection(ops,true,result) ||
            opened!=std::vector<std::uint32_t>{4000000,115200,4000000} ||
            closed!=2 || switched!=1 || result.baud!=4000000 || result.firmware!=4094)
            return fail("fallback/reopen sequence");
        opened.clear(); closed=switched=0;
        if (!makcu::protocol::ProbeConnection(ops,true,result) ||
            opened!=std::vector<std::uint32_t>{4000000} || closed!=0 || switched!=0)
            return fail("fast baud was not probed first");
        opened.clear(); deviceBaud=115200;
        if (!makcu::protocol::ProbeConnection(ops,false,result) || result.baud!=115200 || switched!=0)
            return fail("standard baud option");
        deviceBaud=115200;
        ops.switchToFast=[&](bool) { wrongFirmware=true; deviceBaud=4000000; return true; };
        if (makcu::protocol::ProbeConnection(ops,true,result)) return fail("changed firmware after reopen accepted");
        ops.identity=[] { return "km.version()"; };
        if (makcu::protocol::ProbeConnection(ops,true,result)) return fail("echo accepted as identity");
        if (makcu::protocol::FirmwareInteger(std::array<std::uint8_t,1>{0xFF})!=0)
            return fail("malformed firmware");
        log::Info("MAKCU compatibility self-test passed: fragmented replies, mask control bytes, baud probing, reopen and firmware verification");
        return true;
    }

    bool RunMakcuConnectionProbe(const std::string& port)
    {
        makcu_device_t* device=makcu_device_create();
        if (!device) return false;
        const auto connected=makcu_connect(device,port.c_str());
        std::array<char,256> version{};
        const bool success=connected==MAKCU_SUCCESS &&
            makcu_get_version(device,version.data(),version.size())==MAKCU_SUCCESS;
        if (success) log::Info("MAKCU connection probe: firmware %s, port %s; no movement or clicks injected",version.data(),port.c_str());
        else log::Error("MAKCU connection probe failed: %s",makcu_error_string(connected));
        makcu_disconnect(device);
        makcu_device_destroy(device);
        return success;
    }
}
