// edworld — what the proxy's parts share: settings, the log, the hooks' entry point.
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace edworld
  {
  // Two builds of one source: edworld works on its own (Status.json, EDSM); edworld_eht (EDWORLD_EHT defined) also
  // publishes the panels' anchors to EHT's overlay and takes the destination's allegiance from EHT's `target`.
#if defined(EDWORLD_EHT)
  inline constexpr bool with_eht{true};
  inline constexpr wchar_t plugin_name[]{L"edworld_eht"};
#else
  inline constexpr bool with_eht{false};
  inline constexpr wchar_t plugin_name[]{L"edworld"};
#endif

  struct settings_t
    {
    ///\brief the next d3d11 in the chain (EDHM, EDVR renamed, ReShade); empty = the system copy
    std::wstring next;
    ///\brief vertex shaders whose draws are panels: the cockpit holo panel family, its "GUI effects off" twin, and
    /// the one the game switches the same panels to near a war settlement (surface at PS t1; seen 2026-10-05)
    std::vector<std::uint64_t> watch_vs{0x81216C77F90DEDD6ull, 0x1989E6D3B405FDE0ull, 0x925ACEDA0153AA5Bull};
#if defined(EDWORLD_EHT)
    ///\brief the tmpfs directory shared with EHT: `panels` is published there, `target` read from there
    /// (Z: is the Linux root under Wine); empty = neither
    std::wstring shm_dir{L"Z:\\dev\\shm\\eht"};
#endif
    ///\brief a summary line of the frame's panels at most this often; 0 = never
    std::uint32_t log_interval_ms{1000};
    ///\brief a gap between two panel draws longer than this starts a new frame
    std::uint32_t frame_gap_us{2500};
    ///\brief log the hash of every vertex shader the game creates (discovery)
    bool log_all_vs{false};
    ///\brief edworld's output directory (edloader's log directory, else the dll's); a file `edworld_dump` there
    /// asks for one dump of every panel's interface surface into `edworld_dumps\` (raw rows; size, format and row
    /// pitch in the name)
    std::wstring dir;

    // ---- the jump panel patch: drawn by the proxy onto the panel's interface surface while a jump charges ----
    ///\brief 0 off, 1 on: the right superpower emblem over the panel's wrong one (destination from Status.json,
    /// its allegiance from EHT's `target` in edworld_eht, else from EDSM), 2 test: a bright frame where the patch goes, whatever the destination
    std::uint32_t patch{1};
    ///\brief the interface surface that carries the jump panel
    std::uint32_t patch_surface_width{3072};
    std::uint32_t patch_surface_height{660};
    ///\brief the patch's centre and size on that surface, in its pixels; the emblem's height
    float patch_x{1535.f};
    float patch_y{280.f};
    float patch_width{260.f};
    float patch_height{120.f};
    float patch_emblem_height{92.f};
    std::uint32_t patch_ground{0x020304u};
    ///\brief 0 = the destination's own; 1 Federation, 2 Empire, 3 Alliance = draw that emblem whatever the
    /// destination (placing the patch where the game shows no emblem, e.g. in deep space)
    std::uint32_t patch_force{0};
    ///\brief ask EDSM for the destination's allegiance (only the system's id goes out)
    bool edsm{true};
    };

  auto settings() noexcept -> settings_t const &;
  auto load_settings(std::wstring const & config_dir, std::wstring const & output_dir) -> void;

  auto log_open(std::wstring const & dir) -> void;
  ///\brief one line, time-stamped (UTC), flushed
  auto log_line(char const * fmt, ...) noexcept -> void;

  ///\brief once per device the game gets from the chain
  auto attach_to_device(ID3D11Device * device) noexcept -> void;
  }  // namespace edworld
