#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Bem.hpp"
#include "core/Connectivity.hpp"
#include "io/ExcellonIo.hpp"
#include "io/GerberIo.hpp"
#include "io/GerberProject.hpp"
#include "io/KicadPcbIo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;

TEST_CASE("Gerber parser extracts a stroked copper polygon", "[gerber]") {
    const auto path = std::filesystem::temp_directory_path() / "ccc_test-F_Cu.gbr";
    {
        std::ofstream out(path);
        out << "%FSLAX46Y46*%\n"
            << "%MOMM*%\n"
            << "%ADD10C,0.400*%\n"
            << "D10*\n"
            << "X0.0Y0.0D02*\n"
            << "X10.0Y0.0D01*\n"
            << "M02*\n";
    }

    auto layer = ccc::io::readGerberLayer(path.string());
    REQUIRE(layer.layerId == "F.Cu");
    REQUIRE(layer.polygons.size() == 1);
    REQUIRE(layer.polygons.front().outline.size() >= 8);
    std::filesystem::remove(path);
}

TEST_CASE("Gerber parser preserves KiCad TO.N net attributes", "[gerber]") {
    const auto path = std::filesystem::temp_directory_path() / "ccc_test_net_attrs-CuBottom.gbr";
    {
        std::ofstream out(path);
        out << "%FSLAX46Y46*%\n"
            << "%MOMM*%\n"
            << "%ADD10C,1.000*%\n"
            << "D10*\n"
            << "%TO.N,/SIG1*%\n"
            << "X0Y0D03*\n"
            << "%TO.N,/SIG2*%\n"
            << "X3000000Y0D03*\n"
            << "M02*\n";
    }

    auto layer = ccc::io::readGerberLayer(path.string());
    REQUIRE(layer.polygons.size() == 2);
    REQUIRE(layer.polygons[0].net == "/SIG1");
    REQUIRE(layer.polygons[1].net == "/SIG2");

    std::filesystem::remove(path);
}

TEST_CASE("Gerber project keeps same-layer different nets as separate conductors", "[gerber]") {
    const auto dir = std::filesystem::temp_directory_path() / "ccc_gerber_net_union_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "board-CuBottom.gbr")
        << "%FSLAX46Y46*%\n"
        << "%MOMM*%\n"
        << "%ADD10R,10.000X10.000*%\n"
        << "%ADD11C,2.000*%\n"
        << "D10*\n"
        << "%TO.N,GND*%\n"
        << "X0Y0D03*\n"
        << "D11*\n"
        << "%TO.N,/SIG1*%\n"
        << "X0Y0D03*\n"
        << "M02*\n";

    auto model = ccc::io::readGerberProject(dir.string());
    std::set<std::string> nets;
    for (const auto& z : model.zones) nets.insert(z.net);
    REQUIRE(nets.contains("GND"));
    REQUIRE(nets.contains("/SIG1"));

    ccc::core::ConductorRefs sig;
    for (const auto& z : model.zones)
        if (z.net == "/SIG1") sig.zoneIds.push_back(z.id);
    auto panels = ccc::core::panelizeConductor(sig, model, 0.5);
    REQUIRE_FALSE(panels.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("Gerber project approximates arc strokes as round disks", "[gerber]") {
    const auto dir = std::filesystem::temp_directory_path() / "ccc_gerber_arc_disk_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "disk-CuBottom.gbr")
        << "%FSLAX46Y46*%\n"
        << "%MOMM*%\n"
        << "%ADD10C,2.000*%\n"
        << "D10*\n"
        << "X1.0Y0.0D02*\n"
        << "G75*\n"
        << "G02*\n"
        << "X-1.0Y0.0I-1.0J0.0D01*\n"
        << "G01*\n"
        << "X-1.0Y0.0D02*\n"
        << "G75*\n"
        << "G02*\n"
        << "X1.0Y0.0I1.0J0.0D01*\n"
        << "M02*\n";

    auto model = ccc::io::readGerberProject(dir.string());
    REQUIRE(model.zones.size() == 1);

    double yMin = model.zones.front().outline.front()[1];
    double yMax = yMin;
    for (const auto& p : model.zones.front().outline) {
        yMin = std::min(yMin, p[1]);
        yMax = std::max(yMax, p[1]);
    }
    REQUIRE((yMax - yMin) == Approx(4.0).margin(0.15));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Gerber parser does not turn macro flashes into giant rectangles", "[gerber]") {
    const auto path = std::filesystem::temp_directory_path() / "ccc_test_macro-F_Cu.gbr";
    {
        std::ofstream out(path);
        out << "%FSLAX46Y46*%\n"
            << "%MOMM*%\n"
            << "%AMFreePoly0*\n"
            << "4,1,4,-0.5,-0.5,0.5,-0.5,0.5,0.5,-0.5,0.5,$1*%\n"
            << "%ADD10FreePoly0,90.0*%\n"
            << "D10*\n"
            << "X0Y0D03*\n"
            << "M02*\n";
    }

    auto layer = ccc::io::readGerberLayer(path.string());
    REQUIRE(layer.polygons.size() == 1);
    double xMin = layer.polygons.front().outline.front()[0];
    double xMax = xMin;
    for (const auto& p : layer.polygons.front().outline) {
        xMin = std::min(xMin, p[0]);
        xMax = std::max(xMax, p[0]);
    }
    REQUIRE((xMax - xMin) == Approx(1.0).margin(0.01));

    std::filesystem::remove(path);
}

TEST_CASE("Gerber layer inference recognizes KiCad CuTop CuBottom CuIn names", "[gerber]") {
    REQUIRE(ccc::io::inferGerberLayerId("16ver8-CuTop.gbr") == "F.Cu");
    REQUIRE(ccc::io::inferGerberLayerId("16ver8-CuIn1.gbr") == "In1.Cu");
    REQUIRE(ccc::io::inferGerberLayerId("16ver8-CuIn2.gbr") == "In2.Cu");
    REQUIRE(ccc::io::inferGerberLayerId("16ver8-CuBottom.gbr") == "B.Cu");
    REQUIRE(ccc::io::inferGerberLayerId("16ver8-EdgeCuts.gbr") == "Edge.Cuts");
}

TEST_CASE("Excellon parser extracts drill hits and tool diameters", "[gerber]") {
    const auto path = std::filesystem::temp_directory_path() / "ccc_test.drl";
    {
        std::ofstream out(path);
        out << "M48\n"
            << "METRIC\n"
            << "T1C0.300\n"
            << "%\n"
            << "T1\n"
            << "X1.0Y2.0\n"
            << "M30\n";
    }

    auto hits = ccc::io::readExcellonDrillFile(path.string());
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().x == Approx(1.0));
    REQUIRE(hits.front().y == Approx(2.0));
    REQUIRE(hits.front().diameter == Approx(0.3));
    std::filesystem::remove(path);
}

TEST_CASE("Connectivity assigns one object across layers through a via", "[gerber]") {
    ccc::core::Model m;
    m.layers = {
        {"F.Cu", "F.Cu", 0.035, "#c8342a", 1.0, true, true, 1.0},
        {"B.Cu", "B.Cu", 0.035, "#23964f", 1.0, true, true, 1.0},
    };
    m.zones = {
        {"Z1", "F.Cu", "", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
        {"Z2", "B.Cu", "", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
    };

    auto stats = ccc::core::assignConnectedCopperObjects(m, {{0.5, 0.5, 0.3, "F.Cu", "B.Cu"}});
    REQUIRE(stats.objectCount == 1);
    REQUIRE(m.zones[0].net == m.zones[1].net);
}

TEST_CASE("Gerber project maps KiCad gbrjob inner copper layers", "[gerber]") {
    const auto dir = std::filesystem::temp_directory_path() / "ccc_gerber_project_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const char* gbr =
        "%FSLAX46Y46*%\n"
        "%MOMM*%\n"
        "G36*\n"
        "X0.0Y0.0D02*\n"
        "X1.0Y0.0D01*\n"
        "X1.0Y1.0D01*\n"
        "X0.0Y1.0D01*\n"
        "X0.0Y0.0D01*\n"
        "G37*\n"
        "M02*\n";
    for (const auto* name : {"l1.gbr", "l2.gbr", "l3.gbr", "l4.gbr"}) {
        std::ofstream(dir / name) << gbr;
    }
    std::ofstream(dir / "board.gbrjob")
        << R"({
  "FilesAttributes": [
    {"Path": "l1.gbr", "FileFunction": "Copper,L1,Top"},
    {"Path": "l2.gbr", "FileFunction": "Copper,L2,Inr"},
    {"Path": "l3.gbr", "FileFunction": "Copper,L3,Inr"},
    {"Path": "l4.gbr", "FileFunction": "Copper,L4,Bot"}
  ]
})";

    auto model = ccc::io::readGerberProject(dir.string());
    REQUIRE(model.findLayer("F.Cu") != nullptr);
    REQUIRE(model.findLayer("In1.Cu") != nullptr);
    REQUIRE(model.findLayer("In2.Cu") != nullptr);
    REQUIRE(model.findLayer("B.Cu") != nullptr);
    REQUIRE(model.findLayer("F.Cu")->visible);
    REQUIRE_FALSE(model.findLayer("In1.Cu")->visible);
    REQUIRE_FALSE(model.findLayer("In2.Cu")->visible);
    REQUIRE_FALSE(model.findLayer("B.Cu")->visible);
    REQUIRE(model.zones.size() == 4);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Gerber project uses KiCad stackup only when explicitly provided", "[gerber][stackup]") {
    const auto root = std::filesystem::temp_directory_path() / "ccc_gerber_explicit_stackup_test";
    const auto dir = root / "gerber";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(dir);

    const char* gbr =
        "%FSLAX46Y46*%\n"
        "%MOMM*%\n"
        "G36*\n"
        "X0.0Y0.0D02*\n"
        "X1.0Y0.0D01*\n"
        "X1.0Y1.0D01*\n"
        "X0.0Y1.0D01*\n"
        "X0.0Y0.0D01*\n"
        "G37*\n"
        "M02*\n";
    std::ofstream(dir / "board-CuTop.gbr") << gbr;
    std::ofstream(dir / "board-CuBottom.gbr") << gbr;

    const auto kicad = root / "board.kicad_pcb";
    {
        std::ofstream out(kicad);
        out << "(kicad_pcb\n"
            << "  (layers\n"
            << "    (0 \"F.Cu\" signal)\n"
            << "    (2 \"B.Cu\" signal)\n"
            << "  )\n"
            << "  (setup\n"
            << "    (stackup\n"
            << "      (layer \"F.Cu\" (type \"copper\") (thickness 0.035))\n"
            << "      (layer \"dielectric 1\" (type \"core\") (thickness 0.42) (epsilon_r 3.7))\n"
            << "      (layer \"B.Cu\" (type \"copper\") (thickness 0.035))\n"
            << "    )\n"
            << "  )\n"
            << ")\n";
    }

    const auto gerberOnly = ccc::io::readGerberProject(dir.string());
    REQUIRE(gerberOnly.layers.size() == 2);
    REQUIRE(gerberOnly.findLayer("dielectric 1") == nullptr);

    const auto withStack = ccc::io::readGerberProject(dir.string(), kicad.string());
    REQUIRE(withStack.layers.size() == 3);
    REQUIRE(withStack.layers[1].id == "dielectric 1");
    REQUIRE(withStack.layers[1].thickness == Approx(0.42));
    REQUIRE(withStack.layers[1].permittivity == Approx(3.7));

    std::filesystem::remove_all(root);
}

TEST_CASE("Gerber project unions same-layer overlapping polygons into islands", "[gerber]") {
    const auto dir = std::filesystem::temp_directory_path() / "ccc_gerber_union_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "board-CuTop.gbr")
        << "%FSLAX46Y46*%\n"
        << "%MOMM*%\n"
        << "G36*\n"
        << "X0.0Y0.0D02*\n"
        << "X2.0Y0.0D01*\n"
        << "X2.0Y1.0D01*\n"
        << "X0.0Y1.0D01*\n"
        << "X0.0Y0.0D01*\n"
        << "G37*\n"
        << "G36*\n"
        << "X1.0Y0.0D02*\n"
        << "X3.0Y0.0D01*\n"
        << "X3.0Y1.0D01*\n"
        << "X1.0Y1.0D01*\n"
        << "X1.0Y0.0D01*\n"
        << "G37*\n"
        << "M02*\n";

    auto model = ccc::io::readGerberProject(dir.string());
    REQUIRE(model.zones.size() == 1);
    REQUIRE(model.zones.front().layerId == "F.Cu");

    std::filesystem::remove_all(dir);
}

TEST_CASE("Zone panelization keeps a coarse fallback panel for thin copper", "[gerber][bem]") {
    ccc::core::Model model;
    model.layers = {{"B.Cu", "B.Cu", 0.035, "#23964f", 1.0, true, true, 1.0}};
    model.zones = {{
        "Z1",
        "B.Cu",
        "/THIN",
        {{0.0, 0.0}, {20.0, 0.0}, {20.0, 0.1},
         {0.1, 0.1}, {0.1, 20.0}, {0.0, 20.0}},
    }};

    ccc::core::ConductorRefs refs;
    refs.zoneIds.push_back("Z1");
    const auto panels = ccc::core::panelizeConductor(refs, model, 8.0);
    REQUIRE(panels.size() == 1);
    REQUIRE(panels.front().area == Approx(3.99));
}

TEST_CASE("BEM panelization keeps imported copper layers at distinct z", "[gerber][bem]") {
    ccc::core::Model model;
    model.layers = {
        {"F.Cu", "F.Cu", 0.10, "#c8342a", 1.0, true, true, 1.0},
        {"B.Cu", "B.Cu", 0.30, "#23964f", 1.0, true, true, 1.0},
    };
    model.zones = {
        {"ZF", "F.Cu", "/A", {{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}}},
        {"ZB", "B.Cu", "/B", {{4.0, 0.0}, {6.0, 0.0}, {6.0, 2.0}, {4.0, 2.0}}},
    };

    ccc::core::ConductorRefs top;
    top.zoneIds.push_back("ZF");
    ccc::core::ConductorRefs bottom;
    bottom.zoneIds.push_back("ZB");
    const auto topPanels = ccc::core::panelizeConductor(top, model, 1.0);
    const auto bottomPanels = ccc::core::panelizeConductor(bottom, model, 1.0);
    REQUIRE_FALSE(topPanels.empty());
    REQUIRE_FALSE(bottomPanels.empty());
    REQUIRE(topPanels.front().z == Approx(0.05));
    REQUIRE(bottomPanels.front().z == Approx(0.25));
}

TEST_CASE("KiCad import sets viewport fit bounds from imported geometry", "[kicad]") {
    const auto path = std::filesystem::temp_directory_path() / "ccc_kicad_fit_test.kicad_pcb";
    {
        std::ofstream out(path);
        out << "(kicad_pcb\n"
            << "  (layers (0 \"F.Cu\" signal))\n"
            << "  (net 1 \"A\")\n"
            << "  (segment (start 0 0) (end 10 20) (width 0.25) (layer \"F.Cu\") (net 1))\n"
            << ")\n";
    }

    auto model = ccc::io::readKicadPcb(path.string());
    REQUIRE_FALSE(model.glass.visible);
    REQUIRE(model.glass.width == Approx(10.0));
    REQUIRE(model.glass.height == Approx(20.0));

    std::filesystem::remove(path);
}

TEST_CASE("KiCad import preserves dielectric stack thickness for copper Z", "[kicad][stackup]") {
    const auto path = std::filesystem::temp_directory_path() / "ccc_kicad_stackup_test.kicad_pcb";
    {
        std::ofstream out(path);
        out << "(kicad_pcb\n"
            << "  (layers\n"
            << "    (0 \"F.Cu\" signal)\n"
            << "    (4 \"In1.Cu\" signal)\n"
            << "    (2 \"B.Cu\" signal)\n"
            << "  )\n"
            << "  (setup\n"
            << "    (stackup\n"
            << "      (layer \"F.Cu\" (type \"copper\") (thickness 0.035))\n"
            << "      (layer \"dielectric 1\" (type \"prepreg\") (thickness 0.2) (epsilon_r 4.2))\n"
            << "      (layer \"In1.Cu\" (type \"copper\") (thickness 0.035))\n"
            << "      (layer \"dielectric 2\" (type \"core\") (thickness 0.3) (epsilon_r 4.7))\n"
            << "      (layer \"B.Cu\" (type \"copper\") (thickness 0.035))\n"
            << "    )\n"
            << "  )\n"
            << "  (net 1 \"A\")\n"
            << "  (segment (start 0 0) (end 1 0) (width 0.25) (layer \"B.Cu\") (net 1))\n"
            << ")\n";
    }

    auto model = ccc::io::readKicadPcb(path.string());
    REQUIRE(model.layers.size() == 5);
    REQUIRE(model.layers[1].id == "dielectric 1");
    REQUIRE_FALSE(model.layers[1].isConductor);
    REQUIRE(model.layers[1].permittivity == Approx(4.2));

    ccc::core::ConductorRefs refs;
    refs.traceIds.push_back(model.traces.front().id);
    const auto panels = ccc::core::panelizeConductor(refs, model, 0.5);
    REQUIRE_FALSE(panels.empty());
    REQUIRE(panels.front().z == Approx(0.035 + 0.2 + 0.035 + 0.3 + 0.0175));

    std::filesystem::remove(path);
}

TEST_CASE("16ver8 Gerber exposes B.Cu circular sensor nets for capacitance", "[gerber][16ver8]") {
    const auto dir = std::filesystem::path(CCC_SOURCE_DIR)
                     / "16ver8" / "16ver8" / "jlcpcb" / "gerber";
    if (!std::filesystem::is_directory(dir)) {
        SUCCEED("16ver8 Gerber fixture is not present in this checkout");
        return;
    }

    auto model = ccc::io::readGerberProject(dir.string());
    std::set<std::string> copperLayers;
    for (const auto& l : model.layers) {
        if (l.isConductor) copperLayers.insert(l.id);
    }
    REQUIRE(copperLayers == std::set<std::string>{"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"});

    std::set<std::string> bcuSigNets;
    const std::regex sigRe(R"(^/SIG\d+$)");
    for (const auto& z : model.zones) {
        if (z.layerId == "B.Cu" && std::regex_match(z.net, sigRe)) {
            bcuSigNets.insert(z.net);
        }
    }
    REQUIRE(bcuSigNets.size() >= 16);
    REQUIRE(bcuSigNets.contains("/SIG1"));
    REQUIRE(bcuSigNets.contains("/SIG2"));

    auto zoneArea = [](const ccc::core::Zone& z) {
        double area = 0.0;
        for (std::size_t i = 0; i < z.outline.size(); ++i) {
            const auto& a = z.outline[i];
            const auto& b = z.outline[(i + 1) % z.outline.size()];
            area += a[0] * b[1] - b[0] * a[1];
        }
        return std::abs(area) * 0.5;
    };
    auto largestBcuZoneForNet = [&](const std::string& net) -> const ccc::core::Zone* {
        const ccc::core::Zone* best = nullptr;
        double bestArea = -1.0;
        for (const auto& z : model.zones) {
            if (z.layerId != "B.Cu" || z.net != net) continue;
            const double area = zoneArea(z);
            if (area > bestArea) {
                best = &z;
                bestArea = area;
            }
        }
        return best;
    };
    auto requireRoundSensor = [](const ccc::core::Zone& z) {
        REQUIRE_FALSE(z.outline.empty());
        double xMin = z.outline.front()[0];
        double xMax = xMin;
        double yMin = z.outline.front()[1];
        double yMax = yMin;
        for (const auto& p : z.outline) {
            xMin = std::min(xMin, p[0]);
            xMax = std::max(xMax, p[0]);
            yMin = std::min(yMin, p[1]);
            yMax = std::max(yMax, p[1]);
        }
        const double w = xMax - xMin;
        const double h = yMax - yMin;
        REQUIRE(w == Approx(h).epsilon(0.03));
        REQUIRE(w == Approx(12.2).margin(0.4));
    };
    for (const auto& net : bcuSigNets) {
        REQUIRE(largestBcuZoneForNet(net) != nullptr);
        requireRoundSensor(*largestBcuZoneForNet(net));
    }
    std::size_t bcuGndHoles = 0;
    for (const auto& z : model.zones) {
        if (z.layerId == "B.Cu" && z.net == "GND") bcuGndHoles += z.holes.size();
    }
    REQUIRE(bcuGndHoles >= 16);

    auto bboxCenter = [](const ccc::core::Zone& z) {
        double xMin = z.outline.front()[0];
        double xMax = xMin;
        double yMin = z.outline.front()[1];
        double yMax = yMin;
        for (const auto& p : z.outline) {
            xMin = std::min(xMin, p[0]);
            xMax = std::max(xMax, p[0]);
            yMin = std::min(yMin, p[1]);
            yMax = std::max(yMax, p[1]);
        }
        return std::array<double, 2>{(xMin + xMax) * 0.5, (yMin + yMax) * 0.5};
    };
    for (const auto& net : bcuSigNets) {
        const auto* sig = largestBcuZoneForNet(net);
        REQUIRE(sig != nullptr);
        const auto c = bboxCenter(*sig);
        bool gndContainsSensorCenter = false;
        for (const auto& z : model.zones) {
            if (z.layerId == "B.Cu" && z.net == "GND"
                && ccc::core::pointInZoneCopper(z, c[0], c[1])) {
                gndContainsSensorCenter = true;
                break;
            }
        }
        INFO(net);
        REQUIRE_FALSE(gndContainsSensorCenter);
    }

    auto refsForNet = [&model](const std::string& net) {
        ccc::core::ConductorRefs refs;
        for (const auto& z : model.zones) {
            if (z.net == net) refs.zoneIds.push_back(z.id);
        }
        return refs;
    };
    for (const auto& net : bcuSigNets) {
        const auto panels = ccc::core::panelizeConductor(refsForNet(net), model, 1.0);
        INFO(net);
        REQUIRE(panels.size() >= 10);
        REQUIRE(panels.size() < 300);
    }

    const auto panels1 = ccc::core::panelizeConductor(refsForNet("/SIG1"), model, 1.0);
    const auto panels2 = ccc::core::panelizeConductor(refsForNet("/SIG2"), model, 1.0);
    REQUIRE(panels1.size() >= 10);
    REQUIRE(panels2.size() >= 10);
    REQUIRE(panels1.size() < 300);
    REQUIRE(panels2.size() < 300);

    ccc::core::BemOptions opts;
    opts.panelSize = 1.0;
    opts.epsEff = 1.0;
    opts.imageShield = false;
    opts.solver = ccc::core::BemSolver::DirectLU;
    const auto result = ccc::core::computeMutualCapacitance(panels1, panels2, model, opts);
    REQUIRE(result.Cm > 0.0);
}

TEST_CASE("16ver8 B.Cu sensor net pair capacitance matrix computes", "[gerber][16ver8][sig-cap-matrix][.]") {
    const auto dir = std::filesystem::path(CCC_SOURCE_DIR)
                     / "16ver8" / "16ver8" / "jlcpcb" / "gerber";
    REQUIRE(std::filesystem::is_directory(dir));

    auto model = ccc::io::readGerberProject(dir.string());
    const std::regex sigRe(R"(^/SIG\d+$)");

    std::map<std::string, ccc::core::ConductorRefs> refsByNet;
    for (const auto& z : model.zones) {
        if (z.layerId == "B.Cu" && std::regex_match(z.net, sigRe)) {
            refsByNet[z.net].zoneIds.push_back(z.id);
        }
    }
    REQUIRE(refsByNet.size() == 16);

    auto sigNumber = [](const std::string& net) {
        return std::stoi(net.substr(4));
    };
    std::vector<std::string> nets;
    for (const auto& [net, _] : refsByNet) nets.push_back(net);
    std::sort(nets.begin(), nets.end(), [&](const auto& a, const auto& b) {
        return sigNumber(a) < sigNumber(b);
    });

    std::map<std::string, std::vector<ccc::core::Panel>> panelsByNet;
    constexpr double kPanelSizeMm = 1.0;
    for (const auto& net : nets) {
        auto panels = ccc::core::panelizeConductor(refsByNet[net], model, kPanelSizeMm);
        REQUIRE(panels.size() >= 10);
        panelsByNet.emplace(net, std::move(panels));
    }

    const auto outDir = std::filesystem::path(CCC_SOURCE_DIR) / "artifacts" / "cap";
    std::filesystem::create_directories(outDir);
    std::ofstream csv(outDir / "16ver8_bcu_sig_caps.csv");
    csv << "net_a,net_b,panels_a,panels_b,cm_f,cm_pf,assemble_ms,solve_ms\n";

    ccc::core::BemOptions opts;
    opts.panelSize = kPanelSizeMm;
    opts.epsEff = 1.0;
    opts.imageShield = false;
    opts.solver = ccc::core::BemSolver::DirectLU;

    int computed = 0;
    double minCm = std::numeric_limits<double>::infinity();
    double maxCm = 0.0;
    for (std::size_t i = 0; i < nets.size(); ++i) {
        for (std::size_t j = i + 1; j < nets.size(); ++j) {
            const auto& a = panelsByNet[nets[i]];
            const auto& b = panelsByNet[nets[j]];
            const auto result = ccc::core::computeMutualCapacitance(a, b, model, opts);
            INFO(nets[i] << " vs " << nets[j]);
            REQUIRE(std::isfinite(result.Cm));
            REQUIRE(result.Cm > 0.0);
            minCm = std::min(minCm, result.Cm);
            maxCm = std::max(maxCm, result.Cm);
            ++computed;
            csv << nets[i] << ',' << nets[j] << ','
                << a.size() << ',' << b.size() << ','
                << result.Cm << ',' << result.Cm * 1.0e12 << ','
                << result.assembleMs << ',' << result.solveMs << '\n';
        }
    }

    REQUIRE(computed == 120);
    std::cout << "16ver8 B.Cu SIG pair capacitance: " << computed
              << " pairs, min=" << minCm * 1.0e12
              << " pF, max=" << maxCm * 1.0e12
              << " pF, csv=" << (outDir / "16ver8_bcu_sig_caps.csv").string() << '\n';
}

TEST_CASE("16ver8 B.Cu sensor BiCGStab matrix is not constant", "[gerber][16ver8][sig-cap-bicg][.]") {
    const auto dir = std::filesystem::path(CCC_SOURCE_DIR)
                     / "16ver8" / "16ver8" / "jlcpcb" / "gerber";
    REQUIRE(std::filesystem::is_directory(dir));

    auto model = ccc::io::readGerberProject(dir.string());
    const std::regex sigRe(R"(^/SIG\d+$)");
    std::map<std::string, ccc::core::ConductorRefs> refsByNet;
    for (const auto& z : model.zones) {
        if (z.layerId == "B.Cu" && std::regex_match(z.net, sigRe)) {
            refsByNet[z.net].zoneIds.push_back(z.id);
        }
    }
    REQUIRE(refsByNet.size() == 16);

    auto sigNumber = [](const std::string& net) {
        return std::stoi(net.substr(4));
    };
    std::vector<std::string> nets;
    for (const auto& [net, _] : refsByNet) nets.push_back(net);
    std::sort(nets.begin(), nets.end(), [&](const auto& a, const auto& b) {
        return sigNumber(a) < sigNumber(b);
    });

    std::map<std::string, std::vector<ccc::core::Panel>> panelsByNet;
    for (const auto& net : nets) {
        panelsByNet.emplace(net, ccc::core::panelizeConductor(refsByNet[net], model, 1.0));
    }

    ccc::core::BemOptions opts;
    opts.panelSize = 1.0;
    opts.epsEff = 1.0;
    opts.imageShield = false;
    opts.solver = ccc::core::BemSolver::BiCGStab;
    opts.iterTol = 1e-8;
    opts.iterMaxIters = 2000;

    const auto outDir = std::filesystem::path(CCC_SOURCE_DIR) / "artifacts" / "cap";
    std::filesystem::create_directories(outDir);
    std::ofstream csv(outDir / "16ver8_bcu_sig_caps_bicg.csv");
    csv << "net_a,net_b,panels_a,panels_b,cm_f,cm_pf,iters_a,iters_b,residual_a,residual_b\n";

    std::vector<double> values;
    for (std::size_t i = 0; i < nets.size(); ++i) {
        for (std::size_t j = i + 1; j < nets.size(); ++j) {
            const auto& a = panelsByNet[nets[i]];
            const auto& b = panelsByNet[nets[j]];
            const auto result = ccc::core::computeMutualCapacitance(a, b, model, opts);
            INFO(nets[i] << " vs " << nets[j]);
            REQUIRE(std::isfinite(result.Cm));
            REQUIRE(result.Cm > 0.0);
            REQUIRE(result.residual[0] < 1e-4);
            REQUIRE(result.residual[1] < 1e-4);
            values.push_back(result.Cm);
            csv << nets[i] << ',' << nets[j] << ','
                << a.size() << ',' << b.size() << ','
                << result.Cm << ',' << result.Cm * 1.0e12 << ','
                << result.iters[0] << ',' << result.iters[1] << ','
                << result.residual[0] << ',' << result.residual[1] << '\n';
        }
    }

    auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    REQUIRE(values.size() == 120);
    REQUIRE((*maxIt - *minIt) > 1e-14);
}

TEST_CASE("16ver8 all Gerber net pair capacitance smoke computes", "[gerber][16ver8][all-net-caps][.]") {
    const auto dir = std::filesystem::path(CCC_SOURCE_DIR)
                     / "16ver8" / "16ver8" / "jlcpcb" / "gerber";
    REQUIRE(std::filesystem::is_directory(dir));

    auto model = ccc::io::readGerberProject(dir.string());
    std::map<std::string, ccc::core::ConductorRefs> refsByNet;
    for (const auto& p : model.pads) {
        if (!p.net.empty()) refsByNet[p.net].padIds.push_back(p.id);
    }
    for (const auto& t : model.traces) {
        if (!t.net.empty()) refsByNet[t.net].traceIds.push_back(t.id);
    }
    for (const auto& z : model.zones) {
        if (!z.net.empty()) refsByNet[z.net].zoneIds.push_back(z.id);
    }
    REQUIRE(refsByNet.size() >= 16);

    std::map<std::string, std::vector<ccc::core::Panel>> panelsByNet;
    auto panelizeAdaptive = [&](const ccc::core::ConductorRefs& refs) {
        std::vector<ccc::core::Panel> panels;
        for (double panelSize : {8.0, 4.0, 2.0, 1.0}) {
            panels = ccc::core::panelizeConductor(refs, model, panelSize);
            if (panels.size() >= 10 || panelSize <= 1.0) break;
        }
        return panels;
    };
    for (const auto& [net, refs] : refsByNet) {
        auto panels = panelizeAdaptive(refs);
        INFO(net);
        REQUIRE_FALSE(panels.empty());
        panelsByNet.emplace(net, std::move(panels));
    }

    std::vector<std::string> nets;
    nets.reserve(panelsByNet.size());
    for (const auto& [net, _] : panelsByNet) nets.push_back(net);

    const auto outDir = std::filesystem::path(CCC_SOURCE_DIR) / "artifacts" / "cap";
    std::filesystem::create_directories(outDir);
    std::ofstream csv(outDir / "16ver8_all_net_caps_smoke.csv");
    csv << "net_a,net_b,panels_a,panels_b,cm_f,cm_pf,assemble_ms,solve_ms\n";

    ccc::core::BemOptions opts;
    opts.epsEff = 1.0;
    opts.imageShield = false;
    opts.solver = ccc::core::BemSolver::DirectLU;

    int computed = 0;
    double minCm = std::numeric_limits<double>::infinity();
    double maxCm = 0.0;
    std::vector<std::string> invalidPairs;
    for (std::size_t i = 0; i < nets.size(); ++i) {
        for (std::size_t j = i + 1; j < nets.size(); ++j) {
            const auto& a = panelsByNet[nets[i]];
            const auto& b = panelsByNet[nets[j]];
            const auto result = ccc::core::computeMutualCapacitance(a, b, model, opts);
            INFO(nets[i] << " vs " << nets[j]);
            if (!std::isfinite(result.Cm) || result.Cm <= 0.0) {
                invalidPairs.push_back(nets[i] + " vs " + nets[j] + " = "
                                       + std::to_string(result.Cm * 1.0e12) + " pF");
            }
            minCm = std::min(minCm, result.Cm);
            maxCm = std::max(maxCm, result.Cm);
            ++computed;
            csv << nets[i] << ',' << nets[j] << ','
                << a.size() << ',' << b.size() << ','
                << result.Cm << ',' << result.Cm * 1.0e12 << ','
                << result.assembleMs << ',' << result.solveMs << '\n';
        }
    }

    const int expectedPairs = int(nets.size() * (nets.size() - 1) / 2);
    REQUIRE(computed == expectedPairs);
    if (!invalidPairs.empty()) {
        std::cout << "Invalid capacitance pairs: " << invalidPairs.size() << '\n';
        for (std::size_t i = 0; i < std::min<std::size_t>(invalidPairs.size(), 20); ++i) {
            std::cout << "  " << invalidPairs[i] << '\n';
        }
    }
    REQUIRE(invalidPairs.empty());
    std::cout << "16ver8 all Gerber net pair smoke capacitance: " << computed
              << " pairs across " << nets.size()
              << " nets, min=" << minCm * 1.0e12
              << " pF, max=" << maxCm * 1.0e12
              << " pF, csv=" << (outDir / "16ver8_all_net_caps_smoke.csv").string() << '\n';
}

TEST_CASE("16ver8 selected Gerber net pair convergence diagnostic", "[gerber][16ver8][pair-cap-diagnostic][.]") {
    const auto dir = std::filesystem::path(CCC_SOURCE_DIR)
                     / "16ver8" / "16ver8" / "jlcpcb" / "gerber";
    REQUIRE(std::filesystem::is_directory(dir));

    auto model = ccc::io::readGerberProject(dir.string());
    auto refsForNet = [&](const std::string& net) {
        ccc::core::ConductorRefs refs;
        for (const auto& p : model.pads) {
            if (p.net == net) refs.padIds.push_back(p.id);
        }
        for (const auto& t : model.traces) {
            if (t.net == net) refs.traceIds.push_back(t.id);
        }
        for (const auto& z : model.zones) {
            if (z.net == net) refs.zoneIds.push_back(z.id);
        }
        return refs;
    };

    ccc::core::BemOptions opts;
    opts.epsEff = 1.0;
    opts.imageShield = false;
    opts.solver = ccc::core::BemSolver::DirectLU;

    for (const auto& [netA, netB] : std::vector<std::pair<std::string, std::string>>{
             {"/+INB1", "Net-(C44-Pad1)"},
             {"/+INB1", "/SIG1"},
         }) {
        for (double panelSize : {8.0, 4.0, 2.0, 1.0}) {
            const auto pa = ccc::core::panelizeConductor(refsForNet(netA), model, panelSize);
            const auto pb = ccc::core::panelizeConductor(refsForNet(netB), model, panelSize);
            opts.panelSize = panelSize;
            const auto result = ccc::core::computeMutualCapacitance(pa, pb, model, opts);
            std::cout << netA << " vs " << netB
                      << " panelSize=" << panelSize
                      << " panels=" << pa.size() << '+' << pb.size()
                      << " Cm=" << result.Cm * 1.0e12 << " pF\n";
            REQUIRE(std::isfinite(result.Cm));
        }
    }
}
