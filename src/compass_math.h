// edworld — the HUD compass read off its interface surface: pure arithmetic, no D3D, testable anywhere.
//
// The compass by the radar is the sphere of directions around the ship seen from the front: the target's direction
// d = (right, up, forward) in the ship's frame lands on the disc at R * (right, up) (an orthographic projection), the
// dot filled while the target is ahead (forward > 0), hollow while it is behind. So the angle off the nose is
// asin(r / R), not proportional to r: near the rim the dot hardly moves. Checked on 2026-10-06 (ed-lab, a Kestrel,
// the planet as the target): asin(r / R) against 90 degrees plus the flight path angle from Status.json agreed
// within 1-2 degrees while the ship flew straight.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edworld
  {
  ///\brief the dot as found in an RGBA copy of the compass: its centre relative to the disc's centre, in surface
  /// pixels (x right, y down as on the surface)
  struct compass_reading_t
    {
    bool found;
    ///\brief the dot's middle is lit (target ahead); a hollow ring (target behind) leaves it dark
    bool filled;
    float x;
    float y;
    ///\brief the dot's lit pixels (a filled dot ~137, a hollow one ~25 on the 2200x1800 surface)
    std::uint32_t pixels;
    };

  ///\brief the dot in an RGBA copy (pixel (x, y) at data + y * pitch + x * 4) whose disc's centre is (cx, cy) in the
  /// copy: the centroid of the white pixels (each channel above threshold) within limit of the centre - the disc's
  /// grid and rim are blue, never white
  [[nodiscard]]
  inline auto read_compass(std::uint8_t const * data, std::uint32_t pitch, std::int32_t width, std::int32_t height, float cx,
                           float cy, float limit, std::uint8_t threshold = 170) noexcept -> compass_reading_t
    {
    compass_reading_t out{};
    double sx{}, sy{};
    std::uint32_t n{};
    float const limit2{limit * limit};
    for(std::int32_t y{}; y != height; ++y)
      {
      std::uint8_t const * row{data + static_cast<std::size_t>(y) * pitch};
      for(std::int32_t x{}; x != width; ++x)
        {
        std::uint8_t const * p{row + static_cast<std::size_t>(x) * 4u};
        if(p[0] <= threshold or p[1] <= threshold or p[2] <= threshold)
          continue;
        float const dx{static_cast<float>(x) - cx}, dy{static_cast<float>(y) - cy};
        if(dx * dx + dy * dy > limit2)
          continue;
        sx += dx;
        sy += dy;
        ++n;
        }
      }
    if(n == 0)
      return out;
    out.found = true;
    out.pixels = n;
    out.x = static_cast<float>(sx / n);
    out.y = static_cast<float>(sy / n);
    // the middle of the dot: lit when filled, the ring's hole when hollow
    std::int32_t const mx{static_cast<std::int32_t>(std::lround(cx + out.x))}, my{static_cast<std::int32_t>(std::lround(cy + out.y))};
    if(mx >= 0 and my >= 0 and mx < width and my < height)
      {
      std::uint8_t const * p{data + static_cast<std::size_t>(my) * pitch + static_cast<std::size_t>(mx) * 4u};
      out.filled = p[0] > threshold and p[1] > threshold and p[2] > threshold;
      }
    return out;
    }

  ///\brief a reading worth showing: a filled dot is ~137 lit pixels, a hollow ring ~25 (2200x1800, 2026-10-06);
  /// a handful (the dot half drawn, a flash entering an atmosphere) or a thousand (the whole disc lit) is not the dot
  [[nodiscard]]
  constexpr auto compass_plausible(compass_reading_t const & r) noexcept -> bool
    {
    return r.found and (r.filled ? r.pixels >= 80u and r.pixels <= 400u : r.pixels >= 15u and r.pixels <= 40u);
    }

  inline constexpr float compass_degrees{57.29577951308232f};

  ///\brief the target's direction from the dot, in degrees
  struct compass_angles_t
    {
    ///\brief between the nose and the target, 0..180
    float off_nose;
    ///\brief above (+) or below (-) the ship's horizontal plane (its wings' plane): asin(up)
    float up;
    ///\brief right (+) or left (-) of the ship's vertical plane, measured in the plane of nose and wings:
    /// atan2(right, forward), -180..180
    float right;
    };

  ///\brief the angles from the dot's offset (x right, y down, surface pixels) on a disc of radius radius; an offset
  /// past the rim counts as on it
  [[nodiscard]]
  inline auto compass_angles(float x, float y, float radius, bool filled) noexcept -> compass_angles_t
    {
    float rx{x / radius}, ry{-y / radius};
    float const r{std::sqrt(rx * rx + ry * ry)};
    if(r > 1.f)
      {
      rx /= r;
      ry /= r;
      }
    float const forward{std::sqrt(std::max(0.f, 1.f - rx * rx - ry * ry)) * (filled ? 1.f : -1.f)};
    return compass_angles_t{
      std::acos(std::clamp(forward, -1.f, 1.f)) * compass_degrees,
      std::asin(std::clamp(ry, -1.f, 1.f)) * compass_degrees,
      std::atan2(rx, forward) * compass_degrees
    };
    }

  ///\brief the flight path angle (degrees, + climbing) between two fixes over a sphere of radius planet_radius:
  /// latitude and longitude in degrees, altitudes in metres; none (NaN) when the fixes are the same place
  [[nodiscard]]
  inline auto flight_path_angle(double lat1, double lon1, double alt1, double lat2, double lon2, double alt2,
                                double planet_radius) noexcept -> double
    {
    constexpr double rad{3.14159265358979323846 / 180.0};
    double const a1{lat1 * rad}, a2{lat2 * rad}, dl{(lon2 - lon1) * rad};
    double const c{std::acos(std::clamp(std::sin(a1) * std::sin(a2) + std::cos(a1) * std::cos(a2) * std::cos(dl), -1.0, 1.0))};
    double const horizontal{c * (planet_radius + (alt1 + alt2) / 2.0)};
    double const vertical{alt2 - alt1};
    if(horizontal == 0.0 and vertical == 0.0)
      return std::nan("");
    return std::atan2(vertical, horizontal) / rad;
    }

  // ---- the spheres drawn beside the dashboard (PoC): the target's direction in the ship's frame, and a view of it ----
  ///\brief a direction in the ship's frame: x right, y up, z forward (unit length)
  struct compass_direction_t
    {
    float x, y, z;
    };

  ///\brief the target's direction from the dot (x right, y down on the surface, radius in its pixels), as
  /// compass_angles reads it
  [[nodiscard]]
  inline auto compass_direction(float x, float y, float radius, bool filled) noexcept -> compass_direction_t
    {
    float rx{x / radius}, ry{-y / radius};
    float const r{std::sqrt(rx * rx + ry * ry)};
    if(r > 1.f)
      {
      rx /= r;
      ry /= r;
      }
    return compass_direction_t{rx, ry, std::sqrt(std::max(0.f, 1.f - rx * rx - ry * ry)) * (filled ? 1.f : -1.f)};
    }

  ///\brief a direction from its angles in degrees (up: above the wings' plane, right: atan2(right, forward))
  [[nodiscard]]
  inline auto direction_of(float up, float right) noexcept -> compass_direction_t
    {
    float const u{up / compass_degrees}, r{right / compass_degrees};
    return compass_direction_t{std::cos(u) * std::sin(r), std::sin(u), std::cos(u) * std::cos(r)};
    }

  ///\brief a point of the unit sphere as a camera behind, to the left and above sees it: x right and y up on the
  /// picture (the sphere's outline is the unit circle), on_far_side: on the far side of the sphere from the camera
  struct sphere_point_t
    {
    float x, y;
    bool on_far_side;
    };

  ///\brief the camera turned yaw degrees about the ship's vertical (to its left) and tilted pitch degrees down
  /// over it; yaw 0, pitch 0 looks along the nose from behind
  [[nodiscard]]
  inline auto sphere_view(compass_direction_t const & d, float yaw, float pitch) noexcept -> sphere_point_t
    {
    float const cy{std::cos(yaw / compass_degrees)}, sy{std::sin(yaw / compass_degrees)};
    float const cp{std::cos(pitch / compass_degrees)}, sp{std::sin(pitch / compass_degrees)};
    float const x1{d.x * cy - d.z * sy}, z1{d.x * sy + d.z * cy};
    float const y2{d.y * cp + z1 * sp}, z2{-d.y * sp + z1 * cp};
    return sphere_point_t{x1, y2, z2 > 0.f};
    }
  }  // namespace edworld
