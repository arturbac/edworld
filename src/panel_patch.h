// edworld — the right superpower emblem over the jump panel's wrong one, drawn onto the panel's surface.
#pragma once

#include "runtime.h"

namespace edworld
  {
  ///\brief called at a watched panel draw, before the game's draw goes on; draws at most once a frame, only
  /// while a jump charges to a Federation, Empire or Alliance system (or always while charging in test mode),
  /// and only when the draw samples the configured surface
  auto panel_patch(ID3D11DeviceContext * ctx, ID3D11Device * device, std::uint64_t frame) noexcept -> void;

  ///\brief called right after the game's watched panel draw: when it is the jump panel's and the list was made this
  /// frame, the list is drawn under the panel as a quad of its own in the panel's plane
  auto panel_list_after(ID3D11DeviceContext * ctx, std::uint64_t frame, std::uint32_t index_count, std::uint32_t start_index,
                        std::int32_t base_vertex, std::uint32_t start_instance) noexcept -> void;
  }  // namespace edworld
