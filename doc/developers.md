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
| `src/emblems.h`, `tools/gen_emblems.py`, `assets/*.svg` | the emblems as coverage masks, generated from EDAssets |
| `src/imgui_config.h`, `shaders/imgui.hlsl`, `third_party/imgui/` | Dear ImGui build options, backend shaders, the vendored sources |
| `third_party/fonts/`, `tools/gen_font.py` | the list's font (Noto Sans Mono, SIL OFL 1.1), embedded at build time as `build/gen/list_font.h` |
| `tools/build.sh` | build through msvc-wine; exports read from a released EDVR d3d11.dll (environment: `MSVC_WINE_ENV`, `FXC`, `RELEASE_DLL`) |
| `tools/test.sh <scratch>` | smoke test under wine without the game, both builds (each as `d3d11.dll`), plain and chained, patch and list pixels checked, EDSM's answer read from a saved one (`test/data/`) (environment: `MSVC_WINE_ENV`, `FXC`, `EDWORLD_WINEPREFIX`) |
| `tools/edworld_watch.py [path]` | live view of the shared file |
| `tools/dump_to_png.py <dir>` | surface dumps to PNG |
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
; 0 = no per-frame summary
log_interval_ms = 1000
frame_gap_us = 2500
; 1 = log every vertex shader hash the game creates
log_all_vs = 0
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
   the export list of a released d3d11 proxy (`tools/gen_exports_from_release.py` runs EDVR's
   `tools/gen_exports.py` on it); each thunk jumps to the next d3d11's function.
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

