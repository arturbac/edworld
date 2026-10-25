# edworld for developers

How edworld works inside the game process, how it is built and tested, what was learned in the game, and how to
write a d3d11 plugin of your own on the same pattern. Players: [users.md](users.md).

## The two builds

One source, two dlls (`tools/build.sh` makes both; the difference is `EDWORLD_EHT` at compile time):

| Build | For | Files beside it |
|---|---|---|
| `build/edworld/edworld.dll` | anyone: works on its own; the superpower is read from the panel, the factions come from EDSM; publishes nothing, so it copies nothing of the panel draws | `edworld.ini`, `edworld.log` |
| `build/edworld_eht/edworld_eht.dll` | EHT users: also publishes the panels' anchors to `panels` (EHT's overlay follows them) and takes the factions from EHT's `target` before EDSM | `edworld_eht.ini`, `edworld_eht.log` |

The ini and log names are fixed by the build, not taken from the dll's file name: installed alone as the game's
`d3d11.dll`, either build still reads its own ini. Below, "edworld_eht only" marks what the plain build lacks;
`<plugin>` is `edworld` or `edworld_eht`.

## What it does

- Forwards every d3d11 export to the next d3d11 in the chain (`next` in `<plugin>.ini`: EDHM, a renamed
  EDVR, or the system copy). Chaining discipline from EDVR (MIT): system copy loaded in DllMain, next proxy
  on the first export call, a thread-local depth guard for a next proxy that asks for `d3d11.dll` by name.
- Hooks (in place, vtable slots): `ID3D11Device::CreateVertexShader` (FNV-1a 64 of the bytecode, the hash EDVR
  names shaders by) and `ID3D11DeviceContext::VSSetShader`, `Draw`, `DrawIndexed`, `DrawInstanced`,
  `DrawIndexedInstanced`, on the immediate context.
- Watches the cockpit holo-panel vertex shaders (`81216C77F90DEDD6`, and `1989E6D3B405FDE0` when the game's
  "Disable GUI effects" is on, and `925ACEDA0153AA5B`, which the game switches the same panels to near a war
  settlement, with the surface at PS t1). Whether that shader's instance records decode like the family's is not
  checked; the patch does not need them. A shader not in the list is found by the surface it draws: each new
  vertex shader's first draws (up to 64, PS t1/t2 looked at) or, during a charge with no panel drawn for a second,
  the probe; one that samples a panel-sized surface (3072x660, 2048x1280, 2200x1800, 1024x1534) is watched from
  then on and named in the log, gets the patch, and publishes nothing (its records are not known to decode).
  Afterwards a shader costs one table look-up per bind. For each of their draws (edworld_eht only): GPU-copies VS cb0 rows 0..11 into a staging ring,
  notes the interface surface the panel samples (PS t2, else t1: pointer, size, format) and the draw
  arguments. Frames are split by a gap between panel draws (`frame_gap_us`), so no DXGI hook is needed.
- (edworld_eht only) The panel's place comes from its instance record, as the game's own shader reads it: the VB0 instance entry
  at `start_instance` names a 336-byte record in the VS t33 pool (scale, orientation, position), and VS cb1 row
  275 is the world-rebase origin; anchor = cb0 rows 4..7 (the camera) applied to (position - rebase, 1). The
  pool is GPU-copied whole once a frame.
- (edworld_eht only) Reads the copies 1-3 frames later with `Map(DO_NOT_WAIT)` (never a CPU read of a mapped dynamic buffer:
  write-combined under DXVK) and publishes the latest frame to `panels` in `shm_dir` (seqlock; layout in
  `src/edworld_share.h`).
- **Jump panel patch** (`patch = 1`): while `Status.json` says a jump charges (Flags2 bit 19, and Flags bit 30 not yet set: that bit comes with
  StartJump, when the panel is gone, while bit 19 stays until FSDJump), at the draw that
  composites the panel's surface (3072x660) into the cockpit, paints the panel's black and the destination's
  emblem in its colour over the wrong one, then lets the game's draw go on. The patch shows only where the game
  drew on the surface in that frame (the surface's alpha under each pixel, copied just before): once the charge
  is done and the ship still aligns, the game hides the panel while Status.json still says charging, and the
  patch goes with it. The superpower is read from the panel itself: the game writes it right, as text in the row
  labelled SUPERPOWER, while its emblem is wrong. Each charge to a new destination copies the panel's text area
  (x 1100-1980, y 100-470 of the surface) and reads it a few frames later, when the copy is done (the game's wrong
  emblem may show for those frames at the first charge to a destination): the rows are the bands of light pixels
  in the labels' column, the SUPERPOWER row is the one whose label is 231 pixels wide (never one of the first two,
  the region and the system), and the word is told by its width against the label's (EMPIRE 0.509, ALLIANCE
  0.667, FEDERATION 0.859, INDEPENDENT 0.994; the game draws its text the same every time). The emblem stands 40
  pixels under that row's centre, beside it and the two rows under it, whatever the emblem's shape. A row whose
  word fits no width gets no emblem, a log line and its area written to `edworld_dumps`; a panel without the row
  (no superpower, deep space) gets none either. No data source and no EDSM for the emblem: only for the list.
  Independents get an emblem too, the game's phoenix (EDAssets' `independent-power.svg`) in `independent_colour`
  (default a honey gold, F2C14E), in place of the game's dull blue one. The patch is a Dear ImGui
  draw list rendered by ImGui's D3D11 backend into the surface (own ImGui context, no input, no files);
  everything the game had bound is read back first and put back after.
- **Factions list** (`list = 1`): with the patch, a box under the panel with the destination's factions by influence:
  the trend at the last tick (up, flat, down; none when not known), a star for the controlling faction, the name in
  its superpower's colour, the influence and what goes on in the faction (active states, else the recovering ones).
  The list comes whole from one source, never a mix: in edworld_eht from `target` when EHT has influence readings of
  the system (with the pushes on the economy and security bars EHT counts from the missions handed in); otherwise,
  and always in edworld, from EDSM's answer to the same question as the emblem's, with a last row "from EDSM,
  updated ... ago" (the newest `lastUpdate` of its factions; EDSM keeps no tick, so its rows have no trend).
  It is not drawn on the surface: the surface is an atlas, the jump panel shows only its own rectangle of it (x
  1031-2045, y 2-493 of 3072x660, read from the panel's vertices), and the rows below belong to other panels (a
  one-pixel strip at row 519 is stretched over the jump panel; a list drawn there turned it into a black box). The
  list is drawn by ImGui into a texture of its own, and right after the game's draw of the jump panel edworld draws
  a quad of its own: the panel's vertices (read once per draw, see below) give its local-to-surface map, the list's
  box (`list_x`, `list_top`, `list_width`, in the surface's pixels past the panel's edge) goes back through it to
  local points, and the list's vertex shader places them as the game's shader places the panel (its instance
  record, the world-rebase origin, its clip rows, still bound from the game's draw). It shows while the game shows
  the panel (the surface's alpha at the patch's centre). A system without factions (uninhabited, or unknown to
  EDSM) gets no list. Text in Noto Sans Mono (SIL OFL 1.1, the font of EHT's overlay), made once at the first patch
  at `list_text` pixels (50: two and a half times the panel's own text, at which the factions' names read in the
  cockpit), in a black box as wide as its rows and centred under the panel (`list_width` the widest it gets); its
  texture has a full mip chain, sampled half a level sharp, as the cockpit shows it at about 0.7 of its size (the
  game's supersampling below 1). `list_gain` (0.8) scales its colours: the cockpit's target is HDR
  (R11G11B10_FLOAT), where white text blooms more than the panel's own. The first quad drawn logs the target's
  format and the game's blend for the panel. The box's footer names edworld and its version (`edworld v1.0.0`),
  small and grey in its bottom right corner. In test mode
  (`patch = 2`) the box is drawn with placeholder rows when there is nothing to list, to see where it goes.
  The panel's vertex (stride 40): the packed local position in bytes 0..15 (EDVR's decode), the surface
  coordinate in bytes 16..19 as two unorm16 halves, `(h / 32767 - 1) * 16`.
- On request, dumps every panel's interface surface: create an empty file `edworld_dump` in the log folder; at
  the next frame each surface is copied and written to `edworld_dumps\<stamp>_<id>_<w>x<h>_f<fmt>_pitch<n>.raw`
  and the trigger file is removed. `tools/dump_to_png.py <dir>` makes PNGs. With the surfaces, each panel draw of
  that frame has its index range, vertex buffers, VS constant buffers 0..3 and record pool written beside them
  (`<stamp>_g<draw>_<what>.bin`, the draw's arguments in the log); `tools/geometry_dump.py <log> <dir>` says which
  rectangle of its surface each draw shows and where its corners land.
- On request, dumps a whole frame: an empty file `edworld_dump_all` in the log folder, or (edworld_eht) a change of
  the file `dump` in `shm_dir`, which the data source writes when its key for it is pressed (EHT: F10). Every draw of
  the next frame on the immediate context gets a line `a<draw> <kind> vs <hash> count .. inst .. vp .. rt <w>x<h> f<fmt> at <id>` (the
  target's low 32 address bits, as the pixel shader's textures are listed) in
  `<stamp>_draws.txt` and the first 1024 bytes of its VS constant buffers 0..3 as `<stamp>_a<draw>_cb<slot>.bin`, up
  to 12000 draws; the log says when the frame ended after arming, the gap before the panel draw that began the next one
  (`frame_gap_us`) and how many draws of other contexts came meanwhile (not recorded); the panels' surfaces of that frame are dumped as above. Draws of the shaders in `dump_vs` also get
  their index range (`_a<draw>_ib_u16/32.bin`), vertex buffers from their base vertex (`_a<draw>_vb<slot>_s<stride>.bin`,
  256 KiB), their VS constant buffers whole from the first constant (a line `a<draw> cb<slot> first .. count ..` each)
  the target as the draw left it (`_a<draw>_rt_after_<w>x<h>_f<fmt>_pitch<n>.raw`, up to 8192 px: the game's main target
  too, about 27 MB each; all of the frame's copies together at most 56; their time, like the rest of the dump's own work, is not counted in the gap between panels) and the VS record pool at t33 (`_a<draw>_t33.bin`; the draw's records from its start instance), the
  pixel shader's t0..t7 are listed under the draw's line and copied (`_a<draw>_ps<slot>_<w>x<h>_f<fmt>_pitch<n>.raw`,
  first mip, up to 4096 px), and their shader's bytecode is written once per dump (`<stamp>_vs_<hash>.dxbc`, kept from its creation).
  Every 1000th draw, the screen-sized render target being drawn to (2048 px wide or more) is copied as it is before
  that draw (`<stamp>_c<draw>_<w>x<h>_f<fmt>_pitch<n>.raw`, the last at the frame's end); so is it before every 4th of
  the first 52 draws (the panels laid over the scene), and each such target once as the draws leave it (`c<draw>` =
  the first draw after its pass, e.g. the UI atlas); at most 56 copies. They show between which draws something
  appears on the picture. The files are written by a thread of their own, so the game does not stall; once the last is written, edworld_eht
  replaces `dump_done` in `shm_dir` with `<stamp> <draws>`, which the data source shows (EHT: on the overlay). It is
  for finding what is not a panel (the target's hologram, the camera, the light) by what changes with it between two
  dumps.
- **The target's hologram** (PoC, `station_axes = 1`): the game draws its holograms with one vertex shader
  (`station_vs`, 5B0068AF5630F96B on 2026-10-20), each into a 512x512 surface of its own, and lays each over the cockpit
  with a full-screen quad (VS 5559BD94B6852E83, reading the surface as PS t4): the ship's own (a Kestrel: 27885
  indices, record scale 0.0055) and the target's (a commercial outpost: 3576 indices, scale 0.0004), drawn one right
  after the other (2026-10-21: the dump's target ids). The same shader draws a few models in the scene too, so only its
  draws of 200 indices or more to a 512x512 target count. Its disassembly (`fxc /dumpbin`) says how a vertex is placed:
  `q v q*` times the record's scale (byte 4), plus the record's position (byte 16) less the world-rebase origin (cb1
  row 275), through cb1 rows 270..273 to clip space; the record is the panels' 336-byte one (VB0 entry at the start
  instance -> index -> VS t33), `q` four unorm16 at byte 8, each `u / 32767 - 1` (the listing shows the constant
  rounded to 0.000031). For each draw of such a burst edworld GPU-copies the VB0 entry and cb1 rows 270..273 (each
  hologram has its camera), and the whole t33 pool, at most every 50 ms, and reads them back without waiting. Each
  hologram is drawn to a 512x512 surface of its own, which the HUD's pass lays over the screen with a 6-index draw
  whose pixel shader reads it at t4: the ship's own first, the target's second (four whole frames of both CMDRs,
  2026-10-21/22). The target is a draw to the surface laid over second in the frame before, of those the smallest
  scale (a station's parts), the most indices among equal ones (`holo_target_part`); with one laying over there is no
  target. The smallest scale alone took a Panther's own hologram (0.0024) over a construction site's. The log says
  `hologram: the target's surface ...` and `no target's surface` as it changes.
  `src/hologram_math.h` turns its `q` and camera rows into the model's X, Y and Z in the camera's frame (right, up,
  into the picture), which the right sphere draws as the ship's frame. Checked on dumps (2026-10-21): a yaw of about
  90 degrees left turned the target's axes 85-86 degrees about the camera's vertical, 90 right 88 degrees the other
  way; the ship's own hologram stays in one pose. `hologram:` log lines every 2 s give the target draw's indices (which
  model), its scale, `q`, the axes and the camera rows.
  - The models' registry (PoC, 2026-10-22, `src/holo_models.h`): within one build of the game a model keeps its index
    count, so the count names the model; the registry lists the known ones, each a ship or a station, measured (from
    flights and logs) or confirmed (Artur saw a picture of the hologram and named it), with the game's build its count
    was read on (`holo_build`: the exe's size and last write, logged once as `hologram: the game's build ...`). Each count
    is resolved by the count at its first draw of a session (`hologram: <n> indices on the own/target's surface: ...`)
    and sampled once: the draw's indices and the vertices from its base vertex (VB1, stride 40, the position packed in
    its first 12 bytes as the panels' - `decode_local`; at most 8 MB) are copied and read back a few frames later, one
    count at a time, at most one a second: a single hitch per model, no cost per frame. The sample's fingerprint does not
    depend on the order of vertices or indices: the vertex and triangle counts, the bounding box's half-sides over the
    largest, the vertex furthest along each end of each axis, and the box's occupancy in 8x8x8 cells; two compare by a
    likeness 0..1 (`holo_likeness`). A count the registry does not know on this build is matched against the models'
    fingerprints in each of the 24 quarter-turn turns: over 0.9 and 0.05 clear of every other model it is `REMAPPED`,
    the registered count and axes (turned back, `holo_model_axes`) go to the pad's plane and the T1 table; a model that
    fits several turns and not the identity is `AMBIGUOUS` and keeps its drawn axes; the same count and shape on a new
    build is `the same as on another build`. Every entry names its build (`holo_build_2026_10_13`: version
    2026.10.13.334473, with the fingerprints read on it for the models seen since samples were taken); on another build a
    count is its model's only once the fingerprint says so (`holo_table_count`): until then, and for good for a model
    without a fingerprint, the count gives no T1 entry, no pads' planes and no ship (`hologram: <n> indices ...: <model>'s
    count on build ..., not this one`), so a count an update gave to another model is never read as the old one. The
    version VersionInfo.txt beside the exe names is logged too (`hologram: VersionInfo.txt beside the exe names version
    ...`), for people only: a copied game may keep an older file. `hologram model sample: ...` logs the outcome,
    `hologram model fingerprint <n>: {...}` the fingerprint as an entry's initialiser in `holo_models.h`. The target's draw
    is chosen by what it shows before where the HUD lays it (`holo_target_part`, 2026-10-25): a place's model (its kind of
    place not none) on the target's surface, else a place's on the own surface (the one laid over first: the order
    swaps after a ship is swapped at a port, and a carrier is drawn there at its pad), else any draw on the target's
    surface; each the smallest scale. Two ships keep the target's surface (a ship may be the target); a surface not laid
    over counts for nothing. A target taken from the own surface is logged at the change (`hologram: the target's model
    by its kind: ...`). Checked on a construction site's sample from a dump (13083 indices, 4906
    vertices): turned, it is found again (likeness 0.988); the test reads such a sample when `EDWORLD_HOLO_SAMPLE` names
    it (`<ib_u16.bin>;<vb_s40.bin>;<count>`) - game data is not kept in the repository.
  - edworld_eht writes each sample to `edworld_models\` next to its log: `<build>_<stamp>_<count>_ib_u16.bin`,
    `<build>_<stamp>_<count>_vb_s40.bin` (the vertices up to the highest index used) and
    `<build>_<stamp>_<count>_holo.pgm` - the depth of the surface the model was drawn to, as grey (near light, far darker,
    nothing drawn black), copied as the HUD lays it over (none when it is not laid over within 3 s), for Artur to confirm
    the model on the picture before an entry becomes confirmed. The log names the surface of a count's first draw as the
    own, the target's, or other (not laid over: the surfaces change hands as a target is chosen). Beside each sample the
    dumps' writer thread writes `<build>_<stamp>_<count>_context.json` (`src/sample_context.h`): Status.json as it was
    (the target chosen in `Destination`, `BodyName`, the flags), and the newest journal's name, its size then - the
    place in it the sample came at - and its last complete line; a file read cut is null. The files are read on that
    thread, not the render thread.
  - The models' descriptions (offline, `tools/holo_db.py`, PoC 2026-10-23): `scan <edworld_models dir> <db dir>
    [journal dir]` gathers the samples into `<db dir>/models.json`, one row per model - the samples of the same mesh
    (vertices, triangles, the box's proportions) on any build, so a model an update renumbered stays one row. Each row
    gets the game's picture as PNG, three views of its mesh (top, front, side), and the journal's events around each
    sample (LoadGame, Loadout, BookTaxi, Embark, Disembark, the docking events, Undocked, SupercruiseExit, ApproachSettlement,
    Touchdown, Liftoff) as hints - by the sample's place in the journal when its context names one (the journal's own
    clock), else by the machine's time - and the context's target, body and journal place. With
    `--journal-tool <path>` (EHT's `journal_context`, from the elite_help_tool repository) a sample its context places
    in the journal gets the place before it and a minute after it - docked, supercruise, landed, on foot, a taxi; the
    station, pad, body, ship - and the steps between, instead of the nearest events. Every scan reads all the
    samples' events again and deduces what they say together, each deduction with the samples it stands on (one is
    weak): the id kept or renumbered across builds, the own ship's model after a Loadout (not one at the game's load),
    a taxi's, the station a sample came at between the grant and Docked - one station, or several of one type (the
    type's model) - the pads granted, at Undocked, where the placed samples were and a minute later, the targets chosen; the page shows them under the hints. A row's
    description starts as the registry's name or the hint that tells most, and a description a person wrote is never replaced. A
    model of `holo_models.h` no sample shows yet is a row waiting for a picture. `form <db dir> [seconds]` serves the
    page to describe the models (description, role, confirmed) on 127.0.0.1 only - the roles: ship, carrier,
    port-orbital, port-surface, settlement (planetary settlements, also under construction: one hologram), construction-orbital, operations-carrier, operations, installation (orbital installations and planetary hubs: no pads),
    space-entity (stars, planets, moons, orbits), poi, pad, and station and other from before; a row's role shows its
    meaning on hover - a random port and a key in the
    address, the Host and Origin checked; each save keeps the previous file as `models.json.bak`; it ends with Ctrl+C or
    after the time (3600 s). A row waiting for a picture cannot be confirmed. Beside the role a type (`TYPES`, 2026-10-24, from Raven Colonial's build types: the ports' sub-types, each its own hologram and a surface outpost's its own pads, "Type: subtype unknown" when it cannot be told, and "Outpost: any (the model of every T1)" for the one model the HUD shows at every T1 outpost docked or without a target - 3576 indices on 2026-10-24's build, seen at five stations; the carriers, the orbital construction site, the installations and hubs; a settlement none, one hologram for all), its list by the role; `models-header` names a model by its type, else by the first line of its description. A row not confirmed, with no role or type, gets the role and type of a model confirmed with one of its counts as a suggestion (`suggested_from`, once; the page says so) to confirm or correct; a registry placeholder goes once a confirmed model has its count. One confirmation a station (2026-10-24): `scan` keeps each sample's surface from its log line (target's, own, other); the samples drawn to the target's surface with the same target in Status.json, each within 5 s of the last, are the parts of one station's hologram (`station_visits`: Marth's Inheritance shows 4, the T1 table's 5478 among them), and a part confirmed in the form with a role of a place to dock at gives the others with no role or type yet its role and type (a ship or a body confirmed at the same visit gives none), confirmed with it (`confirmed_with`, said on the page; a correction by the person holds). Under each role of a place to dock at the menu lists its stations by that target's name, each page the station's parts confirmed with that role only (a role is a pigeonhole: after a reload each page shows what has the role now; whatever is not confirmed is on the page to confirm), the name marked with the count of its parts and, at a T1 station, whether the part with the pads' table is among them. Beside the stations each such role lists its types (`type: <Type: sub-type>`), each page the parts confirmed with that role and type: one sub-type is one model, so a part filed under the wrong one shows at a glance (Artur 2026-10-24). Docked the HUD's hologram means nothing (Artur 2026-10-24: stars nearby, the model of every T1, even with the station one is docked at chosen): `scan` makes no row from a sample taken docked (Status.json Flags bit 0; `docked_left_out` keeps it from being read again), a row of the model keeps it, and the docked samples are no station's parts; a row seen only docked says so on the page, unless it is a ship's (the own hologram at the shipyard or in a taxi). A menu on the left (2026-10-24) opens one page at a time - the models to confirm, those waiting for a picture, each role, the pads - each at its own address (`view=`), with the count of its rows; Save sends only the rows of the page shown. The page says what is judged: the game's picture is the whole hologram, its three views only the part with the row's indices (one of the hologram's draws: the Coriolis' 948 is its body without the truss), and a confirmation that this part shows in that hologram; a game's picture all black (the hologram not on screen, as in a taxi) is said so. Under the game's picture the page says where and when it was taken, that it is the whole hologram (the part only on the views), and which other rows have a picture of the same moment (within 5 s as a station's visit, `same_moment`): the parts drawn together share one picture (Artur 2026-10-24: three parts at Brescia Platform within a second looked the same). `scan` also keeps each sample's fingerprint
    from the log (`hologram model fingerprint <n>` right after the sample's line, which names the build), so it stays
    once the log is set aside. `models-header <db dir>` writes the confirmed models into `src/holo_models_generated.h`
    (`holo_models_confirmed`, 2026-10-24): one entry for each build and count a model was seen with, its fingerprint
    when one was read, `ship` for a ship's role and `other` for any other (no order rule: the squadron carrier's
    hologram is drawn to the own surface at its pad); a build and count of a measured entry of `holo_models.h` stays
    that entry's. `holo_models` is the measured entries, then these: a model confirmed after an update is known on
    that build by its count (so the T1 table and the pads learned hold for it again: Coppel City's 4431 indices were
    unknown on the build of 2026-10-23 until then) and on later builds by its fingerprint. The same model on two
    builds is two entries alike, so a fingerprint's rivals leave out the best model's own shape.
  - Carriers and a construction site (`pad_planes` in `src/hologram_math.h`): a carrier's hologram stays the carrier's after a grant. The
    squadron carrier's model (84969 indices: Z its length, Y the deck's up) has its pads on planes turned about Z, read
    off its mesh (2026-10-22): the beam's (0), the wings' (+-24 degrees) and the outer edges' (+-46). Which plane a pad
    is on is not known, so the plane nearest the ship's up is taken, its up towards the ship, without a vote
    (`hologram: pad <p> of a carrier ...: the plane at <deg> degrees` as it changes); the plane taken stays until another
    is 4 degrees nearer (half way between two the choice flickered). All the planes hold the carrier's length, and the
    ship's nose on a pad lies along it towards its plus end (pads 1 and 5), so the pad's mark and the nose are drawn as at
    an outpost of the T1 table. Approached from the stern over a wing the nearest plane is the wing's, not the pad's
    (Artur), so the pads known go by their number (`pad_plane_pads`, PRNH's 32 from Artur: L 1-8 the wing's +24 and 17-24 the
    other wing's -24, M 11-12 and S 15-16 the edge's +46, M 25-26 and S 29-30 the other edge's -46, the rest the beam's
    0; the sides from the ship's up on pads 1 and 18); edworld_eht shows
    `PAD <n> NEW` for another and logs `pads' planes at Docked: pad <n> ..., the last plane <deg> degrees` to learn it.
    The planetary construction site (13083 indices) is flat along Y, most of its faces facing +Y, and the ship's up on
    its pad was +Y: one plane, the ground's, without the nose. The table is `pad_planes` (carriers and the site). Players' fleet carriers (one plane of
    pads) and the colonisation carrier (mirrored pads on two sides) come once their models are sampled.
  - The pads learned (2026-10-23): `holo_db.py scan` also reads edworld_eht's logs beside `<edworld_models dir>` and the
    journal. At each Docked after a grant the log told (`state: pad <n> given`, so a docking of the other commander,
    whose grant is in another log, counts nowhere), the first model of `pad_planes` the hologram shows in the minute
    after Docked, until another shows (a target chosen next), gives the ship's up and nose in the model's axes - docked,
    the hologram holds the ship's frame (PRNH pads 5, 18, 21: the wing's 22.5 degrees each side). One row per model,
    station and pad, its dockings put together: the plane (the model's nearest, within 6 degrees), the nose in the pad's
    plane, the spread, and notes (one docking, off every plane, all pads of a station alike). No one confirms a pad
    (2026-10-24): it stands on the model the hologram showed, so a row goes to edworld_eht once that model is
    confirmed in the form with a role of a place to dock at (`DOCK_ROLES`: carrier, ports, settlements, construction sites,
    Operations, station) and is left out when the model is confirmed as anything else (a ship, a planet); a docking off
    the model's planes is left out on its own (Eames Genetics Lab pad 1: one docking at 0 degrees, one at 64), and of
    noses more than 30 degrees apart the most dockings' way is taken, none while two ways tie. The form's pads table
    only shows each row's state and why. `holo_db.py header <db dir>` writes them into `src/holo_pads_generated.h`
    (`pad_learned`), each for its station, and one for any station of a kind of place where two stations or more of
    that kind have the pad on the same plane with noses within 6 degrees and none of the kind has it otherwise (the
    form says, at each pad, whether its kind's stations agree). One hologram (13083) shows the
    planetary construction sites, of two layouts of pads (5: S2 M2 L1, 7: S2 M2 L3), and the on-foot settlements
    (S1 M0 L1, S2 M0 L1, S1 M1 L1), their pads numbered apart: a pad alike at two sites of one layout goes for a site
    of the other too, until one of those reads otherwise. The kind comes from Status.json's destination name
    alone (`site_kind_of`): a construction site's name holds ": " ("Planetary Construction Site: <name>"; the colon
    in other languages of the game not checked), a name the game keeps as a key (`$USS_...;`, a signal or a beacon,
    the only names with a `Name_Localised` beside them) is no place, any other a station or settlement. Each docking
    keeps the station's pads (DockingRequested) and the ship with the pad it needs, to find a rule of the layout from
    the pads granted. edworld_eht looks the rows up by the hologram's model, Status.json's destination (the journal's
    StationName, or a carrier's `<name> | <callsign>`) and the pad: a station's own row, then its kind's, then one for
    any station; the plane before the pad's number (`pad_plane_pads`), the nose drawn where the model's planes alone
    would not. The log says the destination's kind at each change.
    Then `holo_db.py models-header` and `header` write the two headers, and a build takes them in. A model's name in
    the header is its type from the form, else the registry's name, else its role's (`ROLE_NAMES`); the description
    stays in `models.json`, the header holds printable ASCII only.
  - A ship is never the pads' model: near a carrier's pad the target's hologram is the own ship (its count drawn to the
    own surface in the same burst), in a fixed pose that leans with the stick and comes back - not the pad's frame (PRNH,
    pads 5 and 15, 2026-10-22); with it, or with a model the registry names a ship, there is no pad's plane
    (`hologram: the target's hologram shows a ship`).
  - The T1 table's watch (edworld_eht): at Docked, the last look on the table's pad within 3 s before it (the hologram
    turns to the world's frame once docked) is logged as `hologram: T1 check at Docked: pad <p> of model <n> indices:
    plane <deg> degrees to the ship's, nose <deg> degrees off the table's - agrees` or `DEVIATES from the table` (over 15
    degrees of plane or 30 of nose); the table is never changed by it. A look on a T1 pad but none within those 3 s is
    logged as `no look`. Under the right sphere's lines the same shows for a minute, text only: `T1 MATCH <plane> / <nose>`
    in the ok colour, `T1 DEVIATES <plane> / <nose>` in the wrong colour, `T1 NO LOOK` grey (Artur 2026-10-22: seen at
    once, reported while still at the place); and `NEW MODEL <count>` grey for ten seconds when a sampled count is not a
    registered model (unknown, remapped or ambiguous), its picture waiting in `edworld_models\`.
  - The pad's plane (2026-10-21): after the tower gives a pad at an outpost, a port on a body or a settlement, the
    hologram's model is the pads (Brewer Site, a crater outpost: 4596 indices; Brescia Platform, an outpost: 1953).
    Its X stands square to the pads, Y and Z lie in their plane: on the pad at Brewer X was the ship's up, over the pad
    at Brescia its down, so the sign follows the kind of port. The ship is always over the pads, so the pads' up is the end
    of X towards the ship, against the direction to the target. It is voted (`pad_up_vote`, Artur 2026-10-21: as three
    computers in a spacecraft): a vote per look at the target ahead (a filled dot), 12 degrees or more off the pads'
    plane and 3 degrees or more from the last vote's direction, weighted by the sine of the ship's height over the
    plane; three agreeing votes decide (two to two: a fifth), and later three strong votes in a row against (30 degrees
    or more) turn it. A first look from a climb away with the target behind (Brewer, 09:37) gave the wrong end before
    the vote. A model shown for less than a second (another hologram drawn smaller for a moment) leaves the vote as it
    is; a new grant or model starts it afresh. Decided and logged, it makes the tilt 0..180 (upside down = 180,
    red); over 90 degrees (`pad_upside_down`) the disc is a red wire grid and `UPSIDE DOWN` stands over the sphere
    (Artur, 2026-10-21: a red fill alone did not tell a tilt from a ship upside down), logged on the way in and out; until then the tilt ignores X's sign, 0..90 (`pad_tilt_deg`). A Coriolis
    keeps the station's model after the grant (948 indices: the station with arrows to its mailslot, which is on Z, the
    way in); the pads inside are not on its hologram. edworld_eht learns of the grant from the data source's `surface`
    record (the pad, the time of DockingGranted, the StationType; a first-layout record without them is read as no
    grant); edworld alone takes the pad from the HUD's notice. The disc shows while the grant holds (ten minutes at
    most) where the kind of place of the hologram's model (`holo_place_e` in `src/holo_models.h`, from the model's role
    and type in holo_db's form) has pads on its hologram: an orbital outpost (T1) only with the pad's row in the T1
    table, a place on a planet, a carrier only with its row of planes (`pad_planes`; a carrier without one - X and the vote drew a fleet carrier's
    deck 90 degrees off - shows none). A players' fleet carrier (73389 indices, 2026-10-24, the ship's up measured on pads
    2, 7, 13, 11 of Maria Sklodowska): the deck with the L pads 1-8 leaning 10.1 degrees along its length (a plane's
    `lean_deg`, its up towards the turning axis after the turn), the right side +22.5 degrees (M 11-12, S 14-15), the left
    -22.5 (M 9-10, S 13, 16), by the pad's number; the nose along its length, leaning with the deck; not a big station (Coriolis, Orbis, Ocellus, asteroid base), an orbital
    construction site, nor a model the registry does not know. The journal's StationType no longer decides it:
    edworld_eht logs once a grant and model `place: pad N, model M indices is <kind>; the data source's <StationType> -
    agrees` or `DEVIATES`, edworld alone `place: pad N, model M indices is <kind>`. The disc shows also on the pad (Status.json docked or landed) while the hologram keeps the model it had; one log line each
    time the tilt crosses `pad_tilt_green_deg` or `pad_tilt_red_deg`. In the approach view the same disc (`draw_pad_plane`, one
    `pad_plane_now` a frame) stands in the dome through the target, over the base, which turns light blue
    (`compass_colour_pad_base`) while the disc shows: the dome's frame is the ship's, as the hologram's axes.
  - Places on a planet (E, 2026-10-24: a model of the kind `planet` - surface ports, settlements, planetary
    construction sites): always flat. The pads' up is the model's thinnest axis, the least half-side of its bounding box
    in its fingerprint (`holo_flat_of`, in the registry's axes, turned back when remapped): 13083 Y (1.00 x 0.33 x 0.97),
    4596 X (0.01 x 0.98 x 1.00), 2208 X (0.01 x 1.00 x 0.34); its end is the vote's, the ship always over the pads. No
    table in the code: a new model of a surface port has its plane once confirmed in holo_db's form and built in. A pad
    confirmed in the form gives its plane's angle from that axis (turning about the next, leaning to the third - as
    13083's row of `pad_planes`, which holo_db still reads) and the ship's nose, the ship standing on its pad; any other
    pad the plane alone, no direction. A model without a fingerprint, its turn not sure, or with the thinnest half-side
    over half the next (`holo_flat_ratio_max`, no slab) shows none; logged once a grant and model (`pad N of model M
    indices on a planet: up the model's thinnest axis A (ratio r)`).
  - Outposts in space (T1): the ship may come from under the station, where the vote takes the wrong end. `t1_pads`
    in `src/hologram_math.h` gives, for each hologram model (its index count: Military 5238, Commercial 1953,
    Scientific 4404, Industrial 4431, Pirate 7023, Civilian 5478) and pad, the model's axis and end of the pads' up and
    of the ship's nose, measured on a pad or told relative to a docked ship (2026-10-21). With an entry `pad_plane_now`
    takes the up from it at once (one log line) and votes nothing; the pad's number still comes from the data source.
    The table's nose gives the pad's mark `< |||` (`pad_mark`, segments in the pad's plane, the head to the nose) drawn
    on the disc in both views, at 80% of the disc (`draw_pad_plane`); in the dome the target's small pad is drawn again
    on top, outlined dark. The left sphere carries the same mark (`draw_left_mark`, the right sphere's plane of the last
    frame) half the disc's size around its centre, in its own projection, half opaque. In the approach view the left sphere shows the target twice: a ring seen from above
    (`compass_colour_top`) and the white dot where the view ahead has it.
  - The pad's text (PoC, `pad_text_samples`): the tower's box ("PROCEED TO LANDING PAD n - m:ss", two lines, in the
    game's language) stands on the 2200x1800 HUD surface at x 1170..1579, y 819..899 (`pad_box_*` in
    `src/panel_math.h`; the same place in three frame dumps of two commanders, none before a grant). `panel_pad_text`
    copies that area about twice a second at a watched draw sampling the surface, reads the copy a frame or more later,
    and takes the box as shown when 60% of the area is its fill (alpha 64..191, dark). The box is the HUD's notice box,
    not the pad's alone: at Coppel City it carried LEAVE STATION - m:ss, GRAVITY WELL, SUPERCRUISE OVERCHARGE ACTIVE and
    DOCKING REQUEST SENT., which turned into the pad's text without the box going. So each time it shows, and each
    time its text changes (`pad_box_changed`: 2% of the pixels differ in alpha by over 48, a second at least after the
    last sample), and then once a second for ten seconds (`pad_burst_ms`: the countdown's last digit runs through all
    ten, in the pad number's font), the area goes to `edworld_padtext` as a raw sample with the data source's pad in the name
    (edworld_eht; 0 without it), to learn the digits from.
  - The pad's text read (`pad_text_read`, 2026-10-24, `src/pad_text_read.h`): from the same copy, about twice a second,
    without words - the text's pixels (the brightest of r, g, b at least 170), lines, glyphs and words (glyphs closer
    than half a line's height); the tower's notice is two lines, the first ending in a lone dash after a word of one or
    two glyphs (the number), the second four glyphs with a colon (narrow, an empty row inside) second (m:ss). Each digit is
    seen from afar (2026-10-25): its box in the game's own pixels cut into 4 x 6 zones (`pad_text_zones`), each zone's
    share of lit pixels in per mille, the pixels a zone's edge cuts counted in part, in integers; matched by the sum of
    differences against `pad_digits` (`src/pad_text_digits_generated.h`, made by `tools/pad_text_digits.py` from samples
    whose data source named the pad: each digit's mean); sure when the next template is 20 % further. The box of a digit
    is 12, 13 or 14 px wide by where the number stands; scaled to a grid, as before, that moved whole columns and the
    lone 8 and the 3 of 30 were never read sure. `tools/pad_text_digits.py --check` reads the samples all on all and
    each pad's by the other pads' templates (a pad not seen yet): 964 of 964 and 939 of 939 right, the least margin 37 %
    (pad 7 alone has a 7). A notice still unsure 3 s after the box showed, with no pad taken from it, is logged once a box
    (`pad text: a notice of the tower's, its number unsure ...`) and, with `pad_text_unsure_sample`, its picture kept in
    `edworld_padtext` for the templates. A pad read sure twice in a
    row is taken (`pad_text_vote_t`), its grant's time from the countdown (ten minutes); another pad, or the same given anew (its countdown 20 s or more
    up, `pad_text_new_grant`: a misread digit of the countdown is not), is a new grant (`pad text: pad n read from the notice, m:ss left`). edworld alone takes it into `flight()` as the pad
    given (`pad_text_grant`) until the ship leaves the pad it docked at or enters supercruise; edworld_eht keeps the data
    source's and logs once a grant `pad text: read pad n, the data source's m - agrees` (or `DEVIATES`, or none within
    15 s). `test_app` draws notices with the game's own glyphs of each digit (the lone 8 and the 8 of 18, the 3 of 13 and
    30, the 0 of 30) and reads an `edworld_padtext` directory when `EDWORLD_PADTEXT_DIR` names it
    (game data is not kept in the repository; the digits' templates are). Only the English text's structure is known.
  - After a game update: with `station_vs = 0`, a model shader (its first input `INSTANCEANDMODELDATAINDEX`) drawing
    1000 indices or more to a 512x512 target is taken for it, from half a minute after the start, a few looks a tenth of
    a second, and named in the log. Only then: the game makes the hologram's shader when it first shows a station, and
    a search before that took another model's (2026-10-20). To find it by hand: a whole-frame dump with
    a station as the target, a few km out; in `<stamp>_draws.txt` the draws to `rt 512x512` (the hologram's surface,
    format 39 on 2026-10-20), one surface per hologram (the target id after `at`, the composite's `ps t4 texture`); `dump_vs` = its hash and the quad that reads that surface
    as t4 shows the model in the surface's copy. Dead ends on 2026-10-20: the sprites of a station's smoke, the UI's
    text shaders, the radar's icon under the hologram; a diff of hashes between a station and a star as the target
    finds nothing, as a star has a hologram too.
- (edworld_eht only, `shader_catalog`) The catalog of the game's shaders (PoC, 2026-10-23): the device's creations of
  every stage (vertex, pixel, compute, geometry, hull, domain) and of input layouts are hooked too; each shader the game
  creates (none of edworld's own) is noted once a session - its bytecode copied, nothing else done on the game's
  thread - and a thread of the catalog writes the batch once no shader was created for 5 s (loading, the game's build
  of its shaders at start) to `edworld_shaders\<build>\` beside the log, the build being the hologram models' key
  (the exe's size and last write). A shader already there from an earlier session is skipped, so the folder holds
  each build's shaders once: `<stage>_<hash>.dxbc` (an input layout: `il_<hash>.txt`, its elements) and a line of
  `catalog.tsv`: hash, stage, size, shader model, the input and output signatures' semantics, the program's
  declarations (`cb1[276] t33 u0 s0 tg8x8x1 temps8`), its instructions and its chunks. The game's bytecode has no
  names of its own (no RDEF or STAT chunk), so this shape (`src/shader_dxbc.h`) is what finds a shader again when an
  update changes its bytes; the semantics carry the engine's words (`INSTANCEANDMODELDATAINDEX`,
  `__USER_GLASSCOLOURTINTRANGE`). Logged: `shaders: the catalog ...` at attach, `shaders: <n> new to ...` per batch with
  the session's counts by stage. Offline, `tools/shader_catalog.py names` finds the catalog's compute shaders whole in
  the game's `PlanetShaders\*.csa` archives (the planets' generator) and names them by archive and block, and
  `tools/shader_catalog.py diff` tells what an update changed: the same bytes, the same shape under other bytes, new,
  gone - the watched shaders (panels, holograms, the HUD's composition) first.
- Logs to `<plugin>.log` in the log folder: chain, attach, watched shaders, the patch's first draw and the
  destination's allegiance and factions (how many, from which source), and once a second (`log_interval_ms`) a summary: in edworld_eht the frame's panel draws
  with surface size and anchor in NDC, in edworld the draw and fault totals. A fault in edworld's own work is caught; eight of them switch the observer off, never the game call.
  One file per game session: at start, the last session's `<plugin>.log` is renamed `<plugin>.<UTC>.log` (the time it
  was last written); EHT's backup compresses those into its own directory and removes them from the game's folder.

## Files

| Path | What |
|---|---|
| `src/proxy.cc` | exports, chaining, wrapped `D3D11CreateDevice(AndSwapChain)` |
| `src/hooks.cc` | the observer: hooks, staging ring, readback, publishing (edworld_eht), log summary |
| `src/panel_patch.cc` | the jump panel patch and the factions list (ImGui draw list, state saved and restored) |
| `src/game_state.cc` | `Status.json`, EHT's `target` (edworld_eht), EDSM |
| `src/faction_list.h` | the factions list from either source: `target` or EDSM's answer (a small JSON reader) |
| `src/runtime.h` | settings, the build's name (`EDWORLD_EHT`) |
| `src/settings_log.cc` | `<plugin>.ini`, `<plugin>.log` |
| `src/edworld_share.h` | the published layout (plain C++, shared with the Linux side) |
| `src/panel_math.h` | FNV-1a, anchor and projection arithmetic |
| `src/shader_dxbc.h`, `src/shader_catalog.cc` | the shaders' catalog (edworld_eht): a DXBC container's shape; noting at creation, writing per build |
| `src/hologram_math.h`, `src/hologram.h` | the target's hologram (PoC): its orientation's decode, the camera's axes, the model's axes in its frame |
| `src/gate_math.h` | gates down to a place (PoC): angles from the planet's visible edge, the path down (setup onto a flat line, the dive point, the dive), the gates along it, the ship's attitude from the compass and the heading, the projection |
| `src/emblems.h`, `tools/gen_emblems.py`, `assets/*.svg` | the emblems as coverage masks, generated from EDAssets |
| `src/imgui_config.h`, `shaders/imgui.hlsl`, `third_party/imgui/` | Dear ImGui build options, backend shaders, the vendored sources |
| `third_party/fonts/`, `tools/gen_font.py` | the list's font (Noto Sans Mono, SIL OFL 1.1), embedded at build time as `build/gen/list_font.h` |
| `tools/build.sh` | build through msvc-wine; exports from `tools/d3d11_exports.txt` (environment: `MSVC_WINE_ENV`, `FXC`) |
| `tools/d3d11_exports.txt`, `tools/gen_exports_from_list.py` | the Windows d3d11 export names, and the run of EDVR's generator on them |
| `tools/test.sh <scratch>` | smoke test under wine without the game, both builds (each as `d3d11.dll`), plain and chained, patch and list pixels checked, EDSM's answer read from a saved one (`test/data/`), the shaders' catalog written (edworld_eht), game shaders' shapes read when `EDWORLD_SHADER_SAMPLE` names `.dxbc` files (environment: `MSVC_WINE_ENV`, `FXC`, `EDWORLD_WINEPREFIX`) |
| `tools/edworld_watch.py [path]` | live view of the shared file |
| `tools/dump_to_png.py <dir>` | surface dumps to PNG |
| `tools/shader_catalog.py` | the shaders' catalog offline: `names` (the planets' generator's archives), `diff` (two builds) |
| `tools/holo_db.py` | the hologram models' descriptions offline: `scan` (samples, pictures, the journal's hints, the docked pads from the logs), `form` (the page on 127.0.0.1), `models-header` (the confirmed models into `src/holo_models_generated.h`), `header` (their pads into `src/holo_pads_generated.h`), `--self-test` |
| `tools/pad_text_digits.py` | the digits' templates of the pad's text read (`src/pad_text_digits_generated.h`) from `edworld_padtext` samples whose data source named the pad |
| `tools/gen_exports.py`, `tools/EDVR-LICENSE.txt` | EDVR's export thunk generator (MIT) |
| `NOTICE` | third-party work, file by file, with the licences |

## Settings (`<plugin>.ini`)

Loaded through edloader, `<plugin>.ini` is in edloader's `config\` and `<plugin>.log` in its `logs\` (the
folders edloader hands its plugins in `EDLOADER_CONFIG_DIR` and `EDLOADER_LOG_DIR`). Installed alone as the
game's `d3d11.dll`, both are beside the dll.

Comments are whole lines starting with `;` or `#`; a comment after a value would become part of the value.

```ini
; the next d3d11 in the chain; empty = system copy
next = d3d11_edhm.dll
watch_vs = 81216C77F90DEDD6, 1989E6D3B405FDE0, 925ACEDA0153AA5B
; edworld_eht only: panels published, target read here; empty = neither
shm_dir = Z:\dev\shm\eht
; (edworld_eht) 0 = no catalog of the game's shaders in edworld_shaders
shader_catalog = 1
; 0 = no per-frame summary
log_interval_ms = 1000
frame_gap_us = 2500
; 1 = log every vertex shader hash the game creates
log_all_vs = 0
; shaders whose draws a whole-frame dump looks into (bytecode, index and vertex buffers, t33 pool); the target's hologram
dump_vs = 5B0068AF5630F96B, 5559BD94B6852E83
; 0 = observe only; 1 = the jump panel patch; 2 = a test frame where the patch goes, whatever the destination
patch = 1
; the interface surface that carries the jump panel
patch_surface = 3072x660
; the patch's centre and size on that surface, in its pixels
patch_x = 1535
patch_y = 280
patch_width = 260
patch_height = 120
patch_emblem_height = 92
; the panel's own black, hex RRGGBB without '#'
patch_ground = 020304
; 1 Federation, 2 Empire, 3 Alliance, 4 Independent = that emblem whatever the destination
patch_force = 0
; the independents' emblem's colour
independent_colour = F2C14E
; 0 = never ask EDSM
edsm = 1
; the factions list under the panel: 0 = off
list = 1
; its box in the panel surface's pixels, past the panel's edge: horizontal centre, top edge, widest; faction rows at most (EDSM's line comes on top)
list_x = 1540
list_top = 500
list_width = 2200
list_rows = 7
; text height in pixels; read once, at the first patch
list_text = 50
; the list's colours times this in the cockpit
list_gain = 0.8
```

The values above are the defaults. `shm_dir` is where EHT writes `target` (`edworld.dir` in its settings,
default `/dev/shm/eht`).

## Game runs

1. **2026-10-04, ed-lab clone, observer only (GUI effects off).** Chain with EDHM worked; `/dev/shm` written
   by edworld is visible on the host. 11 panel draws a frame on surfaces 2048x1280, 2200x1800, 1024x1534,
   3072x660; a hyperspace charge adds no panel draw.
2. **2026-10-04, ed-lab clone, observer with instance records and surface dumps.** Dumps taken while charging:
   the jump panel is on the 3072x660 surface (top middle, with INFO). One rest anchor fits every ship flown.
3. **2026-10-04, a Steam install, jump panel patch (hand-written shaders), in front of EDHM.** At `patch_y = 280`
   the Federation emblem sits on the panel in place of the wrong one; allegiance from EHT's `target`; no faults
   over the session.
4. **2026-10-05, the same Steam install, jump panel patch drawn by Dear ImGui.** The Federation
   emblem drawn in the same place as by the hand-written shaders; allegiance from EHT's `target`; no faults.
5. **2026-10-05, the same Steam install, factions list on the surface under the panel.** Not seen under the panel;
   a black box over the panel instead. Off (`list = 0`): the box gone.
6. **2026-10-05, ed-lab clone, geometry dumps.** The jump panel shows x 1031-2045, y 2-493 of 3072x660; the black
   box is a one-pixel strip (row 519) stretched over the panel. Hence the list's quad of its own.
7. **2026-10-05, ed-lab clone, the list's quad (test mode).** The test rows under the panel, edge to edge with it,
   following it; the jump panel's draw found by its vertices; no faults.
8. **2026-10-05, a Steam install, the list's quad and the superpower read from the panel.** The Alliance read from
   the panel's text (label 231 px, word 155 px), the emblem beside its rows; the list under the panel and riding it,
   but its text too small and blurred (20 px, no mips), the box twice too wide, its ground grey instead of black.
9. **2026-10-05, the same, the list at 50 px, mipmapped, centred, on black.** The list from the tool's record, centred
   and as wide as its rows, on black; slightly soft. The target is R11G11B10_FLOAT and the game blends the panel
   ONE / INV_SRC_ALPHA, RGB only, as the list does.
10. **2026-10-05, the same, mip LOD bias -0.5 and list_gain 0.8.** Sharper, less glow; accepted as good at the
    game's supersampling of 0.77 (the list shown at about 0.7 of its texture).

## Not known yet

- Cost in the game (expected: a pointer compare per draw plus a few copies per frame; the patch only while charging).
- Loading through edloader, in the game.
- The patch masked by the game's own drawing (hidden panel while aligning), in the game; tested under wine.
- The list's quad under wine: only the pure arithmetic (the panel's vertex decode and map, from the game's own
  vertices) is tested there; the test's draw has no stride-40 vertices, so the quad is drawn only in the game.
- The superpower read from the panel in the game: EMPIRE, FEDERATION and INDEPENDENT are known from screenshots
  (their width against the label's, within 3%), not yet read from the surface; under wine it is tested on rows drawn
  as the game draws them (ALLIANCE's widths from the surface), and on the game's Alliance panel natively.
- edworld_eht reading a `target` of the first layout (an EHT without the factions): written for, not tested.

## Writing a d3d11 plugin of your own

edworld is one example of a d3d11 proxy plugin for Elite Dangerous; edloader runs several of them in one game.
What a plugin of this kind needs, as edworld does it:

1. **Be a `d3d11.dll`.** Export every d3d11 function. edworld's exports are thunks generated at build time from
   the Windows d3d11 export names in `tools/d3d11_exports.txt` (`tools/gen_exports_from_list.py` runs EDVR's
   `tools/gen_exports.py` on them); each thunk jumps to the next d3d11's function.
2. **Find the next d3d11 without the loader lock.** In `DllMain` load nothing but the system's `d3d11.dll`; load
   the next proxy (`next` in the ini) on the first export call. Ask for the next one by the name your settings
   give: `d3d11.dll` reaches edloader, which passes the call on down its list (doc/developers.md in edloader).
   Guard against a next proxy that asks for `d3d11.dll` by name and lands back in you (a thread-local depth).
3. **Keep your files out of the game's folder.** Under edloader, read your settings from `EDLOADER_CONFIG_DIR` and
   write your log to `EDLOADER_LOG_DIR`; without them, beside your dll. A verification of the game's files empties
   the game's folder of everything that is not the game's.
4. **Hook the device, then the context, and pass everything through.** Wrap `D3D11CreateDevice(AndSwapChain)`,
   then patch the vtable slots you need (`src/hooks.cc`: `CreateVertexShader`, `VSSetShader`, the four draws). Every
   hook calls the original unchanged; your own work runs beside it.
5. **Cost nothing per draw that is not yours.** Recognise your draws by a pointer compare (the bound vertex shader
   against a watched set, kept by `VSSetShader`), never by reading buffers on every draw.
6. **Never read GPU data on the CPU in the draw.** Copy into staging resources and map them frames later with
   `D3D11_MAP_FLAG_DO_NOT_WAIT` (`src/hooks.cc` readback, `src/panel_patch.cc` text and vertex reads).
7. **Put the game's state back.** Before drawing anything of your own, read back everything you will bind (shaders,
   input layout, topology, constant buffers in your slots, shader resources, samplers, blend, depth, rasteriser,
   targets) and set it again afterwards (`backup_t`, `list_backup_t` in `src/panel_patch.cc`). Create your own
   shaders with a flag set so your observer never takes them for the game's (`creating_own` in `src/runtime.h`).
8. **Never take the game down.** Wrap your own work in SEH (`guarded_*` in `src/hooks.cc`); count faults and switch
   yourself off after a few, never the game's call. Log once per change of state, not per frame.
9. **Find things in the game by evidence.** `edworld_dump` and `tools/geometry_dump.py` show what each panel draw
   reads and which rectangle of its surface it shows; that is how the jump panel's place and the list's quad were
   found (Game runs, below).

