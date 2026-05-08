# Circuit Capacity Calculator

Native Qt 6 + C++20 desktop calculator and 3D editor for capacitance of
layered PCB topologies, solved via BEM. The original browser prototype
(`sensor3d_editor.html`) is kept separately as a behavioural reference.

## Quick start (any platform)

```
git clone <repo-url> circuit-capacity-calculator
cd circuit-capacity-calculator
# 1. Bootstrap vcpkg (one-time per machine, idempotent):
tools/bootstrap.sh         # macOS / Linux
tools\bootstrap.ps1        # Windows (PowerShell)

# 2. Configure + build:
cmake --preset linux-release        # macos-release / windows-release
cmake --build --preset linux-release -j
ctest --preset linux-release

# 3. Run:
./build/linux-release/circuit_capacity_calculator
```

> First configure compiles Qt from source via vcpkg — **30–60 minutes**. Every
> subsequent configure on the same machine is seconds, and CI/cache covers
> other machines.

## Prerequisites

| Platform | Compiler | Other |
|----------|----------|-------|
| Windows  | Visual Studio 2022 (MSVC + CMake + Windows SDK) | Git, PowerShell 7 |
| macOS    | Xcode 15+ command-line tools                    | Homebrew, `brew install cmake ninja pkg-config` |
| Linux    | GCC 12+ or Clang 15+                            | `sudo apt install build-essential cmake ninja-build git pkg-config libgl1-mesa-dev libxkbcommon-dev libxcb*-dev libxkbcommon-x11-dev` (full list in `.github/workflows/build.yml`) |

CMake **3.25** or newer. Ninja recommended (Visual Studio generator works on
Windows but is slower).

## Multi-machine workflow

This project is set up to move cleanly between machines:

- vcpkg is cloned into `external/vcpkg/` and pinned to a baseline. Both are
  managed by `tools/bootstrap.*` and never committed.
- CMake presets reference the bundled vcpkg, so no environment variables are
  needed.
- A pull → configure → build cycle on a new machine is the same three commands
  as above.

### Build cache (recommended)

Install **sccache** so re-builds across machines / sessions are fast:

```
# macOS
brew install sccache
# Windows
scoop install sccache         # or  winget install sccache
# Ubuntu
sudo apt install sccache
```

`CMakePresets.json` already wires `CMAKE_C{,XX}_COMPILER_LAUNCHER=sccache`, so
once it is on `PATH` it is used automatically. To share the cache across
machines, set `SCCACHE_BUCKET=<your-s3-bucket>` (and credentials) before
running configure — sccache will pull pre-built objects on the second
machine.

## Layout

```
circuit-capacity-calculator/
├── CLAUDE.md                project context for the AI dev assistant
├── README.md                this file
├── CMakeLists.txt           top-level build
├── CMakePresets.json        per-platform presets (Win/Mac/Linux × Debug/Release)
├── vcpkg.json               manifest: Qt6, Eigen, tinygltf, nlohmann_json, Catch2
├── .clang-format            C++ formatting (Google base, 110 cols)
├── .editorconfig            editor-agnostic basics
├── .gitignore / .gitattributes
├── src/                     application source
│   ├── CMakeLists.txt
│   └── main.cpp             scaffold entry point — replace with real UI
├── tests/                   Catch2 v3 tests, mirror src/
│   ├── CMakeLists.txt
│   └── test_smoke.cpp
├── tools/                   dev scripts (bootstrap, formatting, …)
│   ├── bootstrap.sh
│   └── bootstrap.ps1
├── .github/workflows/       CI matrix: Windows / macOS / Linux × Release
│   └── build.yml
└── external/                vcpkg lives here after bootstrap (gitignored)
```

## A note on paths with spaces

If your local clone path contains spaces, CMake and vcpkg can occasionally
misbehave. The safe move is to clone this repo into a path **without spaces**,
e.g. `C:\dev\circuit-capacity-calculator` or `~/code/circuit-capacity-calculator`.
The scaffold itself is portable — just `git clone` to the new path on the
new machine.

## License

TBD — add `LICENSE` in the repo root before publishing.
