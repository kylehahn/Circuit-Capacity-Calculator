#pragma once

#include "FpcPad.hpp"
#include "GlassPlate.hpp"
#include "Layer.hpp"
#include "Meta.hpp"
#include "Pad.hpp"
#include "Trace.hpp"
#include "Zone.hpp"

#include <string_view>
#include <vector>

namespace ccc::core {

// The full editable scene: substrate + film layer stack + sensor-layer
// geometry (pads, FPC pads, traces). This mirrors the JS prototype's
// `sensorEditor` payload one-to-one.
struct Model {
    Meta meta{};
    GlassPlate glass{};
    std::vector<Layer> layers;
    std::vector<Pad> pads;
    std::vector<FpcPad> fpcPads;
    std::vector<Trace> traces;
    std::vector<Zone> zones;     // copper pours (KiCad-imported)

    bool operator==(const Model&) const = default;

    // Default sensor-stack film layers (Shield → Inorganic → Sensor → Organic),
    // matching the JS prototype's DEFAULT_LAYERS.
    static std::vector<Layer> defaultLayers();

    // Default 16-sensor model (4 × 4, 12 mm pads at 30 mm pitch). Matches the
    // browser prototype's `defaultModel()`.
    static Model makeDefault();

    // Generate a cols × rows pad grid with non-overlapping fan-out routing
    // to a `cols * rows`-pin FPC strip along the bottom edge. The mapping
    // and offsets are exactly the algorithm validated in the JS prototype.
    static Model generateGrid(int cols, int rows, double padDiameterMm, double pitchMm);

    // Layer lookup by id (returns nullptr if not found).
    Layer* findLayer(std::string_view id);
    const Layer* findLayer(std::string_view id) const;
};

}  // namespace ccc::core
