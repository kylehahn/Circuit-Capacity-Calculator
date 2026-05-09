#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Model.hpp"
#include "io/JsonIo.hpp"

#include <nlohmann/json.hpp>

using namespace ccc::core;
using namespace ccc::io;
using Catch::Approx;

TEST_CASE("model JSON round-trip preserves every field", "[json]") {
    Model m1 = Model::makeDefault();
    auto    j  = modelToJson(m1);
    Model m2 = modelFromJson(j);
    REQUIRE(m1 == m2);
}

TEST_CASE("non-default values round-trip", "[json]") {
    Model m;
    m.meta.createdAt = "2026-05-09T07:00:00Z";
    m.glass = {120.0, 80.0, 5.5, "#abcdef", 0.42, false};
    m.layers = {
        {"shield", "Shield", 0.04, "#f0a040", 0.85, true},
        {"sensor", "Sensor", 0.05, "#3a3a3a", 1.00, false},
    };
    m.pads = {
        {"PA", -3.5, 4.25, PadShape::Ellipse,       7.0, 5.0},
        {"PB",  0.0, 0.00, PadShape::RoundedSquare, 8.0, 1.5},
    };
    m.fpcPads = {{"FX", -10.0, -25.0, 1.5, 4.0}};
    m.traces  = {
        {"T1", "PA", "FX", 0.35, {{-3.5, 0.0}, {-10.0, 0.0}}},
    };

    auto j = modelToJson(m);
    Model back = modelFromJson(j);
    REQUIRE(back == m);
    REQUIRE(j.at("pads").at(0).at("shape") == "ellipse");
    REQUIRE(j.at("pads").at(1).at("shape") == "roundedSquare");
}

TEST_CASE("JSON shape mirrors the JS prototype field names", "[json]") {
    auto j = modelToJson(Model::makeDefault());
    // Top-level keys
    REQUIRE(j.contains("meta"));
    REQUIRE(j.contains("glass"));
    REQUIRE(j.contains("layers"));
    REQUIRE(j.contains("pads"));
    REQUIRE(j.contains("fpcPads"));
    REQUIRE(j.contains("traces"));
    // Pad has the same keys as the JS prototype's pad
    auto& p = j.at("pads").at(0);
    REQUIRE(p.contains("id"));
    REQUIRE(p.contains("x"));
    REQUIRE(p.contains("y"));
    REQUIRE(p.contains("shape"));
    REQUIRE(p.contains("size"));
    REQUIRE(p.contains("size2"));
    // Trace waypoints serialise as [[x,y], ...]
    auto& t = j.at("traces").at(0);
    REQUIRE(t.at("waypoints").is_array());
    REQUIRE(t.at("waypoints").at(0).is_array());
    REQUIRE(t.at("waypoints").at(0).size() == 2);
}

TEST_CASE("missing optional fields fall back to sensible defaults", "[json]") {
    nlohmann::json minimal = {
        {"meta", {{"type", "16Sensor3D"}, {"version", "1.0"}}},
        {"glass", {{"width", 50}, {"height", 60}, {"thickness", 5}}},
        {"layers", {{{"id", "sensor"}, {"thickness", 0.05}}}},
        {"pads", {{{"id", "P1"}, {"x", 0}, {"y", 0}, {"size", 6}}}},
        {"fpcPads", nlohmann::json::array()},
        {"traces", nlohmann::json::array()},
    };
    Model m = modelFromJson(minimal);
    REQUIRE(m.pads.size() == 1);
    REQUIRE(m.pads[0].shape == PadShape::Circle);    // default
    REQUIRE(m.pads[0].size2 == Approx(6.0));         // size2 falls back to size
    REQUIRE(m.glass.color == "#bcdde8");             // default
}
