# Circuit Capacity Calculator — native C++ port

Capacitance calculator for layered PCB topologies, with an interactive 3D
editor. Replaces an earlier HTML/Three.js prototype (`sensor3d_editor.html`,
kept separately as a behavioural reference) with a Qt 6 + C++20 desktop
application optimised for fast BEM solves and small, native distributables.

The JS prototype is the **behavioural specification**. Open it side-by-side
when porting a feature and verify outputs match before checking in. Keep the
prototype editable; do not break it.

## Status

- [x] Project scaffold (CMake + vcpkg manifest + CI matrix)
- [x] Data model (`Pad`, `Trace`, `FpcPad`, `Layer`, `GlassPlate`, `Meta`, `Model`)
- [x] Native JSON format (`.ccc`) — round-trip via `ccc::io::{modelToJson, modelFromJson}`
- [x] Auto fan-out routing — `Model::generateGrid()` reproduces the JS non-overlapping algorithm
- [ ] Empty Qt window placeholder (`src/main.cpp`) — replace with real `MainWindow` + `Viewport3D`
- [ ] GLB import/export (parity with prototype's `userData.sensorEditor`)
- [ ] Layer rendering (Glass, Shield, Inorganic, Sensor, Organic)
- [ ] Pad / trace / FPC rendering
- [ ] Selection + drag tools, modes (select / addPad / deletePad / addTrace / editTrace / moveFpc)
- [ ] Trace draw + ortho mode
- [ ] Marquee multi-select
- [ ] Copy/place mode (R = mirror)
- [ ] Undo/redo via `QUndoStack`
- [ ] BEM capacitance solver (Eigen + OpenMP, optional MKL)
- [ ] Cap calculation modal

## Build

```
cmake --preset windows-release       # or linux-release / macos-release
cmake --build --preset windows-release -j
ctest --preset windows-release
```

The executable lands in `build/<preset>/circuit_capacity_calculator{,.exe}`. CMake presets pin
the vcpkg toolchain at `${sourceDir}/external/vcpkg/...`, so no environment
variables are needed once `tools/bootstrap.{sh,ps1}` has been run.

## Conventions

- **C++20**. clang-format on save (`.clang-format` is the source of truth).
- One class per `.hpp/.cpp` pair under `src/<module>/`.
- Modules:
  - `core/` — model + algorithms. Header-clean; no Qt dependencies.
  - `ui/` — Qt widgets, OpenGL viewport, gizmos.
  - `io/` — file I/O (GLB, native JSON).
  - The UI never reaches into algorithm internals; data flows through the model.
- Linear algebra: **Eigen 3.4** (`Eigen::PartialPivLU` for solve).
  Link Intel MKL when available (define `EIGEN_USE_MKL_ALL`).
- 3D rendering: raw OpenGL 4.5 in a `QOpenGLWidget` subclass. **Not** Qt 3D.
- Tests: **Catch2 v3** under `tests/`, mirroring `src/`.
  Test names must be **ASCII only** — Windows CMD's cp949 mangles em-dashes
  and arrows in CTest filter args, breaking discovery on that platform.
- Coordinates: Z-up (matches prototype). Geometry units: millimetres.
  Capacitance results in farads (SI).

## Numerics — reference values from the prototype

The C++ port must reproduce these to within ~1 % (free space, ε_r = 1):

| Geometry                                              | Cₘ (BEM)      |
|-------------------------------------------------------|---------------|
| 2 parallel coplanar traces L=10 W=0.4 gap=0.6 mm      | 0.143 pF      |
| 2 coplanar disks R=3 mm, D=10 mm                      | 0.143 pF      |
| Trace ↔ pad (mixed sample case)                       | 0.089 pF      |

Self-term coefficient: `P_ii = a · ln(1+√2) / (2π·ε)`, where `a` is the panel
side length. Off-diagonal: centroid-to-centroid `A_j / (4π·ε·r)`. Optional
grounded-shield image at `z = -glass.thickness` flips sign of the image term.

Implementation notes already validated in JS:

- Solve via Gauss elimination with partial pivoting works but is slow; replace
  with `Eigen::PartialPivLU` (or `LDLT` if matrix is symmetric positive
  definite — verify before assuming).
- Matrix assembly is the bottleneck for small N. **Vectorise via `Eigen::Map`**
  and parallelise with OpenMP; do not Python-style loop.
- For N > ~2000 the next gate is **algorithmic** — FMM (ExaFMM-T) or H-matrices
  (H2Lib). Plan for it but don't pre-optimise.

## Performance budget

These are targets, not guarantees, on a 2024-class laptop, single trace pair:

| N (panels) | Matrix assemble | Solve  | Total  |
|------------|-----------------|--------|--------|
| 200        | < 1 ms          | < 2 ms | < 5 ms |
| 1 000      | < 30 ms         | < 50 ms| < 100 ms|
| 5 000      | < 800 ms        | < 4 s  | < 5 s  |

Beyond that → algorithmic change.

## Multi-machine workflow

This repo is designed to be cloned cleanly onto any machine. The bootstrap
script clones vcpkg as a submodule-equivalent, pins the baseline, and the
preset toolchain points at it. Daily flow:

```
git pull
cmake --preset <preset>     # only if presets changed; otherwise skip
cmake --build --preset <preset>
ctest --preset <preset>
```

Build cache: install **sccache** locally; presets already wire it as
`CMAKE_CXX_COMPILER_LAUNCHER`. For shared cache across machines set
`SCCACHE_BUCKET=...` (S3) before configure.

## Where the prototype lives

`sensor3d_editor.html` is the original browser-based prototype, kept in the
`16 Sensor` working folder (separate from this repo). Open it in a browser
when porting a feature so behaviour and numerics can be checked side-by-side.
If you bring it into this repo for convenience, drop it under `docs/prototype/`
and update this section with the path.
