# Circuit Capacity Calculator

Native C++ desktop app (Qt 6 + C++20) that imports a PCB and computes
**net-to-net mutual capacitance** via a Boundary Element Method (BEM) solver.
Two intended workflows:

1. **Editor mode** (legacy) — interactive 3D layout of capacitive sensors:
   pads / traces / FPC pads on a glass-substrate sensor stack.
2. **KiCad import mode** — load a `.kicad_pcb` file directly and measure
   capacitance between any two named nets.

Originally a port of an HTML/Three.js prototype (`sensor3d_editor.html`,
kept outside the repo). The KiCad path was added later and is now the
primary use case.

## Build

```
cmake --preset windows-release       # or linux-release / macos-release
cmake --build --preset windows-release -j
ctest --preset windows-release
```

`build/<preset>/circuit_capacity_calculator{,.exe}` is the output. The
preset toolchain pins vcpkg under `external/vcpkg/`; run
`tools/bootstrap.{sh,ps1}` once after a fresh clone.

Optional: configure with `-DCCC_ENABLE_CUDA=ON` to wire cuSOLVER for the
GPU LU path. Requires CUDA Toolkit (not in vcpkg).

## Module map

```
src/
├── core/            # Pure data + algorithms. No Qt deps.
│   ├── Pad.hpp/.cpp        # one pad on the sensor / Cu layer
│   ├── FpcPad.hpp          # FPC contact pad (rectangular)
│   ├── Trace.hpp           # polyline trace (waypoints, width, layer, net)
│   ├── Zone.hpp            # KiCad copper pour: outline polygon + layer + net
│   ├── Layer.hpp           # one stackup layer (id, thickness, eps_r, isConductor)
│   ├── GlassPlate.hpp      # legacy glass substrate
│   ├── Meta.hpp            # filename / version metadata
│   ├── Model.hpp/.cpp      # the whole scene: layers + pads + fpcs + traces + zones
│   └── Bem.hpp/.cpp        # BEM solver (panel assembly + LU/BiCGStab)
├── io/
│   ├── JsonIo.cpp          # native .ccc JSON read/write (nlohmann_json)
│   ├── GlbIo.cpp           # GLB read/write with sensorEditor extras (tinygltf)
│   └── KicadPcbIo.cpp      # KiCad 9 .kicad_pcb S-expression parser
└── ui/
    ├── Camera.hpp/.cpp     # Z-up orbit camera
    ├── Viewport3D.hpp/.cpp # QOpenGLWidget — scene rendering, picking, modes
    └── MainWindow.hpp/.cpp # menus, docks, toolbars, BEM dialogs
```

`core/` and `io/` are header-clean — no Qt. UI talks to model through values,
never the other way around.

## Conventions

- **C++20**, MSVC + GCC + Clang. clang-format on save (`.clang-format`).
- Linear algebra: **Eigen 3.4** (`PartialPivLU` + matrix-free BiCGStab).
- 3D rendering: raw OpenGL 3.3 Core in a `QOpenGLWidget` subclass.
  **Not** Qt 3D. Lambert shader, single uniform light.
- Tests: **Catch2 v3** under `tests/`, ASCII names only (Windows CMD cp949).
- Coordinates: **Z-up**, units **mm**. Capacitance in **farads**.

## Data model

`Model` aggregates everything. Each element type:

| Type    | Where it lives          | KiCad-aware fields            |
| ------- | ----------------------- | ----------------------------- |
| `Pad`   | `model.pads[]`          | `net`, `layer` (e.g. "F.Cu")  |
| `FpcPad`| `model.fpcPads[]`       | (no net field — legacy only)  |
| `Trace` | `model.traces[]`        | `net`, `layer`                |
| `Zone`  | `model.zones[]`         | `net`, `layerId`, `outline[]` |
| `Layer` | `model.layers[]`        | `isConductor`, `permittivity` |
| `Glass` | `model.glass`           | hidden by KiCad import        |

KiCad import populates `net` / `layer` for every element. The default
sensor model leaves them empty (legacy).

## BEM solver (Bem.hpp)

Two paths, switchable via `BemOptions::solver`:

- **`DirectLU`** — assemble dense N×N influence matrix, solve via
  `Eigen::PartialPivLU`. Memory **O(N²)**, time **O(N³)**. Optional GPU
  via cuSOLVER (`opts.useGpu` + `CCC_HAS_CUDA`).
- **`BiCGStab`** — matrix-free iterative. `bemMatvec` recomputes A·x on
  the fly each iteration (OpenMP-parallel rows). Memory **O(N)**.
  Practical for KiCad-scale N (>10 000) where dense LU OOMs.

Cancellation: `BemOptions::stopFlag` is checked between matvecs. The
LU solve itself can't be interrupted.

Per-element panel sizes (`panelSizePad`, `panelSizeFpc`, `panelSizeTrace`).
Critical for multi-scale geometry (cm-pads + um-traces) — let pads be
coarse and traces fine.

`panelizeConductor()` builds a flat `vector<Panel>` from a
`ConductorRefs` (lists of pad/fpc/trace/zone IDs). Zone panelisation
rasterises the polygon bbox + point-in-polygon test.

## Reference numerics (validated against JS prototype, ε_r = 1)

| Geometry                                         | Cm (BEM) |
| ------------------------------------------------ | -------- |
| 2 parallel coplanar traces L=10 W=0.4 gap=0.6 mm | 0.143 pF |
| 2 coplanar disks R=3 mm, D=10 mm                 | 0.143 pF |
| Trace ↔ pad mixed sample                         | 0.089 pF |

Reproduce within ~1 %.

## UI workflow — KiCad path (the important one)

1. **File → Open KiCad PCB...** loads a `.kicad_pcb`. Parser extracts:
   - copper layers from `(setup (stackup ...))` (ignores soldermask/silk)
   - all `(net N "name")` (auto-synthesises `Net-N` for unnamed nets)
   - footprint pads (with their `(layers ...)`)
   - segments (with `(layer ...)` and `(net N)`)
   - zones — uses `filled_polygon` islands when present, else outline
   - bbox-centres on (0, 0); KiCad Y-down → our Y-up
2. App switches to **2D top view + fit**. Glass auto-hidden.
3. **Ctrl+M** opens the **Net Capacitance** dialog:
   - Two combo boxes for Net A / Net B (filled from model nets)
   - "Pick from viewport" buttons hide the dialog, listen for one
     viewport selection, then re-open with the picked element's net set.
   - Solver settings: panel sizes, ε_r, solver type
   - Three presets — **Quick** (mm-scale, seconds), **Convergence**
     (~0.3 mm, ~1 % accuracy, minutes), **Precise** (~0.1 mm, hours)
   - **Compute** runs BEM in a `QtConcurrent::run` worker. UI stays
     responsive. **Cancel** flips `stopFlag`.
4. **Cap Measure** mode in the viewport (C key) is also routed to the
   same Net Capacitance dialog.

The legacy element-pick dialog (`runCapacitanceDialog`) still exists but
isn't on a hot path.

## Rendering

- 2D mode is default (top view, RMB pans, no orbit). Toggle via toolbar
  `2D` action. 3D mode lets RMB orbit.
- Per-element layer Z (`Viewport3D::rebuildBatches` → `layerZ` helper):
  KiCad imports stack F.Cu / In1.Cu / In2.Cu / B.Cu in order, each
  ~0.035 mm thick.
- Per-layer colour (`colorForKicadLayer`): F.Cu red, B.Cu green,
  In1 yellow, In2 blue, etc. Easy to tell layers apart.
- Trace render width has a visual minimum (`traceRenderWidthFloor_`,
  default 0.05 mm) so um-scale traces don't disappear. BEM still uses
  the real width.
- Backface culling **disabled** because zone fill triangulation can have
  arbitrary winding.
- Zone fill: ear-clipping triangulation, falls back to bounding box for
  polygons over 200 vertices (KiCad ground planes can be huge).
- Endpoint cylinders on traces are skipped for 2-point traces (KiCad
  segments) to keep batch count down.

## Performance notes (KiCad-scale)

- Sample PCB has 887 pads / 1894 segments / 36 zones (66 islands).
- Default Quick preset (Pad 1.0 mm, Trace 0.5 mm) gives O(thousands)
  panels per net — Direct LU runs in seconds.
- Convergence preset (0.3 mm / 0.1 mm) → O(tens of thousands) panels.
  BiCGStab needed; runtime tens of seconds to minutes per Compute.
- Precise (0.1 mm / 0.03 mm) → easily 100k+ panels. BiCGStab; hours.

## File-truncation gotcha when editing

The Edit tool occasionally truncates very large source files mid-write
(observed at ~1000+ lines). When this happens:

1. Detect via `wc -l` + `tail -3` (file ends mid-statement).
2. `head -N` truncate to a safe line, then `cat >> file << 'EOF'`
   heredoc-append the missing tail.
3. Validate brace balance with a real C tokenizer (a regex-based
   `count('{')` vs `count('}')` is unreliable — use a state machine).

This is why some functions (especially `MainWindow::runCapacitanceDialog`
and `runNetCapacitanceDialog`) are written as one big block: easier to
re-append cleanly than to surgically edit.

## Where the prototype lives

`sensor3d_editor.html` is the original browser-based prototype, in the
`16 Sensor` working folder (separate from this repo). Open it in a
browser when porting a feature so behaviour and numerics match. If you
bring it in for convenience, drop under `docs/prototype/`.
