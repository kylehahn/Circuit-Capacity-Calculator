#pragma once

#include "core/Trace.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace ccc::io {

struct GerberPolygon {
    std::string layerId;
    std::vector<ccc::core::Point2> outline;
    std::string net;
};

struct GerberLayer {
    std::string layerId;
    std::string sourcePath;
    std::vector<GerberPolygon> polygons;
};

struct DrillHit {
    double x = 0.0;
    double y = 0.0;
    double diameter = 0.0;
    std::string fromLayer;
    std::string toLayer;
};

struct GbrJobLayer {
    std::string id;
    std::string type;
    double thickness = 0.0;
    double epsR = 0.0;
    bool isCopper = false;
};

struct GbrJobInfo {
    std::vector<GbrJobLayer> layers;
    std::unordered_map<std::string, std::string> fileToLayer;
};

}  // namespace ccc::io
