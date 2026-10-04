// edworld — the right superpower emblem over the jump panel's wrong one, drawn onto the panel's surface.
#pragma once

#include "runtime.h"

namespace edworld
  {
  ///\brief called at a watched panel draw, before the game's draw goes on; draws at most once a frame, only
  /// while a jump charges to a Federation, Empire or Alliance system (or always while charging in test mode),
  /// and only when the draw samples the configured surface
  auto panel_patch(ID3D11DeviceContext * ctx, ID3D11Device * device, std::uint64_t frame) noexcept -> void;
  }  // namespace edworld
