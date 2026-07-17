#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Model.hpp"

using namespace ccc::core;
using Catch::Approx;

TEST_CASE("default model is a 4x4 grid with 16 pads / 16 fpc / 16 traces", "[model]") {
    auto m = Model::makeDefault();
    REQUIRE(m.pads.size()    == 16);
    REQUIRE(m.fpcPads.size() == 16);
    REQUIRE(m.traces.size()  == 16);
    REQUIRE(m.layers.size()  == 4);
    REQUIRE(m.findLayer("sensor") != nullptr);
}

TEST_CASE("default geometry: pad layout and glass dimensions", "[model]") {
    auto m = Model::makeDefault();
    // Glass: routingH = max(6, 16*0.7+1) = 12.2 → glassH = 90+12+12+4+12.2 = 130.2
    REQUIRE(m.glass.width  == Approx(114.0));
    REQUIRE(m.glass.height == Approx(130.2));

    // Pad row 0 at y = startY = -36.9; row 3 at y = startY + 3*30 = 53.1.
    REQUIRE(m.pads.front().id == "P1");
    REQUIRE(m.pads.front().x  == Approx(-45.0));
    REQUIRE(m.pads.front().y  == Approx(-36.9));
    REQUIRE(m.pads.back().id  == "P16");
    REQUIRE(m.pads.back().x   == Approx( 45.0));
    REQUIRE(m.pads.back().y   == Approx( 53.1));
    REQUIRE(m.pads.front().shape == PadShape::Circle);
    REQUIRE(m.pads.front().size  == Approx(12.0));
}

TEST_CASE("default fan-out: non-overlapping mapping puts P1 to F2", "[model]") {
    // Inside-out mapping: r=0 (left, layer 0) maps to fpc[c*rows + 1].
    // For c=0, that's fpc[1] = F2.
    auto m = Model::makeDefault();
    REQUIRE(m.traces.front().from == "P1");
    REQUIRE(m.traces.front().to   == "F2");

    // Sanity-check P9 (r=2, c=0) → fpc[c*rows + 0] = F1.
    auto t9 = std::find_if(m.traces.begin(), m.traces.end(),
                           [](const Trace& t) { return t.from == "P9"; });
    REQUIRE(t9 != m.traces.end());
    REQUIRE(t9->to == "F1");

    // Each trace has exactly 3 waypoints (drop-out, vertical, sweep)
    for (const auto& t : m.traces) {
        REQUIRE(t.waypoints.size() == 3);
        REQUIRE(t.width == Approx(0.002));
    }
}

TEST_CASE("layer stack matches the JS prototype defaults", "[model]") {
    auto m = Model::makeDefault();
    REQUIRE(m.layers[0].id == "shield");
    REQUIRE(m.layers[1].id == "inorganic");
    REQUIRE(m.layers[2].id == "sensor");
    REQUIRE(m.layers[3].id == "organic");
    REQUIRE(m.findLayer("sensor")->thickness == Approx(0.05));
}

TEST_CASE("padShapeFromString round-trips every variant", "[model]") {
    for (auto s : {PadShape::Circle, PadShape::Square, PadShape::Ellipse, PadShape::RoundedSquare}) {
        REQUIRE(padShapeFromString(padShapeToString(s)) == s);
    }
}
