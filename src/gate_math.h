// edworld — gates down to a place on a planet (PoC): pure arithmetic, no D3D, testable anywhere.
//
// The way down Artur flies (2026-10-11, his words and three recorded approaches): every angle from the planet's visible
// edge (the limb), not the local horizontal. Set up beside the planet, a slight arc onto a flat line - the nose on the
// limb - flown until the place stands dive degrees below the limb, then straight at it. The gates mark that path: from
// the ship to a point Q on the flat line (a turn of at most arc there), along it to the dive point E at a set height,
// then down to the place, smaller towards it. Both recordings dived with the place 16-18 degrees below the limb
// (Kisekka 0.22 R up, Moller 0.44 R up), so the dive is 17 by default.
// Everything is in a frame fixed to the planet's centre (metres): x towards latitude 0 longitude 0, z towards the north
// pole; the planet a sphere of radius planet_radius, the place at that radius (its own height unknown).
// To draw them the ship's attitude comes from the compass (the place's direction in the ship's frame) and the heading
// Status.json gives; the camera is taken as the ship's axes (the cockpit view, no head look).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "compass_math.h"

namespace edworld
  {
  ///\brief a point or direction in double precision: planets are millions of metres across, gates metres apart
  struct dvec3_t
    {
    double x, y, z;
    };

  [[nodiscard]]
  constexpr auto operator+(dvec3_t const & a, dvec3_t const & b) noexcept -> dvec3_t
    {
    return dvec3_t{a.x + b.x, a.y + b.y, a.z + b.z};
    }

  [[nodiscard]]
  constexpr auto operator-(dvec3_t const & a, dvec3_t const & b) noexcept -> dvec3_t
    {
    return dvec3_t{a.x - b.x, a.y - b.y, a.z - b.z};
    }

  [[nodiscard]]
  constexpr auto operator*(dvec3_t const & a, double k) noexcept -> dvec3_t
    {
    return dvec3_t{a.x * k, a.y * k, a.z * k};
    }

  [[nodiscard]]
  constexpr auto dot(dvec3_t const & a, dvec3_t const & b) noexcept -> double
    {
    return a.x * b.x + a.y * b.y + a.z * b.z;
    }

  [[nodiscard]]
  constexpr auto cross(dvec3_t const & a, dvec3_t const & b) noexcept -> dvec3_t
    {
    return dvec3_t{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

  [[nodiscard]]
  inline auto length(dvec3_t const & a) noexcept -> double
    {
    return std::sqrt(dot(a, a));
    }

  ///\brief a unit vector along a; a itself when it has no length
  [[nodiscard]]
  inline auto unit(dvec3_t const & a) noexcept -> dvec3_t
    {
    double const l{length(a)};
    return l == 0.0 ? a : a * (1.0 / l);
    }

  inline constexpr double gate_rad{3.14159265358979323846 / 180.0};

  ///\brief the point at latitude and longitude (degrees) and distance radius from the planet's centre
  [[nodiscard]]
  inline auto planet_point(double lat, double lon, double radius) noexcept -> dvec3_t
    {
    double const a{lat * gate_rad}, o{lon * gate_rad};
    return dvec3_t{radius * std::cos(a) * std::cos(o), radius * std::cos(a) * std::sin(o), radius * std::sin(a)};
    }

  ///\brief the local east, north and up at a point
  struct local_frame_t
    {
    dvec3_t east, north, up;
    };

  [[nodiscard]]
  inline auto local_frame(double lat, double lon) noexcept -> local_frame_t
    {
    double const a{lat * gate_rad}, o{lon * gate_rad};
    return local_frame_t{
      dvec3_t{-std::sin(o), std::cos(o), 0.0},
      dvec3_t{-std::sin(a) * std::cos(o), -std::sin(a) * std::sin(o), std::cos(a)},
      dvec3_t{std::cos(a) * std::cos(o), std::cos(a) * std::sin(o), std::sin(a)}
    };
    }

  ///\brief how far the planet's visible edge lies below the local horizontal from radius r, radians
  [[nodiscard]]
  inline auto limb_dip(double r, double planet_radius) noexcept -> double
    {
    return r <= planet_radius ? 0.0 : std::acos(planet_radius / r);
    }

  ///\brief how far below the local horizontal the place stands from the ship, degrees (negative above it)
  [[nodiscard]]
  inline auto below_horizontal(dvec3_t const & ship, dvec3_t const & place) noexcept -> double
    {
    return std::asin(std::clamp(-dot(unit(place - ship), unit(ship)), -1.0, 1.0)) / gate_rad;
    }

  ///\brief how far below the planet's visible edge the place stands from the ship, degrees (negative above it, behind
  /// the edge)
  [[nodiscard]]
  inline auto below_limb(dvec3_t const & ship, dvec3_t const & place, double planet_radius) noexcept -> double
    {
    return below_horizontal(ship, place) - limb_dip(length(ship), planet_radius) / gate_rad;
    }

  ///\brief where the way down starts: the point at the ship's height (alt metres), on the great circle from the place
  /// towards the ship, from which the place stands dive degrees below the planet's visible edge; found false when no
  /// such point exists (from radius planet_radius / sin(dive) up the place at the disc's middle stands only
  /// 90 - acos(R / r) below the edge, less than dive)
  struct descent_start_t
    {
    bool found;
    dvec3_t point;
    ///\brief its angle round the planet from the place, radians
    double central_angle;
    };

  [[nodiscard]]
  inline auto descent_start(double lat, double lon, double alt, double place_lat, double place_lon, double planet_radius,
                            double dive) noexcept -> descent_start_t
    {
    // from a point at radius r = R + h, central angle c from the place, the place stands below the local horizontal by
    // atan2(r - R cos c, R sin c) and below the edge by that less acos(R / r); d = dive + acos(R / r) below the
    // horizontal means R (tan(d) sin c + cos c) = r, i.e. cos(c - d) = r cos(d) / R
    double const r{planet_radius + alt};
    if(dive <= 0.0 or dive >= 90.0 or planet_radius <= 0.0 or r <= planet_radius)
      return descent_start_t{};
    double const d{dive * gate_rad + limb_dip(r, planet_radius)};
    double const q{r * std::cos(d) / planet_radius};
    if(not (q <= 1.0) or d >= 3.14159265358979323846 / 2.0)
      return descent_start_t{};
    double const c{d - std::acos(q)};
    dvec3_t const p{unit(planet_point(place_lat, place_lon, 1.0))};
    dvec3_t const s{unit(planet_point(lat, lon, 1.0))};
    dvec3_t w{s - p * dot(s, p)};
    if(length(w) < 1e-12)
      w = local_frame(place_lat, place_lon).north;  // the ship over the place: any way down will do
    w = unit(w);
    return descent_start_t{true, (p * std::cos(c) + w * std::sin(c)) * r, c};
    }

  enum struct approach_phase_e : std::uint8_t
    {
    none,   ///< no path: bad settings, or no dive point at the set height
    setup,  ///< still to come onto the flat line (to Q, along it to E) before the dive
    dive    ///< straight at the place
    };

  ///\brief the path the gates mark: point[0] the ship, the last the place; from point[dive_from] on it is the dive
  struct approach_path_t
    {
    approach_phase_e phase;
    std::size_t count;
    std::array<dvec3_t, 4> point;
    std::size_t dive_from;
    ///\brief the place below the visible edge from the ship, degrees
    double below_limb;
    ///\brief the turn onto the flat line, degrees (at Q, or at E when no Q gives at most arc), 0 without one
    double turn;
    };

  ///\brief the path down from the ship (lat, lon, alt metres) to the place, worked out afresh from where the ship is,
  /// with nothing remembered: in the plane of the planet's centre, the ship and the place,
  /// - E: the point at dive_height (a fraction of the planet's radius) above the ground on the ship's side, from which
  ///   the place stands dive degrees below the visible edge;
  /// - the flat line: through E towards the place with the nose on the edge as seen from E, reaching back up from E;
  /// - the dive: the place at least dive below the edge and at most steepest below the local horizontal (90 = no
  ///   limit) - straight at it;
  /// - otherwise the setup: to Q, the point on the flat line short of E with a turn of arc onto it, then along it to E
  ///   and down; when no such Q lies short of E (the ship too far off the line for so slight a turn), straight to E;
  ///   when E is behind the ship along the line, straight at the place
  [[nodiscard]]
  inline auto approach_path(double lat, double lon, double alt, double place_lat, double place_lon, double planet_radius,
                            double dive, double dive_height, double arc, double steepest) noexcept -> approach_path_t
    {
    approach_path_t out{};
    double const R{planet_radius};
    if(R <= 0.0 or alt <= 0.0 or dive <= 0.0 or dive >= 90.0 or dive_height <= 0.0 or arc <= 0.0 or arc >= 90.0)
      return out;
    dvec3_t const ship{planet_point(lat, lon, R + alt)}, place{planet_point(place_lat, place_lon, R)};
    out.below_limb = below_limb(ship, place, R);
    // the plane's axes: p up through the place, w round from it towards the ship
    dvec3_t const p{unit(place)};
    dvec3_t w{unit(ship) - p * dot(unit(ship), p)};
    w = length(w) < 1e-12 ? local_frame(place_lat, place_lon).north : unit(w);
    auto const at = [&](double x, double y) { return w * x + p * y; };
    double const sx{dot(ship, w)}, sy{dot(ship, p)};
    // E and the flat line's way forward f (towards the place, the nose on the edge seen from E)
    double const re{R * (1.0 + dive_height)}, dip{limb_dip(re, R)}, d{dive * gate_rad + dip};
    double const q{re * std::cos(d) / R};
    if(not (q <= 1.0) or d >= 3.14159265358979323846 / 2.0)
      return out;
    double const ce{d - std::acos(q)};
    double const ex{re * std::sin(ce)}, ey{re * std::cos(ce)};
    double const fx{-std::cos(ce) * std::cos(dip) - std::sin(ce) * std::sin(dip)}, fy{std::sin(ce) * std::cos(dip) - std::cos(ce) * std::sin(dip)};
    out.point[0] = ship;
    if(out.below_limb >= dive and below_horizontal(ship, place) <= steepest)
      {
      out.phase = approach_phase_e::dive;
      out.count = 2u;
      out.point[1] = place;
      return out;
      }
    // E from the ship along the line (df) and off it (dn); Q at s back from E sees the ship arc off the line
    double const dx{ex - sx}, dy{ey - sy};
    double const df{dx * fx + dy * fy}, dn{std::fabs(dy * fx - dx * fy)};
    double const s{df - dn / std::tan(arc * gate_rad)};
    out.phase = approach_phase_e::setup;
    if(s >= 0.0)
      {
      double const qx{ex - fx * s}, qy{ey - fy * s};
      bool const on_line{std::hypot(qx - sx, qy - sy) < 1.0};  // already on it: no Q
      out.count = 0u;
      out.point[out.count++] = ship;
      if(not on_line)
        out.point[out.count++] = at(qx, qy);
      out.dive_from = out.count;
      out.point[out.count++] = at(ex, ey);
      out.point[out.count++] = place;
      out.turn = on_line ? 0.0 : arc;
      }
    else if(df > 0.0)
      {
      out.count = 3u;
      out.point[1] = at(ex, ey);
      out.point[2] = place;
      out.dive_from = 1u;
      out.turn = std::atan2(dn, df) / gate_rad;
      }
    else
      {
      out.phase = approach_phase_e::dive;
      out.count = 2u;
      out.point[1] = place;
      }
    return out;
    }

  inline constexpr std::size_t max_gates{24};

  ///\brief a gate: a rectangle square to the line down, corners round it (top left, top right, bottom right, bottom left
  /// as seen flying down the line), top towards the local up
  struct gate_t
    {
    dvec3_t centre;
    std::array<dvec3_t, 4> corner;
    };

  struct gates_t
    {
    std::size_t count;
    std::array<gate_t, max_gates> gate;
    };

  ///\brief count gates (at most max_gates) evenly along the straight line from start to place (the place itself
  /// excluded), each with a half side of spread times its distance from the place, never under min_half metres: a
  /// funnel narrowing towards the place
  [[nodiscard]]
  inline auto descent_gates(dvec3_t const & start, dvec3_t const & place, std::size_t count, double spread, double min_half) noexcept
    -> gates_t
    {
    gates_t out{};
    out.count = std::min(count, max_gates);
    dvec3_t const line{place - start};
    dvec3_t const along{unit(line)};
    // the gate's up: the place's local up, square to the line
    dvec3_t const local_up{unit(place)};
    dvec3_t up{local_up - along * dot(local_up, along)};
    up = length(up) < 1e-12 ? unit(cross(along, dvec3_t{1.0, 0.0, 0.0})) : unit(up);
    dvec3_t const right{cross(along, up)};
    double const total{length(line)};
    for(std::size_t i{}; i != out.count; ++i)
      {
      double const t{static_cast<double>(i) / static_cast<double>(out.count)};
      dvec3_t const c{start + line * t};
      double const half{std::max(min_half, spread * total * (1.0 - t))};
      dvec3_t const u{up * half}, r{right * half};
      out.gate[i] = gate_t{c, {c + u - r, c + u + r, c - u + r, c - u - r}};
      }
    return out;
    }

  ///\brief a gate along a path: dive true from the dive on
  struct path_gate_t
    {
    gate_t gate;
    bool dive;
    };

  struct path_gates_t
    {
    std::size_t count;
    std::array<path_gate_t, max_gates> gate;
    };

  ///\brief count gates (at most max_gates) evenly along a path (the place itself excluded), each square to its piece
  /// of the path with its top towards the local up there, a half side of spread times the path still to fly, never under
  /// min_half metres
  [[nodiscard]]
  inline auto path_gates(approach_path_t const & path, std::size_t count, double spread, double min_half) noexcept -> path_gates_t
    {
    path_gates_t out{};
    if(path.count < 2u)
      return out;
    double total{};
    for(std::size_t i{1u}; i != path.count; ++i)
      total += length(path.point[i] - path.point[i - 1u]);
    if(total <= 0.0)
      return out;
    out.count = std::min(count, max_gates);
    std::size_t piece{1u};
    double before{};  // the path's length before the piece's start
    for(std::size_t i{}; i != out.count; ++i)
      {
      double const at{total * static_cast<double>(i) / static_cast<double>(out.count)};
      while(piece + 1u < path.count and before + length(path.point[piece] - path.point[piece - 1u]) <= at)
        {
        before += length(path.point[piece] - path.point[piece - 1u]);
        ++piece;
        }
      dvec3_t const a{path.point[piece - 1u]}, b{path.point[piece]};
      dvec3_t const along{unit(b - a)};
      dvec3_t const c{a + along * (at - before)};
      dvec3_t const local_up{unit(c)};
      dvec3_t up{local_up - along * dot(local_up, along)};
      up = length(up) < 1e-12 ? unit(cross(along, dvec3_t{1.0, 0.0, 0.0})) : unit(up);
      dvec3_t const right{cross(along, up)};
      double const half{std::max(min_half, spread * (total - at))};
      dvec3_t const u{up * half}, r{right * half};
      out.gate[i] = path_gate_t{gate_t{c, {c + u - r, c + u + r, c - u + r, c - u - r}}, piece - 1u >= path.dive_from};
      }
    return out;
    }

  ///\brief the ship's axes in the planet's frame: right, up, forward (the nose)
  struct attitude_t
    {
    bool found;
    dvec3_t right, up, forward;
    ///\brief the nose above (+) the horizon and the roll (+ right wing down), degrees
    double pitch, roll;
    };

  ///\brief the attitude from one direction known in both frames - the place's, toward in the planet's frame and seen
  /// (compass) in the ship's - and the heading (degrees, 0 north, 90 east: the nose's bearing). The nose's angle off
  /// the place fixes the pitch up to two answers; the one nearer pitch_hint (degrees, the flight path angle: in
  /// supercruise the nose flies along the path) is taken, the smaller one without a hint. Then the place's side of the
  /// nose fixes the roll.
  [[nodiscard]]
  inline auto attitude_from(local_frame_t const & at, dvec3_t const & toward, compass_direction_t const & seen, double heading,
                            double pitch_hint) noexcept -> attitude_t
    {
    dvec3_t const t{unit(toward)};
    double const h{heading * gate_rad};
    dvec3_t const level{at.east * std::sin(h) + at.north * std::cos(h)};
    double const a{dot(t, level)}, b{dot(t, at.up)}, f{static_cast<double>(seen.z)};
    double const m{std::hypot(a, b)};
    if(m < 1e-9)
      return attitude_t{};
    double const base{std::atan2(b, a)}, spread_{std::acos(std::clamp(f / m, -1.0, 1.0))};
    double p1{base + spread_}, p2{base - spread_};
    auto const fits = [](double p) { return std::fabs(p) <= 3.14159265358979323846 / 2.0 + 1e-9; };
    double pitch{};
    if(fits(p1) and fits(p2))
      {
      double const hint{std::isnan(pitch_hint) ? 0.0 : pitch_hint * gate_rad};
      pitch = std::fabs(p1 - hint) <= std::fabs(p2 - hint) ? p1 : p2;
      }
    else if(fits(p1))
      pitch = p1;
    else if(fits(p2))
      pitch = p2;
    else
      return attitude_t{};
    dvec3_t const forward{level * std::cos(pitch) + at.up * std::sin(pitch)};
    dvec3_t const right0{at.east * std::cos(h) - at.north * std::sin(h)};
    dvec3_t const up0{cross(right0, forward)};
    // rolled by phi (right wing down) the place's (right, up) is its unrolled (a0, b0) turned by phi
    double const a0{dot(t, right0)}, b0{dot(t, up0)};
    double roll{0.0};
    if(std::hypot(static_cast<double>(seen.x), static_cast<double>(seen.y)) > 1e-6 and std::hypot(a0, b0) > 1e-9)
      roll = std::atan2(static_cast<double>(seen.y), static_cast<double>(seen.x)) - std::atan2(b0, a0);
    roll = std::remainder(roll, 2.0 * 3.14159265358979323846);
    dvec3_t const right{right0 * std::cos(roll) - up0 * std::sin(roll)};
    dvec3_t const up{right0 * std::sin(roll) + up0 * std::cos(roll)};
    return attitude_t{true, right, up, forward, pitch / gate_rad, roll / gate_rad};
    }

  ///\brief a point on the screen in pixels (x right, y down); in_front false when it is behind the camera
  struct screen_point_t
    {
    bool in_front;
    double x, y;
    };

  ///\brief a point as the camera at eye with the ship's axes sees it: a perspective of vertical field of view fov
  /// degrees on a width x height screen
  [[nodiscard]]
  inline auto project(dvec3_t const & point, dvec3_t const & eye, attitude_t const & axes, double fov, double width, double height) noexcept
    -> screen_point_t
    {
    dvec3_t const d{point - eye};
    double const z{dot(d, axes.forward)};
    if(z <= 0.0)
      return screen_point_t{};
    double const ty{std::tan(fov * gate_rad / 2.0)}, tx{ty * width / height};
    double const nx{dot(d, axes.right) / (z * tx)}, ny{dot(d, axes.up) / (z * ty)};
    return screen_point_t{true, (nx + 1.0) / 2.0 * width, (1.0 - ny) / 2.0 * height};
    }
  }  // namespace edworld
