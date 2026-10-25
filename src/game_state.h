// edworld — what the panel patch needs to know about the game, read from its own files and from EDSM.
#pragma once

#include "faction_list.h"

#include <cstdint>
#include <string>

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
    ///\brief Status.json Flags2 (bit 12: gliding, checked 2026-10-08 in EHT's recorded Status: set in normal space with an altitude)
    std::uint64_t flags2;
    bool has_position;
    double latitude, longitude, altitude, planet_radius, heading;
    ///\brief degrees, + climbing, from the last two fixes that differed; NaN until there are two
    double path_angle;
    ///\brief metres a second, + climbing, from the last two fixes that differed (by the host's clock); NaN until there are two
    double vertical_speed;
    ///\brief the destination's body (Destination.Body; 0 = none or the system) and its name as Status.json gives it
    std::uint32_t destination_body;
    char destination_name[64];
    ///\brief where the destination stands on its body, from the data source (edworld_eht) while it names the same
    /// body and place; false without one
    bool has_place;
    double place_latitude, place_longitude;
    ///\brief the pad the tower gave (PoC), from the data source (edworld_eht): when (unix ms, 0 = no pad given now), its
    /// number and the station's type as the journal names it (Coriolis, Outpost, CraterOutpost...)
    std::int64_t docking_granted_ms;
    std::uint32_t docking_pad;
    char station_type[32];
    };

  ///\brief the pad the HUD's notice gave (render thread, pad_text_read.h) and when (unix ms); 0 = none. edworld alone takes
  /// it into flight() for the data source's; edworld_eht only compares the two
  auto pad_text_grant(std::uint32_t pad, std::int64_t granted_unix_ms) noexcept -> void;

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

  ///\brief edworld_eht: a hologram model's sample in its context (sample_context.h), the game's files read now; for the
  /// dumps' writer thread, not the render thread. Throws only on no memory.
  [[nodiscard]]
  auto sample_context(char const * sample) -> std::string;
  }  // namespace edworld
