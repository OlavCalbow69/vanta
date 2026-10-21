#pragma once
#include <string>

namespace vanta
{
    // Background diagnostics may parse WinRT JSON without owning a UI apartment.
    struct InputApartment
    {
        InputApartment();
        ~InputApartment();
        InputApartment(const InputApartment&)=delete;
        InputApartment& operator=(const InputApartment&)=delete;
    private:
        bool owned_{};
    };
    std::string InputJsonString(const std::string& text);
}
