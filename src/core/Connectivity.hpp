#pragma once

#include "Model.hpp"

#include <string>
#include <vector>

namespace ccc::core {

struct ViaConnection {
    double x = 0.0;
    double y = 0.0;
    double diameter = 0.0;
    std::string fromLayer;
    std::string toLayer;
};

struct ConnectivityStats {
    int objectCount = 0;
    int viaCount = 0;
    int zoneCount = 0;
};

ConnectivityStats assignConnectedCopperObjects(Model& model,
                                                const std::vector<ViaConnection>& vias);

}  // namespace ccc::core
