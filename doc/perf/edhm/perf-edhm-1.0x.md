# ED CPU profile 2026-11-15 18:15:35

- account: Sjona (Steam), pid 360978
- window: 30.2 s, 180677 samples (perf, 1999 Hz per thread)
- fps 44.1 (EHT overlay frame count) = 22.7 ms per frame; Render thread busy 56% = 12.7 ms of CPU per frame

## CPU by module

ms of CPU per second of wall time (1000 = one full core); in brackets the share of that column. Worker\* = all Worker threads together, all threads = every thread of the game.

| module | Render | dxvk-cs | Worker* | RunLoop | all threads | Render ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| ED (EliteDangerous64.exe) | 409.9 (73%) |  | 1075.1 (87%) | 431.2 (93%) | 2025.6 (76%) | 9.29 |
| kernel (system calls) | 14.3 (3%) | 5.8 (4%) | 89.7 (7%) | 18.3 (4%) | 211.8 (8%) | 0.32 |
| wine / Proton | 24.3 (4%) | 16.8 (12%) | 57.9 (5%) | 14.2 (3%) | 165.4 (6%) | 0.55 |
| DXVK | 45.8 (8%) | 63.8 (47%) |  |  | 121.4 (5%) | 1.04 |
| GPU driver | 2.6 (0%) | 48.0 (35%) |  |  | 55.7 (2%) | 0.06 |
| EDHM (d3d11_edhm.dll) | 52.4 (9%) |  |  |  | 52.4 (2%) | 1.19 |
| system libs | 1.2 (0%) | 2.5 (2%) | 5.4 (0%) | 1.3 (0%) | 15.1 (1%) | 0.03 |
| edworld (edworld_eht.dll) | 7.9 (1%) |  |  |  | 7.9 (0%) | 0.18 |
| vdso (clock reads) | 0.6 (0%) | 0.1 (0%) | 4.8 (0%) | 0.6 (0%) | 6.5 (0%) | 0.01 |
| other | 0.1 (0%) |  |  | 0.0 (0%) | 3.4 (0%) | 0.00 |
| **sum** | **559** | **137** | **1233** | **466** | **2665** | **12.68** |

## Threads by CPU load

| thread | % of one core | threads |
|---|---:|---:|
| Worker* | 123.3 | 6 |
| Render | 55.9 | 9 |
| RunLoop | 46.6 | 6 |
| <NULL> | 19.7 | 27 |
| dxvk-cs | 13.7 | 1 |
| The:\ | 4.4 | 1 |
| dxvk-submit | 2.6 | 4 |
| dxvk-queue | 1.9 | 1 |
| dxvk-descriptor | 1.2 | 1 |
| audio_client_ma | 0.5 | 1 |
| S:\steamapps\co | 0.4 | 1 |
| FAudio_AudioCli | 0.3 | 1 |

Notes: self time only, no call stacks. Samples taken right after a hook (EDHM, EDVR, edworld, edloader) forwards a D3D call are counted to the hook, so hook shares can be somewhat high. Rows under 0.5 ms/s are left out of the table but counted in the sums. With the game at its frame limit, the Render thread waits for the next frame in wine (ntdll.so) and vdso clock reads, so those rows there are mostly waiting, not work.
