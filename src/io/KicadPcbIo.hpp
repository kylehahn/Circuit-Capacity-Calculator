#pragma once

#include "core/Model.hpp"

#include <string>

namespace ccc::io {

// Read a KiCad 9 ".kicad_pcb" file (S-expression) and convert to our Model.
// Extracts: nets, footprint pads, track segments. Vias and zones are ignored
// for now. Each Pad/Trace gets its KiCad net name copied into its `net`
// field, so cap measurement can group by net.
//
// Coordinate transform: KiCad uses millimetres with Y-down, page-origin top
// left. We translate to our Z-up world by:
//   - flipping Y          (y_ours = -y_kicad)
//   - centering on bbox   (so the layout sits around our (0,0))
ccc::core::Model readKicadPcb(const std::string& path);

}  // namespace ccc::io
