#!/bin/bash
#
# run-full-benchmark.sh — Run correctness + performance tests across write ratios
#
# Usage:
#   ./run-full-benchmark.sh [glibc_build_dir]
#
# If glibc_build_dir is provided, runs both system and modified glibc.
# Otherwise, runs only with the system glibc.
#
set -e

GLIBC_BUILD="${1:-}"
DURATION=2
MAX_THREADS=$(nproc)
THREAD_COUNTS="1 2 4 8 16 32 64 96 128 160"
WRITE_PCTS="0 1 5 10 25 50"

# Build all benchmarks
echo "=== Building benchmarks ==="
gcc -O2 -pthread -o mutex-to-rwlock-bench mutex-to-rwlock-bench.c
gcc -O2 -pthread -o rdunlock-stress rdunlock-stress.c
gcc -O2 -pthread -o rwlock-correctness-stress rwlock-correctness-stress.c
echo "Done."
echo ""

run_with_glibc() {
    local label="$1"
    shift
    if [ -n "$GLIBC_BUILD" ] && [ "$label" = "modified" ]; then
        "$GLIBC_BUILD/elf/ld-linux-x86-64.so.2" \
            --library-path "$GLIBC_BUILD:$GLIBC_BUILD/nptl" "$@"
    else
        "$@"
    fi
}

# ===========================================================================
# Part 1: Correctness tests
# ===========================================================================
echo "============================================"
echo "  PART 1: CORRECTNESS STRESS TESTS"
echo "============================================"
echo ""

correctness_pass=true

for write_pct in $WRITE_PCTS; do
    for label in system modified; do
        if [ "$label" = "modified" ] && [ -z "$GLIBC_BUILD" ]; then
            continue
        fi
        echo "--- [$label glibc] threads=$MAX_THREADS write_pct=${write_pct}% duration=${DURATION}s ---"
        if ! run_with_glibc "$label" ./rwlock-correctness-stress "$MAX_THREADS" "$DURATION" "$write_pct"; then
            correctness_pass=false
            echo "FAILED!"
        fi
        echo ""
    done
done

if [ "$correctness_pass" = true ]; then
    echo ">>> ALL CORRECTNESS TESTS PASSED <<<"
else
    echo ">>> CORRECTNESS FAILURES DETECTED <<<"
    exit 1
fi
echo ""

# ===========================================================================
# Part 2: Performance — rdunlock stress (minimal critical section)
# ===========================================================================
echo "============================================"
echo "  PART 2: RDUNLOCK STRESS (pure readers)"
echo "============================================"
echo ""

for label in system modified; do
    if [ "$label" = "modified" ] && [ -z "$GLIBC_BUILD" ]; then
        continue
    fi
    echo "--- [$label glibc] ---"
    run_with_glibc "$label" ./rdunlock-stress "$MAX_THREADS" "$DURATION"
    echo ""
done

# ===========================================================================
# Part 3: Performance — varied write ratios
# ===========================================================================
echo "============================================"
echo "  PART 3: WRITE RATIO SCALING"
echo "============================================"
echo ""

for write_pct in $WRITE_PCTS; do
    echo "--- write_pct = ${write_pct}% ---"
    printf "%6s" "threads"
    [ -n "$GLIBC_BUILD" ] && printf "  %15s  %15s  %8s" "system(Mops/s)" "modified(Mops/s)" "speedup"
    [ -z "$GLIBC_BUILD" ] && printf "  %15s" "rwlock(Mops/s)"
    echo ""
    printf "%6s" "------"
    [ -n "$GLIBC_BUILD" ] && printf "  %15s  %15s  %8s" "---------------" "---------------" "--------"
    [ -z "$GLIBC_BUILD" ] && printf "  %15s" "---------------"
    echo ""

    for t in $THREAD_COUNTS; do
        if [ "$t" -gt "$MAX_THREADS" ]; then
            break
        fi

        sys_ops=$(run_with_glibc "system" ./mutex-to-rwlock-bench "$t" "$DURATION" "$write_pct" rwlock-ops)

        if [ -n "$GLIBC_BUILD" ]; then
            mod_ops=$(run_with_glibc "modified" ./mutex-to-rwlock-bench "$t" "$DURATION" "$write_pct" rwlock-ops)
            speedup=$(echo "scale=2; $mod_ops / $sys_ops" | bc 2>/dev/null || echo "N/A")
            printf "%6d  %15.2f  %15.2f  %7sx\n" "$t" \
                "$(echo "scale=2; $sys_ops / 1000000" | bc)" \
                "$(echo "scale=2; $mod_ops / 1000000" | bc)" \
                "$speedup"
        else
            printf "%6d  %15.2f\n" "$t" \
                "$(echo "scale=2; $sys_ops / 1000000" | bc)"
        fi
    done
    echo ""
done

echo "============================================"
echo "  BENCHMARK COMPLETE"
echo "============================================"
