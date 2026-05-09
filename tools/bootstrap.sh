#!/usr/bin/env bash
# Idempotent setup for a fresh checkout on macOS / Linux.
# - Clones vcpkg into external/vcpkg if missing
# - Bootstraps the vcpkg binary
# - Pins a builtin-baseline in vcpkg.json the first time

set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p external

if [[ ! -d external/vcpkg/.git ]]; then
    echo "[1/3] Cloning vcpkg into external/vcpkg (full history — needed for versioning)..."
    git clone https://github.com/microsoft/vcpkg.git external/vcpkg
else
    echo "[1/3] external/vcpkg already present — checking it has full history"
    if [[ "$(git -C external/vcpkg rev-parse --is-shallow-repository)" == "true" ]]; then
        echo "       (shallow clone detected — fetching full history)"
        git -C external/vcpkg fetch --unshallow
    fi
fi

if [[ ! -x external/vcpkg/vcpkg ]]; then
    echo "[2/3] Bootstrapping vcpkg..."
    external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
else
    echo "[2/3] vcpkg binary already built — skipping"
fi

if ! grep -q '"builtin-baseline"' vcpkg.json; then
    echo "[3/3] Pinning vcpkg builtin-baseline for reproducibility..."
    external/vcpkg/vcpkg x-update-baseline --add-initial-baseline
else
    echo "[3/3] builtin-baseline already pinned"
fi

cat <<'NOTE'

----------------------------------------------------------------
Setup complete. Next:

    cmake --preset linux-release       # or macos-release
    cmake --build --preset linux-release -j
    ctest --preset linux-release

CMakePresets.json points the toolchain at external/vcpkg, so no
VCPKG_ROOT environment variable is needed.

First configure will compile Qt from source via vcpkg — expect
30–60 minutes the very first time. Subsequent configures are fast
(seconds) thanks to vcpkg's binary cache.
----------------------------------------------------------------
NOTE
