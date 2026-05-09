#pragma once

#include <string>

namespace ccc::core {

// File-level metadata embedded in saved models.
struct Meta {
    std::string type = "16Sensor3D";
    std::string version = "1.0";
    std::string createdAt;        // ISO-8601 UTC, optional

    bool operator==(const Meta&) const = default;
};

}  // namespace ccc::core
