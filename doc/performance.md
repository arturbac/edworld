# edworld: what it costs

What edworld takes from the game's frame, measured in the game and without it.

`tools/bench.sh <build dir> <scratch> [<width> <height>]` measures edworld without the game: the clouds' volume pass
under DXVK (its `d3d11.dll` and `dxgi.dll` from a game prefix, `EDWORLD_DXVK_DIR`) for the scenes of two planets,
and the render thread's cost of a draw through edworld (`test_app.exe`, `EDWORLD_TEST_BENCH`; one run varies by tens
of percent, so it runs `EDWORLD_BENCH_CPU_RUNS` times, 5 unless set, 1 to 2 s each, and gives each number's median).
Each scene's median is compared with `test/data/bench/<card>-<width>x<height>.txt` (more than 15% slower: FAIL);
`EDWORLD_BENCH_UPDATE=1` writes it again.

The bench against the game (2026-11-18, RX 7900 XTX, DXVK, 9000x2160 at a 60 fps limit, Bleia Eohn IY-B c16-6 1,
CarbonDioxide, radius 3687 km, `aurora_activity = 7`; the game's numbers are the `clouds gpu 5s` medians):

| scene | the game, ms | the bench, ms | game / bench | of a 16.7 ms frame |
|---|---|---|---|---|
| orbit, by day | 1.4 | 2.1 | 0.67 | 8% |
| orbit, by night | 1.0 | 2.5 | 0.41 | 6% |
| inside the layer | 5.3 | 10.7 | 0.50 | 32% |
| inside the aurora | 6.3 (2.3 with `aurora = 0`) | 14.7 (1.2) | 0.43 | 38% |

The bench's scenes stand where the game's were only roughly (the game's heights are not logged), so the bench gives
an upper bound: the game costs 0.4 to 0.7 of it. The aurora seen from inside it is the dearest part (about 4 ms in
the game); the 60 fps held in every scene. A draw through edworld costs the render thread about 0.2 us.
