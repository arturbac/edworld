# edworld

A read-only d3d11.dll proxy for Elite Dangerous that tells other programs where the cockpit panels are on
screen. First use: the EHT overlay patches the wrong superpower logo on the FSD charge panel, and the patch
must follow the panel when the cockpit camera deflects (ship inertia, head look).

Proof of concept. Built and tested without the game only (`tools/test.sh`); never run in the game yet.

## What it does

- Forwards every d3d11 export to the next d3d11 in the chain (`next` in `edworld.ini`: EDHM, a renamed
  EDVR, or the system copy). Chaining discipline from EDVR (MIT): system copy loaded in DllMain, next proxy
  on the first export call, a thread-local depth guard for a next proxy that asks for `d3d11.dll` by name.
- Hooks (in place, vtable slots): `ID3D11Device::CreateVertexShader` (FNV-1a 64 of the bytecode, the hash EDVR
  names shaders by) and `ID3D11DeviceContext::VSSetShader`, `Draw`, `DrawIndexed`, `DrawInstanced`,
  `DrawIndexedInstanced`. Every hook calls the game's call through unchanged.
- Watches the cockpit holo-panel vertex shaders (`81216C77F90DEDD6`, and `1989E6D3B405FDE0` when the game's
  "Disable GUI effects" is on). For each of their draws: GPU-copies VS cb0 rows 0..11 into a staging ring,
  notes the interface surface the panel samples (PS t2, else t1: pointer, size, format) and the draw
  arguments. Frames are split by a gap between panel draws (`frame_gap_us`), so no DXGI hook is needed.
- Reads the ring 1-3 frames later with `Map(DO_NOT_WAIT)` (never a CPU read of a mapped dynamic buffer:
  write-combined under DXVK) and publishes the latest frame to a shared file (`share`, default
  `Z:\dev\shm\edworld` = `/dev/shm/edworld`): layout in `src/edworld_share.h`, seqlock.
- Panel anchor: cb0 rows 4..7 produce SV_Position with one dp4 each; for holo panels their 3x3 is the camera
  matrix and the w column is the camera applied to the panel's translation (EDVR, measured over 7,039 draws),
  so the w column is the panel origin in clip space. Not yet verified that the instance record (t33) adds no
  further offset for this family.
- Logs to `edworld.log` beside the dll: chain, attach, watched shaders, and once a second (`log_interval_ms`)
  the frame's panel draws with surface size and anchor in NDC.

## Files

| Path | What |
|---|---|
| `src/proxy.cc` | exports, chaining, wrapped `D3D11CreateDevice(AndSwapChain)` |
| `src/hooks.cc` | the observer: hooks, staging ring, readback, publishing, log summary |
| `src/settings_log.cc` | `edworld.ini`, `edworld.log` |
| `src/edworld_share.h` | the published layout (plain C++, shared with the Linux side) |
| `src/panel_math.h` | FNV-1a, anchor and projection arithmetic |
| `tools/build.sh` | build through msvc-wine; exports read from a released EDVR d3d11.dll |
| `tools/test.sh <scratch>` | smoke test under wine without the game, plain and chained |
| `tools/edworld_watch.py [path]` | live view of the shared file |
| `tools/lab-install.sh [edhm\|edvr]`, `tools/lab-revert.sh <run dir>` | put into / take out of the ed-lab clone |
| `tools/gen_exports.py`, `tools/EDVR-LICENSE.txt` | EDVR's export thunk generator (MIT) |

## Settings (`edworld.ini` beside the dll)

```ini
next = d3d11_edhm.dll            ; the next d3d11 in the chain; empty = system copy
watch_vs = 81216C77F90DEDD6, 1989E6D3B405FDE0
share = Z:\dev\shm\edworld       ; empty = no publishing
log_interval_ms = 1000           ; 0 = no per-frame summary
frame_gap_us = 2500
log_all_vs = 0                   ; 1 = log every vertex shader hash the game creates
```

## First game run (ed-lab only, with Artur's consent for that run)

1. `tools/build.sh`, then `tools/lab-install.sh edhm` (backs up the clone's current d3d11.dll, EDVR, into
   `/ext/artur/ed-lab/tests/edworld-<stamp>/orig`).
2. Start the clone, fly, charge the FSD a few times (also with some pitch/yaw while charging), note the times.
3. `tools/edworld_watch.py` during the run if `/dev/shm` is shared with the container (not checked), and
   `edworld.log` afterwards: the panel that appears only while charging (Status.json Flags bit 17 / Flags2
   bit 19) names the jump panel by its surface size; its anchor shows how far it moves.
4. `tools/lab-revert.sh /ext/artur/ed-lab/tests/edworld-<stamp>`.

## Not known yet

- Whether the FSD charge panel is drawn by this VS family at all (EDVR names no jump-panel hash).
- Whether t33 adds an offset to the holo-panel position beyond cb0's w column.
- Whether `/dev/shm` inside the Steam runtime container is the host's.
- Cost in the game (expected: a pointer compare per draw plus a few copies per frame).
