# How it works

[← Back to README](../README.md)

Each candidate seed is simulated from the start and compared with what you observed. One mapping plus the RULES decide how a raw MT19937 word becomes an event.

<table>
<tr><th width="18%">Stage</th><th width="22%">Flag</th><th>What it decides</th></tr>
<tr><td><b>1. Mapping</b></td><td><code>--mapping</code></td><td>How one 32-bit MT word becomes one <code>rnd()</code> value.</td></tr>
<tr><td><b>2. RULES: pick rule</b></td><td><code>--pick</code></td><td>How one <code>rnd()</code> value becomes an index among the choices left.</td></tr>
<tr><td><b>3. RULES: model</b></td><td><code>--model</code></td><td>How those indices become the event.</td></tr>
<tr><td><b>4. RULES: stepping</b></td><td><code>--rng-step</code>, <code>--offset-step</code></td><td>Whether other code consumes draws in between, and which start positions to test.</td></tr>
</table>

---

## 1. Mapping

`--mapping`, default `std`.

| Value | `rnd()` is |
| --- | --- |
| `high31` | `w >> 1` |
| `low31` | `w & 0x7fffffff` |
| `reject31` | `w`, redrawn until `w <= 0x7fffffff` |
| `std` | `std::uniform_int_distribution<int32_t>(0, INT32_MAX)` from **your** standard library |

> [!WARNING]
> `std` is ONLY as portable as the standard library you built with. On libstdc++ 10 it rejects draws at or above 2^31, so it equals `reject31`. Newer libstdc++ versions use a different method. Run `--probe-mapping` to see which explicit mapping `std` matches on your build, and pick the explicit one that matches the **target's** build.

```console
$ seedtracker --probe-mapping
std == reject31
probed 5 seeds x 100000 draws
libstdc++ release: 10
```

---

## 2. Pick rules

`--pick`, default `direct`.

| Value | Index is |
| --- | --- |
| `direct` | `rnd() % choices` |
| `fixed-mod` | `rnd()` reduced into a fixed domain (`--base-min`..`--base-max`, stepping by `--value-step`); the value's offset from `--base-min` is then taken `% choices` (the offset, not the value itself: `--base-min` only sets the domain size) |
| `fixed-scale` | Same fixed domain, but the value's position is scaled into equal buckets, one per choice |
| `dist-gcc10` | `std::uniform_int_distribution<int>(0, choices-1)` as libstdc++ 10 implements it: redraw a raw word until it is below `choices * (0xFFFFFFFF / choices)`, then divide |
| `dist-gcc11` | The same distribution as libstdc++ 11 implements it (Lemire's method): `product = (u64)word * choices`, redraw while the low 32 bits are below `(-choices) % choices`, index is `product >> 32` |
| `custom` | User-defined in `include/seedtracker/core/custom_rule.h`. The default stub behaves like `direct`. See [Custom rules](#custom-rules). |

The fixed-domain defaults are `--base-min 1 --base-max 10000 --value-step 1`, which models a call like `rnd_value(1, 10000)`.

> [!NOTE]
> `fixed-mod` takes `(rnd() % domain_count) * value_step`, then `% choices`. `--base-min` is not added before the modulo; it only sets the domain size. This is inherited behaviour from the legacy tool and is kept on purpose. A target that does `rand(min, max) % choices` with `min` not a multiple of `choices` needs `--pick custom` (a ready example is commented out in `custom_rule.h`).

> [!CAUTION]
> `dist-gcc10` and `dist-gcc11` read **raw MT words**, NOT `rnd()` values. `--mapping` is ignored (a warning is printed), and offsets, gaps and `--rng-step` count engine words. Every word read, including a rejected one, advances by `--rng-step`. More than 64 words in one pick marks the event invalid.

**How they were checked.** `dist-gcc10` was compared bit-for-bit against the local libstdc++ 10.3 for 2 to 6 choices. `dist-gcc11` is ported from `uniform_int_distribution::operator()` and `_S_nd` in libstdc++ 11's `bits/uniform_int_dist.h` (32-bit generator branch). A scratch program transcribed that function from source and matched the port on 38 million picks for 2 to 6 choices, including every redraw-triggering word and `--rng-step` 1 and 3. It was NOT compared against a compiled libstdc++ 11, because none was available. The GPL-licensed libstdc++ 11 header is not vendored.

---

## 3. Models

`--model`, default `remaining`.

| Value | Behaviour |
| --- | --- |
| `remaining` | Keep a list of open choices. Erase the picked one; the rest keep their order. |
| `swapremove` | Same, but the last choice moves into the picked slot. |
| `reroll` | Pick from all six; redraw until the choice is still open. Capped at 64 picks per event, after which the event counts as invalid. |
| `shuffle` | Fisher-Yates shuffle of all six. The first five are the order. |
| `roll` | Independent picks from all six (`rand() % 6 + 1`); values repeat, no survivor. Log a long run as 5-value `event` lines with no `gap` between them. |
| `custom` | User-defined in `include/seedtracker/core/custom_rule.h`. The default stub behaves like `remaining`. See [Custom rules](#custom-rules). |

---

## Custom rules

`--pick custom` and `--model custom` run code you write in `include/seedtracker/core/custom_rule.h`. Edit the file and rebuild; nothing else needs changing. The CPU scan and the GPU kernel both compile that file (the GPU build embeds its text), so both backends use your rule.

- `sf_custom_pick` turns one draw into an index in `[0, choices)`. The default behaves like `direct`.
- `sf_custom_event` produces one whole event (five eliminations and a survivor). The default behaves like `remaining`. It can call `sf_pick`, which uses `sf_custom_pick` when `--pick custom` is set, so the two compose.

Rules for the code:

1. Take random draws only through the draw buffer: `buf[*pos]` in a pick, `sf_pick` in an event.
2. Before reading `buf[*pos]`, check `*pos >= W` and, if so, set `*ok = 0` and return 0. After reading, advance `*pos += p->rng_step`.
3. Cap every loop, and when a cap is hit leave `out->valid` unset. No recursion, heap, `double` or global state: the file is plain C that must also compile as OpenCL C 1.2.
4. Fill `sf_event` with rooms in 1..6 (`elim[5]`, `survivor`), set `valid = 1` and `draws_used`. Put constants in the file as `#define SF_CUSTOM_...`; there are no run-time arguments for them.
5. Set `SF_CUSTOM_MAX_PICKS` to the most picks one custom event can make (default 5). Each seed's draw window is sized from it, so a value that is too low drops valid events as invalid without any error.
6. Set `SF_CUSTOM_ELIMINATES` to 0 if the custom model is roll-like (values repeat, no survivor). It is 1 by default.

To verify a change: run the same scan with `--backend cpu` and `--backend opencl` (use a calibration that leaves few candidates, such as `--seed-end 16777216 --calib 5,1 --depth 2`) and compare the sorted CSV rows and the `matching candidates` line. Also check the events against a plain C++ reference of the target's logic (`--emit-events` prints a synthetic log for one seed).

---

## 4. Stepping

- `--rng-step N` makes every pick consume N draws: the one it uses, then N-1 skipped. Use it when other code draws from the same stream between picks.
- `--offset-step N` tests start positions `offset-begin`, `offset-begin + N`, and so on, up to `offset-end`.

Both accept 1 to 2^20.

---

## Multi-event matching and volatility

With `--obs`, every start offset and every value inside each `gap A..B` range is tried. A seed survives when one path through those gaps matches every logged event.

The gap before the **last** event is unknown, so several gap values can match the logged prefix and still predict different endings. **Volatility** measures that:

```text
volatility = 1 - (count of the most common prediction) / (number of matching gaps)
```

| Volatility | Meaning |
| --- | --- |
| `0.000` | Every matching gap predicts the same ending. Trust it. |
| close to `1` | The ending depends on a gap you didn't observe. Treat it as a guess. |

A one-event run has no gap, so its volatility is always 0. The report lists the lowest-volatility survivors and a continuation distribution where each survivor votes with weight `1 - volatility`.

---

## GPU backend

`--backend auto|opencl|cpu` picks where the scan runs.

| Value | Behaviour |
| --- | --- |
| `auto` *(default)* | GPU when the build has OpenCL, a device initialises, and the options fit the GPU limits. Otherwise CPU, with one line on stderr: `note: OpenCL unavailable (<reason>), using CPU backend`. |
| `opencl` | GPU or exit with `error: --backend opencl: <reason>`. Never falls back. |
| `cpu` | Thread pool ONLY. Never touches the GPU. |

The report's first line names what actually ran, for example `backend : opencl (NVIDIA GeForce GTX 1650)` or `backend : cpu (8 threads)`.

The GPU is an **exact prefilter**, NOT a second implementation. One work-item runs per seed: it fills that seed's draw window and walks the whole chain (all offsets, all gap values, all events) with the same `sim_core.h` functions the CPU uses. Seeds that match every event go into a hit buffer. The CPU then re-runs exactly those seeds, so every number, CSV row and prediction comes from the CPU code.

### What runs on the GPU

- Every mapping except `std` itself. `--mapping std` runs as whichever explicit mapping `--probe-mapping` reports (`reject31` on TDM-GCC 10.3), with a warning.
- Every pick rule and every model.
- Logs of up to 8 events.

The device is a discrete GPU if one exists, otherwise the integrated one. Ties break on compute units times clock speed.

### Batching and the display watchdog

Seeds go to the GPU in batches while the CPU re-checks the previous batch. By default each launch is timed and resized to about 0.4 s: the first launch is 2^16 seeds, and it grows to at most 2^24. That keeps every launch well under the Windows display watchdog (about 2 s), which resets a GPU stuck on one long launch.

If a batch produces more hits than the hit buffer holds (2^22), it is rerun as two halves. No hit is ever dropped.

### Window limits

To stay fast, the kernel computes draws directly from MT19937's first initialisation words instead of keeping the full 624-word state per seed. That ONLY works for a short window of draws:

| Mapping or pick | Max draws in the window |
| --- | --- |
| `high31`, `low31`, raw words (`dist-*`) | 226 |
| `reject31` | 96 (about half its words are rejected) |

The window is roughly `offset-end + the sum of later gap maxima + events × draws per event`. In practice the limit bites on large `--offset-end` values and wide gaps. Over the limit, `auto` falls back to the CPU with a note, and `opencl` fails.

> [!TIP]
> For large offsets, use `--backend cpu`. Keeping the full state on the GPU was measured at 2.3× to 3× **slower** than 8 CPU threads on an Intel UHD Graphics (23 compute units).

The kernel also fills a shorter window than the CPU's worst case: 5 picks per event (24 for `reroll`) times `--rng-step`, plus offsets and gaps. If a seed's event runs past that, the GPU hands the seed to the CPU to settle, including its invalid-event count. With the full window, `dist-gcc10` over 16M seeds took 59 s; with the short window it took 0.7 s.

The limits live in `include/seedtracker/compute/opencl_scanner.hpp`.

---

## CSV format

```text
# model=remaining mapping=std pick=direct base_min=1 base_max=10000 value_step=1 offset_step=1 rng_step=1 obs=5,1
seed,offset,gap_path,e1,e2,e3,e4,e5,survivor,volatility
```

`e1`..`e5` and `survivor` describe the **predicted** (last) event. The `#` line records the settings the file was made with.

- `--csv-in` reads rows in groups and recomputes volatility one group at a time. A scan writes each seed's rows as one block, in ascending seed order, so DON'T sort or shuffle a CSV you plan to re-filter.
- `--csv-in` and `--csv-out` must be different files.
- `#` lines are skipped. Out-of-range rows are rejected with their line number.
- A warning is printed when the file's metadata differs from the current flags.
- The legacy 8-column layout (no `gap_path`, no `volatility`) is also accepted.
