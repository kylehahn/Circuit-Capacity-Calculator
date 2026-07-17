#pragma once

#include "core/Model.hpp"

#include <string>

namespace ccc::io {

// GLB (Binary glTF) save / load.
//
// The model is embedded as a JSON string under `asset.extras.sensorEditor`
// — same shape as the JS prototype's `userData.sensorEditor`. The file also
// contains a single empty scene so it remains a valid glTF that opens in any
// 3D viewer (just shows nothing). For round-trip purposes we only read the
// extras; we never inspect the scene geometry.
//
// Throws std::runtime_error on I/O or parse failure.

void writeModelGlb(const ccc::core::Model& m, const std::string& path);
ccc::core::Model readModelGlb(const std::string& path);

}  // namespace ccc::io
