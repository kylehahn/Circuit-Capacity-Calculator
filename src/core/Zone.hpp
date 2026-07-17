#pragma once

#include "Trace.hpp"   // for Point2

#include <cmath>
#include <string>
#include <vector>

namespace ccc::core {

// A copper pour / zone on one PCB layer. Imported from KiCad's `(zone ...)`.
// Rendered as a flat polygon at the layer's Z; panelised by rasterising its
// bounding box and keeping cells whose centre is inside the outline.
struct Zone {
    std::string id;
    std::string layerId;          // matches Layer::id (e.g., "F.Cu")
    std::string net;              // KiCad net name; empty for stand-alone copper
    std::vector<Point2> outline;  // polygon vertices in mm; closed (last != first)
    std::vector<std::vector<Point2>> holes; // cutouts inside outline, if any

    bool operator==(const Zone&) const = default;
};

inline double polygonSignedArea(const std::vector<Point2>& pts) {
    double area = 0.0;
    if (pts.size() < 3) return area;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const auto& a = pts[i];
        const auto& b = pts[(i + 1) % pts.size()];
        area += a[0] * b[1] - b[0] * a[1];
    }
    return area * 0.5;
}

inline bool pointInPolygon(const std::vector<Point2>& pts, double x, double y) {
    if (pts.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const double xi = pts[i][0], yi = pts[i][1];
        const double xj = pts[j][0], yj = pts[j][1];
        if (((yi > y) != (yj > y))
            && (x < (xj - xi) * (y - yi) / (yj - yi + 1e-30) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

inline bool pointInZoneCopper(const Zone& z, double x, double y) {
    if (!pointInPolygon(z.outline, x, y)) return false;
    for (const auto& hole : z.holes) {
        if (pointInPolygon(hole, x, y)) return false;
    }
    return true;
}

inline double zoneCopperArea(const Zone& z) {
    double area = std::abs(polygonSignedArea(z.outline));
    for (const auto& hole : z.holes) {
        area -= std::abs(polygonSignedArea(hole));
    }
    return std::max(0.0, area);
}

}  // namespace ccc::core
