# ED CPU profile 2026-11-15 18:20:35

- account: Sjona (Steam), pid 371911
- window: 30.2 s, 173675 samples (perf, 1999 Hz per thread)
- fps 44.4 (EHT overlay frame count) = 22.5 ms per frame; Render thread busy 56% = 12.7 ms of CPU per frame

## CPU by module

ms of CPU per second of wall time (1000 = one full core); in brackets the share of that column. Worker\* = all Worker threads together, all threads = every thread of the game.

| module | Render | dxvk-cs | Worker* | RunLoop | all threads | Render ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| ED (EliteDangerous64.exe) | 417.6 (74%) |  | 1072.1 (86%) | 399.6 (92%) | 1890.7 (76%) | 9.41 |
| kernel (system calls) | 14.7 (3%) | 5.6 (4%) | 93.8 (8%) | 19.6 (4%) | 185.5 (7%) | 0.33 |
| DXVK | 47.1 (8%) | 68.4 (47%) |  |  | 128.0 (5%) | 1.06 |
| wine / Proton | 18.6 (3%) | 15.7 (11%) | 64.6 (5%) | 14.0 (3%) | 125.1 (5%) | 0.42 |
| GPU driver | 2.4 (0%) | 52.7 (36%) |  |  | 59.5 (2%) | 0.05 |
| EDHM (d3d11_edhm_test2.dll) | 49.6 (9%) |  |  |  | 49.6 (2%) | 1.12 |
| system libs | 1.4 (0%) | 2.4 (2%) | 4.9 (0%) | 1.3 (0%) | 13.9 (1%) | 0.03 |
| edworld (edworld_eht.dll) | 11.8 (2%) |  |  |  | 11.8 (0%) | 0.27 |
| vdso (clock reads) | 0.4 (0%) | 0.1 (0%) | 5.8 (0%) | 0.6 (0%) | 7.1 (0%) | 0.01 |
| other | 0.1 (0%) |  |  | 0.0 (0%) | 3.9 (0%) | 0.00 |
| **sum** | **564** | **145** | **1241** | **435** | **2475** | **12.70** |

## Threads by CPU load

| thread | % of one core | threads |
|---|---:|---:|
| Worker* | 124.1 | 6 |
| Render | 56.4 | 10 |
| RunLoop | 43.5 | 5 |
| dxvk-cs | 14.5 | 1 |
| The:\ | 5.9 | 1 |
| dxvk-submit | 2.7 | 4 |
| dxvk-queue | 2.1 | 1 |
| <NULL> | 1.4 | 26 |
| dxvk-descriptor | 1.3 | 1 |
| audio_client_ma | 0.5 | 1 |
| S:\steamapps\co | 0.5 | 1 |
| FAudio_AudioCli | 0.2 | 1 |

Notes: self time only, no call stacks. Samples taken right after a hook (EDHM, EDVR, edworld, edloader) forwards a D3D call are counted to the hook, so hook shares can be somewhat high. Rows under 0.5 ms/s are left out of the table but counted in the sums. With the game at its frame limit, the Render thread waits for the next frame in wine (ntdll.so) and vdso clock reads, so those rows there are mostly waiting, not work.
