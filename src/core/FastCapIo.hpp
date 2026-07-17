#pragma once

#include "Bem.hpp"

#include <functional>
#include <cstddef>
#include <string>
#include <vector>

namespace ccc::core {

enum class ExternalCapSolver {
    FastCapFixed,
    FasterCapAdaptive,
};

std::string externalCapSolverName(ExternalCapSolver solver);
std::string defaultExternalCapExecutable(ExternalCapSolver solver);
bool externalCapAvailable(ExternalCapSolver solver, const std::string& executable = {});
std::string defaultFasterCapExecutable();
bool fasterCapAvailable(const std::string& executable = {});
std::string defaultFastCapExecutable();
bool fastCapAvailable(const std::string& executable = {});

struct FastCapProgress {
    std::string phase;
    std::string detail;
    int step = 0;
    int totalSteps = 0;
    int panelsA = 0;
    int panelsB = 0;
    int panelsEnvironment = 0;
    int dielectricPanels = 0;
    std::size_t outputBytes = 0;
    double elapsedSeconds = 0.0;
};

using FastCapProgressCallback = std::function<void(const FastCapProgress&)>;

struct FastCapEnvironmentOptions {
    ExternalCapSolver solver = ExternalCapSolver::FastCapFixed;
    bool includeGroundNets = true;
    bool includeDielectricStack = true;
    double environmentMarginMm = 20.0;
    double environmentPanelSize = 1.0;
    double dielectricPanelSize = 2.0;
    double fasterCapRelativeError = 0.01;
    double solverTimeoutSeconds = 0.0;
    FastCapProgressCallback progressCallback;
};

struct FastCapSweepPoint {
    double panelSizeMm = 0.0;
    BemResult result;
};

struct FastCapFusionResult {
    std::vector<FastCapSweepPoint> fastCapSweep;
    FastCapSweepPoint fasterCapReference;
    double fastCapFineRelativeDelta = 0.0;
    double fasterCapReferenceRelativeDelta = 0.0;
};

std::string writeFastCapQuiString(const std::vector<Panel>& panelsA,
                                  const std::vector<Panel>& panelsB);
BemResult computeMutualCapacitanceFastCap(const std::vector<Panel>& panelsA,
                                           const std::vector<Panel>& panelsB,
                                           const Model& model,
                                           const BemOptions& opts = {},
                                           const std::string& executable = {});
BemResult computeMutualCapacitanceFastCapWithEnvironment(
    const Model& model,
    const ConductorRefs& refsA,
    const ConductorRefs& refsB,
    const std::string& netA,
    const std::string& netB,
    const BemOptions& opts = {},
    const FastCapEnvironmentOptions& env = {},
    const std::string& executable = {});
FastCapFusionResult computeMutualCapacitanceFusion(
    const Model& model,
    const ConductorRefs& refsA,
    const ConductorRefs& refsB,
    const std::string& netA,
    const std::string& netB,
    const BemOptions& opts = {},
    const FastCapEnvironmentOptions& env = {});

}  // namespace ccc::core
