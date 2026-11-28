// edworld — <plugin>.ini and <plugin>.log (edworld or edworld_eht, by build): in the directories edloader hands its
// plugins, else beside the dll; the names do not follow the dll's file name, which may be d3d11.dll. The ini is read,
// completed and watched by edplugin_common's ini module from the table below.
#include "clouds_values.h"
#include "game_state.h"
#include "panel_math.h"
#include "runtime.h"

#include <edplugin_common/ini/ini.h>
#include <edplugin_common/ini/live.h>
#include <edplugin_common/settings/settings_doc.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stralgo/stralgo.h>

namespace edworld
  {
  using namespace std::string_view_literals;

  namespace
    {
    using edplugin_common::value_e;
    using hex_t = edplugin_common::ini_hex_t<settings_t>;
    using colour_t = edplugin_common::ini_colour_t<settings_t>;
    using list_t = edplugin_common::ini_list_t<settings_t>;

    ///\brief WIDTHxHEIGHT, two whole numbers
    template<auto width, auto height>
    constexpr auto surface() noexcept -> edplugin_common::ini_custom_t<settings_t>
      {
      return {[](settings_t & s, std::string_view value) -> bool
                {
                auto const x{value.find('x')};
                if(x == std::string_view::npos)
                  return false;
                auto const w{edplugin_common::read_uint32(edplugin_common::ini_trim(value.substr(0, x)))};
                auto const h{edplugin_common::read_uint32(edplugin_common::ini_trim(value.substr(x + 1)))};
                if(not w or not h)
                  return false;
                s.*width = *w;
                s.*height = *h;
                return true;
                },
              [](settings_t const & s) -> std::string { return std::format("{}x{}", s.*width, s.*height); },
              [](settings_t & to, settings_t const & from) -> void
                {
                to.*width = from.*width;
                to.*height = from.*height;
                }};
      }

    ///\brief the topic a setting is listed under in doc/settings.md, in its order (groups below)
    enum struct setting_group_e : std::size_t
      {
      clouds,
      navballs,
      jump_panel,
      other
      };

    constexpr std::string_view groups[]{"Clouds"sv, "Navballs"sv, "Jump panel"sv, "Other"sv};

    using setting_read_e = edplugin_common::setting_read_e;

    ///\brief a row as this table writes it, `note` (the comment over the key in the ini; the keys a player looks for
    /// first have one) after `about` (what the setting does, for doc/settings.md)
    struct key_t
      {
      std::string_view name;
      edplugin_common::ini_field_t<settings_t> field;
      setting_group_e group;
      setting_read_e read;
      ///\brief the values that make sense, "(clamped)" where edworld holds the value to them; empty = any. Also in the
      /// note of a line that cannot be read
      char const * range;
      ///\brief what the setting does, for doc/settings.md
      char const * about;
      ///\brief the comment over the key in the ini; none for most
      char const * note{};
      ///\brief raw for a path, a folder or a dll's name: no comment after its value (`D:\Games #2\...`)
      value_e value{value_e::plain};
      };

    using g = setting_group_e;
    using r = setting_read_e;

    // every setting once: read from the ini and written to it by this list, in the order the ini gets them;
    // doc/settings.md is made from it too (write_settings_doc), tools/doc.sh checks the two agree
    constexpr key_t keys[]{
      {"next", &settings_t::next, g::other, r::at_start, "a dll's name or empty", "the next d3d11 mod in the chain (`d3d11_edhm.dll`, a mod's dll renamed); empty = the system's own, under edloader the next on its list", "Where edworld goes on to, the next d3d11 mod. Through edloader leave it empty: edworld goes on down edloader.txt. On its own: a mod's dll you renamed, d3d11_edhm.dll; empty = the system's own", value_e::raw},
      {"creator_pass", &settings_t::creator_pass, g::other, r::at_start, "dll names by commas, or empty", "dlls passed over when edworld tells who asks for a device: a program that puts itself in the game's own call (an antivirus's hooks; `bdhkm64.dll`, seen in a player's log, by default); without it a device asked through such a dll is taken for an overlay's and not followed, and edworld draws nothing. The log's `not followed` line names the dll"},
      {"build_info", &settings_t::build_info, g::other, r::at_start, "0..4", "edworld's name and version, small and grey, in a corner of the picture (the swap chain's back buffer, after the game's own drawing of it; in a picture wider than 16:9 - one wide window or a desktop over several monitors - a corner of its middle part of the picture's height x 16/9, where the game lays its HUD): `1` top left, `2` top right, `3` bottom right, `4` bottom left; `0` = none; edupscale's and edwide_screen's lines (version, what they do now) beside it when they are loaded; edworld_eht's says it is off when a 3Dmigoto (EDHM) stands before it in the chain. The log's `build_info:` line says where it went, or that no back buffer was seen", "edworld's version in a corner of the screen: 1 top left, 2 top right, 3 bottom right, 4 bottom left, 0 = none"},
      {"status_file", &settings_t::status_file, g::other, r::at_start, "a path or empty", "the game's `Status.json`, the journals beside it; empty = in Saved Games; for a game whose Saved Games lie elsewhere", "The game's Status.json, the journals beside it; empty = the Saved Games folder; change it only if edworld looks in the wrong place", value_e::raw},
      {"clouds", &settings_t::clouds, g::clouds, r::at_start, "0, 1", "clouds over a planet with an atmosphere that the game draws without any; `0` = none, and then nothing of the clouds is read again while the game runs", "Clouds over planets with an atmosphere: 0 = none"},
      {"clouds_height", &settings_t::clouds_height, g::clouds, r::at_start, "0..0.05", "the flat layer's height over the surface, as a part of the planet's radius", "the layer's height over the surface, as a part of the planet's radius"},
      {"clouds_alpha", &settings_t::clouds_alpha, g::clouds, r::at_start, "0..1", "how much the flat layer covers what is under it, `0` nothing to `1` all", "how much they cover what is under them, 0 to 1"},
      {"clouds_cover", &settings_t::clouds_cover, g::clouds, r::at_start, "0..1", "how much of the sky the clouds take, `0` clear to `1` overcast", "how much of the sky they take, 0 clear to 1 overcast"},
      {"clouds_light", &settings_t::clouds_light, g::clouds, r::at_start, "0..1", "how much of the star's light the clouds give back", "how much of the sun they give back"},
      {"clouds_volume", &settings_t::clouds_volume, g::clouds, r::at_start, "0, 1", "`1` = clouds with depth you fly through; `0` = the flat layer, lighter for a weaker graphics card", "Clouds with depth you fly through: 0 = the flat layer, lighter for a weaker graphics card"},
      {"clouds_bottom", &settings_t::clouds_bottom, g::clouds, r::at_start, "0..clouds_top", "where the clouds with depth begin over the surface, as a part of the planet's radius"},
      {"clouds_top", &settings_t::clouds_top, g::clouds, r::at_start, "over clouds_bottom", "where the clouds with depth end over the surface, as a part of the planet's radius"},
      {"clouds_steps", &settings_t::clouds_steps, g::clouds, r::at_start, "1..256 (clamped)", "the steps a ray takes through the clouds with depth: fewer are lighter for the graphics card and coarser"},
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
      {"clouds_noise_time", &settings_t::clouds_noise_time, g::clouds, r::live, "0, 1, 2", "the grain of the clouds with depth, the aurora and the snow (where each pixel's steps start) moved each frame: `1` = while edupscale's temporal anti-aliasing jitters the camera, so it averages the grain instead of flickering; `0` = never; `2` = always, also without it"},
      {"clouds_far_steps", &settings_t::clouds_far_steps, g::clouds, r::live, "1..256 (clamped)", "the steps through the clouds with depth from afar"},
      {"clouds_near_from", &settings_t::clouds_near_from, g::clouds, r::live, "0 and more", "near steps under this height over the surface, as a part of the planet's radius; a blend up to `clouds_near_to`", "near steps under this height over the surface, far ones over the next, as parts of the planet's radius"},
      {"clouds_near_to", &settings_t::clouds_near_to, g::clouds, r::live, "over clouds_near_from (clamped)", "far steps over this height over the surface, as a part of the planet's radius"},
      {"clouds_debug", &settings_t::clouds_debug, g::clouds, r::live, "0, 1, 2", "(test) `1` = every pixel the clouds with depth are worked out for painted magenta, `2` = the game's fade of its atmosphere painted, green at 1, red at 0; `0` = off", "(test) 1 = the pixels of the clouds with depth painted magenta, 2 = the game's fade of its atmosphere painted, green at 1, red at 0"},
      {"clouds_game_fade", &settings_t::clouds_game_fade, g::clouds, r::live, "0, 1", "(test) `1` = the clouds with depth fade with the game's own atmosphere, `0` = never", "(test) 1 = the clouds with depth fade with the game's own atmosphere, 0 = never"},
      {"clouds_hide_below_px", &settings_t::clouds_hide_below_px, g::clouds, r::live, "0 and more", "no clouds with depth while the planet is narrower on the screen than this many pixels (of the picture the game draws, before its upscaling); a blend up to `clouds_full_from_px`", "no clouds with depth while the planet is smaller on the screen than this many pixels across, all of them from the next"},
      {"clouds_full_from_px", &settings_t::clouds_full_from_px, g::clouds, r::live, "over clouds_hide_below_px (clamped)", "all the clouds with depth from a planet this many pixels across"},
      {"clouds_max_steps", &settings_t::clouds_max_steps, g::clouds, r::live, "1..256 (clamped)", "(test) the most steps through the clouds with depth: the steps follow the way's length through them, many at the planet's edge", "(test) the most steps through the clouds with depth, taken where the way through them is long, at the planet's edge"},
      {"clouds_edge_hide_px", &settings_t::clouds_edge_hide_px, g::clouds, r::live, "0 and more", "no clouds with depth out of the planet's edge while their layer is thinner on the screen than this many pixels; a blend up to `clouds_edge_full_px`", "no clouds with depth out of the planet's edge while their layer is thinner on the screen than this many pixels, all of them from the next; on the disc the same within clouds_edge_band_px of the edge"},
      {"clouds_edge_full_px", &settings_t::clouds_edge_full_px, g::clouds, r::live, "over clouds_edge_hide_px (clamped)", "all the clouds with depth out of the planet's edge from a layer this many pixels thick on the screen"},
      {"clouds_edge_band_px", &settings_t::clouds_edge_band_px, g::clouds, r::live, "over 0 (clamped)", "on the disc, the same fading within this many pixels of the planet's edge"},
      {"clouds_haze", &settings_t::clouds_haze, g::clouds, r::live, "0 and more (clamped)", "from under the clouds with depth and in them, a cloud this far away covers e times less, so they fade into the haze at the horizon instead of ending in a sharp line; a part of the planet's radius, `0` = never less; from orbit they stay as they are", "from under the clouds with depth and in them, a cloud this far away covers e times less, so they fade into the haze at the horizon; a part of the planet's radius, 0 = never less"},
      {"clouds_kind_read_s", &settings_t::clouds_kind_read_s, g::clouds, r::live, "0 and more", "how often the game's atmosphere is read for its kind and the planet's day, in seconds; `0` = never, the same clouds on every planet, by the ini", "how often the game's atmosphere is read for its kind and the planet's day, in seconds: 0 = never, the clouds as before"},
      {"clouds_by_kind", &settings_t::clouds_by_kind, g::clouds, r::live, "0, 1", "`1` = the clouds with depth by the atmosphere's kind (needs `clouds_kind_read_s`), `0` = the same on every planet", "1 = the clouds with depth by the atmosphere's kind (needs clouds_kind_read_s), 0 = the same on every planet"},
      {"clouds_physics_intensity", &settings_t::clouds_physics_intensity, g::clouds, r::live, "0 and more (clamped)", "how strongly the kinds' clouds show: `1` = as the physics gives, `2` = twice as strong (at most `clouds_opacity`) and a haze's cover up to clouds', `0` = none", "how strongly the kinds' clouds show: 1 = as the physics gives, 2 = twice as strong (at most clouds_opacity) and the cover of a haze up to clouds', 0 = none"},
      {"clouds_tint", &settings_t::clouds_tint, g::clouds, r::live, "0 and more (clamped)", "how strongly the kinds' clouds take their colour: `1` = a little off white as [clouds.md](clouds.md) gives, `0` = white on every planet, `2` = twice as far from white", "how strongly the kinds' clouds take their colour: 1 = a little off white, as the table gives, 0 = white, 2 = twice as far from white"},
      {"clouds_size_var", &settings_t::clouds_size_var, g::clouds, r::live, "0..0.9 (clamped)", "the lumps' size noised by region: their scale times 1 +- this from a slow noise over the planet; `0` = one size everywhere", "the lumps' size noised by region, their scale times 1 +- this: 0 = one size everywhere, at most 0.9"},
      {"clouds_cyclones", &settings_t::clouds_cyclones, g::clouds, r::live, "0, 1", "`1` = storms in the kinds' clouds by the planet's size, its day and the kind, `0` = none", "1 = storms in the kinds' clouds, by the planet's size, its day and the kind; 0 = none"},
      {"clouds_lumps_by_layer", &settings_t::clouds_lumps_by_layer, g::clouds, r::live, "0, 1", "`1` = the kinds' lumps by the layer's thickness: larger on small planets, smaller on large ones, the fine shape near you a quarter of the layer, each lump as high as it is strong; `0` = as before", "1 = the kinds' lumps by the layer's thickness and each as high as it is strong, 0 = as before"},
      {"clouds_weather", &settings_t::clouds_weather, g::clouds, r::live, "0..2 (clamped)", "the weather in the kinds' clouds: overcast and clear regions by how fast the planet turns for its size; `1` = as [clouds.md](clouds.md) gives, `0` = none, `2` = twice as strong", "the weather in the kinds' clouds, overcast and clear regions: 1 = as designed, 0 = none, 2 = twice as strong"},
      {"aurora", &settings_t::aurora, g::clouds, r::live, "0, 1", "`1` = an aurora over the night side of a planet whose atmosphere's kind is known, as [clouds.md](clouds.md#aurora) gives; `0` = none", "1 = an aurora over the night side of a planet whose atmosphere's kind is known, 0 = none"},
      {"aurora_gain", &settings_t::aurora_gain, g::clouds, r::live, "0..3 (clamped)", "how bright the aurora is; `1` = as designed", "how bright the aurora is, 1 = as designed"},
      {"aurora_activity", &settings_t::aurora_activity, g::clouds, r::live, "-1, 0..9 (clamped)", "(test) how stormy: `-1` = by the time, each 3 hours its own and the same for everyone; `0`..`9` = always this, as the Kp index", "(test) how stormy: -1 = by the time, 0..9 = always this, as the Kp index"},
      {"aurora_dance", &settings_t::aurora_dance, g::clouds, r::live, "0..2 (clamped)", "how much the aurora moves in seconds in a substorm: waves along its arc, folds, wiggles, curls; `1` = as designed, `0` = none of these (its rays still grow and fade), `2` = twice as much", "how much the aurora moves in seconds in a substorm: 1 = as designed, 0 = none (its rays still change), 2 = twice as much"},
      {"aurora_flicker", &settings_t::aurora_flicker, g::clouds, r::live, "0, 1", "`1` = small patches of the aurora flicker a few times a second in a substorm, as real ones do; `0` = none", "1 = small patches of the aurora flicker a few times a second in a substorm, 0 = none"},
      {"aurora_field", &settings_t::aurora_field, g::clouds, r::live, "-1, 0, 1", "(test) whether the planet has a magnetic field of its own: `-1` = as guessed from its size and turn, `1` = always (the whole aurora on any planet whose kind is known), `0` = never (a faint glow only)", "(test) the planet's own field: -1 = as guessed from its size and turn, 1 = always, 0 = never"},
      {"compass", &settings_t::compass, g::navballs, r::at_start, "0, 1, 2, 3", "`0` = no navballs, the game's compass is not read at all; `1` = the angles; `2` also everything known for checking them; `3` also a grid of the HUD surface's pixels", "The navballs beside the radar: 0 = none, the game's compass is not read at all"},
      {"edsm", &settings_t::edsm, g::jump_panel, r::at_start, "0, 1", "`1` = EDSM is asked for the destination's allegiance and factions, once per new destination, with the system's id only; `0` = never (no list then without EHT)", "The destination's factions under the jump panel: 0 = never ask EDSM"},
      {"list_alpha", &settings_t::list_alpha, g::jump_panel, r::at_start, "0..1 (clamped)", "how much the factions list's box covers what is behind it, `0` nothing to `1` all", "how much the list's box covers what is behind it, 0 to 1"},
#if defined(EDWORLD_EHT)
      {"shm_dir", &settings_t::shm_dir, g::other, r::at_start, "a folder or empty", "edworld_eht only: the folder shared with EHT as the game sees it (`Z:` is the Linux root under Wine); `panels` published there, `target` read from there; empty = neither", "The folder shared with EHT, as the game sees it", value_e::raw},
      {"shader_catalog", &settings_t::shader_catalog, g::other, r::at_start, "0, 1", "edworld_eht only: `1` keeps each shader the game creates, once per build of the game, in `edworld_shaders` beside the log; `0` none"},
      {"atmo_record_s", &settings_t::atmo_record_s, g::other, r::at_start, "0 and more", "edworld_eht only, (test): the atmosphere's fingerprint to `edworld_atmo` beside the log at most this often, in seconds; `0` = never", "(test) the atmosphere's fingerprint to edworld_atmo beside the log at most this often, in seconds: 0 = never"},
#endif
      {"patch", &settings_t::patch, g::jump_panel, r::at_start, "0, 1, 2", "`1` = the right superpower's emblem over the jump panel's wrong one, and the factions list; `2` = a test frame showing where they go; `0` = off", "The rest, all in doc/settings.md"},
      {"patch_x", &settings_t::patch_x, g::jump_panel, r::at_start, "", "the patch's centre on the panel's surface, x, in its pixels"},
      {"patch_y", &settings_t::patch_y, g::jump_panel, r::at_start, "", "the patch's centre, y, until the panel's SUPERPOWER row is read (the test frame); then 40 pixels under that row"},
      {"patch_width", &settings_t::patch_width, g::jump_panel, r::at_start, "over 0", "the patch's width, in the surface's pixels"},
      {"patch_height", &settings_t::patch_height, g::jump_panel, r::at_start, "over 0", "the patch's height, in the surface's pixels"},
      {"patch_emblem_height", &settings_t::patch_emblem_height, g::jump_panel, r::at_start, "over 0", "the emblem's height, in the surface's pixels"},
      {"patch_surface", surface<&settings_t::patch_surface_width, &settings_t::patch_surface_height>(), g::jump_panel, r::at_start, "WIDTHxHEIGHT", "the interface surface that carries the jump panel"},
      {"patch_ground", colour_t{&settings_t::patch_ground}, g::jump_panel, r::at_start, "RRGGBB", "the panel's own black under the patch"},
      {"independent_colour", colour_t{&settings_t::independent_colour}, g::jump_panel, r::at_start, "RRGGBB", "the independents' emblem's colour (the game's own is a dull blue); the others keep their superpower's"},
      {"patch_force", &settings_t::patch_force, g::jump_panel, r::at_start, "0..4", "`0` = the destination's own emblem; `1` Federation, `2` Empire, `3` Alliance, `4` Independent = that emblem whatever the destination (to place the patch where the game shows none)"},
      {"list", &settings_t::list, g::jump_panel, r::at_start, "0, 1", "`1` = the destination's factions by influence under the jump panel, from EHT's `target` in edworld_eht when it has them, else from EDSM (marked so); `0` = no list"},
      {"list_x", &settings_t::list_x, g::jump_panel, r::at_start, "", "the list's horizontal centre, in the panel surface's pixels"},
      {"list_top", &settings_t::list_top, g::jump_panel, r::at_start, "", "the list's top edge, in the panel surface's pixels, past the panel's edge"},
      {"list_width", &settings_t::list_width, g::jump_panel, r::at_start, "64..4096 (clamped)", "the list's box at its widest, in the panel surface's pixels; the box is as wide as its rows"},
      {"list_rows", &settings_t::list_rows, g::jump_panel, r::at_start, "1 and more (clamped)", "factions listed at most (EDSM's source line comes on top of them)"},
      {"list_text", &settings_t::list_text, g::jump_panel, r::at_start, "8..64 (clamped)", "the list's text height, in the panel surface's pixels; the font is made at the first patch"},
      {"list_gain", &settings_t::list_gain, g::jump_panel, r::at_start, "0 and more", "the list's colours times this in the cockpit (its target is HDR: white text there blooms more than the panel's own)"},
      {"compass_surface", surface<&settings_t::compass_surface_width, &settings_t::compass_surface_height>(), g::navballs, r::at_start, "WIDTHxHEIGHT", "the interface surface that carries the HUD's compass (the HUD's left cluster)"},
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
      {"compass_colour_disc", colour_t{&settings_t::compass_colour_disc}, g::navballs, r::at_start, "RRGGBB", "the disc behind a sphere, at its rim; its middle is mixed towards `compass_colour_rim`"},
      {"compass_colour_grid", colour_t{&settings_t::compass_colour_grid}, g::navballs, r::at_start, "RRGGBB", "the rings at 30 and 60 degrees and the meridians"},
      {"compass_colour_rim", colour_t{&settings_t::compass_colour_rim}, g::navballs, r::at_start, "RRGGBB", "the left sphere's rim and the wings' plane on the right one"},
      {"compass_colour_target", colour_t{&settings_t::compass_colour_target}, g::navballs, r::at_start, "RRGGBB", "the target's dot"},
      {"compass_colour_behind", colour_t{&settings_t::compass_colour_behind}, g::navballs, r::at_start, "RRGGBB", "the target behind, where it should be, `PUSH DOWN` / `PULL UP`"},
      {"compass_colour_nose", colour_t{&settings_t::compass_colour_nose}, g::navballs, r::at_start, "RRGGBB", "the nose's arrow"},
      {"compass_colour_text_left", colour_t{&settings_t::compass_colour_text_left}, g::navballs, r::at_start, "RRGGBB", "the left sphere's angles"},
      {"compass_colour_text_right", colour_t{&settings_t::compass_colour_text_right}, g::navballs, r::at_start, "RRGGBB", "the right sphere's lines"},
      {"compass_colour_ok", colour_t{&settings_t::compass_colour_ok}, g::navballs, r::at_start, "RRGGBB", "`ON PATH`; the approach view's base while the ship is over it"},
      {"compass_colour_wrong", colour_t{&settings_t::compass_colour_wrong}, g::navballs, r::at_start, "RRGGBB", "the approach view's base while the ship is under it (in space only)"},
      {"compass_colour_pad_base", colour_t{&settings_t::compass_colour_pad_base}, g::navballs, r::at_start, "RRGGBB", "(test) the approach view's base while the pad's plane shows"},
      {"compass_colour_top", colour_t{&settings_t::compass_colour_top}, g::navballs, r::at_start, "RRGGBB", "(test) in the approach view the left sphere's ring: the target seen from above"},
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
      {"compass_dot_hue_gap", &settings_t::compass_dot_hue_gap, g::navballs, r::at_start, "0 to 180", "a coloured pixel is the dot's when its hue is this many degrees or more from the compass disc's; white always is (a red disc with a yellow dot is ~40 apart)"},
      {"compass_log_ms", &settings_t::compass_log_ms, g::navballs, r::at_start, "0 and more", "a line of the compass's reading in the log at most this often, in milliseconds; `0` = never"},
      {"gate_dive", &settings_t::gate_dive, g::navballs, r::at_start, "0..90", "(not drawn yet) the gates down to a place: the dive, in degrees below the planet's visible edge"},
      {"gate_dive_height", &settings_t::gate_dive_height, g::navballs, r::at_start, "over 0", "the gates: the height of the point the dive starts from, in planet radii"},
      {"gate_arc", &settings_t::gate_arc, g::navballs, r::at_start, "0..180", "the gates: the largest turn, in degrees, onto the flat line before the dive"},
      {"gate_dive_steepest", &settings_t::gate_dive_steepest, g::navballs, r::at_start, "0..90", "the gates: the steepest dive, in degrees below the local horizontal; a straight line steeper than this sets up the approach instead"},
      {"station_axes", &settings_t::station_axes, g::navballs, r::at_start, "0, 1", "(test) `1` = the target's axes in the right sphere: the X, Y and Z of the model the game shows as the target's hologram; `0` = off"},
      {"pad_tilt_green_deg", &settings_t::pad_tilt_green_deg, g::navballs, r::at_start, "0..pad_tilt_red_deg", "(test, with `station_axes`) after the tower gives a pad, a disc of the pads' plane in the right sphere: green while the ship is tilted to the pads by at most this many degrees, then through yellow to red"},
      {"pad_tilt_red_deg", &settings_t::pad_tilt_red_deg, g::navballs, r::at_start, "over pad_tilt_green_deg", "(test) the pads' disc red from this many degrees of tilt on"},
      {"snow", &settings_t::snow, g::clouds, r::live, "0, 1", "`1` = crystals of the air's own gas falling near the ground, on foot outdoors or in a ship or an SRV in the air, under the clouds, on an ammonia or methane planet (a cold ammonia one, its second colour, also a little in a clear sky); `0` = none", "Snow of the air's own gas near the ground: 1 = on"},
      {"snow_gain", &settings_t::snow_gain, g::clouds, r::live, "0 and more", "how many crystals, times the kind's own (ammonia 0.5 under a full cloud, a cold ammonia 1, methane 0.8)"},
      {"snow_bench", &settings_t::snow_bench, g::clouds, r::live, "0, 1", "(test) `1` = the snow's cost by its parts: every 10 s the next of all, none, the haze alone, the crystals alone, each logged as it starts (`snow bench:`), to read beside `clouds gpu 5s`; `0` = off"},
      {"galmap", &settings_t::galmap, g::other, r::at_start, "0, 1", "edworld_eht only: the names the galaxy map shows (systems, the grid's coordinates), read off its text draws and published to `galmap` in shm_dir for EHT, a few times a second while the map is open; `0` = off", "The galaxy map's names for EHT: 0 = off"},
      {"galmap_interval_ms", &settings_t::galmap_interval_ms, g::other, r::at_start, "100 and more (clamped)", "how often, at most, the galaxy map's names are read (milliseconds)"},
      {"pad_text_samples", &settings_t::pad_text_samples, g::navballs, r::at_start, "0, 1", "(test) `1` = each time the HUD's notice box shows or changes its text, its area is written to `edworld_padtext` beside the log as a sample for learning the pad's digits; `0` = off"},
      {"pad_text_read", &settings_t::pad_text_read, g::navballs, r::at_start, "0, 1", "`1` = the pad's number and the grant's time read from the tower's notice on the HUD: edworld takes them as the pad given, edworld_eht logs whether they agree with EHT's; `0` = off"},
      {"pad_text_unsure_sample", &settings_t::pad_text_unsure_sample, g::navballs, r::at_start, "0, 1", "a notice whose pad number cannot be read sure gives no pad and a log line; `1` also keeps its picture once in `edworld_padtext` beside the log for learning the digits; `0` = the log line only"},
      {"station_vs", hex_t{&settings_t::station_vs}, g::navballs, r::at_start, "a shader hash or 0", "(test) the game's shader for the hologram's model; after a game update that changes it, `0`: edworld finds it by its draws and names it in the log"},
      {"log_interval_ms", &settings_t::log_interval_ms, g::other, r::at_start, "0 and more", "a summary line of the frame's panels in the log at most this often, in milliseconds; `0` = never"},
      {"frame_gap_us", &settings_t::frame_gap_us, g::other, r::at_start, "over 0", "a gap between two panel draws longer than this, in microseconds, starts a new frame"},
      {"log_all_vs", &settings_t::log_all_vs, g::other, r::at_start, "0, 1", "`1` = the hash of every vertex shader the game creates in the log (discovery)"},
      {"context_hook", &settings_t::context_hook, g::other, r::at_start, "0, 1, 2", "how the game's context is watched: `0` auto (a table of edworld's own when the context's methods are the system d3d11's, else in place), `1` in place, `2` a table of edworld's own whatever the context is"},
      {"watch_vs", list_t{&settings_t::watch_vs}, g::other, r::at_start, "shader hashes, comma separated", "the vertex shaders whose draws are panels: the cockpit holo panels, their GUI-effects-off twin and the one near a war settlement"},
      {"dump_vs", list_t{&settings_t::dump_vs}, g::other, r::at_start, "shader hashes, comma separated", "the vertex shaders a whole-frame dump looks into: their bytecode, buffers and what their pixel shader reads; the target's hologram"},
    };
    using setting_t = edplugin_common::ini_setting_t<settings_t, setting_group_e>;

    constexpr auto row_of(key_t const & k) -> setting_t
      {
      return {k.name, k.field, k.group, k.read, k.range, k.about, {}, k.note ? std::string_view{k.note} : std::string_view{}, k.value};
      }

    template<std::size_t... i>
    constexpr auto rows_of(std::index_sequence<i...>) -> std::array<setting_t, sizeof...(i)>
      {
      return {{row_of(keys[i])...}};
      }

    constexpr std::array<setting_t, std::size(keys)> rows{rows_of(std::make_index_sequence<std::size(keys)>{})};

    ///\brief the comment rule of the row of this name; none for a name this build does not have
    constexpr auto comment_rule_of(std::string_view name) -> std::optional<edplugin_common::comment_e>
      {
      for(setting_t const & row: rows)
        if(row.name == name)
          return edplugin_common::ini_comment_rule(row);
      return std::nullopt;
      }

    // a path, a folder or a dll's name keeps a ';' or '#' (doc/users.md says so); the dll names of creator_pass lose a
    // comment after a space, a number one right after it
    static_assert(comment_rule_of("next") == edplugin_common::comment_e::none);
    static_assert(comment_rule_of("status_file") == edplugin_common::comment_e::none);
#if defined(EDWORLD_EHT)
    static_assert(comment_rule_of("shm_dir") == edplugin_common::comment_e::none);
#endif
    static_assert(comment_rule_of("creator_pass") == edplugin_common::comment_e::after_space);
    static_assert(comment_rule_of("clouds") == edplugin_common::comment_e::anywhere);
    static_assert(comment_rule_of("watch_vs") == edplugin_common::comment_e::anywhere);

    // the keys taken out: their lines passed over with a note of their own, once a session
    constexpr edplugin_common::ini_withdrawn_t withdrawn[]{
      {"share"sv, "shm_dir"sv},
      {"clouds_radius"sv, ""sv},
    };

    // a shader's name of 0 asks edworld to find the shader; a list keeps its good names and says the others
    constexpr edplugin_common::ini_table_t<settings_t, setting_group_e> table{rows, withdrawn, edplugin_common::ini_notes_e::own,
                                                                              edplugin_common::bad_item_e::skip_item,
                                                                              edplugin_common::hex_zero_e::taken};

    constexpr edplugin_common::ini_texts_t ini_texts{
      "; edworld's settings, written at its first start. Take the ';' off the start of a line to change it;\n"
      "; a change takes a new start of the game, but for those doc/settings.md lists as read while it runs.\n"
      "; A line left as a comment follows edworld's default.\n"sv,
      "; Settings this file did not name yet, added by edworld.\n"sv};

    // the settings as the game's threads see them: the start-only ones from any thread, the live ones (and the
    // generation) on the render thread, which reads the ini again while it draws the clouds (refresh_live_settings)
    edplugin_common::live_settings_t<settings_t> live;
    edplugin_common::ini_watch_t ini_watch;
    edplugin_common::ini_told_t told;

    auto log_completion(edplugin_common::ini_complete_t const & done) -> void
      {
      using edplugin_common::ini_complete_e;
      switch(done.status)
        {
        case ini_complete_e::made:
          log_line("settings: {} made, all its lines comments", wide(ini_watch.path()));
          break;
        case ini_complete_e::added:
          log_line("settings: {} added to {}", done.count, wide(ini_watch.path()));
          break;
        case ini_complete_e::write_failed:
          log_line("settings: {} not written whole (error {}); {}", wide(ini_watch.path()), done.error,
                   done.undone ? "nothing of it left" : "a part of it left");
          break;
        case ini_complete_e::no_memory:
          log_line("settings: the lines {} lacks could not be made (no memory); nothing added", wide(ini_watch.path()));
          break;
        case ini_complete_e::nothing:
        case ini_complete_e::unreadable:  // said by the read after it
        case ini_complete_e::utf16:
          break;
        }
      }

    ///\brief the ini read into `s` (the defaults, `dir` set); false when nothing was read: no file, one that cannot be
    /// read (said once until it is read) or UTF-16 (said once)
    auto read_into(settings_t & s, edplugin_common::ini_read_t & read) -> bool
      {
      using edplugin_common::ini_file_e;
      switch(edplugin_common::read_ini_file(ini_watch.path(), table, s, read, told))
        {
        case ini_file_e::read:
          ini_watch.readable();
          return true;
        case ini_file_e::none:
          return false;
        case ini_file_e::unreadable:
          if(ini_watch.unreadable())
            log_line("settings: {} could not be read; the settings stay as they were", wide(ini_watch.path()));
          return false;
        case ini_file_e::utf16:
          if(told.first("utf16"))
            log_line("settings: {} is UTF-16: neither read nor added to (save it as UTF-8); the settings stay as they were",
                     wide(ini_watch.path()));
          return false;
        }
      return false;
      }

    auto log_notes(std::vector<std::string> const & notes) -> void
      {
      for(std::string const & n: notes)
        log_line("settings: {}", n);
      }

    ///\brief the values held to their ranges after a read (the table's "(clamped)" of the clouds and the aurora)
    auto clamp(settings_t & s) noexcept -> void { clouds_settings_held(s); }
    }  // namespace

  auto settings() noexcept -> settings_t const & { return live.get(); }

  // the slot in use for now; edplugin_common's live3_settings_t gives a copy of its own to the start-only fields
  auto start_fields() noexcept -> settings_t const & { return live.get(); }

  auto settings_generation() noexcept -> std::uint32_t { return live.slot().generation; }

  auto start_settings(std::wstring const & config_dir, std::wstring const & output_dir) noexcept -> void
    {
    try
      {
      ini_watch.set_path(stralgo::stl::merge(config_dir, L'\\', std::wstring_view{plugin_name}, L".ini"sv));
      // the values written are the defaults edworld runs on without the file; status_file stays empty (Saved Games):
      // the folder's full path holds the player's Windows user name, and players share their ini
      settings_t s{};
      s.dir = output_dir;
      log_completion(edplugin_common::complete_ini(ini_watch.path(), table, s, ini_texts));
      // a change made after this, before the first refresh, is read then (and edworld's own addition is none)
      ini_watch.mark_seen();
      edplugin_common::ini_read_t read;
      if(read_into(s, read))
        log_notes(read.notes);
      clamp(s);
      live.start(s);
      }
    catch(std::exception const & e)
      {
      log_line("settings: the ini not read: {}; the defaults", e.what());
      }
    catch(...)
      {
      log_line("settings: the ini not read; the defaults");
      }
    }

  auto refresh_live_settings() noexcept -> void
    {
    try
      {
      switch(ini_watch.look(GetTickCount64()))
        {
        case edplugin_common::ini_look_e::changed:
          break;
        case edplugin_common::ini_look_e::gone:
          log_line("settings: {} taken away; the settings stay as they are until it is back", wide(ini_watch.path()));
          return;
        case edplugin_common::ini_look_e::not_due:
        case edplugin_common::ini_look_e::same:
          return;
        }
      settings_t s{};
      s.dir = settings().dir;
      edplugin_common::ini_read_t read;
      if(not read_into(s, read))
        return;
      clamp(s);
      edplugin_common::at_start_changes(table, s, read, settings(), told, read.notes);
      log_notes(read.notes);
      live.publish(s, table);
      std::string values;
      for(setting_t const & row: rows)
        {
        if(row.read != r::live)
          continue;
        ((values += values.empty() ? "" : ", ") += row.name) += ' ';
        values += edplugin_common::ini_value_text(settings(), row.field);
        }
      log_line("settings: read again: {}", values);
      }
    catch(std::exception const & e)
      {
      log_line("settings: the ini not read again: {}", e.what());
      }
    catch(...)
      {
      log_line("settings: the ini not read again");
      }
    }

  auto write_settings_doc() noexcept -> bool
    {
    try
      {
      settings_t const defaults{};
      edplugin_common::ini_doc_rows_t<settings_t, setting_group_e> const doc_rows{table, defaults};
      constexpr edplugin_common::settings_doc_t doc{
        "edworld settings"sv,
        "edworld from its table of settings (src/settings_log.cc, `EDWORLD_SETTINGS_DOC`)"sv,
        "In `edworld.ini` (or `edworld_eht.ini`), beside the dll or in `edloader\\config`; how the file is made and kept: "
        "[users.md](users.md#settings).\n"
        "\n"
        "- **Read while the game runs**: edworld looks at the ini at most once a second while it draws clouds (a planet "
        "with an atmosphere on the screen, `clouds = 1`); a change shows within a second or two and the log has "
        "`settings: read again: ...`.\n"
        "- **Read at the game's start**: a change takes a new start of the game.\n"
        "- **Range**: the values that make sense; *(clamped)* = edworld holds the value to it, elsewhere it does not check.\n"sv,
        groups};
      edplugin_common::settings_doc_result_t const r{edplugin_common::write_settings_doc(L"EDWORLD_SETTINGS_DOC", doc, doc_rows.rows())};
      switch(r.status)
        {
        case edplugin_common::settings_doc_status_e::not_asked:
          return true;
        case edplugin_common::settings_doc_status_e::written:
          log_line("settings: the settings' doc written to {}", wide(r.path));
          return true;
        case edplugin_common::settings_doc_status_e::path_too_long:
          log_line("settings: the settings' doc not written: its path is 260 characters or longer ({}...)", wide(r.path));
          break;
        case edplugin_common::settings_doc_status_e::invalid:
          log_line("settings: the settings' doc not written: {}", edplugin_common::settings_doc_error_name(r.error));
          break;
        case edplugin_common::settings_doc_status_e::cannot_open:
          log_line("settings: the settings' doc {} cannot be made (error {})", wide(r.path), r.system_error);
          break;
        case edplugin_common::settings_doc_status_e::write_failed:
          log_line("settings: writing the settings' doc {} failed (error {})", wide(r.path), r.system_error);
          break;
        }
      }
    catch(std::exception const & e)
      {
      log_line("settings: the settings' doc not written: {}", e.what());
      }
    catch(...)
      {
      log_line("settings: the settings' doc not written");
      }
    return false;
    }

  auto test_fault(char const * where) -> void
    {
    static std::string const asked{[]
                                     {
                                       char value[32]{};
                                       DWORD const n{GetEnvironmentVariableA("EDWORLD_TEST_FAULT", value, sizeof value)};
                                       return n != 0 and n < sizeof value ? std::string{value, n} : std::string{};
                                     }()};
    static std::atomic<bool> done{};
    if(asked.empty() or asked != where or done.exchange(true))
      return;
    if(asked == "draw-cpp" or asked == "thread")
      throw std::runtime_error{"EDWORLD_TEST_FAULT"};
    *static_cast<int volatile *>(nullptr) = 1;
    }
  }  // namespace edworld
