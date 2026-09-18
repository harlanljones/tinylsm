#!/usr/bin/env bash
# Builds the WebAssembly bundle and stages it next to the site (design section 7).
# Requires the Emscripten SDK; set EMSDK or install it at ~/toolchains/emsdk.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v emcmake >/dev/null 2>&1; then
  for candidate in "${EMSDK:-}" "$HOME/toolchains/emsdk" /opt/emsdk; do
    if [ -n "$candidate" ] && [ -f "$candidate/emsdk_env.sh" ]; then
      # shellcheck disable=SC1090
      source "$candidate/emsdk_env.sh" >/dev/null 2>&1
      break
    fi
  done
fi
if ! command -v emcmake >/dev/null 2>&1; then
  echo "emcmake not found: install the Emscripten SDK and re-run (see wasm/README.md)." >&2
  exit 1
fi

emcmake cmake -S "$root/wasm" -B "$root/build-wasm" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$root/build-wasm"
mkdir -p "$root/wasm/site"
cp "$root/build-wasm/tinylsm_wasm.js" "$root/build-wasm/tinylsm_wasm.wasm" "$root/wasm/site/"
if [ -f "$root/build-release/benchmark.jsonl" ]; then
  cp "$root/build-release/benchmark.jsonl" "$root/wasm/site/benchmark.jsonl"
fi
echo "Staged wasm/site/tinylsm_wasm.{js,wasm}"
