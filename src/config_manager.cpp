#define IMGUI_DEFINE_MATH_OPERATORS

#include "config_manager.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

#include "capture_controller.hpp"
#include "bomb_timer_controller.hpp"
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

    JsonObject CaptureToJson(
        const vanta::CaptureConfig& config)
    {
        JsonObject result;
        PutNumber(
            result,
            L"backend",
            config.backendIndex);
        PutNumber(
            result,
            L"source",
            config.sourceIndex);
        PutNumber(
            result,
            L"region",
            config.regionIndex);
        PutNumber(
            result,
            L"region_size",
            config.regionSize);
        PutBoolean(
            result,
            L"draw_outline",
            config.drawOutline);
        result.Insert(
            L"outline_color",
            ColorToJson(config.outlineColor));
        PutBoolean(
            result,
            L"apply_filter",
            config.applyFilter);
        PutBoolean(
            result,
            L"binary_mask",
            config.binaryMask);
        PutNumber(
            result,
            L"color_target",
            config.colorTargetIndex);
        PutString(
            result,
            L"monitor_device",
            config.monitorDevice);
        PutString(
            result,
            L"window_executable",
            config.windowExecutable);
        PutString(
            result,
            L"window_class",
            config.windowClass);
        PutString(
            result,
            L"window_title",
            config.windowTitle);
        return result;
    }

    vanta::CaptureConfig CaptureFromJson(
        const JsonObject& object,
        const vanta::CaptureConfig& defaults)
    {
        vanta::CaptureConfig result = defaults;
        result.backendIndex =
            static_cast<int>(
                Number(
                    object,
                    L"backend",
                    result.backendIndex));
        result.sourceIndex =
            static_cast<int>(
                Number(
                    object,
                    L"source",
                    result.sourceIndex));
        result.regionIndex =
            static_cast<int>(
                Number(
                    object,
                    L"region",
                    result.regionIndex));
        result.regionSize =
            static_cast<int>(
                Number(
                    object,
                    L"region_size",
                    result.regionSize));
        result.drawOutline =
            Boolean(
                object,
                L"draw_outline",
                result.drawOutline);
        result.outlineColor =
            ColorFromJson(
                Child(
                    object,
                    L"outline_color"),
                result.outlineColor);
        result.applyFilter =
            Boolean(
                object,
                L"apply_filter",
                result.applyFilter);
        result.binaryMask =
            Boolean(
                object,
                L"binary_mask",
                result.binaryMask);
        result.colorTargetIndex =
            static_cast<int>(
                Number(
                    object,
                    L"color_target",
                    result.colorTargetIndex));
        result.monitorDevice =
            String(
                object,
                L"monitor_device",
                result.monitorDevice);
        result.windowExecutable =
            String(
                object,
                L"window_executable",
                result.windowExecutable);
        result.windowClass =
            String(
                object,
                L"window_class",
                result.windowClass);
        result.windowTitle =
            String(
                object,
                L"window_title",
                result.windowTitle);
        return result;
    }

    JsonObject MouseOutputToJson(
        const vanta::MouseOutputConfig& config)
    {
        JsonObject result;
        PutNumber(
            result,
            L"backend",
            config.backendIndex);
        PutString(
            result,
            L"makcu_port",
            config.makcuPort);
        PutBoolean(
            result,
            L"high_performance",
            config.highPerformanceMode);
        PutBoolean(
            result,
            L"auto_detect_and_connect",
            config.autoDetectAndConnect);
        return result;
    }

    vanta::MouseOutputConfig MouseOutputFromJson(
        const JsonObject& object,
        const vanta::MouseOutputConfig& defaults)
    {
        vanta::MouseOutputConfig result = defaults;
        result.backendIndex =
            static_cast<int>(
                Number(
                    object,
                    L"backend",
                    result.backendIndex));
        result.makcuPort =
            String(
                object,
                L"makcu_port",
                result.makcuPort);
        result.highPerformanceMode =
            Boolean(
                object,
                L"high_performance",
                result.highPerformanceMode);
        result.autoDetectAndConnect =
            Boolean(
                object,
                L"auto_detect_and_connect",
                result.autoDetectAndConnect);
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

    JsonObject BombTimerToJson(
        const vanta::BombTimerConfig& config)
    {
        JsonObject result;
        PutBoolean(result, L"enabled", config.enabled);
        result.Insert(
            L"target_color",
            ColorToJson(config.targetColor));
        PutNumber(
            result,
            L"color_tolerance",
            config.colorTolerance);
        PutNumber(result, L"region_left", config.regionLeft);
        PutNumber(result, L"region_top", config.regionTop);
        PutNumber(result, L"region_width", config.regionWidth);
        PutNumber(result, L"region_height", config.regionHeight);
        PutNumber(
            result,
            L"required_matching_pixels",
            config.requiredMatchingPixels);
        PutNumber(
            result,
            L"required_consecutive_frames",
            config.requiredConsecutiveFrames);
        PutNumber(result, L"reset_key", config.resetKey);
        result.Insert(
            L"safe_color",
            ColorToJson(config.safeColor));
        result.Insert(
            L"warning_color",
            ColorToJson(config.warningColor));
        PutNumber(
            result,
            L"widget_opacity",
            config.widgetOpacity);
        PutNumber(
            result,
            L"widget_style",
            config.widgetStyle);
        PutNumber(
            result,
            L"widget_font",
            config.widgetFont);
        PutBoolean(
            result,
            L"has_widget_position",
            config.hasWidgetPosition);
        PutNumber(
            result,
            L"widget_x",
            config.widgetPositionX);
        PutNumber(
            result,
            L"widget_y",
            config.widgetPositionY);
        return result;
    }

    vanta::BombTimerConfig BombTimerFromJson(
        const JsonObject& object,
        const vanta::BombTimerConfig& defaults)
    {
        vanta::BombTimerConfig result = defaults;
        result.enabled =
            Boolean(object, L"enabled", result.enabled);
        result.targetColor =
            ColorFromJson(
                Child(object, L"target_color"),
                result.targetColor);
        result.colorTolerance =
            static_cast<int>(
                Number(
                    object,
                    L"color_tolerance",
                    result.colorTolerance));
        result.regionLeft =
            static_cast<float>(
                Number(
                    object,
                    L"region_left",
                    result.regionLeft));
        result.regionTop =
            static_cast<float>(
                Number(
                    object,
                    L"region_top",
                    result.regionTop));
        result.regionWidth =
            static_cast<float>(
                Number(
                    object,
                    L"region_width",
                    result.regionWidth));
        result.regionHeight =
            static_cast<float>(
                Number(
                    object,
                    L"region_height",
                    result.regionHeight));
        result.requiredMatchingPixels =
            static_cast<int>(
                Number(
                    object,
                    L"required_matching_pixels",
                    result.requiredMatchingPixels));
        result.requiredConsecutiveFrames =
            static_cast<int>(
                Number(
                    object,
                    L"required_consecutive_frames",
                    result.requiredConsecutiveFrames));
        result.resetKey =
            static_cast<int>(
                Number(
                    object,
                    L"reset_key",
                    result.resetKey));
        result.safeColor =
            ColorFromJson(
                Child(object, L"safe_color"),
                result.safeColor);
        result.warningColor =
            ColorFromJson(
                Child(object, L"warning_color"),
                result.warningColor);
        result.widgetOpacity =
            static_cast<float>(
                Number(
                    object,
                    L"widget_opacity",
                    result.widgetOpacity));
        result.widgetStyle =
            static_cast<int>(
                Number(
                    object,
                    L"widget_style",
                    result.widgetStyle));
        result.widgetFont =
            static_cast<int>(
                Number(
                    object,
                    L"widget_font",
                    result.widgetFont));
        result.hasWidgetPosition =
            Boolean(
                object,
                L"has_widget_position",
                result.hasWidgetPosition);
        result.widgetPositionX =
            static_cast<float>(
                Number(
                    object,
                    L"widget_x",
                    result.widgetPositionX));
        result.widgetPositionY =
            static_cast<float>(
                Number(
                    object,
                    L"widget_y",
                    result.widgetPositionY));
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
        PutNumber(result, L"hold_key", config.triggerKey);
        PutNumber(result, L"capture_radius", config.roiOffset);
        PutNumber(result, L"ray_length", config.rayLength);
        PutNumber(result, L"ray_half_width", config.rayHalfWidth);
        PutNumber(result, L"hit_mode", config.hitMode);
        PutBoolean(result, L"burst_limiter", config.burstLimiter);
        PutNumber(result, L"shot_delay", config.burstDelay);
        PutNumber(result, L"burst_pause", config.burstPause);
        PutNumber(result, L"burst_size", config.burstSize);
        PutBoolean(
            result,
            L"pause_while_moving",
            config.respectMovement);
        return result;
    }

    vanta::TestClickConfig TestClickFromJson(
        const JsonObject& object,
        const vanta::TestClickConfig& defaults)
    {
        vanta::TestClickConfig result = defaults;
        result.enabled =
            Boolean(object, L"enabled", result.enabled);
        result.hsvRangeIndex =
            static_cast<int>(
                Number(object, L"color_target", result.hsvRangeIndex));
        result.triggerKey =
            static_cast<int>(
                Number(object, L"hold_key", result.triggerKey));
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
        result.respectMovement =
            Boolean(
                object,
                L"pause_while_moving",
                result.respectMovement);
        return result;
    }

    JsonObject TestMoveToJson(
        const vanta::TestMoveConfig& config)
    {
        JsonObject result;
        PutBoolean(result, L"enabled", config.enabled);
        PutNumber(result, L"color_target", config.hsvRangeIndex);
        PutNumber(result, L"aim_key", config.aimKey);
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
        const vanta::TestMoveConfig& defaults)
    {
        vanta::TestMoveConfig result = defaults;
        result.enabled =
            Boolean(object, L"enabled", result.enabled);
        result.hsvRangeIndex =
            static_cast<int>(
                Number(object, L"color_target", result.hsvRangeIndex));
        result.aimKey =
            static_cast<int>(
                Number(object, L"aim_key", result.aimKey));
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
        root.Insert(
            L"mouse_output",
            MouseOutputToJson(config.mouseOutput));
        root.Insert(
            L"updates",
            UpdateToJson(config.updates));
        root.Insert(
            L"bomb_timer",
            BombTimerToJson(config.bombTimer));
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
            result.bombTimer =
                BombTimerFromJson(
                    Child(root, L"bomb_timer"),
                    defaults.bombTimer);
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
                    result.testClick);
            result.testMove =
                TestMoveFromJson(
                    Child(root, L"test_move"),
                    result.testMove);
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
        if (!RunBombTimerSelfTest())
        {
            vanta::log::Error(
                "Bomb Timer self-test failed");
            return false;
        }
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
        local.capture.backendIndex = 1;
        local.capture.sourceIndex = 0;
        local.capture.regionIndex = 1;
        local.capture.regionSize = 512;
        local.capture.monitorDevice =
            "\\\\.\\DISPLAY2";
        local.capture.windowExecutable =
            "C:\\Tests\\Target.exe";
        local.capture.windowClass =
            "VantaConfigSelfTestWindow";
        local.capture.windowTitle =
            "Configuration Test";
        local.capture.colorTargetIndex = 1;
        local.mouseOutput.backendIndex = 1;
        local.mouseOutput.makcuPort = "COM42";
        local.mouseOutput.autoDetectAndConnect = false;
        local.updates.automaticChecks = false;
        local.updates.automaticDownloads = false;
        local.updates.silentAutomaticInstallation = false;
        local.bombTimer.enabled = true;
        local.bombTimer.colorTolerance = 24;
        local.bombTimer.regionLeft = 0.42F;
        local.bombTimer.requiredMatchingPixels = 7;
        local.bombTimer.resetKey = VK_F8;
        local.bombTimer.widgetOpacity = 0.66F;
        local.bombTimer.hasWidgetPosition = true;
        local.bombTimer.widgetPositionX = 123.0F;
        local.bombTimer.widgetPositionY = 456.0F;
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
        profile.testClick.enabled = true;
        profile.testClick.triggerKey = VK_XBUTTON1;
        profile.testClick.rayLength = 73;
        profile.testClick.hitMode = 1;
        profile.testMove.aimKey = VK_LBUTTON;
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
        SharedProfile legacyProfile;
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
        if (DeserializeProfile(
                "{\"kind\":\"vanta-test-profile\","
                "\"schema_version\":999}",
                rejectedProfile,
                parseError))
        {
            return finish(false);
        }

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
        std::uint64_t bombTimerRevision{};
        std::uint64_t updateRevision{};
        std::uint64_t menuRevision{};

        bool SaveLocalConfig(
            const LocalConfig& config)
        {
            if (!enabled)
            {
                return true;
            }
            bool changed = false;
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
            TestMoveController& testMove)
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
                "Machine-local capture, input, Bomb Timer, menu and "
                "style/update settings "
                "save automatically. Test profiles include both Test "
                "enable states.");
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
        const BombTimerController& bombTimer,
        const UpdateController& updates)
    {
        auto& impl = *implementation_;
        impl.captureRevision =
            capture.SettingsRevision();
        impl.mouseRevision =
            mouseOutput.SettingsRevision();
        impl.bombTimerRevision =
            bombTimer.SettingsRevision();
        impl.updateRevision =
            updates.SettingsRevision();
        impl.menuRevision =
            menu::SettingsRevision();
    }

    void ConfigManager::AutoSaveLocal(
        const CaptureController& capture,
        const MakcuController& mouseOutput,
        const BombTimerController& bombTimer,
        const UpdateController& updates)
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
        const std::uint64_t bombTimerRevision =
            bombTimer.SettingsRevision();
        const std::uint64_t updateRevision =
            updates.SettingsRevision();
        const std::uint64_t menuRevision =
            menu::SettingsRevision();
        if (captureRevision ==
                impl.captureRevision &&
            mouseRevision ==
                impl.mouseRevision &&
            bombTimerRevision ==
                impl.bombTimerRevision &&
            updateRevision ==
                impl.updateRevision &&
            menuRevision ==
                impl.menuRevision)
        {
            return;
        }

        LocalConfig current;
        current.capture =
            capture.GetConfig();
        current.mouseOutput =
            mouseOutput.GetConfig();
        current.bombTimer =
            bombTimer.GetConfig();
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
            impl.bombTimerRevision =
                bombTimerRevision;
            impl.updateRevision =
                updateRevision;
            impl.menuRevision =
                menuRevision;
        }
    }

    void ConfigManager::RenderPanel(
        HWND owner,
        TestClickController& testClick,
        TestMoveController& testMove)
    {
        implementation_->RenderPanel(
            owner,
            testClick,
            testMove);
    }

    void ConfigManager::Shutdown()
    {
        if (implementation_ != nullptr)
        {
            implementation_->Shutdown();
        }
    }
}
