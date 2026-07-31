#pragma once

#define VANTA_VERSION_MAJOR 1
#define VANTA_VERSION_MINOR 1
#define VANTA_VERSION_PATCH 0
#define VANTA_VERSION_BUILD 0
#define VANTA_AUTOMATED_RELEASE_BUILD 0

#define VANTA_VERSION_FILE \
    VANTA_VERSION_MAJOR, VANTA_VERSION_MINOR, \
    VANTA_VERSION_PATCH, VANTA_VERSION_BUILD
#define VANTA_VERSION_STRING "1.1.0"
#define VANTA_VERSION_WSTRING L"1.1.0"
#define VANTA_VERSION_FILE_STRING "1.1.0.0"

#ifdef __cplusplus
#include <string_view>

namespace vanta
{
    inline constexpr std::string_view kVersion{
        VANTA_VERSION_STRING};
}
#endif
