# edworld settings

<!-- Made by edworld from its table of settings (src/settings_log.cc, `EDWORLD_SETTINGS_DOC`); tools/test.sh checks
     it is up to date. Change the table, not this file. -->

In `edworld.ini` (or `edworld_eht.ini`), beside the dll or in `edloader\config`; how the file is made and kept: [users.md](users.md#settings).

- **Read while the game runs**: edworld looks at the ini at most once a second while it draws clouds (a planet with an atmosphere on the screen, `clouds = 1`); a change shows within a second or two and the log has `settings: read again: ...`.
- **Read at the game's start**: a change takes a new start of the game.
- **Range**: the values that make sense; *(clamped)* = edworld holds the value to it, elsewhere it does not check.

## Clouds

### Read while the game runs

| setting | default | range | what |
|---|---|---|---|
| `clouds_mode` | `0` | 0, 1, 2 | (test) the clouds with depth from afar in fewer steps, the same shape: `0` = by your height, `1` = always near, `2` = always far |
| `clouds_far_steps` | `6` | 1 and more (clamped) | the steps through the clouds with depth from afar |
| `clouds_near_from` | `0.25` | 0 and more | near steps under this height over the surface, as a part of the planet's radius; a blend up to `clouds_near_to` |
| `clouds_near_to` | `0.5` | over clouds_near_from (clamped) | far steps over this height over the surface, as a part of the planet's radius |
| `clouds_debug` | `0` | 0, 1, 2 | (test) `1` = every pixel the clouds with depth are worked out for painted magenta, `2` = the game's fade of its atmosphere painted, green at 1, red at 0; `0` = off |
| `clouds_game_fade` | `1` | 0, 1 | (test) `1` = the clouds with depth fade with the game's own atmosphere, `0` = never |
| `clouds_hide_below_px` | `20` | 0 and more | no clouds with depth while the planet is narrower on the screen than this many pixels (of the picture the game draws, before its upscaling); a blend up to `clouds_full_from_px` |
| `clouds_full_from_px` | `80` | over clouds_hide_below_px (clamped) | all the clouds with depth from a planet this many pixels across |
| `clouds_max_steps` | `64` | 1 and more (clamped) | (test) the most steps through the clouds with depth: the steps follow the way's length through them, many at the planet's edge |
| `clouds_edge_hide_px` | `2` | 0 and more | no clouds with depth out of the planet's edge while their layer is thinner on the screen than this many pixels; a blend up to `clouds_edge_full_px` |
| `clouds_edge_full_px` | `8` | over clouds_edge_hide_px (clamped) | all the clouds with depth out of the planet's edge from a layer this many pixels thick on the screen |
| `clouds_edge_band_px` | `16` | over 0 (clamped) | on the disc, the same fading within this many pixels of the planet's edge |
| `clouds_haze` | `0.05` | 0 and more (clamped) | from under the clouds with depth and in them, a cloud this far away covers e times less, so they fade into the haze at the horizon instead of ending in a sharp line; a part of the planet's radius, `0` = never less; from orbit they stay as they are |
| `clouds_kind_read_s` | `10` | 0 and more | how often the game's atmosphere is read for its kind and the planet's day, in seconds; `0` = never, the clouds as in 1.2.0-beta.8 |
| `clouds_by_kind` | `1` | 0, 1 | `1` = the clouds with depth by the atmosphere's kind (needs `clouds_kind_read_s`), `0` = the same on every planet |
| `clouds_physics_intensity` | `1` | 0 and more (clamped) | how strongly the kinds' clouds show: `1` = as the physics gives, `2` = twice as strong (at most `clouds_opacity`) and a haze's cover up to clouds', `0` = none |
| `clouds_tint` | `1` | 0 and more (clamped) | how strongly the kinds' clouds take their colour: `1` = a little off white as [clouds.md](clouds.md) gives, `0` = white on every planet, `2` = twice as far from white |
| `clouds_size_var` | `0.25` | 0..0.9 (clamped) | (PoC) the lumps' size noised by region: their scale times 1 +- this from a slow noise over the planet; `0` = one size everywhere |
| `clouds_cyclones` | `1` | 0, 1 | `1` = storms in the kinds' clouds by the planet's size, its day and the kind, `0` = none |

### Read at the game's start (a change needs a restart)

| setting | default | range | what |
|---|---|---|---|
| `clouds` | `1` | 0, 1 | clouds over a planet with an atmosphere that the game draws without any; `0` = none, and then nothing of the clouds is read again while the game runs |
| `clouds_height` | `0.005` | 0..0.05 | the flat layer's height over the surface, as a part of the planet's radius |
| `clouds_alpha` | `0.45` | 0..1 | how much the flat layer covers what is under it, `0` nothing to `1` all |
| `clouds_cover` | `0.55` | 0..1 | how much of the sky the clouds take, `0` clear to `1` overcast |
| `clouds_light` | `0.4` | 0..1 | how much of the star's light the clouds give back |
| `clouds_volume` | `1` | 0, 1 | `1` = clouds with depth you fly through; `0` = the flat layer, lighter for a weaker graphics card |
| `clouds_bottom` | `0.003` | 0..clouds_top | where the clouds with depth begin over the surface, as a part of the planet's radius |
| `clouds_top` | `0.012` | over clouds_bottom | where the clouds with depth end over the surface, as a part of the planet's radius |
| `clouds_steps` | `32` | 1 and more | the steps a ray takes through the clouds with depth: fewer are lighter for the graphics card and coarser |
| `clouds_density` | `1.2` | 0 and more | how thick the clouds with depth are |
| `clouds_opacity` | `0.75` | 0..1 | how much the clouds with depth cover at most; the kinds' clouds (`clouds_by_kind`) are scaled by it |
| `clouds_min_light` | `400` | 0 and more | the star's light the clouds with depth get at least, in the game's units, so a faint star does not lose them; the light's hue stays; `0` = as the game gives it |
| `clouds_under_gain` | `2` | 1 and more | how much lighter the clouds with depth are seen from under them, against a day's sky; `1` = as they are |
| `clouds_detail` | `1` | 0..1 | how much the fine shape near you cuts the clouds with depth; `0` = none, a smooth fog from the ground |
| `clouds_detail_size` | `3000` | over 0 | the fine shape's largest lumps, in metres |
| `clouds_detail_range` | `500000` | over 0 | how far from you the fine shape reaches, in metres |
| `clouds_detail_edge` | `8` | over 0 | how sharp a cloud's edge is near you; more = sharper |
| `clouds_detail_gap` | `0.14` | 0..1 | how much clear sky the fine shape opens among the clouds with depth near you, from under them too; more = thinner, the view from orbit stays |
| `clouds_wind_low` | `100` | 0 and more | the wind at the floor of the clouds with depth, km/h; with `clouds_wind_high` both `0` = still clouds |
| `clouds_wind_high` | `400` | 0 and more | the wind at the top of the clouds with depth, km/h |
| `clouds_top_gain` | `2` | 1 and more | how much lighter the clouds with depth are seen from above, over a light ground; `1` = as from under them |

## Navballs

### Read while the game runs

None.

### Read at the game's start (a change needs a restart)

| setting | default | range | what |
|---|---|---|---|
| `compass` | `1` | 0, 1, 2, 3 | `0` = no navballs, the game's compass is not read at all; `1` = the angles; `2` also everything known for checking them; `3` also a grid of the HUD surface's pixels |
| `compass_surface` | `2200x1800` | WIDTHxHEIGHT | the interface surface that carries the HUD's compass (the HUD's left cluster) |
| `compass_x` | `430` |  | the compass disc's centre on that surface, x, in its pixels |
| `compass_y` | `271` |  | the compass disc's centre, y |
| `compass_radius` | `54` | over 0 | the compass disc's rim radius, in the surface's pixels |
| `compass_text_x` | `1185` |  | the angles' top left corner above the radar (`compass_text`), x, in the surface's pixels |
| `compass_text_y` | `822` |  | the angles' top left corner, y |
| `compass_text_size` | `34` | over 0 | the angles' height, in the surface's pixels |
| `compass_info_x` | `1620` |  | the checking lines (`compass = 2`), x, in the surface's pixels |
| `compass_info_y` | `24` |  | the checking lines, y |
| `compass_info_size` | `18` | over 0 | the checking lines' height, in the surface's pixels |
| `compass_spheres` | `1` | 0, 1 | `1` = the two navballs on the dashboard: the left one the target seen from the front, the right one from behind with the place the target should be; `0` = no spheres |
| `compass_sphere_height` | `0.135` | over 0 | on the side panels (`compass_anchor = 0`): a sphere's height in the panels' local units |
| `compass_sphere_aspect` | `1.13` | over 0 | on the side panels: a sphere's width over its height, so it is round on the screen |
| `compass_sphere_a_x` | `0.043` |  | on the side panels: the left sphere's centre in its panel's local plane, x |
| `compass_sphere_a_y` | `-0.289` |  | on the side panels: the left sphere's centre, y |
| `compass_sphere_c_x` | `-0.041` |  | on the side panels: the right sphere's centre in its panel's local plane, x |
| `compass_sphere_c_y` | `-0.289` |  | on the side panels: the right sphere's centre, y |
| `compass_should_dive` | `45` | 0..90 | the dive, in degrees below the horizon, the right sphere steers towards near a planet |
| `compass_sphere_gamma` | `2.2` | 0 and more | the spheres' colours taken from sRGB back to linear with this gamma (the cockpit is linear HDR); `0` = as drawn |
| `compass_sphere_gain` | `1` | 0 and more | all the spheres' colours times this: lighter or darker |
| `compass_sphere_text_above` | `1` | 0, 1 | `1` = the spheres' lines in a band over them (at a planet the right sphere's go to the left band, under the angles), `0` = under them |
| `compass_anchor` | `1` | 0, 1 | `1` = the spheres held to the game's compass and speed readout, in their places in every ship; `0` = on the two side panels (`compass_sphere_*`, which differ from ship to ship) |
| `compass_anchor_c_px` | `416` |  | the right sphere's anchor, the speed readout, on the HUD surface, x, in its pixels |
| `compass_anchor_c_py` | `157` |  | the right sphere's anchor, y |
| `compass_anchor_scale` | `1.9` | over 0 | a sphere's diameter, in the game compass's diameters |
| `compass_anchor_a_dx` | `-0.41` |  | the left sphere's centre from the compass, in compass diameters, x to the right |
| `compass_anchor_a_dy` | `0.45` |  | the left sphere's centre from the compass, y up |
| `compass_anchor_c_dx` | `1.22` |  | the right sphere's centre from the speed readout, in compass diameters, x to the right |
| `compass_anchor_c_dy` | `0.83` |  | the right sphere's centre from the speed readout, y up |
| `compass_text` | `0` | 0, 1 | `1` = the angles also written above the radar; `0` = the spheres carry them |
| `compass_hide_game` | `0` | 0, 1 | `1` = the game's own compass hidden once read, the left sphere stands in for it; it returns whenever edworld is not there |
| `compass_colour_disc` | `101214` | RRGGBB | the disc behind a sphere, at its rim; its middle is mixed towards `compass_colour_rim` |
| `compass_colour_grid` | `5A6068` | RRGGBB | the rings at 30 and 60 degrees and the meridians |
| `compass_colour_rim` | `A8AEB5` | RRGGBB | the left sphere's rim and the wings' plane on the right one |
| `compass_colour_target` | `FFFFFF` | RRGGBB | the target's dot |
| `compass_colour_behind` | `D9A15A` | RRGGBB | the target behind, where it should be, `PUSH DOWN` / `PULL UP` |
| `compass_colour_nose` | `8CC9A0` | RRGGBB | the nose's arrow |
| `compass_colour_text_left` | `C9CED4` | RRGGBB | the left sphere's angles |
| `compass_colour_text_right` | `D6CFB4` | RRGGBB | the right sphere's lines |
| `compass_colour_ok` | `9AD3A8` | RRGGBB | `ON PATH`; the approach view's base while the ship is over it |
| `compass_colour_wrong` | `C0504D` | RRGGBB | the approach view's base while the ship is under it (in space only) |
| `compass_colour_pad_base` | `7AB8E0` | RRGGBB | (test) the approach view's base while the pad's plane shows |
| `compass_colour_top` | `7AB8E0` | RRGGBB | (test) in the approach view the left sphere's ring: the target seen from above |
| `compass_approach_below` | `20` | 0..90 | in normal flight with the target this many degrees or more below the wings, the approach view: the left sphere shows the target from above, the right one a dome standing on the target with the ship a point on it; `0` = never |
| `compass_approach_smooth_s` | `0.4` | 0 and more | in the approach view the dot's place is averaged with this time constant, in seconds, so the spheres do not shake near a pad; `0` = no averaging |
| `compass_approach_km` | `5` | 0 and more | the approach view also needs the target near: a place on a body within this many km, or a station or carrier in this system while mass locked; `0` = by the angle alone |
| `compass_approach_radius` | `54.8` | over 0 | the dot's distance from the compass's centre with the target on its rim, in the HUD surface's pixels; the angle ahead or behind near a pad comes from how far short of it the dot is |
| `compass_stale_s` | `1` | 0 and more | a dot read holds this many seconds without a new one, then `NO DOT` and the approach view goes off; `0` = held until the next one |
| `compass_max_rate_dps` | `0` | 0 and more | a dot further from the last one than the ship can turn in the time between (degrees a second, plus 5) is not taken; `0` = no such check |
| `compass_filled_min` | `120` | up to compass_filled_max | a filled dot's pixels on the HUD surface at least; fewer is not the dot |
| `compass_filled_max` | `320` |  | a filled dot's pixels at most; more is not the dot |
| `compass_hollow_min` | `60` | up to compass_hollow_max | a hollow dot's (the target behind) pixels at least |
| `compass_hollow_max` | `180` |  | a hollow dot's pixels at most |
| `compass_log_ms` | `1000` | 0 and more | a line of the compass's reading in the log at most this often, in milliseconds; `0` = never |
| `gate_dive` | `17` | 0..90 | (PoC, not drawn yet) the gates down to a place: the dive, in degrees below the planet's visible edge |
| `gate_dive_height` | `0.3` | over 0 | (PoC) the gates: the height of the point the dive starts from, in planet radii |
| `gate_arc` | `30` | 0..180 | (PoC) the gates: the largest turn, in degrees, onto the flat line before the dive |
| `gate_dive_steepest` | `73` | 0..90 | (PoC) the gates: the steepest dive, in degrees below the local horizontal; a straight line steeper than this sets up the approach instead |
| `station_axes` | `1` | 0, 1 | (test) `1` = the target's axes in the right sphere: the X, Y and Z of the model the game shows as the target's hologram; `0` = off |
| `pad_tilt_green_deg` | `10` | 0..pad_tilt_red_deg | (test, with `station_axes`) after the tower gives a pad, a disc of the pads' plane in the right sphere: green while the ship is tilted to the pads by at most this many degrees, then through yellow to red |
| `pad_tilt_red_deg` | `20` | over pad_tilt_green_deg | (test) the pads' disc red from this many degrees of tilt on |
| `pad_text_samples` | `0` | 0, 1 | (test) `1` = each time the HUD's notice box shows or changes its text, its area is written to `edworld_padtext` beside the log as a sample for learning the pad's digits; `0` = off |
| `pad_text_read` | `1` | 0, 1 | `1` = the pad's number and the grant's time read from the tower's notice on the HUD: edworld takes them as the pad given, edworld_eht logs whether they agree with EHT's; `0` = off |
| `pad_text_unsure_sample` | `1` | 0, 1 | a notice whose pad number cannot be read sure gives no pad and a log line; `1` also keeps its picture once in `edworld_padtext` beside the log for learning the digits; `0` = the log line only |
| `station_vs` | `5B0068AF5630F96B` | a shader hash or 0 | (test) the game's shader for the hologram's model; after a game update that changes it, `0`: edworld finds it by its draws and names it in the log |

## Jump panel

### Read while the game runs

None.

### Read at the game's start (a change needs a restart)

| setting | default | range | what |
|---|---|---|---|
| `edsm` | `1` | 0, 1 | `1` = EDSM is asked for the destination's allegiance and factions, once per new destination, with the system's id only; `0` = never (no list then without EHT) |
| `list_alpha` | `0.65` | 0..1 (clamped) | how much the factions list's box covers what is behind it, `0` nothing to `1` all |
| `patch` | `1` | 0, 1, 2 | `1` = the right superpower's emblem over the jump panel's wrong one, and the factions list; `2` = a test frame showing where they go; `0` = off |
| `patch_x` | `1535` |  | the patch's centre on the panel's surface, x, in its pixels |
| `patch_y` | `280` |  | the patch's centre, y, until the panel's SUPERPOWER row is read (the test frame); then 40 pixels under that row |
| `patch_width` | `260` | over 0 | the patch's width, in the surface's pixels |
| `patch_height` | `120` | over 0 | the patch's height, in the surface's pixels |
| `patch_emblem_height` | `92` | over 0 | the emblem's height, in the surface's pixels |
| `patch_surface` | `3072x660` | WIDTHxHEIGHT | the interface surface that carries the jump panel |
| `patch_ground` | `020304` | RRGGBB | the panel's own black under the patch |
| `independent_colour` | `F2C14E` | RRGGBB | the independents' emblem's colour (the game's own is a dull blue); the others keep their superpower's |
| `patch_force` | `0` | 0..4 | `0` = the destination's own emblem; `1` Federation, `2` Empire, `3` Alliance, `4` Independent = that emblem whatever the destination (to place the patch where the game shows none) |
| `list` | `1` | 0, 1 | `1` = the destination's factions by influence under the jump panel, from EHT's `target` in edworld_eht when it has them, else from EDSM (marked so); `0` = no list |
| `list_x` | `1540` |  | the list's horizontal centre, in the panel surface's pixels |
| `list_top` | `500` |  | the list's top edge, in the panel surface's pixels, past the panel's edge |
| `list_width` | `2200` | 64..4096 (clamped) | the list's box at its widest, in the panel surface's pixels; the box is as wide as its rows |
| `list_rows` | `7` | 1 and more (clamped) | factions listed at most (EDSM's source line comes on top of them) |
| `list_text` | `50` | 8..64 (clamped) | the list's text height, in the panel surface's pixels; the font is made at the first patch |
| `list_gain` | `0.8` | 0 and more | the list's colours times this in the cockpit (its target is HDR: white text there blooms more than the panel's own) |

## Other

### Read while the game runs

None.

### Read at the game's start (a change needs a restart)

| setting | default | range | what |
|---|---|---|---|
| `next` | empty | a dll's name or empty | the next d3d11 mod in the chain (`d3d11_edhm.dll`, a mod's dll renamed); empty = the system's own, under edloader the next on its list |
| `status_file` | empty | a path or empty | the game's `Status.json`, the journals beside it; empty = in Saved Games; for a game whose Saved Games lie elsewhere |
| `shm_dir` | `Z:\dev\shm\eht` | a folder or empty | edworld_eht only: the folder shared with EHT as the game sees it (`Z:` is the Linux root under Wine); `panels` published there, `target` read from there; empty = neither |
| `shader_catalog` | `1` | 0, 1 | edworld_eht only: `1` keeps each shader the game creates, once per build of the game, in `edworld_shaders` beside the log; `0` none |
| `atmo_record_s` | `10` | 0 and more | edworld_eht only, (test): the atmosphere's fingerprint to `edworld_atmo` beside the log at most this often, in seconds; `0` = never |
| `log_interval_ms` | `1000` | 0 and more | a summary line of the frame's panels in the log at most this often, in milliseconds; `0` = never |
| `frame_gap_us` | `2500` | over 0 | a gap between two panel draws longer than this, in microseconds, starts a new frame |
| `log_all_vs` | `0` | 0, 1 | `1` = the hash of every vertex shader the game creates in the log (discovery) |
| `context_hook` | `0` | 0, 1, 2 | how the game's context is watched: `0` auto (a table of edworld's own when the context's methods are the system d3d11's, else in place), `1` in place, `2` a table of edworld's own whatever the context is |
| `watch_vs` | `81216C77F90DEDD6, 1989E6D3B405FDE0, 925ACEDA0153AA5B` | shader hashes, comma separated | the vertex shaders whose draws are panels: the cockpit holo panels, their GUI-effects-off twin and the one near a war settlement |
| `dump_vs` | `5B0068AF5630F96B, 5559BD94B6852E83` | shader hashes, comma separated | the vertex shaders a whole-frame dump looks into: their bytecode, buffers and what their pixel shader reads; the target's hologram |
