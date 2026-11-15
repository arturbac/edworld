# ED CPU profile 2026-11-15 18:29:44

from EDHM_2DMigoto-v0.1.3-x64.zip

- account: Sjona (Steam), pid 385489
- window: 30.2 s, 176334 samples (perf, 1999 Hz per thread)
- fps 44.2 (EHT overlay frame count) = 22.6 ms per frame; Render thread busy 58% = 13.1 ms of CPU per frame

## CPU by module

ms of CPU per second of wall time (1000 = one full core); in brackets the share of that column. Worker\* = all Worker threads together, all threads = every thread of the game.

| module | Render | dxvk-cs | Worker* | RunLoop | all threads | Render ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| ED (EliteDangerous64.exe) | 412.5 (71%) |  | 1093.2 (87%) | 420.3 (93%) | 1929.2 (76%) | 9.33 |
| kernel (system calls) | 15.9 (3%) | 5.5 (4%) | 90.9 (7%) | 18.6 (4%) | 194.4 (8%) | 0.36 |
| DXVK | 52.4 (9%) | 67.1 (46%) |  |  | 131.5 (5%) | 1.19 |
| wine / Proton | 26.0 (5%) | 17.0 (12%) | 58.4 (5%) | 12.9 (3%) | 129.2 (5%) | 0.59 |
| GPU driver | 2.9 (1%) | 52.1 (36%) |  |  | 59.7 (2%) | 0.07 |
| EDHM (d3d11_edhm_2dmigoto.dll) | 59.4 (10%) |  |  |  | 59.4 (2%) | 1.34 |
| system libs | 1.1 (0%) | 2.5 (2%) | 5.3 (0%) | 1.2 (0%) | 14.9 (1%) | 0.03 |
| edworld (edworld_eht.dll) | 7.6 (1%) |  |  |  | 7.6 (0%) | 0.17 |
| vdso (clock reads) | 0.5 (0%) | 0.1 (0%) | 5.7 (0%) | 0.4 (0%) | 7.0 (0%) | 0.01 |
| other | 0.1 (0%) |  | 0.0 (0%) | 0.1 (0%) | 2.6 (0%) | 0.00 |
| **sum** | **579** | **144** | **1254** | **453** | **2536** | **13.09** |

## Threads by CPU load

| thread | % of one core | threads |
|---|---:|---:|
| Worker* | 125.4 | 6 |
| Render | 57.9 | 10 |
| RunLoop | 45.3 | 7 |
| dxvk-cs | 14.4 | 1 |
| The:\ | 6.9 | 1 |
| <NULL> | 2.8 | 27 |
| dxvk-submit | 2.5 | 4 |
| dxvk-queue | 2.2 | 1 |
| dxvk-descriptor | 1.3 | 1 |
| audio_client_ma | 0.6 | 1 |
| S:\steamapps\co | 0.5 | 1 |
| FAudio_AudioCli | 0.3 | 1 |

Notes: self time only, no call stacks. Samples taken right after a hook (EDHM, EDVR, edworld, edloader) forwards a D3D call are counted to the hook, so hook shares can be somewhat high. Rows under 0.5 ms/s are left out of the table but counted in the sums. With the game at its frame limit, the Render thread waits for the next frame in wine (ntdll.so) and vdso clock reads, so those rows there are mostly waiting, not work.
