#pragma once

#include "GerberTypes.hpp"

#include <string>
#include <vector>

namespace ccc::io {

GerberLayer readGerberLayer(const std::string& path, const std::string& layerId = {});
std::string inferGerberLayerId(const std::string& path);

}  // namespace ccc::io
