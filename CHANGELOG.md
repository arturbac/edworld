# Changelog

## Unreleased

- The log says once where the game's context table lies (memory type, module and section) and which of
  `ID3D11DeviceContext1`..`4` and `ID3D11Multithread` the context answers at its own pointer; a diagnosis for hooks on
  Windows, nothing changes by it.

## 1.2.1

Corrections to 1.2.0: edworld draws only when the game's state fits its shaders, and its shaders' loops have an end.

- edloader 1.3.1 in the zip.
- The version in the log: `vX.Y.Z` only for a release; any other build `<the last tag>-<commit's short hash>`, with
  `-dirty` when built from uncommitted changes (before, `git describe`: a build of uncommitted changes on the tag's
  commit said only `vX.Y.Z-dirty`).
- edworld's own draws only on a fitting state of the game: the clouds over the game's shell and the list's and the
  spheres' quads are not drawn while the game's target is an integer one, more than one target or its UAVs are bound,
  or its constant buffers hold fewer rows than edworld's shaders read (the quads: also its records at t33 of another
  stride, or the panel surface read in an integer format); the log names each reason once. Before, they were drawn
  whatever the state.
- `clouds_steps`, `clouds_far_steps`, `clouds_max_steps` held to 1..256, and the clouds' march ends at 256 steps in
  the shader too: a value with no upper bound could make a loop the graphics card cannot end in time.
- The storms' loops end at 8 in the shader; the clouds at half size only over a picture of the depth's size; the
  depth and the gates read inside their textures.
- The patch draws only on panel surfaces of the four-channel UNORM and float formats; its masks and the copy of the
  panel's text are made again when the surface's format changes; the surface is unbound as a shader resource before
  it is bound as the patch's target.
- Tests: the VS drawn with the panel's PS give it the TEXCOORD0 it reads; the CPU bench draws with an input layout
  and a vertex buffer.

## 1.2.0

The clouds (by the atmosphere's kind, with weather, lumps and storms), the aurora and the snow of the betas below,
released: no longer proofs of concept in the documentation.

- `clouds_wind_high` is `600` km/h by default (`400` before): the wind at the top of the clouds with depth.
- edloader 1.3.0 in the zip.

- `clouds_noise_time` (1 by default): the grain of the clouds with depth (the aurora and the snow too) moved each frame while edupscale's temporal
  anti-aliasing jitters the camera, so it averages the grain instead of the clouds flickering; `0` never, `2` always.

- Each d3d11 proxy before edworld in the chain is named in the log at the first device (`warning: dll before edworld
  in the chain: ...`, with its version resource), with edloader or with a chain made by hand. edworld_eht stays off
  for the session with a 3Dmigoto (EDHM) before it, told by its version resource: the log and its corner line say so,
  and its catalog of shaders does not learn EDHM's replacements.
- The game's shaders edworld knows by name in one place, `src/shader_ids.h`.

- `build_info`: edupscale's and edwide_screen's lines (their version and what they do now) beside edworld's when
  they are loaded, from their export `<name>_build_info`.
- simple_enum by CPM (the version of elite_help_tool's): enumerations in the log by their names (the shader stages
  created, the D3D11 blend and depth states); the formats stay numbers.
- snprintf, swprintf and vsnprintf replaced by `format_into` (std::format_to_n, cut and NUL-terminated as snprintf,
  never thrown); `full_line` of the whole-frame dump checked when compiled too.
- The log through `std::format` (checked when compiled; wide strings as UTF-8) in place of printf's `va_list`; the SRW
  locks held by scope objects.
- stralgo (and small_vectors) by CPM, the versions of elite_help_tool's: the settings' notes and the paths put together
  with `merge` in place of `+` chains.
- **The compass in a coloured EDHM theme:** a pixel is the dot's when its hue is 30 degrees or more from the disc's
  (60 before): a player's red disc (hue ~0) with a yellow dot (~40) gave NO DOT, not one pixel of the dot read.
  `compass_dot_hue_gap` sets it. The flash rule (a disc turned the dot's hue) takes the same gap.
- doc/performance.md: the cost in the game and the bench, moved from developers.md; linked from the README.

## 1.2.0-beta.14

- **What it costs, measured:** in the game at 9000x2160 (RX 7900 XTX, Linux, DXVK, a CarbonDioxide planet) the clouds
  take about 1.4 ms a frame from orbit by day, 1.0 ms by night, 5.3 ms inside the layer, and 6.3 ms inside an aurora
  (the aurora itself about 4 ms); 60 fps held in each. A draw through edworld adds about 0.2 us to the render thread.
  The numbers and the bench they are set against: doc/developers.md ("Cost in the game and the bench").
- **Tests without the game:** the render test draws the clouds, the aurora and the snow of six kinds with the game's
  own buffer and compares them with reference pictures; tools/bench.sh measures the cost per scene under DXVK.
- edloader 1.2.1 in the zip.

- **After a second audit:** the atmosphere's reads no longer wait for good behind the shell drawn for reflections
  (no day read, so no storms and no aurora); a read of no known kind never takes a known kind away; the weather's
  pattern comes in over 4 s once the planet's day is read, as the storms do; the clouds come in when another planet
  becomes the nearest; the snow no longer jumps 10-20 m every 10 km at speed; each part of a hologram copies the
  record pool again (the target's records written after the own ship's); a read the atmosphere's thread does not finish
  in 10 s, or a thread that stops, hands the work to the render thread; a writer thread that stops says so and
  nothing more piles up; no out-of-memory in a few functions ends the game; a path in the ini may hold ` #` or ` ;`;
  `clouds gpu 5s` times one draw in five, so two shells are both timed; the compass's line every second is off by
  default (`compass_log_ms = 1000` brings it back).
- **The docs agree with the code:** the snow's 12 samples from 1.5 m, the second and third aurora arcs coming in from
  levels 2 and 5, ammonia's second colour as seen on the two coldest ammonia planets. Methane's snow takes its kind's
  typical 93 K as the rest of edworld does (95 K before). A value of the atmosphere's table that is an infinity or
  NaN no longer counts as its brightest light.
- **Less work on the game's render thread:** the compass spheres and the faction list are drawn again only when what
  they show changes (each frame before), the panels' part of the game's record pool is copied each frame instead of all of it (0.7 to 3.4 MB),
  the draws between the HUD's panels take no timestamps, the corner text keeps its target's check, the galaxy map's
  names wait for their interval before looking at the target, the atmosphere's reads keep their copies instead of
  making them each time and are worked out on a thread of their own, one cloud draw in five is timed, and a watched shader is found with one look. The EDSM
  request runs on a thread of its own, so the charge and the flight keep coming while EDSM answers. The log's
  summary every second is off by default (`log_interval_ms = 1000` brings it back); its lines go out in one write.
- **Clouds cheaper in storms and on the screen around them:** a ray looks only at the storms it passes, the storms'
  own shadow is worked out on every other step, and the pass laying the half size clouds over the picture skips the
  pixels with no clouds. The weather's swirls and the lumps' size follow the storms as the clouds themselves do, and
  the weak, low lumps of the coldest kinds keep their whole column. clouds.md says why helium's clouds are thinner
  than its pressure gives and that slowly turning planets have fewer clouds than their kind's cover.
- **Snow without jumps:** the crystals keep their places after hours of play and across 10 km of flight, the eye's
  way is timed by when the game wrote Status.json and held where it went when you stop, a switch of the altitude's
  reference (a pad, the ground under the ship) moves nothing, the snow follows the clouds straight above as they are
  drawn, and it falls on foot also where the ground stands over 5 km above the planet's mean radius.
- **The aurora steadier:** its rays keep their places as the arcs fold (they ran along them at tens of km/s), no
  seam where the oval closes at noon, substorms come in over levels 2.5 to 3.5 and their dance falls off smoothly,
  the flicker keeps its beat after hours, it stands over the clouds that storms raise, a strong star's wind raises the
  level for the substorms as for the oval, a planet with no field of its own glows the same in a storm, and a brown
  dwarf whose light is red-poor is told by its blue.
- **Storms and the aurora wait for the planet's day** (read about half a minute near the planet) and then come in over
  four seconds, instead of standing on the kind's typical day first and jumping once it is read.
- **Two atmospheres in view keep their own kinds.** Each planet read keeps its kind and its day; the clouds, the snow
  and the aurora follow the nearest. A single read that matches no kind no longer drops a planet's known kind for ten
  seconds (another kind takes over after three reads in a row), and the reads take each planet in view in turn.
- **The ini says what it cannot read.** A value edworld cannot read (`0,45`, a word, NaN, a number too large) keeps
  its default and the log names the line; an unknown name (a typo) is logged too; a comment after a value
  (`clouds = 1 ; on`) is taken off, but not from a path or a dll's name. The ini edworld writes leaves `status_file` empty (the Saved Games folder), so it
  holds no Windows user name.
- **The game's state kept whole around edworld's drawing:** constant buffers with their offsets, the pixel shader's
  UAVs and the hull, domain and compute shaders are put back as the game had them. edworld follows only the game's
  first device (a second is logged, not followed). A surface's size is read anew at each draw.
- **No error of edworld's own takes the game down.** A C++ exception (out of memory) in edworld's work on the
  render thread now reaches its guard instead of ending the process; every fault or exception caught is logged with
  its code and place. edworld's own threads are guarded too and the lasting ones start again after a fault, eight times at most.
  Files and the log are written with Windows' own calls (a file not written whole is removed), the live settings
  are read into a second copy and then published, a change of the ini before the first clouds is no longer missed,
  and edworld's own draws pass its hooks unobserved.
- **No crash at the game's exit with a `status_file` in a folder that does not exist.** The thread reading
  Status.json is stopped wherever it is when the game exits; caught in a failed open, it left a file half open, and
  the C runtime's closing of all files when edworld was unloaded ended the process (0xC0000409). Status.json and
  the data source's files are now read with Windows' own calls.

## 1.2.0-beta.13

- **edloader 1.2.0 in the zip:** it knows ReShade (`dxgi.dll`) and needs nothing set for it. doc/users.md: ReShade
  and an antivirus with edloader (Bitdefender: edworld needs nothing set; edloader, if its log says `TAKEOVER:` with
  no ReShade loaded, `accept_takeover = true`).
- **edworld's version in a corner of the screen** (`build_info`, 3 = the bottom right by default; 1 top left, 2 top
  right, 4 bottom left, 0 none), in place of the footer under the factions list. Drawn into the swap chain's back
  buffer right after the game's own drawing of it; the log's `build_info:` line says where it went, or that no back
  buffer was seen.
- **A program in the game's own call no longer hides the game's device** (`creator_pass`). A dll from outside the
  game's folder in the call for the game's device (an antivirus's hooks, `bdhkm64.dll` in a player's log) made
  edworld take the game's device for an overlay's and follow none. The `not followed` line now names the setting with
  the dll; the dlls listed in `creator_pass` are passed over like a hook's code, so the game behind them is found.
  `bdhkm64.dll`, the one in that log, is listed by default: such a player needs no change in the ini.
- **Argon known at more planets.** edworld took a hue for a kind only within 0.004 of the kind's, which the closest
  two kinds need; an Argon planet 0.005 off was left without its kind, so without its clouds and its aurora. Now each
  kind may lie as far off as half the way to the nearest other kind, from 0.004 to 0.010.
- **The target behind read in an EDHM theme.** An EDHM theme draws the hollow dot (the target behind) with 25 to 58
  pixels, the game's own HUD with about 111; edworld took a hollow dot only from 60 pixels, so in an EDHM theme the
  navballs said `NO DOT` with the target behind. `compass_hollow_min` is now 20 by default (was 60).
- **The game's own compass hidden by default** once the dot is read, the left sphere standing in for it
  (`compass_hide_game` 1, was 0); it returns whenever edworld is not there.
- **The pads' disc turns yellow and red later:** green up to 15 degrees of tilt, red from 35 (`pad_tilt_green_deg`,
  `pad_tilt_red_deg`, were 10 and 20).
- **Snow (PoC, `snow`, on by default).** On foot outdoors or in a ship or an SRV in the air of an ammonia or
  methane planet, crystals of its own gas fall near you under the clouds, streaking past as you fly (`snow_bench`, a
  test, measures the cost of its parts); a cold ammonia planet (its second colour) snows steadily and glints a little in a
  clear sky. From the render and Status.json alone. See doc/clouds.md.
- **edworld_eht: the atmosphere's print for EHT** (`atmosphere` in `shm_dir`). Each read of a planet's atmosphere
  off the render puts its radius, its print and whether edworld knows that print (its kind, a second print of the
  kind) where EHT reads it, so EHT can say when an atmosphere is a case never seen.
- **edworld_eht: the galaxy map's names for EHT** (`galmap`, `galmap_interval_ms`). While the map is open, a few times a
  second, the names it shows are read off its text draws, each glyph by its shape in the game's font, and put in
  `galmap` in `shm_dir`. Letters not learned yet are `?`.
- **The aurora seen from afar no brighter than it is.** A curtain thinner than the step a far ray samples is widened
  to that step so the ray meets it; it kept its brightness, so from orbit its light grew as many times (tens of
  times on a small planet, a bright smooth band). Now it is as much dimmer: its light stays a thin curtain's.
- **A whole-frame dump also on the galaxy map** (edworld_dump_all, EHT's F10): without panels drawn a frame begins at
  any draw after a gap; before, the dump waited for the next panel.

## 1.2.0-beta.12

- **Aurora (PoC).** On the night side of a planet whose atmosphere's kind is known, an oval of curtains around its
  pole, over the clouds, in its gas's colours: oxygen green low and red high, nitrogen violet with a pink lower edge,
  neon orange, and so on. Whether a planet has a field of its own comes from its size and turn (the game has none to
  read), how strong from its star's class read off the colour of its light, how stormy from the time, each 3 hours
  its own and the same for everyone. In a substorm the aurora brightens over minutes and dances in seconds: waves
  run along its arcs, curtains fold, rays light up. One arc in a quiet sky, two in a storm, a third while a strong
  substorm expands, each broken by gaps; a bright part is wider and taller than a faint one. Each arc wiggles across
  itself on its own and winds into curls here and there. It stands as high as the kind's air makes it: low over the
  cold, thin neon worlds, hundreds of km up over small carbon dioxide and ammonia worlds. Its curtains are thin, tall
  sheets, a few km thick, of dense rays about a km apart, each growing and fading on its own over a few seconds: a
  strong ray bright, tall and coming down deeper, a weak one faint and short, so the curtain is ragged at its foot
  and at its top, and only the tall rays reach the high colour. Its wiggles and curls are slow and few. It costs the
  GPU less where much of it is in view: its rays and gaps are worked out only near each arc. Settings `aurora`, `aurora_gain`, `aurora_activity`,
  `aurora_dance`, `aurora_flicker`, and for a look on any planet `aurora_field`; doc/auroras.md says how, which colours there are and how often.
- **A second ammonia.** The game draws some ammonia atmospheres in a colour of their own, so edworld did not know
  such a planet and gave it the default clouds. Now it is ammonia too.

## 1.2.0-beta.11

- **edloader 1.1.0 in the zip: Epic's overlay needs nothing set.** The game from the Epic Games Store loads Epic's
  overlay, which takes the game's calls a moment after its first device; edloader 1.0.1 stopped there unless
  `accept_takeover = true` was in `edloader.txt`. edloader 1.1.0 knows the overlay and goes on by itself. doc/users.md
  says what to do with an older edloader.

- **Fixed: on foot the atmosphere's kind was not told.** edworld took the game's atmosphere in only while the ship's
  HUD was drawn, so a session begun in a settlement, or on foot, had the clouds of no kind (as before 1.2.0-beta.9)
  until you boarded a ship. Now it is told a moment after the planet is in view, on foot too; when no read gets
  through for a minute, the log says why.

- **Storms keep their clouds in the weather's clear bands.** The weather of 1.2.0-beta.10 thinned a storm that stood
  in a clear band, as it moves the clouds' edge everywhere. Now it is given back inside a storm, which brings its own
  clouds, so the storms look as in 1.2.0-beta.9 wherever they are.

## 1.2.0-beta.10

- **Volcanic neon planets get neon's clouds.** The game draws a neon atmosphere over volcanism in a colour of its
  own, so edworld did not know it and gave such a planet the default clouds. Now it is neon too.
- **The clouds' colour by the kind.** The kinds' tints were so close to white that every planet's clouds looked the
  same white, coloured only by the star. Now ammonia's clouds are cream, methane's orange-brown, sulphur dioxide's
  yellowish and silicate vapour's grey-brown, from what colours them; the kinds whose ices are white get a slight
  tint only to tell them apart, and water's stay white. `clouds_tint` scales it (`0` = white, `2` = twice as far).
- **Lumps of more than one size.** The clouds' shape had one scale over the whole planet, so its lumps were the
  same size everywhere. Now a slow noise over the planet makes them larger in some regions and smaller in others
  (`clouds_size_var`, by default their scale times 1 ± 0.25; `0` gives the old clouds).
- **Lumps by the layer's thickness (PoC).** The lumps were a fixed share of the planet's radius, so a small planet
  had small ones and a large planet huge flat ones, all reaching the same height. Now their size follows the
  thickness of the kind's cloud layer (larger on small planets, smaller on large ones, the fine shape near you too),
  and each lump is as high as it is strong: weak ones low and flat, strong ones towering, warm kinds' higher than
  cold ones' (`clouds_lumps_by_layer`).
- **Weather (PoC).** Overcast and clear regions instead of one cover over the whole planet: on a planet that turns
  slowly for its size the clouds gather on the day side under the star; on a faster one they lie in bands by
  latitude, the equator and the middle latitudes overcast and about 25° clearer, with patches the size of its
  weather systems (`clouds_weather`).
- **doc/settings.md.** Every setting by its topic (clouds, navballs, jump panel, other), its default, the values
  that make sense, and whether it is read while the game runs or at its start. edworld makes the page from its own
  table of settings, which also says which settings are read again while the game runs, so the two cannot drift.

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
