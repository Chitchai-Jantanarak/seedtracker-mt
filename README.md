<div align="center">

# seedtracker

**Find the MT19937 seed behind an observed draw, then predict what it does next.**

Offline · CPU or GPU · one observed event or a whole log of them

[Quick start](#quick-start) · [How it works](#how-it-works) · [Observation logs](#observation-logs) · [Output](#output) · [Options](#options) · [GPU backend](#gpu-backend) · [Verify](#verify)

</div>

---

## What it does

Anything that draws from one `std::mt19937` stream through fixed rules is deterministic, so the events you observe pin down the seed.

`seedtracker` **REPLAYS** the random state from any point in the stream against the rules you assume, and returns every `(seed, offset, gap path)` that reproduces what you saw, plus odds for what comes next.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/diagram/pipeline-dark.svg">
    <img alt="Seed → std::mt19937 → RULES → Match → Candidates" src="docs/diagram/pipeline-light.svg" width="100%">
  </picture>
</p>

> [!NOTE]
> The RULES are assumptions. Unsure which fit your target? Scan with a few and keep the one whose predictions keep coming true.

| You give it | Example input | It gives you |
| --- | --- | --- |
| One event's first results | `--calib 5,1` | Every matching seed and offset, plus odds for the rest |
| Several events with unknown gaps | `--obs examples/observations.txt` | The few seeds that fit the whole chain, plus a prediction |
| A saved CSV and one more result | `--csv-in candidates.csv --calib 5,1,2` | A smaller CSV, without rescanning |

---

## Quick start

### 1. Requirements

| Requirement | Version | Status |
| --- | --- | --- |
| C++ compiler | C++17 (tested: TDM-GCC 10.3) | **REQUIRED** |
| CMake | ^3.20 | **REQUIRED** |
| MinGW Makefiles generator | ships with TDM-GCC / MinGW-w64 | **REQUIRED** for the presets |
| GPU driver with OpenCL (`OpenCL.dll`) | ^1.2 | OPTIONAL: GPU backend, run time ONLY |
| POSIX shell (Git Bash, MSYS2) | ANY | OPTIONAL: `scripts/parity.sh` ONLY |

> [!TIP]
> No OpenCL SDK is needed to build. The headers are vendored and `OpenCL.dll` loads at start-up, so the same `.exe` falls back to the CPU on machines without a driver.

### 2. Build

```powershell
cmake --preset mingw-release-opencl
cmake --build --preset mingw-release-opencl
.\build\mingw-release-opencl\seedtracker.exe --help
```

<details>
<summary><b>Other presets</b></summary>

<br>

| Preset | Build |
| --- | --- |
| `mingw-release-opencl` | `-O2`, CPU + GPU (default) |
| `mingw-release` | `-O2`, CPU ONLY |
| `mingw-debug` | `-O0 -g`, CPU ONLY |

Other generators: `cmake -S . -B build/custom -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEEDTRACKER_OPENCL=ON`

</details>

### 3. Scan

The first two results were **5**, then **1**:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --seed-end 100000000 --calib 5,1 --depth 3 --csv-out candidates.csv
```

### 4. Narrow

The third was **2**. Re-filter instead of rescanning:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --csv-in candidates.csv --calib 5,1,2 --depth 2 --csv-out narrowed.csv
```

> [!IMPORTANT]
> One event ONLY narrows so far. To reach a handful of seeds, log several events in an [observation log](#observation-logs): each fully known extra event cuts false seeds about 720×.

---

## How it works

| Stage | Flag | Example input | Default |
| --- | --- | --- | --- |
| Mapping: MT word → `rnd()` | `--mapping` | `--mapping reject31` | `std` |
| RULES: `rnd()` → index | `--pick` | `--pick fixed-mod` | `direct` |
| RULES: indices → event | `--model` | `--model shuffle` | `remaining` |
| RULES: draws per pick | `--rng-step` | `--rng-step 2` | `1` |
| Start positions tested | `--offset-step` | `--offset-step 2` | `1` |

Full detail on every mapping, pick rule and model: [docs/CRITERIA.md](docs/CRITERIA.md).

---

## Observation logs

```powershell
.\build\mingw-release-opencl\seedtracker.exe --seed-end 100000000 --offset-end 8 --obs examples/observations.txt
```

Example input, [`examples/observations.txt`](examples/observations.txt):

```text
event 5,1,2,6,3
gap 0..40
event 2,4,1,3,5
gap 10..200
event 3
```

| Line | Example input | Status | Meaning |
| --- | --- | --- | --- |
| `event [LIST]` | `event 5,1,2` | **REQUIRED** | one line per event; LIST is the values seen, `1`..`6`, up to five (a prefix, or none) |
| `include=A,B` | `include=5` | OPTIONAL | values that appear somewhere in the event's five |
| `exclude=A,B` | `exclude=3` | OPTIONAL | values that appear nowhere in the event's five |
| `gap N` / `gap A..B` | `gap 0..40` | OPTIONAL | `rnd()` calls before the next event; no line = 0 |
| `# ...` | `# round 2` | OPTIONAL | comment |

> [!IMPORTANT]
> The **last** event is the one predicted. Max 8 events. `--calib` is a one-event shorthand; use it OR `--obs`, not both.

More logs with known answers, each with its command at the top:

| File | Shows |
| --- | --- |
| [`roll-run.txt`](examples/roll-run.txt) | 13 dice rolls in a row → seed 42 |
| [`roll-daily-gaps.txt`](examples/roll-daily-gaps.txt) | daily rolls with roughly known gaps → seed 42 |
| [`roll-include-exclude.txt`](examples/roll-include-exclude.txt) | a day only remembered as "a 5, no 3" → seed 42 |
| [`elimination-partial.txt`](examples/elimination-partial.txt) | half-seen elimination round with `exclude=` → seed 777, plus one chance match |

---

## Output

```text
matching candidates: 33372

Next depth 1 (elimination #3):
  3 : 25.201% (8410)
  6 : 25.123% (8384)
...
Example surviving candidates:
  seed=40 offset=0 seq=5-1-3-6-4 survivor=2
```

Near-even odds mean "not enough evidence yet". Multi-event runs add **volatility**:

```text
seed=12345 offset=3 gaps=5-10 seq=1-4-3-6-2 survivor=5 volatility=0.000
seed=12772 offset=3 gaps=14-0 seq=1-4-3-6-5 survivor=2 volatility=0.667
```

`0` means the prediction doesn't depend on the unknown last gap; near `1` is a guess. See [volatility](docs/CRITERIA.md#multi-event-matching-and-volatility).

---

## Options

Every flag is OPTIONAL. Without `--calib` or `--obs`, every seed matches and a warning says so. Full text: `seedtracker --help`.

| Flag | Example input | Default | Meaning |
| --- | --- | --- | --- |
| `--calib` | `--calib 5,1` | none | one event's prefix |
| `--obs` | `--obs log.txt` | none | multi-event log |
| `--seed-begin` / `--seed-end` | `--seed-end 100000000` | 0 / 2^20 | seed range, end exclusive |
| `--offset-begin` / `--offset-end` | `--offset-end 8` | 0 / 1 | start positions, end exclusive |
| `--depth` | `--depth 2` | 3 | steps to predict (1–5) |
| `--base-min` / `--base-max` / `--value-step` | `--base-max 100` | 1 / 10000 / 1 | domain for `fixed-*` |
| `--backend` | `--backend cpu` | `auto` | `auto`, `opencl`, `cpu` |
| `--threads` | `--threads 4` | all cores | CPU workers |
| `--gpu-batch` | `--gpu-batch 65536` | auto (~0.4 s) | seeds per GPU launch |
| `--csv-out` / `--csv-in` | `--csv-out c.csv` | | save / re-filter |
| `--examples` | `--examples 3` | 12 | examples printed |
| `--probe-mapping` | `--probe-mapping` | | which mapping `std` equals, then exit |
| `--emit-events` | `--emit-events 12345 3 5,10` | | print a known seed's log |

Stage flags (`--mapping`, `--pick`, `--model`, `--rng-step`, `--offset-step`) are in [How it works](#how-it-works).

---

## GPU backend

| `--backend` | Behaviour |
| --- | --- |
| `auto` | GPU if available and within limits, else CPU with a `note:` on stderr |
| `opencl` | GPU or exit with an error |
| `cpu` | thread pool ONLY |

> [!NOTE]
> The GPU is an exact prefilter: it runs the same `sim_core.h` code and flags matching seeds, then the CPU re-runs them. Results are identical to CPU-only.

Batching, window limits and fallbacks: [docs/CRITERIA.md#gpu-backend](docs/CRITERIA.md#gpu-backend).

---

## CSV

```text
# model=remaining mapping=std pick=direct ... obs=5,1
seed,offset,gap_path,e1,e2,e3,e4,e5,survivor,volatility
```

> [!WARNING]
> `--csv-in` reads each seed's rows as one block. **DON'T sort or shuffle a CSV you'll re-filter.** Input and output must be different files. Format: [docs/CRITERIA.md#csv-format](docs/CRITERIA.md#csv-format).

---

## Verify

**Round trip.** Emit a known seed's log, widen its gaps, scan it back:

```powershell
.\build\mingw-release-opencl\seedtracker.exe --emit-events 12345 3 5,10
```

Set both gaps to `gap 0..20`, cut the last event to `event 1,4`, save as `self.txt`, then run with `--seed-begin 12000 --seed-end 13000 --offset-end 8 --obs self.txt`. Seed `12345` comes back with volatility `0.000`.

**Parity.** `scripts/parity.sh [legacy.exe] [seedtracker.exe]` compares against the original tool (37 cases, needs `build/legacy.exe`) and GPU against CPU (5 cases, skipped without OpenCL).

---

## Project map

<details>
<summary><b>Source layout</b></summary>

<br>

```text
seedtracker/
├── apps/seedtracker/main.cpp          entry point
├── include/seedtracker/core/sim_core.h all RNG logic, C++17 + OpenCL C
├── src/
│   ├── config/                        options, flags, --help
│   ├── rng/                           reference engine, rnd() buffer
│   ├── rules/                         RULES wrappers
│   ├── post/                          per-event checks
│   ├── logs/                          observation logs
│   ├── compute/                       matching, CPU pool, OpenCL + kernels/scan.cl
│   └── export/                        CSV and report
├── scripts/parity.sh                  parity checks
└── examples/observations.txt          sample log
```

</details>

---

## References

- Inspired by rAthena's [`random.hpp`](third_party/rathena/src/common/random.hpp) / [`random.cpp`](third_party/rathena/src/common/random.cpp) (sparse submodule).
- [`std::mt19937` reference](https://cplusplus.com/reference/random/mt19937/)

## License

The vendored OpenCL headers (`third_party/OpenCL-Headers`) are Apache-2.0. The rest of the project has no license file yet.
