#pragma once

#include <string>

namespace ccc::core {

// Rectangular FPC connector pad along the bottom edge of the sensor.
struct FpcPad {
    std::string id;
    double x = 0.0;
    double y = 0.0;
    double width  = 1.0;   // mm (X-extent)
    double height = 4.0;   // mm (Y-extent)

    bool operator==(const FpcPad&) const = default;
};

}  // namespace ccc::core
