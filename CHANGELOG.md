# Changelog

## Unreleased

- **The clouds' colour by the kind.** The kinds' tints were so close to white that every planet's clouds looked the
  same white, coloured only by the star. Now ammonia's clouds are cream, methane's orange-brown, sulphur dioxide's
  yellowish and silicate vapour's grey-brown, from what colours them; the kinds whose ices are white get a slight
  tint only to tell them apart, and water's stay white. `clouds_tint` scales it (`0` = white, `2` = twice as far).
- **Lumps of more than one size.** The clouds' shape had one scale over the whole planet, so its lumps were the
  same size everywhere. Now a slow noise over the planet makes them larger in some regions and smaller in others
  (`clouds_size_var`, by default their scale times 1 ± 0.25; `0` gives the old clouds).

## 1.2.0-beta.9

- **Clouds by the atmosphere's kind.** edworld reads the kind off how the game draws the atmosphere (not the
  journal), every `clouds_kind_read_s` seconds: each of the 15 kinds on landable planets gets its own layer, cover,
  opacity and tint from the physics of its typical planet; a gas that does not condense there gives only a haze
  (`clouds_by_kind`, `clouds_physics_intensity`).
- **Storms.** Cyclones with spiral arms, a dense mass of clouds and a clear eye shaped like a funnel, as many and as
  large as the planet's size, its day (read off its turn in the picture) and its kind allow (`clouds_cyclones`).
- `clouds_kind_read_s = 0` in `edworld.ini` gives the clouds of 1.2.0-beta.8 back. A video:
  https://youtu.be/_mVhSVoFirw

## 1.2.0-beta.8

- **Fixed: no navballs below 2160 rows.** The game draws its HUD's surfaces at a size that follows the picture's
  height: the compass's 2200×1800 at 2160 rows is 1466×1200 at 1440 and 1100×900 at 1080 (the width rounded down;
  measured in the game in a window). edworld looked for 2200×1800 only, so at a lower resolution it found no compass
  and the game's own stayed. Now a surface of the same shape at another scale is taken: the compass is read on it in
  its own pixels and the reading goes back to the pixels the settings are measured in, so the settings stay as they
  are; what edworld draws there is scaled onto the surface. The log says the scale (`compass: first drawn (... surface
  1466x1200 = 0.667 of its size at 2160 rows ...)`). Checked in the game at 2560×1440 and 1920×1080 in a window.
- **Fixed: nothing seen of the game on a Windows where the hooks in the context's table were taken back.** On a
  player's Windows (AMD, Epic's version) every draw hook was back to d3d11.dll 30 s after the start and no draw
  reached edworld. When the context's methods are the system d3d11's own, the game's context now points at a table
  of edworld's: edworld's hooks in their slots, and in every slot a call through the runtime's own entry as it is at
  that moment (EDVR's way, its LiveCopy), so whatever the runtime does to its table is followed. A wrapper's context
  (EDHM's 3Dmigoto, ReShade) keeps its table, hooked in place as before. `context_hook` in the ini: `0` this choice,
  `1` in place always, `2` edworld's table always. The log says which (`context: ...`), the multithread protection
  at the start and in the `census` lines, and how many of edworld's slots the runtime changed since the start.
  Checked on that player's Windows (clouds, the target's hologram and the panels seen) and under Proton with EDHM and
  EDVR (in place, as before). Why the hooks were taken back there is not known: in the run with edworld's table the
  runtime changed none of the slots.
- The pad's number is read off the HUD at 2200×1800 only (its digits are known at that size); at another size the log
  says so once. The jump panel's emblem still needs the panel at 3072×660, so below 2160 rows the game's emblem
  stays.

## 1.2.0-beta.7

- **Fixed: nothing shown with Epic's overlay.** The overlay of Epic Online Services (`EOSOVH-Win64-Shipping.dll`)
  makes a graphics device of its own a moment after the game's. edworld took the latest device for the game's, so it
  passed over all of the game's draws: no navballs, no panel, no clouds. Now a device asked for from outside the game's
  folder is logged (`asked by ... from outside the game's folder: not followed`) and not followed; one asked for by
  the game, or by nothing edworld can tell, is followed as before. Through edloader, EDHM and EDVR are passed over as
  they are proxies of d3d11, not the asker.
- **The log says what the game runs on:** at each start `machine:` lines (Windows or Wine, the processor, the
  memory, the graphics cards with their drivers and displays) and `graphics:` lines (the game's display settings,
  options and presets); for each device its card, feature level and whether the driver takes command lists, and who
  asked for it. Names and numbers only, no folder and no user name.
- **`census` lines 30 s and 5 min after the start:** the draws seen on each device and on others, the indirect ones,
  the command lists run, how often the atmosphere's shell was drawn, and whether edworld's draw hooks are still in
  place (or which dll took them); the same check of the hooks when a jump's probe sees no draw at all. The log also
  says when the game makes the atmosphere shell's shader.
- Checked under Proton (in a window and full screen, alone and with EDHM and EDVR). The overlay case is checked by
  the smoke test only, not yet in the game on Windows; the 5-minute census line is not yet seen in the game.

## 1.2.0-beta.6

- **The clouds with depth come in smoothly as you fly in.** None while the planet is narrower on the screen than
  `clouds_hide_below_px` (20) pixels, all of them from `clouds_full_from_px` (80), a blend between; before, a planet a
  few dozen pixels across showed only big white lumps, some beside it.
- **No lumps standing out of the planet's edge.** Seen from afar the layer is a pixel or two thin there and its lumps
  flickered as the ship moved: none out of the edge while the layer is thinner on the screen than
  `clouds_edge_hide_px` (2) pixels, all from `clouds_edge_full_px` (8); on the disc the same within
  `clouds_edge_band_px` (16) of the edge. The disc's middle stays as it was.
- **The planet's size on the screen whichever way the ship looks:** it followed the ship's heading, so the clouds came
  in later than set, and suddenly as the ship turned.
- **From afar the same shape in fewer steps** (`clouds_far_steps`, 6), near ones under `clouds_near_from` (0.25 of the
  radius) over the surface, a blend up to `clouds_near_to` (0.5); the steps follow the way through the layer, up to
  `clouds_max_steps` (64), many at the edge seen edge on.
- **(test) Settings to look into the clouds while the game runs:** `clouds_mode` forces near or far, `clouds_debug = 1`
  paints the pixels the clouds are worked out for, `2` the game's fade of its atmosphere, `clouds_game_fade = 0` keeps
  the clouds from following that fade. Once, on the way in, the clouds faded out for a moment about 8.3 Mm from a
  planet; on later flights it did not come back and the game's fade stayed full. These and all the thresholds above
  are read again from the ini while the game runs, at most once a second.
- The pixels of the thresholds are the picture the game draws, before its upscaling: with FSR or a lower
  supersampling they are fewer than the screen's. Looked at in the game on one commander's screen (9000x2160 with FSR);
  not yet on another setup.

## 1.2.0-beta.5

- **The clouds with depth cost about a quarter of what they did.** They are worked out at half the picture's width and
  height and laid over it by the depth, so they keep to the edge of the ground and the ship; their march is cheaper
  too. Measured at one place on a planet at 9000x2160, the same settings both times (render 6750x1620): from 7.8 ms
  of the graphics card a frame to 1.7 ms; the smoothing of the grain below adds about 0.2 ms.
- **No slanting stripes in them near the horizon.** The steps of the march start by blue noise; what stays there is a
  fine, even grain, softened as the clouds are laid over the picture.
- **The log tells what they cost:** a line `clouds gpu 5s` every 5 s.
- **Their swirls are worked out from where the view meets the layer's floor,** to hold them still at the planet's edge
  and as you enter the layer; not yet looked at there in the game.
- **edloader's guide:** EDHM goes into `edloader\plugins` with its files and is listed by name; the list needs
  `+d3dcompiler_47.dll` before it. EDHM in the game's folder, listed by its full path, works as before.

## 1.2.0-beta.4

- **The guides: EDVR's `ui_quality = off`.** With EDVR's anti-aliasing on, its bigger cockpit panels took the navballs
  away. The setting keeps both. The clouds were never affected.
- **edloader's guide:** EDVR's `real_dll` stands in the `[advanced]` section; why EDHM goes before EDVR.

## 1.2.0-beta.3

- **Through edloader the mods after edworld are reached by themselves.** An empty `next` used to end the list at
  edworld, so EDHM and EDVR were loaded and never used, and the ini edworld writes itself leaves `next` empty. Under
  edloader an empty `next` now goes on down `edloader.txt`. On its own edworld works as before.
- **edloader's guide:** EDHM stays in the game's folder and is listed by its full path.

## 1.2.0-beta.2

- **Clouds with depth (`clouds_volume`, on by default; `clouds_bottom`, `clouds_top`, `clouds_density`, `clouds_opacity`,
  `clouds_steps`).** You fly into them and they thin out around you. `clouds_volume = 0` gives the flat layer of
  1.2.0-beta.1, lighter for a weaker graphics card.
- **The clouds near you have a shape of their own (`clouds_detail`, `clouds_detail_size`, `clouds_detail_range`,
  `clouds_detail_edge`, `clouds_detail_gap`).** From the ground they were one smooth fog: lumps of a few km now cut them,
  with clear sky among them. The view from orbit stays.
- **They stay seen.** `clouds_top_gain` and `clouds_under_gain` make them lighter from above and from under them,
  `clouds_min_light` keeps them from going out by a faint star. `clouds_cover` is 0.55 now.
- **They drift (`clouds_wind_low`, `clouds_wind_high`).** Slowly and calmly, the large shapes faster high up, each planet
  its own way; both 0 = still clouds.
- **The ini writes itself.** At the first start edworld makes `edworld.ini` if there is none, with every setting as a
  comment holding the value it runs on, and at each start adds at the end what the file does not name yet. Lines that
  are there stay as they are. See doc/users.md "Settings".
- **`status_file`: where the game's Status.json is**, the journals beside it, for a game whose Saved Games lie elsewhere.

## 1.2.0-beta.1

- **Clouds over a planet with an atmosphere (`clouds`, on by default; `clouds_height`, `clouds_alpha`, `clouds_cover`, `clouds_light`).**
  After the game's own shell of the atmosphere, the same sphere is drawn once more with shaders of edworld, a little
  above the surface, with clouds worked out from noise, each planet its own, seen from orbit and from under them alike.
  See doc/users.md "Clouds".
- **A whole frame's dump of a block-compressed texture no longer stops edworld.** Its copy read four times past the
  end of the texture: a fault at every frame, and after eight of them edworld left the game alone until it restarted,
  with nothing in the log. The copy now counts rows of blocks, and stopping says so in the log.
- **The factions list's box is translucent (`list_alpha`, 0.65).** What is behind the box shows through it; `1` is the
  solid box it was.
- **Build with CMake and clang-cl.** `tools/build.sh` (MSVC through msvc-wine) is replaced by a CMake build with
  clang-cl against MSVC's and the Windows SDK's headers and libraries (`MSVC_ROOT`), with presets for Debug and
  Release; no MSVC compiler is run. `tools/test.sh` takes the build directory. See doc/developers.md "Build".

## 1.1.0

The navballs of the betas below, released, with the pad's plane in the right sphere, from the target's hologram the
game draws.

- **The pad's plane (test, `station_axes`, `pad_tilt_green_deg`, `pad_tilt_red_deg`).** After the tower gives a pad,
  a translucent disc of the pads' plane in the right sphere, green while the ship is level with the pads, through
  yellow to red as it tilts; a red wire grid and `UPSIDE DOWN` with the ship's top towards the pads. In the approach
  view the disc stands in the dome over a light blue base (`compass_colour_pad_base`).
  - At the six kinds of orbital outpost the pads' up comes from a table, with the pad's mark `< |||` pointing to
    where the ship's nose goes on the pad.
  - On a planet (surface ports, settlements, planetary construction sites) the plane is always flat: the hologram
    model's thinnest axis, turned towards the ship.
  - At a squadron carrier and a players' fleet carrier each pad by its number (the fleet carrier's deck leans 10.1°
    along its length, its side pads 22.5°).
  - A Coriolis, Orbis, Ocellus or asteroid base gets no plane: its hologram has no pads.
- **The hologram's models.** The target's hologram is told by its model (the draw's indices, and a fingerprint of the
  model that finds it again after an update of the game changes the indices); the kind of place, and so the rule of
  the plane, comes from the model, not from a journal. The models known are in `src/holo_models.h` and
  `src/holo_models_generated.h`.
- **The target's hologram by what it shows.** A known place (a port, a settlement, a carrier) is the target whichever
  of the HUD's two hologram surfaces it comes on: after a ship is swapped at a port the game lays them over the other
  way round, and the own ship was taken for the target. A known place on the own surface is never taken for a ship.
- **The pad's number from the HUD (`pad_text_read`).** The tower's notice (`PROCEED TO LANDING PAD n` over its
  countdown) gives the pad and the time of the grant, so edworld alone knows the pad given. Each digit is read by
  its shape seen from afar (4 x 6 zones of its glyph), whatever the pixel the number stands on: pad 8 and pad 30 are
  read too. A number not read sure gives no pad; its notice's picture is kept (`pad_text_unsure_sample`), and
  `pad_text_samples` (off) keeps all the notice's pictures, for learning the digits.
- **`station_vs`**: the game's shader of the hologram's model; `0` finds it after an update that changes it.
- **edworld_eht only:** a catalog of the game's shaders once per build (`shader_catalog`), a whole frame's dump on
  request, a picture of each new hologram model for naming it, and log lines that compare the place and the pad with
  EHT's.
- **`tools/holo_db.py`**: the hologram models' samples gathered offline, named on a page on 127.0.0.1, written into
  the two generated headers.
- In the approach view the left sphere's ring is light blue (`compass_colour_top`).

Known: the plane is checked in the game at orbital outposts, planetary construction sites, a surface port and a
squadron carrier; at a players' fleet carrier, and after a ship swap, it is not yet seen in the game. The pad's number
is read in English only.

## 1.1.0-beta.3

A beta for the game's update of 2026-10-20 (the Anaconda Mk2 update): the navballs read the new compass again.

- **The new compass.** Since the update the target's dot is cyan on an orange compass (it was read only as white, an
  EDHM theme's dot, so the navballs said `NO DOT`). The dot is now told by its hue against the compass's own,
  worked out every frame, so it is read in the game's own colours and in any EDHM theme; a moment when the whole
  compass flashes in the dot's colour is skipped. The dot's sizes follow the new compass (`compass_filled_min/max`,
  `compass_hollow_min/max`).
- **A reading goes stale.** A dot read holds `compass_stale_s` (1 s); then `NO DOT`. Before, a single stray reading
  stayed on the spheres until the next one.
- **The approach view only near the target.** Besides the target 20° or more below the wings, it needs the target
  near: a place on a body within `compass_approach_km` (5 km), or a station or carrier in this system while the ship
  is mass locked. After undocking, a target in another system no longer turns it on.
- **`compass_max_rate_dps`** (off by default): a dot further from the last one than the ship can turn is not taken.
- **Dive 45°.** `compass_should_dive` is 45 by default (was 35).
- **Gates down to a place (proof of concept, arithmetic only, not drawn).**
- **Build** without any EDVR binary: the d3d11 export names come from a list in the repository.

Known: the new compass is checked in the Kestrel at 9000×2160; the approach view near a station or carrier (mass
locked) is not yet checked in the game. On my machine both EDHM builds I had crashed the game after this update;
edworld itself ran without EDHM.

## 1.1.0-beta.2

A beta: the approach view, checked in the game at an outpost in space and at a surface port.

- **Screen resolution.** `doc/users.md` warns that the navballs are checked at one resolution only (9000×2160, the
  cockpit on the middle 3840×2160) and what to expect at 1920×1080 or lower.
- **At a planet** the base of the approach view stays green (the ship can not be under the pad there), and the right
  sphere's lines stand in two columns with the middle left to the game's own readout.
- **Navball colours.** The spheres' colours are settings (`compass_colour_*`, `RRGGBB`), a muted grey by default;
  `doc/users.md` has the beta's blue as an example.
- **Approach view.** In normal flight with the target 20° or more below the wings, the left
  sphere shows it from above and the right one a dome standing on it, the ship a point on the dome (in space the
  whole sphere with the base as a gridded plane, tinted green with the ship over it and red under it); after a planet's
  glide the right sphere is always the dome and the dive guidance shows only in orbital cruise and glide. The dot's
  place is averaged there (`compass_approach_smooth_s`, 0.4 s).

Known: the lines in the two columns are small (about 10 px tall at 3840×2160); ahead or behind near a pad is read
from a fraction of a pixel (a few degrees at best); with a settlement or a surface port targeted, `NOSE` and
`PUSH DOWN` / `PULL UP` in orbital cruise and glide still take the planet's centre as the target and are wrong there.

## 1.1.0-beta.1

A beta: testers wanted (README).

- **Navballs.** Two spheres beside the radar showing the selected target's direction: on the left seen from the
  front with the angles above it, on the right seen from behind, the left and above with the nose marked and how far
  the target is off it; in a planet's gravity well with the planet targeted, the nose's angle below the horizon,
  `PUSH DOWN` / `PULL UP` towards a 35° dive (`compass_should_dive`) and the altitude. The direction is read off the
  game's own compass on the HUD's surface. The spheres are quads in the HUD's plane, held to the compass and to the
  speed readout; their size and offsets are in the HUD mesh's own units, taken from the compass. On by default
  (`compass = 0` turns them off). The log records each sphere's place and size whenever it changes.

Known: placed and checked in the Kestrel's cockpit only; the lines near a planet are not yet checked in the game;
the game does not draw its compass every frame, so a sphere keeps its last reading in between.

## 1.0.0

The first release.

- **Jump panel emblem.** While a hyperspace jump charges, the wrong superpower emblem the game shows on the jump
  panel for the Federation, the Empire and the Alliance is painted over with the right one, drawn onto the panel's
  own surface so it moves with the panel. The superpower is read from the panel's own text (its SUPERPOWER row),
  so no outside source is needed and the emblem is right for systems nobody has recorded. Independents get the
  game's phoenix in gold (`independent_colour`) in place of its dull blue one.
- **Factions list.** Under the panel, the destination's factions by influence, with the controlling one starred,
  the trend at the last tick and their states. It comes from EHT's record of the system (edworld_eht) when EHT has
  influence readings of it, otherwise from EDSM, marked so with the age of its data. The list is a quad of its own
  in the panel's plane, placed by the game's own transform of the panel, and its box is as wide as its rows; its
  footer names edworld and its version.
- **Panel anchors** (edworld_eht): where the cockpit panels are on screen, published to a tmpfs file for EHT's
  overlay.
- **Discovery tools.** On request, dumps of the panels' surfaces and of what each panel draw reads (indices,
  vertices, constant buffers, instance records), with `tools/dump_to_png.py` and `tools/geometry_dump.py`.
- Chains to another d3d11 proxy (EDHM, ReShade, EDVR) named in its ini.

Known: after an on-foot conflict zone edworld once saw no panel draws for about four minutes (the jump panel
then kept the game's emblem); the Empire's, the Federation's and the independents' words on the panel are told by
widths measured on screenshots, not yet read from the panel in the game.
