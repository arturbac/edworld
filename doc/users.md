# edworld for players

edworld fixes the panel the game shows while the frame shift drive charges for a jump.

- **The right superpower emblem.** The game shows a wrong emblem there for the Federation, the Empire and the
  Alliance. edworld reads the superpower the panel itself writes and paints the right emblem over the wrong one.
  Independents get the game's phoenix in gold in place of its dull blue one.
- **The destination's factions** under the panel: by influence, the controlling one starred, the trend at the last
  tick, their states. They come from EDSM, or from Elite Help Tool (EHT) when you use it and it has the system.
  A small grey footer names edworld and its version.

Both move with the panel when the cockpit camera swings.

**Navballs (beta, 1.1.0-beta.1).** Two spheres beside the radar show where the selected target is. The left one is
the sphere seen from the front, as the game's compass shows it, with the angles above it: `UP`/`DN` above or below
the wings' plane, `LT`/`RT` left or right of the nose, `BEHIND` when the target is behind you, `NO DOT` when no
target's dot is read. The right one is the same direction seen from behind, the left and above, with the nose marked and `OFF`,
the target's angle off the nose. In a planet's gravity well it assumes the planet is the target and gives instead
`NOSE` (the nose's angle below or above the horizon), `PUSH DOWN` / `PULL UP` towards a dive of `compass_should_dive`
degrees (`ON PATH` within 2°) and `ALT`, the altitude with the vertical speed.

Apart from the jump panel and the navballs nothing in the game changes.

## Which dll

| dll | for |
|---|---|
| `edworld.dll` | anyone: the factions come from EDSM |
| `edworld_eht.dll` | players who run Elite Help Tool: the factions come from EHT's records first, EDSM otherwise |

## Install

Both dlls are in `edworld-<version>.zip` on the [Releases](https://github.com/arturbac/edworld/releases) page.

### On its own

1. Find the game's folder: `Products/elite-dangerous-odyssey-64`, the one with `EliteDangerous64.exe`.
2. If there is a `d3d11.dll` in it already (EDHM, ReShade, EDVR), rename it, for example to `d3d11_edhm.dll`.
3. Copy edworld's dll into the game's folder as `d3d11.dll`.
4. If you renamed another mod's dll in step 2, create `edworld.ini` (or `edworld_eht.ini`) beside it with
   `next = d3d11_edhm.dll` (the name you gave it).
5. Start the game; `edworld.log` (or `edworld_eht.log`) appears beside the dll.

### Through edloader

edloader runs several d3d11 mods at once (its own doc/users.md). Put edworld's dll into `edloader\plugins`, list it
in `edloader.txt`, and put `edworld.ini` with `next = d3d11.dll` into `edloader\config`. Its log is then in
`edloader\logs`.

### With Elite Help Tool

`edworld_eht.dll` reads what EHT knows of your destination from a shared folder, `shm_dir` in its ini. It must be
the folder EHT writes to (`edworld.dir` in EHT's `eht_settings.json`), as seen from the game: EHT's
`/dev/shm/eht-sjona` is `Z:\dev\shm\eht-sjona` for the game under Proton. Give each commander's EHT a folder of its
own: two tools writing one folder replace each other's destination.

## Settings

In `edworld.ini` (or `edworld_eht.ini`), beside the dll or in `edloader\config`; whole lines, `;` or `#` starts a
comment line. Every setting has a default; the file is needed only to change one.

| setting | default | what |
|---|---|---|
| `next` | empty | the next d3d11 mod (`d3d11_edhm.dll`); empty = the system's own; `d3d11.dll` under edloader |
| `patch` | `1` | `0` off, `1` the emblem and the list, `2` a test frame showing where they go |
| `independent_colour` | `F2C14E` | the independents' emblem's colour, `RRGGBB` |
| `list` | `1` | `0` = no factions list |
| `list_text` | `50` | the list's text height (pixels of the panel's surface) |
| `list_gain` | `0.8` | the list's brightness |
| `list_top`, `list_x` | `500`, `1540` | where the list starts under the panel (pixels of the panel's surface) |
| `list_rows` | `7` | factions listed at most |
| `edsm` | `1` | `0` = never ask EDSM (no list then without EHT) |
| `shm_dir` | `Z:\dev\shm\eht` | `edworld_eht.dll` only: the folder EHT writes to |
| `compass` | `1` | `0` = no navballs; the game's compass is then not read at all |
| `compass_spheres` | `1` | `0` = no spheres |
| `compass_anchor` | `1` | `1` the spheres held to the compass and the speed readout; `0` on the two side panels left and right of the radar |
| `compass_anchor_scale` | `1.9` | a sphere's diameter, in the game compass's diameters |
| `compass_anchor_a_dx`, `compass_anchor_a_dy` | `-0.41`, `0.45` | the left sphere's centre from the compass, in compass diameters (x right, y up) |
| `compass_anchor_c_dx`, `compass_anchor_c_dy` | `1.22`, `0.83` | the right sphere's centre from the speed readout, in compass diameters |
| `compass_sphere_text_above` | `1` | `1` the spheres' lines over them, `0` under them |
| `compass_text` | `0` | `1` = the angles also written above the radar |
| `compass_hide_game` | `0` | `1` = the game's own compass hidden once read (the left sphere stands in for it) |
| `compass_should_dive` | `35` | the dive, in degrees below the horizon, the right sphere steers towards near a planet |

The rest of the settings (`doc/developers.md`) are for finding things in the game.

EDSM is asked once per new destination, with the system's id only.

## Check that it works

The log lists `patch: first drawn` at your first charge, `superpower: ... reads Alliance on the panel` (or the
Federation, the Empire, Independent) for each new destination and `list: N faction(s) ... drawn, from EDSM` (or
`from the data source`, EHT). A system without a superpower (deep space) gets no emblem, and one without factions
no list.

Navballs: the log lists `compass: first drawn`, `compass: the left sphere first drawn by its anchor` (and the right
one), and `compass: the left sphere's centre at local (...)` with its size each time its place changes, for example
in another ship.

### Testing the navballs

For each ship you fly with the beta, open an [issue](https://github.com/arturbac/edworld/issues) with the ship's
type, a screenshot with both spheres and the log (`edworld.log` or `edworld_eht.log`). If a sphere sits in a bad
place, say where you would like it; `compass_anchor_*` moves it, and the values you used help too.

## After a verification of the game's files

Steam's and the launcher's verification removes from the game's folder everything that is not the game's: edworld's
dll and its ini. Copy them again. Through edloader only edloader's `d3d11.dll` has to be copied again.

## Remove

Delete edworld's `d3d11.dll` and its ini; put back the mod's dll you renamed, as `d3d11.dll`.

## Known

- The game's wrong emblem may show for a few frames at the first charge to a destination, while edworld reads the
  panel.
- After an on-foot conflict zone edworld once saw no panel for about four minutes; the jump panel kept the game's
  emblem until then.
- Navballs (beta): placed and checked in the Kestrel's cockpit only; other cockpits are larger or smaller, so the
  spheres may sit too close to or too far from the radar. The lines near a planet are not yet checked in the game.
  The game does not draw its compass every frame; a sphere keeps its last reading in between.
- The Empire's, the Federation's and the independents' words on the panel were measured on screenshots; a word
  edworld does not recognise leaves the game's emblem and says so in the log.
