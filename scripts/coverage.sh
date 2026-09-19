#!/usr/bin/env bash
# Line-coverage report for the engine (design section 5, target > 85%).
# Requires gcovr: pip install --user gcovr
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-cov"
threshold="${COVERAGE_THRESHOLD:-85}"

if ! python3 -m gcovr --version >/dev/null 2>&1; then
  echo "gcovr not found: pip install --user gcovr" >&2
  exit 1
fi

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTINYLSM_COVERAGE=ON >/dev/null
cmake --build "$build" -j "$(nproc)"
ctest --test-dir "$build" -j 2 --output-on-failure >/dev/null
echo "ran $(ctest --test-dir "$build" -N 2>/dev/null | grep -c 'Test #') test binaries under coverage instrumentation"

mkdir -p "$root/reports"
set +e
# --gcov-ignore-parse-errors works around GCC bug 68080 (negative hit counts
# on headers with inline control flow), which otherwise aborts the report on
# GCC 13+. It only downgrades parser errors to warnings; coverage is unaffected.
python3 -m gcovr -r "$root" --object-directory "$build" \
  --filter 'src/' --filter 'include/' --txt --fail-under-line "$threshold" \
  --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
  | tee "$root/reports/coverage.txt"
status=${PIPESTATUS[0]}
set -e
if [ "$status" -ne 0 ]; then
  echo "coverage below the ${threshold}% target (see reports/coverage.txt)" >&2
  exit 1
fi
echo "coverage gate: >= ${threshold}% line coverage (reports/coverage.txt)"