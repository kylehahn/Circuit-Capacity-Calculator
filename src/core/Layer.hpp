#pragma once

#include <string>

namespace ccc::core {

// A film layer in the sensor stack. The "sensor" layer (id == "sensor") is
// special: the pads, traces and FPC pads live in it; for all other layers
// the geometry is the entire glass extent at the layer's Z slab.
struct Layer {
    std::string id;
    std::string name;
    double thickness = 0.05;      // mm
    std::string color = "#888888";
    double opacity = 1.0;
    bool visible = true;

    bool operator==(const Layer&) const = default;
};

}  // namespace ccc::core
