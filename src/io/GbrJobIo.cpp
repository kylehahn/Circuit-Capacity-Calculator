#include "GbrJobIo.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>

namespace ccc::io {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

double numberOrZero(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return 0.0;
    if (j.at(key).is_number()) return j.at(key).get<double>();
    if (j.at(key).is_string()) {
        try {
            return std::stod(j.at(key).get<std::string>());
        } catch (...) {
        }
    }
    return 0.0;
}

std::string layerFromFunction(const std::string& fileFunction, const std::string& fallbackPath) {
    const std::string f = upper(fileFunction);
    if (f.find("COPPER") == std::string::npos) return {};
    if (f.find("TOP") != std::string::npos) return "F.Cu";
    if (f.find("BOT") != std::string::npos) return "B.Cu";
    std::smatch m;
    if (std::regex_search(f, m, std::regex(R"((?:^|,)L(\d+)(?:,|$))"))) {
        const int physicalLayer = std::stoi(m[1].str());
        if (physicalLayer > 1) return "In" + std::to_string(physicalLayer - 1) + ".Cu";
    }
    const std::string stem = upper(std::filesystem::path(fallbackPath).stem().string());
    if (stem.find("CUTOP") != std::string::npos
        || stem.find("CU_TOP") != std::string::npos
        || stem.find("CU-TOP") != std::string::npos
        || stem.find("TOPCU") != std::string::npos
        || stem.find("TOP_CU") != std::string::npos
        || stem.find("TOP-CU") != std::string::npos) {
        return "F.Cu";
    }
    if (stem.find("CUBOT") != std::string::npos
        || stem.find("CU_BOT") != std::string::npos
        || stem.find("CU-BOT") != std::string::npos
        || stem.find("CUBOTTOM") != std::string::npos
        || stem.find("BOTTOMCU") != std::string::npos
        || stem.find("BOTTOM_CU") != std::string::npos
        || stem.find("BOTTOM-CU") != std::string::npos) {
        return "B.Cu";
    }
    std::smatch stemMatch;
    if (std::regex_search(stem, stemMatch, std::regex(R"(CU[_.-]?IN(\d+))"))) {
        return "In" + stemMatch[1].str() + ".Cu";
    }
    if (std::regex_search(stem, stemMatch, std::regex(R"(IN(\d+)[_.-]?CU)"))) {
        return "In" + stemMatch[1].str() + ".Cu";
    }
    const auto in = stem.find("IN");
    const auto cu = stem.find("CU", in == std::string::npos ? 0 : in);
    if (in != std::string::npos && cu != std::string::npos && cu > in + 2) {
        return "In" + stem.substr(in + 2, cu - in - 3) + ".Cu";
    }
    return std::filesystem::path(fallbackPath).stem().string();
}

}  // namespace

GbrJobInfo readGbrJob(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("gbrjob: cannot open " + path);
    nlohmann::json j;
    in >> j;

    GbrJobInfo info;
    auto readFileAttrs = [&](const nlohmann::json& attrs) {
        if (!attrs.is_array()) return;
        for (const auto& f : attrs) {
            const std::string p = f.value("Path", f.value("File", ""));
            const std::string fn = f.value("FileFunction", "");
            const std::string layer = layerFromFunction(fn, p);
            if (!p.empty() && !layer.empty())
                info.fileToLayer[std::filesystem::path(p).filename().string()] = layer;
        }
    };
    if (auto it = j.find("FilesAttributes"); it != j.end()) readFileAttrs(*it);
    if (auto it = j.find("FileAttributes"); it != j.end()) readFileAttrs(*it);

    if (auto it = j.find("MaterialStackup"); it != j.end() && it->is_array()) {
        for (const auto& row : *it) {
            GbrJobLayer l;
            l.id = row.value("Name", row.value("Layer", ""));
            l.type = row.value("Type", "");
            const std::string t = upper(l.type + " " + l.id);
            l.isCopper = t.find("COPPER") != std::string::npos
                         || t.find(".CU") != std::string::npos;
            l.thickness = numberOrZero(row, "Thickness");
            l.epsR = numberOrZero(row, "EpsilonR");
            if (!l.id.empty()) info.layers.push_back(std::move(l));
        }
    }
    return info;
}

}  // namespace ccc::io
