// edworld — the planets whose atmosphere edworld has read, a few at once; pure, no D3D, testable anywhere.
//
// The shell's reads come from whichever planet the game draws first after the period (2026-11-17 audit, C2): with two
// atmospheres in view (the twin moons 7 d and 7 e of Spoihee IT-F d12-1113), one slot for "the planet" lost the near
// one's kind to the other's read, or switched between them. Each planet (told by its radius) keeps its own kind; the
// clouds, the snow and the aurora take the nearest planet's (distance over radius at its last read) among those read
// lately. A read of no known kind does not undo a known one (C3): a planet's kind changes only after three reads in a
// row agree on another.
#pragma once

#include "atmosphere_kind.h"

#include <cmath>
#include <cstdint>

namespace edworld
  {
  struct atmosphere_planet_t
    {
    float radius{};  ///< metres; 0 = an empty slot
    atmosphere_kind_e kind{atmosphere_kind_e::unknown};
    bool volcanic{};  ///< neon's second print
    bool cold{};      ///< ammonia's second print
    std::uint64_t kind_since_ms{};  ///< when the kind was taken (the clouds fade it in from there)
    float closeness{};              ///< distance over radius at the last read
    std::uint64_t seen_ms{};        ///< the last read
    atmosphere_kind_e other{atmosphere_kind_e::unknown};  ///< a kind read against the one kept
    bool other_volcanic{};
    bool other_cold{};
    std::uint32_t other_reads{};  ///< reads in a row of `other`
    bool unknown_told{};          ///< a read of no known kind said since the last read of its kind
    };

  struct atmosphere_planets_t
    {
    static constexpr std::size_t size{8};
    static constexpr float same_radius_m{4.f};
    static constexpr std::uint32_t reads_to_change{3};
    static constexpr std::uint64_t recent_ms{60000};  ///< a planet read longer ago is not "in view" any more
    atmosphere_planet_t slot[size]{};
    };

  ///\brief what a read did: the slot of its planet, and whether that planet is new or its kind changed (to be logged)
  struct atmosphere_note_t
    {
    std::size_t index{};
    bool fresh{};
    bool changed{};
    bool held{};  ///< a read of another kind (or none) that did not change the kind kept
    bool first_unknown{};  ///< held by a read of no known kind, the first since a read of its kind
    };

  ///\brief the slot of the planet of this radius; none when it is not there
  [[nodiscard]]
  constexpr auto atmosphere_planet_find(atmosphere_planets_t const & p, float radius) noexcept -> atmosphere_planet_t const *
    {
    for(atmosphere_planet_t const & s: p.slot)
      if(s.radius > 0.f and std::fabs(s.radius - radius) < atmosphere_planets_t::same_radius_m)
        return &s;
    return nullptr;
    }

  ///\brief a read of the planet of `radius` at `closeness` (distance / radius) gave `kind` (unknown when none matched)
  constexpr auto atmosphere_planet_note(atmosphere_planets_t & p, float radius, float closeness, atmosphere_kind_e kind,
                                        bool volcanic, bool cold, std::uint64_t now_ms) noexcept -> atmosphere_note_t
    {
    std::size_t at{atmosphere_planets_t::size};
    for(std::size_t i{}; i != atmosphere_planets_t::size; ++i)
      if(p.slot[i].radius > 0.f and std::fabs(p.slot[i].radius - radius) < atmosphere_planets_t::same_radius_m)
        at = i;
    atmosphere_note_t note{};
    if(at == atmosphere_planets_t::size)
      {
      // a new planet takes an empty slot, else the one read longest ago; its kind is the read's
      at = 0;
      for(std::size_t i{}; i != atmosphere_planets_t::size; ++i)
        if(p.slot[i].radius <= 0.f)
          {
          at = i;
          break;
          }
        else if(p.slot[i].seen_ms < p.slot[at].seen_ms)
          at = i;
      p.slot[at] = atmosphere_planet_t{.radius = radius, .kind = kind, .volcanic = volcanic, .cold = cold, .kind_since_ms = now_ms};
      note.fresh = true;
      }
    atmosphere_planet_t & s{p.slot[at]};
    s.closeness = closeness;
    s.seen_ms = now_ms;
    note.index = at;
    if(note.fresh)
      return note;
    bool const same{kind == s.kind and volcanic == s.volcanic and cold == s.cold};
    if(same)
      {
      s.other_reads = 0;
      s.unknown_told = false;
      return note;
      }
    // an unknown planet takes the first kind read; a known one only what three reads in a row agree on
    if(s.kind == atmosphere_kind_e::unknown and kind != atmosphere_kind_e::unknown)
      {
      s.kind = kind;
      s.volcanic = volcanic;
      s.cold = cold;
      s.kind_since_ms = now_ms;
      s.other_reads = 0;
      note.changed = true;
      return note;
      }
    // a read of no known kind never takes a known kind away, however many come (as the header and developers.md say;
    // three in a row did before the second audit, 2026-11-18); it does not break a run of another kind either
    if(kind == atmosphere_kind_e::unknown)
      {
      note.held = true;
      note.first_unknown = not s.unknown_told;  // said once until a read of its kind again
      s.unknown_told = true;
      return note;
      }
    if(s.other_reads != 0 and kind == s.other and volcanic == s.other_volcanic and cold == s.other_cold)
      ++s.other_reads;
    else
      {
      s.other = kind;
      s.other_volcanic = volcanic;
      s.other_cold = cold;
      s.other_reads = 1;
      }
    if(s.other_reads >= atmosphere_planets_t::reads_to_change)
      {
      s.kind = kind;
      s.volcanic = volcanic;
      s.cold = cold;
      s.kind_since_ms = now_ms;
      s.other_reads = 0;
      note.changed = true;
      }
    else
      note.held = true;
    return note;
    }

  ///\brief the planet the clouds, the snow and the aurora are for: the nearest (by distance over radius) of those read
  /// in the last minute; none when none was
  [[nodiscard]]
  constexpr auto atmosphere_planet_nearest(atmosphere_planets_t const & p, std::uint64_t now_ms) noexcept -> atmosphere_planet_t const *
    {
    atmosphere_planet_t const * best{};
    for(atmosphere_planet_t const & s: p.slot)
      if(s.radius > 0.f and now_ms - s.seen_ms <= atmosphere_planets_t::recent_ms and (not best or s.closeness < best->closeness))
        best = &s;
    return best;
    }
  }  // namespace edworld
