// edworld — the hologram models' registry (PoC, 2026-10-22): which model a hologram draw is, and the same model again
// after an update of the game; pure arithmetic, no D3D, testable anywhere.
//
// Within one build of the game a hologram model keeps its index count, so the count tells the model (the T1 table, the
// target's surface). An update may renumber it, turn it, or leave it as it was. Each model keeps a fingerprint of its
// shape - counts, the bounding box's proportions, the vertices furthest along each axis and an 8x8x8 occupancy of the
// box - that does not depend on the order of its vertices and indices. A count the registry does not know is matched by
// its fingerprint against the models', in each of the 24 turns by quarter turns: the best match over the threshold,
// clear of every other model, takes the model's place, with the turn that carries the model's axes (the T1 table's)
// onto the new ones. Entries are measured (from flights and logs) until Artur confirms the hologram on a picture.
#pragma once

#include "panel_math.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace edworld
  {
  enum struct holo_role_e : std::uint8_t
    {
    ship,    ///< a ship: the own hologram, or the target's when a ship is targeted
    station,  ///< a station, port, outpost or site: only ever the target's
    other     ///< a model confirmed in holo_db's form that is no ship: known by its count and fingerprint, no order rule
    };

  enum struct holo_status_e : std::uint8_t
    {
    measured,  ///< from flights and logs; not yet seen by Artur on a picture of the hologram
    confirmed  ///< Artur saw the picture of the hologram and named the model
    };

  ///\brief the kind of place a model shows, from its role and type in holo_db's form (D, Artur 2026-10-24): whether the
  /// target's hologram after a grant holds pads, and which rule gives their plane; edworld alone has no StationType
  enum struct holo_place_e : std::uint8_t
    {
    none,                 ///< a ship, a body, a pad, an installation without pads, any other model, or no model known
    outpost_t1,           ///< an orbital outpost (T1): its pads' plane and the nose from the T1 table only
    big_station,          ///< a Coriolis, Orbis, Ocellus, asteroid base: the hologram stays the station's, no pads
    planet,               ///< a surface port, a settlement or a planetary construction site: pads flat on the ground
    carrier,              ///< a fleet or squadron carrier, Operations' small carrier: the pads' planes table
    construction_orbital  ///< the orbital construction site: no plane yet
    };

  [[nodiscard]]
  constexpr auto holo_place_name(holo_place_e place) noexcept -> char const *
    {
    switch(place)
      {
      case holo_place_e::outpost_t1:           return "an orbital outpost (T1)";
      case holo_place_e::big_station:          return "a big station";
      case holo_place_e::planet:               return "a place on a planet";
      case holo_place_e::carrier:              return "a carrier";
      case holo_place_e::construction_orbital: return "an orbital construction site";
      case holo_place_e::none:                 break;
      }
    return "no place with pads";
    }

  ///\brief whether a grant at this kind of place draws the pad's plane
  [[nodiscard]]
  constexpr auto holo_place_has_plane(holo_place_e place) noexcept -> bool
    {
    return place == holo_place_e::outpost_t1 or place == holo_place_e::planet or place == holo_place_e::carrier;
    }

  ///\brief the kind of place the journal's StationType names, for edworld_eht's check of the model's (none: a type with
  /// no counterpart, as a megaship)
  [[nodiscard]]
  constexpr auto holo_place_of_station_type(std::string_view t) noexcept -> holo_place_e
    {
    if(t == "Outpost")
      return holo_place_e::outpost_t1;
    for(std::string_view const s: {"CraterOutpost", "CraterPort", "SurfaceStation", "OnFootSettlement", "PlanetaryConstructionDepot"})
      if(t == s)
        return holo_place_e::planet;
    if(t == "FleetCarrier")
      return holo_place_e::carrier;
    for(std::string_view const s: {"Coriolis", "Orbis", "Ocellus", "Bernal", "AsteroidBase", "Dodec"})
      if(t == s)
        return holo_place_e::big_station;
    if(t == "SpaceConstructionDepot")
      return holo_place_e::construction_orbital;
    return holo_place_e::none;
    }

  inline constexpr std::uint32_t holo_grid_side{8};

  struct holo_fingerprint_t
    {
    std::uint32_t vertices;
    std::uint32_t triangles;
    std::array<float, 3> extent;         ///< the bounding box's half-sides over the largest one
    std::array<vec3_t, 6> extreme;       ///< the vertex furthest along +X, -X, +Y, -Y, +Z, -Z, in the box's frame
    std::array<std::uint64_t, 8> grid;   ///< occupancy of the box in 8x8x8 cells: word z, bit x + 8 y
    };

  ///\brief the box's frame: centred, the largest half-side 1
  struct holo_box_t
    {
    vec3_t centre;
    float half;
    };

  namespace detail
    {
    [[nodiscard]]
    constexpr auto component(vec3_t const & v, int k) noexcept -> float
      {
      return k == 0 ? v.x : k == 1 ? v.y : v.z;
      }

    ///\brief a before b along axis k (larger first), ties broken by the other axes so that the order of vertices does not count
    [[nodiscard]]
    constexpr auto further(vec3_t const & a, vec3_t const & b, int k, float sign) noexcept -> bool
      {
      for(int i{}; i != 3; ++i)
        {
        float const x{component(a, (k + i) % 3) * (i == 0 ? sign : 1.f)};
        float const y{component(b, (k + i) % 3) * (i == 0 ? sign : 1.f)};
        if(x != y)
          return x > y;
        }
      return false;
      }

    [[nodiscard]]
    constexpr auto cell(float q) noexcept -> std::uint32_t
      {
      int const c{static_cast<int>((q + 1.f) * 0.5f * static_cast<float>(holo_grid_side))};
      return static_cast<std::uint32_t>(std::clamp(c, 0, static_cast<int>(holo_grid_side) - 1));
      }
    }  // namespace detail

  ///\brief the fingerprint of the vertices a model's indices use (each once, any order) and its triangle count
  [[nodiscard]]
  inline auto holo_fingerprint(vec3_t const * used, std::size_t n, std::uint32_t triangles) noexcept -> holo_fingerprint_t
    {
    holo_fingerprint_t f{};
    f.vertices = static_cast<std::uint32_t>(n);
    f.triangles = triangles;
    if(n == 0u)
      return f;
    vec3_t lo{used[0]}, hi{used[0]};
    for(std::size_t i{1}; i != n; ++i)
      {
      lo = {std::min(lo.x, used[i].x), std::min(lo.y, used[i].y), std::min(lo.z, used[i].z)};
      hi = {std::max(hi.x, used[i].x), std::max(hi.y, used[i].y), std::max(hi.z, used[i].z)};
      }
    vec3_t const centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    vec3_t const half{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
    float const largest{std::max({half.x, half.y, half.z})};
    float const s{largest > 0.f ? 1.f / largest : 1.f};
    f.extent = {half.x * s, half.y * s, half.z * s};
    std::array<bool, 6> have{};
    for(std::size_t i{}; i != n; ++i)
      {
      vec3_t const q{(used[i].x - centre.x) * s, (used[i].y - centre.y) * s, (used[i].z - centre.z) * s};
      for(int k{}; k != 3; ++k)
        for(int e{}; e != 2; ++e)
          {
          auto const at{static_cast<std::size_t>(2 * k + e)};
          if(not have[at] or detail::further(q, f.extreme[at], k, e == 0 ? 1.f : -1.f))
            {
            f.extreme[at] = q;
            have[at] = true;
            }
          }
      std::uint32_t const cx{detail::cell(q.x)}, cy{detail::cell(q.y)}, cz{detail::cell(q.z)};
      f.grid[cz] |= std::uint64_t{1} << (cx + holo_grid_side * cy);
      }
    return f;
    }

  ///\brief a turn by quarter turns: the new frame's axis k is the old axis perm[k], sign[k] its end
  struct holo_turn_t
    {
    std::array<std::int8_t, 3> perm;
    std::array<std::int8_t, 3> sign;
    };

  ///\brief the 24 turns (no mirror), the identity first
  [[nodiscard]]
  constexpr auto holo_turns() noexcept -> std::array<holo_turn_t, 24>
    {
    std::array<holo_turn_t, 24> out{};
    std::size_t n{};
    constexpr std::array<std::array<std::int8_t, 3>, 6> perms{{{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}}};
    for(std::size_t p{}; p != perms.size(); ++p)
      for(int bits{}; bits != 8; ++bits)
        {
        std::array<std::int8_t, 3> const sign{static_cast<std::int8_t>(bits & 1 ? -1 : 1), static_cast<std::int8_t>(bits & 2 ? -1 : 1),
                                              static_cast<std::int8_t>(bits & 4 ? -1 : 1)};
        int const parity{p < 3 ? 1 : -1};  // the first three permutations are even
        if(parity * sign[0] * sign[1] * sign[2] == 1)
          out[n++] = holo_turn_t{perms[p], sign};
        }
    return out;
    }

  inline constexpr std::array<holo_turn_t, 24> holo_turn_list{holo_turns()};

  [[nodiscard]]
  constexpr auto turn(holo_turn_t const & t, vec3_t const & v) noexcept -> vec3_t
    {
    return {t.sign[0] * detail::component(v, t.perm[0]), t.sign[1] * detail::component(v, t.perm[1]),
            t.sign[2] * detail::component(v, t.perm[2])};
    }

  ///\brief the fingerprint of the model turned by t (as if its vertices were)
  [[nodiscard]]
  inline auto turned(holo_fingerprint_t const & f, holo_turn_t const & t) noexcept -> holo_fingerprint_t
    {
    holo_fingerprint_t g{f};
    g.grid = {};
    for(std::size_t k{}; k != 3; ++k)
      {
      auto const from{static_cast<std::size_t>(t.perm[k])};
      g.extent[k] = f.extent[from];
      std::size_t const plus{2u * from + (t.sign[k] > 0 ? 0u : 1u)}, minus{2u * from + (t.sign[k] > 0 ? 1u : 0u)};
      g.extreme[2u * k] = turn(t, f.extreme[plus]);
      g.extreme[2u * k + 1u] = turn(t, f.extreme[minus]);
      }
    for(std::uint32_t z{}; z != holo_grid_side; ++z)
      for(std::uint32_t y{}; y != holo_grid_side; ++y)
        for(std::uint32_t x{}; x != holo_grid_side; ++x)
          {
          if(not (f.grid[z] >> (x + holo_grid_side * y) & 1u))
            continue;
          std::array<std::uint32_t, 3> const c{x, y, z};
          std::array<std::uint32_t, 3> n{};
          for(std::size_t k{}; k != 3; ++k)
            {
            std::uint32_t const v{c[static_cast<std::size_t>(t.perm[k])]};
            n[k] = t.sign[k] > 0 ? v : holo_grid_side - 1u - v;
            }
          g.grid[n[2]] |= std::uint64_t{1} << (n[0] + holo_grid_side * n[1]);
          }
    return g;
    }

  ///\brief how alike two fingerprints are as they stand, 0..1: the cells shared, the box's proportions, the extreme
  /// vertices' places and the counts
  [[nodiscard]]
  inline auto holo_likeness(holo_fingerprint_t const & a, holo_fingerprint_t const & b) noexcept -> float
    {
    int both{}, either{};
    for(std::size_t i{}; i != a.grid.size(); ++i)
      {
      both += std::popcount(a.grid[i] & b.grid[i]);
      either += std::popcount(a.grid[i] | b.grid[i]);
      }
    float const cells{either ? static_cast<float>(both) / static_cast<float>(either) : 0.f};
    float box{};
    for(std::size_t k{}; k != 3; ++k)
      box += std::fabs(a.extent[k] - b.extent[k]);
    box = std::max(0.f, 1.f - box / 3.f);
    float ends{};
    for(std::size_t i{}; i != 6; ++i)
      {
      vec3_t const d{a.extreme[i].x - b.extreme[i].x, a.extreme[i].y - b.extreme[i].y, a.extreme[i].z - b.extreme[i].z};
      ends += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      }
    ends = std::max(0.f, 1.f - ends / 6.f / 2.f);
    auto const ratio = [](std::uint32_t x, std::uint32_t y) noexcept -> float
      { return std::max(x, y) ? static_cast<float>(std::min(x, y)) / static_cast<float>(std::max(x, y)) : 1.f; };
    float const counts{(ratio(a.vertices, b.vertices) + ratio(a.triangles, b.triangles)) * 0.5f};
    return 0.4f * cells + 0.2f * box + 0.2f * ends + 0.2f * counts;
    }

  struct holo_model_t
    {
    char const * name;
    holo_role_e role;
    std::uint32_t count;     ///< the draw's indices in `build`
    holo_status_e status;
    char const * build;      ///< the game's build the count was read on (holo_build_key); "" = before builds were logged
    bool has_fingerprint;
    holo_fingerprint_t fingerprint;
    holo_place_e place{holo_place_e::none};
    };

  // The build every count below was read on: EliteDangerous64.exe of 103482368 bytes written 2026-10-20 11:30:58Z,
  // the game's version 2026.10.13.334473 (VersionInfo.txt copied with it that day; the next build came on 2026-10-23).
  inline constexpr char holo_build_2026_10_13[]{"103482368-01dd60867a106e8a"};

  // The fingerprints read on that build (edworld_eht log "hologram model fingerprint", 2026-10-22 and 23, Sjona's and
  // Dunkan's logs alike, each the same at every reading).
  inline constexpr holo_fingerprint_t holo_fp_panther{
    65068u, 35610u, {0.7987f, 0.2291f, 1.0000f},
    {{{0.7987f, -0.0675f, 0.0894f}, {-0.7987f, -0.0675f, 0.0894f}, {0.5649f, 0.2291f, -0.5846f},
     {0.0097f, -0.2291f, 0.1458f}, {0.0000f, -0.0469f, 1.0000f}, {0.5292f, 0.1708f, -1.0000f}}},
    {{0x0000006624000000ull, 0x0000007e7e000000ull, 0x0000007e7e000000ull, 0x0000007eff000000ull,
     0x000000ffff000000ull, 0x0000007eff000000ull, 0x0000007eff000000ull, 0x000000183c000000ull}}};
  inline constexpr holo_fingerprint_t holo_fp_construction_site{
    4906u, 4361u, {1.0000f, 0.3316f, 0.9699f},
    {{{1.0000f, -0.1074f, 0.5385f}, {-1.0000f, -0.2529f, 0.1813f}, {-0.0430f, 0.3316f, -0.7010f},
     {0.5828f, -0.3316f, -0.1654f}, {0.4768f, -0.1741f, 0.9699f}, {0.2102f, -0.2022f, -0.9699f}}},
    {{0x0000081818180000ull, 0x0000081818180000ull, 0x0000000007770000ull, 0x00000000f7f70000ull,
     0x000000c0f7c70000ull, 0x000000c0ff9f0000ull, 0x00000000f88c0000ull, 0x00000000b8f80000ull}}};
  inline constexpr holo_fingerprint_t holo_fp_kestrel{
    10249u, 9295u, {0.8093f, 0.1836f, 1.0000f},
    {{{0.8093f, -0.1787f, -0.0096f}, {-0.8093f, -0.1782f, 0.0150f}, {-0.0549f, 0.1836f, 0.0904f},
     {-0.8038f, -0.1836f, -0.0626f}, {-0.0160f, -0.0032f, 1.0000f}, {-0.1805f, 0.0673f, -1.0000f}}},
    {{0x0000007e00000000ull, 0x0000007e18000000ull, 0x0000007e3c000000ull, 0x0000003cff000000ull,
     0x0000003cff000000ull, 0x0000003c3c000000ull, 0x0000001818000000ull, 0x0000001818000000ull}}};
  inline constexpr holo_fingerprint_t holo_fp_t1_commercial_station{
    2158u, 1192u, {0.9294f, 1.0000f, 0.9242f},
    {{{0.9294f, 0.1291f, 0.0010f}, {-0.9294f, 0.0300f, -0.0180f}, {-0.1584f, 1.0000f, 0.1084f},
     {0.8086f, -1.0000f, 0.0145f}, {0.4907f, 0.0755f, 0.9242f}, {0.0034f, 0.2662f, -0.9242f}}},
    {{0x0000187838000000ull, 0x0000180818000000ull, 0x0c001c7838000000ull, 0xc408c4cdadc00080ull,
     0x8c008cedfdc000c0ull, 0x0000002820000000ull, 0x0000002020000000ull, 0x0000002020000000ull}}};
  inline constexpr holo_fingerprint_t holo_fp_sidewinder{
    1968u, 1308u, {1.0000f, 0.2466f, 0.6289f},
    {{{1.0000f, -0.0551f, -0.5662f}, {-1.0000f, -0.0551f, -0.5662f}, {0.0000f, 0.2466f, -0.5714f},
     {0.2960f, -0.2466f, -0.6073f}, {0.2541f, -0.0267f, 0.6289f}, {0.1338f, 0.0769f, -0.6289f}}},
    {{0x0000000000000000ull, 0x000000ffff000000ull, 0x000000bda5000000ull, 0x00000099bd000000ull,
     0x0000003ce7000000ull, 0x0000007e7e000000ull, 0x0000003c7e000000ull, 0x0000000000000000ull}}};
  inline constexpr holo_fingerprint_t holo_fp_crater_outpost{
    3351u, 1532u, {0.0136f, 0.9800f, 1.0000f},
    {{{0.0136f, -0.5320f, -0.5834f}, {-0.0136f, 0.3119f, 0.2697f}, {-0.0040f, 0.9800f, 0.0576f},
     {-0.0133f, -0.9800f, -0.3571f}, {-0.0036f, -0.1068f, 1.0000f}, {-0.0042f, -0.4140f, -1.0000f}}},
    {{0x0000000000181800ull, 0x1818000000181818ull, 0x1818000000181818ull, 0x1818080000001818ull,
     0x1810180000081818ull, 0x0818181800001818ull, 0x0000181818081818ull, 0x0000001818181818ull}}};
  inline constexpr holo_fingerprint_t holo_fp_squadron_carrier{
    56565u, 28323u, {0.5145f, 0.2834f, 1.0000f},
    {{{0.5145f, -0.1042f, -0.1431f}, {-0.5145f, -0.1042f, -0.1431f}, {-0.0133f, 0.2834f, -0.6193f},
     {0.0000f, -0.2834f, -0.5271f}, {0.0085f, 0.0543f, 1.0000f}, {0.0191f, 0.0922f, -1.0000f}}},
    {{0x0000001818000000ull, 0x0000183c3c180000ull, 0x0000003c7e000000ull, 0x0000003c7e000000ull,
     0x0000003c3c000000ull, 0x0000003c18000000ull, 0x0000001818000000ull, 0x0000001818000000ull}}};
  inline constexpr holo_fingerprint_t holo_fp_coriolis{
    642u, 316u, {1.0000f, 1.0000f, 0.9932f},
    {{{1.0000f, 0.2370f, -0.0157f}, {-1.0000f, 0.2370f, -0.0157f}, {0.2370f, 1.0000f, -0.0157f},
     {0.2370f, -1.0000f, -0.0157f}, {0.9555f, 0.0000f, 0.9932f}, {0.2285f, -0.0577f, -0.9932f}}},
    {{0x1800009999000018ull, 0x1000008100000010ull, 0x0000000000000000ull, 0x9900008181000099ull,
     0x8100000000000081ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x1800008181000018ull}}};

  // The models known on 2026-10-22, all measured (none confirmed on a picture yet), each with the build its count was
  // read on: on another build a count is the model's only when its fingerprint says so, so a model without one is
  // unknown there until read again. Stations' counts: the T1 table (hologram_math.h), the Coriolis after a grant, a
  // commercial outpost's station before one, a construction site; ships: the own holograms. A station's kind of place
  // (holo_place_e) as its role in holo_db's form gives it; the T1 commercial station, seen without a target, none.
  inline constexpr holo_model_t holo_models_measured[]{
    {"Kestrel", holo_role_e::ship, 27885u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_kestrel},
    {"Panther", holo_role_e::ship, 106830u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_panther},
    {"T1 military (Fukuda Town)", holo_role_e::station, 5238u, holo_status_e::measured, holo_build_2026_10_13, false, {}, holo_place_e::outpost_t1},
    {"T1 commercial pads (Brescia Platform)", holo_role_e::station, 1953u, holo_status_e::measured, holo_build_2026_10_13, false, {}, holo_place_e::outpost_t1},
    {"T1 scientific (Archer Base)", holo_role_e::station, 4404u, holo_status_e::measured, holo_build_2026_10_13, false, {}, holo_place_e::outpost_t1},
    {"T1 industrial (Coppel City)", holo_role_e::station, 4431u, holo_status_e::measured, holo_build_2026_10_13, false, {}, holo_place_e::outpost_t1},
    {"T1 pirate (Fearnley Horizons)", holo_role_e::station, 7023u, holo_status_e::measured, holo_build_2026_10_13, false, {}, holo_place_e::outpost_t1},
    {"T1 civilian (Marth's Inheritance)", holo_role_e::station, 5478u, holo_status_e::measured, holo_build_2026_10_13, false, {}, holo_place_e::outpost_t1},
    {"T1 commercial station", holo_role_e::station, 3576u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_t1_commercial_station},
    {"crater outpost pads (Brewer)", holo_role_e::station, 4596u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_crater_outpost, holo_place_e::planet},
    {"Coriolis", holo_role_e::station, 948u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_coriolis, holo_place_e::big_station},
    {"planetary construction site", holo_role_e::station, 13083u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_construction_site, holo_place_e::planet},
    {"squadron carrier", holo_role_e::station, 84969u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_squadron_carrier, holo_place_e::carrier},
    {"Sidewinder", holo_role_e::ship, 3924u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_sidewinder},
  };
  }  // namespace edworld

#include "holo_models_generated.h"

namespace edworld
  {
  ///\brief two tables of models as one
  template<std::size_t a, std::size_t b>
  [[nodiscard]]
  constexpr auto holo_join(holo_model_t const (&first)[a], holo_model_t const (&second)[b]) noexcept
    -> std::array<holo_model_t, a + b>
    {
    std::array<holo_model_t, a + b> out{};
    for(std::size_t i{}; i != a; ++i)
      out[i] = first[i];
    for(std::size_t i{}; i != b; ++i)
      out[a + i] = second[i];
    return out;
    }

  // The registry: the measured models above, then the ones confirmed in holo_db's form on the builds they were seen on
  // (holo_models_generated.h, holo_db.py models-header), so a model Artur confirmed after an update is known on that build by
  // its count and elsewhere by its fingerprint. A count on two builds, or the same model on two, is two entries; the
  // first that fits wins.
  inline constexpr auto holo_models{holo_join(holo_models_measured, holo_models_confirmed)};

  enum struct holo_resolve_e : std::uint8_t
    {
    unknown,    ///< no model: not in the registry, nor a fingerprint over the threshold clear of the others
    known,      ///< the count of a model read on this build (or before builds were logged)
    stable,     ///< a model's count on another build, the fingerprint unchanged
    remapped,   ///< another count, the fingerprint a model's (maybe turned)
    ambiguous   ///< the fingerprint fits a model in more than one turn (a symmetric model), none the identity
    };

  struct holo_resolved_t
    {
    holo_resolve_e how{holo_resolve_e::unknown};
    int model{-1};    ///< into holo_models
    int turn{0};      ///< into holo_turn_list: the model's axes onto the drawn ones
    float score{};    ///< the fingerprint's likeness to the model's (0 without fingerprints)
    float second{};   ///< the best likeness of any other model
    };

  inline constexpr float holo_match_min{0.9f};
  inline constexpr float holo_match_clear{0.05f};
  inline constexpr float holo_turn_tie{0.02f};

  ///\brief which model a draw of `count` indices is, on the game's `build`, with its fingerprint when read (or null)
  [[nodiscard]]
  inline auto holo_resolve(holo_model_t const * models, std::size_t n, std::uint32_t count, char const * build,
                           holo_fingerprint_t const * f) noexcept -> holo_resolved_t
    {
    for(std::size_t i{}; i != n; ++i)
      {
      holo_model_t const & m{models[i]};
      if(m.count != count)
        continue;
      bool const same_build{m.build[0] == '\0' or std::strcmp(m.build, build) == 0};
      float const score{f and m.has_fingerprint ? holo_likeness(*f, m.fingerprint) : 0.f};
      if(same_build)
        return holo_resolved_t{holo_resolve_e::known, static_cast<int>(i), 0, score, 0.f};
      if(score >= holo_match_min)
        return holo_resolved_t{holo_resolve_e::stable, static_cast<int>(i), 0, score, 0.f};
      }
    if(not f)
      return holo_resolved_t{};
    holo_resolved_t best{};
    float best_identity{};
    bool turned_tie{};
    for(std::size_t i{}; i != n; ++i)
      {
      holo_model_t const & m{models[i]};
      if(not m.has_fingerprint)
        continue;
      float model_best{-1.f}, identity{};
      int model_turn{};
      std::array<float, 24> scores{};
      for(std::size_t t{}; t != holo_turn_list.size(); ++t)
        {
        scores[t] = holo_likeness(*f, turned(m.fingerprint, holo_turn_list[t]));
        if(t == 0u)
          identity = scores[t];
        if(scores[t] > model_best)
          {
          model_best = scores[t];
          model_turn = static_cast<int>(t);
          }
        }
      if(model_best > best.score)
        {
        best.score = model_best;
        best.model = static_cast<int>(i);
        best.turn = model_turn;
        best_identity = identity;
        turned_tie = false;
        for(std::size_t t{1}; t != holo_turn_list.size(); ++t)
          if(static_cast<int>(t) != model_turn and scores[t] >= model_best - holo_turn_tie)
            turned_tie = true;
        }
      }
    // the rivals leave out the best model's own shape: the same model on another build (confirmed in the form after a
    // measured entry, 2026-10-24) is no rival
    if(best.model >= 0)
      for(std::size_t i{}; i != n; ++i)
        {
        holo_model_t const & m{models[i]};
        if(static_cast<int>(i) == best.model or not m.has_fingerprint
           or holo_likeness(m.fingerprint, models[best.model].fingerprint) >= holo_match_min)
          continue;
        for(holo_turn_t const & t: holo_turn_list)
          best.second = std::max(best.second, holo_likeness(*f, turned(m.fingerprint, t)));
        }
    if(best.model < 0 or best.score < holo_match_min or best.score - best.second < holo_match_clear)
      return holo_resolved_t{holo_resolve_e::unknown, -1, 0, best.score, best.second};
    // a symmetric model fits several turns: the identity when it is among the best, else no sure axes
    if(best_identity >= best.score - holo_turn_tie)
      best.turn = 0;
    else if(turned_tie)
      {
      best.how = holo_resolve_e::ambiguous;
      return best;
      }
    best.how = holo_resolve_e::remapped;
    return best;
    }

  ///\brief the registered model's axes (as the T1 table names them) from the drawn model's, for a model drawn turned by t
  /// (the drawn axis k is the model's axis perm[k], sign[k] its end)
  template<typename axis_t>
  [[nodiscard]]
  constexpr auto holo_model_axes(std::array<axis_t, 3> const & drawn, holo_turn_t const & t) noexcept -> std::array<axis_t, 3>
    {
    std::array<axis_t, 3> out{drawn};
    for(std::size_t k{}; k != 3; ++k)
      {
      axis_t a{drawn[k]};
      if(t.sign[k] < 0)
        a = axis_t{-a.x, -a.y, -a.z};
      out[static_cast<std::size_t>(t.perm[k])] = a;
      }
    return out;
    }

  ///\brief the chosen draw shows a ship: its count is drawn to the own hologram's surface in the same burst too (near a
  /// carrier's pad the target's hologram is the own ship, 2026-10-22), or the model it resolved to (null: none) is a ship;
  /// a model of a place is never a ship, though it comes on the own surface (after a ship swap, 2760 at Ahn 2026-10-25)
  [[nodiscard]]
  inline auto holo_shows_ship(std::uint32_t count, std::uint32_t const * counts, bool const * own, std::uint32_t n,
                              holo_model_t const * model) noexcept -> bool
    {
    if(model and model->place != holo_place_e::none)
      return false;
    for(std::uint32_t i{}; i != n; ++i)
      if(own[i] and counts[i] == count)
        return true;
    return model and model->role == holo_role_e::ship;
    }

  ///\brief the kind of place of a resolved draw's model, whether its axes are sure or not (ambiguous); none for a draw
  /// that resolved to no model
  [[nodiscard]]
  inline auto holo_place_of(holo_resolved_t const & r, holo_model_t const * models) noexcept -> holo_place_e
    {
    return r.model >= 0 and r.how != holo_resolve_e::unknown ? models[r.model].place : holo_place_e::none;
    }

  ///\brief a model's thinnest axis: the least half-side of its bounding box (0 X, 1 Y, 2 Z; -1 without a fingerprint) and
  /// its ratio to the next least - a place on a planet is a slab of pads (E, 2026-10-24: 13083 Y 0.33/0.97, 4596 X
  /// 0.01/0.79, 2208 X 0.01/0.34)
  struct holo_flat_t
    {
    std::int8_t axis{-1};
    float ratio{1.f};
    };

  [[nodiscard]]
  constexpr auto holo_flat_of(std::array<float, 3> const & extent) noexcept -> holo_flat_t
    {
    int least{};
    for(int k{1}; k != 3; ++k)
      if(extent[static_cast<std::size_t>(k)] < extent[static_cast<std::size_t>(least)])
        least = k;
    float const next{std::min(extent[static_cast<std::size_t>((least + 1) % 3)], extent[static_cast<std::size_t>((least + 2) % 3)])};
    return holo_flat_t{static_cast<std::int8_t>(least), next > 0.f ? extent[static_cast<std::size_t>(least)] / next : 1.f};
    }

  ///\brief the thinnest axis of a resolved draw's model in the axes the view gives (the model's, turned back when remapped);
  /// none when the axes are not sure (ambiguous), the draw no model, or the model without a fingerprint
  [[nodiscard]]
  inline auto holo_flat_of(holo_resolved_t const & r, holo_model_t const * models) noexcept -> holo_flat_t
    {
    bool const sure{r.how == holo_resolve_e::known or r.how == holo_resolve_e::stable or r.how == holo_resolve_e::remapped};
    if(not sure or r.model < 0 or not models[r.model].has_fingerprint)
      return holo_flat_t{};
    return holo_flat_of(models[r.model].fingerprint.extent);
    }

  ///\brief the most a place on a planet's thinnest half-side may be of the next least: thicker, no slab of pads
  inline constexpr float holo_flat_ratio_max{0.5f};

  ///\brief the model's count on the build the tables (T1, the pads' planes) were read on, for a resolved draw; 0 when the
  /// draw is no model this build is known to share with them (unknown, ambiguous): a count the tables hold may be another
  /// model's after an update
  [[nodiscard]]
  inline auto holo_table_count(holo_resolved_t const & r, holo_model_t const * models) noexcept -> std::uint32_t
    {
    switch(r.how)
      {
      case holo_resolve_e::known:
      case holo_resolve_e::stable:
      case holo_resolve_e::remapped:
        return models[r.model].count;
      case holo_resolve_e::unknown:
      case holo_resolve_e::ambiguous:
        break;
      }
    return 0u;
    }

  ///\brief the vertices a draw's indices use, each once, decoded (stride vertex_stride, position in the first 12 bytes);
  /// false when an index lies past the vertices read
  [[nodiscard]]
  inline auto holo_used_vertices(std::uint8_t const * indices, std::uint32_t index_size, std::uint32_t count,
                                 std::uint8_t const * vertices, std::uint32_t vertex_bytes, std::vector<vec3_t> & out) -> bool
    {
    std::uint32_t const have{vertex_bytes / vertex_stride};
    std::vector<bool> seen(have);
    out.clear();
    for(std::uint32_t i{}; i != count; ++i)
      {
      std::uint32_t at{};
      if(index_size == 2u)
        {
        std::uint16_t v{};
        std::memcpy(&v, indices + i * 2u, 2);
        at = v;
        }
      else
        std::memcpy(&at, indices + i * 4u, 4);
      if(at >= have)
        return false;
      if(seen[at])
        continue;
      seen[at] = true;
      std::uint32_t w[3]{};
      std::memcpy(w, vertices + static_cast<std::size_t>(at) * vertex_stride, sizeof w);
      out.push_back(decode_local(w[0], w[1], w[2]));
      }
    return true;
    }
  }  // namespace edworld
