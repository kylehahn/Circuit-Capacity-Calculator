#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/FastCapIo.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ccc::core;
using Catch::Approx;

TEST_CASE("FasterCap executable accepts generated two-conductor QUI and returns farads", "[fastercap]") {
    if (!fasterCapAvailable()) {
        SKIP("external/fastercap/FasterCap.exe is not available");
    }

    Panel a;
    a.x = 500.0;
    a.y = 500.0;
    a.z = 0.0;
    a.a = 1000.0;
    a.area = 1'000'000.0;

    Panel b = a;
    b.x = 2500.0;

    BemOptions opts;
    opts.epsEff = 1.0;

    const Model model;
    const auto result = computeMutualCapacitanceFastCap({a}, {b}, model, opts);
    REQUIRE(result.NA == 1);
    REQUIRE(result.NB == 1);
    REQUIRE(result.CM[0][1] == Approx(-6.846e-12).epsilon(0.02));
    REQUIRE(result.CM[1][0] == Approx(-6.846e-12).epsilon(0.02));
    REQUIRE(result.Cm == Approx(6.846e-12).epsilon(0.02));
}

TEST_CASE("FastCap fixed-panel environment solve uses generated panel size", "[fastcap]") {
    if (!fastCapAvailable()) {
        SKIP("external/fastcap/fastcap.exe is not available");
    }

    Model model;
    model.layers = {
        {"B.Cu", "B.Cu", 0.035, "#23964f", 1.0, true, true, 1.0},
    };
    model.zones = {
        {"ZA", "B.Cu", "/SIG1", {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}},
        {"ZB", "B.Cu", "/SIG2", {{3.0, 0.0}, {4.0, 0.0}, {4.0, 1.0}, {3.0, 1.0}}},
    };

    ConductorRefs a;
    a.zoneIds.push_back("ZA");
    ConductorRefs b;
    b.zoneIds.push_back("ZB");

    BemOptions opts;
    opts.panelSize = 0.5;
    opts.panelSizePad = 0.5;
    opts.panelSizeTrace = 0.5;
    opts.epsEff = 1.0;

    FastCapEnvironmentOptions env;
    env.solver = ExternalCapSolver::FastCapFixed;
    env.includeGroundNets = false;
    env.includeDielectricStack = false;
    std::vector<std::string> phases;
    env.progressCallback = [&](const FastCapProgress& p) {
        phases.push_back(p.phase);
    };

    const auto result = computeMutualCapacitanceFastCapWithEnvironment(
        model, a, b, "/SIG1", "/SIG2", opts, env);
    REQUIRE(result.NA == 4);
    REQUIRE(result.NB == 4);
    REQUIRE(std::isfinite(result.Cm));
    REQUIRE(result.Cm > 0.0);
    REQUIRE(!phases.empty());
    REQUIRE(std::find(phases.begin(), phases.end(), "Running FastCap") != phases.end());
}

TEST_CASE("Fusion capacitance runs FastCap sweep and FasterCap reference", "[fastcap][fastercap]") {
    if (!fastCapAvailable() || !fasterCapAvailable()) {
        SKIP("external FastCap and FasterCap executables are not available");
    }

    Model model;
    model.layers = {
        {"B.Cu", "B.Cu", 0.035, "#23964f", 1.0, true, true, 1.0},
    };
    model.zones = {
        {"ZA", "B.Cu", "/SIG1", {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}},
        {"ZB", "B.Cu", "/SIG2", {{3.0, 0.0}, {4.0, 0.0}, {4.0, 1.0}, {3.0, 1.0}}},
    };

    ConductorRefs a;
    a.zoneIds.push_back("ZA");
    ConductorRefs b;
    b.zoneIds.push_back("ZB");

    BemOptions opts;
    opts.epsEff = 1.0;

    FastCapEnvironmentOptions env;
    env.includeGroundNets = true;
    env.includeDielectricStack = true;

    const auto result = computeMutualCapacitanceFusion(
        model, a, b, "/SIG1", "/SIG2", opts, env);
    REQUIRE(result.fastCapSweep.size() == 3);
    REQUIRE(result.fastCapSweep[0].result.NA == 1);
    REQUIRE(result.fastCapSweep[1].result.NA == 4);
    REQUIRE(result.fastCapSweep[2].result.NA == 25);
    REQUIRE(result.fasterCapReference.result.NA == 4);
    REQUIRE(std::isfinite(result.fasterCapReference.result.Cm));
    REQUIRE(result.fastCapFineRelativeDelta >= 0.0);
    REQUIRE(result.fasterCapReferenceRelativeDelta >= 0.0);
}

TEST_CASE("FasterCap model solve includes local grounded conductor and stack media", "[fastercap]") {
    if (!fasterCapAvailable()) {
        SKIP("external/fastercap/FasterCap.exe is not available");
    }

    Model model;
    model.layers = {
        {"B.Cu", "B.Cu", 0.035, "#23964f", 1.0, true, true, 1.0},
        {"soldermask", "Soldermask", 0.020, "#9fb3c8", 0.2, false, false, 3.3},
    };
    model.zones = {
        {"ZA", "B.Cu", "/SIG1", {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}},
        {"ZB", "B.Cu", "/SIG2", {{3.0, 0.0}, {4.0, 0.0}, {4.0, 1.0}, {3.0, 1.0}}},
        {"ZG", "B.Cu", "GND", {{1.4, -0.8}, {2.6, -0.8}, {2.6, 1.8}, {1.4, 1.8}}},
    };

    ConductorRefs a;
    a.zoneIds.push_back("ZA");
    ConductorRefs b;
    b.zoneIds.push_back("ZB");

    BemOptions opts;
    opts.panelSize = 1.0;
    opts.panelSizePad = 1.0;
    opts.panelSizeTrace = 1.0;
    opts.epsEff = 1.0;

    FastCapEnvironmentOptions env;
    env.solver = ExternalCapSolver::FasterCapAdaptive;
    env.includeGroundNets = true;
    env.includeDielectricStack = true;
    env.environmentMarginMm = 5.0;
    env.environmentPanelSize = 1.0;
    env.dielectricPanelSize = 2.0;

    const auto result = computeMutualCapacitanceFastCapWithEnvironment(
        model, a, b, "/SIG1", "/SIG2", opts, env);
    REQUIRE(result.NA == 1);
    REQUIRE(result.NB == 1);
    REQUIRE(std::isfinite(result.Cm));
    REQUIRE(result.Cm > 0.0);
    REQUIRE(result.CselfA > result.Cm);
    REQUIRE(result.CselfB > result.Cm);
}
