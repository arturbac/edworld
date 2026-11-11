// edworld — <plugin>.ini and <plugin>.log (edworld or edworld_eht, by build): in the directories edloader hands its
// plugins, else beside the dll; the names do not follow the dll's file name, which may be d3d11.dll.
#include "game_state.h"
#include "panel_math.h"
#include "runtime.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <iterator>
#include <mutex>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace edworld
  {
  namespace
    {
    settings_t current;
    std::FILE * log_file{};
    std::mutex log_mutex;

    auto trim(std::string_view s) -> std::string_view
      {
      while(not s.empty() and (s.front() == ' ' or s.front() == '\t'))
        s.remove_prefix(1);
      while(not s.empty() and (s.back() == ' ' or s.back() == '\t' or s.back() == '\r' or s.back() == '\n'))
        s.remove_suffix(1);
      return s;
      }

    auto widen(std::string_view s) -> std::wstring
      {
      if(s.empty())
        return {};
      int const n{MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)};
      std::wstring w(static_cast<std::size_t>(n), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
      return w;
      }

    auto to_uint(std::string_view s, std::uint32_t fallback) -> std::uint32_t
      {
      std::uint32_t v{};
      if(s.empty())
        return fallback;
      for(char const c: s)
        {
        if(c < '0' or c > '9')
          return fallback;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
        }
      return v;
      }
    auto to_float(std::string_view s, float fallback) -> float
      {
      std::string const text{s};
      char * end{};
      float const v{std::strtof(text.c_str(), &end)};
      return end and end != text.c_str() ? v : fallback;
      }

    auto narrow(std::wstring_view s) -> std::string
      {
      if(s.empty())
        return {};
      int const n{WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr)};
      std::string text(static_cast<std::size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), text.data(), n, nullptr, nullptr);
      return text;
      }

    struct hash_field_t
      {
      std::uint64_t settings_t::* field;
      };

    ///\brief 0xRRGGBB
    struct colour_field_t
      {
      std::uint32_t settings_t::* field;
      };

    struct list_field_t
      {
      std::vector<std::uint64_t> settings_t::* field;
      };

    ///\brief WIDTHxHEIGHT
    struct surface_field_t
      {
      std::uint32_t settings_t::* width;
      std::uint32_t settings_t::* height;
      };

    using field_t = std::variant<
      bool settings_t::*,
      std::uint32_t settings_t::*,
      float settings_t::*,
      std::wstring settings_t::*,
      hash_field_t,
      colour_field_t,
      list_field_t,
      surface_field_t>;

    ///\brief the topic a setting is listed under in doc/settings.md
    enum struct setting_group_e
      {
      clouds,
      navballs,
      jump_panel,
      other
      };

    ///\brief when a setting is read: at the game's start only, or again while the game runs (refresh_live_settings)
    enum struct setting_read_e
      {
      at_start,
      live
      };

    struct key_t
      {
      std::string_view name;
      field_t field;
      setting_group_e group;
      setting_read_e read;
      ///\brief the values that make sense, "(clamped)" where edworld holds the value to them; empty = any
      char const * range;
      ///\brief what the setting does, for doc/settings.md
      char const * about;
      ///\brief the comment over the key in the ini; the keys a player looks for first have one
      char const * note{};
      };

    using g = setting_group_e;
    using r = setting_read_e;

    // every setting once: read from the ini and written to it by this list, in the order the ini gets them; doc/settings.md
    // is made from it too (write_settings_doc), tools/test.sh checks the two agree
    constexpr key_t keys[]{
      {"next", &settings_t::next, g::other, r::at_start, "a dll's name or empty", "the next d3d11 mod in the chain (`d3d11_edhm.dll`, a mod's dll renamed); empty = the system's own, under edloader the next on its list", "Where edworld goes on to, the next d3d11 mod. Through edloader leave it empty: edworld goes on down edloader.txt. On its own: a mod's dll you renamed, d3d11_edhm.dll; empty = the system's own"},
      {"status_file", &settings_t::status_file, g::other, r::at_start, "a path or empty", "the game's `Status.json`, the journals beside it; empty = in Saved Games; for a game whose Saved Games lie elsewhere", "The game's Status.json, the journals beside it; change it only if edworld looks in the wrong place"},
      {"clouds", &settings_t::clouds, g::clouds, r::at_start, "0, 1", "clouds over a planet with an atmosphere that the game draws without any; `0` = none, and then nothing of the clouds is read again while the game runs", "Clouds over planets with an atmosphere: 0 = none"},
      {"clouds_height", &settings_t::clouds_height, g::clouds, r::at_start, "0..0.05", "the flat layer's height over the surface, as a part of the planet's radius", "the layer's height over the surface, as a part of the planet's radius"},
      {"clouds_alpha", &settings_t::clouds_alpha, g::clouds, r::at_start, "0..1", "how much the flat layer covers what is under it, `0` nothing to `1` all", "how much they cover what is under them, 0 to 1"},
      {"clouds_cover", &settings_t::clouds_cover, g::clouds, r::at_start, "0..1", "how much of the sky the clouds take, `0` clear to `1` overcast", "how much of the sky they take, 0 clear to 1 overcast"},
      {"clouds_light", &settings_t::clouds_light, g::clouds, r::at_start, "0..1", "how much of the star's light the clouds give back", "how much of the sun they give back"},
      {"clouds_volume", &settings_t::clouds_volume, g::clouds, r::at_start, "0, 1", "`1` = clouds with depth you fly through; `0` = the flat layer, lighter for a weaker graphics card", "Clouds with depth you fly through: 0 = the flat layer, lighter for a weaker graphics card"},
      {"clouds_bottom", &settings_t::clouds_bottom, g::clouds, r::at_start, "0..clouds_top", "where the clouds with depth begin over the surface, as a part of the planet's radius"},
      {"clouds_top", &settings_t::clouds_top, g::clouds, r::at_start, "over clouds_bottom", "where the clouds with depth end over the surface, as a part of the planet's radius"},
      {"clouds_steps", &settings_t::clouds_steps, g::clouds, r::at_start, "1 and more", "the steps a ray takes through the clouds with depth: fewer are lighter for the graphics card and coarser"},
      {"clouds_density", &settings_t::clouds_density, g::clouds, r::at_start, "0 and more", "how thick the clouds with depth are"},
      {"clouds_opacity", &settings_t::clouds_opacity, g::clouds, r::at_start, "0..1", "how much the clouds with depth cover at most; the kinds' clouds (`clouds_by_kind`) are scaled by it"},
      {"clouds_min_light", &settings_t::clouds_min_light, g::clouds, r::at_start, "0 and more", "the star's light the clouds with depth get at least, in the game's units, so a faint star does not lose them; the light's hue stays; `0` = as the game gives it", "the star's light the clouds with depth get at least, so a faint star does not lose them; 0 = as the game gives it"},
      {"clouds_under_gain", &settings_t::clouds_under_gain, g::clouds, r::at_start, "1 and more", "how much lighter the clouds with depth are seen from under them, against a day's sky; `1` = as they are", "how much lighter the clouds with depth are seen from under them, 1 = as they are"},
      {"clouds_detail", &settings_t::clouds_detail, g::clouds, r::at_start, "0..1", "how much the fine shape near you cuts the clouds with depth; `0` = none, a smooth fog from the ground", "how much the fine shape near you cuts the clouds with depth, 0 = none: a smooth fog from the ground"},
      {"clouds_detail_size", &settings_t::clouds_detail_size, g::clouds, r::at_start, "over 0", "the fine shape's largest lumps, in metres"},
      {"clouds_detail_range", &settings_t::clouds_detail_range, g::clouds, r::at_start, "over 0", "how far from you the fine shape reaches, in metres"},
      {"clouds_detail_edge", &settings_t::clouds_detail_edge, g::clouds, r::at_start, "over 0", "how sharp a cloud's edge is near you; more = sharper"},
      {"clouds_detail_gap", &settings_t::clouds_detail_gap, g::clouds, r::at_start, "0..1", "how much clear sky the fine shape opens among the clouds with depth near you, from under them too; more = thinner, the view from orbit stays", "how much clear sky there is among the clouds with depth near you, from under them too; more = thinner, the view from orbit stays"},
      {"clouds_wind_low", &settings_t::clouds_wind_low, g::clouds, r::at_start, "0 and more", "the wind at the floor of the clouds with depth, km/h; with `clouds_wind_high` both `0` = still clouds", "the wind in km/h at the floor of the clouds with depth and at their top; both 0 = still clouds"},
      {"clouds_wind_high", &settings_t::clouds_wind_high, g::clouds, r::at_start, "0 and more", "the wind at the top of the clouds with depth, km/h"},
      {"clouds_top_gain", &settings_t::clouds_top_gain, g::clouds, r::at_start, "1 and more", "how much lighter the clouds with depth are seen from above, over a light ground; `1` = as from under them", "how much lighter the clouds with depth are seen from above, 1 = as from under them"},
      {"clouds_mode", &settings_t::clouds_mode, g::clouds, r::live, "0, 1, 2", "(test) the clouds with depth from afar in fewer steps, the same shape: `0` = by your height, `1` = always near, `2` = always far", "(test) clouds with depth from afar in fewer steps: 0 = by the height, 1 = always near, 2 = always far; this and the four below change while the game runs"},
      {"clouds_far_steps", &settings_t::clouds_far_steps, g::clouds, r::live, "1 and more (clamped)", "the steps through the clouds with depth from afar"},
      {"clouds_near_from", &settings_t::clouds_near_from, g::clouds, r::live, "0 and more", "near steps under this height over the surface, as a part of the planet's radius; a blend up to `clouds_near_to`", "near steps under this height over the surface, far ones over the next, as parts of the planet's radius"},
      {"clouds_near_to", &settings_t::clouds_near_to, g::clouds, r::live, "over clouds_near_from (clamped)", "far steps over this height over the surface, as a part of the planet's radius"},
      {"clouds_debug", &settings_t::clouds_debug, g::clouds, r::live, "0, 1, 2", "(test) `1` = every pixel the clouds with depth are worked out for painted magenta, `2` = the game's fade of its atmosphere painted, green at 1, red at 0; `0` = off", "(test) 1 = the pixels of the clouds with depth painted magenta, 2 = the game's fade of its atmosphere painted, green at 1, red at 0"},
      {"clouds_game_fade", &settings_t::clouds_game_fade, g::clouds, r::live, "0, 1", "(test) `1` = the clouds with depth fade with the game's own atmosphere, `0` = never", "(test) 1 = the clouds with depth fade with the game's own atmosphere, 0 = never"},
      {"clouds_hide_below_px", &settings_t::clouds_hide_below_px, g::clouds, r::live, "0 and more", "no clouds with depth while the planet is narrower on the screen than this many pixels (of the picture the game draws, before its upscaling); a blend up to `clouds_full_from_px`", "no clouds with depth while the planet is smaller on the screen than this many pixels across, all of them from the next"},
      {"clouds_full_from_px", &settings_t::clouds_full_from_px, g::clouds, r::live, "over clouds_hide_below_px (clamped)", "all the clouds with depth from a planet this many pixels across"},
      {"clouds_max_steps", &settings_t::clouds_max_steps, g::clouds, r::live, "1 and more (clamped)", "(test) the most steps through the clouds with depth: the steps follow the way's length through them, many at the planet's edge", "(test) the most steps through the clouds with depth, taken where the way through them is long, at the planet's edge"},
      {"clouds_edge_hide_px", &settings_t::clouds_edge_hide_px, g::clouds, r::live, "0 and more", "no clouds with depth out of the planet's edge while their layer is thinner on the screen than this many pixels; a blend up to `clouds_edge_full_px`", "no clouds with depth out of the planet's edge while their layer is thinner on the screen than this many pixels, all of them from the next; on the disc the same within clouds_edge_band_px of the edge"},
      {"clouds_edge_full_px", &settings_t::clouds_edge_full_px, g::clouds, r::live, "over clouds_edge_hide_px (clamped)", "all the clouds with depth out of the planet's edge from a layer this many pixels thick on the screen"},
      {"clouds_edge_band_px", &settings_t::clouds_edge_band_px, g::clouds, r::live, "over 0 (clamped)", "on the disc, the same fading within this many pixels of the planet's edge"},
      {"clouds_haze", &settings_t::clouds_haze, g::clouds, r::live, "0 and more (clamped)", "from under the clouds with depth and in them, a cloud this far away covers e times less, so they fade into the haze at the horizon instead of ending in a sharp line; a part of the planet's radius, `0` = never less; from orbit they stay as they are", "from under the clouds with depth and in them, a cloud this far away covers e times less, so they fade into the haze at the horizon; a part of the planet's radius, 0 = never less"},
      {"clouds_kind_read_s", &settings_t::clouds_kind_read_s, g::clouds, r::live, "0 and more", "how often the game's atmosphere is read for its kind and the planet's day, in seconds; `0` = never, the clouds as in 1.2.0-beta.8", "how often the game's atmosphere is read for its kind and the planet's day, in seconds: 0 = never, the clouds as before"},
      {"clouds_by_kind", &settings_t::clouds_by_kind, g::clouds, r::live, "0, 1", "`1` = the clouds with depth by the atmosphere's kind (needs `clouds_kind_read_s`), `0` = the same on every planet", "1 = the clouds with depth by the atmosphere's kind (needs clouds_kind_read_s), 0 = the same on every planet"},
      {"clouds_physics_intensity", &settings_t::clouds_physics_intensity, g::clouds, r::live, "0 and more (clamped)", "how strongly the kinds' clouds show: `1` = as the physics gives, `2` = twice as strong (at most `clouds_opacity`) and a haze's cover up to clouds', `0` = none", "how strongly the kinds' clouds show: 1 = as the physics gives, 2 = twice as strong (at most clouds_opacity) and the cover of a haze up to clouds', 0 = none"},
      {"clouds_tint", &settings_t::clouds_tint, g::clouds, r::live, "0 and more (clamped)", "how strongly the kinds' clouds take their colour: `1` = a little off white as [clouds.md](clouds.md) gives, `0` = white on every planet, `2` = twice as far from white", "how strongly the kinds' clouds take their colour: 1 = a little off white, as the table gives, 0 = white, 2 = twice as far from white"},
      {"clouds_size_var", &settings_t::clouds_size_var, g::clouds, r::live, "0..0.9 (clamped)", "(PoC) the lumps' size noised by region: their scale times 1 +- this from a slow noise over the planet; `0` = one size everywhere", "(PoC) the lumps' size noised by region, their scale times 1 +- this: 0 = one size everywhere, at most 0.9"},
      {"clouds_cyclones", &settings_t::clouds_cyclones, g::clouds, r::live, "0, 1", "`1` = storms in the kinds' clouds by the planet's size, its day and the kind, `0` = none", "1 = storms in the kinds' clouds, by the planet's size, its day and the kind; 0 = none"},
      {"compass", &settings_t::compass, g::navballs, r::at_start, "0, 1, 2, 3", "`0` = no navballs, the game's compass is not read at all; `1` = the angles; `2` also everything known for checking them; `3` also a grid of the HUD surface's pixels", "The navballs beside the radar: 0 = none, the game's compass is not read at all"},
      {"edsm", &settings_t::edsm, g::jump_panel, r::at_start, "0, 1", "`1` = EDSM is asked for the destination's allegiance and factions, once per new destination, with the system's id only; `0` = never (no list then without EHT)", "The destination's factions under the jump panel: 0 = never ask EDSM"},
      {"list_alpha", &settings_t::list_alpha, g::jump_panel, r::at_start, "0..1 (clamped)", "how much the factions list's box covers what is behind it, `0` nothing to `1` all", "how much the list's box covers what is behind it, 0 to 1"},
#if defined(EDWORLD_EHT)
      {"shm_dir", &settings_t::shm_dir, g::other, r::at_start, "a folder or empty", "edworld_eht only: the folder shared with EHT as the game sees it (`Z:` is the Linux root under Wine); `panels` published there, `target` read from there; empty = neither", "The folder shared with EHT, as the game sees it"},
      {"shader_catalog", &settings_t::shader_catalog, g::other, r::at_start, "0, 1", "edworld_eht only: `1` keeps each shader the game creates, once per build of the game, in `edworld_shaders` beside the log; `0` none"},
      {"atmo_record_s", &settings_t::atmo_record_s, g::other, r::at_start, "0 and more", "edworld_eht only, (test): the atmosphere's fingerprint to `edworld_atmo` beside the log at most this often, in seconds; `0` = never", "(test) the atmosphere's fingerprint to edworld_atmo beside the log at most this often, in seconds: 0 = never"},
#endif
      {"patch", &settings_t::patch, g::jump_panel, r::at_start, "0, 1, 2", "`1` = the right superpower's emblem over the jump panel's wrong one, and the factions list; `2` = a test frame showing where they go; `0` = off", "The rest, all in doc/users.md"},
      {"patch_x", &settings_t::patch_x, g::jump_panel, r::at_start, "", "the patch's centre on the panel's surface, x, in its pixels"},
      {"patch_y", &settings_t::patch_y, g::jump_panel, r::at_start, "", "the patch's centre, y, until the panel's SUPERPOWER row is read (the test frame); then 40 pixels under that row"},
      {"patch_width", &settings_t::patch_width, g::jump_panel, r::at_start, "over 0", "the patch's width, in the surface's pixels"},
      {"patch_height", &settings_t::patch_height, g::jump_panel, r::at_start, "over 0", "the patch's height, in the surface's pixels"},
      {"patch_emblem_height", &settings_t::patch_emblem_height, g::jump_panel, r::at_start, "over 0", "the emblem's height, in the surface's pixels"},
      {"patch_surface", surface_field_t{&settings_t::patch_surface_width, &settings_t::patch_surface_height}, g::jump_panel, r::at_start, "WIDTHxHEIGHT", "the interface surface that carries the jump panel"},
      {"patch_ground", colour_field_t{&settings_t::patch_ground}, g::jump_panel, r::at_start, "RRGGBB", "the panel's own black under the patch"},
      {"independent_colour", colour_field_t{&settings_t::independent_colour}, g::jump_panel, r::at_start, "RRGGBB", "the independents' emblem's colour (the game's own is a dull blue); the others keep their superpower's"},
      {"patch_force", &settings_t::patch_force, g::jump_panel, r::at_start, "0..4", "`0` = the destination's own emblem; `1` Federation, `2` Empire, `3` Alliance, `4` Independent = that emblem whatever the destination (to place the patch where the game shows none)"},
      {"list", &settings_t::list, g::jump_panel, r::at_start, "0, 1", "`1` = the destination's factions by influence under the jump panel, from EHT's `target` in edworld_eht when it has them, else from EDSM (marked so); `0` = no list"},
      {"list_x", &settings_t::list_x, g::jump_panel, r::at_start, "", "the list's horizontal centre, in the panel surface's pixels"},
      {"list_top", &settings_t::list_top, g::jump_panel, r::at_start, "", "the list's top edge, in the panel surface's pixels, past the panel's edge"},
      {"list_width", &settings_t::list_width, g::jump_panel, r::at_start, "64..4096 (clamped)", "the list's box at its widest, in the panel surface's pixels; the box is as wide as its rows"},
      {"list_rows", &settings_t::list_rows, g::jump_panel, r::at_start, "1 and more (clamped)", "factions listed at most (EDSM's source line comes on top of them)"},
      {"list_text", &settings_t::list_text, g::jump_panel, r::at_start, "8..64 (clamped)", "the list's text height, in the panel surface's pixels; the font is made at the first patch"},
      {"list_gain", &settings_t::list_gain, g::jump_panel, r::at_start, "0 and more", "the list's colours times this in the cockpit (its target is HDR: white text there blooms more than the panel's own)"},
      {"compass_surface", surface_field_t{&settings_t::compass_surface_width, &settings_t::compass_surface_height}, g::navballs, r::at_start, "WIDTHxHEIGHT", "the interface surface that carries the HUD's compass (the HUD's left cluster)"},
      {"compass_x", &settings_t::compass_x, g::navballs, r::at_start, "", "the compass disc's centre on that surface, x, in its pixels"},
      {"compass_y", &settings_t::compass_y, g::navballs, r::at_start, "", "the compass disc's centre, y"},
      {"compass_radius", &settings_t::compass_radius, g::navballs, r::at_start, "over 0", "the compass disc's rim radius, in the surface's pixels"},
      {"compass_text_x", &settings_t::compass_text_x, g::navballs, r::at_start, "", "the angles' top left corner above the radar (`compass_text`), x, in the surface's pixels"},
      {"compass_text_y", &settings_t::compass_text_y, g::navballs, r::at_start, "", "the angles' top left corner, y"},
      {"compass_text_size", &settings_t::compass_text_size, g::navballs, r::at_start, "over 0", "the angles' height, in the surface's pixels"},
      {"compass_info_x", &settings_t::compass_info_x, g::navballs, r::at_start, "", "the checking lines (`compass = 2`), x, in the surface's pixels"},
      {"compass_info_y", &settings_t::compass_info_y, g::navballs, r::at_start, "", "the checking lines, y"},
      {"compass_info_size", &settings_t::compass_info_size, g::navballs, r::at_start, "over 0", "the checking lines' height, in the surface's pixels"},
      {"compass_spheres", &settings_t::compass_spheres, g::navballs, r::at_start, "0, 1", "`1` = the two navballs on the dashboard: the left one the target seen from the front, the right one from behind with the place the target should be; `0` = no spheres"},
      {"compass_sphere_height", &settings_t::compass_sphere_height, g::navballs, r::at_start, "over 0", "on the side panels (`compass_anchor = 0`): a sphere's height in the panels' local units"},
      {"compass_sphere_aspect", &settings_t::compass_sphere_aspect, g::navballs, r::at_start, "over 0", "on the side panels: a sphere's width over its height, so it is round on the screen"},
      {"compass_sphere_a_x", &settings_t::compass_sphere_a_x, g::navballs, r::at_start, "", "on the side panels: the left sphere's centre in its panel's local plane, x"},
      {"compass_sphere_a_y", &settings_t::compass_sphere_a_y, g::navballs, r::at_start, "", "on the side panels: the left sphere's centre, y"},
      {"compass_sphere_c_x", &settings_t::compass_sphere_c_x, g::navballs, r::at_start, "", "on the side panels: the right sphere's centre in its panel's local plane, x"},
      {"compass_sphere_c_y", &settings_t::compass_sphere_c_y, g::navballs, r::at_start, "", "on the side panels: the right sphere's centre, y"},
      {"compass_should_dive", &settings_t::compass_should_dive, g::navballs, r::at_start, "0..90", "the dive, in degrees below the horizon, the right sphere steers towards near a planet"},
      {"compass_sphere_gamma", &settings_t::compass_sphere_gamma, g::navballs, r::at_start, "0 and more", "the spheres' colours taken from sRGB back to linear with this gamma (the cockpit is linear HDR); `0` = as drawn"},
      {"compass_sphere_gain", &settings_t::compass_sphere_gain, g::navballs, r::at_start, "0 and more", "all the spheres' colours times this: lighter or darker"},
      {"compass_sphere_text_above", &settings_t::compass_sphere_text_above, g::navballs, r::at_start, "0, 1", "`1` = the spheres' lines in a band over them (at a planet the right sphere's go to the left band, under the angles), `0` = under them"},
      {"compass_anchor", &settings_t::compass_anchor, g::navballs, r::at_start, "0, 1", "`1` = the spheres held to the game's compass and speed readout, in their places in every ship; `0` = on the two side panels (`compass_sphere_*`, which differ from ship to ship)"},
      {"compass_anchor_c_px", &settings_t::compass_anchor_c_px, g::navballs, r::at_start, "", "the right sphere's anchor, the speed readout, on the HUD surface, x, in its pixels"},
      {"compass_anchor_c_py", &settings_t::compass_anchor_c_py, g::navballs, r::at_start, "", "the right sphere's anchor, y"},
      {"compass_anchor_scale", &settings_t::compass_anchor_scale, g::navballs, r::at_start, "over 0", "a sphere's diameter, in the game compass's diameters"},
      {"compass_anchor_a_dx", &settings_t::compass_anchor_a_dx, g::navballs, r::at_start, "", "the left sphere's centre from the compass, in compass diameters, x to the right"},
      {"compass_anchor_a_dy", &settings_t::compass_anchor_a_dy, g::navballs, r::at_start, "", "the left sphere's centre from the compass, y up"},
      {"compass_anchor_c_dx", &settings_t::compass_anchor_c_dx, g::navballs, r::at_start, "", "the right sphere's centre from the speed readout, in compass diameters, x to the right"},
      {"compass_anchor_c_dy", &settings_t::compass_anchor_c_dy, g::navballs, r::at_start, "", "the right sphere's centre from the speed readout, y up"},
      {"compass_text", &settings_t::compass_text, g::navballs, r::at_start, "0, 1", "`1` = the angles also written above the radar; `0` = the spheres carry them"},
      {"compass_hide_game", &settings_t::compass_hide_game, g::navballs, r::at_start, "0, 1", "`1` = the game's own compass hidden once read, the left sphere stands in for it; it returns whenever edworld is not there"},
      {"compass_colour_disc", colour_field_t{&settings_t::compass_colour_disc}, g::navballs, r::at_start, "RRGGBB", "the disc behind a sphere, at its rim; its middle is mixed towards `compass_colour_rim`"},
      {"compass_colour_grid", colour_field_t{&settings_t::compass_colour_grid}, g::navballs, r::at_start, "RRGGBB", "the rings at 30 and 60 degrees and the meridians"},
      {"compass_colour_rim", colour_field_t{&settings_t::compass_colour_rim}, g::navballs, r::at_start, "RRGGBB", "the left sphere's rim and the wings' plane on the right one"},
      {"compass_colour_target", colour_field_t{&settings_t::compass_colour_target}, g::navballs, r::at_start, "RRGGBB", "the target's dot"},
      {"compass_colour_behind", colour_field_t{&settings_t::compass_colour_behind}, g::navballs, r::at_start, "RRGGBB", "the target behind, where it should be, `PUSH DOWN` / `PULL UP`"},
      {"compass_colour_nose", colour_field_t{&settings_t::compass_colour_nose}, g::navballs, r::at_start, "RRGGBB", "the nose's arrow"},
      {"compass_colour_text_left", colour_field_t{&settings_t::compass_colour_text_left}, g::navballs, r::at_start, "RRGGBB", "the left sphere's angles"},
      {"compass_colour_text_right", colour_field_t{&settings_t::compass_colour_text_right}, g::navballs, r::at_start, "RRGGBB", "the right sphere's lines"},
      {"compass_colour_ok", colour_field_t{&settings_t::compass_colour_ok}, g::navballs, r::at_start, "RRGGBB", "`ON PATH`; the approach view's base while the ship is over it"},
      {"compass_colour_wrong", colour_field_t{&settings_t::compass_colour_wrong}, g::navballs, r::at_start, "RRGGBB", "the approach view's base while the ship is under it (in space only)"},
      {"compass_colour_pad_base", colour_field_t{&settings_t::compass_colour_pad_base}, g::navballs, r::at_start, "RRGGBB", "(test) the approach view's base while the pad's plane shows"},
      {"compass_colour_top", colour_field_t{&settings_t::compass_colour_top}, g::navballs, r::at_start, "RRGGBB", "(test) in the approach view the left sphere's ring: the target seen from above"},
      {"compass_approach_below", &settings_t::compass_approach_below, g::navballs, r::at_start, "0..90", "in normal flight with the target this many degrees or more below the wings, the approach view: the left sphere shows the target from above, the right one a dome standing on the target with the ship a point on it; `0` = never"},
      {"compass_approach_smooth_s", &settings_t::compass_approach_smooth_s, g::navballs, r::at_start, "0 and more", "in the approach view the dot's place is averaged with this time constant, in seconds, so the spheres do not shake near a pad; `0` = no averaging"},
      {"compass_approach_km", &settings_t::compass_approach_km, g::navballs, r::at_start, "0 and more", "the approach view also needs the target near: a place on a body within this many km, or a station or carrier in this system while mass locked; `0` = by the angle alone"},
      {"compass_approach_radius", &settings_t::compass_approach_radius, g::navballs, r::at_start, "over 0", "the dot's distance from the compass's centre with the target on its rim, in the HUD surface's pixels; the angle ahead or behind near a pad comes from how far short of it the dot is"},
      {"compass_stale_s", &settings_t::compass_stale_s, g::navballs, r::at_start, "0 and more", "a dot read holds this many seconds without a new one, then `NO DOT` and the approach view goes off; `0` = held until the next one"},
      {"compass_max_rate_dps", &settings_t::compass_max_rate_dps, g::navballs, r::at_start, "0 and more", "a dot further from the last one than the ship can turn in the time between (degrees a second, plus 5) is not taken; `0` = no such check"},
      {"compass_filled_min", &settings_t::compass_filled_min, g::navballs, r::at_start, "up to compass_filled_max", "a filled dot's pixels on the HUD surface at least; fewer is not the dot"},
      {"compass_filled_max", &settings_t::compass_filled_max, g::navballs, r::at_start, "", "a filled dot's pixels at most; more is not the dot"},
      {"compass_hollow_min", &settings_t::compass_hollow_min, g::navballs, r::at_start, "up to compass_hollow_max", "a hollow dot's (the target behind) pixels at least"},
      {"compass_hollow_max", &settings_t::compass_hollow_max, g::navballs, r::at_start, "", "a hollow dot's pixels at most"},
      {"compass_log_ms", &settings_t::compass_log_ms, g::navballs, r::at_start, "0 and more", "a line of the compass's reading in the log at most this often, in milliseconds; `0` = never"},
      {"gate_dive", &settings_t::gate_dive, g::navballs, r::at_start, "0..90", "(PoC, not drawn yet) the gates down to a place: the dive, in degrees below the planet's visible edge"},
      {"gate_dive_height", &settings_t::gate_dive_height, g::navballs, r::at_start, "over 0", "(PoC) the gates: the height of the point the dive starts from, in planet radii"},
      {"gate_arc", &settings_t::gate_arc, g::navballs, r::at_start, "0..180", "(PoC) the gates: the largest turn, in degrees, onto the flat line before the dive"},
      {"gate_dive_steepest", &settings_t::gate_dive_steepest, g::navballs, r::at_start, "0..90", "(PoC) the gates: the steepest dive, in degrees below the local horizontal; a straight line steeper than this sets up the approach instead"},
      {"station_axes", &settings_t::station_axes, g::navballs, r::at_start, "0, 1", "(test) `1` = the target's axes in the right sphere: the X, Y and Z of the model the game shows as the target's hologram; `0` = off"},
      {"pad_tilt_green_deg", &settings_t::pad_tilt_green_deg, g::navballs, r::at_start, "0..pad_tilt_red_deg", "(test, with `station_axes`) after the tower gives a pad, a disc of the pads' plane in the right sphere: green while the ship is tilted to the pads by at most this many degrees, then through yellow to red"},
      {"pad_tilt_red_deg", &settings_t::pad_tilt_red_deg, g::navballs, r::at_start, "over pad_tilt_green_deg", "(test) the pads' disc red from this many degrees of tilt on"},
      {"pad_text_samples", &settings_t::pad_text_samples, g::navballs, r::at_start, "0, 1", "(test) `1` = each time the HUD's notice box shows or changes its text, its area is written to `edworld_padtext` beside the log as a sample for learning the pad's digits; `0` = off"},
      {"pad_text_read", &settings_t::pad_text_read, g::navballs, r::at_start, "0, 1", "`1` = the pad's number and the grant's time read from the tower's notice on the HUD: edworld takes them as the pad given, edworld_eht logs whether they agree with EHT's; `0` = off"},
      {"pad_text_unsure_sample", &settings_t::pad_text_unsure_sample, g::navballs, r::at_start, "0, 1", "a notice whose pad number cannot be read sure gives no pad and a log line; `1` also keeps its picture once in `edworld_padtext` beside the log for learning the digits; `0` = the log line only"},
      {"station_vs", hash_field_t{&settings_t::station_vs}, g::navballs, r::at_start, "a shader hash or 0", "(test) the game's shader for the hologram's model; after a game update that changes it, `0`: edworld finds it by its draws and names it in the log"},
      {"log_interval_ms", &settings_t::log_interval_ms, g::other, r::at_start, "0 and more", "a summary line of the frame's panels in the log at most this often, in milliseconds; `0` = never"},
      {"frame_gap_us", &settings_t::frame_gap_us, g::other, r::at_start, "over 0", "a gap between two panel draws longer than this, in microseconds, starts a new frame"},
      {"log_all_vs", &settings_t::log_all_vs, g::other, r::at_start, "0, 1", "`1` = the hash of every vertex shader the game creates in the log (discovery)"},
      {"context_hook", &settings_t::context_hook, g::other, r::at_start, "0, 1, 2", "how the game's context is watched: `0` auto (a table of edworld's own when the context's methods are the system d3d11's, else in place), `1` in place, `2` a table of edworld's own whatever the context is"},
      {"watch_vs", list_field_t{&settings_t::watch_vs}, g::other, r::at_start, "shader hashes, comma separated", "the vertex shaders whose draws are panels: the cockpit holo panels, their GUI-effects-off twin and the one near a war settlement"},
      {"dump_vs", list_field_t{&settings_t::dump_vs}, g::other, r::at_start, "shader hashes, comma separated", "the vertex shaders a whole-frame dump looks into: their bytecode, buffers and what their pixel shader reads; the target's hologram"},
    };

    template<typename... functions_t>
    struct overloaded_t : functions_t...
      {
      using functions_t::operator()...;
      };

    auto read_value(settings_t & s, field_t const & field, std::string_view value) -> void
      {
      std::visit(
        overloaded_t{
          [&](bool settings_t::* f) { s.*f = value == "1" or value == "true"; },
          [&](std::uint32_t settings_t::* f) { s.*f = to_uint(value, s.*f); },
          [&](float settings_t::* f) { s.*f = to_float(value, s.*f); },
          [&](std::wstring settings_t::* f) { s.*f = widen(value); },
          [&](hash_field_t f) { s.*f.field = parse_hash(value).value_or(0); },
          [&](colour_field_t f)
          {
            if(auto const v{parse_hash(value)}; v and *v <= 0xffffffu)
              s.*f.field = static_cast<std::uint32_t>(*v);
          },
          [&](list_field_t f)
          {
            std::vector<std::uint64_t> & list{s.*f.field};
            list.clear();
            std::string_view rest{value};
            while(not rest.empty())
              {
              auto const comma{rest.find(',')};
              if(auto const h{parse_hash(rest.substr(0, comma))}; h)
                list.push_back(*h);
              if(comma == std::string_view::npos)
                break;
              rest.remove_prefix(comma + 1);
              }
          },
          [&](surface_field_t f)
          {
            auto const x{value.find('x')};
            if(x == std::string_view::npos)
              return;
            s.*f.width = to_uint(trim(value.substr(0, x)), s.*f.width);
            s.*f.height = to_uint(trim(value.substr(x + 1)), s.*f.height);
          }
        },
        field
      );
      }

    auto value_text(settings_t const & s, field_t const & field) -> std::string
      {
      char text[32];
      return std::visit(
        overloaded_t{
          [&](bool settings_t::* f) -> std::string { return s.*f ? "1" : "0"; },
          [&](std::uint32_t settings_t::* f) -> std::string { return std::to_string(s.*f); },
          [&](float settings_t::* f) -> std::string
          {
            std::snprintf(text, sizeof text, "%g", static_cast<double>(s.*f));
            return text;
          },
          [&](std::wstring settings_t::* f) -> std::string { return narrow(s.*f); },
          [&](hash_field_t f) -> std::string
          {
            std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(s.*f.field));
            return text;
          },
          [&](colour_field_t f) -> std::string
          {
            std::snprintf(text, sizeof text, "%06X", s.*f.field);
            return text;
          },
          [&](list_field_t f) -> std::string
          {
            std::string list;
            for(std::uint64_t const h: s.*f.field)
              {
              std::snprintf(text, sizeof text, "%s%016llX", list.empty() ? "" : ", ", static_cast<unsigned long long>(h));
              list += text;
              }
            return list;
          },
          [&](surface_field_t f) -> std::string
          {
            std::snprintf(text, sizeof text, "%ux%u", s.*f.width, s.*f.height);
            return text;
          }
        },
        field
      );
      }

    ///\brief the setting a line names, as a comment too: ";clouds = 1" -> clouds; empty for a line of words
    auto named_key(std::string_view line) -> std::string_view
      {
      line = trim(line);
      while(not line.empty() and (line.front() == ';' or line.front() == '#' or line.front() == ' '))
        line.remove_prefix(1);
      auto const eq{line.find('=')};
      return eq == std::string_view::npos ? std::string_view{} : trim(line.substr(0, eq));
      }

    auto read_whole(std::wstring const & path) -> std::string
      {
      std::string text;
      if(std::FILE * const f{_wfopen(path.c_str(), L"rb")})
        {
        char block[4096];
        while(std::size_t const n{std::fread(block, 1, sizeof block, f)})
          text.append(block, n);
        std::fclose(f);
        }
      return text;
      }

    ///\brief the lines for the settings `ini` names nowhere, with the values of `s`
    auto missing_lines(std::string_view ini, settings_t const & s, std::size_t & count) -> std::string
      {
      std::vector<std::string_view> named;
      for(std::string_view rest{ini}; not rest.empty();)
        {
        auto const end{rest.find('\n')};
        named.push_back(named_key(rest.substr(0, end)));
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        }
      std::string lines;
      for(key_t const & key: keys)
        {
        if(std::find(named.begin(), named.end(), key.name) != named.end())
          continue;
        if(key.note)
          (lines += "\n; ") += key.note, lines += '\n';
        ((lines += ';') += key.name) += " = ";
        lines += value_text(s, key.field);
        lines += '\n';
        ++count;
        }
      return lines;
      }

    ///\brief one setting's value from `from` into `to`
    auto copy_value(settings_t & to, settings_t const & from, field_t const & field) -> void
      {
      std::visit(
        overloaded_t{
          [&]<typename value_t>(value_t settings_t::* f) { to.*f = from.*f; },
          [&](hash_field_t f) { to.*f.field = from.*f.field; },
          [&](colour_field_t f) { to.*f.field = from.*f.field; },
          [&](list_field_t f) { to.*f.field = from.*f.field; },
          [&](surface_field_t f)
          {
            to.*f.width = from.*f.width;
            to.*f.height = from.*f.height;
          }
        },
        field
      );
      }

    ///\brief doc/settings.md: every setting of this build by its topic, those read while the game runs first, with the
    /// default, the range and what it does
    auto settings_doc() -> std::string
      {
      struct group_title_t
        {
        setting_group_e group;
        char const * title;
        };
      constexpr group_title_t groups[]{
        {g::clouds, "Clouds"}, {g::navballs, "Navballs"}, {g::jump_panel, "Jump panel"}, {g::other, "Other"}
      };
      settings_t const defaults{};
      std::string doc{
        "# edworld settings\n\n"
        "<!-- Made by edworld from its table of settings (src/settings_log.cc, `EDWORLD_SETTINGS_DOC`); tools/test.sh checks\n"
        "     it is up to date. Change the table, not this file. -->\n\n"
        "In `edworld.ini` (or `edworld_eht.ini`), beside the dll or in `edloader\\config`; how the file is made and kept: "
        "[users.md](users.md#settings).\n\n"
        "- **Read while the game runs**: edworld looks at the ini at most once a second while it draws clouds (a planet "
        "with an atmosphere on the screen, `clouds = 1`); a change shows within a second or two and the log has "
        "`settings: read again: ...`.\n"
        "- **Read at the game's start**: a change takes a new start of the game.\n"
        "- **Range**: the values that make sense; *(clamped)* = edworld holds the value to it, elsewhere it does not check.\n"
      };
      for(group_title_t const & group: groups)
        {
        ((doc += "\n## ") += group.title) += '\n';
        for(setting_read_e const read: {r::live, r::at_start})
          {
          doc += read == r::live ? "\n### Read while the game runs\n\n" : "\n### Read at the game's start (a change needs a restart)\n\n";
          std::string rows;
          for(key_t const & key: keys)
            {
            if(key.group != group.group or key.read != read)
              continue;
            std::string const value{value_text(defaults, key.field)};
            ((rows += "| `") += key.name) += "` | ";
            rows += value.empty() ? std::string{"empty"} : '`' + value + '`';
            ((rows += " | ") += key.range) += " | ";
            (rows += key.about) += " |\n";
            }
          if(rows.empty())
            doc += "None.\n";
          else
            (doc += "| setting | default | range | what |\n|---|---|---|---|\n") += rows;
          }
        }
      return doc;
      }
    }  // namespace

  auto settings() noexcept -> settings_t const & { return current; }

  namespace
    {
    std::wstring ini_path;

    auto read_ini(std::wstring const & path, settings_t & s) -> bool
      {
      std::FILE * f{_wfopen(path.c_str(), L"rb")};
      if(not f)
        return false;
      char line[1024];
      while(std::fgets(line, sizeof line, f))
        {
        std::string_view const text{trim(line)};
        if(text.empty() or text.front() == ';' or text.front() == '#' or text.front() == '[')
          continue;
        auto const eq{text.find('=')};
        if(eq == std::string_view::npos)
          continue;
        std::string_view const key{trim(text.substr(0, eq))};
        std::string_view const value{trim(text.substr(eq + 1))};
        auto const found{std::find_if(std::begin(keys), std::end(keys), [&](key_t const & k) { return k.name == key; })};
        if(found != std::end(keys))
          read_value(s, found->field, value);
        }
      std::fclose(f);
      return true;
      }
    }  // namespace

  auto load_settings(std::wstring const & config_dir, std::wstring const & output_dir) -> void
    {
    current.dir = output_dir;
    ini_path = config_dir + L"\\" + plugin_name + L".ini";
    read_ini(ini_path, current);
    }

  auto refresh_live_settings() noexcept -> void
    {
    static ULONGLONG looked{};
    static FILETIME written{};
    ULONGLONG const now{GetTickCount64()};
    if(now - looked < 1000u or ini_path.empty())
      return;
    looked = now;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(not GetFileAttributesExW(ini_path.c_str(), GetFileExInfoStandard, &data))
      return;
    if(CompareFileTime(&data.ftLastWriteTime, &written) == 0)
      return;
    bool const first{written.dwLowDateTime == 0u and written.dwHighDateTime == 0u};
    written = data.ftLastWriteTime;
    if(first)
      return;  // the ini load_settings read
    try
      {
      settings_t read{current};
      if(not read_ini(ini_path, read))
        return;
      std::string values;
      for(key_t const & key: keys)
        {
        if(key.read != r::live)
          continue;
        copy_value(current, read, key.field);
        ((values += values.empty() ? "" : ", ") += key.name) += ' ';
        values += value_text(current, key.field);
        }
      log_line("settings: read again: %s", values.c_str());
      }
    catch(std::exception const & e)
      {
      log_line("settings: the ini not read again: %s", e.what());
      }
    catch(...)
      {
      log_line("settings: the ini not read again");
      }
    }

  auto complete_settings(std::wstring const & config_dir) noexcept -> void
    {
    try
      {
      std::wstring const path{config_dir + L"\\" + plugin_name + L".ini"};
      std::string const ini{read_whole(path)};
      settings_t running{current};
      running.status_file = status_path();
      std::size_t count{};
      std::string const lines{missing_lines(ini, running, count)};
      if(count == 0)
        return;
      std::FILE * const f{_wfopen(path.c_str(), L"ab")};
      if(not f)
        {
        log_line("settings: %S cannot be written (error %d); edworld runs on as it is", path.c_str(), errno);
        return;
        }
      std::string text;
      if(ini.empty())
        text = "; edworld's settings, written at its first start. Take the ';' off the start of a line to change it;\n"
               "; a change takes a new start of the game, but for those doc/settings.md lists as read while it runs.\n"
               "; A line left as a comment follows edworld's default.\n";
      else
        text = (ini.back() == '\n' ? "\n" : "\n\n") + std::string{"; Settings this file did not name yet, added by edworld.\n"};
      text += lines;
      bool const written{std::fwrite(text.data(), 1, text.size(), f) == text.size()};
      bool const closed{std::fclose(f) == 0};
      if(written and closed)
        log_line("settings: %zu added to %S", count, path.c_str());
      else
        log_line("settings: writing %S failed; edworld runs on as it is", path.c_str());
      }
    catch(std::exception const & e)
      {
      log_line("settings: the ini not completed: %s", e.what());
      }
    catch(...)
      {
      log_line("settings: the ini not completed");
      }
    }

  auto write_settings_doc() noexcept -> void
    {
    wchar_t path[MAX_PATH]{};
    DWORD const n{GetEnvironmentVariableW(L"EDWORLD_SETTINGS_DOC", path, MAX_PATH)};
    if(n == 0u or n >= MAX_PATH)
      return;
    try
      {
      std::string const doc{settings_doc()};
      std::FILE * const f{_wfopen(path, L"wb")};
      if(not f)
        {
        log_line("settings: the settings' doc %S cannot be written (error %d)", path, errno);
        return;
        }
      bool const written{std::fwrite(doc.data(), 1, doc.size(), f) == doc.size()};
      bool const closed{std::fclose(f) == 0};
      log_line(written and closed ? "settings: the settings' doc written to %S" : "settings: writing the settings' doc %S failed", path);
      }
    catch(std::exception const & e)
      {
      log_line("settings: the settings' doc not written: %s", e.what());
      }
    catch(...)
      {
      log_line("settings: the settings' doc not written");
      }
    }

  // One file per game session: the last session's log is set aside under the time it was last written,
  // <plugin>.<UTC>.log, before this one starts - the tool beside the game takes those away (EHT's backup).
  auto log_open(std::wstring const & dir) -> void
    {
    std::wstring const path{dir + L"\\" + plugin_name + L".log"};
    bool set_aside_failed{};
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) and (a.nFileSizeHigh != 0 or a.nFileSizeLow != 0))
      {
      SYSTEMTIME t{};
      FileTimeToSystemTime(&a.ftLastWriteTime, &t);
      wchar_t aside[64];
      std::swprintf(aside, 64, L"\\%ls.%04u%02u%02uT%02u%02u%02uZ.log", plugin_name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      set_aside_failed = not MoveFileExW(path.c_str(), (dir + aside).c_str(), 0);
      }
    log_file = _wfopen(path.c_str(), L"ab");
    if(set_aside_failed)
      log_line("log: the last session's log could not be set aside (error %lu); appending to it", GetLastError());
    }

  auto log_line(char const * fmt, ...) noexcept -> void
    {
    if(not log_file)
      return;
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[2048];
    int n{std::snprintf(
      buf,
      sizeof buf,
      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ ",
      t.wYear,
      t.wMonth,
      t.wDay,
      t.wHour,
      t.wMinute,
      t.wSecond,
      t.wMilliseconds
    )};
    va_list args;
    va_start(args, fmt);
    int const m{std::vsnprintf(buf + n, sizeof buf - static_cast<std::size_t>(n) - 2, fmt, args)};
    va_end(args);
    n = m < 0 ? n : std::min<int>(n + m, static_cast<int>(sizeof buf) - 2);
    buf[n++] = '\n';
    std::lock_guard const lock{log_mutex};
    std::fwrite(buf, 1, static_cast<std::size_t>(n), log_file);
    std::fflush(log_file);
    }
  }  // namespace edworld
