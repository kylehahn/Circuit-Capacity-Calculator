#pragma once

#include <string>

namespace ccc::core {

// One film layer in the sensor stack. Layers stack along +Z above the glass
// substrate (z = 0 is the glass top surface).
//
// isConductor == true means the layer is metal/conductive (Shield, Sensor).
// In that case `permittivity` is meaningless and ignored by the solver --
// the layer is treated as an equipotential. For dielectric layers
// (Inorganic, Organic, etc.) `permittivity` is the relative dielectric
// constant epsilon_r used by the BEM capacitance solver.
struct Layer {
    std::string id;
    std::string name;
    double thickness = 0.05;       // mm
    std::string color = "#888888";
    double opacity = 1.0;
    bool   visible = true;
    bool   isConductor = false;
    double permittivity = 1.0;     // epsilon_r; ignored when isConductor

    bool operator==(const Layer&) const = default;
};

}  // namespace ccc::core
