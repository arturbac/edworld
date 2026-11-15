# ED CPU profile 2026-11-15 18:25:49

- account: Sjona (Steam), pid 379345
- window: 30.2 s, 168188 samples (perf, 1999 Hz per thread)
- fps 44.3 (EHT overlay frame count) = 22.6 ms per frame; Render thread busy 53% = 12.1 ms of CPU per frame

## CPU by module

ms of CPU per second of wall time (1000 = one full core); in brackets the share of that column. Worker\* = all Worker threads together, all threads = every thread of the game.

| module | Render | dxvk-cs | Worker* | RunLoop | all threads | Render ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| ED (EliteDangerous64.exe) | 395.2 (74%) |  | 1024.6 (88%) | 394.1 (93%) | 1816.7 (77%) | 8.93 |
| kernel (system calls) | 13.1 (2%) | 5.5 (4%) | 83.2 (7%) | 17.6 (4%) | 179.4 (8%) | 0.30 |
| DXVK | 45.6 (9%) | 64.5 (46%) |  |  | 122.1 (5%) | 1.03 |
| wine / Proton | 17.4 (3%) | 17.0 (12%) | 51.5 (4%) | 12.2 (3%) | 111.9 (5%) | 0.39 |
| GPU driver | 2.6 (0%) | 51.2 (36%) |  |  | 58.3 (2%) | 0.06 |
| EDHM (d3d11_edhm_test1.dll) | 48.1 (9%) |  |  |  | 48.1 (2%) | 1.09 |
| system libs | 1.1 (0%) | 2.3 (2%) | 4.7 (0%) | 1.2 (0%) | 14.3 (1%) | 0.03 |
| edworld (edworld_eht.dll) | 10.8 (2%) |  |  |  | 10.8 (0%) | 0.24 |
| vdso (clock reads) | 0.6 (0%) | 0.1 (0%) | 4.1 (0%) | 0.5 (0%) | 5.5 (0%) | 0.01 |
| other | 0.1 (0%) |  |  | 0.1 (0%) | 3.0 (0%) | 0.00 |
| **sum** | **535** | **141** | **1168** | **426** | **2370** | **12.08** |

## Threads by CPU load

| thread | % of one core | threads |
|---|---:|---:|
| Worker* | 116.8 | 6 |
| Render | 53.5 | 10 |
| RunLoop | 42.6 | 7 |
| dxvk-cs | 14.1 | 1 |
| The:\ | 5.6 | 1 |
| <NULL> | 2.7 | 27 |
| dxvk-submit | 2.4 | 4 |
| dxvk-queue | 2.1 | 1 |
| dxvk-descriptor | 1.3 | 1 |
| audio_client_ma | 0.6 | 1 |
| S:\steamapps\co | 0.4 | 1 |
| FAudio_AudioCli | 0.3 | 1 |

Notes: self time only, no call stacks. Samples taken right after a hook (EDHM, EDVR, edworld, edloader) forwards a D3D call are counted to the hook, so hook shares can be somewhat high. Rows under 0.5 ms/s are left out of the table but counted in the sums. With the game at its frame limit, the Render thread waits for the next frame in wine (ntdll.so) and vdso clock reads, so those rows there are mostly waiting, not work.
