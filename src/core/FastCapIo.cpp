#include "FastCapIo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ccc::core {

namespace {

std::string quoteArg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string processCommand(const std::string& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
    // _popen runs through cmd.exe; "call" avoids cmd's fragile handling of a
    // command line that starts with a quoted executable path.
    std::string command = "call " + quoteArg(exe);
#else
    std::string command = quoteArg(exe);
#endif
    for (const auto& arg : args) command += " " + quoteArg(arg);
    command += " 2>&1";
    return command;
}

std::string runAndCapture(const std::string& command, bool allowNonZero = false) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) throw std::runtime_error("external capacitance solver: failed to spawn process");
    std::string output;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), int(buf.size()), pipe)) output += buf.data();
#ifdef _WIN32
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (rc != 0 && !allowNonZero) {
        std::string msg = "external capacitance solver: process failed with exit code "
                          + std::to_string(rc);
        if (!output.empty()) {
            msg += "\n\nSolver output:\n";
            msg += output.substr(0, 1200);
        }
        throw std::runtime_error(msg);
    }
    return output;
}

std::string trimCopy(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                    [&](unsigned char c) { return !isSpace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [&](unsigned char c) { return !isSpace(c); }).base(), s.end());
    return s;
}

void emitProgress(const FastCapProgressCallback& cb,
                  std::string phase,
                  std::string detail = {},
                  int step = 0,
                  int totalSteps = 0,
                  int panelsA = 0,
                  int panelsB = 0,
                  int panelsEnvironment = 0,
                  int dielectricPanels = 0,
                  std::size_t outputBytes = 0,
                  double elapsedSeconds = 0.0) {
    if (!cb) return;
    FastCapProgress p;
    p.phase = std::move(phase);
    p.detail = std::move(detail);
    p.step = step;
    p.totalSteps = totalSteps;
    p.panelsA = panelsA;
    p.panelsB = panelsB;
    p.panelsEnvironment = panelsEnvironment;
    p.dielectricPanels = dielectricPanels;
    p.outputBytes = outputBytes;
    p.elapsedSeconds = elapsedSeconds;
    cb(p);
}

std::string runSolverAndCapture(const std::string& exe,
                                const std::vector<std::string>& args,
                                bool allowNonZero,
                                const std::atomic<bool>* stopFlag,
                                double timeoutSeconds,
                                const FastCapProgressCallback& progressCallback = {}) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        throw std::runtime_error("external capacitance solver: failed to create stdout pipe");
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::string command = quoteArg(exe);
    for (const auto& arg : args) command += " " + quoteArg(arg);
    std::vector<char> cmd(command.begin(), command.end());
    cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        throw std::runtime_error("external capacitance solver: failed to spawn process");
    }

    std::string output;
    std::array<char, 4096> buf{};
    const auto t0 = std::chrono::steady_clock::now();
    bool killed = false;
    std::string killReason;
    std::string pendingLine;
    auto elapsedSeconds = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    };
    auto appendOutput = [&](const char* data, DWORD size) {
        output.append(data, size);
        pendingLine.append(data, size);
        for (;;) {
            const auto pos = pendingLine.find('\n');
            if (pos == std::string::npos) break;
            std::string line = trimCopy(pendingLine.substr(0, pos));
            pendingLine.erase(0, pos + 1);
            if (!line.empty()) {
                emitProgress(progressCallback, "Solver output", line, 0, 0,
                             0, 0, 0, 0, output.size(), elapsedSeconds());
            }
        }
    };
    for (;;) {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)
               && available > 0) {
            DWORD read = 0;
            const DWORD want = std::min<DWORD>(DWORD(buf.size() - 1), available);
            if (!ReadFile(readPipe, buf.data(), want, &read, nullptr) || read == 0) break;
            appendOutput(buf.data(), read);
            available = 0;
        }

        const DWORD wait = WaitForSingleObject(pi.hProcess, 25);
        if (wait == WAIT_OBJECT_0) break;

        if (stopFlag && stopFlag->load(std::memory_order_relaxed)) {
            killed = true;
            killReason = "external capacitance solver: cancelled";
        } else if (timeoutSeconds > 0.0) {
            const auto elapsed = elapsedSeconds();
            if (elapsed > timeoutSeconds) {
                killed = true;
                std::ostringstream ss;
                ss << "external capacitance solver: timed out after "
                   << timeoutSeconds << " seconds";
                killReason = ss.str();
            }
        }
        if (killed) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }

    DWORD available = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)
           && available > 0) {
        DWORD read = 0;
        const DWORD want = std::min<DWORD>(DWORD(buf.size() - 1), available);
        if (!ReadFile(readPipe, buf.data(), want, &read, nullptr) || read == 0) break;
        appendOutput(buf.data(), read);
        available = 0;
    }
    const std::string tail = trimCopy(pendingLine);
    if (!tail.empty()) {
        emitProgress(progressCallback, "Solver output", tail, 0, 0,
                     0, 0, 0, 0, output.size(), elapsedSeconds());
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);

    if (killed) {
        if (!output.empty()) {
            killReason += "\n\nSolver output:\n";
            killReason += output.substr(0, 1200);
        }
        throw std::runtime_error(killReason);
    }
    if (exitCode != 0 && !allowNonZero) {
        std::string msg = "external capacitance solver: process failed with exit code "
                          + std::to_string(exitCode);
        if (!output.empty()) {
            msg += "\n\nSolver output:\n";
            msg += output.substr(0, 1200);
        }
        throw std::runtime_error(msg);
    }
    return output;
#else
    (void)stopFlag;
    (void)timeoutSeconds;
    (void)progressCallback;
    return runAndCapture(processCommand(exe, args), allowNonZero);
#endif
}

std::vector<double> numbersInLine(std::string line) {
    for (char& c : line) {
        if (c == 'D' || c == 'd') c = 'E';
    }
    static const std::regex number(
        R"([+-]?(?:(?:\d+\.\d*)|(?:\.\d+)|(?:\d+))(?:[eE][+-]?\d+)?)");
    std::vector<double> out;
    for (std::sregex_iterator it(line.begin(), line.end(), number), end; it != end; ++it) {
        out.push_back(std::stod(it->str()));
    }
    return out;
}

struct FastCapMatrix {
    std::vector<std::vector<double>> values;
    double unitScale = 1.0;
};

double fastCapUnitScale(const std::string& headerLine) {
    std::string upper = headerLine;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    if (upper.find("ATTOFARAD") != std::string::npos) return 1.0e-18;
    if (upper.find("FEMTOFARAD") != std::string::npos) return 1.0e-15;
    if (upper.find("PICOFARAD") != std::string::npos) return 1.0e-12;
    if (upper.find("NANOFARAD") != std::string::npos) return 1.0e-9;
    if (upper.find("MICROFARAD") != std::string::npos) return 1.0e-6;
    if (upper.find("MILLIFARAD") != std::string::npos) return 1.0e-3;
    return 1.0;
}

FastCapMatrix parseFastCapMatrix(const std::string& stdoutText, int expectedN = 2) {
    std::istringstream in(stdoutText);
    std::string line;
    FastCapMatrix current;
    FastCapMatrix lastComplete;
    bool inMatrix = false;
    while (std::getline(in, line)) {
        std::string upper = line;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        if (upper.find("CAPACITANCE") != std::string::npos) {
            if (upper.find("MATRIX") != std::string::npos) {
                inMatrix = true;
                current = {};
                current.values.reserve(std::max(expectedN, 0));
            }
            if (inMatrix) current.unitScale = fastCapUnitScale(line);
            continue;
        }
        if (!inMatrix) continue;
        if (upper.find("DIMENSION") != std::string::npos) {
            continue;
        }
        auto nums = numbersInLine(line);
        if (expectedN > 0 && nums.size() >= std::size_t(expectedN + 1)) {
            std::vector<double> row;
            row.reserve(std::size_t(expectedN));
            for (int i = expectedN; i > 0; --i)
                row.push_back(nums[nums.size() - std::size_t(i)]);
            current.values.push_back(std::move(row));
        }
        if (expectedN > 0 && int(current.values.size()) == expectedN) {
            lastComplete = current;
            inMatrix = false;
        }
    }
    if (expectedN <= 0 || int(lastComplete.values.size()) != expectedN)
        throw std::runtime_error("FasterCap: could not parse capacitance matrix");
    for (auto& row : lastComplete.values) {
        for (double& value : row) value *= lastComplete.unitScale;
    }
    return lastComplete;
}

std::filesystem::path makeTempQuiPath() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path()
                / ("ccc_fastercap_" + std::to_string(now) + ".qui");
    return path;
}

std::filesystem::path makeTempProblemDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path()
                / ("ccc_fastercap_" + std::to_string(now));
    std::filesystem::create_directories(path);
    return path;
}

void appendPanel(std::ostringstream& out, int conductor, const Panel& p) {
    const double half = p.a * 0.5e-3;
    const double x = p.x * 1e-3;
    const double y = p.y * 1e-3;
    const double z = p.z * 1e-3;
    out << "Q " << conductor << ' '
        << x - half << ' ' << y - half << ' ' << z << ' '
        << x + half << ' ' << y - half << ' ' << z << ' '
        << x + half << ' ' << y + half << ' ' << z << ' '
        << x - half << ' ' << y + half << ' ' << z << '\n';
}

struct BBox {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    bool valid = false;

    void add(double x, double y) {
        if (!valid) {
            x0 = x1 = x;
            y0 = y1 = y;
            valid = true;
            return;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }

    void add(const Panel& p) {
        const double half = p.a * 0.5;
        add(p.x - half, p.y - half);
        add(p.x + half, p.y + half);
    }

    void expand(double margin) {
        if (!valid) return;
        x0 -= margin;
        y0 -= margin;
        x1 += margin;
        y1 += margin;
    }

    bool contains(const Panel& p) const {
        return valid && p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
    }
};

BBox boundsOf(const std::vector<Panel>& a, const std::vector<Panel>& b) {
    BBox box;
    for (const auto& p : a) box.add(p);
    for (const auto& p : b) box.add(p);
    return box;
}

std::vector<Panel> filterPanels(const std::vector<Panel>& panels, const BBox& box) {
    if (!box.valid) return panels;
    std::vector<Panel> out;
    out.reserve(panels.size());
    for (const auto& p : panels) {
        if (box.contains(p)) out.push_back(p);
    }
    return out;
}

struct LayerSpan {
    const Layer* layer = nullptr;
    double z0 = 0.0;
    double z1 = 0.0;
};

std::vector<LayerSpan> layerSpans(const Model& model) {
    std::vector<LayerSpan> spans;
    spans.reserve(model.layers.size());
    double z = 0.0;
    for (const auto& layer : model.layers) {
        const double z1 = z + std::max(0.0, layer.thickness);
        spans.push_back({&layer, z, z1});
        z = z1;
    }
    return spans;
}

bool hasDielectricStack(const std::vector<LayerSpan>& spans) {
    return std::any_of(spans.begin(), spans.end(), [](const LayerSpan& s) {
        return s.layer && !s.layer->isConductor && s.layer->thickness > 0.0;
    });
}

double positivePerm(double eps, double fallback = 1.0) {
    return (std::isfinite(eps) && eps > 0.0) ? eps : fallback;
}

double fallbackPerm(const BemOptions& opts) {
    return positivePerm(opts.epsEff, 1.0);
}

int layerIndexAtZ(const std::vector<LayerSpan>& spans, double z) {
    for (int i = 0; i < int(spans.size()); ++i) {
        const auto& s = spans[std::size_t(i)];
        if (z >= s.z0 - 1e-9 && z <= s.z1 + 1e-9) return i;
    }
    return -1;
}

double nearestDielectricPerm(const std::vector<LayerSpan>& spans, int start,
                             int step, const BemOptions& opts) {
    for (int i = start; i >= 0 && i < int(spans.size()); i += step) {
        const auto* layer = spans[std::size_t(i)].layer;
        if (layer && !layer->isConductor && layer->thickness > 0.0)
            return positivePerm(layer->permittivity, fallbackPerm(opts));
    }
    return 0.0;
}

double permittivityForPanel(const std::vector<LayerSpan>& spans,
                            const Panel& panel,
                            const BemOptions& opts) {
    if (!hasDielectricStack(spans)) return fallbackPerm(opts);
    const int idx = layerIndexAtZ(spans, panel.z);
    if (idx < 0) return fallbackPerm(opts);
    const auto* layer = spans[std::size_t(idx)].layer;
    if (!layer) return fallbackPerm(opts);
    if (!layer->isConductor)
        return positivePerm(layer->permittivity, fallbackPerm(opts));

    const double above = nearestDielectricPerm(spans, idx - 1, -1, opts);
    const double below = nearestDielectricPerm(spans, idx + 1, +1, opts);
    if (above > 0.0 && below > 0.0) return 0.5 * (above + below);
    if (above > 0.0) return above;
    if (below > 0.0) return below;
    return fallbackPerm(opts);
}

double permKey(double eps) {
    return std::round(positivePerm(eps, 1.0) * 1000.0) / 1000.0;
}

struct LabeledPanel {
    int conductor = 0;
    Panel panel;
};

std::string fastCapPath(const std::filesystem::path& path) {
    return path.generic_string();
}

void writeLabeledPanelFile(const std::filesystem::path& path,
                           const std::vector<LabeledPanel>& panels) {
    std::ostringstream ss;
    ss.setf(std::ios::scientific);
    ss.precision(17);
    ss << "0 ccc_generated_units_meters\n";
    for (const auto& lp : panels) appendPanel(ss, lp.conductor, lp.panel);
    std::ofstream out(path);
    if (!out) throw std::runtime_error("FasterCap: cannot create conductor surface file");
    out << ss.str();
}

void writeDielectricPanelFile(const std::filesystem::path& path,
                              const std::vector<Panel>& panels) {
    std::ostringstream ss;
    ss.setf(std::ios::scientific);
    ss.precision(17);
    ss << "0 ccc_generated_dielectric_units_meters\n";
    for (const auto& p : panels) appendPanel(ss, 0, p);
    std::ofstream out(path);
    if (!out) throw std::runtime_error("FasterCap: cannot create dielectric surface file");
    out << ss.str();
}

std::vector<Panel> panelizeRect(const BBox& box, double z, double panelSize) {
    std::vector<Panel> out;
    if (!box.valid || panelSize <= 0.0) return out;
    const double width = std::max(0.0, box.x1 - box.x0);
    const double height = std::max(0.0, box.y1 - box.y0);
    if (width <= 0.0 || height <= 0.0) return out;
    const int nx = std::max(1, int(std::ceil(width / panelSize)));
    const int ny = std::max(1, int(std::ceil(height / panelSize)));
    const double sx = width / nx;
    const double sy = height / ny;
    out.reserve(std::size_t(nx) * std::size_t(ny));
    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            const double area = sx * sy;
            out.push_back({box.x0 + (ix + 0.5) * sx,
                           box.y0 + (iy + 0.5) * sy,
                           z,
                           area,
                           std::sqrt(area)});
        }
    }
    return out;
}

struct DielectricInterface {
    double outerPerm = 1.0;
    double innerPerm = 1.0;
    double refX = 0.0;
    double refY = 0.0;
    double refZ = 0.0;
    std::vector<Panel> panels;
};

std::vector<DielectricInterface> dielectricInterfaces(
    const std::vector<LayerSpan>& spans,
    const BBox& box,
    const BemOptions& opts,
    const FastCapEnvironmentOptions& env) {
    std::vector<DielectricInterface> out;
    if (!env.includeDielectricStack || !hasDielectricStack(spans) || !box.valid) return out;
    const double ps = env.dielectricPanelSize > 0.0
        ? env.dielectricPanelSize
        : std::max(2.0, env.environmentPanelSize);
    const double cx = 0.5 * (box.x0 + box.x1);
    const double cy = 0.5 * (box.y0 + box.y1);

    auto addInterface = [&](double z, double outerPerm, double innerPerm, double refZ) {
        outerPerm = positivePerm(outerPerm, fallbackPerm(opts));
        innerPerm = positivePerm(innerPerm, fallbackPerm(opts));
        if (std::abs(outerPerm - innerPerm) < 1e-9) return;
        DielectricInterface di;
        di.outerPerm = outerPerm;
        di.innerPerm = innerPerm;
        di.refX = cx;
        di.refY = cy;
        di.refZ = refZ;
        di.panels = panelizeRect(box, z, ps);
        if (!di.panels.empty()) out.push_back(std::move(di));
    };

    for (std::size_t i = 0; i + 1 < spans.size(); ++i) {
        const auto* a = spans[i].layer;
        const auto* b = spans[i + 1].layer;
        if (!a || !b || a->isConductor || b->isConductor) continue;
        const double epsA = positivePerm(a->permittivity, fallbackPerm(opts));
        const double epsB = positivePerm(b->permittivity, fallbackPerm(opts));
        addInterface(spans[i].z1, epsA, epsB,
                     spans[i].z1 + std::max(1e-6, b->thickness * 0.25));
    }

    return out;
}

bool refsEmpty(const ConductorRefs& refs) {
    return refs.padIds.empty() && refs.fpcIds.empty()
           && refs.traceIds.empty() && refs.zoneIds.empty();
}

double relativeDelta(double a, double b) {
    const double denom = std::max({std::abs(a), std::abs(b), 1e-30});
    return std::abs(a - b) / denom;
}

std::string normalizedNet(std::string net) {
    std::string out;
    out.reserve(net.size());
    for (unsigned char c : net) {
        if (std::isalnum(c)) out.push_back(char(std::toupper(c)));
    }
    return out;
}

bool isGroundLikeNet(const std::string& net) {
    const std::string n = normalizedNet(net);
    return n == "GND" || n == "GROUND" || n == "AGND" || n == "DGND"
           || n == "PGND" || n == "GNDA" || n == "GNDD"
           || n == "0V" || n == "VSS"
           || n.find("GND") != std::string::npos;
}

ConductorRefs collectGroundRefs(const Model& model,
                                const std::string& netA,
                                const std::string& netB) {
    ConductorRefs refs;
    auto isEnvGround = [&](const std::string& net) {
        return !net.empty() && net != netA && net != netB && isGroundLikeNet(net);
    };
    for (const auto& p : model.pads) {
        if (isEnvGround(p.net)) refs.padIds.push_back(p.id);
    }
    for (const auto& t : model.traces) {
        if (isEnvGround(t.net)) refs.traceIds.push_back(t.id);
    }
    for (const auto& z : model.zones) {
        if (isEnvGround(z.net)) refs.zoneIds.push_back(z.id);
    }
    return refs;
}

bool looksLikeFasterCapExecutable(const std::string& exe) {
    try {
        std::string out = runAndCapture(processCommand(exe, {"-b?"}), true);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        return out.find("FASTERCAP") != std::string::npos
               && out.find("CAPACITANCE") != std::string::npos;
    } catch (...) {
        return false;
    }
}

bool looksLikeFastCapExecutable(const std::string& exe) {
    try {
        std::string out = runAndCapture(processCommand(exe, {"-h"}), true);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        if (out.find("FASTIMP") != std::string::npos
            || out.find("FAST IMPEDANCE") != std::string::npos) {
            return false;
        }
        return out.find("FASTCAP") != std::string::npos
               || out.find("CAPACITANCE") != std::string::npos;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> solverArgs(ExternalCapSolver solver,
                                    const std::filesystem::path& inputPath,
                                    bool listInput,
                                    double fasterCapRelativeError) {
    switch (solver) {
    case ExternalCapSolver::FastCapFixed:
        return listInput
            ? std::vector<std::string>{"-l" + inputPath.string()}
            : std::vector<std::string>{inputPath.string()};
    case ExternalCapSolver::FasterCapAdaptive: {
        std::ostringstream err;
        err << "-a" << ((std::isfinite(fasterCapRelativeError)
                         && fasterCapRelativeError > 0.0)
                            ? fasterCapRelativeError
                            : 0.01);
        return {inputPath.string(), "-b", err.str()};
    }
    }
    return {inputPath.string()};
}

}  // namespace

std::string externalCapSolverName(ExternalCapSolver solver) {
    switch (solver) {
    case ExternalCapSolver::FastCapFixed:
        return "FastCap";
    case ExternalCapSolver::FasterCapAdaptive:
        return "FasterCap";
    }
    return "ExternalCap";
}

std::string defaultFasterCapExecutable() {
#ifdef CCC_SOURCE_DIR
    return std::string(CCC_SOURCE_DIR) + "/external/fastercap/FasterCap.exe";
#else
    return "external/fastercap/FasterCap.exe";
#endif
}

std::string defaultFastCapExecutable() {
#ifdef CCC_SOURCE_DIR
    return std::string(CCC_SOURCE_DIR) + "/external/fastcap/fastcap.exe";
#else
    return "external/fastcap/fastcap.exe";
#endif
}

std::string defaultExternalCapExecutable(ExternalCapSolver solver) {
    return solver == ExternalCapSolver::FasterCapAdaptive
        ? defaultFasterCapExecutable()
        : defaultFastCapExecutable();
}

bool fasterCapAvailable(const std::string& executable) {
    const std::filesystem::path p(executable.empty() ? defaultFasterCapExecutable() : executable);
    return std::filesystem::exists(p)
           && std::filesystem::is_regular_file(p)
           && looksLikeFasterCapExecutable(p.string());
}

bool fastCapAvailable(const std::string& executable) {
    const std::filesystem::path p(executable.empty() ? defaultFastCapExecutable() : executable);
    return std::filesystem::exists(p)
           && std::filesystem::is_regular_file(p)
           && looksLikeFastCapExecutable(p.string());
}

bool externalCapAvailable(ExternalCapSolver solver, const std::string& executable) {
    return solver == ExternalCapSolver::FasterCapAdaptive
        ? fasterCapAvailable(executable)
        : fastCapAvailable(executable);
}

std::string writeFastCapQuiString(const std::vector<Panel>& panelsA,
                                  const std::vector<Panel>& panelsB) {
    std::ostringstream out;
    out.setf(std::ios::scientific);
    out.precision(17);
    out << "0 ccc_generated_units_meters\n";
    for (const auto& p : panelsA) appendPanel(out, 1, p);
    for (const auto& p : panelsB) appendPanel(out, 2, p);
    return out.str();
}

BemResult computeMutualCapacitanceFastCap(const std::vector<Panel>& panelsA,
                                           const std::vector<Panel>& panelsB,
                                           const Model& model,
                                           const BemOptions& opts,
                                           const std::string& executable) {
    (void)model;
    BemResult result;
    result.NA = int(panelsA.size());
    result.NB = int(panelsB.size());
    if (result.NA <= 0 || result.NB <= 0)
        throw std::runtime_error("FasterCap: both conductors need at least one panel");

    const auto solver = ExternalCapSolver::FasterCapAdaptive;
    const std::string exe = executable.empty() ? defaultExternalCapExecutable(solver) : executable;
    if (!externalCapAvailable(solver, exe))
        throw std::runtime_error(
            "FasterCap: executable is missing or is not FasterCap at " + exe);

    const auto quiPath = makeTempQuiPath();
    {
        std::ofstream out(quiPath);
        if (!out) throw std::runtime_error("FasterCap: cannot create temp .qui file");
        out << writeFastCapQuiString(panelsA, panelsB);
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::string stdoutText;
    try {
        stdoutText = runSolverAndCapture(
            exe, solverArgs(solver, quiPath, false, 0.01),
            true, opts.stopFlag, 0.0);
        std::filesystem::remove(quiPath);
    } catch (...) {
        std::filesystem::remove(quiPath);
        throw;
    }
    const auto t1 = std::chrono::steady_clock::now();

    const auto cm = parseFastCapMatrix(stdoutText);
    const double scale = opts.epsEff > 0.0 ? opts.epsEff : 1.0;
    result.CM[0][0] = cm.values[0][0] * scale;
    result.CM[0][1] = cm.values[0][1] * scale;
    result.CM[1][0] = cm.values[1][0] * scale;
    result.CM[1][1] = cm.values[1][1] * scale;
    result.Cm = -0.5 * (result.CM[0][1] + result.CM[1][0]);
    result.CselfA = result.CM[0][0] + result.CM[0][1];
    result.CselfB = result.CM[1][0] + result.CM[1][1];
    result.assembleMs = 0.0;
    result.solveMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

BemResult computeMutualCapacitanceFastCapWithEnvironment(
    const Model& model,
    const ConductorRefs& refsA,
    const ConductorRefs& refsB,
    const std::string& netA,
    const std::string& netB,
    const BemOptions& opts,
    const FastCapEnvironmentOptions& env,
    const std::string& executable) {
    BemResult result;
    if (refsEmpty(refsA) || refsEmpty(refsB))
        throw std::runtime_error("FasterCap: both selected nets need copper");

    const std::string exe = executable.empty()
        ? defaultExternalCapExecutable(env.solver)
        : executable;
    if (!externalCapAvailable(env.solver, exe))
        throw std::runtime_error(
            externalCapSolverName(env.solver)
            + ": executable is missing or not recognized at " + exe);
    const auto& progress = env.progressCallback;

    const auto t0 = std::chrono::steady_clock::now();
    auto checkStop = [&]() {
        if (opts.stopFlag && opts.stopFlag->load(std::memory_order_relaxed))
            throw std::runtime_error("FasterCap cancelled");
    };

    emitProgress(progress, "Panelizing selected nets", "Building panels for net A");
    const auto panelsA = panelizeConductor(refsA, model,
                                           opts.panelSizePad > 0.0 ? opts.panelSizePad : opts.panelSize,
                                           opts.panelSizeFpc > 0.0 ? opts.panelSizeFpc : opts.panelSize,
                                           opts.panelSizeTrace > 0.0 ? opts.panelSizeTrace : opts.panelSize);
    checkStop();
    emitProgress(progress, "Panelizing selected nets", "Building panels for net B",
                 0, 0, int(panelsA.size()), 0);
    const auto panelsB = panelizeConductor(refsB, model,
                                           opts.panelSizePad > 0.0 ? opts.panelSizePad : opts.panelSize,
                                           opts.panelSizeFpc > 0.0 ? opts.panelSizeFpc : opts.panelSize,
                                           opts.panelSizeTrace > 0.0 ? opts.panelSizeTrace : opts.panelSize);
    checkStop();
    result.NA = int(panelsA.size());
    result.NB = int(panelsB.size());
    if (result.NA <= 0 || result.NB <= 0)
        throw std::runtime_error("FasterCap: both selected nets need at least one panel");
    emitProgress(progress, "Selected net panels ready", {},
                 0, 0, result.NA, result.NB);

    BBox window = boundsOf(panelsA, panelsB);
    window.expand(std::max(0.0, env.environmentMarginMm));

    std::vector<Panel> panelsGnd;
    if (env.includeGroundNets) {
        emitProgress(progress, "Adding nearby ground", "Collecting local GND copper",
                     0, 0, result.NA, result.NB);
        const auto refsGnd = collectGroundRefs(model, netA, netB);
        if (!refsEmpty(refsGnd)) {
            const double psEnv = env.environmentPanelSize > 0.0
                ? env.environmentPanelSize
                : std::max(1.0, (opts.panelSizePad > 0.0 ? opts.panelSizePad : opts.panelSize) * 2.0);
            auto allGnd = panelizeConductor(refsGnd, model, psEnv, psEnv, psEnv);
            panelsGnd = filterPanels(allGnd, window);
        }
    }
    checkStop();
    emitProgress(progress, "Ground environment ready", {},
                 0, 0, result.NA, result.NB, int(panelsGnd.size()));

    const auto spans = layerSpans(model);
    const bool stackMedia = env.includeDielectricStack && hasDielectricStack(spans);
    std::map<double, std::vector<LabeledPanel>> conductorSurfacesByPerm;
    auto addConductorPanels = [&](int conductor, const std::vector<Panel>& panels) {
        for (const auto& p : panels) {
            const double eps = stackMedia ? permittivityForPanel(spans, p, opts)
                                          : fallbackPerm(opts);
            conductorSurfacesByPerm[permKey(eps)].push_back({conductor, p});
        }
    };
    addConductorPanels(1, panelsA);
    addConductorPanels(2, panelsB);
    const int expectedConductors = panelsGnd.empty() ? 2 : 3;
    if (!panelsGnd.empty()) addConductorPanels(3, panelsGnd);

    emitProgress(progress, "Building dielectric interfaces", {},
                 0, 0, result.NA, result.NB, int(panelsGnd.size()));
    auto dielectric = dielectricInterfaces(spans, window, opts, env);
    int dielectricPanelCount = 0;
    for (const auto& di : dielectric) dielectricPanelCount += int(di.panels.size());
    checkStop();
    emitProgress(progress, "Dielectric stack ready", {},
                 0, 0, result.NA, result.NB, int(panelsGnd.size()), dielectricPanelCount);

    const auto problemDir = makeTempProblemDir();
    const auto listPath = problemDir / "input.lst";
    std::string stdoutText;
    const auto tAssembled0 = std::chrono::steady_clock::now();
    try {
        emitProgress(progress, "Writing solver input", {},
                     0, 0, result.NA, result.NB, int(panelsGnd.size()), dielectricPanelCount);
        std::ofstream list(listPath);
        if (!list) throw std::runtime_error("FasterCap: cannot create temp list file");

        int surfaceIndex = 0;
        int remainingSurfaces = int(conductorSurfacesByPerm.size());
        for (const auto& [eps, panels] : conductorSurfacesByPerm) {
            const auto path = problemDir / ("cond_" + std::to_string(surfaceIndex++) + ".qui");
            writeLabeledPanelFile(path, panels);
            --remainingSurfaces;
            list << "C " << fastCapPath(path) << ' ' << eps << " 0 0 0";
            if (remainingSurfaces > 0) list << " +";
            list << '\n';
        }

        int dielectricIndex = 0;
        for (const auto& di : dielectric) {
            const auto path = problemDir / ("diel_" + std::to_string(dielectricIndex++) + ".qui");
            writeDielectricPanelFile(path, di.panels);
            list << "D " << fastCapPath(path) << ' '
                 << di.outerPerm << ' ' << di.innerPerm << " 0 0 0 "
                 << di.refX * 1e-3 << ' '
                 << di.refY * 1e-3 << ' '
                 << di.refZ * 1e-3 << " -\n";
        }
        list.close();
        checkStop();

        const auto tRun0 = std::chrono::steady_clock::now();
        emitProgress(progress, "Running " + externalCapSolverName(env.solver),
                     "Waiting for capacitance matrix",
                     0, 0, result.NA, result.NB, int(panelsGnd.size()), dielectricPanelCount);
        stdoutText = runSolverAndCapture(
            exe, solverArgs(env.solver, listPath, true, env.fasterCapRelativeError),
            true, opts.stopFlag, env.solverTimeoutSeconds, progress);
        const auto tRun1 = std::chrono::steady_clock::now();
        result.assembleMs = std::chrono::duration<double, std::milli>(tRun0 - t0).count();
        result.solveMs = std::chrono::duration<double, std::milli>(tRun1 - tRun0).count();
        std::filesystem::remove_all(problemDir);
    } catch (...) {
        std::filesystem::remove_all(problemDir);
        throw;
    }
    (void)tAssembled0;

    emitProgress(progress, "Parsing capacitance matrix", {},
                 0, 0, result.NA, result.NB, int(panelsGnd.size()), dielectricPanelCount);
    const auto cm = parseFastCapMatrix(stdoutText, expectedConductors);
    result.CM[0][0] = cm.values[0][0];
    result.CM[0][1] = cm.values[0][1];
    result.CM[1][0] = cm.values[1][0];
    result.CM[1][1] = cm.values[1][1];
    result.Cm = -0.5 * (result.CM[0][1] + result.CM[1][0]);
    result.CselfA = result.CM[0][0];
    result.CselfB = result.CM[1][1];
    return result;
}

FastCapFusionResult computeMutualCapacitanceFusion(
    const Model& model,
    const ConductorRefs& refsA,
    const ConductorRefs& refsB,
    const std::string& netA,
    const std::string& netB,
    const BemOptions& opts,
    const FastCapEnvironmentOptions& env) {
    if (!fastCapAvailable())
        throw std::runtime_error("FastCap: executable is missing or not recognized");
    if (!fasterCapAvailable())
        throw std::runtime_error("FasterCap: executable is missing or not recognized");

    auto checkStop = [&]() {
        if (opts.stopFlag && opts.stopFlag->load(std::memory_order_relaxed))
            throw std::runtime_error("fusion capacitance calculation cancelled");
    };
    auto makeOpts = [&](double panelSize) {
        BemOptions local = opts;
        local.panelSize = panelSize;
        local.panelSizePad = panelSize;
        local.panelSizeFpc = panelSize;
        local.panelSizeTrace = panelSize;
        return local;
    };
    auto makeEnv = [&](ExternalCapSolver solver, double panelSize) {
        FastCapEnvironmentOptions local = env;
        local.solver = solver;
        local.includeGroundNets = true;
        local.includeDielectricStack = true;
        local.environmentPanelSize = std::max(1.0, panelSize * 2.0);
        local.dielectricPanelSize = std::max(2.0, local.environmentPanelSize * 2.0);
        return local;
    };

    FastCapFusionResult out;
    for (double panelSize : {1.0, 0.5, 0.2}) {
        checkStop();
        emitProgress(env.progressCallback, "Fusion FastCap sweep",
                     "FastCap fixed mesh " + std::to_string(panelSize) + " mm",
                     int(out.fastCapSweep.size()) + 1, 4);
        auto localOpts = makeOpts(panelSize);
        auto localEnv = makeEnv(ExternalCapSolver::FastCapFixed, panelSize);
        out.fastCapSweep.push_back({
            panelSize,
            computeMutualCapacitanceFastCapWithEnvironment(
                model, refsA, refsB, netA, netB, localOpts, localEnv)});
    }

    checkStop();
    emitProgress(env.progressCallback, "Fusion FasterCap reference",
                 "FasterCap adaptive check at 0.5 mm", 4, 4);
    auto refOpts = makeOpts(0.5);
    auto refEnv = makeEnv(ExternalCapSolver::FasterCapAdaptive, 0.5);
    out.fasterCapReference = {
        0.5,
        computeMutualCapacitanceFastCapWithEnvironment(
            model, refsA, refsB, netA, netB, refOpts, refEnv)};

    if (out.fastCapSweep.size() >= 3) {
        out.fastCapFineRelativeDelta = relativeDelta(
            out.fastCapSweep[1].result.Cm,
            out.fastCapSweep[2].result.Cm);
        out.fasterCapReferenceRelativeDelta = relativeDelta(
            out.fastCapSweep[1].result.Cm,
            out.fasterCapReference.result.Cm);
    }
    return out;
}

}  // namespace ccc::core
