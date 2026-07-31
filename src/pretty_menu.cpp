#define IMGUI_DEFINE_MATH_OPERATORS

#include "pretty_menu.hpp"

#include "capture_controller.hpp"
#include "bomb_timer_controller.hpp"
#include "config_manager.hpp"
#include "logger.hpp"
#include "makcu_controller.hpp"
#include "testclick_controller.hpp"
#include "testmove_controller.hpp"
#include "update_controller.hpp"

#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_internal.h"
#include "imgui_settings.h"
#include "custom_widgets.hpp"
#include "font.h"
#include "font_defines.h"

#include <algorithm>
#include <atomic>

namespace
{
    bool g_visible = true;
    bool g_notifications = true;
    bool g_compactMode = false;
    bool g_lastCompactMode = false;
    int g_page = 1;
    ImVec2 g_menuPosition{};
    ImVec2 g_menuSize{};
    bool g_applyConfiguredGeometry = false;
    std::atomic<std::uint64_t>
        g_settingsRevision{0};

    constexpr ImGuiColorEditFlags palettePickerFlags =
        ImGuiColorEditFlags_NoSidePreview |
        ImGuiColorEditFlags_AlphaBar |
        ImGuiColorEditFlags_NoInputs |
        ImGuiColorEditFlags_AlphaPreview;

    bool PaletteColor(const char* label, ImColor& colorValue)
    {
        return custom::ColorEdit4(
            label,
            &colorValue.Value.x,
            palettePickerFlags);
    }

    bool PaletteColor(const char* label, ImVec4& colorValue)
    {
        return custom::ColorEdit4(
            label,
            &colorValue.x,
            palettePickerFlags);
    }

    void SynchronizeImGuiPalette()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] =
            c::window_bg_color.Value;
        style.Colors[ImGuiCol_ChildBg] =
            c::child::background;
        style.Colors[ImGuiCol_PopupBg] =
            c::window_bg_color.Value;
        style.Colors[ImGuiCol_Border] =
            c::child::stroke;
        style.Colors[ImGuiCol_FrameBg] =
            c::elements::background;
        style.Colors[ImGuiCol_FrameBgHovered] =
            c::elements::background_hovered;
        style.Colors[ImGuiCol_FrameBgActive] =
            c::page::background_active;
        style.Colors[ImGuiCol_Header] =
            c::page::background;
        style.Colors[ImGuiCol_HeaderHovered] =
            c::page::background_active;
        style.Colors[ImGuiCol_HeaderActive] =
            c::page::background_active;
        style.Colors[ImGuiCol_CheckMark] =
            c::checkbox::mark;
        style.Colors[ImGuiCol_SliderGrab] =
            c::main_color.Value;
        style.Colors[ImGuiCol_SliderGrabActive] =
            c::second_color.Value;
        style.Colors[ImGuiCol_Text] =
            c::text::label::active.Value;
        style.Colors[ImGuiCol_TextDisabled] =
            c::text::label::default_color.Value;
        style.Colors[ImGuiCol_ResizeGrip] =
            ImVec4(
                c::main_color.Value.x,
                c::main_color.Value.y,
                c::main_color.Value.z,
                0.25F);
        style.Colors[ImGuiCol_ResizeGripHovered] =
            c::main_color.Value;
        style.Colors[ImGuiCol_ResizeGripActive] =
            c::second_color.Value;
    }

    POINT OverlayScreenOrigin(HWND overlayWindow)
    {
        POINT origin{};
        ClientToScreen(overlayWindow, &origin);
        return origin;
    }

    ImVec2 InitialMenuPosition(HWND overlayWindow)
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        const HMONITOR monitor = MonitorFromPoint(
            cursor,
            MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO information{sizeof(MONITORINFO)};
        const POINT origin = OverlayScreenOrigin(overlayWindow);
        if (monitor == nullptr ||
            !GetMonitorInfoW(monitor, &information))
        {
            return ImVec2(42.0F, 42.0F);
        }

        return ImVec2(
            static_cast<float>(
                information.rcWork.left - origin.x + 42),
            static_cast<float>(
                information.rcWork.top - origin.y + 42));
    }

    ImVec2 MaximumMenuSize(HWND overlayWindow)
    {
        const POINT origin = OverlayScreenOrigin(overlayWindow);
        HMONITOR monitor = nullptr;

        if (g_menuSize.x > 0.0F && g_menuSize.y > 0.0F)
        {
            RECT currentRectangle{
                origin.x + static_cast<LONG>(g_menuPosition.x),
                origin.y + static_cast<LONG>(g_menuPosition.y),
                origin.x + static_cast<LONG>(
                    g_menuPosition.x + g_menuSize.x),
                origin.y + static_cast<LONG>(
                    g_menuPosition.y + g_menuSize.y)};
            monitor = MonitorFromRect(
                &currentRectangle,
                MONITOR_DEFAULTTONEAREST);
        }
        else
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            monitor = MonitorFromPoint(
                cursor,
                MONITOR_DEFAULTTOPRIMARY);
        }

        MONITORINFO information{sizeof(MONITORINFO)};
        if (monitor == nullptr ||
            !GetMonitorInfoW(monitor, &information))
        {
            return ImVec2(1920.0F, 1080.0F);
        }

        return ImVec2(
            static_cast<float>(
                information.rcWork.right -
                information.rcWork.left -
                16),
            static_cast<float>(
                information.rcWork.bottom -
                information.rcWork.top -
                16));
    }

    ImVec2 ClampWindowToMonitor(
        HWND overlayWindow,
        ImVec2 menuPosition,
        const ImVec2& size)
    {
        constexpr LONG margin = 8;
        const POINT origin = OverlayScreenOrigin(overlayWindow);
        RECT screenRectangle{
            origin.x + static_cast<LONG>(menuPosition.x),
            origin.y + static_cast<LONG>(menuPosition.y),
            origin.x + static_cast<LONG>(menuPosition.x + size.x),
            origin.y + static_cast<LONG>(menuPosition.y + size.y)};

        ImGuiContext& context = *GImGui;
        ImGuiWindow* currentWindow = ImGui::GetCurrentWindow();
        const bool moving =
            context.MovingWindow != nullptr &&
            context.MovingWindow->RootWindow ==
                currentWindow->RootWindow;

        HMONITOR monitor = nullptr;
        if (moving)
        {
            POINT cursor{};
            if (GetCursorPos(&cursor))
            {
                monitor = MonitorFromPoint(
                    cursor,
                    MONITOR_DEFAULTTONEAREST);
            }
        }
        if (monitor == nullptr)
        {
            monitor = MonitorFromRect(
                &screenRectangle,
                MONITOR_DEFAULTTONEAREST);
        }

        MONITORINFO information{sizeof(MONITORINFO)};
        if (monitor == nullptr ||
            !GetMonitorInfoW(monitor, &information))
        {
            return menuPosition;
        }

        const float minimumX = static_cast<float>(
            information.rcWork.left - origin.x + margin);
        const float minimumY = static_cast<float>(
            information.rcWork.top - origin.y + margin);
        const float maximumX = std::max(
            minimumX,
            static_cast<float>(
                information.rcWork.right - origin.x - margin) -
                size.x);
        const float maximumY = std::max(
            minimumY,
            static_cast<float>(
                information.rcWork.bottom - origin.y - margin) -
                size.y);

        menuPosition.x = std::clamp(
            menuPosition.x,
            minimumX,
            maximumX);
        menuPosition.y = std::clamp(
            menuPosition.y,
            minimumY,
            maximumY);
        return menuPosition;
    }

    ImFont* AddEmbeddedFont(
        unsigned char* bytes,
        int byteCount,
        float size,
        int builderFlags)
    {
        ImFontConfig config{};
        config.FontDataOwnedByAtlas = false;
        config.FontBuilderFlags = builderFlags;
        config.OversampleH = 2;
        config.OversampleV = 2;
        return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
            bytes,
            byteCount,
            size,
            &config,
            ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
    }
}

namespace vanta::menu
{
    bool InitializeFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        constexpr int flags =
            ImGuiFreeTypeBuilderFlags_LightHinting |
            ImGuiFreeTypeBuilderFlags_LoadColor;

        ImFont* primary = AddEmbeddedFont(
            PoppinsRegular,
            static_cast<int>(sizeof(PoppinsRegular)),
            18.0F,
            flags);

        static const ImWchar iconRanges[]{0xE000, 0xF8FF, 0};
        ImFontConfig iconConfig{};
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphOffset = ImVec2(0.0F, 2.0F);
        iconConfig.OversampleH = 1;
        iconConfig.OversampleV = 1;
        iconConfig.FontBuilderFlags = flags;
        io.Fonts->AddFontFromMemoryCompressedBase85TTF(
            icomoon_compressed_data_base85,
            18.0F,
            &iconConfig,
            iconRanges);

        font::esp_font = AddEmbeddedFont(
            PoppinsRegular,
            static_cast<int>(sizeof(PoppinsRegular)),
            17.0F,
            flags);
        font::regular_m = AddEmbeddedFont(
            PoppinsMedium,
            static_cast<int>(sizeof(PoppinsMedium)),
            19.0F,
            flags);
        font::regular_l = AddEmbeddedFont(
            PoppinsMedium,
            static_cast<int>(sizeof(PoppinsMedium)),
            36.0F,
            flags);
        font::s_inter_semibold = AddEmbeddedFont(
            PoppinsSemiBold,
            static_cast<int>(sizeof(PoppinsSemiBold)),
            17.0F,
            flags);
        font::bold_font = AddEmbeddedFont(
            PoppinsBold,
            static_cast<int>(sizeof(PoppinsBold)),
            22.0F,
            flags);
        font::inter_medium = AddEmbeddedFont(
            PoppinsMedium,
            static_cast<int>(sizeof(PoppinsMedium)),
            17.0F,
            flags);
        font::inter_semibold = font::s_inter_semibold;
        font::icomoon_page = primary;
        font::icomoon_logo = primary;
        font::icon_notify = primary;
        ImFontConfig timerConfig{};
        timerConfig.FontBuilderFlags = flags;
        timerConfig.OversampleH = 2;
        timerConfig.OversampleV = 2;
        font::timer_segoe = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeuib.ttf",
            22.0F,
            &timerConfig,
            io.Fonts->GetGlyphRangesCyrillic());
        font::timer_bahnschrift = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\bahnschrift.ttf",
            22.0F,
            &timerConfig,
            io.Fonts->GetGlyphRangesCyrillic());
        font::timer_consolas = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\consolab.ttf",
            22.0F,
            &timerConfig,
            io.Fonts->GetGlyphRangesCyrillic());
        if (font::timer_segoe == nullptr)
        {
            font::timer_segoe = font::bold_font;
        }
        if (font::timer_bahnschrift == nullptr)
        {
            font::timer_bahnschrift = font::bold_font;
        }
        if (font::timer_consolas == nullptr)
        {
            font::timer_consolas = font::bold_font;
        }
        io.FontDefault = primary;

        const bool built = primary != nullptr && io.Fonts->Build();
        if (built)
        {
            vanta::log::Info(
                "FreeType font atlas built with embedded Poppins and icon fonts");
        }
        else
        {
            vanta::log::Error("FreeType font atlas build failed");
        }
        return built;
    }

    void ApplyStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark(&style);

        style.FramePadding = ImVec2(10.0F, 6.0F);
        style.ItemSpacing = ImVec2(8.0F, 10.0F);
        style.ItemInnerSpacing = ImVec2(8.0F, 6.0F);
        style.WindowPadding = ImVec2(18.0F, 18.0F);
        style.WindowRounding = 15.0F;
        style.ChildRounding = 8.0F;
        style.FrameRounding = 5.0F;
        style.PopupRounding = 7.0F;
        style.GrabRounding = 5.0F;
        style.ScrollbarRounding = 8.0F;
        style.WindowBorderSize = 0.0F;
        style.ChildBorderSize = 1.0F;
        style.PopupBorderSize = 0.0F;
        style.ScrollbarSize = 6.0F;
        style.WindowShadowSize = 0.0F;

        c::main_color = rgba(173, 143, 233, 1.0F);
        c::second_color = rgba(100, 92, 122, 1.0F);
        c::background_color = rgba(20, 20, 20, 0.50F);
        c::stroke_color = ImColor(255, 255, 255, 0);
        c::window_bg_color = rgba(22, 22, 22, 1.0F);
        c::separator = ImColor(22, 23, 26);
        c::anim::active = ImColor(114, 149, 255, 255);
        c::anim::default_color = ImColor(22, 23, 26, 255);
        c::bg::background = rgba(22, 22, 22, 0.71F);
        c::child::background = rgba(60, 60, 60, 0.25F);
        c::child::stroke = ImColor(18, 18, 24, 0);
        c::page::background_active = ImColor(21, 22, 25);
        c::page::background = ImColor(16, 17, 18);
        c::page::text_hov = ImColor(150, 162, 205);
        c::page::text = ImColor(150, 162, 205);
        c::elements::background_hovered =
            ImColor(21, 22, 25);
        c::elements::background = ImColor(16, 17, 18);
        c::checkbox::mark = ImColor(255, 255, 255, 255);
        c::text::label::active =
            ImColor(255, 255, 255, 255);
        c::text::label::hovered =
            ImColor(205, 205, 205, 255);
        c::text::label::default_color =
            ImColor(150, 150, 150, 220);
        c::text::description::active =
            ImColor(200, 200, 200, 102);
        c::text::description::hovered =
            ImColor(200, 200, 200, 63);
        c::text::description::default_color =
            ImColor(200, 200, 200, 40);
        c::text::text_active = ImColor(255, 255, 255);
        c::text::text_hov = ImColor(150, 162, 205);
        c::text::text = ImColor(150, 162, 205);
        c::bg::background_size = ImVec2(850.0F, 596.0F);

        SynchronizeImGuiPalette();
    }

    MenuConfig GetConfig()
    {
        const auto rgba = [](const ImVec4& value)
        {
            return RgbaColor{
                value.x,
                value.y,
                value.z,
                value.w};
        };
        MenuConfig result;
        result.notifications =
            g_notifications;
        result.compactMode =
            g_compactMode;
        result.activePage =
            g_page;
        result.hasGeometry =
            g_menuSize.x > 0.0F &&
            g_menuSize.y > 0.0F;
        result.positionX =
            g_menuPosition.x;
        result.positionY =
            g_menuPosition.y;
        result.width =
            result.hasGeometry
            ? g_menuSize.x
            : 850.0F;
        result.height =
            result.hasGeometry
            ? g_menuSize.y
            : 596.0F;

        auto& palette = result.palette;
        palette[static_cast<std::size_t>(
            PaletteSlot::accent)] =
            rgba(c::main_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::secondary)] =
            rgba(c::second_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::windowBackground)] =
            rgba(c::window_bg_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::widgetBackground)] =
            rgba(c::background_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::stroke)] =
            rgba(c::stroke_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::separator)] =
            rgba(c::separator);
        palette[static_cast<std::size_t>(
            PaletteSlot::animationActive)] =
            rgba(c::anim::active.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::animationDefault)] =
            rgba(c::anim::default_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::mainBackground)] =
            rgba(c::bg::background);
        palette[static_cast<std::size_t>(
            PaletteSlot::childBackground)] =
            rgba(c::child::background);
        palette[static_cast<std::size_t>(
            PaletteSlot::childStroke)] =
            rgba(c::child::stroke);
        palette[static_cast<std::size_t>(
            PaletteSlot::checkboxMark)] =
            rgba(c::checkbox::mark);
        palette[static_cast<std::size_t>(
            PaletteSlot::pageActive)] =
            rgba(c::page::background_active);
        palette[static_cast<std::size_t>(
            PaletteSlot::pageBackground)] =
            rgba(c::page::background);
        palette[static_cast<std::size_t>(
            PaletteSlot::pageHoverText)] =
            rgba(c::page::text_hov);
        palette[static_cast<std::size_t>(
            PaletteSlot::pageText)] =
            rgba(c::page::text);
        palette[static_cast<std::size_t>(
            PaletteSlot::elementHovered)] =
            rgba(c::elements::background_hovered);
        palette[static_cast<std::size_t>(
            PaletteSlot::elementBackground)] =
            rgba(c::elements::background);
        palette[static_cast<std::size_t>(
            PaletteSlot::labelActive)] =
            rgba(c::text::label::active.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::labelHovered)] =
            rgba(c::text::label::hovered.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::labelDefault)] =
            rgba(c::text::label::default_color.Value);
        palette[static_cast<std::size_t>(
            PaletteSlot::descriptionActive)] =
            rgba(c::text::description::active);
        palette[static_cast<std::size_t>(
            PaletteSlot::descriptionHovered)] =
            rgba(c::text::description::hovered);
        palette[static_cast<std::size_t>(
            PaletteSlot::descriptionDefault)] =
            rgba(c::text::description::default_color);
        palette[static_cast<std::size_t>(
            PaletteSlot::textActive)] =
            rgba(c::text::text_active);
        palette[static_cast<std::size_t>(
            PaletteSlot::textHovered)] =
            rgba(c::text::text_hov);
        palette[static_cast<std::size_t>(
            PaletteSlot::textDefault)] =
            rgba(c::text::text);
        return result;
    }

    void ApplyConfig(const MenuConfig& config)
    {
        const auto vector = [](const RgbaColor& value)
        {
            return ImVec4(
                std::clamp(value.red, 0.0F, 1.0F),
                std::clamp(value.green, 0.0F, 1.0F),
                std::clamp(value.blue, 0.0F, 1.0F),
                std::clamp(value.alpha, 0.0F, 1.0F));
        };
        const auto paletteColor = [&](PaletteSlot slot)
        {
            return vector(
                config.palette[
                    static_cast<std::size_t>(slot)]);
        };

        g_notifications =
            config.notifications;
        g_compactMode =
            config.compactMode;
        g_lastCompactMode =
            g_compactMode;
        g_page =
            std::clamp(
                config.activePage,
                0,
                8);
        if (config.hasGeometry)
        {
            g_menuPosition = ImVec2(
                config.positionX,
                config.positionY);
            g_menuSize = ImVec2(
                std::clamp(
                    config.width,
                    760.0F,
                    4096.0F),
                std::clamp(
                    config.height,
                    540.0F,
                    2160.0F));
            g_applyConfiguredGeometry = true;
        }

        c::main_color.Value =
            paletteColor(PaletteSlot::accent);
        c::second_color.Value =
            paletteColor(PaletteSlot::secondary);
        c::window_bg_color.Value =
            paletteColor(PaletteSlot::windowBackground);
        c::background_color.Value =
            paletteColor(PaletteSlot::widgetBackground);
        c::stroke_color.Value =
            paletteColor(PaletteSlot::stroke);
        c::separator =
            paletteColor(PaletteSlot::separator);
        c::anim::active.Value =
            paletteColor(PaletteSlot::animationActive);
        c::anim::default_color.Value =
            paletteColor(PaletteSlot::animationDefault);
        c::bg::background =
            paletteColor(PaletteSlot::mainBackground);
        c::child::background =
            paletteColor(PaletteSlot::childBackground);
        c::child::stroke =
            paletteColor(PaletteSlot::childStroke);
        c::checkbox::mark =
            paletteColor(PaletteSlot::checkboxMark);
        c::page::background_active =
            paletteColor(PaletteSlot::pageActive);
        c::page::background =
            paletteColor(PaletteSlot::pageBackground);
        c::page::text_hov =
            paletteColor(PaletteSlot::pageHoverText);
        c::page::text =
            paletteColor(PaletteSlot::pageText);
        c::elements::background_hovered =
            paletteColor(PaletteSlot::elementHovered);
        c::elements::background =
            paletteColor(PaletteSlot::elementBackground);
        c::text::label::active.Value =
            paletteColor(PaletteSlot::labelActive);
        c::text::label::hovered.Value =
            paletteColor(PaletteSlot::labelHovered);
        c::text::label::default_color.Value =
            paletteColor(PaletteSlot::labelDefault);
        c::text::description::active =
            paletteColor(PaletteSlot::descriptionActive);
        c::text::description::hovered =
            paletteColor(PaletteSlot::descriptionHovered);
        c::text::description::default_color =
            paletteColor(PaletteSlot::descriptionDefault);
        c::text::text_active =
            paletteColor(PaletteSlot::textActive);
        c::text::text_hov =
            paletteColor(PaletteSlot::textHovered);
        c::text::text =
            paletteColor(PaletteSlot::textDefault);
        SynchronizeImGuiPalette();
        g_settingsRevision.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    std::uint64_t SettingsRevision() noexcept
    {
        return g_settingsRevision.load(
            std::memory_order_relaxed);
    }

    void Render(
        const char* surfaceDescription,
        HWND overlayWindow,
        CaptureController& capture,
        BombTimerController& bombTimer,
        ConfigManager& configManager,
        MakcuController& makcu,
        TestClickController& testClick,
        TestMoveController& testMove,
        UpdateController& updates,
        ID3D11ShaderResourceView* logoTexture)
    {
        if (!g_visible)
        {
            return;
        }

        c::anim::animation_speed = ImGui::GetIO().DeltaTime * 12.0F;
        SynchronizeImGuiPalette();

        const ImVec2 preferredSize =
            g_compactMode
            ? ImVec2(760.0F, 540.0F)
            : ImVec2(850.0F, 596.0F);
        if (g_applyConfiguredGeometry)
        {
            ImGui::SetNextWindowSize(
                g_menuSize,
                ImGuiCond_Always);
            ImGui::SetNextWindowPos(
                g_menuPosition,
                ImGuiCond_Always);
            g_applyConfiguredGeometry = false;
            g_lastCompactMode =
                g_compactMode;
        }
        else if (g_compactMode != g_lastCompactMode)
        {
            ImGui::SetNextWindowSize(
                preferredSize,
                ImGuiCond_Always);
            g_lastCompactMode = g_compactMode;
        }
        else
        {
            ImGui::SetNextWindowSize(
                preferredSize,
                ImGuiCond_FirstUseEver);
        }
        const ImVec2 maximumSize =
            MaximumMenuSize(overlayWindow);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(760.0F, 540.0F),
            maximumSize);
        if (g_menuSize.x <= 0.0F ||
            g_menuSize.y <= 0.0F)
        {
            ImGui::SetNextWindowPos(
                InitialMenuPosition(overlayWindow),
                ImGuiCond_FirstUseEver);
        }

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar;

        const int previousPage = g_page;
        const ImVec2 previousMenuPosition =
            g_menuPosition;
        const ImVec2 previousMenuSize =
            g_menuSize;
        if (!ImGui::Begin("##vanta-overlay-menu", &g_visible, flags))
        {
            ImGui::End();
            return;
        }

        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowPosition = ClampWindowToMonitor(
            overlayWindow,
            ImGui::GetWindowPos(),
            windowSize);
        ImGui::SetWindowPos(windowPosition, ImGuiCond_Always);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            windowPosition,
            windowPosition + windowSize,
            ImGui::GetColorU32(c::window_bg_color.Value),
            c::bg::rounding);
        drawList->AddRectFilled(
            windowPosition,
            windowPosition + ImVec2(windowSize.x, 70.0F),
            ImGui::GetColorU32(c::child::background),
            15.0F,
            ImDrawFlags_RoundCornersTop);

        const ImVec2 logoMinimum =
            windowPosition + ImVec2(20.0F, 16.0F);
        const ImVec2 logoMaximum =
            logoMinimum + ImVec2(38.0F, 38.0F);
        if (logoTexture != nullptr)
        {
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(logoTexture),
                logoMinimum,
                logoMaximum);
            drawList->AddRect(
                logoMinimum,
                logoMaximum,
                ImGui::GetColorU32(
                    ImVec4(
                        c::main_color.Value.x,
                        c::main_color.Value.y,
                        c::main_color.Value.z,
                        0.32F)),
                4.0F);
        }
        else
        {
            drawList->AddText(
                logoMinimum + ImVec2(11.0F, 8.0F),
                ImGui::GetColorU32(c::main_color.Value),
                "V");
        }

        if (font::bold_font != nullptr)
        {
            ImGui::PushFont(font::bold_font);
        }
        drawList->AddText(
            windowPosition + ImVec2(68.0F, 22.0F),
            ImGui::GetColorU32(
                c::text::label::active.Value),
            "VANTA");
        if (font::bold_font != nullptr)
        {
            ImGui::PopFont();
        }

        ImGui::SetCursorPos(ImVec2(20.0F, 85.0F));
        ImGui::PushStyleVar(
            ImGuiStyleVar_ItemSpacing,
            ImVec2(14.0F, 14.0F));
        if (ImGui::BeginChild(
                "##vanta-navigation",
                ImVec2(
                    160.0F,
                    std::max(
                        180.0F,
                        windowSize.y - 125.0F)),
                false,
                ImGuiWindowFlags_NoScrollbar))
        {
            if (font::regular_m != nullptr)
            {
                ImGui::PushFont(font::regular_m);
            }
            ImGui::TextColored(
                c::text::label::active.Value,
                "WORKSPACE");
            if (font::regular_m != nullptr)
            {
                ImGui::PopFont();
            }
            if (custom::Tab(
                    ICON_HOME_4_LINE "  Overlay",
                    &g_page,
                    0))
            {
                g_page = 0;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_VIDEO_CAMERA_LINE "  Capture",
                    &g_page,
                    1))
            {
                g_page = 1;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_BOMB_LINE "  Bomb Timer",
                    &g_page,
                    7))
            {
                g_page = 7;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_USB_LINE "  Mouse Output",
                    &g_page,
                    3))
            {
                g_page = 3;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_AIMING_LINE "  TestClick",
                    &g_page,
                    4))
            {
                g_page = 4;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_AIMING_LINE "  TestMove",
                    &g_page,
                    5))
            {
                g_page = 5;
                page_is_changing = false;
            }
            ImGui::Spacing();
            if (font::regular_m != nullptr)
            {
                ImGui::PushFont(font::regular_m);
            }
            ImGui::TextColored(
                c::text::label::active.Value,
                "PREFERENCES");
            if (font::regular_m != nullptr)
            {
                ImGui::PopFont();
            }
            if (custom::Tab(
                    ICON_PALETTE_LINE "  Style",
                    &g_page,
                    2))
            {
                g_page = 2;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_FILE_LINE "  Configs",
                    &g_page,
                    6))
            {
                g_page = 6;
                page_is_changing = false;
            }
            if (custom::Tab(
                    ICON_DOWNLOAD_2_LINE "  Updates",
                    &g_page,
                    8))
            {
                g_page = 8;
                page_is_changing = false;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SetCursorPos(ImVec2(200.0F, 85.0F));
        if (g_page == 0)
        {
            const float bodyHeight = std::max(
                190.0F,
                ImGui::GetContentRegionAvail().y - 40.0F);
            custom::Child(
                ICON_MONITOR_LINE
                    "  Renderer status##renderer-status",
                ImVec2(0.0F, bodyHeight),
                true);
            ImGui::TextColored(
                ImVec4(0.45F, 0.96F, 0.65F, 1.0F),
                "Transparent DirectComposition overlay is active");
            ImGui::TextWrapped("%s", surfaceDescription);
            ImGui::Spacing();
            bool settingsChanged = false;
            settingsChanged |= custom::Checkbox(
                "Enable notifications",
                &g_notifications);
            settingsChanged |= custom::Checkbox(
                "Compact menu",
                &g_compactMode);
            settingsChanged |= custom::SliderFloat(
                "Window opacity",
                &c::window_bg_color.Value.w,
                0.10F,
                1.0F,
                "%.2f");
            if (settingsChanged)
            {
                g_settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            if (custom::Button("Write a console test event", ImVec2(240.0F, 38.0F)))
            {
                vanta::log::Info(
                    "pretty-menu test: notifications=%s opacity=%.2f",
                    g_notifications ? "on" : "off",
                    c::window_bg_color.Value.w);
            }
            custom::EndChild();
        }
        else if (g_page == 1)
        {
            capture.RenderPanel();
        }
        else if (g_page == 2)
        {
            const ImVec2 available =
                ImGui::GetContentRegionAvail();
            const float bodyHeight = std::max(
                190.0F,
                available.y - 40.0F);
            const float gap =
                ImGui::GetStyle().ItemSpacing.x * 3.0F;
            const float columnWidth =
                std::max(
                    190.0F,
                    (available.x - gap) * 0.5F);

            custom::Child(
                ICON_COLOR_PICKER_LINE
                    "  Core palette##core-palette",
                ImVec2(columnWidth, bodyHeight),
                true);
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));
            bool paletteChanged = false;
            paletteChanged |= PaletteColor(
                "Accent", c::main_color);
            paletteChanged |= PaletteColor(
                "Secondary", c::second_color);
            paletteChanged |= PaletteColor(
                "Window background",
                c::window_bg_color);
            paletteChanged |= PaletteColor(
                "Widget background",
                c::background_color);
            paletteChanged |= PaletteColor(
                "Stroke", c::stroke_color);
            paletteChanged |= PaletteColor(
                "Separator", c::separator);
            paletteChanged |= PaletteColor(
                "Animation active",
                c::anim::active);
            paletteChanged |= PaletteColor(
                "Animation default",
                c::anim::default_color);
            paletteChanged |= PaletteColor(
                "Main background",
                c::bg::background);
            paletteChanged |= PaletteColor(
                "Child background",
                c::child::background);
            paletteChanged |= PaletteColor(
                "Child stroke",
                c::child::stroke);
            paletteChanged |= PaletteColor(
                "Checkbox mark",
                c::checkbox::mark);
            ImGui::PopStyleVar();
            custom::EndChild();

            ImGui::SameLine(0.0F, gap);
            custom::Child(
                ICON_PALETTE_LINE
                    "  Surface + text##surface-text-palette",
                ImVec2(columnWidth, bodyHeight),
                true);
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2(12.0F, 12.0F));
            paletteChanged |= PaletteColor(
                "Page active",
                c::page::background_active);
            paletteChanged |= PaletteColor(
                "Page background",
                c::page::background);
            paletteChanged |= PaletteColor(
                "Page hover text",
                c::page::text_hov);
            paletteChanged |= PaletteColor(
                "Page text", c::page::text);
            paletteChanged |= PaletteColor(
                "Element hovered",
                c::elements::background_hovered);
            paletteChanged |= PaletteColor(
                "Element background",
                c::elements::background);
            paletteChanged |= PaletteColor(
                "Label active",
                c::text::label::active);
            paletteChanged |= PaletteColor(
                "Label hovered",
                c::text::label::hovered);
            paletteChanged |= PaletteColor(
                "Label default",
                c::text::label::default_color);
            paletteChanged |= PaletteColor(
                "Description active",
                c::text::description::active);
            paletteChanged |= PaletteColor(
                "Description hovered",
                c::text::description::hovered);
            paletteChanged |= PaletteColor(
                "Description default",
                c::text::description::default_color);
            paletteChanged |= PaletteColor(
                "Text active",
                c::text::text_active);
            paletteChanged |= PaletteColor(
                "Text hovered",
                c::text::text_hov);
            paletteChanged |= PaletteColor(
                "Text default", c::text::text);
            ImGui::PopStyleVar();
            custom::EndChild();
            if (paletteChanged)
            {
                g_settingsRevision.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }
        else if (g_page == 4)
        {
            testClick.RenderPanel();
        }
        else if (g_page == 5)
        {
            testMove.RenderPanel();
        }
        else if (g_page == 6)
        {
            configManager.RenderPanel(
                overlayWindow,
                testClick,
                testMove);
        }
        else if (g_page == 7)
        {
            bombTimer.RenderPanel();
        }
        else if (g_page == 8)
        {
            updates.RenderPanel();
        }
        else
        {
            makcu.RenderPanel();
        }

        drawList->AddText(
            windowPosition +
                ImVec2(15.0F, windowSize.y - 30.0F),
            ImGui::GetColorU32(
                c::text::label::default_color.Value),
            "Insert: hide | End: exit");

        g_menuPosition = ImGui::GetWindowPos();
        g_menuSize = ImGui::GetWindowSize();
        if (g_page != previousPage ||
            g_menuPosition.x !=
                previousMenuPosition.x ||
            g_menuPosition.y !=
                previousMenuPosition.y ||
            g_menuSize.x != previousMenuSize.x ||
            g_menuSize.y != previousMenuSize.y)
        {
            g_settingsRevision.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        ImGui::End();
    }

    void Toggle()
    {
        g_visible = !g_visible;
        vanta::log::Info("menu visibility: %s", g_visible ? "shown" : "hidden");
    }

    bool IsVisible() noexcept
    {
        return g_visible;
    }

    bool ContainsPoint(float x, float y) noexcept
    {
        return g_visible &&
            x >= g_menuPosition.x &&
            y >= g_menuPosition.y &&
            x < g_menuPosition.x + g_menuSize.x &&
            y < g_menuPosition.y + g_menuSize.y;
    }

    bool GetBounds(
        float& x,
        float& y,
        float& width,
        float& height) noexcept
    {
        x = g_menuPosition.x;
        y = g_menuPosition.y;
        width = g_menuSize.x;
        height = g_menuSize.y;
        return g_visible && width > 0.0F && height > 0.0F;
    }
}
