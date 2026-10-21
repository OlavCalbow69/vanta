#include "input_backend_utils.hpp"
#include <Windows.h>
#include <objbase.h>
#include <winrt/base.h>

namespace vanta
{
    InputApartment::InputApartment()
    {
        const auto result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        owned_=SUCCEEDED(result);
        if(FAILED(result) && result!=RPC_E_CHANGED_MODE)winrt::throw_hresult(result);
    }
    InputApartment::~InputApartment(){if(owned_)CoUninitialize();}
    std::string InputJsonString(const std::string& text)
    {
        constexpr char hex[]="0123456789abcdef";
        std::string result="\"";
        for(unsigned char c:text)
        {
            if(c=='"' || c=='\\'){result+='\\';result+=char(c);}
            else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
            else result+=char(c);
        }
        return result+'"';
    }
}
