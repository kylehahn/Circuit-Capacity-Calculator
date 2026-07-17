#pragma once

#include "core/Model.hpp"

#include <string>

namespace ccc::io {

ccc::core::Model readGerberProject(const std::string& folderPath,
                                   const std::string& kicadPcbPath = {});

}  // namespace ccc::io
