#include "JsonIo.hpp"

#include "core/FpcPad.hpp"
#include "core/GlassPlate.hpp"
#include "core/Layer.hpp"
#include "core/Meta.hpp"
#include "core/Pad.hpp"
#include "core/Trace.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

// ----------------------------------------------------------------------------
// Per-type to_json / from_json — kept in ccc::core so ADL finds them when
// nlohmann::json serialises a vector<T>. Headers stay free of nlohmann.
// ----------------------------------------------------------------------------
namespace ccc::core {

void to_json(nlohmann::json& j, const Meta& m) {
    j = {{"type", m.type}, {"version", m.version}};
    if (!m.createdAt.empty()) j["createdAt"] = m.createdAt;
}
void from_json(const nlohmann::json& j, Meta& m) {
    m.type      = j.value("type", std::string{"16Sensor3D"});
    m.version   = j.value("version", std::string{"1.0"});
    m.createdAt = j.value("createdAt", std::string{});
}

void to_json(nlohmann::json& j, const GlassPlate& g) {
    j = {
        {"width", g.width}, {"height", g.height}, {"thickness", g.thickness},
        {"color", g.color}, {"opacity", g.opacity}, {"visible", g.visible},
    };
}
void from_json(const nlohmann::json& j, GlassPlate& g) {
    g.width     = j.value("width", 50.0);
    g.height    = j.value("height", 60.0);
    g.thickness = j.value("thickness", 5.0);
    g.color     = j.value("color", std::string{"#bcdde8"});
    g.opacity   = j.value("opacity", 0.30);
    g.visible   = j.value("visible", true);
}

void to_json(nlohmann::json& j, const Layer& l) {
    j = {
        {"id", l.id}, {"name", l.name}, {"thickness", l.thickness},
        {"color", l.color}, {"opacity", l.opacity}, {"visible", l.visible},
    };
}
void from_json(const nlohmann::json& j, Layer& l) {
    l.id        = j.at("id").get<std::string>();
    l.name      = j.value("name", l.id);
    l.thickness = j.value("thickness", 0.05);
    l.color     = j.value("color", std::string{"#888888"});
    l.opacity   = j.value("opacity", 1.0);
    l.visible   = j.value("visible", true);
}

void to_json(nlohmann::json& j, const Pad& p) {
    j = {
        {"id", p.id}, {"x", p.x}, {"y", p.y},
        {"shape", padShapeToString(p.shape)},
        {"size", p.size}, {"size2", p.size2},
    };
}
void from_json(const nlohmann::json& j, Pad& p) {
    p.id    = j.at("id").get<std::string>();
    p.x     = j.at("x").get<double>();
    p.y     = j.at("y").get<double>();
    p.shape = padShapeFromString(j.value("shape", std::string{"circle"}));
    p.size  = j.value("size", 6.0);
    p.size2 = j.value("size2", p.size);
}

void to_json(nlohmann::json& j, const FpcPad& f) {
    j = {{"id", f.id}, {"x", f.x}, {"y", f.y},
         {"width", f.width}, {"height", f.height}};
}
void from_json(const nlohmann::json& j, FpcPad& f) {
    f.id     = j.at("id").get<std::string>();
    f.x      = j.at("x").get<double>();
    f.y      = j.at("y").get<double>();
    f.width  = j.value("width", 1.0);
    f.height = j.value("height", 4.0);
}

void to_json(nlohmann::json& j, const Trace& t) {
    j = {{"id", t.id}, {"from", t.from}, {"to", t.to},
         {"width", t.width}, {"waypoints", t.waypoints}};
}
void from_json(const nlohmann::json& j, Trace& t) {
    t.id        = j.at("id").get<std::string>();
    t.from      = j.at("from").get<std::string>();
    t.to        = j.at("to").get<std::string>();
    t.width     = j.value("width", 0.4);
    t.waypoints = j.value("waypoints", std::vector<Point2>{});
}

}  // namespace ccc::core

// ----------------------------------------------------------------------------
// Top-level model <-> json
// ----------------------------------------------------------------------------
namespace ccc::io {

nlohmann::json modelToJson(const ccc::core::Model& m) {
    nlohmann::json j;
    j["meta"]    = m.meta;
    j["glass"]   = m.glass;
    j["layers"]  = m.layers;
    j["pads"]    = m.pads;
    j["fpcPads"] = m.fpcPads;
    j["traces"]  = m.traces;
    return j;
}

ccc::core::Model modelFromJson(const nlohmann::json& j) {
    using namespace ccc::core;
    Model m;
    m.meta    = j.value("meta", Meta{});
    m.glass   = j.value("glass", GlassPlate{});
    m.layers  = j.value("layers", Model::defaultLayers());
    m.pads    = j.value("pads", std::vector<Pad>{});
    m.fpcPads = j.value("fpcPads", std::vector<FpcPad>{});
    m.traces  = j.value("traces", std::vector<Trace>{});
    return m;
}

void writeModelFile(const ccc::core::Model& m, const std::string& path, int indent) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("failed to open for write: " + path);
    out << modelToJson(m).dump(indent) << '\n';
    if (!out) throw std::runtime_error("write failed: " + path);
}

ccc::core::Model readModelFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("failed to open for read: " + path);
    nlohmann::json j;
    in >> j;
    return modelFromJson(j);
}

}  // namespace ccc::io
