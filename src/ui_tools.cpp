#include "ui_tools.hpp"
#include <SDL.h>
#include <imgui.h>
namespace uitools {

ImGuiKey SDLKeyToImGuiKey(SDL_Keycode key)
{
    switch (key)
    {
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
        case SDLK_RCTRL: return ImGuiKey_RightCtrl;
        case SDLK_LSHIFT: return ImGuiKey_LeftShift;
        case SDLK_RSHIFT: return ImGuiKey_RightShift;
        case SDLK_LALT: return ImGuiKey_LeftAlt;
        case SDLK_RALT: return ImGuiKey_RightAlt;
        case SDLK_LGUI: return ImGuiKey_LeftSuper;
        case SDLK_RGUI: return ImGuiKey_RightSuper;
        case SDLK_DELETE: return ImGuiKey_Delete;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_PAGEUP: return ImGuiKey_PageUp;
        case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
        case SDLK_INSERT: return ImGuiKey_Insert;

        // Map alphanumeric keys
        case SDLK_a: return ImGuiKey_A;
        case SDLK_b: return ImGuiKey_B;
        case SDLK_c: return ImGuiKey_C;
        case SDLK_d: return ImGuiKey_D;
        case SDLK_e: return ImGuiKey_E;
        case SDLK_f: return ImGuiKey_F;
        case SDLK_g: return ImGuiKey_G;
        case SDLK_h: return ImGuiKey_H;
        case SDLK_i: return ImGuiKey_I;
        case SDLK_j: return ImGuiKey_J;
        case SDLK_k: return ImGuiKey_K;
        case SDLK_l: return ImGuiKey_L;
        case SDLK_m: return ImGuiKey_M;
        case SDLK_n: return ImGuiKey_N;
        case SDLK_o: return ImGuiKey_O;
        case SDLK_p: return ImGuiKey_P;
        case SDLK_q: return ImGuiKey_Q;
        case SDLK_r: return ImGuiKey_R;
        case SDLK_s: return ImGuiKey_S;
        case SDLK_t: return ImGuiKey_T;
        case SDLK_u: return ImGuiKey_U;
        case SDLK_v: return ImGuiKey_V;
        case SDLK_w: return ImGuiKey_W;
        case SDLK_x: return ImGuiKey_X;
        case SDLK_y: return ImGuiKey_Y;
        case SDLK_z: return ImGuiKey_Z;

        // Map number keys (top row)
        case SDLK_0: return ImGuiKey_0;
        case SDLK_1: return ImGuiKey_1;
        case SDLK_2: return ImGuiKey_2;
        case SDLK_3: return ImGuiKey_3;
        case SDLK_4: return ImGuiKey_4;
        case SDLK_5: return ImGuiKey_5;
        case SDLK_6: return ImGuiKey_6;
        case SDLK_7: return ImGuiKey_7;
        case SDLK_8: return ImGuiKey_8;
        case SDLK_9: return ImGuiKey_9;

        // Function keys
        case SDLK_F1: return ImGuiKey_F1;
        case SDLK_F2: return ImGuiKey_F2;
        case SDLK_F3: return ImGuiKey_F3;
        case SDLK_F4: return ImGuiKey_F4;
        case SDLK_F5: return ImGuiKey_F5;
        case SDLK_F6: return ImGuiKey_F6;
        case SDLK_F7: return ImGuiKey_F7;
        case SDLK_F8: return ImGuiKey_F8;
        case SDLK_F9: return ImGuiKey_F9;
        case SDLK_F10: return ImGuiKey_F10;
        case SDLK_F11: return ImGuiKey_F11;
        case SDLK_F12: return ImGuiKey_F12;

        default: return ImGuiKey_None;
    }
}


void CollapseAllWindows() {
    // for (int i = 0; i < ImGui::GetIO().WantSaveIniSettings; i++) { // Iterate based on WantSaveIniSettings (a hack but works)
    //     ImGuiWindow* window = ImGui::FindWindowByID(ImGui::GetID(ImGui::GetIO().IniSavingObjects[i])); //Correct way to access the windows.
    //     if (window && !(window->Flags & ImGuiWindowFlags_NoCollapse) && !(window->Flags & ImGuiWindowFlags_ChildWindow)) {
    //         window->Collapsed = true;
    //     }
    // }
}

void SetAmberRedTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Main
    colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.10f, 0.02f, 0.02f, 0.94f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.12f, 0.02f, 0.02f, 0.94f);
    colors[ImGuiCol_Border]                 = ImVec4(1.00f, 0.20f, 0.00f, 0.65f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    // Headers
    colors[ImGuiCol_FrameBg]                = ImVec4(0.62f, 0.08f, 0.08f, 0.54f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.95f, 0.20f, 0.20f, 0.40f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(1.00f, 0.14f, 0.14f, 0.67f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.02f, 0.02f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.75f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    
    // Tabs
    colors[ImGuiCol_Tab]                    = ImVec4(0.62f, 0.08f, 0.08f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(1.00f, 0.20f, 0.20f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.85f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.45f, 0.06f, 0.06f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.62f, 0.08f, 0.08f, 1.00f);
    
    // Menu
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.02f, 0.02f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    
    // Buttons
    colors[ImGuiCol_Button]                 = ImVec4(0.62f, 0.08f, 0.08f, 0.54f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(1.00f, 0.20f, 0.20f, 0.40f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(1.00f, 0.14f, 0.14f, 0.67f);
    
    // Checkmarks
    colors[ImGuiCol_CheckMark]              = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);
    
    // Sliders
    colors[ImGuiCol_SliderGrab]             = ImVec4(1.00f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);
    
    // Headers
    colors[ImGuiCol_Header]                 = ImVec4(0.62f, 0.08f, 0.08f, 0.54f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(1.00f, 0.20f, 0.20f, 0.65f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(1.00f, 0.14f, 0.14f, 0.00f);
    
    // Separators
    colors[ImGuiCol_Separator]              = ImVec4(1.00f, 0.20f, 0.00f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(1.00f, 0.24f, 0.24f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(1.00f, 0.24f, 0.24f, 1.00f);
    
    // Resize Grips
    colors[ImGuiCol_ResizeGrip]             = ImVec4(1.00f, 0.20f, 0.20f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(1.00f, 0.20f, 0.20f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(1.00f, 0.20f, 0.20f, 0.95f);
    
    // Plot colors
    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(1.00f, 0.14f, 0.14f, 0.65f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.24f, 0.24f, 1.00f);
    
    // Table colors
    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.62f, 0.08f, 0.08f, 0.54f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
    
    // Text Selection
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(1.00f, 0.20f, 0.20f, 0.35f);
    
    // Drag Drop
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 0.20f, 0.20f, 0.90f);
    
    // Navigation
    colors[ImGuiCol_NavHighlight]           = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    
    // Modal
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    // Style tweaks for neon effect
    style.FrameBorderSize = 1.0f;           // Add borders to frames
    style.WindowBorderSize = 1.0f;          // Consistent border size
    style.PopupBorderSize = 1.0f;           // Consistent border size
    style.TabBorderSize = 1.0f;             // Add borders to tabs
    style.WindowRounding = 4.0f;            // Slight rounding on windows
    style.FrameRounding = 4.0f;             // Matching frame rounding
    style.PopupRounding = 4.0f;             // Matching popup rounding
    style.ScrollbarRounding = 4.0f;         // Matching scrollbar rounding
    style.GrabRounding = 4.0f;              // Matching grab rounding
    style.TabRounding = 4.0f;               // Matching tab rounding
}

void SetDarkRedTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    float t = 0.5;

    // Main - Pure black backgrounds with subtle red tint in specific places
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.00f, 0.00f, 0.00f, t);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.05f, 0.00f, 0.00f, t);
    colors[ImGuiCol_Border]                 = ImVec4(0.85f, 0.10f, 0.10f, t * 1.5f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    // Headers - Deeper red for normal state, brighter for interaction
    colors[ImGuiCol_FrameBg]                = ImVec4(0.45f, 0.05f, 0.05f, t);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.85f, 0.10f, 0.10f, t);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.85f, 0.10f, 0.10f, t);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.05f, 0.00f, 0.00f, t);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.00, 0.00f, 0.00f,  t);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, t);
    
    // Tabs - Consistent with the deeper red theme
    colors[ImGuiCol_Tab]                    = ImVec4(0.45f, 0.05f, 0.05f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.85f, 0.10f, 0.10f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.65f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.28f, 0.03f, 0.03f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.45f, 0.05f, 0.05f, 1.00f);
    
    // Menu - Keeping mostly black
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.05f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.45f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.65f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    
    // Buttons - Using the core red shade
    colors[ImGuiCol_Button]                 = ImVec4(0.45f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.85f, 0.10f, 0.10f, 0.40f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.85f, 0.10f, 0.10f, 0.67f);
    
    // Interactive Elements - Brighter red for visibility
    colors[ImGuiCol_CheckMark]              = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    
    // Headers - Matching the button scheme
    colors[ImGuiCol_Header]                 = ImVec4(0.45f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.85f, 0.10f, 0.10f, 0.65f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.85f, 0.10f, 0.10f, 0.00f);
    
    // Separators - Subtle red tint
    colors[ImGuiCol_Separator]              = ImVec4(0.85f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.85f, 0.10f, 0.10f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    
    // Resize Grips - More subtle than before
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.85f, 0.10f, 0.10f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.85f, 0.10f, 0.10f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.85f, 0.10f, 0.10f, 0.95f);
    
    // Plot colors
    colors[ImGuiCol_PlotLines]              = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.85f, 0.10f, 0.10f, 0.65f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    
    // Table colors
    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.45f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.45f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
    
    // Selection
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.85f, 0.10f, 0.10f, 0.35f);
    
    // Drag Drop
    colors[ImGuiCol_DragDropTarget]         = ImVec4(0.85f, 0.10f, 0.10f, 0.90f);
    
    // Navigation
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.85f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    
    // Modal
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

    // Style
    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.WindowRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
}

void SetTheme(ImVec4 accent, ImVec4 bg, float windowalpha)
{
    ImGuiStyle& style = ImGui::GetStyle();
    // Derive colors
    const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 black(0.0f, 0.0f, 0.0f, 1.0f);
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    
    // Lighten/darken helpers
    auto lighten = [](const ImVec4& col, float factor) {
        return ImVec4(
            1.0f - (1.0f - col.x) * factor,
            1.0f - (1.0f - col.y) * factor,
            1.0f - (1.0f - col.z) * factor,
            col.w
        );
    };
    
    auto darken = [](const ImVec4& col, float factor) {
        return ImVec4(
            col.x * factor,
            col.y * factor,
            col.z * factor,
            col.w
        );
    };

    // Keep ImGui's default style parameters
    style.Alpha                     = 1.0f;
    style.DisabledAlpha            = 0.6f;
    style.WindowPadding            = ImVec2(8, 8);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32, 32);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    style.FramePadding            = ImVec2(4, 3);
    style.FrameRounding           = 0.0f;
    style.FrameBorderSize         = 0.0f;
    style.ItemSpacing             = ImVec2(8, 4);
    style.ItemInnerSpacing        = ImVec2(4, 4);
    style.IndentSpacing           = 21.0f;
    style.ScrollbarSize           = 14.0f;
    style.ScrollbarRounding       = 0.0f;
    style.GrabMinSize             = 10.0f;
    style.GrabRounding            = 0.0f;
    style.TabRounding             = 0.0f;
    style.TabBorderSize           = 0.0f;
    style.ColorButtonPosition     = ImGuiDir_Right;

    // Colors
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = white;
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.0f, 0.0f, 0.0f, windowalpha);
    colors[ImGuiCol_ChildBg]                = transparent;
    colors[ImGuiCol_PopupBg]                = ImVec4(0.0f, 0.0f, 0.0f, 0.94f);
    colors[ImGuiCol_Border]                 = accent;
    colors[ImGuiCol_BorderShadow]           = transparent;
    colors[ImGuiCol_FrameBg]                = bg;
    colors[ImGuiCol_FrameBgHovered]         = lighten(bg, 0.8f);
    colors[ImGuiCol_FrameBgActive]          = lighten(bg, 0.6f);
    colors[ImGuiCol_TitleBg]                = darken(bg, 0.8f);
    colors[ImGuiCol_TitleBgActive]          = darken(bg, 0.6f);
    colors[ImGuiCol_TitleBgCollapsed]       = darken(bg, 0.6f);
    colors[ImGuiCol_MenuBarBg]              = darken(bg, 0.6f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = bg;
    colors[ImGuiCol_ScrollbarGrabHovered]   = lighten(bg, 0.8f);
    colors[ImGuiCol_ScrollbarGrabActive]    = lighten(bg, 0.6f);
    colors[ImGuiCol_CheckMark]              = accent;
    colors[ImGuiCol_SliderGrab]             = accent;
    colors[ImGuiCol_SliderGrabActive]       = lighten(accent, 0.8f);
    colors[ImGuiCol_Button]                 = bg;
    colors[ImGuiCol_ButtonHovered]          = lighten(bg, 0.8f);
    colors[ImGuiCol_ButtonActive]           = lighten(bg, 0.6f);
    colors[ImGuiCol_Header]                 = bg;
    colors[ImGuiCol_HeaderHovered]          = lighten(bg, 0.8f);
    colors[ImGuiCol_HeaderActive]           = lighten(bg, 0.6f);
    colors[ImGuiCol_Separator]              = accent;
    colors[ImGuiCol_SeparatorHovered]       = lighten(accent, 0.8f);
    colors[ImGuiCol_SeparatorActive]        = lighten(accent, 0.6f);
    colors[ImGuiCol_ResizeGrip]             = bg;
    colors[ImGuiCol_ResizeGripHovered]      = lighten(bg, 0.8f);
    colors[ImGuiCol_ResizeGripActive]       = lighten(bg, 0.6f);
    colors[ImGuiCol_Tab]                    = darken(bg, 0.8f);
    colors[ImGuiCol_TabHovered]             = bg;
    colors[ImGuiCol_TabActive]              = bg;
    colors[ImGuiCol_TabUnfocused]           = darken(bg, 0.9f);
    colors[ImGuiCol_TabUnfocusedActive]     = darken(bg, 0.8f);
    colors[ImGuiCol_PlotLines]              = accent;
    colors[ImGuiCol_PlotLinesHovered]       = lighten(accent, 0.8f);
    colors[ImGuiCol_PlotHistogram]          = accent;
    colors[ImGuiCol_PlotHistogramHovered]   = lighten(accent, 0.8f);
    colors[ImGuiCol_TableHeaderBg]          = darken(bg, 0.8f);
    colors[ImGuiCol_TableBorderStrong]      = accent;
    colors[ImGuiCol_TableBorderLight]       = darken(accent, 0.7f);
    colors[ImGuiCol_TableRowBg]             = transparent;
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = accent;
    colors[ImGuiCol_NavHighlight]           = accent;
    colors[ImGuiCol_NavWindowingHighlight]  = white;
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.58f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.0f, 0.0f, 0.0f, 0.58f);
}
}