#!/usr/bin/env bash
# Linux-only host sanitizer gate. It deliberately fails instead of skipping when the compiler,
# sanitizer runtime, CMake target, or leak detector is unavailable: a green job must mean all
# detectors actually executed over every declared host boundary target.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

mode=run
if [ "${1:-}" = --self-test ]; then mode=self-test; shift; fi
if [ "$#" -ne 0 ]; then
    echo "usage: scripts/run-sanitizer-tests.sh [--self-test]" >&2
    exit 2
fi
if [ "$(uname -s)" != Linux ]; then
    echo "sanitizer-gate: Linux is required for the CI ASan/UBSan/LSan contract" >&2
    exit 2
fi
command -v cmake >/dev/null 2>&1 || { echo "sanitizer-gate: cmake is required" >&2; exit 2; }

can_link_sanitizers() {
    local candidate="$1"
    printf 'int main(){}\n' | "$candidate" -fsanitize=address,undefined,leak -x c++ - -o /dev/null >/dev/null 2>&1
}

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    for candidate in clang++ g++; do
        if command -v "$candidate" >/dev/null 2>&1 && can_link_sanitizers "$candidate"; then
            CXX="$candidate"
            break
        fi
    done
    if [ -z "$CXX" ]; then
        if command -v clang++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1; then
            echo "sanitizer-gate: clang++ or g++ found, but sanitizer runtimes (-fsanitize=address,undefined,leak) failed to link" >&2
            exit 2
        else
            echo "sanitizer-gate: clang++ or g++ is required" >&2
            exit 2
        fi
    fi
else
    command -v "$CXX" >/dev/null 2>&1 || {
        echo "sanitizer-gate: configured compiler is unavailable: $CXX" >&2
        exit 2
    }
    if ! can_link_sanitizers "$CXX"; then
        echo "sanitizer-gate: configured compiler '$CXX' cannot link sanitizer runtimes (-fsanitize=address,undefined,leak)" >&2
        exit 2
    fi
fi

# CMake must not combine Clang's C++ ASan with a default GCC C runtime when a native boundary
# includes the pinned SDK's C query fixture. An explicit CC remains subject to the mixed probe.
CC="${CC:-}"
if [ -z "$CC" ]; then
    case "${CXX##*/}" in
        *clang++*) CC="${CXX/clang++/clang}" ;;
        *g++*) CC="${CXX/g++/gcc}" ;;
        *) echo "sanitizer-gate: set CC to the C compiler matching configured CXX '$CXX'" >&2; exit 2 ;;
    esac
fi
command -v "$CC" >/dev/null 2>&1 || {
    echo "sanitizer-gate: configured C compiler is unavailable: $CC" >&2
    exit 2
}

work="$(mktemp -d "${TMPDIR:-/tmp}/tesla-key-sanitizers.XXXXXX")"
trap 'rm -rf "$work"' EXIT

flags=(-std=c++17 -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all
       -fsanitize=address,undefined,leak)
c_flags=(-std=c11 "${flags[@]:1}")
asan_options="detect_leaks=1:halt_on_error=1:abort_on_error=1:strict_string_checks=1"
ubsan_options="halt_on_error=1:print_stacktrace=1"
lsan_options="exitcode=97"

compile_one() {
    local source="$1" output="$2"
    "$CXX" "${flags[@]}" "$source" -o "$output"
}

expect_detector_failure() {
    local label="$1" source="$2" needle="$3" output rc
    local binary="$work/$label"
    compile_one "$source" "$binary"
    set +e
    output="$(ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
        LSAN_OPTIONS="$lsan_options" "$binary" 2>&1)"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ] || ! printf '%s' "$output" | grep -Eq "$needle"; then
        echo "sanitizer-gate: $label detector tripwire was not caught (rc=$rc)" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi
}

if [ "$mode" = self-test ]; then
    mkdir -p "$work/mixed"
    printf '%s\n' 'cmake_minimum_required(VERSION 3.16)' \
        'project(sanitizer_mixed_probe LANGUAGES C CXX)' \
        'add_executable(mixed probe.c main.cpp)' > "$work/mixed/CMakeLists.txt"
    printf '%s\n' 'int c_probe(void){return 0;}' > "$work/mixed/probe.c"
    printf '%s\n' 'extern "C" int c_probe(void);' \
        'int main(){return c_probe();}' > "$work/mixed/main.cpp"
    cmake -S "$work/mixed" -B "$work/mixed-build" \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="${c_flags[*]}" -DCMAKE_CXX_FLAGS="${flags[*]}" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined,leak" >/dev/null
    cmake --build "$work/mixed-build" --parallel >/dev/null
    ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
        LSAN_OPTIONS="$lsan_options" "$work/mixed-build/mixed"

    printf '%s\n' '#include <stdlib.h>' \
        'int c_probe(void){int* p=malloc(sizeof(int)); free(p); volatile int* q=p; q[0]=7; return 0;}' \
        > "$work/mixed/probe.c"
    cmake --build "$work/mixed-build" --parallel >/dev/null
    set +e
    mixed_output="$(ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
        LSAN_OPTIONS="$lsan_options" "$work/mixed-build/mixed" 2>&1)"
    mixed_rc=$?
    set -e
    if [ "$mixed_rc" -eq 0 ] || ! printf '%s' "$mixed_output" | grep -q 'heap-use-after-free'; then
        echo "sanitizer-gate: mixed C/C++ ASan tripwire was not caught (rc=$mixed_rc)" >&2
        printf '%s\n' "$mixed_output" >&2
        exit 1
    fi

    printf '%s\n' 'int main(){return 0;}' >"$work/clean.cpp"
    compile_one "$work/clean.cpp" "$work/clean"
    ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
        LSAN_OPTIONS="$lsan_options" "$work/clean"

    printf '%s\n' '#include <cstdlib>' \
        'int main(){int* p=new int[1]; delete[] p; volatile int* q=p; q[0]=7;}' \
        >"$work/asan.cpp"
    expect_detector_failure asan "$work/asan.cpp" 'AddressSanitizer|heap-buffer-overflow'

    printf '%s\n' '#include <climits>' \
        'int main(){volatile int x=INT_MAX; volatile int y=x+1; return y;}' >"$work/ubsan.cpp"
    expect_detector_failure ubsan "$work/ubsan.cpp" 'runtime error|UndefinedBehaviorSanitizer'

    printf '%s\n' '#include <cstdlib>' \
        'void* volatile sink;' \
        'int main(){sink=std::malloc(64); sink=nullptr; return 0;}' >"$work/lsan.cpp"
    expect_detector_failure lsan "$work/lsan.cpp" 'LeakSanitizer|detected memory leaks'

    echo "sanitizer-gate self-test: PASS (mixed C/C++ runtime; ASan, UBSan, LSan tripwires detected)"
    exit 0
fi

cmake -S test -B "$work/build" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_CXX_FLAGS="${flags[*]}" \
    -DCMAKE_C_FLAGS="${c_flags[*]}" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined,leak" >/dev/null
cmake --build "$work/build" --parallel \
    --target logic_tests nvs_storage_tests runtime_boundary_tests clock_runtime_tests config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests heap_supervisor_tests

for target in logic_tests nvs_storage_tests runtime_boundary_tests clock_runtime_tests config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests heap_supervisor_tests; do
    binary="$work/build/$target"
    if [ ! -x "$binary" ]; then
        echo "sanitizer-gate: declared target did not produce an executable: $target" >&2
        exit 1
    fi
    ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
        LSAN_OPTIONS="$lsan_options" "$binary"
done

"$CXX" "${flags[@]}" -Wall -Wextra -Werror -Imain test/fuzz_smoke.cpp \
    -o "$work/fuzz_smoke"
ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
    LSAN_OPTIONS="$lsan_options" "$work/fuzz_smoke" \
    --seed 0x5445534c414b4559 --iterations 20000

ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
    LSAN_OPTIONS="$lsan_options" CXX="$CXX" python3 test/test_led_driver_contract.py --sanitize

ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \
    LSAN_OPTIONS="$lsan_options" CXX="$CXX" python3 test/test_display_parity_contract.py --render-only --sanitize

echo "sanitizer-gate: PASS (logic, NVS, runtime boundaries, clock/config shells, pinned SDK query, deterministic fuzz)"
