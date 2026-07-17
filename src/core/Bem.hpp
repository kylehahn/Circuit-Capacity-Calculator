#pragma once

#include "Model.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace ccc::core {

// One sensor-layer panel: a small flat square with area and centroid (mm).
// Used for BEM matrix assembly.
struct Panel {
    double x = 0, y = 0, z = 0;   // centroid in mm
    double area = 0;              // mm^2
    double a = 0;                 // equivalent square side length in mm
};

// Choice of linear-system solver inside the BEM compute step. The two
// approaches trade memory for time:
//   DirectLU   stores the full N x N influence matrix (O(N^2) memory) and
//              solves it once via Eigen's PartialPivLU (O(N^3)). Fastest for
//              small/medium N (up to a few thousand).
//   BiCGStab   never stores the matrix; computes A*x on the fly each
//              iteration (O(N^2) per iter, O(N) memory). Practical for
//              N >> 10000 where dense LU OOMs.
enum class BemSolver { DirectLU, BiCGStab };

struct BemOptions {
    double panelSize  = 0.30;     // mm; legacy single panel size (still used
                                  // when the per-element fields below are 0).
    // Per-element panel sizes (mm). Zero means "fall back to panelSize".
    // Critical for multi-scale geometries: large pads can use coarse panels
    // while um-scale traces use fine panels, so neither dominates N.
    double panelSizePad   = 0.0;
    double panelSizeFpc   = 0.0;
    double panelSizeTrace = 0.0;
    double epsEff     = 4.0;      // effective relative permittivity if no per-layer info
    bool   imageShield = true;    // grounded shield image at z = -glass.thickness
    // Optional cancellation flag. If set and the worker observes it true, the
    // solver throws std::runtime_error("BEM cancelled"). For DirectLU this is
    // best-effort during assemble only. For BiCGStab cancellation lands at
    // the next iteration boundary.
    const std::atomic<bool>* stopFlag = nullptr;
    // Use GPU (cuSOLVER) for the dense LU solve. Requires build with
    // CCC_HAS_CUDA defined and a working CUDA runtime. Only honoured for
    // solver == DirectLU. If unavailable at runtime, throws.
    bool useGpu = false;
    // Solver choice (see BemSolver above).
    BemSolver solver = BemSolver::DirectLU;
    // BiCGStab parameters (ignored for DirectLU).
    int    iterMaxIters = 500;
    double iterTol      = 1.0e-6;   // relative residual tolerance
};

// True iff the binary was built with CCC_HAS_CUDA AND a CUDA-capable device
// is visible at runtime. Cheap (cached after first call); call from the UI
// to decide whether to enable the GPU checkbox.
bool gpuAvailable();

struct BemResult {
    double Cm     = 0.0;          // mutual capacitance, Farads
    double CselfA = 0.0;
    double CselfB = 0.0;
    double CM[2][2]{};            // Maxwell capacitance matrix
    int    NA = 0;
    int    NB = 0;
    double assembleMs = 0;
    double solveMs    = 0;
    // For BiCGStab only: iterations to converge (per RHS) and final relative
    // residual. Unused for DirectLU.
    int    iters[2]    = {0, 0};
    double residual[2] = {0.0, 0.0};
};

// Discretise individual elements on the sensor-layer Z plane.
std::vector<Panel> panelizePad   (const Pad& p,    double sensorZCenter, double panelSize);
std::vector<Panel> panelizeFpc   (const FpcPad& f, double sensorZCenter, double panelSize);
std::vector<Panel> panelizeTrace (const Trace& t, const Model& m,
                                  double sensorZCenter, double panelSize);

// Aggregate panels of every element in a "conductor" — a connected set of
// pad / fpc / trace ids.
struct ConductorRefs {
    std::vector<std::string> padIds;
    std::vector<std::string> fpcIds;
    std::vector<std::string> traceIds;
    std::vector<std::string> zoneIds;
};
// Single-size (legacy): use the same panelSize for pads, FPCs, and traces.
std::vector<Panel> panelizeConductor(const ConductorRefs& c, const Model& m, double panelSize);
// Per-element: use independent panel sizes per element type. Crucial for
// multi-scale geometries (cm-sized pads + um-sized traces) so that one
// element type doesn't dominate N.
std::vector<Panel> panelizeConductor(const ConductorRefs& c, const Model& m,
                                     double panelSizePad,
                                     double panelSizeFpc,
                                     double panelSizeTrace);

// Compute the mutual capacitance between two panel sets via BEM.
//   Cm > 0 in farads. Throws std::runtime_error on degenerate input.
BemResult computeMutualCapacitance(const std::vector<Panel>& panelsA,
                                    const std::vector<Panel>& panelsB,
                                    const Model& model,
                                    const BemOptions& opts = {});

// Format helper — pretty-print a capacitance value (auto fF/pF/nF).
std::string formatCapacitance(double farads);

}  // namespace ccc::core
