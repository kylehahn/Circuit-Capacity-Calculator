#pragma once

#include "GerberTypes.hpp"

#include <string>
#include <vector>

namespace ccc::io {

std::vector<DrillHit> readExcellonDrillFile(const std::string& path);

}  // namespace ccc::io
