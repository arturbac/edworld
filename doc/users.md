# edworld for players

edworld fixes the panel the game shows while the frame shift drive charges for a jump.

- **The right superpower emblem.** The game shows a wrong emblem there for the Federation, the Empire and the
  Alliance. edworld reads the superpower the panel itself writes and paints the right emblem over the wrong one.
  Independents get the game's phoenix in gold in place of its dull blue one.
- **The destination's factions** under the panel: by influence, the controlling one starred, the trend at the last
  tick, their states. They come from EDSM, or from Elite Help Tool (EHT) when you use it and it has the system.
  A small grey footer names edworld and its version.

Both move with the panel when the cockpit camera swings.

**Navballs (beta, 1.1.0-beta.2).** Two spheres beside the radar show where the selected target is. The left one is
the sphere seen from the front, as the game's compass shows it, with the angles above it: `UP`/`DN` above or below
the wings' plane, `LT`/`RT` left or right of the nose, `BEHIND` when the target is behind you, `NO DOT` when no
target's dot is read. The right one is the same direction seen from behind, the left and above, with the nose marked and `OFF`,
the target's angle off the nose. In a planet's gravity well it assumes the planet is the target and gives instead
`NOSE` (the nose's angle below or above the horizon), `PUSH DOWN` / `PULL UP` towards a dive of `compass_should_dive`
degrees (`ON PATH` within 2°) and `ALT`, the altitude with the vertical speed.

> **Warning: screen resolution.** The navballs are checked at one resolution only: 9000×2160 over three screens,
> the cockpit on the middle 3840×2160. At 1920×1080 or lower, do not expect much:
> - edworld reads the game's compass off the HUD surface the game draws it into: 2200×1800 here
>   (`compass_surface_width`, `compass_surface_height`), the compass about 54 px in radius (`compass_radius`).
>   Whether the game sizes that surface by the screen resolution is not checked. If it does, edworld may not find
>   the compass at all until those settings match. A smaller compass also reads coarser: here about 0.2° near the
>   nose and about 2° near the rim.
> - The spheres and their lines are drawn into textures of a fixed size laid on the cockpit, so on screen they
>   shrink with the resolution. Here the lines in the two columns near a planet are about 10 px tall; at
>   1920×1080 they would be about half that, too small to read.
>
> A report of how they work at another resolution (an issue with a screenshot) is welcome.

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

[edloader](https://github.com/arturbac/edloader) runs several d3d11 mods at once. The release zip carries it in its
`edloader` folder (its `d3d11.dll`, `edloader.txt.example` and its own `doc/users.md`, which tells how to install
it). Put edworld's dll into `edloader\plugins`, list it in `edloader.txt`, and put `edworld.ini` with
`next = d3d11.dll` into `edloader\config`. Its log is then in `edloader\logs`. With EDHM and EDVR, list EDHM before
EDVR (edloader's doc/users.md, "EDHM and EDVR").

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
| `compass_sphere_text_above` | `1` | `1` the spheres' lines over them, `0` under them. Over them at a planet, the right sphere's lines go in two columns by the band's edges: the game writes its latitude, longitude and gravity in the middle |
| `compass_text` | `0` | `1` = the angles also written above the radar |
| `compass_hide_game` | `0` | `1` = the game's own compass hidden once read (the left sphere stands in for it) |
| `compass_should_dive` | `35` | the dive, in degrees below the horizon, the right sphere steers towards near a planet |
| `compass_approach_below` | `20` | in normal flight with the target this many degrees or more below the wings, the approach view: the left sphere shows the target from above (straight down in its middle, `FWD`/`AFT` and `LT`/`RT` off the vertical), the right one a dome standing on the target with the ship a point on it (straight over the target at its top; in space the whole sphere, the base a plane with a grid); `0` = never |
| `compass_approach_radius` | `54.8` | the dot's distance from the compass's centre with the target on its rim (pixels of the HUD surface); the angle ahead or behind near a pad comes from how far short of it the dot is |
| `compass_approach_smooth_s` | `0.4` | in the approach view the dot's place is averaged with this time constant (seconds), so the spheres do not shake near a pad; `0` = no averaging |
| `compass_colour_disc` | `101214` | the spheres' colours, `RRGGBB` (see "Navball colours"): the disc behind a sphere |
| `compass_colour_grid` | `5A6068` | the rings at 30° and 60° and the meridians |
| `compass_colour_rim` | `A8AEB5` | the left sphere's rim and the wings' plane on the right one |
| `compass_colour_target` | `FFFFFF` | the target's dot |
| `compass_colour_behind` | `D9A15A` | the target behind, where it should be, `PUSH DOWN` / `PULL UP` |
| `compass_colour_nose` | `8CC9A0` | the nose's arrow |
| `compass_colour_text_left` | `C9CED4` | the left sphere's angles |
| `compass_colour_text_right` | `D6CFB4` | the right sphere's lines |
| `compass_colour_ok` | `9AD3A8` | `ON PATH`; the approach view's base while the ship is over it |
| `compass_colour_wrong` | `C0504D` | the approach view's base while the ship is under it (in space only: near a planet the pad is on the ground and the base stays `compass_colour_ok`) |

The rest of the settings (`doc/developers.md`) are for finding things in the game.

### Navball colours

The spheres are a muted grey by default. Each `compass_colour_*` setting takes a colour as `RRGGBB` (hex, no `#`);
the fainter shades (the far side of the right sphere, the middle of the disc, the outline) are mixed from them. For
example, the blue of 1.1.0-beta.1:

```ini
compass_colour_disc = 080E1E
compass_colour_grid = 325596
compass_colour_rim = 5096FF
compass_colour_target = FFFFFF
compass_colour_behind = FF9628
compass_colour_nose = 50FF8C
compass_colour_text_left = 96E6FF
compass_colour_text_right = FFE678
compass_colour_ok = 78FFA0
compass_colour_wrong = FF5050
```

To make your own, change the lines you want; a setting left out keeps its grey. The ini is read when the game
starts. `compass_sphere_gain` makes all of them lighter or darker.

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
  spheres may sit too close to or too far from the radar. Near a planet the lines are checked at one surface port
  (two landings); in the two columns they are small, about 10 px tall at 3840×2160. With a settlement or a surface
  port targeted, `NOSE` and `PUSH DOWN` / `PULL UP` still take the planet's centre as the target and are wrong there.
  The game does not draw its compass every frame; a sphere keeps its last reading in between.
- Approach view (beta; checked at an outpost in space and at a surface port): near a pad the game's compass puts the target on its rim, so ahead or behind is
  read from a fraction of a pixel there (a few degrees at best), left or right to about a degree. Below the
  planet's glide the right sphere is always the dome; its base is parallel to the ship's wings, not to the ground
  (Status.json gives neither pitch nor roll).
- The Empire's, the Federation's and the independents' words on the panel were measured on screenshots; a word
  edworld does not recognise leaves the game's emblem and says so in the log.
