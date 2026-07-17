#include "ExcellonIo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ccc::io {

namespace {

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

double parseExcellonCoord(const std::string& token, double unitScale) {
    if (token.find('.') != std::string::npos) return std::stod(token) * unitScale;
    return (std::stod(token) / 1000.0) * unitScale;
}

bool extractCoord(const std::string& line, char letter, double unitScale, double& out) {
    const auto p = line.find(letter);
    if (p == std::string::npos) return false;
    std::size_t e = p + 1;
    if (e < line.size() && (line[e] == '+' || line[e] == '-')) ++e;
    while (e < line.size()
           && (std::isdigit(static_cast<unsigned char>(line[e])) || line[e] == '.')) {
        ++e;
    }
    if (e == p + 1) return false;
    out = parseExcellonCoord(line.substr(p + 1, e - p - 1), unitScale);
    return true;
}

}  // namespace

std::vector<DrillHit> readExcellonDrillFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Excellon: cannot open " + path);

    std::unordered_map<int, double> tools;
    int currentTool = -1;
    double unitScale = 1.0;
    double x = 0.0;
    double y = 0.0;
    std::vector<DrillHit> hits;

    std::string raw;
    std::regex toolDef(R"(^T0*(\d+)C([0-9.+-]+))", std::regex::icase);
    std::regex toolSel(R"(^T0*(\d+)$)", std::regex::icase);
    while (std::getline(in, raw)) {
        auto line = trim(raw);
        const auto semi = line.find(';');
        if (semi != std::string::npos) line = trim(line.substr(0, semi));
        if (line.empty()) continue;
        line = upper(line);
        if (line.find("METRIC") != std::string::npos) {
            unitScale = 1.0;
            continue;
        }
        if (line.find("INCH") != std::string::npos) {
            unitScale = 25.4;
            continue;
        }
        std::smatch m;
        if (std::regex_search(line, m, toolDef)) {
            tools[std::stoi(m[1].str())] = std::stod(m[2].str()) * unitScale;
            continue;
        }
        if (std::regex_match(line, m, toolSel)) {
            currentTool = std::stoi(m[1].str());
            continue;
        }
        bool hasCoord = false;
        double v = 0.0;
        if (extractCoord(line, 'X', unitScale, v)) {
            x = v;
            hasCoord = true;
        }
        if (extractCoord(line, 'Y', unitScale, v)) {
            y = v;
            hasCoord = true;
        }
        if (!hasCoord || currentTool < 0) continue;
        const auto it = tools.find(currentTool);
        if (it == tools.end()) continue;
        hits.push_back({x, y, it->second, "F.Cu", "B.Cu"});
    }
    return hits;
}

}  // namespace ccc::io
