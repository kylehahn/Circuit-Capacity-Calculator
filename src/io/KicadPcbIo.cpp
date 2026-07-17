#include "KicadPcbIo.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ccc::io {

namespace {

// ============================================================================
//  Minimal S-expression parser (KiCad-style).
// ============================================================================
struct Sexpr {
    bool                isList = false;
    std::string         atom;
    std::vector<Sexpr>  children;

    const std::string& head() const {
        if (isList && !children.empty() && !children.front().isList)
            return children.front().atom;
        static const std::string empty;
        return empty;
    }
    const Sexpr* findChild(std::string_view name) const {
        if (!isList) return nullptr;
        for (const auto& c : children)
            if (c.isList && c.head() == name) return &c;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view src) : src_(src) {}
    Sexpr parseTop() {
        skipWs();
        if (pos_ >= src_.size() || src_[pos_] != '(')
            throw std::runtime_error("KiCad PCB: expected '(' at top");
        return parseList();
    }
private:
    std::string_view src_;
    size_t pos_ = 0;
    void skipWs() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            break;
        }
    }
    Sexpr parseList() {
        ++pos_;
        Sexpr s; s.isList = true;
        skipWs();
        while (pos_ < src_.size() && src_[pos_] != ')') {
            s.children.push_back(parseValue()); skipWs();
        }
        if (pos_ >= src_.size())
            throw std::runtime_error("KiCad PCB: unmatched '('");
        ++pos_;
        return s;
    }
    Sexpr parseValue() {
        skipWs();
        if (pos_ >= src_.size())
            throw std::runtime_error("KiCad PCB: unexpected end of input");
        const char c = src_[pos_];
        if (c == '(') return parseList();
        if (c == '"') return parseString();
        return parseAtom();
    }
    Sexpr parseString() {
        ++pos_; std::string out;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\\' && pos_ + 1 < src_.size()) { out.push_back(src_[pos_+1]); pos_ += 2; continue; }
            if (c == '"') { ++pos_; break; }
            out.push_back(c); ++pos_;
        }
        Sexpr s; s.atom = std::move(out); return s;
    }
    Sexpr parseAtom() {
        const size_t start = pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '(' || c == ')' || c == ' ' || c == '\t' ||
                c == '\n' || c == '\r' || c == '"') break;
            ++pos_;
        }
        Sexpr s; s.atom.assign(src_.substr(start, pos_ - start)); return s;
    }
};

double atomDouble(const Sexpr& n, size_t i, double d = 0.0) {
    if (!n.isList || i >= n.children.size() || n.children[i].isList) return d;
    try { return std::stod(n.children[i].atom); } catch (...) { return d; }
}
int atomInt(const Sexpr& n, size_t i, int d = 0) {
    if (!n.isList || i >= n.children.size() || n.children[i].isList) return d;
    try { return std::stoi(n.children[i].atom); } catch (...) { return d; }
}
std::string atomStr(const Sexpr& n, size_t i) {
    if (!n.isList || i >= n.children.size() || n.children[i].isList) return {};
    return n.children[i].atom;
}

// ============================================================================
//  Helpers
// ============================================================================
struct XY { double x = 0, y = 0; };
XY readXY(const Sexpr& at) { return { atomDouble(at, 1), atomDouble(at, 2) }; }

using NetMap = std::unordered_map<int, std::string>;

// ----------------------------------------------------------------------------
//  Layer stack
// ----------------------------------------------------------------------------
// Build Model.layers from `(layers ...)` and optional `(setup (stackup ...))`.
// Layers are listed in stack order; thicknesses read from the stackup if
// present, otherwise default to 0.035 mm (copper) / 0.1 mm (dielectric).
struct LayerInfo {
    std::string id;            // KiCad name, e.g., "F.Cu", "Dielectric 1"
    bool        isCopper = false;
    double      thickness = 0.05;
    double      epsR     = 1.0;
};
std::vector<LayerInfo> buildLayerStack(const Sexpr& root) {
    std::vector<LayerInfo> out;

    // Prefer the physical stackup order when present. The top-level `(layers)`
    // list contains board-editor layers, but not dielectric layers such as
    // prepreg/core. Capacitance extraction needs the physical Z stack.
    auto* setup = root.findChild("setup");
    auto* stackup = setup ? setup->findChild("stackup") : nullptr;
    if (stackup) {
        std::vector<LayerInfo> stack;
        for (const auto& sl : stackup->children) {
            if (!sl.isList || sl.head() != "layer") continue;
            const std::string name = atomStr(sl, 1);
            auto* tType = sl.findChild("type");
            auto* tThk  = sl.findChild("thickness");
            auto* tEps  = sl.findChild("epsilon_r");
            const std::string type = tType ? atomStr(*tType, 1) : "";
            const double thk  = tThk ? atomDouble(*tThk, 1, 0) : 0;
            const double eps  = tEps ? atomDouble(*tEps, 1, 0) : 0;
            const bool isCopper = (type == "copper");
            std::string typeUpper = type + " " + name;
            std::transform(typeUpper.begin(), typeUpper.end(), typeUpper.begin(),
                           [](unsigned char c) { return char(std::toupper(c)); });
            const bool isPhysicalDielectric =
                !isCopper && thk > 0.0
                && (eps > 0.0
                    || typeUpper.find("DIELECTRIC") != std::string::npos
                    || typeUpper.find("PREPREG") != std::string::npos
                    || typeUpper.find("CORE") != std::string::npos
                    || typeUpper.find("MASK") != std::string::npos);
            if (!isCopper && !isPhysicalDielectric) continue;

            LayerInfo li;
            li.id = name;
            li.isCopper = isCopper;
            li.thickness = (thk > 0) ? thk : (isCopper ? 0.035 : 0.1);
            li.epsR = (eps > 0) ? eps : (isCopper ? 1.0 : 4.5);
            stack.push_back(std::move(li));
        }
        if (!stack.empty()) return stack;
    }

    // Fallback: read top-level copper layers when no physical stackup exists.
    if (auto* layers = root.findChild("layers")) {
        for (const auto& c : layers->children) {
            if (!c.isList) continue;            // skip head atom "layers"
            // (N "name" "type" ["userName"])
            if (c.children.size() < 3) continue;
            const std::string name = atomStr(c, 1);
            const std::string type = atomStr(c, 2);
            LayerInfo info;
            info.id = name;
            info.isCopper = (type == "signal" || type == "power" || type == "mixed");
            if (!info.isCopper) continue;
            info.thickness = 0.035;
            info.epsR = 1.0;
            out.push_back(std::move(info));
        }
    }
    return out;
}

// ----------------------------------------------------------------------------
//  Zone (pour) outline extraction
// ----------------------------------------------------------------------------
struct ParsedZone {
    std::string layer;
    std::string netName;
    std::vector<XY> outline;
};
// Read all (xy x y) children of an (pts ...) node into a vector.
void readPts(const Sexpr& pts, std::vector<XY>& out) {
    for (const auto& c : pts.children) {
        if (c.isList && c.head() == "xy") out.push_back(readXY(c));
    }
}

// ----------------------------------------------------------------------------
//  Footprint pads
// ----------------------------------------------------------------------------
struct ParsedPad {
    double x = 0, y = 0;
    double sizeX = 0, sizeY = 0;
    std::string shape;
    std::string netName;
    std::string layer;     // first copper layer the pad sits on
};
void collectFootprintPads(const Sexpr& fp, const NetMap& nets,
                          std::vector<ParsedPad>& out) {
    XY fpAt; double fpRotDeg = 0;
    if (auto* at = fp.findChild("at")) {
        fpAt = readXY(*at);
        fpRotDeg = atomDouble(*at, 3, 0.0);
    }
    const double cosR = std::cos(fpRotDeg * std::numbers::pi / 180.0);
    const double sinR = std::sin(fpRotDeg * std::numbers::pi / 180.0);

    for (const auto& c : fp.children) {
        if (!c.isList || c.head() != "pad") continue;
        ParsedPad p;
        p.shape = atomStr(c, 3);
        if (auto* atP = c.findChild("at")) {
            const XY local = readXY(*atP);
            p.x = fpAt.x + local.x * cosR - local.y * sinR;
            p.y = fpAt.y + local.x * sinR + local.y * cosR;
        }
        if (auto* sz = c.findChild("size")) {
            p.sizeX = atomDouble(*sz, 1);
            p.sizeY = atomDouble(*sz, 2);
        }
        if (auto* nt = c.findChild("net")) {
            const int id = atomInt(*nt, 1, -1);
            p.netName = atomStr(*nt, 2);
            if (p.netName.empty()) {
                auto it = nets.find(id);
                if (it != nets.end()) p.netName = it->second;
            }
        }
        // (layers "F.Cu" "*.Mask")  — first matching *.Cu wins.
        if (auto* layers = c.findChild("layers")) {
            for (size_t k = 1; k < layers->children.size(); ++k) {
                const std::string lname = atomStr(*layers, k);
                if (lname.find(".Cu") != std::string::npos) {
                    p.layer = lname; break;
                }
            }
            if (p.layer.empty() && layers->children.size() > 1) {
                p.layer = atomStr(*layers, 1);
            }
        }
        out.push_back(std::move(p));
    }
}

}  // namespace

// ============================================================================
//  Public entrypoint
// ============================================================================
ccc::core::Model readKicadPcb(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("KiCad PCB: cannot open " + path);
    std::ostringstream ss; ss << in.rdbuf();
    const std::string text = ss.str();

    Parser parser(text);
    Sexpr root = parser.parseTop();
    if (root.head() != "kicad_pcb")
        throw std::runtime_error("KiCad PCB: not a kicad_pcb file: " + path);

    // ----- Pass: collect nets ---------------------------------------------
    NetMap nets;
    for (const auto& c : root.children) {
        if (c.isList && c.head() == "net") {
            const int id = atomInt(c, 1, -1);
            std::string name = atomStr(c, 2);
            if (id >= 0) {
                // KiCad lets nets be unnamed (empty string) -- typical for
                // auto-routed power/ground pours and the no-net id 0. We
                // still need them to be DISTINGUISHABLE for net-aware
                // selection, so synthesise a unique placeholder.
                if (name.empty() && id > 0) name = "Net-" + std::to_string(id);
                nets[id] = std::move(name);
            }
        }
    }

    // ----- Pass: layer stack ----------------------------------------------
    auto layerInfos = buildLayerStack(root);

    // ----- Pass: pads, segments, zones ------------------------------------
    struct ParsedSeg {
        double x0=0,y0=0,x1=0,y1=0,width=0;
        std::string netName, layer;
    };
    std::vector<ParsedPad>  pads;
    std::vector<ParsedSeg>  segments;
    std::vector<ParsedZone> zones;

    for (const auto& c : root.children) {
        if (!c.isList) continue;
        const std::string& h = c.head();
        if (h == "footprint") {
            collectFootprintPads(c, nets, pads);
        } else if (h == "segment") {
            ParsedSeg s;
            if (auto* st = c.findChild("start")) { auto p = readXY(*st); s.x0=p.x; s.y0=p.y; }
            if (auto* en = c.findChild("end"))   { auto p = readXY(*en); s.x1=p.x; s.y1=p.y; }
            if (auto* w  = c.findChild("width")) s.width = atomDouble(*w, 1);
            if (auto* l  = c.findChild("layer")) s.layer = atomStr(*l, 1);
            if (auto* nt = c.findChild("net")) {
                const int id = atomInt(*nt, 1, -1);
                auto it = nets.find(id);
                if (it != nets.end()) s.netName = it->second;
            }
            segments.push_back(std::move(s));
        } else if (h == "zone") {
            ParsedZone z;
            if (auto* l  = c.findChild("layer"))   z.layer = atomStr(*l, 1);
            if (auto* l  = c.findChild("layers"))  z.layer = atomStr(*l, 1);
            if (auto* nn = c.findChild("net_name")) z.netName = atomStr(*nn, 1);
            else if (auto* nt = c.findChild("net")) {
                const int id = atomInt(*nt, 1, -1);
                auto it = nets.find(id);
                if (it != nets.end()) z.netName = it->second;
            }
            // Prefer filled_polygon (the actual copper after fill). Take all
            // such children (a zone may have multiple islands).
            bool addedFromFill = false;
            for (const auto& sub : c.children) {
                if (!sub.isList) continue;
                if (sub.head() != "filled_polygon") continue;
                if (auto* pts = sub.findChild("pts")) {
                    std::vector<XY> outline;
                    readPts(*pts, outline);
                    if (!outline.empty()) {
                        ParsedZone copy = z;
                        copy.outline = std::move(outline);
                        zones.push_back(std::move(copy));
                        addedFromFill = true;
                    }
                }
            }
            // Fallback: if no filled_polygon exists for this zone block (the
            // user hasn't run "Fill all zones" in KiCad), use the outline
            // polygon. The outline still gives a usable polygon for BEM.
            if (!addedFromFill) {
                if (auto* poly = c.findChild("polygon")) {
                    if (auto* pts = poly->findChild("pts")) {
                        std::vector<XY> outline;
                        readPts(*pts, outline);
                        if (!outline.empty()) {
                            z.outline = std::move(outline);
                            zones.push_back(std::move(z));
                        }
                    }
                }
            }
        }
    }

    // ----- Centre on bbox -------------------------------------------------
    double xMin =  1e18, xMax = -1e18, yMin =  1e18, yMax = -1e18;
    auto note = [&](double x, double y) {
        xMin = std::min(xMin, x); xMax = std::max(xMax, x);
        yMin = std::min(yMin, y); yMax = std::max(yMax, y);
    };
    for (const auto& p : pads)     note(p.x, p.y);
    for (const auto& s : segments) { note(s.x0, s.y0); note(s.x1, s.y1); }
    for (const auto& z : zones)    for (const auto& xy : z.outline) note(xy.x, xy.y);
    if (xMin > xMax) { xMin = xMax = yMin = yMax = 0.0; }
    const double cx = 0.5 * (xMin + xMax);
    const double cy = 0.5 * (yMin + yMax);
    auto fix = [&](double x, double y) {
        return std::pair<double,double>{ x - cx, -(y - cy) };
    };

    // ----- Build Model ----------------------------------------------------
    ccc::core::Model m{};
    m.glass = ccc::core::GlassPlate{};   // default
    m.glass.visible = false;             // KiCad PCBs don't have a glass substrate
    m.glass.width = xMax - xMin;
    m.glass.height = yMax - yMin;
    m.glass.thickness = 0.0;

    // Layers: physical copper + dielectric stack. Rendering still hides
    // dielectric layers by default, but the solver uses their thicknesses to
    // put copper at the correct Z and their eps_r for FasterCap stack media.
    bool hasAnyCu = false;
    for (const auto& li : layerInfos) if (li.isCopper) { hasAnyCu = true; break; }
    if (hasAnyCu) {
        for (const auto& li : layerInfos) {
            ccc::core::Layer L;
            L.id           = li.id;
            L.name         = li.id;
            L.thickness    = li.thickness;
            L.isConductor  = li.isCopper;
            L.permittivity = li.epsR;
            L.color        = li.isCopper ? "#c89764" : "#9fb3c8";
            L.opacity      = li.isCopper ? 1.0 : 0.18;
            L.visible      = li.isCopper;
            m.layers.push_back(std::move(L));
        }
    } else {
        m.layers = ccc::core::Model::defaultLayers();
    }

    int padCounter = 0;
    for (const auto& p : pads) {
        ccc::core::Pad pp;
        pp.id = "P" + std::to_string(++padCounter);
        const auto [wx, wy] = fix(p.x, p.y);
        pp.x = wx; pp.y = wy;
        if (p.shape == "circle" || p.shape == "oval") {
            pp.shape = ccc::core::PadShape::Circle;
            pp.size = std::min(p.sizeX, p.sizeY);
            if (pp.size <= 0) pp.size = std::max(p.sizeX, p.sizeY);
            if (pp.size <= 0) pp.size = 0.5;
            pp.size2 = pp.size;
        } else if (p.shape == "rect") {
            pp.shape = ccc::core::PadShape::Square;
            pp.size = std::max(p.sizeX, p.sizeY);
            if (pp.size <= 0) pp.size = 0.5;
            pp.size2 = pp.size;
        } else {
            pp.shape = ccc::core::PadShape::RoundedSquare;
            pp.size = std::max(p.sizeX, p.sizeY);
            if (pp.size <= 0) pp.size = 0.5;
            pp.size2 = pp.size * 0.2;
        }
        pp.net   = p.netName;
        pp.layer = p.layer;
        m.pads.push_back(std::move(pp));
    }

    int traceCounter = 0;
    for (const auto& s : segments) {
        ccc::core::Trace tr;
        tr.id = "T" + std::to_string(++traceCounter);
        tr.width = (s.width > 0) ? s.width : 0.002;
        const auto [x0, y0] = fix(s.x0, s.y0);
        const auto [x1, y1] = fix(s.x1, s.y1);
        tr.waypoints.push_back({x0, y0});
        tr.waypoints.push_back({x1, y1});
        tr.net   = s.netName;
        tr.layer = s.layer;
        m.traces.push_back(std::move(tr));
    }

    int zoneCounter = 0;
    for (const auto& z : zones) {
        ccc::core::Zone zz;
        zz.id      = "Z" + std::to_string(++zoneCounter);
        zz.layerId = z.layer;
        zz.net     = z.netName;
        for (const auto& xy : z.outline) {
            const auto [wx, wy] = fix(xy.x, xy.y);
            zz.outline.push_back({wx, wy});
        }
        m.zones.push_back(std::move(zz));
    }

    return m;
}

}  // namespace ccc::io
