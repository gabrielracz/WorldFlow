#ifndef UI_TOOLS_HPP_
#define UI_TOOLS_HPP_

#include "imgui.h"
#include "imgui_impl_sdl2.h"
namespace uitools {
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
    colors[ImGuiCol_Border]                 = ImVec4(0.85f, 0.10f, 0.10f, t * 1.5);
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
}
#endif