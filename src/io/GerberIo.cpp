#include "GerberIo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace ccc::io {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Gerber: cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

std::string trim(std::string s) {
    auto isWs = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isWs(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && isWs(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

struct Aperture {
    std::string type = "C";
    double a = 0.0;
    double b = 0.0;
    std::vector<ccc::core::Point2> macroPolygon;
};

struct ApertureMacro {
    std::vector<ccc::core::Point2> polygon;
    int rotationParam = 0;
};

std::vector<std::string> splitGerberCommands(const std::string& src) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < src.size()) {
        while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
        if (i >= src.size()) break;
        if (src[i] == '%') {
            const auto end = src.find('%', i + 1);
            if (end == std::string::npos) break;
            std::string block = src.substr(i + 1, end - i - 1);
            std::size_t p = 0;
            while (p < block.size()) {
                const auto star = block.find('*', p);
                const auto n = (star == std::string::npos) ? block.size() : star;
                auto cmd = block.substr(p, n - p);
                if (!cmd.empty()) out.push_back(cmd);
                if (star == std::string::npos) break;
                p = star + 1;
            }
            i = end + 1;
        } else {
            const auto end = src.find('*', i);
            if (end == std::string::npos) break;
            auto cmd = src.substr(i, end - i);
            if (!cmd.empty()) out.push_back(cmd);
            i = end + 1;
        }
    }
    return out;
}

struct GerberState {
    int xDecimals = 6;
    int yDecimals = 6;
    double unitScale = 1.0;
    double x = 0.0;
    double y = 0.0;
    int currentD = 0;
    int interpolation = 1; // 1=line, 2=CW arc, 3=CCW arc
    bool dark = true;
    bool inRegion = false;
    std::string definingMacro;
    std::string currentNet;
    std::vector<ccc::core::Point2> region;
    std::unordered_map<int, Aperture> apertures;
    std::unordered_map<std::string, ApertureMacro> macros;
};

std::optional<std::string> tokenAfterLetter(std::string_view cmd, char letter) {
    const auto p = cmd.find(letter);
    if (p == std::string_view::npos) return std::nullopt;
    std::size_t e = p + 1;
    if (e < cmd.size() && (cmd[e] == '+' || cmd[e] == '-')) ++e;
    while (e < cmd.size()
           && (std::isdigit(static_cast<unsigned char>(cmd[e])) || cmd[e] == '.')) {
        ++e;
    }
    if (e == p + 1) return std::nullopt;
    return std::string(cmd.substr(p + 1, e - p - 1));
}

std::optional<int> dCode(std::string_view cmd) {
    auto tok = tokenAfterLetter(cmd, 'D');
    if (!tok) return std::nullopt;
    try {
        return std::stoi(*tok);
    } catch (...) {
        return std::nullopt;
    }
}

double parseCoord(const std::string& token, int decimals, double unitScale) {
    if (token.find('.') != std::string::npos) return std::stod(token) * unitScale;
    const double div = std::pow(10.0, decimals);
    return (std::stod(token) / div) * unitScale;
}

std::optional<double> coord(std::string_view cmd, char letter,
                            int decimals, double unitScale) {
    auto tok = tokenAfterLetter(cmd, letter);
    if (!tok) return std::nullopt;
    return parseCoord(*tok, decimals, unitScale);
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::size_t p = 0;
    while (p <= s.size()) {
        const auto n = s.find(',', p);
        out.push_back(s.substr(p, n == std::string::npos ? std::string::npos : n - p));
        if (n == std::string::npos) break;
        p = n + 1;
    }
    return out;
}

std::optional<double> parseNumberToken(const std::string& s) {
    if (s.empty() || s.front() == '$') return std::nullopt;
    try {
        return std::stod(s);
    } catch (...) {
        return std::nullopt;
    }
}

ccc::core::Point2 rotatePoint(ccc::core::Point2 p, double degrees) {
    if (std::abs(degrees) < 1e-12) return p;
    const double a = degrees * kPi / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);
    return {p[0] * c - p[1] * s, p[0] * s + p[1] * c};
}

void addPolygon(std::vector<GerberPolygon>& out, const std::string& layerId,
                const std::vector<ccc::core::Point2>& points,
                double cx, double cy, double rotationDeg = 0.0) {
    if (points.size() < 3) return;
    GerberPolygon p;
    p.layerId = layerId;
    p.outline.reserve(points.size());
    for (auto q : points) {
        q = rotatePoint(q, rotationDeg);
        p.outline.push_back({cx + q[0], cy + q[1]});
    }
    out.push_back(std::move(p));
}

void addCircle(std::vector<GerberPolygon>& out, const std::string& layerId,
               double cx, double cy, double diameter, int segments = 48) {
    if (diameter <= 0.0) return;
    GerberPolygon p;
    p.layerId = layerId;
    p.outline.reserve(static_cast<std::size_t>(segments));
    const double r = diameter * 0.5;
    for (int i = 0; i < segments; ++i) {
        const double a = 2.0 * kPi * double(i) / double(segments);
        p.outline.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    out.push_back(std::move(p));
}

void addRect(std::vector<GerberPolygon>& out, const std::string& layerId,
             double cx, double cy, double w, double h) {
    if (w <= 0.0 || h <= 0.0) return;
    const double x0 = cx - w * 0.5;
    const double x1 = cx + w * 0.5;
    const double y0 = cy - h * 0.5;
    const double y1 = cy + h * 0.5;
    out.push_back({layerId, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}});
}

void addCapsule(std::vector<GerberPolygon>& out, const std::string& layerId,
                double x0, double y0, double x1, double y1, double width);

void addObround(std::vector<GerberPolygon>& out, const std::string& layerId,
                double cx, double cy, double w, double h) {
    if (w <= 0.0 || h <= 0.0) return;
    if (std::abs(w - h) < 1e-9) {
        addCircle(out, layerId, cx, cy, w);
    } else if (w > h) {
        const double dx = (w - h) * 0.5;
        addCapsule(out, layerId, cx - dx, cy, cx + dx, cy, h);
    } else {
        const double dy = (h - w) * 0.5;
        addCapsule(out, layerId, cx, cy - dy, cx, cy + dy, w);
    }
}

void addCapsule(std::vector<GerberPolygon>& out, const std::string& layerId,
                double x0, double y0, double x1, double y1, double width) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
        addCircle(out, layerId, x0, y0, width);
        return;
    }
    const double r = width * 0.5;
    const double angle = std::atan2(dy, dx);
    GerberPolygon p;
    p.layerId = layerId;
    constexpr int kCapSegments = 12;
    p.outline.reserve(kCapSegments * 2 + 2);
    for (int i = 0; i <= kCapSegments; ++i) {
        const double a = angle - kPi * 0.5 + kPi * double(i) / double(kCapSegments);
        p.outline.push_back({x1 + r * std::cos(a), y1 + r * std::sin(a)});
    }
    for (int i = 0; i <= kCapSegments; ++i) {
        const double a = angle + kPi * 0.5 + kPi * double(i) / double(kCapSegments);
        p.outline.push_back({x0 + r * std::cos(a), y0 + r * std::sin(a)});
    }
    out.push_back(std::move(p));
}

void addStroke(std::vector<GerberPolygon>& out, const std::string& layerId,
               const Aperture& ap, double x0, double y0, double x1, double y1);

std::vector<ccc::core::Point2> arcPolyline(double x0, double y0, double x1, double y1,
                                           double i, double j, bool clockwise) {
    const double cx = x0 + i;
    const double cy = y0 + j;
    const double r = std::hypot(i, j);
    if (r < 1e-9) return {{x1, y1}};

    const double a0 = std::atan2(y0 - cy, x0 - cx);
    const double a1 = std::atan2(y1 - cy, x1 - cx);
    double sweep = a1 - a0;
    if (clockwise) {
        while (sweep >= 0.0) sweep -= 2.0 * kPi;
    } else {
        while (sweep <= 0.0) sweep += 2.0 * kPi;
    }
    if (std::hypot(x1 - x0, y1 - y0) < 1e-9 && std::abs(sweep) < 1e-9) {
        sweep = clockwise ? -2.0 * kPi : 2.0 * kPi;
    }

    const int steps = std::clamp(int(std::ceil(std::abs(sweep) * r / 0.12)), 8, 160);
    std::vector<ccc::core::Point2> pts;
    pts.reserve(static_cast<std::size_t>(steps));
    for (int step = 1; step <= steps; ++step) {
        const double a = a0 + sweep * double(step) / double(steps);
        pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    pts.back() = {x1, y1};
    return pts;
}

void addArcStroke(std::vector<GerberPolygon>& out, const std::string& layerId,
                  const Aperture& ap, double x0, double y0, double x1, double y1,
                  double i, double j, bool clockwise) {
    auto pts = arcPolyline(x0, y0, x1, y1, i, j, clockwise);
    double ax = x0;
    double ay = y0;
    for (const auto& p : pts) {
        addStroke(out, layerId, ap, ax, ay, p[0], p[1]);
        ax = p[0];
        ay = p[1];
    }
}

void addStroke(std::vector<GerberPolygon>& out, const std::string& layerId,
               const Aperture& ap, double x0, double y0, double x1, double y1) {
    if (ap.type == "C" || ap.type == "O") {
        addCapsule(out, layerId, x0, y0, x1, y1, std::max(ap.a, ap.b));
        return;
    }
    addCapsule(out, layerId, x0, y0, x1, y1, std::max(ap.a, ap.b));
}

void addFlash(std::vector<GerberPolygon>& out, const std::string& layerId,
              const Aperture& ap, double x, double y) {
    if (!ap.macroPolygon.empty()) {
        addPolygon(out, layerId, ap.macroPolygon, x, y);
    } else if (ap.type == "C") {
        addCircle(out, layerId, x, y, ap.a);
    } else if (ap.type == "O") {
        addObround(out, layerId, x, y, ap.a, ap.b > 0.0 ? ap.b : ap.a);
    } else {
        addRect(out, layerId, x, y, ap.a, ap.b > 0.0 ? ap.b : ap.a);
    }
}

void parseFormat(GerberState& st, std::string_view cmd) {
    std::regex re(R"(FS[LT]?A?X(\d)(\d)Y(\d)(\d))", std::regex::icase);
    std::cmatch m;
    const std::string c(cmd);
    if (std::regex_search(c.c_str(), m, re)) {
        st.xDecimals = std::stoi(m[2].str());
        st.yDecimals = std::stoi(m[4].str());
    }
}

bool captureMacroPrimitive(GerberState& st, const std::string& cmd) {
    if (st.definingMacro.empty()) return false;
    if (cmd.empty()) return true;
    if (cmd[0] == '0') return true; // macro comment
    if (!std::isdigit(static_cast<unsigned char>(cmd[0]))) {
        st.definingMacro.clear();
        return false;
    }

    const auto parts = splitComma(cmd);
    if (parts.size() < 5 || parts[0] != "4") return true;
    const auto count = parseNumberToken(parts[2]);
    if (!count || *count < 3) return true;
    const int n = int(*count);
    if (parts.size() < std::size_t(3 + n * 2 + 1)) return true;

    ApertureMacro macro;
    macro.polygon.reserve(static_cast<std::size_t>(n));
    for (int idx = 0; idx < n; ++idx) {
        const auto x = parseNumberToken(parts[3 + idx * 2]);
        const auto y = parseNumberToken(parts[4 + idx * 2]);
        if (!x || !y) {
            macro.polygon.clear();
            break;
        }
        macro.polygon.push_back({*x * st.unitScale, *y * st.unitScale});
    }
    const std::string& rotation = parts[3 + n * 2];
    if (rotation.size() > 1 && rotation.front() == '$') {
        try {
            macro.rotationParam = std::stoi(rotation.substr(1));
        } catch (...) {
            macro.rotationParam = 0;
        }
    }
    if (!macro.polygon.empty()) st.macros[upper(st.definingMacro)] = std::move(macro);
    return true;
}

void parseAperture(GerberState& st, std::string_view cmd) {
    std::regex re(R"(ADD(\d+)([A-Za-z_][A-Za-z0-9_.-]*),?([^*]*))");
    std::cmatch m;
    const std::string c(cmd);
    if (!std::regex_match(c.c_str(), m, re)) return;
    const int code = std::stoi(m[1].str());
    Aperture ap;
    ap.type = upper(m[2].str());
    std::string params = m[3].str();
    const std::vector<std::string> values = splitComma(params.empty() ? std::string{} : params);
    const auto x = upper(params).find('X');
    try {
        if (ap.type == "ROUNDRECT") {
            auto nums = splitComma(params);
            if (nums.size() == 1) {
                nums.clear();
                std::string tmp = params;
                std::size_t p = 0;
                while (p <= tmp.size()) {
                    const auto n = tmp.find('X', p);
                    nums.push_back(tmp.substr(p, n == std::string::npos ? std::string::npos : n - p));
                    if (n == std::string::npos) break;
                    p = n + 1;
                }
            }
            if (nums.size() >= 9) {
                for (int i = 0; i < 4; ++i) {
                    const double px = std::stod(nums[1 + i * 2]) * st.unitScale;
                    const double py = std::stod(nums[2 + i * 2]) * st.unitScale;
                    ap.macroPolygon.push_back(rotatePoint({px, py},
                                                          nums.size() > 9 ? std::stod(nums[9]) : 0.0));
                }
            }
            ap.a = ap.b = 0.0;
        } else if (auto macroIt = st.macros.find(ap.type); macroIt != st.macros.end()) {
            double rotation = 0.0;
            if (macroIt->second.rotationParam > 0
                && values.size() >= std::size_t(macroIt->second.rotationParam)) {
                rotation = std::stod(values[std::size_t(macroIt->second.rotationParam - 1)]);
            }
            ap.macroPolygon.reserve(macroIt->second.polygon.size());
            for (auto p : macroIt->second.polygon) ap.macroPolygon.push_back(rotatePoint(p, rotation));
        } else if (x == std::string::npos) {
            ap.a = std::stod(params) * st.unitScale;
            ap.b = ap.a;
        } else {
            ap.a = std::stod(params.substr(0, x)) * st.unitScale;
            ap.b = std::stod(params.substr(x + 1)) * st.unitScale;
        }
        st.apertures[code] = ap;
    } catch (...) {
    }
}

}  // namespace

std::string inferGerberLayerId(const std::string& path) {
    const std::string ext = upper(std::filesystem::path(path).extension().string());
    if (ext == ".GTL") return "F.Cu";
    if (ext == ".GBL") return "B.Cu";
    if (ext.size() >= 3 && ext[0] == '.' && ext[1] == 'G'
        && std::isdigit(static_cast<unsigned char>(ext[2]))) {
        try {
            const int inner = std::stoi(ext.substr(2));
            if (inner > 0) return "In" + std::to_string(inner) + ".Cu";
        } catch (...) {
        }
    }
    if (ext.size() >= 4 && ext[0] == '.' && ext[1] == 'G' && ext[2] == 'P'
        && std::isdigit(static_cast<unsigned char>(ext[3]))) {
        try {
            const int inner = std::stoi(ext.substr(3));
            if (inner > 0) return "In" + std::to_string(inner) + ".Cu";
        } catch (...) {
        }
    }
    const std::string stem = upper(std::filesystem::path(path).stem().string());
    auto has = [&](std::string_view s) { return stem.find(s) != std::string::npos; };
    if (has("EDGECUT") || has("EDGE_CUT") || has("EDGE-CUT") || has("EDGE.CUT")
        || has("PROFILE") || has("OUTLINE") || has("GM1") || has("GKO")) {
        return "Edge.Cuts";
    }
    if (has("F_CU") || has("F.CU") || has("F-CU") || has("GTL")
        || has("CUTOP") || has("CU_TOP") || has("CU-TOP") || has("TOPCU")
        || has("TOP_CU") || has("TOP-CU")) {
        return "F.Cu";
    }
    if (has("B_CU") || has("B.CU") || has("B-CU") || has("GBL")
        || has("CUBOT") || has("CU_BOT") || has("CU-BOT") || has("CUBOTTOM")
        || has("BOTTOMCU") || has("BOTTOM_CU") || has("BOTTOM-CU")) {
        return "B.Cu";
    }
    std::regex innerAfter(R"(CU[_.-]?IN(\d+))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(stem, m, innerAfter)) return "In" + m[1].str() + ".Cu";
    std::regex innerBefore(R"(IN(\d+)[_.-]?CU)", std::regex::icase);
    if (std::regex_search(stem, m, innerBefore)) return "In" + m[1].str() + ".Cu";
    std::regex layer(R"(L(\d+))", std::regex::icase);
    if (std::regex_search(stem, m, layer)) return "L" + m[1].str() + ".Cu";
    return std::filesystem::path(path).stem().string();
}

GerberLayer readGerberLayer(const std::string& path, const std::string& layerIdIn) {
    const std::string layerId = layerIdIn.empty() ? inferGerberLayerId(path) : layerIdIn;
    GerberState st;
    GerberLayer layer;
    layer.layerId = layerId;
    layer.sourcePath = path;

    for (const auto& raw : splitGerberCommands(readTextFile(path))) {
        const std::string rawCmd = trim(raw);
        const std::string cmd = upper(rawCmd);
        if (cmd.empty()) continue;
        if (startsWith(cmd, "AM")) {
            st.definingMacro = rawCmd.substr(2);
            continue;
        }
        if (captureMacroPrimitive(st, cmd)) continue;
        if (startsWith(cmd, "TO.N,")) {
            st.currentNet = rawCmd.substr(5);
            continue;
        }
        if (cmd == "TD") {
            st.currentNet.clear();
            continue;
        }

        if (startsWith(cmd, "FS")) {
            parseFormat(st, cmd);
            continue;
        }
        if (startsWith(cmd, "MOMM")) {
            st.unitScale = 1.0;
            continue;
        }
        if (startsWith(cmd, "MOIN")) {
            st.unitScale = 25.4;
            continue;
        }
        if (startsWith(cmd, "LPD")) {
            st.dark = true;
            continue;
        }
        if (startsWith(cmd, "LPC")) {
            st.dark = false;
            continue;
        }
        if (startsWith(cmd, "ADD")) {
            parseAperture(st, cmd);
            continue;
        }
        if (startsWith(cmd, "G04")) {
            continue;
        }
        if (startsWith(cmd, "G75")) {
            continue;
        }
        if (startsWith(cmd, "G01")) {
            st.interpolation = 1;
            if (cmd.find('X') == std::string::npos
                && cmd.find('Y') == std::string::npos
                && !dCode(cmd)) continue;
        } else if (startsWith(cmd, "G02")) {
            st.interpolation = 2;
            if (cmd.find('X') == std::string::npos
                && cmd.find('Y') == std::string::npos
                && !dCode(cmd)) continue;
        } else if (startsWith(cmd, "G03")) {
            st.interpolation = 3;
            if (cmd.find('X') == std::string::npos
                && cmd.find('Y') == std::string::npos
                && !dCode(cmd)) continue;
        }
        if (startsWith(cmd, "G36")) {
            st.inRegion = true;
            st.region.clear();
            continue;
        }
        if (startsWith(cmd, "G37")) {
            if (st.dark && st.region.size() >= 3) {
                GerberPolygon p{layerId, st.region};
                p.net = st.currentNet;
                layer.polygons.push_back(std::move(p));
            }
            st.region.clear();
            st.inRegion = false;
            continue;
        }
        if (cmd.size() > 1 && cmd[0] == 'D' && std::isdigit(static_cast<unsigned char>(cmd[1]))) {
            auto d = dCode(cmd);
            if (d && *d >= 10) st.currentD = *d;
            continue;
        }

        const double oldX = st.x;
        const double oldY = st.y;
        if (auto x = coord(cmd, 'X', st.xDecimals, st.unitScale)) st.x = *x;
        if (auto y = coord(cmd, 'Y', st.yDecimals, st.unitScale)) st.y = *y;
        const auto arcI = coord(cmd, 'I', st.xDecimals, st.unitScale);
        const auto arcJ = coord(cmd, 'J', st.yDecimals, st.unitScale);
        const int op = dCode(cmd).value_or((cmd.find('X') != std::string::npos
                                            || cmd.find('Y') != std::string::npos)
                                               ? 1
                                               : 0);
        if (op == 2) {
            if (st.inRegion && st.region.empty()) st.region.push_back({st.x, st.y});
            continue;
        }
        if (op == 1) {
            const bool isArc = (st.interpolation == 2 || st.interpolation == 3)
                               && arcI && arcJ;
            if (st.inRegion) {
                if (st.region.empty()) st.region.push_back({oldX, oldY});
                if (isArc) {
                    auto pts = arcPolyline(oldX, oldY, st.x, st.y, *arcI, *arcJ,
                                           st.interpolation == 2);
                    st.region.insert(st.region.end(), pts.begin(), pts.end());
                } else {
                    st.region.push_back({st.x, st.y});
                }
            } else if (st.dark) {
                auto it = st.apertures.find(st.currentD);
                if (it != st.apertures.end()) {
                    const auto before = layer.polygons.size();
                    if (isArc) {
                        addArcStroke(layer.polygons, layerId, it->second,
                                     oldX, oldY, st.x, st.y, *arcI, *arcJ,
                                     st.interpolation == 2);
                    } else {
                        addStroke(layer.polygons, layerId, it->second,
                                  oldX, oldY, st.x, st.y);
                    }
                    for (std::size_t k = before; k < layer.polygons.size(); ++k) {
                        layer.polygons[k].net = st.currentNet;
                    }
                }
            }
            continue;
        }
        if (op == 3 && st.dark) {
            auto it = st.apertures.find(st.currentD);
            if (it != st.apertures.end()) {
                const auto before = layer.polygons.size();
                addFlash(layer.polygons, layerId, it->second, st.x, st.y);
                for (std::size_t k = before; k < layer.polygons.size(); ++k) {
                    layer.polygons[k].net = st.currentNet;
                }
            }
        }
    }

    return layer;
}

}  // namespace ccc::io
