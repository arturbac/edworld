// edworld — the target's hologram (PoC): the orientation the game draws its model with, as directions in the ship's
// frame; pure arithmetic, no D3D, testable anywhere.
//
// Read off the game on 2026-10-20 (vertex shader 5B0068AF5630F96B, disassembled): the model's vertex v goes to
// q v q* scaled by the record's scale, plus the record's position less the world-rebase origin (cb1 row 275), then
// through cb1 rows 270..273 (one row per coordinate, row 273 the translation) to clip space. The record is the
// 336-byte one the panels have (VB0 entry -> index -> VS t33), q as 4 x unorm16 at byte 8, each u / 32767 - 1.
#pragma once

#include "compass_math.h"
#include "panel_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace edworld
  {
  struct quat_t
    {
    float x, y, z, w;
    };

  ///\brief the record's orientation as the game's shader decodes it (the constant is 1/32767 in its bytecode)
  [[nodiscard]]
  inline auto hologram_orientation(std::uint8_t const * record) noexcept -> quat_t
    {
    std::uint16_t u[4]{};
    std::memcpy(u, record + 8, sizeof u);
    auto const f = [](std::uint16_t v) noexcept -> float { return static_cast<float>(v) / 32767.f - 1.f; };
    return quat_t{f(u[0]), f(u[1]), f(u[2]), f(u[3])};
    }

  [[nodiscard]]
  constexpr auto dot(vec3_t const & a, vec3_t const & b) noexcept -> float
    {
    return a.x * b.x + a.y * b.y + a.z * b.z;
    }

  [[nodiscard]]
  constexpr auto cross(vec3_t const & a, vec3_t const & b) noexcept -> vec3_t
    {
    return vec3_t{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

  ///\brief q v q*, as the shader writes it: v (2w^2 - 1) + 2 (q.v) q + 2 w (q x v)
  [[nodiscard]]
  constexpr auto rotate(quat_t const & q, vec3_t const & v) noexcept -> vec3_t
    {
    vec3_t const u{q.x, q.y, q.z};
    float const a{2.f * q.w * q.w - 1.f}, b{2.f * dot(u, v)}, c{2.f * q.w};
    vec3_t const k{cross(u, v)};
    return vec3_t{v.x * a + u.x * b + k.x * c, v.y * a + u.y * b + k.y * c, v.z * a + u.z * b + k.z * c};
    }

  [[nodiscard]]
  inline auto normalised(vec3_t const & v) noexcept -> std::optional<vec3_t>
    {
    float const n{std::sqrt(dot(v, v))};
    if(not(n > 1e-6f))
      return std::nullopt;
    return vec3_t{v.x / n, v.y / n, v.z / n};
    }

  ///\brief the hologram camera's axes in its world: right, up and forward (into the picture)
  struct camera_axes_t
    {
    vec3_t right, up, forward;
    };

  ///\brief the camera of cb1 rows 270..273 (clip = x row270 + y row271 + z row272 + row273): clip x, y, z and w are each
  /// a column; forward is the w column for a perspective (w grows with depth), the z column for an orthographic one (w
  /// constant). Right and up are made square to forward; none when the rows are degenerate.
  [[nodiscard]]
  inline auto camera_axes(float const (&rows)[4][4]) noexcept -> std::optional<camera_axes_t>
    {
    auto const column = [&](int c) noexcept -> vec3_t { return vec3_t{rows[0][c], rows[1][c], rows[2][c]}; };
    vec3_t const cw{column(3)}, cz{column(2)};
    bool const perspective{dot(cw, cw) > 1e-8f * std::max(1e-12f, dot(cz, cz))};
    std::optional<vec3_t> const forward{normalised(perspective ? cw : cz)};
    if(not forward)
      return std::nullopt;
    auto const square = [&](vec3_t const & v) noexcept -> std::optional<vec3_t>
      {
      float const along{dot(v, *forward)};
      return normalised(vec3_t{v.x - along * forward->x, v.y - along * forward->y, v.z - along * forward->z});
      };
    std::optional<vec3_t> const right{square(column(0))}, up{square(column(1))};
    if(not right or not up)
      return std::nullopt;
    return camera_axes_t{*right, *up, *forward};
    }

  ///\brief the model's axes X, Y, Z as the hologram shows them: rotated by q, then in the camera's frame (x right, y up,
  /// z forward: the compass's frame, the hologram's camera taken for the ship)
  [[nodiscard]]
  inline auto hologram_axes(quat_t const & q, camera_axes_t const & cam) noexcept -> std::array<compass_direction_t, 3>
    {
    std::array<compass_direction_t, 3> out{};
    vec3_t const unit[3]{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    for(int i{}; i != 3; ++i)
      {
      vec3_t const a{rotate(q, unit[i])};
      out[i] = compass_direction_t{dot(cam.right, a), dot(cam.up, a), dot(cam.forward, a)};
      }
    return out;
    }

  // ---- the pad's plane (PoC, 2026-10-21) ----
  // After the tower gives a pad at an outpost, a surface port or a settlement, the hologram's model is the pads (Brewer
  // Site: 4596 indices, Brescia Platform: 1953). Its X stands square to the pads and Y, Z lie in their plane: X was the
  // ship's up on the pad at Brewer and its down at Brescia, so its sign follows the kind of port - a plane needs none.
  // A Coriolis keeps the station's model (948 indices, the mailslot on Z) and has no pads on its hologram. Rules of Y at
  // a settlement (27885 indices) and a construction site (106830) were measured on the ship's own hologram (a Kestrel,
  // a Panther; 2026-10-22), not the target's, and are gone. Whether the hologram shows pads is its model's kind of place
  // (holo_place_e, holo_models.h; 2026-10-24), no more the journal's StationType.

  // ---- the orbital outposts' pads (T1), 2026-10-21 ----
  // At an outpost in space the ship may come from under the station, so a vote on the pads' up from the direction to the
  // target goes wrong (Fukuda Town, Brescia Platform). Each kind of outpost has its own hologram model, told by its index
  // count, with two to five pads in fixed places: the pads' up and the ship's nose on each pad, in the model's axes, as
  // measured on the pad (the model's axis along the ship's up a second before Docked) or told by Artur relative to a
  // docked ship, each told twice and the two put together (2026-10-21).
  struct t1_pad_t
    {
    std::uint32_t model;  ///< the hologram model's index count
    std::uint32_t pad;
    std::int8_t up_axis, up_sign;      ///< the pads' up: the model's axis (0 X, 1 Y, 2 Z) and its end
    std::int8_t nose_axis, nose_sign;  ///< the ship's nose on the pad, the same way (for the approach's course, later)
    };

  inline constexpr t1_pad_t t1_pads[]{
    // Military (Fukuda Town): the three side by side
    {5238u, 1u, 0, 1, 1, 1}, {5238u, 2u, 0, 1, 1, 1}, {5238u, 3u, 0, 1, 1, 1},
    // Commercial (Brescia Platform): S 1-3 side by side; M 4 on the other side, turned 90 degrees
    {1953u, 1u, 0, 1, 1, 1}, {1953u, 2u, 0, 1, 1, 1}, {1953u, 3u, 0, 1, 1, 1}, {1953u, 4u, 0, -1, 2, -1},
    // Scientific (Archer Base): S 1-2 side by side; M 3 on the other side (a barrel roll)
    {4404u, 1u, 1, 1, 0, 1}, {4404u, 2u, 1, 1, 0, 1}, {4404u, 3u, 1, -1, 0, 1},
    // Industrial (Coppel City): S 1 and 2 on opposite sides; M 3 square to them
    {4431u, 1u, 0, 1, 2, -1}, {4431u, 2u, 0, -1, 2, -1}, {4431u, 3u, 1, -1, 0, -1},
    // Pirate (Fearnley Horizons): S 1 and 2 in one plane, 90 degrees apart; M 3 square to them
    {7023u, 1u, 0, 1, 1, 1}, {7023u, 2u, 0, 1, 2, 1}, {7023u, 3u, 1, -1, 0, 1},
    // Civilian (Marth's Inheritance): S 1-2 one side, S 3-4 and M 5 the other
    {5478u, 1u, 1, 1, 2, -1}, {5478u, 2u, 1, 1, 2, 1}, {5478u, 3u, 1, -1, 0, -1}, {5478u, 4u, 1, -1, 0, 1}, {5478u, 5u, 1, -1, 2, 1},
  };

  ///\brief the pad's own mark `< |||` (as painted on the pads), in the pad's plane: segments from (a, b) to (c, d), a and c
  /// along the ship's nose on the pad, b and d across it, in the disc's radii; the head points to the nose (Artur 2026-10-21)
  struct pad_mark_segment_t
    {
    float a, b, c, d;
    };

  inline constexpr pad_mark_segment_t pad_mark[]{
    {0.55f, 0.f, 0.25f, 0.22f},  {0.55f, 0.f, 0.25f, -0.22f},  // the head <
    {0.05f, 0.2f, 0.05f, -0.2f}, {-0.15f, 0.2f, -0.15f, -0.2f}, {-0.35f, 0.2f, -0.35f, -0.2f},  // the bars |||
  };

  ///\brief the T1 table's entry for a hologram model and a pad; none for another model or pad
  [[nodiscard]]
  constexpr auto t1_pad(std::uint32_t model, std::uint32_t pad) noexcept -> t1_pad_t const *
    {
    for(t1_pad_t const & e: t1_pads)
      if(e.model == model and e.pad == pad)
        return &e;
    return nullptr;
    }

  // ---- the pads' planes of models that stay after a grant: carriers, a construction site (2026-10-22) ----
  // A carrier's or a construction site's hologram stays the model's after a grant (no pads' model) and turns to the own
  // ship's close to the pad.
  // The squadron carrier (84969 indices: the model's Z its length, Y the deck's up, X across) has its pads on planes
  // turned about Z, read off its mesh: the beam's (0), the wings' (+-24 degrees, PRNH pad 5) and the outer edges' (+-46,
  // pad 15; Artur: the edge pads lean more, the middle ones lie in the beam's plane). Which plane a pad is on is not
  // known, so the plane nearest the ship's up is taken. A players' fleet carrier has its deck and two sides (2026-10-24,
  // below), the colonisation carrier mirrored pads on its two sides (Artur 2026-10-22): its model once sampled. Approached from the stern over
  // a wing the nearest plane is the wing's, not the pad's (Artur), so the squadron carrier's pads known go by their
  // number (pad_plane_pads), the nearest plane only for the others. A planetary construction site (13083 indices) is
  // flat along Y, 35% of its faces facing +Y; the ship's up on its pad 1 was +Y: one plane, the ground's, the ship's
  // nose on it not known.
  struct pad_planes_t
    {
    std::uint32_t model;       ///< the hologram model's index count
    std::int8_t up_axis;       ///< the deck's up (0 X, 1 Y, 2 Z)
    std::int8_t about_axis;    ///< the axis the planes turn about
    std::int8_t towards_axis;  ///< a positive angle leans the up towards this axis
    std::uint8_t planes;
    std::array<float, 5> angle_deg;
    bool nose_along;           ///< the ship's nose on a pad lies along about_axis, towards its plus end
    std::array<float, 5> lean_deg{};  ///< each plane's lean after its turn, its up towards about_axis (a fleet carrier's deck)
    };

  inline constexpr pad_planes_t pad_planes[]{
    {84969u, 1, 2, 0, 5u, {0.f, 24.f, -24.f, 46.f, -46.f}, true},  // squadron carrier
    {13083u, 1, 2, 0, 1u, {0.f}, false},                           // planetary construction site
    // a players' fleet carrier (Maria Sklodowska W1V-NXM, 2026-10-24, the ship's up on pads 2, 7, 13, 11): the deck with the
    // L pads leaning 10.1 degrees along its length, the sides +-22.5 degrees across it; the nose along Z
    {73389u, 1, 2, 0, 3u, {0.f, 22.5f, -22.5f}, true, {10.1f, 0.f, 0.f}},
  };

  ///\brief a pad whose plane is known by its number
  struct pad_plane_pad_t
    {
    std::uint32_t model;
    std::uint32_t pad;
    float angle_deg;  ///< one of the model's planes
    };

  // PRNH, all 32 pads (Artur 2026-10-23, from the game): L 1-8 and 17-24 on the wings, M 11-12, 25-26 and S 15-16, 29-30
  // on the outer edges, M 9-10, 27-28 and S 13-14, 31-32 on the beam; pads 1-16 on one side, 17-32 on the other. The
  // sides by the ship's up on the pad: pad 1 +24 (2026-10-22), pad 18 -22.5 (2026-10-23)
  inline constexpr pad_plane_pad_t pad_plane_pads[]{
    {84969u, 1u, 24.f},   {84969u, 2u, 24.f},   {84969u, 3u, 24.f},   {84969u, 4u, 24.f},
    {84969u, 5u, 24.f},   {84969u, 6u, 24.f},   {84969u, 7u, 24.f},   {84969u, 8u, 24.f},
    {84969u, 9u, 0.f},    {84969u, 10u, 0.f},   {84969u, 11u, 46.f},  {84969u, 12u, 46.f},
    {84969u, 13u, 0.f},   {84969u, 14u, 0.f},   {84969u, 15u, 46.f},  {84969u, 16u, 46.f},
    {84969u, 17u, -24.f}, {84969u, 18u, -24.f}, {84969u, 19u, -24.f}, {84969u, 20u, -24.f},
    {84969u, 21u, -24.f}, {84969u, 22u, -24.f}, {84969u, 23u, -24.f}, {84969u, 24u, -24.f},
    {84969u, 25u, -46.f}, {84969u, 26u, -46.f}, {84969u, 27u, 0.f},   {84969u, 28u, 0.f},
    {84969u, 29u, -46.f}, {84969u, 30u, -46.f}, {84969u, 31u, 0.f},   {84969u, 32u, 0.f},
    // a fleet carrier (Artur 2026-10-24, from the game): L 1-8 on the deck, M 11-12 and S 14-15 on the right (+X), M 9-10
    // and S 13, 16 on the left
    {73389u, 1u, 0.f},    {73389u, 2u, 0.f},    {73389u, 3u, 0.f},    {73389u, 4u, 0.f},
    {73389u, 5u, 0.f},    {73389u, 6u, 0.f},    {73389u, 7u, 0.f},    {73389u, 8u, 0.f},
    {73389u, 9u, -22.5f}, {73389u, 10u, -22.5f}, {73389u, 11u, 22.5f}, {73389u, 12u, 22.5f},
    {73389u, 13u, -22.5f}, {73389u, 14u, 22.5f}, {73389u, 15u, 22.5f}, {73389u, 16u, -22.5f},
  };

  ///\brief the plane of a model's pad by its number; null when not known
  [[nodiscard]]
  constexpr auto pad_plane_of_pad(std::uint32_t model, std::uint32_t pad) noexcept -> float const *
    {
    for(pad_plane_pad_t const & p: pad_plane_pads)
      if(p.model == model and p.pad == pad)
        return &p.angle_deg;
    return nullptr;
    }

  ///\brief the kind of place Status.json's destination names, by its name alone: one hologram (13083 indices) shows every
  /// planetary construction site and on-foot settlement (Artur 2026-10-24), and only a construction site's name holds ": "
  /// ("Planetary Construction Site: <name>"; Artur: the colon in every language of the game, not checked); a name the game
  /// keeps as a key ("$USS_HighGradeEmissions;", a signal or a beacon) is no place with pads
  enum struct site_kind_e : std::uint8_t
    {
    unknown,
    construction_site,
    other
    };

  [[nodiscard]]
  constexpr auto site_kind_of(std::string_view destination) noexcept -> site_kind_e
    {
    if(destination.empty() or destination.front() == '$')
      return site_kind_e::unknown;
    return destination.find(": ") != std::string_view::npos ? site_kind_e::construction_site : site_kind_e::other;
    }

  [[nodiscard]]
  constexpr auto site_kind_name(site_kind_e kind) noexcept -> char const *
    {
    switch(kind)
      {
      case site_kind_e::construction_site: return "a construction site";
      case site_kind_e::other:             return "a station or settlement";
      case site_kind_e::unknown:           break;
      }
    return "not known";
    }

  ///\brief a pad learned from dockings and confirmed by a person in holo_db's form (tools/holo_db.py header writes
  /// holo_pads_generated.h): its plane, of the model's (pad_planes), and the ship's nose on it in the model's axes, at one
  /// station, at any station of a kind of the model's (station empty, kind given) or at any of the model's (station empty,
  /// kind unknown). Docked, the hologram holds the ship's frame (PRNH pads 5, 18, 21: the wing's 22.5 degrees each side,
  /// 2026-10-22/23)
  struct pad_learned_t
    {
    std::uint32_t model;
    char const * station;  ///< the journal's StationName; "" any station of the model, or of its kind
    std::uint32_t pad;
    float angle_deg;
    std::array<float, 3> nose;  ///< the ship's nose on the pad along the model's X, Y, Z; all 0 not known
    site_kind_e kind{site_kind_e::unknown};  ///< with station "": the row for any station of this kind; unknown any at all
    };

  ///\brief whether Status.json's destination names the journal's station: the same name, or a carrier's "<name> | <callsign>"
  /// against its callsign
  [[nodiscard]]
  constexpr auto station_matches(std::string_view destination, std::string_view station) noexcept -> bool
    {
    if(station.empty() or destination == station)
      return true;
    return destination.size() > station.size() + 3u and destination.ends_with(station)
           and destination.substr(destination.size() - station.size() - 3u, 3u) == " | ";
    }

  ///\brief the learned pad of a model at the destination: the station's own, else the one for its kind of place, else the
  /// one for any station; null none
  [[nodiscard]]
  constexpr auto pad_learned_in(std::span<pad_learned_t const> table, std::uint32_t model, std::string_view destination,
                                std::uint32_t pad) noexcept -> pad_learned_t const *
    {
    site_kind_e const kind{site_kind_of(destination)};
    pad_learned_t const * of_kind{};
    pad_learned_t const * any{};
    for(pad_learned_t const & p: table)
      {
      if(p.model != model or p.pad != pad)
        continue;
      if(p.station[0] != '\0')
        {
        if(station_matches(destination, p.station))
          return &p;
        }
      else if(p.kind == site_kind_e::unknown)
        any = &p;
      else if(p.kind == kind)
        of_kind = &p;
      }
    return of_kind ? of_kind : any;
    }
  }  // namespace edworld

#include "holo_pads_generated.h"

namespace edworld
  {
  [[nodiscard]]
  constexpr auto pad_learned_of(std::uint32_t model, std::string_view destination, std::uint32_t pad) noexcept
    -> pad_learned_t const *
    {
    return pad_learned_in(pad_learned, model, destination, pad);
    }

  [[nodiscard]]
  constexpr auto pad_planes_of(std::uint32_t model) noexcept -> pad_planes_t const *
    {
    for(pad_planes_t const & c: pad_planes)
      if(c.model == model)
        return &c;
    return nullptr;
    }

  struct picked_plane_t
    {
    std::array<compass_direction_t, 3> axes;  ///< the plane's up, then two axes in it (along about_axis, across)
    float angle_deg;                          ///< which of the carrier's planes
    };

  inline constexpr float pad_plane_hold_deg{4.f};  ///< another plane is taken once nearer by this much

  ///\brief of a carrier's planes the one whose up is nearest the ship's (the camera's y), from the model's axes in the
  /// hologram camera's frame; the plane taken before (`held_deg`, none outside -90..90) stays until another is nearer by
  /// pad_plane_hold_deg (between two planes the choice flickered, PRNH pad 1 2026-10-22). The planes all hold the
  /// carrier's length (about_axis): the ship's nose on any pad lies along it, towards its plus end (pads 1 and 5)
  [[nodiscard]]
  inline auto pick_pad_plane(pad_planes_t const & c, std::array<compass_direction_t, 3> const & model, float held_deg = 1000.f,
                             float const * known_deg = nullptr) noexcept -> picked_plane_t
    {
    compass_direction_t const & up{model[static_cast<std::size_t>(c.up_axis)]};
    compass_direction_t const & towards{model[static_cast<std::size_t>(c.towards_axis)]};
    compass_direction_t const & along{model[static_cast<std::size_t>(c.about_axis)]};
    auto const plane = [&](float deg) noexcept -> picked_plane_t
      {
      float const a{deg / 57.29578f};
      float const co{std::cos(a)}, si{std::sin(a)};
      compass_direction_t const turned{co * up.x + si * towards.x, co * up.y + si * towards.y, co * up.z + si * towards.z};
      // the plane's lean, its up towards about_axis and the along away from it (a fleet carrier's deck, 10.1 degrees)
      float lean_deg{};
      for(std::uint8_t i{}; i != c.planes; ++i)
        if(c.angle_deg[i] == deg)
          lean_deg = c.lean_deg[i];
      float const l{lean_deg / 57.29578f};
      float const cl{std::cos(l)}, sl{std::sin(l)};
      compass_direction_t const n{cl * turned.x + sl * along.x, cl * turned.y + sl * along.y, cl * turned.z + sl * along.z};
      compass_direction_t const on{cl * along.x - sl * turned.x, cl * along.y - sl * turned.y, cl * along.z - sl * turned.z};
      compass_direction_t const across{n.y * on.z - n.z * on.y, n.z * on.x - n.x * on.z, n.x * on.y - n.y * on.x};
      return picked_plane_t{{n, on, across}, deg};
      };
    if(known_deg)
      return plane(*known_deg);  // the pad's own, by its number
    auto const off_deg = [](picked_plane_t const & p) noexcept -> float { return std::acos(std::clamp(p.axes[0].y, -1.f, 1.f)) * 57.29578f; };
    picked_plane_t best{};
    float best_off{1000.f};
    bool held_known{};
    for(std::uint8_t i{}; i != c.planes; ++i)
      {
      picked_plane_t const p{plane(c.angle_deg[i])};
      held_known = held_known or c.angle_deg[i] == held_deg;
      if(float const off{off_deg(p)}; off < best_off)
        {
        best_off = off;
        best = p;
        }
      }
    if(held_known and best.angle_deg != held_deg)
      if(picked_plane_t const held{plane(held_deg)}; off_deg(held) - best_off < pad_plane_hold_deg)
        return held;
    return best;
    }

  // ---- the T1 table's watch at Docked (edworld_eht, 2026-10-22) ----
  // The hologram turns to the world's frame once docked, so the check takes the last look on the table's pad before it.
  inline constexpr unsigned long long t1_check_ms{3000};
  inline constexpr float t1_plane_max_deg{15.f};  ///< the pads' plane to the ship's, docked: about 0 when the table holds
  inline constexpr float t1_nose_max_deg{30.f};   ///< the ship's nose to the table's, docked

  struct t1_look_t
    {
    unsigned long long at;  ///< GetTickCount64 of the look; 0 none
    std::uint32_t pad;      ///< 0 none
    std::uint32_t model;
    float tilt;             ///< pad_tilt_deg with the table's up
    float nose_deg;         ///< nose_off_deg of the table's nose
    };

  ///\brief degrees between a direction in the ship's frame (x right, y up, z forward) and the ship's nose
  [[nodiscard]]
  inline auto nose_off_deg(compass_direction_t const & v) noexcept -> float
    {
    float const n{std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z)};
    if(n <= 0.f)
      return 180.f;
    return std::acos(std::clamp(v.z / n, -1.f, 1.f)) * 57.29578f;
    }

  ///\brief whether a look before Docked agrees with the table: the pads' plane level with the ship's, the nose the table's
  [[nodiscard]]
  constexpr auto t1_agrees(float tilt_deg, float nose_deg) noexcept -> bool
    {
    return tilt_deg <= t1_plane_max_deg and nose_deg <= t1_nose_max_deg;
    }

  enum struct t1_verdict_e : std::uint8_t
    {
    none,      ///< no look on a T1 pad this approach: nothing to say
    match,     ///< the last look agrees with the table
    deviates,  ///< it does not
    no_look    ///< a look on a T1 pad, but none within t1_check_ms before Docked
    };

  ///\brief the T1 table's watch at Docked (now: GetTickCount64)
  [[nodiscard]]
  constexpr auto t1_verdict(t1_look_t const & look, unsigned long long now) noexcept -> t1_verdict_e
    {
    if(look.pad == 0u)
      return t1_verdict_e::none;
    if(now - look.at > t1_check_ms)
      return t1_verdict_e::no_look;
    return t1_agrees(look.tilt, look.nose_deg) ? t1_verdict_e::match : t1_verdict_e::deviates;
    }

  ///\brief which end of the model's X is the pads' up: the ship is always over the pads (Artur, 2026-10-21), so up is the
  /// end towards the ship, against the direction to the target. +1 or -1; 0 while the target lies within about 12 degrees
  /// of the pads' plane, where the sign is not sure
  [[nodiscard]]
  inline auto pad_up_sign(compass_direction_t const & x, compass_direction_t const & to_target) noexcept -> int
    {
    float const nx{std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z)};
    float const nt{std::sqrt(to_target.x * to_target.x + to_target.y * to_target.y + to_target.z * to_target.z)};
    if(not(nx > 1e-6f) or not(nt > 1e-6f))
      return 0;
    float const towards_ship{-(x.x * to_target.x + x.y * to_target.y + x.z * to_target.z) / (nx * nt)};
    if(std::fabs(towards_ship) < 0.2f)
      return 0;
    return towards_ship > 0.f ? 1 : -1;
    }

  ///\brief the vote on which end of X is the pads' up (Artur, 2026-10-21: several findings and a majority, as three
  /// computers in a spacecraft; the findings from changing angles, those near the pads' plane the weaker): each vote is
  /// one look at the target ahead, its weight the sine of the ship's height over the pads' plane
  struct pad_up_vote_t
    {
    int decided;  ///< +1 or -1 once three votes agree, 0 before
    int plus, minus;
    float plus_weight, minus_weight;
    int against;  ///< strong votes in a row against the decision
    compass_direction_t last;  ///< the direction of the last vote
    bool have_last;
    };

  enum struct pad_up_event_e : std::uint8_t
    {
    none,
    decided,  ///< three votes agreed
    turned    ///< three strong votes in a row against the decision turned it
    };

  ///\brief one look at the target: a vote when the target is ahead (a filled dot: z > 0), at least min_sine off the pads'
  /// plane, and its direction at least min_step_deg from the last vote's. Undecided, three agreeing votes decide (four
  /// at most split two to two, a fifth then settles it); decided, three votes in a row against it of at least
  /// strong_sine turn it
  inline auto pad_up_vote(pad_up_vote_t & v, compass_direction_t const & x, compass_direction_t const & to_target,
                          float min_sine = 0.2f, float min_step_deg = 3.f, float strong_sine = 0.5f) noexcept
    -> pad_up_event_e
    {
    float const nx{std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z)};
    float const nt{std::sqrt(to_target.x * to_target.x + to_target.y * to_target.y + to_target.z * to_target.z)};
    if(not(nx > 1e-6f) or not(nt > 1e-6f) or not(to_target.z > 0.f))
      return pad_up_event_e::none;
    float const e{-(x.x * to_target.x + x.y * to_target.y + x.z * to_target.z) / (nx * nt)};
    if(std::fabs(e) < min_sine)
      return pad_up_event_e::none;
    compass_direction_t const unit{to_target.x / nt, to_target.y / nt, to_target.z / nt};
    if(v.have_last)
      {
      float const c{unit.x * v.last.x + unit.y * v.last.y + unit.z * v.last.z};
      if(c > std::cos(min_step_deg / 57.29578f))
        return pad_up_event_e::none;
      }
    v.last = unit;
    v.have_last = true;
    int const sign{e > 0.f ? 1 : -1};
    float const weight{std::fabs(e)};
    if(v.decided == 0)
      {
      if(sign > 0)
        {
        ++v.plus;
        v.plus_weight += weight;
        }
      else
        {
        ++v.minus;
        v.minus_weight += weight;
        }
      if(v.plus >= 3 or v.minus >= 3)
        {
        v.decided = v.plus >= 3 ? 1 : -1;
        return pad_up_event_e::decided;
        }
      return pad_up_event_e::none;
      }
    if(sign != v.decided and weight >= strong_sine)
      {
      if(++v.against >= 3)
        {
        v.decided = sign;
        v.against = 0;
        return pad_up_event_e::turned;
        }
      }
    else if(sign == v.decided)
      v.against = 0;
    return pad_up_event_e::none;
    }

  ///\brief degrees between the pad's plane and the ship's (x right, y up, z forward), from the pad's normal (the model's
  /// X) against the ship's up. With the pads' up known (up_sign +1 or -1, pad_up_sign) 0..180, a ship upside down over
  /// the pads 180; without it (0) whichever way X points, 0..90
  [[nodiscard]]
  inline auto pad_tilt_deg(compass_direction_t const & pad_normal, int up_sign = 0) noexcept -> float
    {
    float const n{std::sqrt(pad_normal.x * pad_normal.x + pad_normal.y * pad_normal.y + pad_normal.z * pad_normal.z)};
    if(not(n > 1e-6f))
      return 90.f;
    float const c{up_sign == 0 ? std::min(1.f, std::fabs(pad_normal.y) / n)
                               : std::clamp(static_cast<float>(up_sign) * pad_normal.y / n, -1.f, 1.f)};
    return std::acos(c) * 57.29578f;
    }

  ///\brief how long a pad given by the tower is taken to hold without word from the journal (the game lets it go after a
  /// few minutes; the data source clears it when the ship docks, leaves or jumps)
  inline constexpr std::int64_t pad_grant_kept_ms{10 * 60 * 1000};

  ///\brief a pad given now: the tower's grant (unix ms, 0 = none) not older than pad_grant_kept_ms. The tower gives one only
  /// near the station, so it also tells the approach view the target is near - no destination needs choosing for it
  /// (Artur, 2026-10-21)
  [[nodiscard]]
  constexpr auto pad_granted(std::int64_t granted_unix_ms, std::int64_t now_unix_ms) noexcept -> bool
    {
    return granted_unix_ms != 0 and now_unix_ms - granted_unix_ms < pad_grant_kept_ms;
    }

  ///\brief a draw of a hologram's burst as the choice of the target sees it
  struct holo_part_t
    {
    float scale{};          ///< the record's scale (byte 4)
    std::uint32_t count{};  ///< the draw's indices
    bool target{};          ///< drawn to the surface the HUD lays over second (the target's)
    bool own{};             ///< drawn to the surface the HUD lays over first (the own ship's)
    bool place{};           ///< its model is a place with pads (holo_place_of not none): a station, a port, a carrier
    };

  ///\brief of the draws `take` lets through, the smallest scale and the most indices among equal ones (a station's
  /// parts); -1 none
  template<typename take_t>
  [[nodiscard]]
  constexpr auto holo_smallest_part(holo_part_t const * parts, std::uint32_t n, take_t take) noexcept -> int
    {
    int best{-1};
    for(std::uint32_t p{}; p != n; ++p)
      {
      holo_part_t const & h{parts[p]};
      if(not take(h) or not(h.scale > 0.f))
        continue;
      holo_part_t const * const b{best < 0 ? nullptr : parts + best};
      bool const smaller{not b or h.scale < b->scale * 0.99f};
      bool const same_more{b and h.scale <= b->scale * 1.01f and h.count > b->count};
      if(smaller or same_more)
        best = static_cast<int>(p);
      }
    return best;
    }

  ///\brief the target's draw in a burst, by what it shows before where the HUD lays it (Artur 2026-10-25: a place known
  /// is the target whichever surface it comes on): a place's draw on the target's surface, else a place's on the own
  /// surface (the HUD's order swapped, as after a ship is swapped at a port; a carrier at its pad), else a draw on the
  /// target's surface; each the smallest scale, the most indices among equal ones. A surface not laid over counts for
  /// nothing; -1 none
  [[nodiscard]]
  constexpr auto holo_target_part(holo_part_t const * parts, std::uint32_t n) noexcept -> int
    {
    if(int const p{holo_smallest_part(parts, n, [](holo_part_t const & h) { return h.place and h.target; })}; p >= 0)
      return p;
    if(int const p{holo_smallest_part(parts, n, [](holo_part_t const & h) { return h.place and h.own; })}; p >= 0)
      return p;
    return holo_smallest_part(parts, n, [](holo_part_t const & h) { return h.target; });
    }

  ///\brief the ship upside down over the pads: its top towards them, over 90 degrees - known only with the pads' up
  [[nodiscard]]
  constexpr auto pad_upside_down(float tilt_deg, int up_sign) noexcept -> bool
    {
    return up_sign != 0 and tilt_deg > 90.f;
    }

  ///\brief the pad plane's colour for a tilt: green up to `green_deg`, then by the hue through yellow to red, red from
  /// `red_deg` on (0xRRGGBB)
  [[nodiscard]]
  inline auto pad_tilt_colour(float tilt_deg, float green_deg, float red_deg) noexcept -> std::uint32_t
    {
    constexpr std::uint32_t green{0x40c060u}, yellow{0xd8c040u}, red{0xd04030u};
    if(not(tilt_deg > green_deg))
      return green;
    if(not(red_deg > green_deg) or tilt_deg >= red_deg)
      return red;
    float const t{(tilt_deg - green_deg) / (red_deg - green_deg)};
    return t < 0.5f ? mix_rgb(green, yellow, t * 2.f) : mix_rgb(yellow, red, (t - 0.5f) * 2.f);
    }
  }  // namespace edworld
