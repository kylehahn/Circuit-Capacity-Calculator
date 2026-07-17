#include "GerberProject.hpp"

#include "ExcellonIo.hpp"
#include "GbrJobIo.hpp"
#include "GerberIo.hpp"
#include "KicadPcbIo.hpp"
#include "core/Connectivity.hpp"

#include <algorithm>
#include <cctype>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <clipper2/clipper.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

namespace ccc::io {

namespace fs = std::filesystem;

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

bool isGerberExt(const fs::path& p) {
    const std::string e = upper(p.extension().string());
    return e == ".GBR" || e == ".GTL" || e == ".GBL"
           || e == ".GKO" || e == ".GM1"
           || std::regex_match(e, std::regex(R"(\.G\d+)", std::regex::icase))
           || std::regex_match(e, std::regex(R"(\.GP\d+)", std::regex::icase));
}

bool looksMechanicalLayer(const std::string& id) {
    const std::string u = upper(id);
    return u.find("EDGE") != std::string::npos
           || u.find("CUT") != std::string::npos
           || u.find("PROFILE") != std::string::npos
           || u.find("OUTLINE") != std::string::npos
           || u.find("MECH") != std::string::npos
           || u.find("DRAWING") != std::string::npos
           || u.find("SILK") != std::string::npos
           || u.find("MASK") != std::string::npos
           || u.find("PASTE") != std::string::npos;
}

bool looksCopperLayer(const std::string& id) {
    const std::string u = upper(id);
    if (looksMechanicalLayer(id)) return false;
    return u == "F.CU" || u == "B.CU"
           || std::regex_match(u, std::regex(R"(IN\d+\.CU)", std::regex::icase))
           || std::regex_match(u, std::regex(R"(L\d+\.CU)", std::regex::icase))
           || u.find("_CU") != std::string::npos
           || u.find("-CU") != std::string::npos
           || u.find(".CU") != std::string::npos;
}

int layerRank(const std::string& id) {
    const std::string u = upper(id);
    if (u == "F.CU") return 0;
    if (u == "B.CU") return 100000;
    const auto in = u.find("IN");
    if (in != std::string::npos) {
        try {
            return 100 + std::stoi(u.substr(in + 2));
        } catch (...) {
        }
    }
    return 1000;
}

std::string colorForLayer(const std::string& id) {
    const std::string u = upper(id);
    if (u == "F.CU") return "#c8342a";
    if (u == "B.CU") return "#23964f";
    if (u.find("IN1") != std::string::npos) return "#d1a800";
    if (u.find("IN2") != std::string::npos) return "#2f72d6";
    return "#9a67c7";
}

double copperThicknessFromJob(const GbrJobInfo& job, const std::string& id) {
    for (const auto& l : job.layers) {
        if (l.id == id && l.thickness > 0.0) return l.thickness;
    }
    return 0.035;
}

struct PolygonWithHoles {
    std::vector<ccc::core::Point2> outline;
    std::vector<std::vector<ccc::core::Point2>> holes;
};

std::vector<ccc::core::Point2> toPointPath(const Clipper2Lib::PathD& path) {
    std::vector<ccc::core::Point2> out;
    out.reserve(path.size());
    for (const auto& p : path) out.push_back({p.x, p.y});
    return out;
}

void collectSolidPolygons(const Clipper2Lib::PolyPathD& parent,
                          std::vector<PolygonWithHoles>& out) {
    for (std::size_t i = 0; i < parent.Count(); ++i) {
        const auto* child = parent.Child(i);
        if (!child) continue;
        if (!child->IsHole() && child->Polygon().size() >= 3) {
            PolygonWithHoles poly;
            poly.outline = toPointPath(child->Polygon());
            for (std::size_t h = 0; h < child->Count(); ++h) {
                const auto* hole = child->Child(h);
                if (hole && hole->IsHole() && hole->Polygon().size() >= 3) {
                    poly.holes.push_back(toPointPath(hole->Polygon()));
                }
            }
            out.push_back(std::move(poly));
        }
        collectSolidPolygons(*child, out);
    }
}

std::vector<PolygonWithHoles> unionPolygons(
    const std::vector<std::vector<ccc::core::Point2>>& polygons) {
    Clipper2Lib::PathsD subjects;
    subjects.reserve(polygons.size());
    for (const auto& poly : polygons) {
        if (poly.size() < 3) continue;
        Clipper2Lib::PathD path;
        path.reserve(poly.size());
        for (const auto& p : poly) path.emplace_back(p[0], p[1]);
        subjects.push_back(std::move(path));
    }
    if (subjects.empty()) return {};

    // KiCad Gerbers are emitted in millimetres. Six decimal places preserves
    // micron-scale coordinates while letting Clipper2 merge touching flashes,
    // strokes, and regions into stable copper islands.
    Clipper2Lib::PolyTreeD solution;
    Clipper2Lib::BooleanOp(Clipper2Lib::ClipType::Union,
                           Clipper2Lib::FillRule::NonZero,
                           subjects, {}, solution, 6);
    std::vector<PolygonWithHoles> out;
    collectSolidPolygons(solution, out);
    return out;
}

void addMissingCopperLayers(ccc::core::Model& model,
                            const std::vector<std::string>& layerIds,
                            const GbrJobInfo& job) {
    for (const auto& id : layerIds) {
        if (model.findLayer(id)) continue;
        const bool defaultVisible = (id == "F.Cu") || layerIds.size() == 1;
        model.layers.push_back({id, id, copperThicknessFromJob(job, id),
                                colorForLayer(id), 1.0, defaultVisible, true, 1.0});
    }
    std::sort(model.layers.begin(), model.layers.end(), [](const auto& a, const auto& b) {
        return layerRank(a.id) < layerRank(b.id);
    });
}

std::optional<std::vector<ccc::core::Layer>> kicadStackupLayers(
    const fs::path& kicadPcbPath,
    const std::vector<std::string>& copperLayerIds) {
    if (kicadPcbPath.empty()) return std::nullopt;
    if (!fs::is_regular_file(kicadPcbPath))
        throw std::runtime_error("Gerber project: KiCad PCB file does not exist: "
                                 + kicadPcbPath.string());
    std::unordered_set<std::string> copperSet(copperLayerIds.begin(), copperLayerIds.end());
    auto kicad = readKicadPcb(kicadPcbPath.string());
    std::unordered_set<std::string> found;
    for (const auto& l : kicad.layers) {
        if (l.isConductor && copperSet.contains(l.id)) found.insert(l.id);
    }
    if (found.size() == copperSet.size()) return std::move(kicad.layers);
    return std::nullopt;
}

void applyPhysicalStackupIfAvailable(ccc::core::Model& model,
                                      const fs::path& kicadPcbPath,
                                      const std::vector<std::string>& copperLayerIds) {
    const auto stackup = kicadStackupLayers(kicadPcbPath, copperLayerIds);
    if (!stackup) {
        if (!kicadPcbPath.empty()) {
            throw std::runtime_error(
                "Gerber project: KiCad PCB stackup does not match Gerber copper layers: "
                + kicadPcbPath.string());
        }
        return;
    }

    std::unordered_map<std::string, ccc::core::Layer> currentCopper;
    for (const auto& l : model.layers) {
        if (l.isConductor) currentCopper.emplace(l.id, l);
    }

    std::unordered_set<std::string> copperSet(copperLayerIds.begin(), copperLayerIds.end());
    std::vector<ccc::core::Layer> layers;
    layers.reserve(stackup->size());
    for (auto layer : *stackup) {
        if (layer.isConductor) {
            if (!copperSet.contains(layer.id)) continue;
            if (auto it = currentCopper.find(layer.id); it != currentCopper.end()) {
                layer.color = it->second.color;
                layer.visible = it->second.visible;
                layer.opacity = it->second.opacity;
            } else {
                layer.color = colorForLayer(layer.id);
                layer.visible = (layer.id == "F.Cu");
                layer.opacity = 1.0;
            }
        } else if (layer.thickness <= 0.0) {
            continue;
        } else {
            layer.visible = false;
            layer.opacity = 0.18;
        }
        layers.push_back(std::move(layer));
    }

    std::unordered_set<std::string> found;
    for (const auto& l : layers) {
        if (l.isConductor) found.insert(l.id);
    }
    if (found.size() == copperSet.size()) model.layers = std::move(layers);
}

}  // namespace

ccc::core::Model readGerberProject(const std::string& folderPath,
                                   const std::string& kicadPcbPath) {
    if (!fs::is_directory(folderPath))
        throw std::runtime_error("Gerber project: folder does not exist: " + folderPath);

    GbrJobInfo job;
    for (const auto& e : fs::directory_iterator(folderPath)) {
        if (upper(e.path().extension().string()) == ".GBRJOB") {
            job = readGbrJob(e.path().string());
            break;
        }
    }

    std::vector<DrillHit> drills;
    for (const auto& e : fs::directory_iterator(folderPath)) {
        if (upper(e.path().extension().string()) == ".DRL") {
            auto more = readExcellonDrillFile(e.path().string());
            drills.insert(drills.end(), more.begin(), more.end());
        }
    }

    ccc::core::Model model;
    model.meta.type = "GerberProject";
    model.meta.version = "1.0";
    model.glass.visible = false;
    model.glass.thickness = 0.0;
    model.glass.width = 0.0;
    model.glass.height = 0.0;

    std::vector<std::string> copperLayerIds;
    using LayerNetKey = std::pair<std::string, std::string>;
    std::map<LayerNetKey, std::vector<std::vector<ccc::core::Point2>>> rawPolygons;
    for (const auto& [_, layerId] : job.fileToLayer) {
        if (looksCopperLayer(layerId)) copperLayerIds.push_back(layerId);
    }
    for (const auto& l : job.layers) {
        if (l.isCopper && !l.id.empty()) copperLayerIds.push_back(l.id);
    }
    int zoneIndex = 1;
    for (const auto& e : fs::directory_iterator(folderPath)) {
        if (!e.is_regular_file() || !isGerberExt(e.path())) continue;
        const std::string filename = e.path().filename().string();
        std::string layerId;
        if (auto it = job.fileToLayer.find(filename); it != job.fileToLayer.end())
            layerId = it->second;
        else
            layerId = inferGerberLayerId(e.path().string());
        if (!looksCopperLayer(layerId)) continue;

        auto layer = readGerberLayer(e.path().string(), layerId);
        if (layer.polygons.empty()) continue;
        copperLayerIds.push_back(layerId);
        for (const auto& poly : layer.polygons) {
            if (poly.outline.size() < 3) continue;
            rawPolygons[{layerId, poly.net}].push_back(poly.outline);
        }
    }

    for (const auto& [key, polygons] : rawPolygons) {
        const auto& [layerId, net] = key;
        for (auto& poly : unionPolygons(polygons)) {
            ccc::core::Zone z;
            z.id = "GZ" + std::to_string(zoneIndex++);
            z.layerId = layerId;
            z.net = net;
            z.outline = std::move(poly.outline);
            z.holes = std::move(poly.holes);
            model.zones.push_back(std::move(z));
        }
    }

    std::sort(copperLayerIds.begin(), copperLayerIds.end(),
              [](const auto& a, const auto& b) { return layerRank(a) < layerRank(b); });
    copperLayerIds.erase(std::unique(copperLayerIds.begin(), copperLayerIds.end()),
                         copperLayerIds.end());

    if (model.zones.empty()) throw std::runtime_error("Gerber project: no copper polygons found");
    addMissingCopperLayers(model, copperLayerIds, job);
    applyPhysicalStackupIfAvailable(model, fs::path(kicadPcbPath), copperLayerIds);

    double x0 = std::numeric_limits<double>::max();
    double y0 = std::numeric_limits<double>::max();
    double x1 = std::numeric_limits<double>::lowest();
    double y1 = std::numeric_limits<double>::lowest();
    for (const auto& z : model.zones) {
        for (const auto& p : z.outline) {
            x0 = std::min(x0, p[0]);
            x1 = std::max(x1, p[0]);
            y0 = std::min(y0, p[1]);
            y1 = std::max(y1, p[1]);
        }
        for (const auto& hole : z.holes) {
            for (const auto& p : hole) {
                x0 = std::min(x0, p[0]);
                x1 = std::max(x1, p[0]);
                y0 = std::min(y0, p[1]);
                y1 = std::max(y1, p[1]);
            }
        }
    }
    const double cx = (x0 + x1) * 0.5;
    const double cy = (y0 + y1) * 0.5;
    for (auto& z : model.zones) {
        for (auto& p : z.outline) {
            p[0] -= cx;
            p[1] -= cy;
        }
        for (auto& hole : z.holes) {
            for (auto& p : hole) {
                p[0] -= cx;
                p[1] -= cy;
            }
        }
    }
    for (auto& d : drills) {
        d.x -= cx;
        d.y -= cy;
    }
    model.glass.width = x1 - x0;
    model.glass.height = y1 - y0;

    std::vector<ccc::core::ViaConnection> vias;
    vias.reserve(drills.size());
    for (const auto& d : drills)
        vias.push_back({d.x, d.y, d.diameter, d.fromLayer, d.toLayer});
    ccc::core::assignConnectedCopperObjects(model, vias);

    return model;
}

}  // namespace ccc::io
