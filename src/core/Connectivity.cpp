#include "Connectivity.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace ccc::core {

namespace {

constexpr double kTol = 1.0e-6;

struct BBox {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

BBox bboxOf(const Zone& z) {
    BBox b;
    if (z.outline.empty()) return b;
    b.x0 = b.x1 = z.outline.front()[0];
    b.y0 = b.y1 = z.outline.front()[1];
    for (const auto& p : z.outline) {
        b.x0 = std::min(b.x0, p[0]);
        b.x1 = std::max(b.x1, p[0]);
        b.y0 = std::min(b.y0, p[1]);
        b.y1 = std::max(b.y1, p[1]);
    }
    return b;
}

bool bboxOverlap(const BBox& a, const BBox& b, double grow = 0.0) {
    return a.x0 - grow <= b.x1 && a.x1 + grow >= b.x0
           && a.y0 - grow <= b.y1 && a.y1 + grow >= b.y0;
}

double cross(double ax, double ay, double bx, double by) {
    return ax * by - ay * bx;
}

bool pointInPolygon(const std::vector<Point2>& pts, double x, double y) {
    bool in = false;
    const std::size_t n = pts.size();
    if (n < 3) return false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = pts[i][0], yi = pts[i][1];
        const double xj = pts[j][0], yj = pts[j][1];
        if (((yi > y) != (yj > y))
            && (x < (xj - xi) * (y - yi) / (yj - yi + 1e-30) + xi)) {
            in = !in;
        }
    }
    return in;
}

bool segmentIntersects(const Point2& a, const Point2& b,
                       const Point2& c, const Point2& d) {
    const double abx = b[0] - a[0], aby = b[1] - a[1];
    const double acx = c[0] - a[0], acy = c[1] - a[1];
    const double adx = d[0] - a[0], ady = d[1] - a[1];
    const double cdx = d[0] - c[0], cdy = d[1] - c[1];
    const double cax = a[0] - c[0], cay = a[1] - c[1];
    const double cbx = b[0] - c[0], cby = b[1] - c[1];
    const double o1 = cross(abx, aby, acx, acy);
    const double o2 = cross(abx, aby, adx, ady);
    const double o3 = cross(cdx, cdy, cax, cay);
    const double o4 = cross(cdx, cdy, cbx, cby);
    return (o1 * o2 <= kTol) && (o3 * o4 <= kTol)
           && std::min(a[0], b[0]) - kTol <= std::max(c[0], d[0])
           && std::max(a[0], b[0]) + kTol >= std::min(c[0], d[0])
           && std::min(a[1], b[1]) - kTol <= std::max(c[1], d[1])
           && std::max(a[1], b[1]) + kTol >= std::min(c[1], d[1]);
}

bool polygonsTouch(const Zone& a, const Zone& b) {
    if (a.outline.size() < 3 || b.outline.size() < 3) return false;
    if (pointInPolygon(a.outline, b.outline.front()[0], b.outline.front()[1])) return true;
    if (pointInPolygon(b.outline, a.outline.front()[0], a.outline.front()[1])) return true;
    for (std::size_t i = 0; i < a.outline.size(); ++i) {
        const auto& a0 = a.outline[i];
        const auto& a1 = a.outline[(i + 1) % a.outline.size()];
        for (std::size_t j = 0; j < b.outline.size(); ++j) {
            const auto& b0 = b.outline[j];
            const auto& b1 = b.outline[(j + 1) % b.outline.size()];
            if (segmentIntersects(a0, a1, b0, b1)) return true;
        }
    }
    return false;
}

double pointSegmentDistance(double px, double py, const Point2& a, const Point2& b) {
    const double vx = b[0] - a[0];
    const double vy = b[1] - a[1];
    const double wx = px - a[0];
    const double wy = py - a[1];
    const double len2 = vx * vx + vy * vy;
    if (len2 <= kTol) return std::hypot(px - a[0], py - a[1]);
    const double t = std::clamp((wx * vx + wy * vy) / len2, 0.0, 1.0);
    return std::hypot(px - (a[0] + t * vx), py - (a[1] + t * vy));
}

bool viaTouchesZone(const ViaConnection& via, const Zone& z) {
    if (z.outline.size() < 3) return false;
    if (!via.fromLayer.empty() && !via.toLayer.empty()
        && z.layerId != via.fromLayer && z.layerId != via.toLayer) {
        // KiCad's regular .drl is normally through-hole; keep this check only
        // for explicit blind/buried metadata if it appears later.
    }
    if (pointInPolygon(z.outline, via.x, via.y)) return true;
    const double r = std::max(via.diameter * 0.5, 0.0);
    for (std::size_t i = 0; i < z.outline.size(); ++i) {
        if (pointSegmentDistance(via.x, via.y, z.outline[i],
                                 z.outline[(i + 1) % z.outline.size()]) <= r + kTol) {
            return true;
        }
    }
    return false;
}

bool netsCompatible(const Zone& a, const Zone& b) {
    if (a.net.empty() || b.net.empty()) return a.net.empty() && b.net.empty();
    return a.net == b.net;
}

class UnionFind {
public:
    explicit UnionFind(std::size_t n) : parent_(n), rank_(n, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }
    std::size_t find(std::size_t x) {
        if (parent_[x] == x) return x;
        parent_[x] = find(parent_[x]);
        return parent_[x];
    }
    void unite(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }
private:
    std::vector<std::size_t> parent_;
    std::vector<int> rank_;
};

}  // namespace

ConnectivityStats assignConnectedCopperObjects(Model& model,
                                                const std::vector<ViaConnection>& vias) {
    ConnectivityStats stats;
    stats.viaCount = int(vias.size());
    stats.zoneCount = int(model.zones.size());
    if (model.zones.empty()) return stats;

    UnionFind uf(model.zones.size());
    std::vector<BBox> boxes;
    boxes.reserve(model.zones.size());
    for (const auto& z : model.zones) boxes.push_back(bboxOf(z));

    for (std::size_t i = 0; i < model.zones.size(); ++i) {
        for (std::size_t j = i + 1; j < model.zones.size(); ++j) {
            if (model.zones[i].layerId != model.zones[j].layerId) continue;
            if (!netsCompatible(model.zones[i], model.zones[j])) continue;
            if (!bboxOverlap(boxes[i], boxes[j], 1e-4)) continue;
            if (polygonsTouch(model.zones[i], model.zones[j])) uf.unite(i, j);
        }
    }

    for (const auto& via : vias) {
        std::vector<std::size_t> touched;
        const BBox vb{via.x - via.diameter * 0.5, via.y - via.diameter * 0.5,
                      via.x + via.diameter * 0.5, via.y + via.diameter * 0.5};
        for (std::size_t i = 0; i < model.zones.size(); ++i) {
            if (!bboxOverlap(vb, boxes[i], 0.0)) continue;
            if (viaTouchesZone(via, model.zones[i])) touched.push_back(i);
        }
        for (std::size_t i = 0; i < touched.size(); ++i) {
            for (std::size_t j = i + 1; j < touched.size(); ++j) {
                if (netsCompatible(model.zones[touched[i]], model.zones[touched[j]])) {
                    uf.unite(touched[i], touched[j]);
                }
            }
        }
    }

    std::unordered_map<std::size_t, std::string> rootToExplicitNet;
    for (std::size_t i = 0; i < model.zones.size(); ++i) {
        if (model.zones[i].net.empty()) continue;
        const auto root = uf.find(i);
        rootToExplicitNet.try_emplace(root, model.zones[i].net);
    }

    std::unordered_map<std::size_t, std::string> rootToName;
    int next = 1;
    for (std::size_t i = 0; i < model.zones.size(); ++i) {
        const auto root = uf.find(i);
        auto it = rootToName.find(root);
        if (it == rootToName.end()) {
            auto explicitIt = rootToExplicitNet.find(root);
            const std::string name = explicitIt != rootToExplicitNet.end()
                                         ? explicitIt->second
                                         : "OBJ-" + std::to_string(next++);
            it = rootToName.emplace(root, name).first;
        }
        model.zones[i].net = it->second;
    }
    stats.objectCount = int(rootToName.size());
    return stats;
}

}  // namespace ccc::core
