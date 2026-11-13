<div align="center">

# seedtracker

**Find the MT19937 seed behind an observed draw, then predict what it does next.**

Offline · CPU or GPU · one observed event or a whole log of them

[Quick start](#quick-start) · [How it works](#how-it-works) · [Observation logs](#observation-logs) · [Reading the output](#reading-the-output) · [Options](#options) · [GPU backend](#gpu-backend) · [Verify](#verify)

</div>

---

## What it does

Anything that draws from one `std::mt19937` stream through fixed rules is deterministic: the seed and the rules decide every event. Turn that around and the events you observe pin down the seed.

`seedtracker` replays the random state from any point in the stream against the rules you assume. You give it what you saw, and it returns every `(seed, offset, gap path)` that reproduces it, plus odds for what comes next. Candidate mappings are explicit, `--probe-mapping` checks which one `std::uniform_int_distribution` really equals on a build, and the scan runs in parallel on the CPU or the GPU.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/diagram/pipeline-dark.svg">
    <img alt="Seed → std::mt19937 → RULES → Match → Candidates" src="docs/diagram/pipeline-light.svg" width="100%">
  </picture>
</p>

> [!NOTE]
> The RULES are assumptions, so each stage is configurable. If you don't know which combination your target uses, scan with a few and keep the one whose predictions keep coming true.

| You give it | It gives you |
| --- | --- |
| One event's first eliminations (`--calib 5,1`) | Every seed and offset that starts that way, and the odds for eliminations 3, 4, 5 and the survivor |
| A log of several events with unknown gaps between them (`--obs FILE`) | The few seeds that fit the whole chain, with the gaps they needed and a prediction for the last event |
| A saved candidate CSV plus one more observed room (`--csv-in`) | A smaller CSV, without rescanning seeds |

---

## Quick start

### 1. Check the requirements

| Requirement | Version | Needed for |
| --- | --- | --- |
| C++ compiler | C++17 (tested with TDM-GCC 10.3) | Always |
| CMake | 3.20 or newer | Always |
| MinGW Makefiles generator | ships with TDM-GCC / MinGW-w64 | The bundled presets |
| GPU driver with OpenCL (`OpenCL.dll`) | OpenCL 1.2 or newer | Only the GPU backend, and only at **run** time |
| POSIX shell (Git Bash, MSYS2) | any | Only `scripts/parity.sh` |

> [!TIP]
> No OpenCL SDK is needed to build. The Khronos headers are vendored in `third_party/OpenCL-Headers`, and `OpenCL.dll` is loaded when the program starts instead of being linked. The same `.exe` runs on a machine without a GPU driver; it just uses the CPU.

### 2. Build

```powershell
cmake --preset mingw-release-opencl
cmake --build --preset mingw-release-opencl
.\build\mingw-release-opencl\seedtracker.exe --help
```

<details>
<summary><b>Other build presets</b></summary>

<br>

| Preset | What you get |
| --- | --- |
| `mingw-release-opencl` | **Default.** `-O2`, CPU and GPU backends. |
| `mingw-release` | `-O2`, CPU only (`SEEDTRACKER_OPENCL=OFF`). |
| `mingw-debug` | `-O0 -g`, CPU only, for stepping through in a debugger. |

The executable lands in `build/<preset>/seedtracker.exe`. Swap the preset name in both commands above.

</details>

<details>
<summary><b>Building without the presets</b> (another generator or toolchain)</summary>

<br>

```powershell
cmake -S . -B build/custom -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DSEEDTRACKER_OPENCL=ON
cmake --build build/custom
```

`SEEDTRACKER_OPENCL` defaults to `OFF` outside the `mingw-release-opencl` preset.

</details>

### 3. Run your first scan

You watched an event and the first two rooms eliminated were **5**, then **1**:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --seed-end 100000000 --calib 5,1 --depth 3 --csv-out candidates.csv
```

This tests seeds `0` to `99,999,999`, keeps every seed whose first two eliminations are 5 then 1, prints the odds for the next three, and saves the survivors to `candidates.csv`.

### 4. Narrow it down as you see more

The third room turned out to be **2**. Re-filter the saved file instead of rescanning:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --csv-in candidates.csv --calib 5,1,2 --depth 2 --csv-out narrowed.csv
```

> [!IMPORTANT]
> One event can only narrow the field so far: there are 720 possible elimination orders, and a 100-million-seed scan has far more seeds than that. To get down to a handful of seeds, log **several** events and use an [observation log](#observation-logs). Each fully known extra event cuts the false seeds by roughly 720×.

---

## How it works

Each candidate seed is simulated from the start and compared with what you observed. One mapping plus the RULES decide how a raw MT19937 word becomes an event.

<table>
<tr><th width="18%">Stage</th><th width="22%">Flag</th><th>What it decides</th></tr>
<tr><td><b>1. Mapping</b></td><td><code>--mapping</code></td><td>How one 32-bit MT word becomes one <code>rnd()</code> value.</td></tr>
<tr><td><b>2. RULES: pick rule</b></td><td><code>--pick</code></td><td>How one <code>rnd()</code> value becomes an index among the choices left.</td></tr>
<tr><td><b>3. RULES: model</b></td><td><code>--model</code></td><td>How those indices become the event.</td></tr>
<tr><td><b>4. RULES: stepping</b></td><td><code>--rng-step</code>, <code>--offset-step</code></td><td>Whether other code consumes draws in between, and which start positions to test.</td></tr>
</table>

<details>
<summary><b>Mappings</b> (<code>--mapping</code>, default <code>std</code>)</summary>

<br>

| Value | `rnd()` is |
| --- | --- |
| `high31` | `w >> 1` |
| `low31` | `w & 0x7fffffff` |
| `reject31` | `w`, redrawn until `w <= 0x7fffffff` |
| `std` | `std::uniform_int_distribution<int32_t>(0, INT32_MAX)` from **your** standard library |

> [!WARNING]
> `std` is only as portable as the standard library you built with. On libstdc++ 10 it rejects draws at or above 2^31, so it equals `reject31`. Newer libstdc++ versions use a different method. Run `--probe-mapping` to see which explicit mapping `std` matches on your build, and pick the explicit one that matches the **target's** build.

```console
$ seedtracker --probe-mapping
std == reject31
probed 5 seeds x 100000 draws
libstdc++ release: 10
```

</details>

<details>
<summary><b>Pick rules</b> (<code>--pick</code>, default <code>direct</code>)</summary>

<br>

| Value | Index is |
| --- | --- |
| `direct` | `rnd() % choices` |
| `fixed-mod` | `rnd()` reduced into a fixed domain (`--base-min`..`--base-max`, stepping by `--value-step`), then that value `% choices` |
| `fixed-scale` | Same fixed domain, but the value's position is scaled into equal buckets, one per choice |
| `dist-gcc10` | `std::uniform_int_distribution<int>(0, choices-1)` as libstdc++ 10 implements it: redraw a raw word until it is below `choices * (0xFFFFFFFF / choices)`, then divide |
| `dist-gcc11` | The same distribution as libstdc++ 11 implements it (Lemire's method): `product = (u64)word * choices`, redraw while the low 32 bits are below `(-choices) % choices`, index is `product >> 32` |

The fixed-domain defaults are `--base-min 1 --base-max 10000 --value-step 1`, which models a call like `rnd_value(1, 10000)`.

> [!CAUTION]
> `dist-gcc10` and `dist-gcc11` read **raw MT words**, not `rnd()` values. `--mapping` is ignored (a warning is printed), and offsets, gaps and `--rng-step` count engine words. Every word read, including a rejected one, advances by `--rng-step`. More than 64 words in one pick marks the event invalid.

**How they were checked.** `dist-gcc10` was compared bit-for-bit against the local libstdc++ 10.3 for 2 to 6 choices. `dist-gcc11` is ported from `uniform_int_distribution::operator()` and `_S_nd` in libstdc++ 11's `bits/uniform_int_dist.h` (32-bit generator branch). A scratch program transcribed that function from source and matched the port on 38 million picks for 2 to 6 choices, including every redraw-triggering word and `--rng-step` 1 and 3. It was **not** compared against a compiled libstdc++ 11, because none was available. The GPL-licensed libstdc++ 11 header is not vendored.

</details>

<details>
<summary><b>Models</b> (<code>--model</code>, default <code>remaining</code>)</summary>

<br>

| Value | Behaviour |
| --- | --- |
| `remaining` | Keep a list of open rooms. Erase the picked one; the rest keep their order. |
| `swapremove` | Same, but the last room moves into the picked slot. |
| `reroll` | Pick from all six rooms; redraw until the room is still open. Capped at 64 picks per event, after which the event counts as invalid. |
| `shuffle` | Fisher-Yates shuffle of all six rooms. The first five are the elimination order. |

</details>

<details>
<summary><b>Stepping</b> (<code>--rng-step</code>, <code>--offset-step</code>)</summary>

<br>

- `--rng-step N` makes every pick consume N draws: the one it uses, then N-1 skipped. Use it when other code draws from the same stream between picks.
- `--offset-step N` tests start positions `offset-begin`, `offset-begin + N`, and so on, up to `offset-end`.

Both accept 1 to 2^20.

</details>

---

## Observation logs

One event leaves thousands of candidates. A log of several events, with the unknown number of `rnd()` calls between them, leaves a handful. Pass it with `--obs FILE`:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --seed-end 100000000 --offset-end 8 --obs examples/observations.txt --csv-out chain.csv
```

From [`examples/observations.txt`](examples/observations.txt):

```text
event 5,1,2,6,3 survivor=4
gap 0..40
event 2,4,1,3,5
gap 10..200
event 3
```

| Line | Meaning |
| --- | --- |
| `event LIST` | The eliminations you saw, rooms `1`..`6`, at most five. A partial prefix is fine. |
| `survivor=N` | *Optional.* The room that was left, when you know it. |
| `exclude=A,B` | *Optional.* Rooms known **not** to be the survivor. |
| `gap N` | Exactly N `rnd()` calls between the previous event's last draw and the next event's first draw. |
| `gap A..B` | Somewhere between A and B calls, inclusive. Every value is tried. |
| *(no gap line)* | Gap 0. |
| `# ...` | Comment. |

> [!IMPORTANT]
> The **last** event is the one being predicted, so it is usually a partial prefix. Every earlier event should be as complete as you can make it. At most 8 events. Parse errors name the line.

> [!NOTE]
> `--calib 5,1` is shorthand for a one-event log. Use one or the other, not both.

---

## Reading the output

<details open>
<summary><b>One-event scan</b>, <code>--seed-end 1000000 --calib 5,1 --depth 3</code></summary>

<br>

```text
=== Candidate summary ===
backend    : opencl (Intel(R) UHD Graphics)
model      : remaining
mapping    : std
pick rule  : direct
calibration: 5,1
matching candidates: 33372

Next depth 1 (elimination #3):
  3 : 25.201% (8410)
  6 : 25.123% (8384)
  2 : 25.081% (8370)
  4 : 24.595% (8208)
...
Survivor distribution:
  6 : 25.138% (8389)
  ...
Top continuation paths (up to depth 3):
  3-2-4 : 4.342% (1449)
  ...
Example surviving candidates:
  seed=40 offset=0 seq=5-1-3-6-4 survivor=2
```

| Section | Read it as |
| --- | --- |
| `matching candidates` | How many `(seed, offset)` pairs reproduce what you saw. |
| `Next depth N` | Odds for the Nth room after your prefix, counted over all candidates. |
| `Survivor distribution` | Odds for the room left standing. |
| `Top continuation paths` | The most common full continuations up to `--depth`. |
| `Example surviving candidates` | A few real rows, so you can spot-check them with `--emit-events`. |

Near-even odds like these mean "not enough evidence yet". Add events until one answer dominates.

</details>

<details>
<summary><b>Multi-event scan</b>, with volatility</summary>

<br>

```text
matching candidates: 4

Lowest-volatility survivors (0 = the prediction does not depend on the unknown last gap):
  seed=12345 offset=3 gaps=5-10 seq=1-4-3-6-2 survivor=5 volatility=0.000
  seed=12772 offset=3 gaps=14-0 seq=1-4-3-6-5 survivor=2 volatility=0.667
  seed=12772 offset=3 gaps=14-7 seq=1-4-2-5-6 survivor=3 volatility=0.667

Volatility-weighted continuation (each survivor votes with weight 1 - volatility):
  3-6-2 : 66.667% (weight 1.333)
  2-5-6 : 16.667% (weight 0.333)
  3-6-5 : 16.667% (weight 0.333)
```

**Volatility** measures how much a candidate's prediction depends on the one thing it can't know: the gap before the last event. Several gap values can match the logged prefix and still predict different endings.

```text
volatility = 1 - (count of the most common prediction) / (number of matching gaps)
```

| Volatility | Meaning |
| --- | --- |
| `0.000` | Every matching gap predicts the same ending. Trust it. |
| close to `1` | The ending depends on a gap you didn't observe. Treat it as a guess. |

A one-event run has no gap, so its volatility is always 0.

</details>

---

## Options

Run `seedtracker --help` for the full text. Defaults are in **bold**.

<details open>
<summary><b>Search range</b></summary>

<br>

| Flag | Default | Meaning |
| --- | --- | --- |
| `--seed-begin N` | **0** | First seed tested (inclusive). |
| `--seed-end N` | **2^20** | Stop before this seed (exclusive). The default is a small demo range. |
| `--offset-begin N` | **0** | First start position in the `rnd()` stream. |
| `--offset-end N` | **1** | Stop before this start position. |
| `--offset-step N` | **1** | Test every Nth start position. |
| `--rng-step N` | **1** | Draws consumed per pick. |

</details>

<details open>
<summary><b>What you observed</b></summary>

<br>

| Flag | Default | Meaning |
| --- | --- | --- |
| `--calib 5,1,2` | | One event's elimination prefix. |
| `--obs FILE` | | A multi-event [observation log](#observation-logs). |
| `--depth N` | **3** | How many future eliminations to predict (1 to 5). |
| `--examples N` | **12** | How many example candidates to print. |

</details>

<details>
<summary><b>RNG model</b></summary>

<br>

| Flag | Values | Default |
| --- | --- | --- |
| `--model` | `remaining` · `reroll` · `swapremove` · `shuffle` | **`remaining`** |
| `--mapping` | `std` · `high31` · `low31` · `reject31` | **`std`** |
| `--pick` | `direct` · `fixed-mod` · `fixed-scale` · `dist-gcc10` · `dist-gcc11` | **`direct`** |
| `--base-min N` | 0..INT32_MAX | **1** |
| `--base-max N` | 0..INT32_MAX | **10000** |
| `--value-step N` | 1..2^20 | **1** |

See [How it works](#how-it-works) for what each value does.

</details>

<details>
<summary><b>Backend and performance</b></summary>

<br>

| Flag | Default | Meaning |
| --- | --- | --- |
| `--backend` | **`auto`** | `auto`, `opencl` or `cpu`. See [GPU backend](#gpu-backend). |
| `--threads N` | **all cores** | CPU worker threads. With the GPU, these re-check GPU hits. Max 4096. |
| `--gpu-batch N` | **auto-sized** | Seeds per GPU launch. Auto sizing aims for about 0.4 s per launch. |

</details>

<details>
<summary><b>Files and tools</b></summary>

<br>

| Flag | Meaning |
| --- | --- |
| `--csv-out FILE` | Save every surviving candidate. |
| `--csv-in FILE` | Re-filter a saved CSV instead of scanning seeds. |
| `--probe-mapping` | Print which explicit mapping `std` equals on this build, then exit. |
| `--emit-events SEED OFFSET GAP1,GAP2,...` | Print the observation log a known seed would produce. Use `-` for no gaps. |

</details>

---

## GPU backend

`--backend auto|opencl|cpu` picks where the scan runs.

| Value | Behaviour |
| --- | --- |
| `auto` *(default)* | GPU when the build has OpenCL, a device initialises, and the options fit the GPU limits. Otherwise CPU, with one line on stderr: `note: OpenCL unavailable (<reason>), using CPU backend`. |
| `opencl` | GPU or exit with `error: --backend opencl: <reason>`. Never falls back. |
| `cpu` | Thread pool only. Never touches the GPU. |

The report's first line names what actually ran, for example `backend : opencl (NVIDIA GeForce GTX 1650)` or `backend : cpu (8 threads)`.

> [!NOTE]
> The GPU is an **exact prefilter**, not a second implementation. It runs the same `sim_core.h` code as the CPU and only flags seeds that match every event. The CPU then re-runs those seeds, so every number, CSV row and prediction comes from the CPU code. GPU and CPU results are identical; `scripts/parity.sh` checks this.

<details>
<summary><b>What runs on the GPU</b></summary>

<br>

- Every mapping except `std` itself. `--mapping std` runs as whichever explicit mapping `--probe-mapping` reports (`reject31` on TDM-GCC 10.3), with a warning.
- Every pick rule and every model.
- Logs of up to 8 events.

The device is a discrete GPU if one exists, otherwise the integrated one. Ties break on compute units times clock speed.

</details>

<details>
<summary><b>Batching and the display watchdog</b></summary>

<br>

Seeds go to the GPU in batches while the CPU re-checks the previous batch. By default each launch is timed and resized to about 0.4 s: the first launch is 2^16 seeds, and it grows to at most 2^24. That keeps every launch well under the Windows display watchdog (about 2 s), which resets a GPU stuck on one long launch.

If a batch produces more hits than the hit buffer holds (2^22), it is rerun as two halves. No hit is ever dropped.

</details>

<details>
<summary><b>Window limits</b>: when the GPU can't be used</summary>

<br>

To stay fast, the kernel computes draws directly from MT19937's first initialisation words instead of keeping the full 624-word state per seed. That only works for a short window of draws:

| Mapping or pick | Max draws in the window |
| --- | --- |
| `high31`, `low31`, raw words (`dist-*`) | 226 |
| `reject31` | 96 (about half its words are rejected) |

The window is roughly `offset-end + the sum of later gap maxima + events × draws per event`. In practice the limit bites on large `--offset-end` values and wide gaps. Over the limit, `auto` falls back to the CPU with a note, and `opencl` fails.

> [!TIP]
> For large offsets, use `--backend cpu`. Keeping the full state on the GPU was measured at 2.3× to 3× **slower** than 8 CPU threads on an Intel UHD Graphics (23 compute units).

The kernel also fills a shorter window than the CPU's worst case: 5 picks per event (24 for `reroll`) times `--rng-step`, plus offsets and gaps. If a seed's event runs past that, the GPU hands the seed to the CPU to settle, including its invalid-event count. With the full window, `dist-gcc10` over 16M seeds took 59 s; with the short window it took 0.7 s.

The limits live in `include/seedtracker/compute/opencl_scanner.hpp`.

</details>

---

## CSV files

```text
# model=remaining mapping=std pick=direct base_min=1 base_max=10000 value_step=1 offset_step=1 rng_step=1 obs=5,1
seed,offset,gap_path,e1,e2,e3,e4,e5,survivor,volatility
```

`e1`..`e5` and `survivor` describe the **predicted** (last) event. The `#` line records the settings the file was made with.

> [!WARNING]
> `--csv-in` reads rows in groups and recomputes volatility one group at a time. A scan writes each seed's rows as one block, in ascending seed order. **Don't sort or shuffle a CSV you plan to re-filter.**

<details>
<summary><b>More <code>--csv-in</code> rules</b></summary>

<br>

- `--csv-in` and `--csv-out` must be different files.
- `#` lines are skipped. Out-of-range rows are rejected with their line number.
- A warning is printed when the file's metadata differs from the current flags.
- The legacy 8-column layout (no `gap_path`, no `volatility`) is also accepted.

</details>

---

## Verify

### Round trip: prove a seed comes back out

`--emit-events` prints the log a known seed produces. Feed it back in with widened gaps, and the seed should be found.

```powershell
.\build\mingw-release-opencl\seedtracker.exe --emit-events 12345 3 5,10
```

```text
event 2,1,4,3,5 survivor=6
gap 5
event 5,4,6,3,1 survivor=2
gap 10
event 1,4,3,6,2 survivor=5
```

Save it as `self.txt`, change both gaps to `gap 0..20`, and cut the last event to `event 1,4`. Then scan around the seed:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --seed-begin 12000 --seed-end 13000 --offset-end 8 --obs self.txt
```

`seed=12345 offset=3 gaps=5-10` comes back with volatility `0.000`, and it predicts `3-6-2`, survivor `5`, which matches the emitted log.

### Parity script

`scripts/parity.sh` runs in a POSIX shell (Git Bash or MSYS2).

| Section | Compares | Cases |
| --- | --- | --- |
| 1. Legacy | Original single-file tool against `--backend cpu` | 37: 4 models × 3 mappings × 3 pick rules, plus a step run |
| 2. GPU | `--backend opencl` against `--backend cpu` over `GPU_SEED_END` seeds (default 2^24) | 5: single event, two-event log with a wide gap, `reroll`, `dist-gcc10`, `dist-gcc11` |

```bash
scripts/parity.sh [legacy.exe] [seedtracker.exe]
```

> [!NOTE]
> Section 1 needs the original tool built as `build/legacy.exe`; it is not part of this repo. Section 2 is skipped with a message when the binary has no OpenCL or no GPU initialises. Rows are sorted before diffing, and section 2 also requires identical stdout apart from the backend line.

---

## Project map

<details>
<summary><b>Source layout</b></summary>

<br>

| Path | Contents |
| --- | --- |
| `apps/seedtracker/main.cpp` | Entry point: parse, validate, scan or re-filter, report. |
| `include/seedtracker/core/sim_core.h` | All random logic: MT19937 with on-demand twist, mappings, pick rules, models. Compiles as both C++17 and OpenCL C 1.2, no STL. |
| `src/config/` | `Options`, validation warnings, flag parsing, `--help`. |
| `src/rng/` | `std::mt19937` reference engine for `--mapping std`; per-seed buffer of `rnd()` values. |
| `src/rules/` | C++ wrappers that map options onto `sim_core.h`. |
| `src/post/` | Checks applied to each simulated event: prefix, known survivor, excluded rooms. |
| `src/logs/` | Reads and writes observation logs. |
| `src/compute/` | Multi-event matching, volatility, CPU thread pool (64K-seed chunks), statistics, backend selection, OpenCL host code. |
| `src/compute/kernels/scan.cl` | GPU kernel. It and `sim_core.h` are embedded into the `.exe` at build time. |
| `src/export/` | CSV read/write with metadata; the stdout report. |
| `scripts/parity.sh` | Legacy and GPU-vs-CPU parity checks. |
| `examples/observations.txt` | Sample multi-event log. |

</details>

---

## References

- Inspired by rAthena's random interface: [`random.hpp`](third_party/rathena/src/common/random.hpp) and [`random.cpp`](third_party/rathena/src/common/random.cpp), tracked as a shallow sparse submodule on upstream `master`.
- [`std::mt19937` reference](https://cplusplus.com/reference/random/mt19937/): the engine every candidate is simulated with.

## License

The vendored Khronos OpenCL headers in `third_party/OpenCL-Headers` (commit `82f6031`) are Apache-2.0. The rest of the project has no license file yet.
