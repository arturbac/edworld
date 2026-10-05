# edworld

A d3d11.dll proxy for Elite Dangerous that tells other programs where the cockpit panels are on screen, and
draws the right superpower emblem onto the panel of a hyperspace jump being charged (the game shows a wrong
one for the Federation, the Empire and the Alliance). Drawn onto the panel's own interface surface, the emblem
moves with the panel when the cockpit camera swings (ship inertia, head look).

Proof of concept. Observing is read-only: every hook calls the game's call through unchanged. The jump panel
patch is the only place edworld changes what the game draws.

Checked in the game: the observer (ed-lab clone, chained in front of EDHM) and the jump panel patch, first drawn by
hand-written shaders, now by Dear ImGui (one commander's Steam install, chained in front of EDHM, without edloader).
Not checked in the game yet: edworld loaded through edloader.

## Two builds

One source, two dlls (`tools/build.sh` makes both; the difference is `EDWORLD_EHT` at compile time):

| Build | For | Files beside it |
|---|---|---|
| `build/edworld/edworld.dll` | anyone: works on its own; the allegiance comes from EDSM; publishes nothing, so it copies nothing of the panel draws | `edworld.ini`, `edworld.log` |
| `build/edworld_eht/edworld_eht.dll` | EHT users: also publishes the panels' anchors to `panels` (EHT's overlay follows them) and takes the allegiance from EHT's `target` before EDSM | `edworld_eht.ini`, `edworld_eht.log` |

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
  patch goes with it. In edworld_eht the allegiance comes from `target`
  in `shm_dir`, written by EHT from its database; when EHT does not know the system, and always in edworld, from EDSM
  (`api-system-v1/factions`, only the destination's id64, once per new destination). The patch is a Dear ImGui
  draw list rendered by ImGui's D3D11 backend into the surface (own ImGui context, no input, no files);
  everything the game had bound is read back first and put back after.
- On request, dumps every panel's interface surface: create an empty file `edworld_dump` in the log folder; at
  the next frame each surface is copied and written to `edworld_dumps\<stamp>_<id>_<w>x<h>_f<fmt>_pitch<n>.raw`
  and the trigger file is removed. `tools/dump_to_png.py <dir>` makes PNGs.
- Logs to `<plugin>.log` in the log folder: chain, attach, watched shaders, the patch's first draw and the
  destination's allegiance, and once a second (`log_interval_ms`) a summary: in edworld_eht the frame's panel draws
  with surface size and anchor in NDC, in edworld the draw and fault totals. A fault in edworld's own work is caught; eight of them switch the observer off, never the game call.
  One file per game session: at start, the last session's `<plugin>.log` is renamed `<plugin>.<UTC>.log` (the time it
  was last written); EHT's backup compresses those into its own directory and removes them from the game's folder.

## Files

| Path | What |
|---|---|
| `src/proxy.cc` | exports, chaining, wrapped `D3D11CreateDevice(AndSwapChain)` |
| `src/hooks.cc` | the observer: hooks, staging ring, readback, publishing (edworld_eht), log summary |
| `src/panel_patch.cc` | the jump panel patch (ImGui draw list, state saved and restored) |
| `src/game_state.cc` | `Status.json`, EHT's `target` (edworld_eht), EDSM |
| `src/runtime.h` | settings, the build's name (`EDWORLD_EHT`) |
| `src/settings_log.cc` | `<plugin>.ini`, `<plugin>.log` |
| `src/edworld_share.h` | the published layout (plain C++, shared with the Linux side) |
| `src/panel_math.h` | FNV-1a, anchor and projection arithmetic |
| `src/emblems.h`, `tools/gen_emblems.py`, `assets/*.svg` | the emblems as coverage masks, generated from EDAssets |
| `src/imgui_config.h`, `shaders/imgui.hlsl`, `third_party/imgui/` | Dear ImGui build options, backend shaders, the vendored sources |
| `tools/build.sh` | build through msvc-wine; exports read from a released EDVR d3d11.dll (environment: `MSVC_WINE_ENV`, `FXC`, `RELEASE_DLL`) |
| `tools/test.sh <scratch>` | smoke test under wine without the game, both builds (each as `d3d11.dll`), plain and chained, patch pixels checked (environment: `MSVC_WINE_ENV`, `FXC`, `EDWORLD_WINEPREFIX`) |
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
; 1 = the jump panel patch; 2 = a test frame where the patch goes, whatever the destination
patch = 0
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
; 1 Federation, 2 Empire, 3 Alliance = that emblem whatever the destination
patch_force = 0
; 0 = never ask EDSM
edsm = 1
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
4. **2026-10-05, the same Steam install, jump panel patch drawn by Dear ImGui (the current code).** The Federation
   emblem drawn in the same place as by the hand-written shaders; allegiance from EHT's `target`; no faults.

## Not known yet

- Cost in the game (expected: a pointer compare per draw plus a few copies per frame; the patch only while charging).
- Loading through edloader, in the game.
- The patch masked by the game's own drawing (hidden panel while aligning), in the game; tested under wine.

## Credits

edworld builds on the work of the [EDVR unofficial patch](https://github.com/characterecho-sean/edvr-unofficial-patch)
team (MIT): the proxy chaining discipline, the export thunk generator (`tools/gen_exports.py`, unchanged), the
shader hash and EDVR's decode of the cockpit panels' constant buffers and instance records. It also uses
[Dear ImGui](https://github.com/ocornut/imgui) (MIT) and the emblems of
[EDAssets](https://github.com/Venefilyn/EDAssets) (MIT; artwork of Frontier Developments). What comes from where,
file by file, with the licence texts: `NOTICE`.
