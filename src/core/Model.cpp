#include "Model.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ccc::core {

std::vector<Layer> Model::defaultLayers() {
    return {
        {"shield",    "Shield",          0.05, "#f0a040", 0.85, true},
        {"inorganic", "Inorganic layer", 0.10, "#a0a0a0", 0.75, true},
        {"sensor",    "Sensor / Trace",  0.05, "#3a3a3a", 1.00, true},
        {"organic",   "Organic layer",   0.10, "#d0d4dc", 0.45, true},
    };
}

Model Model::makeDefault() {
    return generateGrid(4, 4, 12.0, 30.0);
}

Model Model::generateGrid(int cols, int rows, double padDia, double pitch) {
    constexpr double kMargin = 6.0;
    constexpr double kFpcRowHeight = 4.0;

    const int fpcCount = cols * rows;
    const double routingH = std::max(6.0, fpcCount * 0.7 + 1.0);
    const double glassW = (cols - 1) * pitch + padDia + kMargin * 2.0;
    const double glassH = (rows - 1) * pitch + padDia + kMargin * 2.0 + kFpcRowHeight + routingH;
    const double sensorAreaCenterY = (kFpcRowHeight + routingH) / 2.0;
    const double startX = -((cols - 1) * pitch) / 2.0;
    const double startY = sensorAreaCenterY - ((rows - 1) * pitch) / 2.0;

    Model m;
    m.glass = {glassW, glassH, 5.0, "#bcdde8", 0.30, true};
    m.layers = defaultLayers();

    // Pads: row-major grid, ids "P1" .. "P{cols*rows}"
    m.pads.reserve(static_cast<std::size_t>(fpcCount));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            Pad p;
            p.id    = "P" + std::to_string(r * cols + c + 1);
            p.x     = startX + c * pitch;
            p.y     = startY + r * pitch;
            p.shape = PadShape::Circle;
            p.size  = padDia;
            p.size2 = padDia;
            m.pads.push_back(std::move(p));
        }
    }

    // FPC strip: evenly spread along the column span, at the bottom of the glass
    const double fpcSpacing = (cols * pitch) / fpcCount;
    const double fpcStartX  = -((fpcCount - 1) * fpcSpacing) / 2.0;
    const double fpcStripY  = -glassH / 2.0 + kMargin / 2.0 + kFpcRowHeight / 2.0;
    m.fpcPads.reserve(static_cast<std::size_t>(fpcCount));
    for (int i = 0; i < fpcCount; ++i) {
        FpcPad f;
        f.id     = "F" + std::to_string(i + 1);
        f.x      = fpcStartX + i * fpcSpacing;
        f.y      = fpcStripY;
        f.width  = 1.2;
        f.height = kFpcRowHeight;
        m.fpcPads.push_back(std::move(f));
    }

    // Non-overlapping fan-out routing.
    // Even-indexed rows go LEFT, odd rows RIGHT; lower-indexed rows take the
    // FPC closest to the column centre, higher rows take the outer FPC. The
    // drop X follows the same scheme so closer FPCs get shorter drop channels
    // and farther FPCs get longer ones — guarantees zero segment crossings.
    constexpr double kOffsetStep = 0.8;
    const double baseOffset = padDia / 2.0 + 0.6;
    const int numLeft = (rows + 1) / 2;          // ceil(rows / 2)
    auto dropOffsetForRow = [&](int r) -> double {
        const int layer = r / 2;
        const int sign = (r % 2 == 0) ? -1 : 1;
        return sign * (baseOffset + layer * kOffsetStep);
    };
    auto fpcIdxForRow = [&](int c, int r) -> int {
        const int layer = r / 2;
        if (r % 2 == 0) return c * rows + (numLeft - 1 - layer);  // left, inner first
        return c * rows + (numLeft + layer);                      // right, inner first
    };
    const double channelTop    = startY - padDia / 2.0 - 0.6;
    const double channelBottom = (fpcStripY + kFpcRowHeight / 2.0) + 0.6;
    const double dy = (channelTop - channelBottom) / (fpcCount + 1);

    m.traces.reserve(static_cast<std::size_t>(fpcCount));
    for (int i = 0; i < fpcCount; ++i) {
        const int r = i / cols;
        const int c = i % cols;
        const Pad& p = m.pads[static_cast<std::size_t>(i)];
        const int fIdx = fpcIdxForRow(c, r);
        if (fIdx < 0 || fIdx >= fpcCount) continue;
        const FpcPad& f = m.fpcPads[static_cast<std::size_t>(fIdx)];

        const double offset = dropOffsetForRow(r);
        const double dropX  = p.x + offset;
        const double chY    = channelTop - (i + 1) * dy;

        Trace t;
        t.id    = "T" + std::to_string(i + 1);
        t.from  = p.id;
        t.to    = f.id;
        t.width = 0.4;
        t.waypoints = {
            {dropX, p.y},
            {dropX, chY},
            {f.x,   chY},
        };
        m.traces.push_back(std::move(t));
    }
    return m;
}

Layer* Model::findLayer(std::string_view id) {
    auto it = std::find_if(layers.begin(), layers.end(),
                           [&](const Layer& l) { return l.id == id; });
    return it != layers.end() ? &*it : nullptr;
}

const Layer* Model::findLayer(std::string_view id) const {
    auto it = std::find_if(layers.begin(), layers.end(),
                           [&](const Layer& l) { return l.id == id; });
    return it != layers.end() ? &*it : nullptr;
}

}  // namespace ccc::core
