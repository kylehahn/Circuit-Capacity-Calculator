#pragma once

#include "core/Model.hpp"

#include <nlohmann/json.hpp>

namespace ccc::io {

// JSON serialisation for the data model. The on-disk shape mirrors the JS
// prototype's `sensorEditor` payload (same field names, same nesting), so a
// `.s3d` / `.ccc` file written here can be loaded by the prototype and vice
// versa.

nlohmann::json modelToJson(const ccc::core::Model& m);
ccc::core::Model modelFromJson(const nlohmann::json& j);

// Convenience: read/write to a file path. Throws std::runtime_error on I/O
// or parse failure.
void writeModelFile(const ccc::core::Model& m, const std::string& path, int indent = 2);
ccc::core::Model readModelFile(const std::string& path);

}  // namespace ccc::io
