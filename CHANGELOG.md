# Changelog

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
