#pragma once

#include <array>
#include <string>
#include <vector>

namespace ccc::core {

// (X, Y) point in the sensor-layer plane. Units: millimetres.
using Point2 = std::array<double, 2>;

// A copper trace on the sensor layer connecting two terminals (pads or FPC pads).
// The endpoints are referenced by id; the polyline through `waypoints` is the
// path *between* the two endpoints (the endpoint coordinates themselves are
// not stored here — they are looked up from the model on use).
struct Trace {
    std::string id;
    std::string from;             // source pad / FPC pad id
    std::string to;               // target pad / FPC pad id
    double width = 0.002;         // mm (= 2 um)
    std::vector<Point2> waypoints;
    // Net membership (for KiCad-imported designs). Empty means "no net info";
    // fall back to topology-based connectivity in cap measurements.
    std::string net;
    // Copper layer this trace lives on (e.g. "F.Cu"). Empty = sensor layer.
    std::string layer;

    bool operator==(const Trace&) const = default;
};

}  // namespace ccc::core
