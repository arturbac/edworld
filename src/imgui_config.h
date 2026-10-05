// edworld — Dear ImGui build options (IMGUI_USER_CONFIG). ImGui draws only into the game's interface surface: no
// windows of its own, no input, no files (no imgui.ini in the game's folder), and a failed check is logged, never
// an abort of the game.
#pragma once

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_FILE_FUNCTIONS

namespace edworld
  {
  auto imgui_check_failed(char const * what, char const * file, int line) noexcept -> void;
  }  // namespace edworld

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ::edworld::imgui_check_failed(#_EXPR, __FILE__, __LINE__))
