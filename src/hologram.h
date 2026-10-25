// edworld — the target's hologram (PoC): what the observer (hooks.cc) read of it last, for the right sphere.
#pragma once

#include "hologram_math.h"
#include "holo_models.h"

#include <array>

namespace edworld
  {
  struct hologram_view_t
    {
    bool have;  ///< read less than half a second ago
    std::array<compass_direction_t, 3> axes;  ///< the model's X, Y, Z in the hologram camera's frame (compass frame)
    std::uint32_t count;  ///< the chosen draw's indices on the running build: the same hologram from frame to frame
    std::uint32_t table;  ///< its model's count in the registry and the tables (holo_table_count); 0 = none on this build
    bool ship;            ///< the model is a ship (holo_shows_ship): never the pads
    holo_place_e place;   ///< its model's kind of place (holo_place_of): whether a grant draws the pad's plane, and how
    holo_flat_t flat;     ///< its model's thinnest axis (holo_flat_of): the pads' up at a place on a planet
    };

  ///\brief the hologram's axes as last read (render thread)
  auto hologram_view() noexcept -> hologram_view_t;
  }  // namespace edworld
