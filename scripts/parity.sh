#!/usr/bin/env bash
# Legacy parity: run the legacy binary and the new one with the same flags and
# compare the surviving rows and the "matching candidates" count.
#
#   scripts/parity.sh [legacy.exe] [seedtracker.exe]
#
# Section 1 (legacy): the legacy binary against the new one with `--backend cpu`.
# Section 2 (gpu):    the new binary with `--backend opencl` against `--backend cpu`
#                     (sorted CSV rows and stdout minus the backend line). Skipped
#                     with a message when the binary has no OpenCL or no GPU
#                     initialises. GPU_SEED_END (default 16777216) sets its seed range.
#
# Defaults: build/legacy.exe and build/mingw-release-opencl/seedtracker.exe (or
# build/mingw-release/seedtracker.exe when the OpenCL build is absent), relative
# to the repo root. Build the legacy binary first from the original single-file
# tool, which is not part of this repo:
#   g++ -O2 -std=c++17 path/to/sakray_mt_candidate_filter_latest.cpp -o build/legacy.exe
#
# The new CSV has two extra columns (gap_path, volatility). They are dropped
# before the diff, as are '#' metadata lines and the header, so only
# seed,offset,e1..e5,survivor are compared. Rows are sorted because the thread
# split differs between the two tools.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LEGACY="${1:-$ROOT/build/legacy.exe}"
if [ -x "$ROOT/build/mingw-release-opencl/seedtracker.exe" ]; then
    DEFAULT_NEW="$ROOT/build/mingw-release-opencl/seedtracker.exe"
else
    DEFAULT_NEW="$ROOT/build/mingw-release/seedtracker.exe"
fi
NEW="${2:-$DEFAULT_NEW}"
GPU_SEED_END="${GPU_SEED_END:-16777216}"
OUT="$ROOT/build/parity"
mkdir -p "$OUT"
export LC_ALL=C

for exe in "$LEGACY" "$NEW"; do
    if [ ! -x "$exe" ]; then
        echo "error: executable not found: $exe" >&2
        exit 2
    fi
done

fail=0
total=0

count_of() { grep -m1 '^matching candidates:' "$1" | sed 's/.*: //'; }

run_case() {
    local name="$1"; shift
    local args=("$@")
    total=$((total + 1))
    local why=""

    "$LEGACY" "${args[@]}" --csv-out "$OUT/$name.legacy.csv" > "$OUT/$name.legacy.out" 2>/dev/null         || why="legacy exited non-zero"
    "$NEW" --backend cpu "${args[@]}" --csv-out "$OUT/$name.new.csv" > "$OUT/$name.new.out" 2>"$OUT/$name.new.err"         || why="${why:+$why; }new exited non-zero"

    grep -v -e '^seed,' "$OUT/$name.legacy.csv" 2>/dev/null | sort > "$OUT/$name.legacy.rows"
    grep -v -e '^#' -e '^seed,' "$OUT/$name.new.csv" 2>/dev/null | cut -d, -f1,2,4-9 | sort > "$OUT/$name.new.rows"

    local lc nc
    lc="$(count_of "$OUT/$name.legacy.out")"
    nc="$(count_of "$OUT/$name.new.out")"
    if ! [[ "$lc" =~ ^[0-9]+$ ]] || ! [[ "$nc" =~ ^[0-9]+$ ]]; then
        why="${why:+$why; }count missing or not a number"
    elif [ "$lc" != "$nc" ]; then
        why="${why:+$why; }counts differ"
    elif [ "$lc" != "0" ] && { [ ! -s "$OUT/$name.legacy.rows" ] || [ ! -s "$OUT/$name.new.rows" ]; }; then
        why="${why:+$why; }empty row file with nonzero count"
    fi
    if [ -z "$why" ] && ! cmp -s "$OUT/$name.legacy.rows" "$OUT/$name.new.rows"; then
        why="rows differ"
    fi

    local result="identical"
    if [ -n "$why" ]; then
        result="DIFFERENT ($why)"
        fail=$((fail + 1))
    fi
    local invalid
    invalid="$(grep -m1 '^invalid events' "$OUT/$name.new.out" | sed 's/^[^:]*: *\([0-9]*\).*/\1/')"
    printf '%-34s legacy=%-8s new=%-8s %s%s\n' "$name" "$lc" "$nc" "$result" \
        "${invalid:+  (invalid events: $invalid)}"
}

for model in remaining reroll swapremove shuffle; do
    for mapping in high31 reject31 std; do
        for pick in direct fixed-mod fixed-scale; do
            run_case "$model.$mapping.$pick" \
                --seed-end 200000 --calib 5,1 --model "$model" --mapping "$mapping" --pick "$pick"
        done
    done
done

run_case "steps.offset1-50.rng2" \
    --seed-end 200000 --offset-begin 1 --offset-end 50 --offset-step 2 --rng-step 2 --calib 4,3

echo "----"
echo "legacy: $((total - fail))/$total runs identical"
legacy_fail=$fail

# ---- Section 2: GPU (--backend opencl) against CPU (--backend cpu) ----------

gpu_total=0
gpu_fail=0

# Both runs use the same flags; compares every CSV column (rows sorted, since the
# two backends write in different orders) and stdout without the "backend" line.
run_gpu_case() {
    local name="$1"; shift
    local args=("$@")
    gpu_total=$((gpu_total + 1))
    local why=""
    for b in cpu opencl; do
        "$NEW" --backend "$b" "${args[@]}" --csv-out "$OUT/gpu.$name.$b.csv"             > "$OUT/gpu.$name.$b.out" 2> "$OUT/gpu.$name.$b.err" || why="${why:+$why; }$b exited non-zero"
        grep -v -e '^#' -e '^seed,' "$OUT/gpu.$name.$b.csv" 2>/dev/null | sort > "$OUT/gpu.$name.$b.rows"
        grep -v '^backend ' "$OUT/gpu.$name.$b.out" > "$OUT/gpu.$name.$b.stdout"
    done
    grep -q '^backend    : opencl' "$OUT/gpu.$name.opencl.out" || why="${why:+$why; }opencl run did not use the GPU"
    cmp -s "$OUT/gpu.$name.cpu.rows" "$OUT/gpu.$name.opencl.rows" || why="${why:+$why; }csv rows differ"
    cmp -s "$OUT/gpu.$name.cpu.stdout" "$OUT/gpu.$name.opencl.stdout" || why="${why:+$why; }stdout differs"
    local rows
    rows="$(wc -l < "$OUT/gpu.$name.cpu.rows" | tr -d ' ')"
    local result="identical"
    if [ -n "$why" ]; then
        result="DIFFERENT ($why)"
        gpu_fail=$((gpu_fail + 1))
    fi
    printf '%-34s rows=%-8s %s' "$name" "$rows" "$result"
    echo
}

echo
echo "== GPU vs CPU (seed-end $GPU_SEED_END) =="
probe_err="$("$NEW" --backend opencl --seed-end 1 --calib 5,1 2>&1 >/dev/null | head -n1)"
if "$NEW" --backend opencl --seed-end 1 --calib 5,1 >/dev/null 2>&1; then
    # Case b: a two-event log from a known seed, gap widened, last event cut to 2 values.
    OBS_B="$OUT/gpu.obs_b.txt"
    "$NEW" --backend cpu --emit-events 123456 3 17 | tr -d '\r' | awk '
        { line[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) {
                if (line[i] ~ /^gap/) { print "gap 0..40"; continue }
                if (i == NR) {
                    split(substr(line[i], 7), v, ",")
                    print "event " v[1] "," v[2]; continue
                }
                print line[i]
            }
        }' > "$OBS_B"

    run_gpu_case "a.calib51"          --seed-end "$GPU_SEED_END" --calib 5,1
    run_gpu_case "b.obs-two-events"   --seed-end "$GPU_SEED_END" --offset-end 8 --obs "$OBS_B"
    run_gpu_case "c.reroll.high31"    --seed-end "$GPU_SEED_END" --model reroll --mapping high31 --calib 5,1
    run_gpu_case "d.dist-gcc10"       --seed-end "$GPU_SEED_END" --pick dist-gcc10 --calib 5,1
    run_gpu_case "e.dist-gcc11"       --seed-end "$GPU_SEED_END" --pick dist-gcc11 --calib 5,1
    if ! grep -q '^123456,3,' "$OUT/gpu.b.obs-two-events.opencl.csv"; then
        echo "note: case b did not recover the generating seed 123456 at offset 3 (offset-end 8)"
        gpu_fail=$((gpu_fail + 1))
    fi
    echo "----"
    echo "gpu: $((gpu_total - gpu_fail))/$gpu_total runs identical"
else
    echo "skipped: this binary cannot run --backend opencl (${probe_err:-no output})"
fi

[ "$legacy_fail" -eq 0 ] && [ "$gpu_fail" -eq 0 ]
