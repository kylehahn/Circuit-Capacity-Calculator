#pragma once

#include <string>

namespace ccc::core {

// The glass substrate the sensor stack sits on. Z range [-thickness, 0].
struct GlassPlate {
    double width = 50.0;            // mm (X-extent)
    double height = 60.0;           // mm (Y-extent)
    double thickness = 5.0;         // mm
    std::string color = "#bcdde8";
    double opacity = 0.30;
    bool   visible = true;
    double permittivity = 7.0;      // epsilon_r (typical glass ~6-8)

    bool operator==(const GlassPlate&) const = default;
};

}  // namespace ccc::core
