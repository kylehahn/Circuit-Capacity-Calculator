#pragma once

#include "GerberTypes.hpp"

#include <string>

namespace ccc::io {

GbrJobInfo readGbrJob(const std::string& path);

}  // namespace ccc::io
