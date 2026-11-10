# seedfinder

Offline scanner for a Sakray/rAthena-style RNG chain:

```
std::mt19937 -> rnd() -> pick rule -> elimination model (6 rooms, 5 eliminated)
```

Give it what you observed (one event, or several events with unknown `rnd()` gaps between them) and it lists every `(seed, offset, gap path)` that reproduces it, plus a prediction of what comes next.

## Build

Needs a C++17 compiler (tested with TDM-GCC 10.3) and CMake 3.20+. The GPU backend also needs a GPU driver that provides OpenCL.

The default build has the GPU backend:

```
cmake --preset mingw-release-opencl
cmake --build --preset mingw-release-opencl
build/mingw-release-opencl/seedfinder.exe --help
```

`mingw-release` is the CPU-only build (`SEEDFINDER_OPENCL=OFF`), and `mingw-debug` is `-O0 -g`. None of the builds needs network. The OpenCL build needs no OpenCL driver at build time: the Khronos OpenCL headers are vendored in `third_party/OpenCL-Headers` (Apache-2.0, commit `82f6031`), and the OpenCL runtime (`OpenCL.dll`, installed with the GPU driver) is loaded when the program starts instead of being linked. A machine without an OpenCL driver therefore still runs the exe and falls back to the CPU.

## Backends

`--backend auto|opencl|cpu`, default `auto`.

| Value | Behaviour |
|---|---|
| `auto` | Use the GPU when this build has OpenCL, a GPU device initialises and the options fit the GPU limits (see "GPU backend"). Otherwise use the CPU and print one line on stderr: `note: OpenCL unavailable (<reason>), using CPU backend`. In a CPU-only build, `auto` silently means `cpu`. |
| `opencl` | Use the GPU or fail with the reason (`error: --backend opencl: ...`). Never falls back. In a CPU-only build the reason is `built without OpenCL`. |
| `cpu` | Thread pool only (`--threads`). |

The report starts with the backend actually used, for example `backend    : opencl (NVIDIA GeForce GTX 1650)` or `backend    : cpu (8 threads)`.

`--gpu-batch N` sets the seeds per GPU launch. Without it, launches are timed and sized to about 0.4 s each (first launch 2^16 seeds, then growing, never above 2^24). That keeps each launch under the Windows display watchdog (about 2 s), which resets a GPU stuck on one long launch.

## Layout

| Path | Concept |
|---|---|
| `include/seedfinder/core/sim_core.h` | All random logic: MT19937 (on-demand twist), mappings, pick rules, models. C++17 and OpenCL C 1.2 compatible, no STL. |
| `config/options`, `config/cli` | `Options` (the setter), `validate()` warnings, flag parsing, `--help` |
| `rng/engine`, `rng/draw_buffer` | `std::mt19937` reference engine (for `--mapping std`); per-seed buffer of `rnd()` outputs |
| `rules/pick`, `rules/model` | C++ wrappers that map options onto `sim_core.h` |
| `post/rules` | Post-rules applied to each simulated event (prefix, known survivor, excluded rooms) |
| `logs/observation_log` | Reads and writes the observation log |
| `compute/chain` | Gap-tolerant multi-event matching |
| `compute/volatility` | Volatility score for the predicted event |
| `compute/cpu_scanner`, `compute/stats` | Thread pool over 64K-seed chunks; result accumulation |
| `compute/opencl_scanner`, `compute/kernels/scan.cl` | GPU prefilter (OpenCL host code and kernel); the kernel source and `sim_core.h` are embedded into the exe at build time |
| `compute/backend_select` | Picks the backend from `--backend` and what the build and machine support |
| `export/csv`, `export/report` | CSV write/read with metadata; stdout summary |
| `apps/seedfinder/main.cpp` | Wiring: parse, validate, scan or re-filter, report |
| `scripts/parity.sh` | Compares the new tool with the legacy one, and the GPU backend with the CPU backend |

## How the random logic works

1. **Mapping** turns one MT19937 word into `rnd()`: `high31` is `w >> 1`, `low31` is `w & 0x7fffffff`, `reject31` redraws until `w <= 0x7fffffff`. `std` uses `std::uniform_int_distribution<int32_t>(0, INT32_MAX)` from your standard library.
2. **Pick rule** turns a draw into an index among the remaining choices: `direct` is `raw % choices`; `fixed-mod` and `fixed-scale` first reduce the draw into a fixed arithmetic domain (`--base-min`, `--base-max`, `--value-step`), then take the stepped value modulo the choices or scale the ordinal into equal buckets.
3. **Model** turns indices into eliminated rooms: `remaining` (erase, order kept), `swapremove`, `reroll` (redraw until the room is unused, capped at 64 picks per event, after which the event counts as non-matching and is reported as an invalid event), `shuffle` (Fisher-Yates, first five are the elimination order).
4. `--rng-step N` makes every pick consume N buffer slots (the draw, then N-1 skipped). `--offset-step N` tests starts `offset_begin, +N, ...`.

**`dist-gcc10` and `dist-gcc11` pick rules.** Emulates `std::uniform_int_distribution<int>(0, choices-1)` called directly on the MT engine, as libstdc++ 10 implements it (draw a raw 32-bit word until it is below `choices * (0xFFFFFFFF / choices)`, then divide). It reads raw MT words, so `--mapping` is ignored (a warning is printed) and offsets, gaps and `--rng-step` count engine words, not `rnd()` calls. Each word read, including a rejected one, advances by `--rng-step`. More than 64 words in one pick marks the event invalid. `dist-gcc10` was checked bit-for-bit against the local libstdc++ 10.3 for choices 2..6.

`dist-gcc11` is the same distribution as libstdc++ 11 implements it, Lemire's nearly divisionless method: `product = (u64)word * choices`, redraw while the low 32 bits of the product are below `(-choices) % choices`, result `product >> 32`. It is ported from `uniform_int_distribution::operator()` and `_S_nd` in libstdc++ 11's `bits/uniform_int_dist.h` (the 32-bit generator branch), with the same raw-word, `--rng-step` and 64-word-cap handling as `dist-gcc10`. The check is limited to this: a scratch program (not in the repo) transcribed that function from the libstdc++ 11 source and compared it with the port on 38 million picks for choices 2..6, including every raw word that triggers a redraw and `--rng-step` 1 and 3, with no mismatches. There is no GCC 11 on this machine, so it was not compared against a compiled libstdc++ 11. The libstdc++ 11 header is GPL-licensed and is not vendored here.

`--probe-mapping` prints which explicit mapping `--mapping std` equals on this build (`std == reject31` on TDM-GCC 10.3).

`--mapping std` depends on the standard library. On libstdc++ 10 it rejects draws at or above 2^31, so it equals `reject31` there. Newer libstdc++ versions use a different method, so check which one matches your server's build.

## GPU backend

The GPU is an exact prefilter, not a second implementation of the output. One work-item runs per seed. It fills that seed's draw window, then walks the whole chain (all offsets, all gap values, all events) with the same `sim_core.h` functions the CPU uses. If any path matches every event, the seed goes into a hit buffer. The host then runs the CPU `ChainMatcher` on exactly those seeds, so stats, CSV rows, volatility and examples come from the CPU code. The CSV is written in ascending seed order.

- **What runs on the GPU:** every mapping except `std` itself, every pick rule, every model, any number of events up to 8. `--mapping std` uses the explicit mapping that `--probe-mapping` finds equal to it (`reject31` on TDM-GCC 10.3) and prints a warning saying so.
- **Batches:** seeds go to the GPU in batches while the previous batch is re-checked on the CPU threads. If a batch finds more hits than the hit buffer holds (2^22), it is rerun as two halves, so no hit is dropped.
- **Invalid events:** the kernel counts them the way the CPU does, so the `invalid events` line matches.
- **Short window:** the CPU sizes its draw window for the worst case (a `dist-*` pick may read 64 words, a reroll event 64 picks). The kernel fills only what events usually use: 5 picks per event (24 for reroll) times `--rng-step`, plus the offsets and gaps. If an event in some seed runs past that, the kernel cannot tell a real cap from a cut-off window, so it hands that seed to the CPU, which settles it, including its invalid-event count. With the full window, `dist-gcc10` over 16M seeds measured 59 s on the GPU; with the short window it measured 0.7 s (Intel UHD Graphics).
- **GPU window limit:** the kernel builds the draws from the first init words of the Mersenne Twister without the 624-word state (`sf_seed_fill_lazy`). That works only for a short window: at most 226 draws for `high31`, `low31` and raw words, and 96 for `reject31` (about half of its words are dropped). Beyond that the kernel would need the full state in each work-item's private memory. On the one GPU tested (Intel UHD Graphics, 23 compute units) that measured 2.3x to 3x slower than 8 CPU threads (window 311: 2.4 s against 1.0 s on 2M seeds; window 3011: 16.8 s against 6.4 s). Over the limit, `auto` uses the CPU with a note and `opencl` fails. The window is `offset_end + sum of later gap maxima + events x slots per event`, so the limit mostly restricts `--offset-end` and gap widths: for large offsets, use the CPU backend. The limits are in `include/seedfinder/compute/opencl_scanner.hpp`.
- **Device choice:** a discrete GPU if there is one, otherwise the integrated one (compute units times clock breaks ties). The report shows which.

## Observation log (`--obs FILE`)

```
event 5,1,2,6,3 survivor=4
gap 0..40
event 2,4,1
gap 10..200
event 3
```

- `event LIST [survivor=N] [exclude=A,B]`: eliminations seen (1..6, up to 5, partial prefix allowed). `exclude` lists rooms known not to be the survivor.
- `gap N` or `gap A..B`: number of `rnd()` calls between the previous event's last draw and the next event's first draw. No gap line means 0.
- `#` starts a comment. Errors name the line. At most 8 events. The last event is the one predicted.
- `--calib 5,1` is shorthand for a one-event log.

Each fully known extra event cuts false seeds by roughly 720x.

## Volatility

For each surviving start, the last event's gap is unknown, so several gap values can match the logged prefix and predict different continuations. The tool collects the gaps that match and scores them:

```
volatility = 1 - (count of the most common prediction) / (matching gaps)
```

0 means the prediction does not depend on the unknown gap. A one-event run has no gap, so volatility is 0. The report lists the lowest-volatility survivors and a continuation distribution where each survivor votes with weight `1 - volatility`.

## CSV

```
# model=remaining mapping=std pick=direct base_min=1 base_max=10000 value_step=1 offset_step=1 rng_step=1 obs=5,1
seed,offset,gap_path,e1,e2,e3,e4,e5,survivor,volatility
```

`e1..e5` and `survivor` are the predicted (last) event. `--csv-in` recomputes volatility for the new prefix and depth, one group at a time, so it needs the rows of a group to stay adjacent: do not sort or shuffle a CSV you plan to re-filter. A scan writes each seed's rows as one contiguous block. `--csv-in` and `--csv-out` must be different files. `--csv-in` skips `#` lines, rejects out-of-range rows with the line number, warns when the metadata differs from the current flags, and also reads the legacy 8-column layout.

## Checks

- `scripts/parity.sh`: two sections. (1) 37 legacy-vs-new comparisons with `--backend cpu` (4 models x 3 mappings x 3 pick rules, plus a step run), diffing the sorted rows. (2) `--backend opencl` against `--backend cpu` over `GPU_SEED_END` seeds (default 2^24) for five cases (single event, a two-event log with a widened gap, reroll, `dist-gcc10`, `dist-gcc11`): sorted CSV rows must be identical, and stdout must be identical apart from the backend line. Section 2 is skipped with a message when the binary has no OpenCL or no GPU initialises.
- `--emit-events SEED OFFSET GAP1,GAP2,...` prints a log simulated from a known seed; widen its gaps into ranges and feed it back through `--obs` to check that the seed comes out.
