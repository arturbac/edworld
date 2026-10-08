// edworld — what the proxy's parts share: settings, the log, the hooks' entry point.
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <atomic>
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
    ///\brief 0 off, 1 on: the right superpower emblem over the panel's wrong one, the superpower read from the
    /// panel's own text (its SUPERPOWER row; no data source, no EDSM), placed beside that row and the two under it;
    /// 2 test: a bright frame where the patch goes, whatever the destination
    std::uint32_t patch{1};
    ///\brief the interface surface that carries the jump panel
    std::uint32_t patch_surface_width{3072};
    std::uint32_t patch_surface_height{660};
    ///\brief the patch's centre and size on that surface, in its pixels; the emblem's height. patch_y only until
    /// the SUPERPOWER row is read (the test frame), then 40 pixels under that row's centre
    float patch_x{1535.f};
    float patch_y{280.f};
    float patch_width{260.f};
    float patch_height{120.f};
    float patch_emblem_height{92.f};
    std::uint32_t patch_ground{0x020304u};
    ///\brief the independents' emblem's colour (the game's own is a dull blue); the others keep their superpower's
    std::uint32_t independent_colour{0xf2c14eu};
    ///\brief 0 = the destination's own; 1 Federation, 2 Empire, 3 Alliance, 4 Independent = draw that emblem whatever the
    /// destination (placing the patch where the game shows no emblem, e.g. in deep space)
    std::uint32_t patch_force{0};
    ///\brief ask EDSM for the destination's allegiance and factions (only the system's id goes out)
    bool edsm{true};

    // ---- the destination's factions, listed under the jump panel on the same surface ----
    ///\brief 0 off, 1 on: by influence, from EHT's `target` in edworld_eht when it has them, else from EDSM (marked so)
    std::uint32_t list{1};
    ///\brief the list's box in the panel surface's pixels, carried past its edge: horizontal centre, top edge, widest
    /// (the box is as wide as its rows); faction rows at most (EDSM's source line comes on top of them). The panel shows rows 2-493 of 3072x660 (its vertices, 2026-10-05);
    /// the list is a quad of its own under it in the panel's plane, not drawn on the surface (rows below 493 belong to
    /// other panels: a one-pixel strip at 519 is stretched over the jump panel)
    float list_x{1540.f};
    float list_top{500.f};
    float list_width{2200.f};
    std::uint32_t list_rows{7};
    ///\brief the text's height in surface pixels; the font is made at the first patch, so a change needs a restart
    float list_text{50.f};
    ///\brief the list's colours times this in the cockpit: its target is HDR (R11G11B10_FLOAT), and white text there
    /// blooms more than the panel's own
    float list_gain{0.8f};

    // ---- the compass (PoC): the HUD compass's dot read off its interface surface, the angles written beside it ----
    ///\brief 0 off, 1 the angles, 2 also everything known for checking them, 3 also a grid of the surface's pixels
    /// (the surface is a mosaic of HUD pieces placed around the cockpit; the grid shows which piece goes where)
    std::uint32_t compass{1};
    ///\brief the interface surface that carries the compass (the HUD's left cluster: heat, speed, fuel, target)
    std::uint32_t compass_surface_width{2200};
    std::uint32_t compass_surface_height{1800};
    ///\brief the compass disc's centre and rim radius on that surface (T8 and Kestrel, 2026-10-04/06)
    float compass_x{430.f};
    float compass_y{271.f};
    float compass_radius{54.f};
    ///\brief the angles' top left corner and height, in the surface's pixels: the GRAVITY WELL strip, shown above the
    /// radar (atlas x 1170-1580, y 815-900, seen with the grid on 2026-10-06, Kestrel)
    float compass_text_x{1185.f};
    float compass_text_y{822.f};
    float compass_text_size{34.f};
    ///\brief the checking lines (compass = 2): an empty piece shown above the ship's hologram (x 1600-2150, y 20-190)
    float compass_info_x{1620.f};
    float compass_info_y{24.f};
    float compass_info_size{18.f};
    ///\brief a line of the reading in the log at most this often; 0 = never
    std::uint32_t compass_log_ms{1000};
    ///\brief 1: two spheres of the target's direction as quads of edworld's own on the dashboard, in the planes of the two
    /// panels drawn first from the 2048x1280 surface (12 indices; the left one, then the right one): on the left the
    /// sphere seen from the front (as the compass, larger), on the right seen from behind, the left and above, with the
    /// place the target should be (compass_should_dive)
    std::uint32_t compass_spheres{1};
    ///\brief the quads' height in the panels' local units, their width over height (square on the screen: the panels'
    /// units are not square there), and their centres in each panel's local plane (2026-10-06, Kestrel: the panels' lower
    /// quads span x -0.10..0.18 / -0.18..0.10, y -0.22..-0.36)
    float compass_sphere_height{0.135f};
    float compass_sphere_aspect{1.13f};
    float compass_sphere_a_x{0.043f};
    float compass_sphere_a_y{-0.289f};
    float compass_sphere_c_x{-0.041f};
    float compass_sphere_c_y{-0.289f};
    ///\brief the nose this many degrees below the horizon is the approach to show (PoC: fixed); with the planet as the
    /// target, the target then is 90 minus this below the nose
    float compass_should_dive{35.f};
    ///\brief the spheres' colours taken from sRGB back to linear with this gamma (the cockpit is linear HDR: without it they
    /// came out half again as light and paler, 2026-10-06; 0 = as drawn), then scaled by the gain
    float compass_sphere_gamma{2.2f};
    ///\brief 1: each sphere anchored to a piece of the HUD the game places in every cockpit - the left one to the compass,
    /// the right one to the speed readout right of the radar - by the triangle of its draw holding that point of the HUD
    /// surface; so they keep their places by those pieces in every ship. 0: on the two side panels (compass_sphere_a/c,
    /// which differ from ship to ship)
    std::uint32_t compass_anchor{1};
    ///\brief the right sphere's anchor on the HUD surface: the speed readout (2026-10-06, the HUD surface 2200x1800)
    float compass_anchor_c_px{416.f};
    float compass_anchor_c_py{157.f};
    ///\brief anchored: a sphere's diameter in compass diameters, and each sphere's centre from its anchor in compass diameters
    /// (x to the right, y up); measured on the Kestrel, 2026-10-06
    float compass_anchor_scale{1.9f};
    float compass_anchor_a_dx{-0.41f};
    float compass_anchor_a_dy{0.45f};
    float compass_anchor_c_dx{1.22f};
    float compass_anchor_c_dy{0.83f};
    ///\brief 1: the spheres' lines in one wide band over each sphere (clear of the gauges under it, e.g. the thrust arc);
    /// 0: under it
    std::uint32_t compass_sphere_text_above{1};
    ///\brief 0: no angles in the strip above the radar (the spheres carry them); 1: written there
    std::uint32_t compass_text{0};
    ///\brief 1: the game's own compass cleared off the HUD surface once read (its square made transparent), the left
    /// sphere standing in for it; the game's returns whenever edworld is not there
    std::uint32_t compass_hide_game{0};
    float compass_sphere_gain{1.f};
    ///\brief the approach view (PoC): in normal flight (not supercruise, not gliding, neither docked nor landed) with the
    /// target this many degrees or more below the wings' plane, the left sphere shows the target from above (straight
    /// down in its middle) and the right one a dome standing on the target, the ship a point on it; 0 = never
    float compass_approach_below{20.f};
    ///\brief the dot's distance from the disc's centre when the target is on the rim, for the approach view: near a pad
    /// the dot sits there and the angle ahead or behind comes from how far short of it the dot is (Sjona's five dockings
    /// of 2026-10-07: the dot at 54.72..54.79 px with the pad below, compass_radius 54.46 from the rim's ring)
    float compass_approach_radius{54.8f};
    ///\brief in the approach view the dot's place is an exponential average of the readings with this time constant
    /// (seconds): near the rim a tenth of a pixel is a few degrees ahead or behind, and the sphere shook; 0 = no smoothing
    float compass_approach_smooth_s{0.4f};
    ///\brief the spheres' colours, 0xRRGGBB: a muted grey by default (doc/users.md has the 1.1.0-beta.1 blue as an
    /// example); the fainter shades (the far side, the disc's middle, the outline) are mixed from these
    ///\brief the disc behind a sphere, at its rim; its middle is a fifth of the way to compass_colour_rim
    std::uint32_t compass_colour_disc{0x101214u};
    ///\brief the rings at 30 and 60 degrees and the meridians
    std::uint32_t compass_colour_grid{0x5a6068u};
    ///\brief the front sphere's rim and the wings' plane on the right one
    std::uint32_t compass_colour_rim{0xa8aeb5u};
    ///\brief the target's dot
    std::uint32_t compass_colour_target{0xffffffu};
    ///\brief the target behind, where it should be, PUSH DOWN / PULL UP
    std::uint32_t compass_colour_behind{0xd9a15au};
    ///\brief the nose's arrow
    std::uint32_t compass_colour_nose{0x8cc9a0u};
    ///\brief the left sphere's angles
    std::uint32_t compass_colour_text_left{0xc9ced4u};
    ///\brief the right sphere's lines
    std::uint32_t compass_colour_text_right{0xd6cfb4u};
    ///\brief ON PATH; in the approach view the dome's base while the ship is over it
    std::uint32_t compass_colour_ok{0x9ad3a8u};
    ///\brief in the approach view the base while the ship is under it (the target above the wings)
    std::uint32_t compass_colour_wrong{0xc0504du};
    };

  auto settings() noexcept -> settings_t const &;
  auto load_settings(std::wstring const & config_dir, std::wstring const & output_dir) -> void;

  auto log_open(std::wstring const & dir) -> void;
  ///\brief one line, time-stamped (UTC), flushed
  auto log_line(char const * fmt, ...) noexcept -> void;

  ///\brief set while edworld creates its own shaders: they are never taken for the game's panels
  inline std::atomic<bool> creating_own{false};

  ///\brief once per device the game gets from the chain
  auto attach_to_device(ID3D11Device * device) noexcept -> void;
  }  // namespace edworld
