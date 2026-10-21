#define IMGUI_DEFINE_MATH_OPERATORS

#include "config_manager.hpp"
#include "target_detection.hpp"
#include "stream_receiver_controller.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

#include "capture_controller.hpp"
#include "python_bot_controller.hpp"
#include "logger.hpp"
#include "makcu_controller.hpp"
#include "pretty_menu.hpp"
#include "testclick_controller.hpp"
#include "testmove_controller.hpp"
#include "update_controller.hpp"

#include "custom_widgets.hpp"
#include "font_defines.h"
#include "imgui.h"
#include "imgui_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <commdlg.h>
#include <shellapi.h>

namespace
{
    using winrt::Windows::Data::Json::JsonObject;
    using winrt::Windows::Data::Json::JsonValue;

    constexpr std::array<
        const wchar_t*,
        static_cast<std::size_t>(
            vanta::PaletteSlot::count)>
        kPaletteNames{{
            L"accent",
            L"secondary",
            L"window_background",
            L"widget_background",
            L"stroke",
            L"separator",
            L"animation_active",
            L"animation_default",
            L"main_background",
            L"child_background",
            L"child_stroke",
            L"checkbox_mark",
            L"page_active",
            L"page_background",
            L"page_hover_text",
            L"page_text",
            L"element_hovered",
            L"element_background",
            L"label_active",
            L"label_hovered",
            L"label_default",
            L"description_active",
            L"description_hovered",
            L"description_default",
            L"text_active",
            L"text_hovered",
            L"text_default"}};

    void PutNumber(
        JsonObject& object,
        const wchar_t* name,
        double value)
    {
        object.Insert(
            name,
            JsonValue::CreateNumberValue(value));
    }

    void PutBoolean(
        JsonObject& object,
        const wchar_t* name,
        bool value)
    {
        object.Insert(
            name,
            JsonValue::CreateBooleanValue(value));
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

    double Number(
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

    bool Boolean(
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

    std::string String(
        const JsonObject& object,
        const wchar_t* name,
        const std::string& fallback)
    {
        try
        {
            return object.HasKey(name)
                ? winrt::to_string(
                    object.GetNamedString(name))
                : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    JsonObject Child(
        const JsonObject& object,
        const wchar_t* name)
    {
        try
        {
            if (object.HasKey(name))
            {
                return object.GetNamedObject(name);
            }
        }
        catch (...)
        {
        }
        return JsonObject{};
    }

    vanta::MouseButtonBinding MouseButtonFromStoredValue(
        int value,
        vanta::MouseButtonBinding fallback)
    {
        const auto button =
            static_cast<vanta::MouseButtonBinding>(value);
        return vanta::IsMouseButtonBinding(button)
            ? button
            : fallback;
    }

    vanta::MouseButtonBinding MouseButtonFromLegacyVirtualKey(
        int value,
        vanta::MouseButtonBinding fallback)
    {
        // Profiles through schema 6 persisted Win32 virtual-key numbers.
        // Only the four mouse options are carried forward to MAKCU bindings.
        switch (value)
        {
        case 1:
            return vanta::MouseButtonBinding::left;
        case 2:
            return vanta::MouseButtonBinding::right;
        case 5:
            return vanta::MouseButtonBinding::x1;
        case 6:
            return vanta::MouseButtonBinding::x2;
        default:
            return fallback;
        }
    }

    JsonObject ColorToJson(
        const vanta::RgbaColor& rgbaColor)
    {
        JsonObject result;
        PutNumber(result, L"r", rgbaColor.red);
        PutNumber(result, L"g", rgbaColor.green);
        PutNumber(result, L"b", rgbaColor.blue);
        PutNumber(result, L"a", rgbaColor.alpha);
        return result;
    }

    vanta::RgbaColor ColorFromJson(
        const JsonObject& object,
        const vanta::RgbaColor& fallback)
    {
        vanta::RgbaColor result = fallback;
        result.red = static_cast<float>(
            Number(object, L"r", result.red));
        result.green = static_cast<float>(
            Number(object, L"g", result.green));
        result.blue = static_cast<float>(
            Number(object, L"b", result.blue));
        result.alpha = static_cast<float>(
            Number(object, L"a", result.alpha));
        return result;
    }

    JsonObject CaptureToJson(const vanta::CaptureConfig& config)
    {
        JsonObject result;
        PutString(result,L"source","sunshine");
        PutNumber(result,L"region",config.regionIndex);
        PutNumber(result,L"region_size",config.regionSize);
        PutBoolean(result,L"draw_outline",config.drawOutline);
        result.Insert(L"outline_color",ColorToJson(config.outlineColor));
        PutBoolean(result,L"apply_filter",config.applyFilter);
        PutBoolean(result,L"binary_mask",config.binaryMask);
        PutNumber(result,L"color_target",config.colorTargetIndex);
        return result;
    }

    vanta::CaptureConfig CaptureFromJson(
        const JsonObject& object,const vanta::CaptureConfig& defaults)
    {
        vanta::CaptureConfig result=defaults;
        // Legacy source/backend IDs and local window identities are ignored.
        // Retain ROI and preview tuning, but never reopen a local capture path.
        result.regionIndex=static_cast<int>(Number(object,L"region",result.regionIndex));
        result.regionSize=static_cast<int>(Number(object,L"region_size",result.regionSize));
        result.drawOutline=Boolean(object,L"draw_outline",result.drawOutline);
        result.outlineColor=ColorFromJson(Child(object,L"outline_color"),result.outlineColor);
        result.applyFilter=Boolean(object,L"apply_filter",result.applyFilter);
        result.binaryMask=Boolean(object,L"binary_mask",result.binaryMask);
        result.colorTargetIndex=static_cast<int>(Number(object,L"color_target",result.colorTargetIndex));
        return result;
    }

    JsonObject MouseOutputToJson(const vanta::MouseOutputConfig& config)
    {
        JsonObject result;
        PutString(result,L"backend",config.backend==vanta::InputBackend::medius?"medius":"makcu");
        PutString(result,L"makcu_port",config.makcuPort);
        PutBoolean(result,L"high_performance",config.highPerformanceMode);
        PutBoolean(result,L"auto_detect_and_connect",config.autoDetectAndConnect);
        PutBoolean(result,L"lock_movement_while_correcting",config.lockMovementWhileCorrecting);
        PutBoolean(result,L"lock_generated_click",config.lockGeneratedClick);
        PutBoolean(result,L"reserve_activation_side_buttons",config.reserveActivationSideButtons);
        return result;
    }

    vanta::MouseOutputConfig MouseOutputFromJson(
        const JsonObject& object,const vanta::MouseOutputConfig& defaults)
    {
        vanta::MouseOutputConfig result=defaults;
        result.backend=String(object,L"backend","makcu")=="medius"?vanta::InputBackend::medius:vanta::InputBackend::makcu;
        result.makcuPort=String(object,L"makcu_port",result.makcuPort);
        result.highPerformanceMode=Boolean(object,L"high_performance",result.highPerformanceMode);
        result.autoDetectAndConnect=Boolean(object,L"auto_detect_and_connect",result.autoDetectAndConnect);
        result.lockMovementWhileCorrecting=Boolean(object,L"lock_movement_while_correcting",result.lockMovementWhileCorrecting);
        result.lockGeneratedClick=Boolean(object,L"lock_generated_click",result.lockGeneratedClick);
        result.reserveActivationSideButtons=Boolean(object,L"reserve_activation_side_buttons",result.reserveActivationSideButtons);
        return result;
    }

    JsonObject UpdateToJson(
        const vanta::UpdateConfig& config)
    {
        JsonObject result;
        PutBoolean(
            result,
            L"automatic_checks",
            config.automaticChecks);
        PutBoolean(
            result,
            L"automatic_downloads",
            config.automaticDownloads);
        PutBoolean(
            result,
            L"silent_automatic_installation",
            config.silentAutomaticInstallation);
        return result;
    }

    vanta::UpdateConfig UpdateFromJson(
        const JsonObject& object,
        const vanta::UpdateConfig& defaults)
    {
        vanta::UpdateConfig result = defaults;
        result.automaticChecks = Boolean(
            object,
            L"automatic_checks",
            result.automaticChecks);
        result.automaticDownloads = Boolean(
            object,
            L"automatic_downloads",
            result.automaticDownloads);
        result.silentAutomaticInstallation = Boolean(
            object,
            L"silent_automatic_installation",
            result.silentAutomaticInstallation);
        return result;
    }

    JsonObject MenuToJson(
        const vanta::MenuConfig& config)
    {
        JsonObject result;
        PutBoolean(
            result,
            L"notifications",
            config.notifications);
        PutBoolean(
            result,
            L"compact",
            config.compactMode);
        PutNumber(
            result,
            L"active_page",
            config.activePage);
        PutBoolean(
            result,
            L"has_geometry",
            config.hasGeometry);
        PutNumber(
            result,
            L"x",
            config.positionX);
        PutNumber(
            result,
            L"y",
            config.positionY);
        PutNumber(
            result,
            L"width",
            config.width);
        PutNumber(
            result,
            L"height",
            config.height);
        JsonObject palette;
        for (std::size_t index = 0;
             index < kPaletteNames.size();
             ++index)
        {
            palette.Insert(
                kPaletteNames[index],
                ColorToJson(
                    config.palette[index]));
        }
        result.Insert(L"palette", palette);
        return result;
    }

    vanta::MenuConfig MenuFromJson(
        const JsonObject& object,
        const vanta::MenuConfig& defaults)
    {
        vanta::MenuConfig result = defaults;
        result.notifications =
            Boolean(
                object,
                L"notifications",
                result.notifications);
        result.compactMode =
            Boolean(
                object,
                L"compact",
                result.compactMode);
        result.activePage =
            static_cast<int>(
                Number(
                    object,
                    L"active_page",
                    result.activePage));
        result.hasGeometry =
            Boolean(
                object,
                L"has_geometry",
                result.hasGeometry);
        result.positionX =
            static_cast<float>(
                Number(
                    object,
                    L"x",
                    result.positionX));
        result.positionY =
            static_cast<float>(
                Number(
                    object,
                    L"y",
                    result.positionY));
        result.width =
            static_cast<float>(
                Number(
                    object,
                    L"width",
                    result.width));
        result.height =
            static_cast<float>(
                Number(
                    object,
                    L"height",
                    result.height));
        const JsonObject palette =
            Child(object, L"palette");
        for (std::size_t index = 0;
             index < kPaletteNames.size();
             ++index)
        {
            result.palette[index] =
                ColorFromJson(
                    Child(
                        palette,
                        kPaletteNames[index]),
                    result.palette[index]);
        }
        return result;
    }

    JsonObject TestClickToJson(
        const vanta::TestClickConfig& config)
    {
        JsonObject result;
        PutBoolean(result, L"enabled", config.enabled);
        PutNumber(result, L"color_target", config.hsvRangeIndex);
        PutNumber(
            result,
            L"hold_button",
            static_cast<int>(config.holdButton));
        PutNumber(result, L"capture_radius", config.roiOffset);
        PutNumber(result, L"ray_length", config.rayLength);
        PutNumber(result, L"ray_half_width", config.rayHalfWidth);
        PutNumber(result, L"hit_mode", config.hitMode);
        PutBoolean(result, L"burst_limiter", config.burstLimiter);
        PutNumber(result, L"shot_delay", config.burstDelay);
        PutNumber(result, L"burst_pause", config.burstPause);
        PutNumber(result, L"burst_size", config.burstSize);
        return result;
    }

    vanta::TestClickConfig TestClickFromJson(
        const JsonObject& object,
        const vanta::TestClickConfig& defaults,
        int schema)
    {
        vanta::TestClickConfig result = defaults;
        result.enabled =
            Boolean(object, L"enabled", result.enabled);
        result.hsvRangeIndex =
            static_cast<int>(
                Number(object, L"color_target", result.hsvRangeIndex));
        if (schema >= 7)
        {
            result.holdButton = MouseButtonFromStoredValue(
                static_cast<int>(
                    Number(
                        object,
                        L"hold_button",
                        static_cast<int>(
                            result.holdButton))),
                result.holdButton);
        }
        else
        {
            result.holdButton = MouseButtonFromLegacyVirtualKey(
                static_cast<int>(
                    Number(
                        object,
                        L"hold_key",
                        0)),
                result.holdButton);
        }
        result.roiOffset =
            static_cast<int>(
                Number(object, L"capture_radius", result.roiOffset));
        result.rayLength =
            static_cast<int>(
                Number(object, L"ray_length", result.rayLength));
        result.rayHalfWidth =
            static_cast<int>(
                Number(object, L"ray_half_width", result.rayHalfWidth));
        result.hitMode =
            static_cast<int>(
                Number(object, L"hit_mode", result.hitMode));
        result.burstLimiter =
            Boolean(object, L"burst_limiter", result.burstLimiter);
        result.burstDelay =
            static_cast<float>(
                Number(object, L"shot_delay", result.burstDelay));
        result.burstPause =
            static_cast<float>(
                Number(object, L"burst_pause", result.burstPause));
        result.burstSize =
            static_cast<int>(
                Number(object, L"burst_size", result.burstSize));
        return result;
    }

    using Detector = vanta::TargetDetectionConfig;
    constexpr std::pair<const wchar_t*, bool Detector::*> detectorBooleans[]{
        {L"enabled",&Detector::enabled},{L"area_filter",&Detector::areaFilter},
        {L"width_filter",&Detector::widthFilter},{L"height_filter",&Detector::heightFilter},
        {L"aspect_filter",&Detector::aspectFilter},{L"solidity_filter",&Detector::solidityFilter},
        {L"flat_filter",&Detector::flatFilter},{L"orb_filter",&Detector::orbFilter},
        {L"closing_enabled",&Detector::closingEnabled}};
    constexpr std::pair<const wchar_t*, double Detector::*> detectorNumbers[]{
        {L"minimum_area",&Detector::minimumArea},{L"maximum_area",&Detector::maximumArea},
        {L"minimum_width",&Detector::minimumWidth},{L"maximum_width",&Detector::maximumWidth},
        {L"minimum_height",&Detector::minimumHeight},{L"maximum_height",&Detector::maximumHeight},
        {L"minimum_aspect",&Detector::minimumAspect},{L"maximum_aspect",&Detector::maximumAspect},
        {L"minimum_solidity",&Detector::minimumSolidity},{L"maximum_solidity",&Detector::maximumSolidity},
        {L"flat_aspect_maximum",&Detector::flatAspectMaximum},{L"flat_minimum_width",&Detector::flatMinimumWidth},
        {L"orb_aspect_minimum",&Detector::orbAspectMinimum},{L"orb_aspect_maximum",&Detector::orbAspectMaximum},
        {L"orb_minimum_width",&Detector::orbMinimumWidth},{L"orb_fill_minimum",&Detector::orbFillMinimum},
        {L"orb_solidity_minimum",&Detector::orbSolidityMinimum},{L"merge_proximity",&Detector::mergeProximity},
        {L"offset_x",&Detector::offsetX},{L"offset_y",&Detector::offsetY}};
    constexpr std::pair<const wchar_t*, int Detector::*> detectorIntegers[]{
        {L"reference_width",&Detector::referenceWidth},{L"reference_height",&Detector::referenceHeight},
        {L"closing_kernel",&Detector::closingKernel},{L"dilation_kernel",&Detector::dilationKernel},
        {L"top_rows",&Detector::topRows}};

    JsonObject DetectorToJson(const Detector& c)
    {
        JsonObject object;
        for (const auto& [key,member]:detectorBooleans) PutBoolean(object,key,c.*member);
        for (const auto& [key,member]:detectorNumbers) PutNumber(object,key,c.*member);
        for (const auto& [key,member]:detectorIntegers) PutNumber(object,key,c.*member);
        PutNumber(object,L"aim_point_mode",static_cast<int>(c.aimPointMode));
        return object;
    }

    Detector DetectorFromJson(const JsonObject& object)
    {
        Detector c;
        for (const auto& [key,member]:detectorBooleans) c.*member=Boolean(object,key,c.*member);
        for (const auto& [key,member]:detectorNumbers) c.*member=Number(object,key,c.*member);
        for (const auto& [key,member]:detectorIntegers)
            c.*member=static_cast<int>(std::clamp(Number(object,key,c.*member),-100000.0,100000.0));
        c.aimPointMode=Number(object,L"aim_point_mode",1)==0
            ? vanta::AimPointMode::boundingBox : vanta::AimPointMode::maskTop;
        return vanta::SanitizeDetectionConfig(c);
    }

    JsonObject SunshineToJson(const vanta::SunshineStreamConfig& c)
    {
        JsonObject object;
        PutNumber(object,L"width",c.width); PutNumber(object,L"height",c.height);
        PutNumber(object,L"fps",c.fps); PutNumber(object,L"bitrate_kbps",c.bitrateKbps);
        PutBoolean(object,L"request_display_mode",c.requestDisplayMode);
        constexpr const char* codecs[]{"auto","h264","hevc","av1"};
        PutString(object,L"codec",codecs[std::clamp(int(c.codec),0,3)]);
        PutString(object,L"adapter",c.adapter);
        return object;
    }

    vanta::SunshineStreamConfig SunshineFromJson(const JsonObject& object,bool legacy)
    {
        vanta::SunshineStreamConfig c;
        if(legacy){c.fps=120;c.codec=vanta::CodecPreference::H264;}
        c.width=static_cast<int>(std::clamp(Number(object,L"width",c.width),64.0,16384.0));
        c.height=static_cast<int>(std::clamp(Number(object,L"height",c.height),64.0,16384.0));
        c.fps=static_cast<int>(std::clamp(Number(object,L"fps",c.fps),1.0,1000.0));
        c.bitrateKbps=static_cast<int>(std::clamp(Number(object,L"bitrate_kbps",c.bitrateKbps),1000.0,500000.0));
        c.requestDisplayMode=Boolean(object,L"request_display_mode",c.requestDisplayMode);
        if(!legacy)
        {
            const auto codec=String(object,L"codec","auto");
            if(codec=="auto")c.codec=vanta::CodecPreference::Auto;
            else if(codec=="h264")c.codec=vanta::CodecPreference::H264;
            else if(codec=="hevc")c.codec=vanta::CodecPreference::HEVC;
            else if(codec=="av1")c.codec=vanta::CodecPreference::AV1;
            else throw std::runtime_error("Unsupported stream codec preference");
            c.adapter=String(object,L"adapter","auto");
        }
        return c;
    }

    JsonObject TestMoveToJson(
        const vanta::TestMoveConfig& config)
    {
        JsonObject result;
        result.Insert(L"detector",DetectorToJson(config.detector));
        PutBoolean(result, L"enabled", config.enabled);
        PutNumber(result, L"color_target", config.hsvRangeIndex);
        PutNumber(
            result,
            L"aim_button",
            static_cast<int>(config.aimButton));
        PutNumber(result, L"kill_fov", config.killFov);
        PutNumber(
            result,
            L"target_height_percent",
            config.targetHeightPercent);
        PutBoolean(
            result,
            L"draw_fov_outline",
            config.drawFovOutline);
        result.Insert(
            L"fov_color",
            ColorToJson(config.fovColor));
        PutNumber(
            result,
            L"movement_method",
            static_cast<int>(config.movementMethod));
        PutNumber(result, L"speed", config.speed);
        PutNumber(result, L"smoothing", config.smooth);
        PutNumber(result, L"deadzone", config.deadzone);
        PutNumber(
            result,
            L"merge_proximity",
            config.mergeProximity);
        PutNumber(
            result,
            L"wind_gravity",
            config.windGravity);
        PutNumber(
            result,
            L"wind_strength",
            config.windStrength);
        PutNumber(
            result,
            L"wind_maximum_step",
            config.windMaximumStep);
        PutNumber(
            result,
            L"wind_slowdown_radius",
            config.windSlowdownRadius);
        PutNumber(
            result,
            L"axis_mode",
            static_cast<int>(config.axisMode));
        PutNumber(
            result,
            L"axis_horizontal_multiplier",
            config.axisHorizontalMultiplier);
        PutNumber(
            result,
            L"axis_vertical_multiplier",
            config.axisVerticalMultiplier);
        PutNumber(
            result,
            L"axis_smoothing",
            config.axisSmoothing);
        PutNumber(
            result,
            L"hybrid_vertical_time_ms",
            config.hybridVerticalTimeMilliseconds);
        PutBoolean(
            result,
            L"anti_below_objects",
            config.antiBelowObjects);
        PutBoolean(
            result,
            L"short_stop_enabled",
            config.shortStopEnabled);
        PutNumber(
            result,
            L"short_stop_mode",
            static_cast<int>(config.shortStopMode));
        PutNumber(
            result,
            L"short_stop_chance_percent",
            config.shortStopChancePercent);
        PutNumber(
            result,
            L"short_stop_minimum_pause_ms",
            config.shortStopMinimumPauseMilliseconds);
        PutNumber(
            result,
            L"short_stop_maximum_pause_ms",
            config.shortStopMaximumPauseMilliseconds);
        PutNumber(
            result,
            L"short_stop_slow_multiplier_minimum",
            config.shortStopSlowMultiplierMinimum);
        PutNumber(
            result,
            L"short_stop_slow_multiplier_maximum",
            config.shortStopSlowMultiplierMaximum);
        PutNumber(
            result,
            L"new_target_delay_minimum_ms",
            config.newTargetDelayMinimumMilliseconds);
        PutNumber(
            result,
            L"new_target_delay_maximum_ms",
            config.newTargetDelayMaximumMilliseconds);
        return result;
    }

    vanta::TestMoveConfig TestMoveFromJson(
        const JsonObject& object,
        const vanta::TestMoveConfig& defaults,
        int schema)
    {
        vanta::TestMoveConfig result = defaults;
        if (schema >= 8) result.detector=DetectorFromJson(Child(object,L"detector"));
        else
        {
            result.detector.enabled=false;
            result.detector.aimPointMode=vanta::AimPointMode::boundingBox;
            result.detector.closingEnabled=false;
            result.detector.offsetX=result.detector.offsetY=0.0;
        }
        result.enabled =
            Boolean(object, L"enabled", result.enabled);
        result.hsvRangeIndex =
            static_cast<int>(
                Number(object, L"color_target", result.hsvRangeIndex));
        if (schema >= 7)
        {
            result.aimButton = MouseButtonFromStoredValue(
                static_cast<int>(
                    Number(
                        object,
                        L"aim_button",
                        static_cast<int>(
                            result.aimButton))),
                result.aimButton);
        }
        else
        {
            result.aimButton = MouseButtonFromLegacyVirtualKey(
                static_cast<int>(
                    Number(
                        object,
                        L"aim_key",
                        0)),
                result.aimButton);
        }
        result.killFov =
            static_cast<int>(
                Number(object, L"kill_fov", result.killFov));
        result.targetHeightPercent =
            static_cast<int>(
                Number(
                    object,
                    L"target_height_percent",
                    result.targetHeightPercent));
        result.drawFovOutline =
            Boolean(
                object,
                L"draw_fov_outline",
                result.drawFovOutline);
        result.fovColor =
            ColorFromJson(
                Child(object, L"fov_color"),
                result.fovColor);
        result.movementMethod =
            static_cast<vanta::MovementMethod>(
                static_cast<int>(
                    Number(
                        object,
                        L"movement_method",
                        static_cast<int>(
                            result.movementMethod))));
        result.speed =
            static_cast<float>(
                Number(object, L"speed", result.speed));
        result.smooth =
            static_cast<float>(
                Number(object, L"smoothing", result.smooth));
        result.deadzone =
            static_cast<int>(
                Number(object, L"deadzone", result.deadzone));
        result.mergeProximity =
            static_cast<int>(
                Number(
                    object,
                    L"merge_proximity",
                    result.mergeProximity));
        result.windGravity =
            static_cast<float>(
                Number(
                    object,
                    L"wind_gravity",
                    result.windGravity));
        result.windStrength =
            static_cast<float>(
                Number(
                    object,
                    L"wind_strength",
                    result.windStrength));
        result.windMaximumStep =
            static_cast<float>(
                Number(
                    object,
                    L"wind_maximum_step",
                    result.windMaximumStep));
        result.windSlowdownRadius =
            static_cast<float>(
                Number(
                    object,
                    L"wind_slowdown_radius",
                    result.windSlowdownRadius));
        result.axisMode =
            static_cast<vanta::AxisMovementMode>(
                static_cast<int>(
                    Number(
                        object,
                        L"axis_mode",
                        static_cast<int>(
                            result.axisMode))));
        result.axisHorizontalMultiplier =
            static_cast<float>(
                Number(
                    object,
                    L"axis_horizontal_multiplier",
                    result.axisHorizontalMultiplier));
        result.axisVerticalMultiplier =
            static_cast<float>(
                Number(
                    object,
                    L"axis_vertical_multiplier",
                    result.axisVerticalMultiplier));
        result.axisSmoothing =
            static_cast<float>(
                Number(
                    object,
                    L"axis_smoothing",
                    result.axisSmoothing));
        result.hybridVerticalTimeMilliseconds =
            static_cast<int>(
                Number(
                    object,
                    L"hybrid_vertical_time_ms",
                    result.hybridVerticalTimeMilliseconds));
        result.antiBelowObjects =
            Boolean(
                object,
                L"anti_below_objects",
                result.antiBelowObjects);
        result.shortStopEnabled =
            Boolean(
                object,
                L"short_stop_enabled",
                result.shortStopEnabled);
        result.shortStopMode =
            static_cast<vanta::ShortStopMode>(
                static_cast<int>(
                    Number(
                        object,
                        L"short_stop_mode",
                        static_cast<int>(
                            result.shortStopMode))));
        result.shortStopChancePercent =
            static_cast<int>(
                Number(
                    object,
                    L"short_stop_chance_percent",
                    result.shortStopChancePercent));
        result.shortStopMinimumPauseMilliseconds =
            static_cast<int>(
                Number(
                    object,
                    L"short_stop_minimum_pause_ms",
                    result.shortStopMinimumPauseMilliseconds));
        result.shortStopMaximumPauseMilliseconds =
            static_cast<int>(
                Number(
                    object,
                    L"short_stop_maximum_pause_ms",
                    result.shortStopMaximumPauseMilliseconds));
        result.shortStopSlowMultiplierMinimum =
            static_cast<float>(
                Number(
                    object,
                    L"short_stop_slow_multiplier_minimum",
                    result.shortStopSlowMultiplierMinimum));
        result.shortStopSlowMultiplierMaximum =
            static_cast<float>(
                Number(
                    object,
                    L"short_stop_slow_multiplier_maximum",
                    result.shortStopSlowMultiplierMaximum));
        result.newTargetDelayMinimumMilliseconds =
            static_cast<int>(
                Number(
                    object,
                    L"new_target_delay_minimum_ms",
                    result.newTargetDelayMinimumMilliseconds));
        result.newTargetDelayMaximumMilliseconds =
            static_cast<int>(
                Number(
                    object,
                    L"new_target_delay_maximum_ms",
                    result.newTargetDelayMaximumMilliseconds));
        return result;
    }

    std::string SerializeLocal(
        const vanta::LocalConfig& config)
    {
        JsonObject root;
        PutString(
            root,
            L"kind",
            "vanta-local-config");
        PutNumber(
            root,
            L"schema_version",
            vanta::LocalConfig::schemaVersion);
        root.Insert(
            L"capture",
            CaptureToJson(config.capture));
        root.Insert(L"sunshine",SunshineToJson(config.sunshine));
        root.Insert(
            L"mouse_output",
            MouseOutputToJson(config.mouseOutput));
        root.Insert(
            L"updates",
            UpdateToJson(config.updates));
        if (!config.pythonConfigJson.empty())
            root.Insert(L"python",JsonObject::Parse(winrt::to_hstring(config.pythonConfigJson)));
        root.Insert(
            L"menu",
            MenuToJson(config.menu));
        return
            winrt::to_string(root.Stringify()) +
            "\n";
    }

    bool DeserializeLocal(
        const std::string& text,
        const vanta::LocalConfig& defaults,
        vanta::LocalConfig& result,
        std::string& error)
    {
        try
        {
            const JsonObject root =
                JsonObject::Parse(
                    winrt::to_hstring(text));
            if (String(root, L"kind", {}) !=
                "vanta-local-config")
            {
                error = "not a Vanta local configuration";
                return false;
            }
            const int schema =
                static_cast<int>(
                    Number(
                        root,
                        L"schema_version",
                        0));
            if (schema < 1 ||
                schema >
                    vanta::LocalConfig::schemaVersion)
            {
                error =
                    "unsupported local configuration schema " +
                    std::to_string(schema);
                return false;
            }
            result = defaults;
            if(schema<10){result.sunshine.fps=120;result.sunshine.codec=vanta::CodecPreference::H264;}
            if (schema >= 7) result.sunshine=SunshineFromJson(Child(root,L"sunshine"),schema<10);
            result.capture =
                CaptureFromJson(
                    Child(root, L"capture"),
                    defaults.capture);
            result.mouseOutput =
                MouseOutputFromJson(
                    Child(root, L"mouse_output"),
                    defaults.mouseOutput);
            result.updates =
                UpdateFromJson(
                    Child(root, L"updates"),
                    defaults.updates);
            if (schema>=9 && root.HasKey(L"python"))
            {
                result.pythonConfigJson=winrt::to_string(Child(root,L"python").Stringify());
                vanta::PythonBotConfig python;
                if (!vanta::ParsePythonBotConfig(result.pythonConfigJson,python,error)) return false;
            }
            result.menu =
                MenuFromJson(
                    Child(root, L"menu"),
                    defaults.menu);
            return true;
        }
        catch (const winrt::hresult_error& exception)
        {
            error =
                winrt::to_string(
                    exception.message());
            return false;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    std::string SerializeProfile(
        const vanta::SharedProfile& profile)
    {
        JsonObject root;
        PutString(
            root,
            L"kind",
            "vanta-test-profile");
        PutNumber(
            root,
            L"schema_version",
            vanta::SharedProfile::schemaVersion);
        PutString(root, L"name", profile.name);
        root.Insert(
            L"test_click",
            TestClickToJson(profile.testClick));
        root.Insert(
            L"test_move",
            TestMoveToJson(profile.testMove));
        if (!profile.pythonConfigJson.empty())
            root.Insert(L"python",JsonObject::Parse(winrt::to_hstring(profile.pythonConfigJson)));
        return
            winrt::to_string(root.Stringify()) +
            "\n";
    }

    bool DeserializeProfile(
        const std::string& text,
        vanta::SharedProfile& result,
        std::string& error)
    {
        try
        {
            const JsonObject root =
                JsonObject::Parse(
                    winrt::to_hstring(text));
            if (String(root, L"kind", {}) !=
                "vanta-test-profile")
            {
                error = "not a Vanta Test profile";
                return false;
            }
            const int schema =
                static_cast<int>(
                    Number(
                        root,
                        L"schema_version",
                        0));
            if (schema < 1 ||
                schema >
                    vanta::SharedProfile::schemaVersion)
            {
                error =
                    "unsupported Test profile schema " +
                    std::to_string(schema);
                return false;
            }
            result.name =
                String(root, L"name", "Imported");
            result.testClick =
                TestClickFromJson(
                    Child(root, L"test_click"),
                    result.testClick,
                    schema);
            result.testMove =
                TestMoveFromJson(
                    Child(root, L"test_move"),
                    result.testMove,
                    schema);
            result.pythonConfigJson.clear();
            if (schema>=9 && root.HasKey(L"python"))
            {
                result.pythonConfigJson=winrt::to_string(Child(root,L"python").Stringify());
                vanta::PythonBotConfig python;
                if (!vanta::ParsePythonBotConfig(result.pythonConfigJson,python,error)) return false;
            }
            return true;
        }
        catch (const winrt::hresult_error& exception)
        {
            error =
                winrt::to_string(
                    exception.message());
            return false;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    bool ReadText(
        const std::filesystem::path& path,
        std::string& text)
    {
        std::ifstream input(
            path,
            std::ios::binary);
        if (!input)
        {
            return false;
        }
        text.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        return input.good() || input.eof();
    }

    bool BackupBeforeSchemaUpgrade(const std::filesystem::path& path, const int nextSchema)
    {
        std::error_code error;
        if (!std::filesystem::exists(path,error)) return !error;
        std::string text;
        if (!ReadText(path,text)) return false;
        try
        {
            const auto object=JsonObject::Parse(winrt::to_hstring(text));
            const int schema=static_cast<int>(Number(object,L"schema_version",0));
            if (schema<=0 || schema>=nextSchema) return true;
            auto backup=path;
            backup += L".schema-"+std::to_wstring(schema)+L".bak";
            std::filesystem::copy_file(path,backup,std::filesystem::copy_options::skip_existing,error);
            return !error;
        }
        catch (...) { return false; }
    }

    bool AtomicWrite(
        const std::filesystem::path& path,
        const std::string& text,
        bool& changed)
    {
        changed = false;
        std::string existing;
        if (ReadText(path, existing) &&
            existing == text)
        {
            return true;
        }

        std::error_code error;
        std::filesystem::create_directories(
            path.parent_path(),
            error);
        if (error)
        {
            return false;
        }

        std::filesystem::path temporary = path;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary |
                    std::ios::trunc);
            if (!output)
            {
                return false;
            }
            output.write(
                text.data(),
                static_cast<std::streamsize>(
                    text.size()));
            output.flush();
            if (!output)
            {
                return false;
            }
        }

        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporary.c_str());
            return false;
        }
        changed = true;
        return true;
    }

    std::string SanitizeProfileName(
        const std::string& name)
    {
        std::string result;
        result.reserve(name.size());
        for (const unsigned char value : name)
        {
            result.push_back(
                std::isalnum(value) ||
                        value == '-' ||
                        value == '_' ||
                        value == ' '
                    ? static_cast<char>(value)
                    : '_');
        }
        while (!result.empty() &&
               (result.front() == ' ' ||
                result.front() == '.'))
        {
            result.erase(result.begin());
        }
        while (!result.empty() &&
               (result.back() == ' ' ||
                result.back() == '.'))
        {
            result.pop_back();
        }
        if (result.empty())
        {
            result = "Profile";
        }
        if (result.size() > 80)
        {
            result.resize(80);
        }
        return result;
    }

    std::filesystem::path Utf8Path(
        const std::string& text)
    {
        return std::filesystem::path(
            winrt::to_hstring(text).c_str());
    }

    std::string PathStemUtf8(
        const std::filesystem::path& path)
    {
        return winrt::to_string(
            winrt::hstring(
                path.stem().wstring()));
    }

    std::filesystem::path ChooseOpenProfile(
        HWND owner)
    {
        std::array<wchar_t, 32768> file{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter =
            L"Vanta Test profile (*.json)\0*.json\0"
            L"JSON files (*.json)\0*.json\0"
            L"All files (*.*)\0*.*\0";
        dialog.lpstrFile = file.data();
        dialog.nMaxFile =
            static_cast<DWORD>(file.size());
        dialog.Flags =
            OFN_FILEMUSTEXIST |
            OFN_PATHMUSTEXIST |
            OFN_NOCHANGEDIR;
        return GetOpenFileNameW(&dialog)
            ? std::filesystem::path(file.data())
            : std::filesystem::path{};
    }

    std::filesystem::path ChooseSaveProfile(
        HWND owner,
        const std::string& profileName)
    {
        std::array<wchar_t, 32768> file{};
        const std::wstring suggested =
            Utf8Path(profileName + ".json").
                filename().wstring();
        std::copy_n(
            suggested.c_str(),
            std::min(
                suggested.size(),
                file.size() - 1),
            file.data());

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter =
            L"Vanta Test profile (*.json)\0*.json\0"
            L"JSON files (*.json)\0*.json\0";
        dialog.lpstrFile = file.data();
        dialog.nMaxFile =
            static_cast<DWORD>(file.size());
        dialog.lpstrDefExt = L"json";
        dialog.Flags =
            OFN_OVERWRITEPROMPT |
            OFN_PATHMUSTEXIST |
            OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&dialog)
            ? std::filesystem::path(file.data())
            : std::filesystem::path{};
    }
}

namespace vanta
{
    bool RunConfigurationSelfTest()
    {
        bool apartmentInitialized = false;
        try
        {
            winrt::init_apartment(
                winrt::apartment_type::multi_threaded);
            apartmentInitialized = true;
        }
        catch (const winrt::hresult_error& error)
        {
            if (error.code() != RPC_E_CHANGED_MODE)
            {
                vanta::log::Error(
                    "configuration self-test could not initialize Windows JSON");
                return false;
            }
        }

        const std::filesystem::path temporaryRoot =
            std::filesystem::temp_directory_path().
                lexically_normal();
        const std::filesystem::path testDirectory =
            (temporaryRoot /
                (L"VantaConfigSelfTest-" +
                 std::to_wstring(GetCurrentProcessId()) +
                 L"-" +
                 std::to_wstring(GetTickCount64()))).
                lexically_normal();
        const auto finish = [&](bool succeeded)
        {
            std::error_code canonicalError;
            const std::filesystem::path cleanupParent =
                std::filesystem::weakly_canonical(
                    testDirectory.parent_path(),
                    canonicalError);
            const std::wstring cleanupName =
                testDirectory.filename().wstring();
            if (!canonicalError &&
                cleanupParent ==
                    std::filesystem::weakly_canonical(
                        temporaryRoot,
                        canonicalError) &&
                !canonicalError &&
                cleanupName.starts_with(
                    L"VantaConfigSelfTest-"))
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    testDirectory,
                    cleanupError);
                if (cleanupError)
                {
                    vanta::log::Warning(
                        "configuration self-test cleanup failed: %s",
                        cleanupError.message().c_str());
                }
            }
            if (apartmentInitialized)
            {
                winrt::uninit_apartment();
            }
            vanta::log::Info(
                "configuration self-test %s",
                succeeded ? "passed" : "failed");
            return succeeded;
        };

        std::error_code directoryError;
        std::filesystem::create_directories(
            testDirectory,
            directoryError);
        if (directoryError)
        {
            return finish(false);
        }

        LocalConfig local;
        local.capture.regionIndex = 1;
        local.capture.regionSize = 512;
        local.capture.colorTargetIndex = 1;
        local.mouseOutput.makcuPort = "COM42";
        local.mouseOutput.autoDetectAndConnect = false;
        local.updates.automaticChecks = false;
        local.updates.automaticDownloads = false;
        local.updates.silentAutomaticInstallation = false;
        local.pythonConfigJson=DefaultPythonBotJson();
        local.menu.notifications = false;
        local.menu.compactMode = true;
        local.menu.hasGeometry = true;
        local.menu.positionX = 25.0F;
        local.menu.positionY = 50.0F;
        local.menu.width = 920.0F;
        local.menu.height = 640.0F;
        local.menu.palette[
            static_cast<std::size_t>(
                PaletteSlot::accent)] =
            {0.2F, 0.4F, 0.8F, 0.75F};

        const std::string serializedLocal =
            SerializeLocal(local);
        const auto localPath =
            testDirectory / L"local.json";
        bool changed = false;
        if (!AtomicWrite(
                localPath,
                serializedLocal,
                changed) ||
            !changed)
        {
            return finish(false);
        }
        changed = true;
        if (!AtomicWrite(
                localPath,
                serializedLocal,
                changed) ||
            changed)
        {
            return finish(false);
        }

        std::string localText;
        std::string parseError;
        LocalConfig decodedLocal;
        if (!ReadText(localPath, localText) ||
            !DeserializeLocal(
                localText,
                LocalConfig{},
                decodedLocal,
                parseError) ||
            SerializeLocal(decodedLocal) !=
                serializedLocal)
        {
            return finish(false);
        }

        SharedProfile profile;
        profile.name = "Self Test";
        profile.pythonConfigJson=DefaultPythonBotJson();
        profile.testClick.enabled = true;
        profile.testClick.holdButton =
            MouseButtonBinding::x1;
        profile.testClick.rayLength = 73;
        profile.testClick.hitMode = 1;
        profile.testMove.aimButton =
            MouseButtonBinding::left;
        profile.testMove.enabled = true;
        profile.testMove.killFov = 155;
        profile.testMove.targetHeightPercent = 42;
        profile.testMove.movementMethod =
            MovementMethod::axisControl;
        profile.testMove.axisMode =
            AxisMovementMode::hybrid;
        profile.testMove.axisHorizontalMultiplier =
            1.4F;
        profile.testMove.axisVerticalMultiplier =
            0.8F;
        profile.testMove.axisSmoothing = 0.75F;
        profile.testMove.hybridVerticalTimeMilliseconds =
            450;
        profile.testMove.antiBelowObjects = true;
        profile.testMove.shortStopEnabled = true;
        profile.testMove.shortStopMode =
            ShortStopMode::slowMove;
        profile.testMove.shortStopChancePercent = 9;
        profile.testMove.shortStopMinimumPauseMilliseconds =
            40;
        profile.testMove.shortStopMaximumPauseMilliseconds =
            110;
        profile.testMove.shortStopSlowMultiplierMinimum =
            2.2F;
        profile.testMove.shortStopSlowMultiplierMaximum =
            4.6F;
        profile.testMove.newTargetDelayMinimumMilliseconds =
            20;
        profile.testMove.newTargetDelayMaximumMilliseconds =
            80;

        const std::string serializedProfile =
            SerializeProfile(profile);
        const auto profilePath =
            testDirectory / L"profile.json";
        if (!AtomicWrite(
                profilePath,
                serializedProfile,
                changed))
        {
            return finish(false);
        }
        std::string profileText;
        SharedProfile decodedProfile;
        if (!ReadText(profilePath, profileText) ||
            !DeserializeProfile(
                profileText,
                decodedProfile,
                parseError) ||
            SerializeProfile(decodedProfile) !=
                serializedProfile)
        {
            return finish(false);
        }

        LocalConfig rejectedLocal;
        LocalConfig legacyLocal;
        LocalConfig legacyLocalV2;
        if (!DeserializeLocal(
                "{\"kind\":\"vanta-local-config\","
                "\"schema_version\":1}",
                LocalConfig{},
                legacyLocal,
                parseError) ||
            !DeserializeLocal(
                "{\"kind\":\"vanta-local-config\","
                "\"schema_version\":2}",
                LocalConfig{},
                legacyLocalV2,
                parseError) ||
            DeserializeLocal(
                "{ malformed",
                LocalConfig{},
                rejectedLocal,
                parseError) ||
            DeserializeLocal(
                "{\"kind\":\"vanta-local-config\","
                "\"schema_version\":999}",
                LocalConfig{},
                rejectedLocal,
                parseError))
        {
            return finish(false);
        }
        SharedProfile rejectedProfile;
        SharedProfile schemaSeven;
        if (!DeserializeProfile("{\"kind\":\"vanta-test-profile\",\"schema_version\":7,"
                "\"test_move\":{\"target_height_percent\":23,\"merge_proximity\":11}}",
                schemaSeven,parseError) || schemaSeven.testMove.detector.enabled ||
            schemaSeven.testMove.detector.aimPointMode != AimPointMode::boundingBox ||
            schemaSeven.testMove.targetHeightPercent != 23 || schemaSeven.testMove.mergeProximity != 11)
            return finish(false);
        SharedProfile detectorProfile;
        detectorProfile.testMove.detector.closingKernel=7;
        detectorProfile.testMove.detector.dilationKernel=5;
        detectorProfile.testMove.detector.offsetX=-3.25;
        detectorProfile.testMove.detector.orbFillMinimum=.73;
        SharedProfile detectorDecoded;
        if (!DeserializeProfile(SerializeProfile(detectorProfile),detectorDecoded,parseError) ||
            SerializeProfile(detectorDecoded)!=SerializeProfile(detectorProfile) ||
            !detectorDecoded.testMove.detector.enabled ||
            detectorDecoded.testMove.detector.aimPointMode!=AimPointMode::maskTop)
            return finish(false);
        LocalConfig streamConfig,streamDecoded;
        LocalConfig lockConfig,lockDecoded;
        lockConfig.mouseOutput.lockMovementWhileCorrecting=false;
        lockConfig.mouseOutput.lockGeneratedClick=false;
        lockConfig.mouseOutput.reserveActivationSideButtons=false;
        if(!DeserializeLocal(SerializeLocal(lockConfig),LocalConfig{},lockDecoded,parseError) ||
            lockDecoded.mouseOutput.lockMovementWhileCorrecting || lockDecoded.mouseOutput.lockGeneratedClick ||
            lockDecoded.mouseOutput.reserveActivationSideButtons ||
            !LocalConfig{}.mouseOutput.lockMovementWhileCorrecting || !LocalConfig{}.mouseOutput.lockGeneratedClick ||
            !LocalConfig{}.mouseOutput.reserveActivationSideButtons)return finish(false);
        LocalConfig motionConfig,motionDecoded;
        motionConfig.mouseOutput.backend=InputBackend::medius;
        if(!DeserializeLocal(SerializeLocal(motionConfig),LocalConfig{},motionDecoded,parseError) ||
            SerializeLocal(motionConfig)!=SerializeLocal(motionDecoded) ||
            LocalConfig{}.mouseOutput.backend!=InputBackend::makcu || motionDecoded.mouseOutput.backend!=InputBackend::medius)return finish(false);
        auto obsoleteOutput=MouseOutputToJson(motionConfig.mouseOutput);
        PutNumber(obsoleteOutput,L"python_pipeline",2);PutNumber(obsoleteOutput,L"testmove_pipeline",1);
        PutString(obsoleteOutput,L"renderer_profile","obsolete.json");PutNumber(obsoleteOutput,L"radians_per_count_x",0.0004);
        const auto nativeOutput=MouseOutputToJson(MouseOutputFromJson(obsoleteOutput,{}));
        if(nativeOutput.HasKey(L"python_pipeline") || nativeOutput.HasKey(L"renderer_profile") ||
            nativeOutput.HasKey(L"radians_per_count_x") || String(nativeOutput,L"backend","")!="medius")return finish(false);
        streamConfig.sunshine={1920,1080,144,95000,false};
        if (!DeserializeLocal(SerializeLocal(streamConfig),LocalConfig{},streamDecoded,parseError) ||
            streamConfig.sunshine!=streamDecoded.sunshine ||
            LocalConfig{}.sunshine.width!=2560 || LocalConfig{}.sunshine.height!=1440 ||
            LocalConfig{}.sunshine.fps!=240 || LocalConfig{}.sunshine.bitrateKbps!=150000 || LocalConfig{}.sunshine.codec!=CodecPreference::Auto)
            return finish(false);
        SharedProfile legacyProfile;
        LocalConfig oldNine;
        const std::string schemaNine=R"({"kind":"vanta-local-config","schema_version":9,"sunshine":{"width":1918,"height":1087,"fps":144,"bitrate_kbps":98000,"request_display_mode":false}})";
        if(!DeserializeLocal(schemaNine,LocalConfig{},oldNine,parseError) || oldNine.sunshine.codec!=CodecPreference::H264 ||
            oldNine.sunshine.width!=1918 || oldNine.sunshine.height!=1087 || oldNine.sunshine.fps!=144 || oldNine.sunshine.bitrateKbps!=98000)
            return finish(false);
        const auto ninePath=testDirectory/L"schema-nine.json";
        if(!AtomicWrite(ninePath,schemaNine,changed) || !BackupBeforeSchemaUpgrade(ninePath,LocalConfig::schemaVersion))return finish(false);
        auto nineBackup=ninePath;nineBackup+=L".schema-9.bak";std::string originalNine;
        if(!ReadText(nineBackup,originalNine) || originalNine!=schemaNine)return finish(false);
        if (!DeserializeProfile(
                "{\"kind\":\"vanta-test-profile\","
                "\"schema_version\":1,"
                "\"name\":\"Legacy\"}",
                legacyProfile,
                parseError) ||
            legacyProfile.name != "Legacy")
        {
            return finish(false);
        }
        SharedProfile legacyMouseProfile;
        if (!DeserializeProfile(
                "{\"kind\":\"vanta-test-profile\","
                "\"schema_version\":6,"
                "\"test_click\":{\"hold_key\":5},"
                "\"test_move\":{\"aim_key\":2}}",
                legacyMouseProfile,
                parseError) ||
            legacyMouseProfile.testClick.holdButton !=
                MouseButtonBinding::x1 ||
            legacyMouseProfile.testMove.aimButton !=
                MouseButtonBinding::right)
        {
            return finish(false);
        }
        if (DeserializeProfile(
                "{\"kind\":\"vanta-test-profile\","
                "\"schema_version\":999}",
                rejectedProfile,
                parseError))
        {
            return finish(false);
        }

        LocalConfig migratedLocal;
        const std::string oldBackends=R"({"kind":"vanta-local-config","schema_version":7,"capture":{"source":0,"backend":1,"region":1,"region_size":384,"color_target":3,"apply_filter":true,"monitor_device":"legacy","window_title":"legacy"},"mouse_output":{"backend":1,"makcu_port":"COM42","auto_detect_and_connect":false},"sunshine":{"width":1920,"height":1080,"fps":144,"bitrate_kbps":90000}})";
        if (!DeserializeLocal(oldBackends,LocalConfig{},migratedLocal,parseError) ||
            migratedLocal.capture.regionSize!=384 || migratedLocal.capture.colorTargetIndex!=3 ||
            !migratedLocal.capture.applyFilter || migratedLocal.mouseOutput.makcuPort!="COM42" ||
            migratedLocal.mouseOutput.autoDetectAndConnect ||
            migratedLocal.sunshine.width!=1920 || migratedLocal.sunshine.fps!=144)
            return finish(false);
        // Destroy Windows JSON objects before finish() uninitializes COM.
        const bool canonicalBackends=[&]()
        {
            const auto migratedJson=JsonObject::Parse(winrt::to_hstring(SerializeLocal(migratedLocal)));
            return String(Child(migratedJson,L"capture"),L"source","")=="sunshine" &&
                String(Child(migratedJson,L"mouse_output"),L"backend","")=="makcu" &&
                !Child(migratedJson,L"capture").HasKey(L"window_title") &&
                !Child(migratedJson,L"capture").HasKey(L"monitor_device") &&
                !Child(migratedJson,L"capture").HasKey(L"backend");
        }();
        if (!canonicalBackends)
            return finish(false);
        // Schema 8 migration retains detector tuning, discards the removed
        // timer, and seeds Python from the supplied config without arming it.
        LocalConfig pythonDefaults;
        pythonDefaults.pythonConfigJson=DefaultPythonBotJson();
        LocalConfig schemaEight;
        SharedProfile profileEight;
        const std::string oldEight=R"({"kind":"vanta-local-config","schema_version":8,"bomb_timer":{"enabled":true},"capture":{"region_size":384},"mouse_output":{"makcu_port":"COM9"}})";
        if (!DeserializeLocal(oldEight,pythonDefaults,schemaEight,parseError) ||
            schemaEight.capture.regionSize!=384 || schemaEight.mouseOutput.makcuPort!="COM9" ||
            !schemaEight.mouseOutput.lockMovementWhileCorrecting || !schemaEight.mouseOutput.lockGeneratedClick ||
            !schemaEight.mouseOutput.reserveActivationSideButtons ||
            schemaEight.pythonConfigJson!=pythonDefaults.pythonConfigJson ||
            SerializeLocal(schemaEight).find("bomb_timer")!=std::string::npos ||
            !DeserializeProfile(R"({"kind":"vanta-test-profile","schema_version":8,"test_move":{"detector":{"enabled":true,"offset_x":-2.5}}})",
                profileEight,parseError) || !profileEight.testMove.detector.enabled ||
            profileEight.testMove.detector.offsetX!=-2.5 || !profileEight.pythonConfigJson.empty())
            return finish(false);
        LocalConfig invalidPython;
        if (DeserializeLocal(R"({"kind":"vanta-local-config","schema_version":9,"python":{"fov_radius":0}})",
            pythonDefaults,invalidPython,parseError)) return finish(false);
        const auto eightPath=testDirectory/L"schema-eight.json";
        if (!AtomicWrite(eightPath,oldEight,changed) ||
            !BackupBeforeSchemaUpgrade(eightPath,LocalConfig::schemaVersion)) return finish(false);
        auto eightBackup=eightPath;
        eightBackup+=L".schema-8.bak";
        std::string preservedEight;
        if (!ReadText(eightBackup,preservedEight) || preservedEight!=oldEight) return finish(false);
        const auto legacyLocalPath=testDirectory/L"legacy-local.json";
        if (!AtomicWrite(legacyLocalPath,oldBackends,changed) ||
            !BackupBeforeSchemaUpgrade(legacyLocalPath,LocalConfig::schemaVersion))
            return finish(false);
        auto localBackup=legacyLocalPath;
        localBackup+=L".schema-7.bak";
        std::string originalLocal;
        if (!ReadText(localBackup,originalLocal) || originalLocal!=oldBackends)
            return finish(false);

        const auto oldProfilePath=testDirectory/L"legacy-profile.json";
        const std::string oldProfile="{\"kind\":\"vanta-test-profile\",\"schema_version\":7}";
        if (!AtomicWrite(oldProfilePath,oldProfile,changed) ||
            !BackupBeforeSchemaUpgrade(oldProfilePath,SharedProfile::schemaVersion)) return finish(false);
        std::string backupText;
        auto backupPath=oldProfilePath;
        backupPath+=L".schema-7.bak";
        if (!ReadText(backupPath,backupText) || backupText!=oldProfile) return finish(false);
        if (!AtomicWrite(oldProfilePath,SerializeProfile(detectorProfile),changed) ||
            !BackupBeforeSchemaUpgrade(oldProfilePath,SharedProfile::schemaVersion) ||
            !ReadText(backupPath,backupText) || backupText!=oldProfile) return finish(false);
        return finish(true);
    }

    struct ConfigManager::Implementation
    {
        bool enabled{};
        bool apartmentInitialized{};
        LocalConfig initialLocal;
        std::filesystem::path rootDirectory;
        std::filesystem::path profilesDirectory;
        std::filesystem::path localPath;
        std::string status{
            "Configuration system not initialized"};
        std::vector<std::string> profiles;
        int selectedProfile{};
        std::array<char, 128> profileName{
            'D', 'e', 'f', 'a', 'u', 'l', 't', '\0'};
        std::uint64_t captureRevision{};
        std::uint64_t mouseRevision{};
        std::uint64_t pythonBotRevision{};
        std::uint64_t updateRevision{};
        std::uint64_t menuRevision{};
        std::uint64_t sunshineRevision{};

        bool SaveLocalConfig(
            const LocalConfig& config)
        {
            if (!enabled)
            {
                return true;
            }
            bool changed = false;
            if (!BackupBeforeSchemaUpgrade(localPath,LocalConfig::schemaVersion))
            {
                status="Could not preserve the older local settings before migration";
                return false;
            }
            if (!AtomicWrite(
                    localPath,
                    SerializeLocal(config),
                    changed))
            {
                status =
                    "Could not write local.json";
                vanta::log::Warning(
                    "local configuration write failed: %ls",
                    localPath.c_str());
                return false;
            }
            if (changed)
            {
                status =
                    "Local settings saved";
                vanta::log::Info(
                    "local configuration saved: %ls",
                    localPath.c_str());
            }
            initialLocal = config;
            return true;
        }

        std::filesystem::path ProfilePath(
            const std::string& name) const
        {
            return profilesDirectory /
                Utf8Path(
                    SanitizeProfileName(name) +
                    ".json");
        }

        void RefreshProfiles()
        {
            profiles.clear();
            std::error_code error;
            std::filesystem::create_directories(
                profilesDirectory,
                error);
            if (!error)
            {
                for (const auto& entry :
                     std::filesystem::
                         directory_iterator(
                             profilesDirectory,
                             error))
                {
                    if (error)
                    {
                        break;
                    }
                    if (entry.is_regular_file() &&
                        _wcsicmp(
                            entry.path().
                                extension().
                                c_str(),
                            L".json") == 0)
                    {
                        profiles.push_back(
                            PathStemUtf8(
                                entry.path()));
                    }
                }
            }
            std::sort(
                profiles.begin(),
                profiles.end());
            selectedProfile =
                std::clamp(
                    selectedProfile,
                    0,
                    std::max(
                        0,
                        static_cast<int>(
                            profiles.size()) - 1));
        }

        bool SaveProfile(
            const SharedProfile& source)
        {
            SharedProfile profile = source;
            profile.name =
                SanitizeProfileName(profile.name);
            bool changed = false;
            const auto path =
                ProfilePath(profile.name);
            if (!BackupBeforeSchemaUpgrade(path,SharedProfile::schemaVersion))
            {
                status="Could not preserve the older profile before migration";
                return false;
            }
            if (!AtomicWrite(
                    path,
                    SerializeProfile(profile),
                    changed))
            {
                status =
                    "Could not save Test profile";
                return false;
            }
            status =
                changed
                ? "Saved profile " + profile.name
                : "Profile already up to date";
            RefreshProfiles();
            const auto selected =
                std::find(
                    profiles.begin(),
                    profiles.end(),
                    profile.name);
            if (selected != profiles.end())
            {
                selectedProfile =
                    static_cast<int>(
                        std::distance(
                            profiles.begin(),
                            selected));
            }
            vanta::log::Info(
                "Test profile saved: %s",
                profile.name.c_str());
            return true;
        }

        bool LoadProfileFile(
            const std::filesystem::path& path,
            SharedProfile& profile)
        {
            std::string text;
            if (!ReadText(path, text))
            {
                status =
                    "Could not read Test profile";
                return false;
            }
            std::string error;
            if (!DeserializeProfile(
                    text,
                    profile,
                    error))
            {
                status =
                    "Invalid profile: " + error;
                vanta::log::Warning(
                    "Test profile rejected: %s",
                    error.c_str());
                return false;
            }
            return true;
        }

        void CopyProfileName(
            const std::string& value)
        {
            profileName.fill('\0');
            std::copy_n(
                value.data(),
                std::min(
                    value.size(),
                    profileName.size() - 1),
                profileName.data());
        }

        void RenderPanel(
            HWND owner,
            TestClickController& testClick,
            TestMoveController& testMove,
            PythonBotController& pythonBot)
        {
            const float bodyHeight = std::max(
                190.0F,
                ImGui::GetContentRegionAvail().y -
                    40.0F);
            custom::Child(
                ICON_FILE_LINE
                    "  Configuration##configuration",
                ImVec2(0.0F, bodyHeight),
                true);
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));

            ImGui::TextColored(
                enabled
                    ? ImVec4(
                        0.45F,
                        0.96F,
                        0.65F,
                        1.0F)
                    : ImVec4(
                        0.72F,
                        0.72F,
                        0.78F,
                        1.0F),
                "%s",
                status.c_str());
            ImGui::TextDisabled(
                "Local settings: %ls",
                localPath.c_str());
            ImGui::TextWrapped(
                "Machine-local capture, input, Python, menu and "
                "style/update settings "
                "save automatically. Test profiles include both Test "
                "enable states and Python settings (Python never auto-arms).");
            custom::Separator();

            ImGui::Text(
                "Shareable Test profile");
            ImGui::SetNextItemWidth(
                std::min(
                    360.0F,
                    ImGui::GetContentRegionAvail().x));
            ImGui::InputText(
                "Profile name",
                profileName.data(),
                profileName.size());

            std::vector<const char*> items;
            items.reserve(profiles.size());
            for (const auto& profile : profiles)
            {
                items.push_back(profile.c_str());
            }
            const char* empty[]{
                "No saved profiles"};
            if (custom::Combo(
                    "Saved profiles",
                    &selectedProfile,
                    items.empty()
                        ? empty
                        : items.data(),
                    items.empty()
                        ? 1
                        : static_cast<int>(
                            items.size())) &&
                !profiles.empty())
            {
                CopyProfileName(
                    profiles[
                        static_cast<std::size_t>(
                            selectedProfile)]);
            }

            if (custom::Button(
                    ICON_SAVE_LINE "  Save / overwrite",
                    ImVec2(190.0F, 36.0F)))
            {
                SharedProfile profile;
                profile.name =
                    profileName.data();
                profile.testClick =
                    testClick.GetConfig();
                profile.testMove =
                    testMove.GetConfig();
                profile.pythonConfigJson=pythonBot.GetConfigJson();
                SaveProfile(profile);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(
                profiles.empty());
            if (custom::Button(
                    ICON_FILE_IMPORT_LINE "  Load",
                    ImVec2(130.0F, 36.0F)))
            {
                SharedProfile profile;
                const auto& name =
                    profiles[
                        static_cast<std::size_t>(
                            selectedProfile)];
                if (LoadProfileFile(
                        ProfilePath(name),
                        profile))
                {
                    std::string pythonError;
                    const bool pythonApplied=profile.pythonConfigJson.empty() ||
                        pythonBot.ApplyConfigJson(profile.pythonConfigJson,pythonError);
                    if (!pythonApplied)
                    {
                        status="Python profile rejected: "+pythonError;
                    }
                    else
                    {
                        pythonBot.SetEnabled(false);
                        testClick.ApplyConfig(
                            profile.testClick);
                        testMove.ApplyConfig(
                            profile.testMove);
                        status =
                            "Loaded profile " +
                            profile.name;
                        vanta::log::Info(
                            "Test profile loaded: %s",
                            profile.name.c_str());
                    }
                }
            }
            ImGui::SameLine();
            if (custom::Button(
                    ICON_DELETE_LINE "  Delete",
                    ImVec2(130.0F, 36.0F)))
            {
                const std::string name =
                    profiles[
                        static_cast<std::size_t>(
                            selectedProfile)];
                if (DeleteFileW(
                        ProfilePath(name).c_str()))
                {
                    status =
                        "Deleted profile " +
                        name;
                    RefreshProfiles();
                }
                else
                {
                    status =
                        "Could not delete profile " +
                        name;
                }
            }
            ImGui::EndDisabled();

            if (custom::Button(
                    ICON_FILE_IMPORT_LINE "  Import",
                    ImVec2(150.0F, 36.0F)))
            {
                const auto path =
                    ChooseOpenProfile(owner);
                if (!path.empty())
                {
                    SharedProfile profile;
                    if (LoadProfileFile(
                            path,
                            profile))
                    {
                        profile.name =
                            SanitizeProfileName(
                                profile.name.empty()
                                ? PathStemUtf8(path)
                                : profile.name);
                        const std::string base =
                            profile.name;
                        int suffix = 2;
                        while (
                            std::filesystem::exists(
                                ProfilePath(
                                    profile.name)))
                        {
                            profile.name =
                                base +
                                " (" +
                                std::to_string(
                                    suffix++) +
                                ")";
                        }
                        SaveProfile(profile);
                        status =
                            "Imported profile " +
                            profile.name;
                    }
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(
                profiles.empty());
            if (custom::Button(
                    ICON_FILE_EXPORT_LINE "  Export",
                    ImVec2(150.0F, 36.0F)))
            {
                const std::string name =
                    profiles[
                        static_cast<std::size_t>(
                            selectedProfile)];
                SharedProfile profile;
                if (LoadProfileFile(
                        ProfilePath(name),
                        profile))
                {
                    const auto destination =
                        ChooseSaveProfile(
                            owner,
                            name);
                    if (!destination.empty())
                    {
                        bool changed = false;
                        if (AtomicWrite(
                                destination,
                                SerializeProfile(
                                    profile),
                                changed))
                        {
                            status =
                                "Exported profile " +
                                name;
                        }
                        else
                        {
                            status =
                                "Could not export profile";
                        }
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (custom::Button(
                    ICON_FOLDER_OPEN_LINE "  Open folder",
                    ImVec2(170.0F, 36.0F)))
            {
                ShellExecuteW(
                    owner,
                    L"open",
                    profilesDirectory.c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL);
            }

            ImGui::PopStyleVar();
            custom::EndChild();
        }

        void Shutdown()
        {
            if (apartmentInitialized)
            {
                winrt::uninit_apartment();
                apartmentInitialized = false;
            }
            enabled = false;
        }
    };

    ConfigManager::ConfigManager()
        : implementation_(
              std::make_unique<Implementation>())
    {
    }

    ConfigManager::~ConfigManager()
    {
        Shutdown();
    }

    bool ConfigManager::Initialize(
        bool persistenceEnabled,
        const LocalConfig& defaults)
    {
        auto& impl = *implementation_;
        try
        {
            winrt::init_apartment(
                winrt::apartment_type::
                    multi_threaded);
            impl.apartmentInitialized = true;
        }
        catch (const winrt::hresult_error& error)
        {
            if (error.code() !=
                RPC_E_CHANGED_MODE)
            {
                impl.status =
                    "Windows JSON initialization failed";
                return false;
            }
        }

        impl.enabled =
            persistenceEnabled;
        impl.initialLocal =
            defaults;
        if (!persistenceEnabled)
        {
            impl.status =
                "Configuration disabled for self-test";
            return true;
        }

        std::array<wchar_t, 32768> localAppData{};
        const DWORD length =
            GetEnvironmentVariableW(
                L"LOCALAPPDATA",
                localAppData.data(),
                static_cast<DWORD>(
                    localAppData.size()));
        if (length == 0 ||
            length >= localAppData.size())
        {
            impl.status =
                "LOCALAPPDATA is unavailable";
            return !persistenceEnabled;
        }
        impl.rootDirectory =
            std::filesystem::path(
                localAppData.data()) /
            L"Vanta";
        impl.profilesDirectory =
            impl.rootDirectory /
            L"profiles";
        impl.localPath =
            impl.rootDirectory /
            L"local.json";

        std::error_code directoryError;
        std::filesystem::create_directories(
            impl.profilesDirectory,
            directoryError);
        if (directoryError &&
            persistenceEnabled)
        {
            impl.status =
                "Could not create configuration directory";
            return false;
        }

        if (std::filesystem::exists(
                impl.localPath))
        {
            std::string text;
            std::string error;
            LocalConfig loaded;
            if (ReadText(
                    impl.localPath,
                    text) &&
                DeserializeLocal(
                    text,
                    defaults,
                    loaded,
                    error))
            {
                impl.initialLocal =
                    std::move(loaded);
                impl.status =
                    "Local settings loaded";
                vanta::log::Info(
                    "local configuration loaded: %ls",
                    impl.localPath.c_str());
            }
            else
            {
                impl.status =
                    "Local settings ignored: " +
                    (error.empty()
                        ? "read failed"
                        : error);
                vanta::log::Warning(
                    "local configuration retained but ignored: %s",
                    impl.status.c_str());
            }
        }
        else if (!impl.SaveLocalConfig(
                     defaults))
        {
            return false;
        }
        impl.RefreshProfiles();
        return true;
    }

    const LocalConfig&
    ConfigManager::InitialLocalConfig() const noexcept
    {
        return implementation_->initialLocal;
    }

    void ConfigManager::PrimeRevisions(
        const CaptureController& capture,
        const MakcuController& mouseOutput,
        const PythonBotController& pythonBot,
        const UpdateController& updates,
        const StreamReceiverController& sunshine)
    {
        auto& impl = *implementation_;
        impl.captureRevision =
            capture.SettingsRevision();
        impl.mouseRevision =
            mouseOutput.SettingsRevision();
        impl.pythonBotRevision =
            pythonBot.SettingsRevision();
        impl.updateRevision =
            updates.SettingsRevision();
        impl.menuRevision =
            menu::SettingsRevision();
        impl.sunshineRevision = sunshine.SettingsRevision();
    }

    void ConfigManager::AutoSaveLocal(
        const CaptureController& capture,
        const MakcuController& mouseOutput,
        const PythonBotController& pythonBot,
        const UpdateController& updates,
        const StreamReceiverController& sunshine)
    {
        auto& impl = *implementation_;
        if (!impl.enabled)
        {
            return;
        }
        const std::uint64_t captureRevision =
            capture.SettingsRevision();
        const std::uint64_t mouseRevision =
            mouseOutput.SettingsRevision();
        const std::uint64_t pythonBotRevision =
            pythonBot.SettingsRevision();
        const std::uint64_t updateRevision =
            updates.SettingsRevision();
        const std::uint64_t menuRevision =
            menu::SettingsRevision();
        if (captureRevision ==
                impl.captureRevision &&
            mouseRevision ==
                impl.mouseRevision &&
            pythonBotRevision ==
                impl.pythonBotRevision &&
            updateRevision ==
                impl.updateRevision &&
            menuRevision ==
                impl.menuRevision && sunshine.SettingsRevision() == impl.sunshineRevision)
        {
            return;
        }

        LocalConfig current;
        current.sunshine = sunshine.GetConfig();
        current.capture =
            capture.GetConfig();
        current.mouseOutput =
            mouseOutput.GetConfig();
        current.pythonConfigJson = pythonBot.GetConfigJson();
        current.updates =
            updates.GetConfig();
        current.menu =
            menu::GetConfig();
        if (impl.SaveLocalConfig(current))
        {
            impl.captureRevision =
                captureRevision;
            impl.mouseRevision =
                mouseRevision;
            impl.pythonBotRevision =
                pythonBotRevision;
            impl.updateRevision =
                updateRevision;
            impl.menuRevision =
                menuRevision;
            impl.sunshineRevision = sunshine.SettingsRevision();
        }
    }

    void ConfigManager::RenderPanel(
        HWND owner,
        TestClickController& testClick,
        TestMoveController& testMove,
        PythonBotController& pythonBot)
    {
        implementation_->RenderPanel(
            owner,
            testClick,
            testMove,
            pythonBot);
    }

    void ConfigManager::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }
}
