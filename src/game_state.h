// edworld — what the panel patch needs to know about the game, read from its own files and from EDSM.
#pragma once

#include "faction_list.h"

#include <cstdint>

namespace edworld
  {
  enum struct allegiance_e : std::uint8_t
    {
    unknown,
    federation,
    empire,
    alliance,
    independent,
    other
    };

  struct game_state_t
    {
    ///\brief the hyperspace jump is being charged (Status.json Flags2 bit 19)
    bool charging;
    ///\brief the jump's destination (Status.json Destination.System), 0 when none
    std::uint64_t destination;
    ///\brief the destination's allegiance, from EDSM's controlling faction; unknown until it answered
    allegiance_e allegiance;
    };

  ///\brief what Status.json says of the flight near a planet, for the compass (PoC): the fields are there in orbital
  /// cruise, gliding, flying and landed; has_position false elsewhere
  struct flight_t
    {
    std::uint64_t flags;
    bool has_position;
    double latitude, longitude, altitude, planet_radius, heading;
    ///\brief degrees, + climbing, from the last two fixes that differed; NaN until there are two
    double path_angle;
    ///\brief the destination's body (Destination.Body; 0 = none or the system) and its name as Status.json gives it
    std::uint32_t destination_body;
    char destination_name[64];
    };

  [[nodiscard]]
  auto flight() noexcept -> flight_t;

  ///\brief starts the thread that polls Status.json and asks EDSM about a new destination; once
  auto start_game_state() noexcept -> void;

  [[nodiscard]]
  auto game_state() noexcept -> game_state_t;

  ///\brief the destination's factions for the list under the panel: from the data source when it has them
  /// (edworld_eht), else from EDSM; source none until one answered
  [[nodiscard]]
  auto destination_factions() noexcept -> faction_list_t;

  [[nodiscard]]
  auto allegiance_name(allegiance_e a) noexcept -> char const *;
  }  // namespace edworld
