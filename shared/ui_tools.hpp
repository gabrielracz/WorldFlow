#ifndef UI_TOOLS_HPP_
#define UI_TOOLS_HPP_

#include "imgui.h"
#include "imgui_impl_sdl2.h"
namespace uitools {
#include <SDL.h>
#include <imgui.h>

ImGuiKey SDLKeyToImGuiKey(SDL_Keycode key);
void CollapseAllWindows();
void SetAmberRedTheme();
void SetDarkRedTheme();
void SetTheme(ImVec4 accent, ImVec4 bg, float windowalpha);

}
#endif