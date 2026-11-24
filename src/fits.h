// edworld - whether edworld's own draws fit the game's state at hand: pure rules (no device), so the test checks them
// without the game. edworld draws its own (the clouds over the game's shell, the list's and the spheres' quads) only
// where they fit; elsewhere the game draws as it would without edworld.
#pragma once

#include <dxgiformat.h>

#include <edplugin_common/dxgi/format.h>

#include <algorithm>
#include <cstdint>

namespace edworld
  {
  ///\brief the most steps a ray takes through the clouds with depth (clouds_steps, clouds_far_steps,
  /// clouds_max_steps are held to 1..this; clouds3d.hlsl holds its loop to the same): a value without an upper bound
  /// gave a loop the graphics card could not end in time
  inline constexpr float clouds_steps_most{256.f};

  [[nodiscard]]
  constexpr auto clouds_steps_held(float steps) noexcept -> float
    {
    return not(steps >= 1.f) ? 1.f : std::min(steps, clouds_steps_most);  // NaN too
    }

  // a UINT or SINT format: edplugin_common's rule
  using edplugin_common::is_integer;

  ///\brief a panel surface's format edworld's patch reads (float4 in imgui.hlsl and list.hlsl) and writes (float4 out):
  /// the four-channel UNORM and float ones the game's surfaces have; R8G8B8A8_TYPELESS is viewed as UNORM
  [[nodiscard]]
  constexpr auto is_patch_surface(DXGI_FORMAT f) noexcept -> bool
    {
    switch(f)
      {
      case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
        return true;
      default:
        return false;
      }
    }

  ///\brief an own draw into the game's bound targets (its shaders write SV_Target0 only, as float): fits, or why not -
  /// the game's unordered access views bound (they would stay bound under a draw that is not the game's), no first
  /// target, more than one target (the other targets' values undefined), an integer first target
  enum struct own_draw_e : std::uint8_t
    {
    fits,
    uav,
    no_target,
    more_targets,
    integer,
    count  ///< the number of the values above
    };

  [[nodiscard]]
  constexpr auto own_draw(bool target, DXGI_FORMAT format, bool more_targets, bool uav) noexcept -> own_draw_e
    {
    if(uav)
      return own_draw_e::uav;
    if(not target)
      return own_draw_e::no_target;
    if(more_targets)
      return own_draw_e::more_targets;
    return is_integer(format) ? own_draw_e::integer : own_draw_e::fits;
    }

  [[nodiscard]]
  constexpr auto own_draw_text(own_draw_e e) noexcept -> char const *
    {
    switch(e)
      {
      case own_draw_e::uav:
        return "the game's unordered access views are bound";
      case own_draw_e::no_target:
        return "no first target";
      case own_draw_e::more_targets:
        return "more than one target bound";
      case own_draw_e::integer:
        return "an integer first target";
      default:
        return "it fits";
      }
    }

  ///\brief why an own draw is not drawn (none = drawn): own_draw_e's reasons, then the game's resources it reads - the
  /// constant buffers' windows holding fewer rows than its shader reads, the records at t33 not of list.hlsl's layout,
  /// the panel surface read in an integer format
  enum struct misfit_e : std::uint8_t
    {
    none,
    uav,
    no_target,
    more_targets,
    integer,
    buffers,
    records,
    integer_surface,
    count  ///< the number of the values above
    };

  [[nodiscard]]
  constexpr auto misfit_of(own_draw_e e) noexcept -> misfit_e
    {
    switch(e)
      {
      case own_draw_e::uav:          return misfit_e::uav;
      case own_draw_e::no_target:    return misfit_e::no_target;
      case own_draw_e::more_targets: return misfit_e::more_targets;
      case own_draw_e::integer:      return misfit_e::integer;
      default:                       return misfit_e::none;
      }
    }

  [[nodiscard]]
  constexpr auto misfit_text(misfit_e m) noexcept -> char const *
    {
    switch(m)
      {
      case misfit_e::uav:             return own_draw_text(own_draw_e::uav);
      case misfit_e::no_target:       return own_draw_text(own_draw_e::no_target);
      case misfit_e::more_targets:    return own_draw_text(own_draw_e::more_targets);
      case misfit_e::integer:         return own_draw_text(own_draw_e::integer);
      case misfit_e::buffers:         return "the game's constant buffers hold fewer rows than edworld's shader reads";
      case misfit_e::records:         return "the game's records at t33 are not a structured buffer of 336-byte records";
      case misfit_e::integer_surface: return "the panel surface is read in an integer format";
      default:                        return "it fits";
      }
    }

  ///\brief what a part's log says of its misfits: each reason once, and once that it is drawn after one. A part checked
  /// at several draws a frame (the shell: the picture and its reflections) in different states would else write a line
  /// at each change, every frame
  struct misfit_log_t
    {
    std::uint32_t told{};  ///< the reasons told, a bit each
    bool drawn_told{};     ///< "drawn" told since the last new reason
    };

  enum struct misfit_note_e : std::uint8_t
    {
    quiet,   ///< nothing to log
    misfit,  ///< the reason, the first time
    drawn    ///< drawn, the first time after a reason told
    };

  [[nodiscard]]
  constexpr auto note_misfit(misfit_log_t & log, misfit_e m) noexcept -> misfit_note_e
    {
    if(m == misfit_e::none)
      {
      if(log.told == 0 or log.drawn_told)
        return misfit_note_e::quiet;
      log.drawn_told = true;
      return misfit_note_e::drawn;
      }
    std::uint32_t const bit{1u << static_cast<unsigned>(m)};
    if(log.told & bit)
      return misfit_note_e::quiet;
    log.told |= bit;
    log.drawn_told = false;
    return misfit_note_e::misfit;
    }

  ///\brief the bytes a shader can read through a constant buffer slot bound with the window (first, count) of 16-byte
  /// constants into a buffer of width bytes; count 0 = bound without a window (from first to the buffer's end)
  [[nodiscard]]
  constexpr auto cb_window_bytes(std::uint32_t width, std::uint32_t first, std::uint32_t count) noexcept -> std::uint64_t
    {
    std::uint64_t const start{std::uint64_t{first} * 16u};
    if(start >= width)
      return 0;
    std::uint64_t const rest{width - start};
    return count == 0 ? rest : std::min<std::uint64_t>(std::uint64_t{count} * 16u, rest);
    }

  ///\brief whether a window holds the rows a shader declares (float4 rows[n])
  [[nodiscard]]
  constexpr auto cb_holds(std::uint32_t width, std::uint32_t first, std::uint32_t count, std::uint32_t rows) noexcept -> bool
    {
    return cb_window_bytes(width, first, count) >= std::uint64_t{rows} * 16u;
    }

  ///\brief the rows edworld's shaders read from the game's buffers: clouds.hlsl and clouds3d.hlsl (b0 object_rows[13],
  /// b1 frame_rows[336]), list.hlsl (b0 cb0[8], b1 cb1[276])
  inline constexpr std::uint32_t clouds_object_rows{13};
  inline constexpr std::uint32_t clouds_frame_rows{336};
  inline constexpr std::uint32_t list_cb0_rows{8};
  inline constexpr std::uint32_t list_cb1_rows{276};

  ///\brief list.hlsl's record_t at the game's t33 (a structured buffer of this stride)
  inline constexpr std::uint32_t record_stride{336};

  ///\brief a place on a surface of w x h held inside it (a gate from the settings read by Load)
  [[nodiscard]]
  constexpr auto held_inside(float v, std::uint32_t size) noexcept -> std::int32_t
    {
    if(size == 0)
      return 0;
    float const most{static_cast<float>(size - 1u)};
    return static_cast<std::int32_t>(not(v > 0.f) ? 0.f : std::min(v, most));
    }
  }  // namespace edworld
