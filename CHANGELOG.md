# Changelog

## Unreleased

- **Navball colours.** The spheres' colours are settings (`compass_colour_*`, `RRGGBB`), a muted grey by default;
  `doc/users.md` has the beta's blue as an example.
- **Approach view (proof of concept).** In normal flight with the target 20° or more below the wings, the left
  sphere shows it from above and the right one a dome standing on it, the ship a point on the dome; after a planet's
  glide the right sphere is always the dome and the dive guidance shows only in orbital cruise and glide.

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
