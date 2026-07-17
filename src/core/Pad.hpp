#pragma once

#include <string>
#include <string_view>

namespace ccc::core {

enum class PadShape {
    Circle,
    Square,
    Ellipse,
    RoundedSquare,
};

constexpr std::string_view padShapeToString(PadShape s) noexcept {
    switch (s) {
        case PadShape::Circle:        return "circle";
        case PadShape::Square:        return "square";
        case PadShape::Ellipse:       return "ellipse";
        case PadShape::RoundedSquare: return "roundedSquare";
    }
    return "circle";
}

PadShape padShapeFromString(std::string_view s);

// A sensor pad lying on the sensor/trace layer. All units are millimetres.
//   Circle:        diameter = size, size2 unused (set to size)
//   Square:        side     = size, size2 unused (set to size)
//   Ellipse:       X-diameter = size, Y-diameter = size2
//   RoundedSquare: side     = size, corner radius = size2
struct Pad {
    std::string id;
    double x = 0.0;
    double y = 0.0;
    PadShape shape = PadShape::Circle;
    double size = 6.0;
    double size2 = 6.0;
    // Net membership (for KiCad-imported designs). Empty means "no net info"
    // and falls back to topology-based connectivity in cap measurements.
    std::string net;
    // Copper layer this pad lives on (e.g. "F.Cu"). Empty = sensor layer.
    std::string layer;

    bool operator==(const Pad&) const = default;
};

}  // namespace ccc::core
