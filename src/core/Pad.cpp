#include "Pad.hpp"

#include <stdexcept>
#include <string>

namespace ccc::core {

PadShape padShapeFromString(std::string_view s) {
    if (s == "circle")        return PadShape::Circle;
    if (s == "square")        return PadShape::Square;
    if (s == "ellipse")       return PadShape::Ellipse;
    if (s == "roundedSquare") return PadShape::RoundedSquare;
    throw std::invalid_argument("unknown pad shape: " + std::string(s));
}

}  // namespace ccc::core
